/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include <netinet/in.h> // IPPROTO_TCP
#include <netinet/tcp.h> // TCP_NODELAY
#include <fcntl.h>
#include <stddef.h> // NULL
#include <errno.h>
#include <spice/macros.h>
#include <spice/vd_agent.h>
#include "common/marshaller.h"
#include "common/messages.h"
#include "server/demarshallers.h"
#include "server/generated_marshallers.h"
#include "spice.h"
#include "red_common.h"
#include "reds.h"
#include "red_channel.h"
#include "inputs_channel.h"

// TODO: RECEIVE_BUF_SIZE used to be the same for inputs_channel and main_channel
// since it was defined once in reds.c which contained both.
// Now that they are split we can give a more fitting value for inputs - what
// should it be?
#define REDS_AGENT_WINDOW_SIZE 10
#define REDS_NUM_INTERNAL_AGENT_MESSAGES 1

// approximate max receive message size
#define RECEIVE_BUF_SIZE \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

struct SpiceKbdState {
    int dummy;
};

struct SpiceMouseState {
    int dummy;
};

struct SpiceTabletState {
    int dummy;
};

typedef struct InputsChannel {
    RedChannel base;
    uint8_t recv_buf[RECEIVE_BUF_SIZE];
    VDAgentMouseState mouse_state;
    uint32_t motion_count;
} InputsChannel;

enum {
    PIPE_ITEM_INIT = SPICE_MSG_INPUTS_INIT,
    PIPE_ITEM_MOUSE_MOTION_ACK = SPICE_MSG_INPUTS_MOUSE_MOTION_ACK,
    PIPE_ITEM_KEY_MODIFIERS = SPICE_MSG_INPUTS_KEY_MODIFIERS,
    PIPE_ITEM_MIGRATE = SPICE_MSG_MIGRATE,
};

typedef struct InputsPipeItem {
    PipeItem base;
} InputsPipeItem;

typedef struct KeyModifiersPipeItem {
    PipeItem base;
    uint8_t modifiers;
} KeyModifiersPipeItem;

static SpiceKbdInstance *keyboard = NULL;
static SpiceMouseInstance *mouse = NULL;
static SpiceTabletInstance *tablet = NULL;

static SpiceTimer *key_modifiers_timer;

static InputsChannel *g_inputs_channel = NULL;

#define KEY_MODIFIERS_TTL (1000 * 2) /*2sec*/

#define SCROLL_LOCK_SCAN_CODE 0x46
#define NUM_LOCK_SCAN_CODE 0x45
#define CAPS_LOCK_SCAN_CODE 0x3a

int inputs_inited(void)
{
    return !!g_inputs_channel;
}

int inputs_set_keyboard(SpiceKbdInstance *_keyboard)
{
    if (keyboard) {
        red_printf("already have keyboard");
        return -1;
    }
    keyboard = _keyboard;
    keyboard->st = spice_new0(SpiceKbdState, 1);
    return 0;
}

int inputs_set_mouse(SpiceMouseInstance *_mouse)
{
    if (mouse) {
        red_printf("already have mouse");
        return -1;
    }
    mouse = _mouse;
    mouse->st = spice_new0(SpiceMouseState, 1);
    return 0;
}

int inputs_set_tablet(SpiceTabletInstance *_tablet)
{
    if (tablet) {
        red_printf("already have tablet");
        return -1;
    }
    tablet = _tablet;
    tablet->st = spice_new0(SpiceTabletState, 1);
    return 0;
}

int inputs_has_tablet(void)
{
    return !!tablet;
}

void inputs_detach_tablet(SpiceTabletInstance *_tablet)
{
    red_printf("");
    tablet = NULL;
}

void inputs_set_tablet_logical_size(int x_res, int y_res)
{
    SpiceTabletInterface *sif;

    sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
    sif->set_logical_size(tablet, x_res, y_res);
}

const VDAgentMouseState *inputs_get_mouse_state(void)
{
    ASSERT(g_inputs_channel);
    return &g_inputs_channel->mouse_state;
}

static uint8_t *inputs_channel_alloc_msg_rcv_buf(RedChannel *channel, SpiceDataHeader *msg_header)
{
    InputsChannel *inputs_channel = SPICE_CONTAINEROF(channel, InputsChannel, base);

    return inputs_channel->recv_buf;
}

static void inputs_channel_release_msg_rcv_buf(RedChannel *channel, SpiceDataHeader *msg_header,
                                               uint8_t *msg)
{
}

#define OUTGOING_OK 0
#define OUTGOING_FAILED -1
#define OUTGOING_BLOCKED 1

#define RED_MOUSE_STATE_TO_LOCAL(state)     \
    ((state & SPICE_MOUSE_BUTTON_MASK_LEFT) |          \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) << 1) |   \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) >> 1))

#define RED_MOUSE_BUTTON_STATE_TO_AGENT(state)                      \
    (((state & SPICE_MOUSE_BUTTON_MASK_LEFT) ? VD_AGENT_LBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) ? VD_AGENT_MBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) ? VD_AGENT_RBUTTON_MASK : 0))

static void activate_modifiers_watch()
{
    core->timer_start(key_modifiers_timer, KEY_MODIFIERS_TTL);
}

static void kbd_push_scan(SpiceKbdInstance *sin, uint8_t scan)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);
    sif->push_scan_freg(sin, scan);
}

static uint8_t kbd_get_leds(SpiceKbdInstance *sin)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return 0;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);
    return sif->get_leds(sin);
}

static InputsPipeItem *inputs_pipe_item_new(InputsChannel *inputs_channel, int type)
{
    InputsPipeItem *item = spice_malloc(sizeof(InputsPipeItem));

    red_channel_pipe_item_init(&inputs_channel->base, &item->base, type);
    return item;
}

static KeyModifiersPipeItem *inputs_key_modifiers_item_new(
    InputsChannel *inputs_channel, uint8_t modifiers)
{
    KeyModifiersPipeItem *item = spice_malloc(sizeof(KeyModifiersPipeItem));

    red_channel_pipe_item_init(&inputs_channel->base, &item->base,
                               PIPE_ITEM_KEY_MODIFIERS);
    item->modifiers = modifiers;
    return item;
}

// Right now every PipeItem we add is an InputsPipeItem, later maybe make it simpler
// for type only PipeItems
static void inputs_pipe_add_type(InputsChannel *channel, int type)
{
    InputsPipeItem* pipe_item = inputs_pipe_item_new(channel, type);

    red_channel_pipe_add_push(&channel->base, &pipe_item->base);
}

static void inputs_channel_release_pipe_item(RedChannel *channel,
    PipeItem *base, int item_pushed)
{
    free(base);
}

static void inputs_channel_send_item(RedChannel *channel, PipeItem *base)
{
    InputsChannel *inputs_channel = (InputsChannel *)channel;
    SpiceMarshaller *m = inputs_channel->base.send_data.marshaller;

    red_channel_reset_send_data(channel);
    red_channel_init_send_data(channel, base->type, base);
    switch (base->type) {
        case PIPE_ITEM_KEY_MODIFIERS:
        {
            SpiceMsgInputsKeyModifiers key_modifiers;

            key_modifiers.modifiers =
                SPICE_CONTAINEROF(base, KeyModifiersPipeItem, base)->modifiers;
            spice_marshall_msg_inputs_key_modifiers(m, &key_modifiers);
        }
        case PIPE_ITEM_INIT:
        {
            SpiceMsgInputsInit inputs_init;

            inputs_init.keyboard_modifiers = kbd_get_leds(keyboard);
            spice_marshall_msg_inputs_init(m, &inputs_init);
        }
        case PIPE_ITEM_MIGRATE:
        {
            SpiceMsgMigrate migrate;

            migrate.flags = 0;
            spice_marshall_msg_migrate(m, &migrate);
            break;
        }
        default:
            break;
    }
    red_channel_begin_send_message(channel);
}

static int inputs_channel_handle_parsed(RedChannel *channel, uint32_t size, uint16_t type, void *message)
{
    InputsChannel *inputs_channel = (InputsChannel *)channel;
    uint8_t *buf = (uint8_t *)message;

    ASSERT(g_inputs_channel == inputs_channel);
    switch (type) {
    case SPICE_MSGC_INPUTS_KEY_DOWN: {
        SpiceMsgcKeyDown *key_up = (SpiceMsgcKeyDown *)buf;
        if (key_up->code == CAPS_LOCK_SCAN_CODE || key_up->code == NUM_LOCK_SCAN_CODE ||
            key_up->code == SCROLL_LOCK_SCAN_CODE) {
            activate_modifiers_watch();
        }
    }
    case SPICE_MSGC_INPUTS_KEY_UP: {
        SpiceMsgcKeyDown *key_down = (SpiceMsgcKeyDown *)buf;
        uint8_t *now = (uint8_t *)&key_down->code;
        uint8_t *end = now + sizeof(key_down->code);
        for (; now < end && *now; now++) {
            kbd_push_scan(keyboard, *now);
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_MOTION: {
        SpiceMsgcMouseMotion *mouse_motion = (SpiceMsgcMouseMotion *)buf;

        if (++inputs_channel->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0) {
            inputs_pipe_add_type(inputs_channel, PIPE_ITEM_MOUSE_MOTION_ACK);
        }
        if (mouse && reds_get_mouse_mode() == SPICE_MOUSE_MODE_SERVER) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->motion(mouse,
                        mouse_motion->dx, mouse_motion->dy, 0,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_motion->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_POSITION: {
        SpiceMsgcMousePosition *pos = (SpiceMsgcMousePosition *)buf;

        if (++inputs_channel->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0) {
            inputs_pipe_add_type(inputs_channel, PIPE_ITEM_MOUSE_MOTION_ACK);
        }
        if (reds_get_mouse_mode() != SPICE_MOUSE_MODE_CLIENT) {
            break;
        }
        ASSERT((reds_get_agent_mouse() && reds_has_vdagent()) || tablet);
        if (!reds_get_agent_mouse() || !reds_has_vdagent()) {
            SpiceTabletInterface *sif;
            sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
            sif->position(tablet, pos->x, pos->y, RED_MOUSE_STATE_TO_LOCAL(pos->buttons_state));
            break;
        }
        VDAgentMouseState *mouse_state = &inputs_channel->mouse_state;
        mouse_state->x = pos->x;
        mouse_state->y = pos->y;
        mouse_state->buttons = RED_MOUSE_BUTTON_STATE_TO_AGENT(pos->buttons_state);
        mouse_state->display_id = pos->display_id;
        reds_handle_agent_mouse_event(mouse_state);
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_PRESS: {
        SpiceMsgcMousePress *mouse_press = (SpiceMsgcMousePress *)buf;
        int dz = 0;
        if (mouse_press->button == SPICE_MOUSE_BUTTON_UP) {
            dz = -1;
        } else if (mouse_press->button == SPICE_MOUSE_BUTTON_DOWN) {
            dz = 1;
        }
        if (reds_get_mouse_mode() == SPICE_MOUSE_MODE_CLIENT) {
            if (reds_get_agent_mouse() && reds_has_vdagent()) {
                inputs_channel->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_press->buttons_state) |
                    (dz == -1 ? VD_AGENT_UBUTTON_MASK : 0) |
                    (dz == 1 ? VD_AGENT_DBUTTON_MASK : 0);
                reds_handle_agent_mouse_event(&inputs_channel->mouse_state);
            } else if (tablet) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
                sif->wheel(tablet, dz, RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
            }
        } else if (mouse) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->motion(mouse, 0, 0, dz,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_RELEASE: {
        SpiceMsgcMouseRelease *mouse_release = (SpiceMsgcMouseRelease *)buf;
        if (reds_get_mouse_mode() == SPICE_MOUSE_MODE_CLIENT) {
            if (reds_get_agent_mouse() && reds_has_vdagent()) {
                inputs_channel->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_release->buttons_state);
                reds_handle_agent_mouse_event(&inputs_channel->mouse_state);
            } else if (tablet) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
                sif->buttons(tablet, RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
            }
        } else if (mouse) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->buttons(mouse,
                         RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_KEY_MODIFIERS: {
        SpiceMsgcKeyModifiers *modifiers = (SpiceMsgcKeyModifiers *)buf;
        uint8_t leds;

        if (!keyboard) {
            break;
        }
        leds = kbd_get_leds(keyboard);
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK)) {
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK)) {
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK)) {
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE | 0x80);
        }
        activate_modifiers_watch();
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        red_printf("unexpected type %d", type);
        return FALSE;
    }
    return TRUE;
}

static void inputs_relase_keys(void)
{
    kbd_push_scan(keyboard, 0x2a | 0x80); //LSHIFT
    kbd_push_scan(keyboard, 0x36 | 0x80); //RSHIFT
    kbd_push_scan(keyboard, 0xe0); kbd_push_scan(keyboard, 0x1d | 0x80); //RCTRL
    kbd_push_scan(keyboard, 0x1d | 0x80); //LCTRL
    kbd_push_scan(keyboard, 0xe0); kbd_push_scan(keyboard, 0x38 | 0x80); //RALT
    kbd_push_scan(keyboard, 0x38 | 0x80); //LALT
}

static void inputs_channel_on_incoming_error(RedChannel *channel)
{
    inputs_relase_keys();
    red_channel_destroy(channel);
}

static void inputs_channel_on_outgoing_error(RedChannel *channel)
{
    reds_disconnect();
}

static void inputs_shutdown(Channel *channel)
{
    InputsChannel *inputs_channel = (InputsChannel *)channel->data;
    ASSERT(g_inputs_channel == inputs_channel);

    if (inputs_channel) {
        red_channel_shutdown(&inputs_channel->base);
        inputs_channel->base.incoming.shut = TRUE;
        channel->data = NULL;
        g_inputs_channel = NULL;
    }
}

static void inputs_migrate(Channel *channel)
{
    InputsChannel *inputs_channel = channel->data;
    InputsPipeItem *item;

    ASSERT(g_inputs_channel == (InputsChannel *)channel->data);
    item = inputs_pipe_item_new(inputs_channel, PIPE_ITEM_MIGRATE);
    red_channel_pipe_add_push(&inputs_channel->base, &item->base);
}

static void inputs_pipe_add_init(InputsChannel *inputs_channel)
{
    InputsPipeItem *item = inputs_pipe_item_new(inputs_channel, PIPE_ITEM_INIT);

    red_channel_pipe_add_push(&inputs_channel->base, &item->base);
}

static int inputs_channel_config_socket(RedChannel *channel)
{
    int flags;
    int delay_val = 1;

    if (setsockopt(channel->peer->socket, IPPROTO_TCP, TCP_NODELAY,
            &delay_val, sizeof(delay_val)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
        return FALSE;
    }

    if ((flags = fcntl(channel->peer->socket, F_GETFL)) == -1 ||
                 fcntl(channel->peer->socket, F_SETFL, flags | O_ASYNC) == -1) {
        red_printf("fcntl failed, %s", strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static void inputs_channel_hold_item(PipeItem *item)
{
}

static void inputs_link(Channel *channel, RedsStreamContext *peer, int migration,
                        int num_common_caps, uint32_t *common_caps, int num_caps,
                        uint32_t *caps)
{
    InputsChannel *inputs_channel;
    red_printf("");
    ASSERT(channel->data == NULL);

    g_inputs_channel = inputs_channel = (InputsChannel*)red_channel_create_parser(
        sizeof(*inputs_channel), peer, core, migration, FALSE /* handle_acks */
        ,inputs_channel_config_socket
        ,spice_get_client_channel_parser(SPICE_CHANNEL_INPUTS, NULL)
        ,inputs_channel_handle_parsed
        ,inputs_channel_alloc_msg_rcv_buf
        ,inputs_channel_release_msg_rcv_buf
        ,inputs_channel_hold_item
        ,inputs_channel_send_item
        ,inputs_channel_release_pipe_item
        ,inputs_channel_on_incoming_error
        ,inputs_channel_on_outgoing_error);
    ASSERT(inputs_channel);
    channel->data = inputs_channel;
    inputs_pipe_add_init(inputs_channel);
}

static void inputs_push_keyboard_modifiers(uint8_t modifiers)
{
    KeyModifiersPipeItem *item;

    if (!g_inputs_channel) {
        return;
    }
    item = inputs_key_modifiers_item_new(g_inputs_channel, modifiers);
    red_channel_pipe_add_push(&g_inputs_channel->base, &item->base);
}

void inputs_on_keyboard_leds_change(void *opaque, uint8_t leds)
{
    inputs_push_keyboard_modifiers(leds);
}

static void key_modifiers_sender(void *opaque)
{
    inputs_push_keyboard_modifiers(kbd_get_leds(keyboard));
}

void inputs_init(void)
{
    Channel *channel;

    channel = spice_new0(Channel, 1);
    channel->type = SPICE_CHANNEL_INPUTS;
    channel->link = inputs_link;
    channel->shutdown = inputs_shutdown;
    channel->migrate = inputs_migrate;
    reds_register_channel(channel);

    if (!(key_modifiers_timer = core->timer_add(key_modifiers_sender, NULL))) {
        red_error("key modifiers timer create failed");
    }
}


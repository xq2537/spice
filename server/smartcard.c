/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2010 Red Hat, Inc.

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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arpa/inet.h>

#include <vscard_common.h>
#include "server/reds.h"
#include "server/char_device.h"
#include "server/red_channel.h"
#include "server/smartcard.h"

#define SMARTCARD_MAX_READERS 10

typedef struct SmartCardDeviceState {
    SpiceCharDeviceState base;
    uint32_t             reader_id;
    uint32_t             attached;
    uint8_t             *buf;
    uint32_t             buf_size;
    uint8_t             *buf_pos;
    uint32_t             buf_used;
    RedChannelClient    *rcc; // client providing the remote card
} SmartCardDeviceState;

enum {
    PIPE_ITEM_TYPE_ERROR=1,
    PIPE_ITEM_TYPE_MSG,
};

typedef struct ErrorItem {
    PipeItem base;
    VSCMsgHeader vheader;
    VSCMsgError  error;
} ErrorItem;

typedef struct MsgItem {
    PipeItem base;
    VSCMsgHeader* vheader;
} MsgItem;

typedef struct SmartCardChannel {
    RedChannel base;
} SmartCardChannel;

static struct Readers {
    uint32_t num;
    SpiceCharDeviceInstance* sin[SMARTCARD_MAX_READERS];
} g_smartcard_readers = {0, {NULL}};

static SpiceCharDeviceInstance* smartcard_readers_get_unattached();
static SpiceCharDeviceInstance* smartcard_readers_get(uint32_t reader_id);
static int smartcard_char_device_add_to_readers(SpiceCharDeviceInstance *sin);
static void smartcard_char_device_attach(
    SpiceCharDeviceInstance *char_device, RedChannelClient *rcc);
static void smartcard_char_device_detach(SpiceCharDeviceInstance *char_device);
static void smartcard_channel_write_to_reader(VSCMsgHeader *vheader);

static void smartcard_char_device_on_message_from_device(
    SmartCardDeviceState *state, VSCMsgHeader *header);
static void smartcard_on_message_from_device(
    RedChannelClient *rcc, VSCMsgHeader *vheader);
static SmartCardDeviceState* smartcard_device_state_new();
static void smartcard_device_state_free(SmartCardDeviceState* st);
static void smartcard_init(void);

void smartcard_char_device_wakeup(SpiceCharDeviceInstance *sin)
{
    SmartCardDeviceState* state = SPICE_CONTAINEROF(
                            sin->st, SmartCardDeviceState, base);
    SpiceCharDeviceInterface *sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);
    VSCMsgHeader *vheader = (VSCMsgHeader*)state->buf;
    int n;
    int remaining;
    int actual_length;

    while ((n = sif->read(sin, state->buf_pos, state->buf_size - state->buf_used)) > 0) {
        state->buf_pos += n;
        state->buf_used += n;
        if (state->buf_used < sizeof(VSCMsgHeader)) {
            continue;
        }
        actual_length = ntohl(vheader->length);
        if (actual_length > state->buf_size) {
            state->buf_size = MAX(state->buf_size*2, actual_length + sizeof(VSCMsgHeader));
            state->buf = spice_realloc(state->buf, state->buf_size);
            ASSERT(state->buf != NULL);
        }
        if (state->buf_used - sizeof(VSCMsgHeader) < actual_length) {
            continue;
        }
        smartcard_char_device_on_message_from_device(state, vheader);
        remaining = state->buf_used - sizeof(VSCMsgHeader) > actual_length;
        if (remaining > 0) {
            memcpy(state->buf, state->buf_pos, remaining);
        }
        state->buf_pos = state->buf;
        state->buf_used = remaining;
    }
}

void smartcard_char_device_on_message_from_device(
    SmartCardDeviceState *state,
    VSCMsgHeader *vheader)
{
    VSCMsgHeader *sent_header;

    vheader->type = ntohl(vheader->type);
    vheader->length = ntohl(vheader->length);
    vheader->reader_id = ntohl(vheader->reader_id);

    switch (vheader->type) {
        case VSC_Init:
            return;
            break;
        default:
            break;
    }
    /* We pass any VSC_Error right now - might need to ignore some? */
    if (state->reader_id == VSCARD_UNDEFINED_READER_ID && vheader->type != VSC_Init) {
        red_printf("error: reader_id not assigned for message of type %d", vheader->type);
    }
    if (state->rcc) {
        sent_header = spice_memdup(vheader, sizeof(*vheader) + vheader->length);
        /* We patch the reader_id, since the device only knows about itself, and
         * we know about the sum of readers. */
        sent_header->reader_id = state->reader_id;
        smartcard_on_message_from_device(state->rcc, sent_header);
    }
}

static void smartcard_readers_detach_all(RedChannelClient *rcc)
{
    int i;
    SmartCardDeviceState *st;
    // TODO - can track rcc->{sin}

    for (i = 0 ; i < g_smartcard_readers.num; ++i) {
        st = SPICE_CONTAINEROF(g_smartcard_readers.sin[i]->st, SmartCardDeviceState, base);
        if (!rcc || st->rcc == rcc) {
            smartcard_char_device_detach(g_smartcard_readers.sin[i]);
        }
    }
}

static int smartcard_char_device_add_to_readers(SpiceCharDeviceInstance *char_device)
{
    SmartCardDeviceState* state = SPICE_CONTAINEROF(
                            char_device->st, SmartCardDeviceState, base);

    if (g_smartcard_readers.num >= SMARTCARD_MAX_READERS) {
        return -1;
    }
    state->reader_id = g_smartcard_readers.num;
    g_smartcard_readers.sin[g_smartcard_readers.num++] = char_device;
    smartcard_init();
    return 0;
}

static SpiceCharDeviceInstance *smartcard_readers_get(uint32_t reader_id)
{
    ASSERT(reader_id < g_smartcard_readers.num);
    return g_smartcard_readers.sin[reader_id];
}

static SpiceCharDeviceInstance *smartcard_readers_get_unattached()
{
    int i;
    SmartCardDeviceState* state;

    for (i = 0; i < g_smartcard_readers.num; ++i) {
        state = SPICE_CONTAINEROF(g_smartcard_readers.sin[i]->st,
                                  SmartCardDeviceState, base);
        if (!state->attached) {
            return g_smartcard_readers.sin[i];
        }
    }
    return NULL;
}

static SmartCardDeviceState* smartcard_device_state_new()
{
    SmartCardDeviceState *st;

    st = spice_new0(SmartCardDeviceState, 1);
    st->base.wakeup = smartcard_char_device_wakeup;
    st->reader_id = VSCARD_UNDEFINED_READER_ID;
    st->attached = FALSE;
    st->buf_size = APDUBufSize + sizeof(VSCMsgHeader);
    st->buf = spice_malloc(st->buf_size);
    st->buf_pos = st->buf;
    st->buf_used = 0;
    st->rcc = NULL;
    return st;
}

static void smartcard_device_state_free(SmartCardDeviceState* st)
{
    free(st->buf);
    free(st);
}

void smartcard_device_disconnect(SpiceCharDeviceInstance *char_device)
{
    SmartCardDeviceState *st = SPICE_CONTAINEROF(char_device->st,
        SmartCardDeviceState, base);

    smartcard_device_state_free(st);
}

int smartcard_device_connect(SpiceCharDeviceInstance *char_device)
{
    SmartCardDeviceState *st;

    st = smartcard_device_state_new();
    char_device->st = &st->base;
    if (smartcard_char_device_add_to_readers(char_device) == -1) {
        smartcard_device_state_free(st);
        return -1;
    }
    return 0;
}

static void smartcard_char_device_attach(
    SpiceCharDeviceInstance *char_device, RedChannelClient *rcc)
{
    SmartCardDeviceState *st = SPICE_CONTAINEROF(char_device->st, SmartCardDeviceState, base);

    if (st->attached == TRUE) {
        return;
    }
    st->attached = TRUE;
    st->rcc = rcc;
    VSCMsgHeader vheader = {.type = VSC_ReaderAdd, .reader_id=st->reader_id,
        .length=0};
    smartcard_channel_write_to_reader(&vheader);
}

static void smartcard_char_device_detach(SpiceCharDeviceInstance *char_device)
{
    SmartCardDeviceState *st = SPICE_CONTAINEROF(char_device->st, SmartCardDeviceState, base);

    if (st->attached == FALSE) {
        return;
    }
    st->attached = FALSE;
    st->rcc = NULL;
    VSCMsgHeader vheader = {.type = VSC_ReaderRemove, .reader_id=st->reader_id,
        .length=0};
    smartcard_channel_write_to_reader(&vheader);
}

static int smartcard_channel_client_config_socket(RedChannelClient *rcc)
{
    return TRUE;
}

static uint8_t *smartcard_channel_alloc_msg_rcv_buf(RedChannelClient *rcc,
                                                    uint16_t type,
                                                    uint32_t size)
{
    return spice_malloc(size);
}

static void smartcard_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                                  uint16_t type,
                                                  uint32_t size,
                                                  uint8_t *msg)
{
    red_printf("freeing %d bytes", size);
    free(msg);
}

static void smartcard_channel_send_data(RedChannelClient *rcc, SpiceMarshaller *m,
                                        PipeItem *item, VSCMsgHeader *vheader)
{
    ASSERT(rcc);
    ASSERT(vheader);
    red_channel_client_init_send_data(rcc, SPICE_MSG_SMARTCARD_DATA, item);
    spice_marshaller_add_ref(m, (uint8_t*)vheader, sizeof(VSCMsgHeader));
    if (vheader->length > 0) {
        spice_marshaller_add_ref(m, (uint8_t*)(vheader+1), vheader->length);
    }
    red_channel_client_begin_send_message(rcc);
}

static void smartcard_channel_send_error(
    RedChannelClient *rcc, SpiceMarshaller *m, PipeItem *item)
{
    ErrorItem* error_item = (ErrorItem*)item;

    smartcard_channel_send_data(rcc, m, item, &error_item->vheader);
}

static void smartcard_channel_send_msg(RedChannelClient *rcc,
                                       SpiceMarshaller *m, PipeItem *item)
{
    MsgItem* msg_item = (MsgItem*)item;

    smartcard_channel_send_data(rcc, m, item, msg_item->vheader);
}

static void smartcard_channel_send_item(RedChannelClient *rcc, PipeItem *item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    switch (item->type) {
    case PIPE_ITEM_TYPE_ERROR:
        smartcard_channel_send_error(rcc, m, item);
        break;
    case PIPE_ITEM_TYPE_MSG:
        smartcard_channel_send_msg(rcc, m, item);
    }
}


static void smartcard_channel_release_pipe_item(RedChannelClient *rcc,
                                      PipeItem *item, int item_pushed)
{
    if (item->type == PIPE_ITEM_TYPE_MSG) {
        MsgItem *mi = (MsgItem *)item;
        free(mi->vheader);
    }
    free(item);
}

static void smartcard_channel_on_disconnect(RedChannelClient *rcc)
{
    smartcard_readers_detach_all(rcc);
}

/* this is called from both device input and client input. since the device is
 * a usb device, the context is still the main thread (kvm_main_loop, timers)
 * so no mutex is required. */
static void smartcard_channel_client_pipe_add_push(RedChannelClient *rcc, PipeItem *item)
{
    red_channel_client_pipe_add_push(rcc, item);
}

static void smartcard_push_error(RedChannelClient *rcc, uint32_t reader_id, VSCErrorCode error)
{
    ErrorItem *error_item = spice_new0(ErrorItem, 1);

    red_channel_pipe_item_init(rcc->channel, &error_item->base,
                               PIPE_ITEM_TYPE_ERROR);

    error_item->base.type = PIPE_ITEM_TYPE_ERROR;
    error_item->vheader.reader_id = reader_id;
    error_item->vheader.type = VSC_Error;
    error_item->vheader.length = sizeof(error_item->error);
    error_item->error.code = error;
    smartcard_channel_client_pipe_add_push(rcc, &error_item->base);
}

static void smartcard_push_vscmsg(RedChannelClient *rcc, VSCMsgHeader *vheader)
{
    MsgItem *msg_item = spice_new0(MsgItem, 1);

    red_channel_pipe_item_init(rcc->channel, &msg_item->base,
                               PIPE_ITEM_TYPE_MSG);
    msg_item->vheader = vheader;
    smartcard_channel_client_pipe_add_push(rcc, &msg_item->base);
}

void smartcard_on_message_from_device(RedChannelClient *rcc, VSCMsgHeader* vheader)
{
    smartcard_push_vscmsg(rcc, vheader);
}

static void smartcard_remove_reader(RedChannelClient *rcc, uint32_t reader_id)
{
    SpiceCharDeviceInstance *char_device = smartcard_readers_get(reader_id);
    SmartCardDeviceState *state;

    if (char_device == NULL) {
        smartcard_push_error(rcc, reader_id,
            VSC_GENERAL_ERROR);
        return;
    }

    state = SPICE_CONTAINEROF(char_device->st, SmartCardDeviceState, base);
    if (state->attached == FALSE) {
        smartcard_push_error(rcc, reader_id,
            VSC_GENERAL_ERROR);
        return;
    }
    smartcard_char_device_detach(char_device);
}

static void smartcard_add_reader(RedChannelClient *rcc, uint8_t *name)
{
    // TODO - save name somewhere
    SpiceCharDeviceInstance *char_device =
            smartcard_readers_get_unattached();

    if (char_device != NULL) {
        smartcard_char_device_attach(char_device, rcc);
        // The device sends a VSC_Error message, we will let it through, no
        // need to send our own. We already set the correct reader_id, from
        // our SmartCardDeviceState.
    } else {
        smartcard_push_error(rcc, VSCARD_UNDEFINED_READER_ID,
            VSC_CANNOT_ADD_MORE_READERS);
    }
}

static void smartcard_channel_write_to_reader(VSCMsgHeader *vheader)
{
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;
    uint32_t n;
    uint32_t actual_length = vheader->length;

    ASSERT(vheader->reader_id >= 0 &&
           vheader->reader_id <= g_smartcard_readers.num);
    sin = g_smartcard_readers.sin[vheader->reader_id];
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);
    /* protocol requires messages to be in network endianess */
    vheader->type = htonl(vheader->type);
    vheader->length = htonl(vheader->length);
    vheader->reader_id = htonl(vheader->reader_id);
    n = sif->write(sin, (uint8_t*)vheader,
                   actual_length + sizeof(VSCMsgHeader));
    // TODO - add ring
    ASSERT(n == actual_length + sizeof(VSCMsgHeader));
}

static int smartcard_channel_handle_message(RedChannelClient *rcc,
                                            uint16_t type,
                                            uint32_t size,
                                            uint8_t *msg)
{
    VSCMsgHeader* vheader = (VSCMsgHeader*)msg;

    if (type != SPICE_MSGC_SMARTCARD_DATA) {
        /* handle ack's, spicy sends them while spicec does not */
        return red_channel_client_handle_message(rcc, size, type, msg);
    }

    ASSERT(size == vheader->length + sizeof(VSCMsgHeader));
    switch (vheader->type) {
        case VSC_ReaderAdd:
            smartcard_add_reader(rcc, msg + sizeof(VSCMsgHeader));
            return TRUE;
            break;
        case VSC_ReaderRemove:
            smartcard_remove_reader(rcc, vheader->reader_id);
            return TRUE;
            break;
        case VSC_Init:
            // ignore - we should never get this anyway
            return TRUE;
            break;
        case VSC_Error:
        case VSC_ATR:
        case VSC_CardRemove:
        case VSC_APDU:
            break; // passed on to device
        default:
            printf("ERROR: unexpected message on smartcard channel\n");
            return TRUE;
    }

    if (vheader->reader_id >= g_smartcard_readers.num) {
        red_printf("ERROR: received message for non existent reader: %d, %d, %d", vheader->reader_id,
            vheader->type, vheader->length);
        return FALSE;
    }
    smartcard_channel_write_to_reader(vheader);
    return TRUE;
}

static void smartcard_channel_hold_pipe_item(RedChannelClient *rcc, PipeItem *item)
{
}

static void smartcard_connect(RedChannel *channel, RedClient *client,
                        RedsStream *stream, int migration,
                        int num_common_caps, uint32_t *common_caps,
                        int num_caps, uint32_t *caps)
{
    RedChannelClient *rcc;

    rcc = red_channel_client_create(sizeof(RedChannelClient), channel, client, stream,
                                    num_common_caps, common_caps,
                                    num_caps, caps);
    red_channel_client_ack_zero_messages_window(rcc);
}

static void smartcard_migrate(RedChannelClient *rcc)
{
}

SmartCardChannel *g_smartcard_channel;

static void smartcard_init(void)
{
    ChannelCbs channel_cbs;
    ClientCbs client_cbs;

    ASSERT(!g_smartcard_channel);

    memset(&channel_cbs, 0, sizeof(channel_cbs));
    memset(&client_cbs, 0, sizeof(client_cbs));

    channel_cbs.config_socket = smartcard_channel_client_config_socket;
    channel_cbs.on_disconnect = smartcard_channel_on_disconnect;
    channel_cbs.send_item = smartcard_channel_send_item;
    channel_cbs.hold_item = smartcard_channel_hold_pipe_item;
    channel_cbs.release_item = smartcard_channel_release_pipe_item;
    channel_cbs.alloc_recv_buf = smartcard_channel_alloc_msg_rcv_buf;
    channel_cbs.release_recv_buf = smartcard_channel_release_msg_rcv_buf;

    g_smartcard_channel = (SmartCardChannel*)red_channel_create(sizeof(SmartCardChannel),
                                             core, SPICE_CHANNEL_SMARTCARD, 0,
                                             FALSE /* migration - TODO?*/,
                                             FALSE /* handle_acks */,
                                             smartcard_channel_handle_message,
                                             &channel_cbs);

    if (!g_smartcard_channel) {
        red_error("failed to allocate Inputs Channel");
    }

    client_cbs.connect = smartcard_connect;
    client_cbs.migrate = smartcard_migrate;
    red_channel_register_client_cbs(&g_smartcard_channel->base, &client_cbs);

    reds_register_channel(&g_smartcard_channel->base);
}

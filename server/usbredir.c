/* spice-server usbredir code

   Copyright (C) 2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

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

#include <assert.h>

#include "server/char_device.h"
#include "server/red_channel.h"
#include "server/reds.h"

/* 64K should be enough for all but the largest bulk xfers + 32 bytes hdr */
#define BUF_SIZE (64 * 1024 + 32)

typedef struct UsbRedirPipeItem {
    PipeItem base;
    /* packets which don't fit this will get split, this is not a problem */
    uint8_t buf[BUF_SIZE];
    uint32_t buf_used;
} UsbRedirPipeItem;

typedef struct UsbRedirState {
    RedChannel channel; /* Must be the first item */
    RedChannelClient *rcc;
    SpiceCharDeviceState chardev_st;
    SpiceCharDeviceInstance *chardev_sin;
    UsbRedirPipeItem *pipe_item;
    uint8_t *rcv_buf;
    uint32_t rcv_buf_size;
    int rcv_buf_in_use;
} UsbRedirState;

static void usbredir_chardev_wakeup(SpiceCharDeviceInstance *sin)
{
    UsbRedirState *state;
    SpiceCharDeviceInterface *sif;
    int n;

    state = SPICE_CONTAINEROF(sin->st, UsbRedirState, chardev_st);
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (!state->rcc) {
        return;
    }

    do {
        if (!state->pipe_item) {
            state->pipe_item = spice_malloc(sizeof(UsbRedirPipeItem));
            red_channel_pipe_item_init(&state->channel,
                                       &state->pipe_item->base, 0);
        }

        n = sif->read(sin, state->pipe_item->buf,
                      sizeof(state->pipe_item->buf));
        if (n > 0) {
            state->pipe_item->buf_used = n;
            red_channel_client_pipe_add_push(state->rcc,
                                             &state->pipe_item->base);
            state->pipe_item = NULL;
        }
    } while (n > 0);
}

static int usbredir_red_channel_client_config_socket(RedChannelClient *rcc)
{
    return TRUE;
}

static void usbredir_red_channel_client_on_disconnect(RedChannelClient *rcc)
{
    UsbRedirState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    if (!rcc) {
        return;
    }

    state = SPICE_CONTAINEROF(rcc->channel, UsbRedirState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    red_channel_client_destroy(rcc);
    state->rcc = NULL;
    if (sif->state) {
        sif->state(sin, 0);
    }
}

static int usbredir_red_channel_client_handle_message(RedChannelClient *rcc,
    SpiceDataHeader *header, uint8_t *msg)
{
    UsbRedirState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    state = SPICE_CONTAINEROF(rcc->channel, UsbRedirState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (header->type != SPICE_MSGC_USBREDIR_DATA) {
        return red_channel_client_handle_message(rcc, header->size,
                                                 header->type, msg);
    }

    /*
     * qemu usbredir will consume everything we give it, no need for
     * flow control checks (or to use a pipe).
     */
    sif->write(sin, msg, header->size);

    return TRUE;
}

static uint8_t *usbredir_red_channel_alloc_msg_rcv_buf(RedChannelClient *rcc,
    SpiceDataHeader *msg_header)
{
    UsbRedirState *state;

    state = SPICE_CONTAINEROF(rcc->channel, UsbRedirState, channel);

    assert(!state->rcv_buf_in_use);

    if (msg_header->size > state->rcv_buf_size) {
        state->rcv_buf = spice_realloc(state->rcv_buf, msg_header->size);
        state->rcv_buf_size = msg_header->size;
    }

    state->rcv_buf_in_use = 1;

    return state->rcv_buf;
}

static void usbredir_red_channel_release_msg_rcv_buf(RedChannelClient *rcc,
    SpiceDataHeader *msg_header, uint8_t *msg)
{
    UsbRedirState *state;

    state = SPICE_CONTAINEROF(rcc->channel, UsbRedirState, channel);

    /* NOOP, we re-use the buffer every time and only free it on destruction */
    state->rcv_buf_in_use = 0;
}

static void usbredir_red_channel_hold_pipe_item(RedChannelClient *rcc,
    PipeItem *item)
{
    /* NOOP */
}

static void usbredir_red_channel_send_item(RedChannelClient *rcc,
    PipeItem *item)
{
    UsbRedirPipeItem *i = SPICE_CONTAINEROF(item, UsbRedirPipeItem, base);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    red_channel_client_init_send_data(rcc, SPICE_MSG_USBREDIR_DATA, item);
    spice_marshaller_add_ref(m, i->buf, i->buf_used);
    red_channel_client_begin_send_message(rcc);
}

static void usbredir_red_channel_release_pipe_item(RedChannelClient *rcc,
    PipeItem *item, int item_pushed)
{
    free(item);
}

static void usbredir_connect(RedChannel *channel, RedClient *client,
    RedsStream *stream, int migration, int num_common_caps,
    uint32_t *common_caps, int num_caps, uint32_t *caps)
{
    RedChannelClient *rcc;
    UsbRedirState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    state = SPICE_CONTAINEROF(channel, UsbRedirState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (state->rcc) {
        WARN("channel client %d:%d (%p) already connected, refusing second connection\n",
             channel->type, channel->id, state->rcc);
        // TODO: notify client in advance about the in use channel using
        // SPICE_MSG_MAIN_CHANNEL_IN_USE (for example)
        reds_stream_free(stream);
        return;
    }

    rcc = red_channel_client_create(sizeof(RedChannelClient), channel, client, stream);
    if (!rcc) {
        return;
    }
    state->rcc = rcc;
    red_channel_client_ack_zero_messages_window(rcc);

    if (sif->state) {
        sif->state(sin, 1);
    }
}

static void usbredir_migrate(RedChannelClient *rcc)
{
    /* NOOP */
}

int usbredir_device_connect(SpiceCharDeviceInstance *sin)
{
    static int id = 0;
    UsbRedirState *state;
    ChannelCbs channel_cbs = {0,};
    ClientCbs client_cbs = {0,};

    channel_cbs.config_socket = usbredir_red_channel_client_config_socket;
    channel_cbs.on_disconnect = usbredir_red_channel_client_on_disconnect;
    channel_cbs.send_item = usbredir_red_channel_send_item;
    channel_cbs.hold_item = usbredir_red_channel_hold_pipe_item;
    channel_cbs.release_item = usbredir_red_channel_release_pipe_item;
    channel_cbs.alloc_recv_buf = usbredir_red_channel_alloc_msg_rcv_buf;
    channel_cbs.release_recv_buf = usbredir_red_channel_release_msg_rcv_buf;

    state = (UsbRedirState*)red_channel_create(sizeof(UsbRedirState),
                                   core, SPICE_CHANNEL_USBREDIR, id++,
                                   FALSE /* migration - TODO? */,
                                   FALSE /* handle_acks */,
                                   usbredir_red_channel_client_handle_message,
                                   &channel_cbs);
    red_channel_init_outgoing_messages_window(&state->channel);
    state->chardev_st.wakeup = usbredir_chardev_wakeup;
    state->chardev_sin = sin;
    state->rcv_buf = spice_malloc(BUF_SIZE);
    state->rcv_buf_size = BUF_SIZE;

    client_cbs.connect = usbredir_connect;
    client_cbs.migrate = usbredir_migrate;
    red_channel_register_client_cbs(&state->channel, &client_cbs);

    sin->st = &state->chardev_st;

    reds_register_channel(&state->channel);

    return 0;
}

/* Must be called from RedClient handling thread. */
void usbredir_device_disconnect(SpiceCharDeviceInstance *sin)
{
    UsbRedirState *state;

    state = SPICE_CONTAINEROF(sin->st, UsbRedirState, chardev_st);

    reds_unregister_channel(&state->channel);

    free(state->pipe_item);
    free(state->rcv_buf);
    red_channel_destroy(&state->channel);
}

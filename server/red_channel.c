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


    Author:
        yhalperi@redhat.com
*/

#include <stdio.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "stat.h"
#include "red_channel.h"

static PipeItem *red_channel_pipe_get(RedChannel *channel);
static void red_channel_pipe_clear(RedChannel *channel);
static void red_channel_event(int fd, int event, void *data);

/* return the number of bytes read. -1 in case of error */
static int red_peer_receive(RedsStreamContext *peer, uint8_t *buf, uint32_t size)
{
    uint8_t *pos = buf;
    while (size) {
        int now;
        if (peer->shutdown) {
            return -1;
        }
        if ((now = peer->cb_read(peer->ctx, pos, size)) <= 0) {
            if (now == 0) {
                return -1;
            }
            ASSERT(now == -1);
            if (errno == EAGAIN) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EPIPE) {
                return -1;
            } else {
                red_printf("%s", strerror(errno));
                return -1;
            }
        } else {
            size -= now;
            pos += now;
        }
    }
    return pos - buf;
}

static void red_peer_handle_incoming(RedsStreamContext *peer, IncomingHandler *handler)
{
    int bytes_read;
    uint8_t *parsed;
    size_t parsed_size;
    message_destructor_t parsed_free;

    for (;;) {
        int ret_handle;
        if (handler->header_pos < sizeof(SpiceDataHeader)) {
            bytes_read = red_peer_receive(peer,
                                          ((uint8_t *)&handler->header) + handler->header_pos,
                                          sizeof(SpiceDataHeader) - handler->header_pos);
            if (bytes_read == -1) {
                handler->on_error(handler->opaque);
                return;
            }
            handler->header_pos += bytes_read;

            if (handler->header_pos != sizeof(SpiceDataHeader)) {
                return;
            }
        }

        if (handler->msg_pos < handler->header.size) {
            if (!handler->msg) {
                handler->msg = handler->alloc_msg_buf(handler->opaque, &handler->header);
                if (handler->msg == NULL) {
                    red_printf("ERROR: channel refused to allocate buffer.");
                    handler->on_error(handler->opaque);
                    return;
                }
            }

            bytes_read = red_peer_receive(peer,
                                          handler->msg + handler->msg_pos,
                                          handler->header.size - handler->msg_pos);
            if (bytes_read == -1) {
                handler->release_msg_buf(handler->opaque, &handler->header, handler->msg);
                handler->on_error(handler->opaque);
                return;
            }
            handler->msg_pos += bytes_read;
            if (handler->msg_pos != handler->header.size) {
                return;
            }
        }

        if (handler->parser) {
            parsed = handler->parser(handler->msg,
                handler->msg + handler->header.size, handler->header.type,
                SPICE_VERSION_MINOR, &parsed_size, &parsed_free);
            if (parsed == NULL) {
                red_printf("failed to parse message type %d", handler->header.type);
                handler->on_error(handler->opaque);
                return;
            }
            ret_handle = handler->handle_parsed(handler->opaque, parsed_size,
                                    handler->header.type, parsed);
            parsed_free(parsed);
        } else {
            ret_handle = handler->handle_message(handler->opaque, &handler->header,
                                                 handler->msg);
        }
        if (handler->shut) {
            handler->on_error(handler->opaque);
            return;
        }
        handler->msg_pos = 0;
        handler->msg = NULL;
        handler->header_pos = 0;

        if (!ret_handle) {
            handler->on_error(handler->opaque);
            return;
        }
    }
}

static void red_peer_handle_outgoing(RedsStreamContext *peer, OutgoingHandler *handler)
{
    int n;
    if (handler->size == 0) {
        handler->vec = handler->vec_buf;
        handler->size = handler->get_msg_size(handler->opaque);
        if (!handler->size) {  // nothing to be sent
            return;
        }
    }
    for (;;) {
        handler->prepare(handler->opaque, handler->vec, &handler->vec_size, handler->pos);
        if ((n = peer->cb_writev(peer->ctx, handler->vec, handler->vec_size)) == -1) {
            switch (errno) {
            case EAGAIN:
                handler->on_block(handler->opaque);
                return;
            case EINTR:
                continue;
            case EPIPE:
                handler->on_error(handler->opaque);
                return;
            default:
                red_printf("%s", strerror(errno));
                handler->on_error(handler->opaque);
                return;
            }
        } else {
            handler->pos += n;
            stat_inc_counter(handler->out_bytes_counter, n);
            if (handler->pos == handler->size) { // finished writing data
                handler->on_msg_done(handler->opaque);
                handler->vec = handler->vec_buf;
                handler->pos = 0;
                handler->size = 0;
                return;
            }
        }
    }
}

void red_channel_default_peer_on_error(RedChannel *channel)
{
    channel->disconnect(channel);
}

static void red_channel_peer_on_incoming_error(void *opaque)
{
    RedChannel *channel = (RedChannel *)opaque;

    channel->on_incoming_error(channel);
}

static void red_channel_peer_on_outgoing_error(void *opaque)
{
    RedChannel *channel = (RedChannel *)opaque;

    channel->on_outgoing_error(channel);
}

static int red_channel_peer_get_out_msg_size(void *opaque)
{
    RedChannel *channel = (RedChannel *)opaque;

    return channel->send_data.size;
}

static void red_channel_peer_prepare_out_msg(void *opaque, struct iovec *vec, int *vec_size, int pos)
{
    RedChannel *channel = (RedChannel *)opaque;

    *vec_size = spice_marshaller_fill_iovec(channel->send_data.marshaller,
                                            vec, MAX_SEND_VEC, pos);
}

static void red_channel_peer_on_out_block(void *opaque)
{
    RedChannel *channel = (RedChannel *)opaque;

    channel->send_data.blocked = TRUE;
    channel->core->watch_update_mask(channel->peer->watch,
                                     SPICE_WATCH_EVENT_READ |
                                     SPICE_WATCH_EVENT_WRITE);
}

static void red_channel_peer_on_out_msg_done(void *opaque)
{
    RedChannel *channel = (RedChannel *)opaque;
    channel->send_data.size = 0;
    if (channel->send_data.item) {
        channel->release_item(channel, channel->send_data.item, TRUE);
        channel->send_data.item = NULL;
    }
    if (channel->send_data.blocked) {
        channel->send_data.blocked = FALSE;
        channel->core->watch_update_mask(channel->peer->watch,
                                         SPICE_WATCH_EVENT_READ);
    }
}

RedChannel *red_channel_create(int size, RedsStreamContext *peer,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_configure_socket_proc config_socket,
                               channel_disconnect_proc disconnect,
                               channel_handle_message_proc handle_message,
                               channel_alloc_msg_recv_buf_proc alloc_recv_buf,
                               channel_release_msg_recv_buf_proc release_recv_buf,
                               channel_hold_pipe_item_proc hold_item,
                               channel_send_pipe_item_proc send_item,
                               channel_release_pipe_item_proc release_item)
{
    RedChannel *channel;

    ASSERT(size >= sizeof(*channel));
    ASSERT(config_socket && disconnect && handle_message && alloc_recv_buf &&
           release_item);
    channel = spice_malloc0(size);

    channel->handle_acks = handle_acks;
    channel->disconnect = disconnect;
    channel->send_item = send_item;
    channel->release_item = release_item;
    channel->hold_item = hold_item;

    channel->peer = peer;
    channel->core = core;
    channel->ack_data.messages_window = ~0;  // blocks send message (maybe use send_data.blocked +
                                             // block flags)
    channel->ack_data.client_generation = ~0;
    channel->ack_data.client_window = CLIENT_ACK_WINDOW;

    channel->migrate = migrate;
    ring_init(&channel->pipe);
    channel->send_data.marshaller = spice_marshaller_new();

    channel->incoming.opaque = channel;
    channel->incoming.alloc_msg_buf = (alloc_msg_recv_buf_proc)alloc_recv_buf;
    channel->incoming.release_msg_buf = (release_msg_recv_buf_proc)release_recv_buf;
    channel->incoming.handle_message = (handle_message_proc)handle_message;
    channel->incoming.on_error = (on_incoming_error_proc)red_channel_default_peer_on_error;

    channel->outgoing.opaque = channel;
    channel->outgoing.pos = 0;
    channel->outgoing.size = 0;
    channel->outgoing.out_bytes_counter = NULL;

    channel->outgoing.get_msg_size = red_channel_peer_get_out_msg_size;
    channel->outgoing.prepare = red_channel_peer_prepare_out_msg;
    channel->outgoing.on_block = red_channel_peer_on_out_block;
    channel->outgoing.on_error = (on_outgoing_error_proc)red_channel_default_peer_on_error;
    channel->outgoing.on_msg_done = red_channel_peer_on_out_msg_done;

    channel->shut = 0; // came here from inputs, perhaps can be removed? XXX

    if (!config_socket(channel)) {
        goto error;
    }

    channel->peer->watch = channel->core->watch_add(channel->peer->socket,
                                                    SPICE_WATCH_EVENT_READ,
                                                    red_channel_event, channel);

    return channel;

error:
    spice_marshaller_destroy(channel->send_data.marshaller);
    free(channel);
    peer->cb_free(peer);

    return NULL;
}

void do_nothing_disconnect(RedChannel *red_channel)
{
}

int do_nothing_handle_message(RedChannel *red_channel, SpiceDataHeader *header, uint8_t *msg)
{
    return TRUE;
}

RedChannel *red_channel_create_parser(int size, RedsStreamContext *peer,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_configure_socket_proc config_socket,
                               spice_parse_channel_func_t parser,
                               channel_handle_parsed_proc handle_parsed,
                               channel_alloc_msg_recv_buf_proc alloc_recv_buf,
                               channel_release_msg_recv_buf_proc release_recv_buf,
                               channel_hold_pipe_item_proc hold_item,
                               channel_send_pipe_item_proc send_item,
                               channel_release_pipe_item_proc release_item,
                               channel_on_incoming_error_proc incoming_error,
                               channel_on_outgoing_error_proc outgoing_error)
{
    RedChannel *channel = red_channel_create(size, peer,
        core, migrate, handle_acks, config_socket, do_nothing_disconnect,
        do_nothing_handle_message, alloc_recv_buf, release_recv_buf, hold_item,
        send_item, release_item);

    if (channel == NULL) {
        return NULL;
    }
    channel->incoming.handle_parsed = (handle_parsed_proc)handle_parsed;
    channel->incoming.parser = parser;
    channel->on_incoming_error = incoming_error;
    channel->on_outgoing_error = outgoing_error;
    channel->incoming.on_error = red_channel_peer_on_incoming_error;
    channel->outgoing.on_error = red_channel_peer_on_outgoing_error;
    return channel;
}

void red_channel_destroy(RedChannel *channel)
{
    if (!channel) {
        return;
    }
    red_channel_pipe_clear(channel);
    channel->core->watch_remove(channel->peer->watch);
    channel->peer->cb_free(channel->peer);
    spice_marshaller_destroy(channel->send_data.marshaller);
    free(channel);
}

void red_channel_shutdown(RedChannel *channel)
{
    red_printf("");
    if (channel->peer && !channel->peer->shutdown) {
        channel->core->watch_update_mask(channel->peer->watch,
                                         SPICE_WATCH_EVENT_READ);
        red_channel_pipe_clear(channel);
        shutdown(channel->peer->socket, SHUT_RDWR);
        channel->peer->shutdown = TRUE;
    }
}

void red_channel_init_outgoing_messages_window(RedChannel *channel)
{
    channel->ack_data.messages_window = 0;
    red_channel_push(channel);
}

int red_channel_handle_message(RedChannel *channel, uint32_t size,
                               uint16_t type, void *message)
{
    switch (type) {
    case SPICE_MSGC_ACK_SYNC:
        if (size != sizeof(uint32_t)) {
            red_printf("bad message size");
            return FALSE;
        }
        channel->ack_data.client_generation = *(uint32_t *)(message);
        break;
    case SPICE_MSGC_ACK:
        if (channel->ack_data.client_generation == channel->ack_data.generation) {
            channel->ack_data.messages_window -= channel->ack_data.client_window;
            red_channel_push(channel);
        }
        break;
    default:
        red_printf("invalid message type %u", type);
        return FALSE;
    }
    return TRUE;
}

static void red_channel_event(int fd, int event, void *data)
{
    RedChannel *channel = (RedChannel *)data;

    if (event & SPICE_WATCH_EVENT_READ) {
        red_peer_handle_incoming(channel->peer, &channel->incoming);
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        red_channel_push(channel);
    }
}

void red_channel_add_buf(RedChannel *channel, void *data, uint32_t size)
{
    spice_marshaller_add_ref(channel->send_data.marshaller, data, size);
    channel->send_data.header->size += size;
}

void red_channel_reset_send_data(RedChannel *channel)
{
    spice_marshaller_reset(channel->send_data.marshaller);
    channel->send_data.header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(channel->send_data.marshaller, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(channel->send_data.marshaller, sizeof(SpiceDataHeader));
    channel->send_data.header->type = 0;
    channel->send_data.header->size = 0;
    channel->send_data.header->sub_list = 0;
    channel->send_data.header->serial = ++channel->send_data.serial;
}

void red_channel_init_send_data(RedChannel *channel, uint16_t msg_type, PipeItem *item)
{
    channel->send_data.header->type = msg_type;
    if (item) {
        ASSERT(channel->send_data.item == NULL);
        channel->send_data.item = item;
        channel->hold_item(item);
    }
}

static void red_channel_send(RedChannel *channel)
{
    red_peer_handle_outgoing(channel->peer, &channel->outgoing);
}

void red_channel_begin_send_message(RedChannel *channel)
{
    spice_marshaller_flush(channel->send_data.marshaller);
    channel->send_data.size = spice_marshaller_get_total_size(channel->send_data.marshaller);
    channel->send_data.header->size =  channel->send_data.size - sizeof(SpiceDataHeader);
    channel->ack_data.messages_window++;
    channel->send_data.header = NULL; /* avoid writing to this until we have a new message */
    red_channel_send(channel);
}

void red_channel_push(RedChannel *channel)
{
    PipeItem *pipe_item;

    if (!channel->during_send) {
        channel->during_send = TRUE;
    } else {
        return;
    }

    if (channel->send_data.blocked) {
        red_channel_send(channel);
    }

    while ((pipe_item = red_channel_pipe_get(channel))) {
        channel->send_item(channel, pipe_item);
    }
    channel->during_send = FALSE;
}

uint64_t red_channel_get_message_serial(RedChannel *channel)
{
    return channel->send_data.serial;
}

void red_channel_set_message_serial(RedChannel *channel, uint64_t serial)
{
    channel->send_data.serial = serial;
}

void red_channel_pipe_item_init(RedChannel *channel, PipeItem *item, int type)
{
    ring_item_init(&item->link);
    item->type = type;
}

void red_channel_pipe_add(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);

    channel->pipe_size++;
    ring_add(&channel->pipe, &item->link);
}

void red_channel_pipe_add_push(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);

    channel->pipe_size++;
    ring_add(&channel->pipe, &item->link);
    red_channel_push(channel);
}

void red_channel_pipe_add_after(RedChannel *channel, PipeItem *item, PipeItem *pos)
{
    ASSERT(channel);
    ASSERT(pos);
    ASSERT(item);

    channel->pipe_size++;
    ring_add_after(&item->link, &pos->link);
}

int red_channel_pipe_item_is_linked(RedChannel *channel, PipeItem *item)
{
    return ring_item_is_linked(&item->link);
}

void red_channel_pipe_item_remove(RedChannel *channel, PipeItem *item)
{
    ring_remove(&item->link);
}

void red_channel_pipe_add_tail(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);
    channel->pipe_size++;
    ring_add_before(&item->link, &channel->pipe);

    red_channel_push(channel);
}

void red_channel_pipe_add_type(RedChannel *channel, int pipe_item_type)
{
    PipeItem *item = spice_new(PipeItem, 1);
    red_channel_pipe_item_init(channel, item, pipe_item_type);
    red_channel_pipe_add(channel, item);

    red_channel_push(channel);
}

static PipeItem *red_channel_pipe_get(RedChannel *channel)
{
    PipeItem *item;

    if (!channel || channel->send_data.blocked ||
        (channel->handle_acks &&
         (channel->ack_data.messages_window > channel->ack_data.client_window * 2)) ||
        !(item = (PipeItem *)ring_get_tail(&channel->pipe))) {
        return NULL;
    }

    --channel->pipe_size;
    ring_remove(&item->link);
    return item;
}

int red_channel_is_connected(RedChannel *channel)
{
    return !!channel->peer;
}

static void red_channel_pipe_clear(RedChannel *channel)
{
    PipeItem *item;
    if (channel->send_data.item) {
        channel->release_item(channel, channel->send_data.item, TRUE);
    }

    while ((item = (PipeItem *)ring_get_head(&channel->pipe))) {
        ring_remove(&item->link);
        channel->release_item(channel, item, FALSE);
    }
}


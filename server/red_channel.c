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
#include "generated_marshallers.h"

static PipeItem *red_channel_pipe_get(RedChannel *channel);
static void red_channel_event(int fd, int event, void *data);

/* return the number of bytes read. -1 in case of error */
static int red_peer_receive(RedsStream *stream, uint8_t *buf, uint32_t size)
{
    uint8_t *pos = buf;
    while (size) {
        int now;
        if (stream->shutdown) {
            return -1;
        }
        now = reds_stream_read(stream, pos, size);
        if (now <= 0) {
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

// TODO: this implementation, as opposed to the old implementation in red_worker,
// does many calls to red_peer_receive and through it cb_read, and thus avoids pointer
// arithmetic for the case where a single cb_read could return multiple messages. But
// this is suboptimal potentially. Profile and consider fixing.
static void red_peer_handle_incoming(RedsStream *stream, IncomingHandler *handler)
{
    int bytes_read;
    uint8_t *parsed;
    size_t parsed_size;
    message_destructor_t parsed_free;

    for (;;) {
        int ret_handle;
        if (handler->header_pos < sizeof(SpiceDataHeader)) {
            bytes_read = red_peer_receive(stream,
                                          ((uint8_t *)&handler->header) + handler->header_pos,
                                          sizeof(SpiceDataHeader) - handler->header_pos);
            if (bytes_read == -1) {
                handler->cb->on_error(handler->opaque);
                return;
            }
            handler->header_pos += bytes_read;

            if (handler->header_pos != sizeof(SpiceDataHeader)) {
                return;
            }
        }

        if (handler->msg_pos < handler->header.size) {
            if (!handler->msg) {
                handler->msg = handler->cb->alloc_msg_buf(handler->opaque, &handler->header);
                if (handler->msg == NULL) {
                    red_printf("ERROR: channel refused to allocate buffer.");
                    handler->cb->on_error(handler->opaque);
                    return;
                }
            }

            bytes_read = red_peer_receive(stream,
                                          handler->msg + handler->msg_pos,
                                          handler->header.size - handler->msg_pos);
            if (bytes_read == -1) {
                handler->cb->release_msg_buf(handler->opaque, &handler->header, handler->msg);
                handler->cb->on_error(handler->opaque);
                return;
            }
            handler->msg_pos += bytes_read;
            if (handler->msg_pos != handler->header.size) {
                return;
            }
        }

        if (handler->cb->parser) {
            parsed = handler->cb->parser(handler->msg,
                handler->msg + handler->header.size, handler->header.type,
                SPICE_VERSION_MINOR, &parsed_size, &parsed_free);
            if (parsed == NULL) {
                red_printf("failed to parse message type %d", handler->header.type);
                handler->cb->on_error(handler->opaque);
                return;
            }
            ret_handle = handler->cb->handle_parsed(handler->opaque, parsed_size,
                                    handler->header.type, parsed);
            parsed_free(parsed);
        } else {
            ret_handle = handler->cb->handle_message(handler->opaque, &handler->header,
                                                 handler->msg);
        }
        if (handler->shut) {
            handler->cb->on_error(handler->opaque);
            return;
        }
        handler->msg_pos = 0;
        handler->msg = NULL;
        handler->header_pos = 0;

        if (!ret_handle) {
            handler->cb->on_error(handler->opaque);
            return;
        }
    }
}

void red_channel_receive(RedChannel *channel)
{
    red_peer_handle_incoming(channel->stream, &channel->incoming);
}

static void red_peer_handle_outgoing(RedsStream *stream, OutgoingHandler *handler)
{
    ssize_t n;

    if (handler->size == 0) {
        handler->vec = handler->vec_buf;
        handler->size = handler->cb->get_msg_size(handler->opaque);
        if (!handler->size) {  // nothing to be sent
            return;
        }
    }

    for (;;) {
        handler->cb->prepare(handler->opaque, handler->vec, &handler->vec_size, handler->pos);
        n = reds_stream_writev(stream, handler->vec, handler->vec_size);
        if (n == -1) {
            switch (errno) {
            case EAGAIN:
                handler->cb->on_block(handler->opaque);
                return;
            case EINTR:
                continue;
            case EPIPE:
                handler->cb->on_error(handler->opaque);
                return;
            default:
                red_printf("%s", strerror(errno));
                handler->cb->on_error(handler->opaque);
                return;
            }
        } else {
            handler->pos += n;
            handler->cb->on_output(handler->opaque, n);
            if (handler->pos == handler->size) { // finished writing data
                handler->cb->on_msg_done(handler->opaque);
                handler->vec = handler->vec_buf;
                handler->pos = 0;
                handler->size = 0;
                return;
            }
        }
    }
}

void red_channel_on_output(void *opaque, int n)
{
    RedChannel *channel = opaque;

    stat_inc_counter(channel->out_bytes_counter, n);
}

void red_channel_default_peer_on_error(RedChannel *channel)
{
    channel->disconnect(channel);
}

static void red_channel_peer_on_incoming_error(RedChannel *channel)
{
    channel->on_incoming_error(channel);
}

static void red_channel_peer_on_outgoing_error(RedChannel *channel)
{
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
    channel->core->watch_update_mask(channel->stream->watch,
                                     SPICE_WATCH_EVENT_READ |
                                     SPICE_WATCH_EVENT_WRITE);
}

static void red_channel_reset_send_data(RedChannel *channel)
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

void red_channel_push_set_ack(RedChannel *channel)
{
    red_channel_pipe_add_type(channel, PIPE_ITEM_TYPE_SET_ACK);
}

static void red_channel_send_set_ack(RedChannel *channel)
{
    SpiceMsgSetAck ack;

    ASSERT(channel);
    red_channel_init_send_data(channel, SPICE_MSG_SET_ACK, NULL);
    ack.generation = ++channel->ack_data.generation;
    ack.window = channel->ack_data.client_window;
    channel->ack_data.messages_window = 0;

    spice_marshall_msg_set_ack(channel->send_data.marshaller, &ack);

    red_channel_begin_send_message(channel);
}

static void red_channel_send_item(RedChannel *channel, PipeItem *item)
{
    red_channel_reset_send_data(channel);
    switch (item->type) {
        case PIPE_ITEM_TYPE_SET_ACK:
            red_channel_send_set_ack(channel);
            return;
    }
    /* only reached if not handled here */
    channel->send_item(channel, item);
}

static void red_channel_release_item(RedChannel *channel, PipeItem *item, int item_pushed)
{
    switch (item->type) {
        case PIPE_ITEM_TYPE_SET_ACK:
            free(item);
            return;
    }
    /* only reached if not handled here */
    channel->release_item(channel, item, item_pushed);
}

static void red_channel_peer_on_out_msg_done(void *opaque)
{
    RedChannel *channel = (RedChannel *)opaque;
    channel->send_data.size = 0;
    if (channel->send_data.item) {
        red_channel_release_item(channel, channel->send_data.item, TRUE);
        channel->send_data.item = NULL;
    }
    if (channel->send_data.blocked) {
        channel->send_data.blocked = FALSE;
        channel->core->watch_update_mask(channel->stream->watch,
                                         SPICE_WATCH_EVENT_READ);
    }
}

RedChannel *red_channel_create(int size, RedsStream *stream,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_configure_socket_proc config_socket,
                               channel_disconnect_proc disconnect,
                               channel_handle_message_proc handle_message,
                               channel_alloc_msg_recv_buf_proc alloc_recv_buf,
                               channel_release_msg_recv_buf_proc release_recv_buf,
                               channel_hold_pipe_item_proc hold_item,
                               channel_send_pipe_item_proc send_item,
                               channel_release_pipe_item_proc release_item,
                               channel_handle_migrate_flush_mark handle_migrate_flush_mark,
                               channel_handle_migrate_data handle_migrate_data,
                               channel_handle_migrate_data_get_serial handle_migrate_data_get_serial)
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
    channel->handle_migrate_flush_mark = handle_migrate_flush_mark;
    channel->handle_migrate_data = handle_migrate_data;
    channel->handle_migrate_data_get_serial = handle_migrate_data_get_serial;

    channel->stream = stream;
    channel->core = core;
    channel->ack_data.messages_window = ~0;  // blocks send message (maybe use send_data.blocked +
                                             // block flags)
    channel->ack_data.client_generation = ~0;
    channel->ack_data.client_window = CLIENT_ACK_WINDOW;

    channel->migrate = migrate;
    ring_init(&channel->pipe);
    channel->send_data.marshaller = spice_marshaller_new();

    channel->incoming.opaque = channel;
    channel->incoming_cb.alloc_msg_buf = (alloc_msg_recv_buf_proc)alloc_recv_buf;
    channel->incoming_cb.release_msg_buf = (release_msg_recv_buf_proc)release_recv_buf;
    channel->incoming_cb.handle_message = (handle_message_proc)handle_message;
    channel->incoming_cb.on_error = (on_incoming_error_proc)red_channel_default_peer_on_error;

    channel->outgoing.opaque = channel;
    channel->outgoing.pos = 0;
    channel->outgoing.size = 0;

    channel->outgoing_cb.get_msg_size = red_channel_peer_get_out_msg_size;
    channel->outgoing_cb.prepare = red_channel_peer_prepare_out_msg;
    channel->outgoing_cb.on_block = red_channel_peer_on_out_block;
    channel->outgoing_cb.on_error = (on_outgoing_error_proc)red_channel_default_peer_on_error;
    channel->outgoing_cb.on_msg_done = red_channel_peer_on_out_msg_done;
    channel->outgoing_cb.on_output = red_channel_on_output;

    channel->incoming.cb = &channel->incoming_cb;
    channel->outgoing.cb = &channel->outgoing_cb;

    channel->shut = 0; // came here from inputs, perhaps can be removed? XXX
    channel->out_bytes_counter = 0;

    if (!config_socket(channel)) {
        goto error;
    }

    channel->stream->watch = channel->core->watch_add(channel->stream->socket,
                                                    SPICE_WATCH_EVENT_READ,
                                                    red_channel_event, channel);

    return channel;

error:
    spice_marshaller_destroy(channel->send_data.marshaller);
    free(channel);
    reds_stream_free(stream);

    return NULL;
}

void do_nothing_disconnect(RedChannel *red_channel)
{
}

int do_nothing_handle_message(RedChannel *red_channel, SpiceDataHeader *header, uint8_t *msg)
{
    return TRUE;
}

RedChannel *red_channel_create_parser(int size, RedsStream *stream,
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
                               channel_on_outgoing_error_proc outgoing_error,
                               channel_handle_migrate_flush_mark handle_migrate_flush_mark,
                               channel_handle_migrate_data handle_migrate_data,
                               channel_handle_migrate_data_get_serial handle_migrate_data_get_serial)
{
    RedChannel *channel = red_channel_create(size, stream,
        core, migrate, handle_acks, config_socket, do_nothing_disconnect,
        do_nothing_handle_message, alloc_recv_buf, release_recv_buf, hold_item,
        send_item, release_item, handle_migrate_flush_mark, handle_migrate_data,
        handle_migrate_data_get_serial);

    if (channel == NULL) {
        return NULL;
    }
    channel->incoming_cb.handle_parsed = (handle_parsed_proc)handle_parsed;
    channel->incoming_cb.parser = parser;
    channel->on_incoming_error = incoming_error;
    channel->on_outgoing_error = outgoing_error;
    channel->incoming_cb.on_error = (on_incoming_error_proc)red_channel_peer_on_incoming_error;
    channel->outgoing_cb.on_error = (on_outgoing_error_proc)red_channel_peer_on_outgoing_error;
    return channel;
}

void red_channel_destroy(RedChannel *channel)
{
    if (!channel) {
        return;
    }
    red_channel_pipe_clear(channel);
    reds_stream_free(channel->stream);
    spice_marshaller_destroy(channel->send_data.marshaller);
    free(channel);
}

void red_channel_shutdown(RedChannel *channel)
{
    red_printf("");
    if (channel->stream && !channel->stream->shutdown) {
        channel->core->watch_update_mask(channel->stream->watch,
                                         SPICE_WATCH_EVENT_READ);
        red_channel_pipe_clear(channel);
        shutdown(channel->stream->socket, SHUT_RDWR);
        channel->stream->shutdown = TRUE;
        channel->incoming.shut = TRUE;
    }
}

void red_channel_init_outgoing_messages_window(RedChannel *channel)
{
    channel->ack_data.messages_window = 0;
    red_channel_push(channel);
}

void red_channel_handle_migrate_flush_mark(RedChannel *channel)
{
    if (channel->handle_migrate_flush_mark) {
        channel->handle_migrate_flush_mark(channel);
    }
}

void red_channel_handle_migrate_data(RedChannel *channel, uint32_t size, void *message)
{
    if (!channel->handle_migrate_data) {
        return;
    }
    ASSERT(red_channel_get_message_serial(channel) == 0);
    red_channel_set_message_serial(channel,
        channel->handle_migrate_data_get_serial(channel, size, message));
    channel->handle_migrate_data(channel, size, message);
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
    case SPICE_MSGC_DISCONNECTING:
        break;
    case SPICE_MSGC_MIGRATE_FLUSH_MARK:
        red_channel_handle_migrate_flush_mark(channel);
        break;
    case SPICE_MSGC_MIGRATE_DATA:
        red_channel_handle_migrate_data(channel, size, message);
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
        red_channel_receive(channel);
    }
    // TODO: || channel->send_data.blocked ? (from red_worker. doesn't really make sense if we have an event
    // fired in that case)
    if (event & SPICE_WATCH_EVENT_WRITE || channel->send_data.blocked) {
        if (channel->send_data.blocked && ! (event & SPICE_WATCH_EVENT_WRITE)) {
            red_printf("pushing because of blocked");
        }
        red_channel_push(channel);
    }
}

void red_channel_init_send_data(RedChannel *channel, uint16_t msg_type, PipeItem *item)
{
    ASSERT(channel->send_data.item == NULL);
    channel->send_data.header->type = msg_type;
    channel->send_data.item = item;
    if (item) {
        channel->hold_item(channel, item);
    }
}

void red_channel_send(RedChannel *channel)
{
    red_peer_handle_outgoing(channel->stream, &channel->outgoing);
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

    if (!channel) {
        return;
    }

    if (!channel->during_send) {
        channel->during_send = TRUE;
    } else {
        return;
    }

    if (channel->send_data.blocked) {
        red_channel_send(channel);
    }

    while ((pipe_item = red_channel_pipe_get(channel))) {
        red_channel_send_item(channel, pipe_item);
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

static inline int red_channel_waiting_for_ack(RedChannel *channel)
{
    return (channel->handle_acks && (channel->ack_data.messages_window > channel->ack_data.client_window * 2));
}

static inline PipeItem *red_channel_pipe_get(RedChannel *channel)
{
    PipeItem *item;

    if (!channel || channel->send_data.blocked ||
        red_channel_waiting_for_ack(channel) ||
        !(item = (PipeItem *)ring_get_tail(&channel->pipe))) {
        return NULL;
    }
    --channel->pipe_size;
    ring_remove(&item->link);
    return item;
}

int red_channel_is_connected(RedChannel *channel)
{
    return !!channel->stream;
}

void red_channel_pipe_clear(RedChannel *channel)
{
    PipeItem *item;

    ASSERT(channel);
    if (channel->send_data.item) {
        red_channel_release_item(channel, channel->send_data.item, TRUE);
        channel->send_data.item = NULL;
    }
    while ((item = (PipeItem *)ring_get_head(&channel->pipe))) {
        ring_remove(&item->link);
        red_channel_release_item(channel, item, FALSE);
    }
    channel->pipe_size = 0;
}

void red_channel_ack_zero_messages_window(RedChannel *channel)
{
    channel->ack_data.messages_window = 0;
}

void red_channel_ack_set_client_window(RedChannel *channel, int client_window)
{
    channel->ack_data.client_window = client_window;
}

int red_channel_all_blocked(RedChannel *channel)
{
    return channel->send_data.blocked;
}

int red_channel_any_blocked(RedChannel *channel)
{
    return channel->send_data.blocked;
}

int red_channel_send_message_pending(RedChannel *channel)
{
    return channel->send_data.header->type != 0;
}

/* accessors for RedChannel */
SpiceMarshaller *red_channel_get_marshaller(RedChannel *channel)
{
    return channel->send_data.marshaller;
}

RedsStream *red_channel_get_stream(RedChannel *channel)
{
    return channel->stream;
}

SpiceDataHeader *red_channel_get_header(RedChannel *channel)
{
    return channel->send_data.header;
}
/* end of accessors */

int red_channel_get_first_socket(RedChannel *channel)
{
    if (!channel->stream) {
        return -1;
    }
    return channel->stream->socket;
}

int red_channel_item_being_sent(RedChannel *channel, PipeItem *item)
{
    return channel->send_data.item == item;
}

int red_channel_no_item_being_sent(RedChannel *channel)
{
    return channel->send_data.item == NULL;
}

void red_channel_disconnect(RedChannel *channel)
{
    red_channel_pipe_clear(channel);
    reds_stream_free(channel->stream);
    channel->stream = NULL;
    channel->send_data.blocked = FALSE;
    channel->send_data.size = 0;
}

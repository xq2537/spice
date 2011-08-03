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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "ring.h"
#include "stat.h"
#include "red_channel.h"
#include "generated_marshallers.h"

static void red_channel_client_event(int fd, int event, void *data);
static void red_client_add_channel(RedClient *client, RedChannelClient *rcc);

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

void red_channel_client_receive(RedChannelClient *rcc)
{
    red_peer_handle_incoming(rcc->stream, &rcc->incoming);
}

void red_channel_receive(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;
    RedChannelClient *rcc;

    RING_FOREACH_SAFE(link, next, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        red_channel_client_receive(rcc);
    }
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

static void red_channel_client_on_output(void *opaque, int n)
{
    RedChannelClient *rcc = opaque;

    stat_inc_counter(rcc->channel->out_bytes_counter, n);
}

void red_channel_client_default_peer_on_error(RedChannelClient *rcc)
{
    rcc->channel->channel_cbs.disconnect(rcc);
}

static void red_channel_peer_on_incoming_error(RedChannelClient *rcc)
{
    if (rcc->stream->shutdown) {
        return; // assume error has already been handled which caused the shutdown.
    }
    rcc->channel->on_incoming_error(rcc);
}

static void red_channel_peer_on_outgoing_error(RedChannelClient *rcc)
{
    rcc->channel->on_outgoing_error(rcc);
}

static int red_channel_client_peer_get_out_msg_size(void *opaque)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    return rcc->send_data.size;
}

static void red_channel_client_peer_prepare_out_msg(
    void *opaque, struct iovec *vec, int *vec_size, int pos)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    *vec_size = spice_marshaller_fill_iovec(rcc->send_data.marshaller,
                                            vec, MAX_SEND_VEC, pos);
}

static void red_channel_client_peer_on_out_block(void *opaque)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    rcc->send_data.blocked = TRUE;
    rcc->channel->core->watch_update_mask(rcc->stream->watch,
                                     SPICE_WATCH_EVENT_READ |
                                     SPICE_WATCH_EVENT_WRITE);
}

static void red_channel_client_reset_send_data(RedChannelClient *rcc)
{
    spice_marshaller_reset(rcc->send_data.marshaller);
    rcc->send_data.header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(rcc->send_data.marshaller, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(rcc->send_data.marshaller, sizeof(SpiceDataHeader));
    rcc->send_data.header->type = 0;
    rcc->send_data.header->size = 0;
    rcc->send_data.header->sub_list = 0;
    rcc->send_data.header->serial = ++rcc->send_data.serial;
}

void red_channel_client_push_set_ack(RedChannelClient *rcc)
{
    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_SET_ACK);
}

void red_channel_push_set_ack(RedChannel *channel)
{
    red_channel_pipes_add_type(channel, PIPE_ITEM_TYPE_SET_ACK);
}

static void red_channel_client_send_set_ack(RedChannelClient *rcc)
{
    SpiceMsgSetAck ack;

    ASSERT(rcc);
    red_channel_client_init_send_data(rcc, SPICE_MSG_SET_ACK, NULL);
    ack.generation = ++rcc->ack_data.generation;
    ack.window = rcc->ack_data.client_window;
    rcc->ack_data.messages_window = 0;

    spice_marshall_msg_set_ack(rcc->send_data.marshaller, &ack);

    red_channel_client_begin_send_message(rcc);
}

static void red_channel_client_send_item(RedChannelClient *rcc, PipeItem *item)
{
    int handled = TRUE;

    ASSERT(red_channel_client_no_item_being_sent(rcc));
    red_channel_client_reset_send_data(rcc);
    switch (item->type) {
        case PIPE_ITEM_TYPE_SET_ACK:
            red_channel_client_send_set_ack(rcc);
            break;
        default:
            handled = FALSE;
    }
    if (!handled) {
        rcc->channel->channel_cbs.send_item(rcc, item);
    }
}

static void red_channel_client_release_item(RedChannelClient *rcc, PipeItem *item, int item_pushed)
{
    int handled = TRUE;

    switch (item->type) {
        case PIPE_ITEM_TYPE_SET_ACK:
            free(item);
            break;
        default:
            handled = FALSE;
    }
    if (!handled) {
        rcc->channel->channel_cbs.release_item(rcc, item, item_pushed);
    }
}

static inline void red_channel_client_release_sent_item(RedChannelClient *rcc)
{
    if (rcc->send_data.item) {
        red_channel_client_release_item(rcc,
                                        rcc->send_data.item, TRUE);
        rcc->send_data.item = NULL;
    }
}

static void red_channel_peer_on_out_msg_done(void *opaque)
{
    RedChannelClient *rcc = (RedChannelClient *)opaque;

    rcc->send_data.size = 0;
    red_channel_client_release_sent_item(rcc);
    if (rcc->send_data.blocked) {
        rcc->send_data.blocked = FALSE;
        rcc->channel->core->watch_update_mask(rcc->stream->watch,
                                         SPICE_WATCH_EVENT_READ);
    }
}

static void red_channel_client_pipe_remove(RedChannelClient *rcc, PipeItem *item)
{
    rcc->pipe_size--;
    ring_remove(&item->link);
}

static void red_channel_add_client(RedChannel *channel, RedChannelClient *rcc)
{
    ASSERT(rcc);
    ring_add(&channel->clients, &rcc->channel_link);
    channel->clients_num++;
}

RedChannelClient *red_channel_client_create(
    int size,
    RedChannel *channel,
    RedClient  *client,
    RedsStream *stream)
{
    RedChannelClient *rcc;

    ASSERT(stream && channel && size >= sizeof(RedChannelClient));
    rcc = spice_malloc0(size);
    rcc->stream = stream;
    rcc->channel = channel;
    rcc->client = client;
    rcc->ack_data.messages_window = ~0;  // blocks send message (maybe use send_data.blocked +
                                             // block flags)
    rcc->ack_data.client_generation = ~0;
    rcc->ack_data.client_window = CLIENT_ACK_WINDOW;
    rcc->send_data.marshaller = spice_marshaller_new();

    rcc->incoming.opaque = rcc;
    rcc->incoming.cb = &channel->incoming_cb;

    rcc->outgoing.opaque = rcc;
    rcc->outgoing.cb = &channel->outgoing_cb;
    rcc->outgoing.pos = 0;
    rcc->outgoing.size = 0;
    if (!channel->channel_cbs.config_socket(rcc)) {
        goto error;
    }

    ring_init(&rcc->pipe);
    rcc->pipe_size = 0;

    stream->watch = channel->core->watch_add(stream->socket,
                                           SPICE_WATCH_EVENT_READ,
                                           red_channel_client_event, rcc);
    rcc->id = channel->clients_num;
    red_channel_add_client(channel, rcc);
    red_client_add_channel(client, rcc);
    return rcc;
error:
    free(rcc);
    reds_stream_free(stream);
    return NULL;
}

RedChannel *red_channel_create(int size,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_handle_message_proc handle_message,
                               ChannelCbs *channel_cbs)
{
    RedChannel *channel;

    ASSERT(size >= sizeof(*channel));
    ASSERT(channel_cbs->config_socket && channel_cbs->disconnect && handle_message &&
           channel_cbs->alloc_recv_buf && channel_cbs->release_item);
    channel = spice_malloc0(size);
    channel->handle_acks = handle_acks;
    channel->channel_cbs.disconnect = channel_cbs->disconnect;
    channel->channel_cbs.send_item = channel_cbs->send_item;
    channel->channel_cbs.release_item = channel_cbs->release_item;
    channel->channel_cbs.hold_item = channel_cbs->hold_item;
    channel->channel_cbs.handle_migrate_flush_mark = channel_cbs->handle_migrate_flush_mark;
    channel->channel_cbs.handle_migrate_data = channel_cbs->handle_migrate_data;
    channel->channel_cbs.handle_migrate_data_get_serial = channel_cbs->handle_migrate_data_get_serial;
    channel->channel_cbs.config_socket = channel_cbs->config_socket;

    channel->core = core;
    channel->migrate = migrate;
    ring_init(&channel->clients);

    // TODO: send incoming_cb as parameters instead of duplicating?
    channel->incoming_cb.alloc_msg_buf = (alloc_msg_recv_buf_proc)channel_cbs->alloc_recv_buf;
    channel->incoming_cb.release_msg_buf = (release_msg_recv_buf_proc)channel_cbs->release_recv_buf;
    channel->incoming_cb.handle_message = (handle_message_proc)handle_message;
    channel->incoming_cb.on_error =
        (on_incoming_error_proc)red_channel_client_default_peer_on_error;
    channel->outgoing_cb.get_msg_size = red_channel_client_peer_get_out_msg_size;
    channel->outgoing_cb.prepare = red_channel_client_peer_prepare_out_msg;
    channel->outgoing_cb.on_block = red_channel_client_peer_on_out_block;
    channel->outgoing_cb.on_error =
        (on_outgoing_error_proc)red_channel_client_default_peer_on_error;
    channel->outgoing_cb.on_msg_done = red_channel_peer_on_out_msg_done;
    channel->outgoing_cb.on_output = red_channel_client_on_output;

    channel->shut = 0; // came here from inputs, perhaps can be removed? XXX
    channel->out_bytes_counter = 0;
    return channel;
}

static int do_nothing_handle_message(RedChannelClient *rcc, SpiceDataHeader *header, uint8_t *msg)
{
    return TRUE;
}

RedChannel *red_channel_create_parser(int size,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               spice_parse_channel_func_t parser,
                               channel_handle_parsed_proc handle_parsed, 
                               channel_on_incoming_error_proc incoming_error,
                               channel_on_outgoing_error_proc outgoing_error,
                               ChannelCbs *channel_cbs)
{
    RedChannel *channel = red_channel_create(size,
        core, migrate, handle_acks, do_nothing_handle_message,
        channel_cbs);

    if (channel == NULL) {
        return NULL;
    }
    channel->incoming_cb.handle_parsed = (handle_parsed_proc)handle_parsed;
    channel->incoming_cb.parser = parser;
    channel->incoming_cb.on_error = (on_incoming_error_proc)red_channel_peer_on_incoming_error;
    channel->outgoing_cb.on_error = (on_outgoing_error_proc)red_channel_peer_on_outgoing_error;
    channel->on_incoming_error = incoming_error;
    channel->on_outgoing_error = outgoing_error;
    return channel;
}

void red_channel_client_destroy(RedChannelClient *rcc)
{
    red_channel_client_disconnect(rcc);
    spice_marshaller_destroy(rcc->send_data.marshaller);
    free(rcc);
}

void red_channel_destroy(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;

    if (!channel) {
        return;
    }
    red_channel_pipes_clear(channel);
    RING_FOREACH_SAFE(link, next, &channel->clients) {
        red_channel_client_destroy(
            SPICE_CONTAINEROF(link, RedChannelClient, channel_link));
    }
    free(channel);
}

void red_channel_client_shutdown(RedChannelClient *rcc)
{
    if (rcc->stream && !rcc->stream->shutdown) {
        rcc->channel->core->watch_remove(rcc->stream->watch);
        rcc->stream->watch = NULL;
        shutdown(rcc->stream->socket, SHUT_RDWR);
        rcc->stream->shutdown = TRUE;
        rcc->incoming.shut = TRUE;
    }
    red_channel_client_release_sent_item(rcc);
}

void red_channel_shutdown(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;

    red_printf("%d", channel->clients_num);
    RING_FOREACH_SAFE(link, next, &channel->clients) {
        red_channel_client_shutdown(SPICE_CONTAINEROF(link, RedChannelClient, channel_link));
    }
    red_channel_pipes_clear(channel);
}

void red_channel_client_send(RedChannelClient *rcc)
{
    red_peer_handle_outgoing(rcc->stream, &rcc->outgoing);
}

void red_channel_send(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;

    RING_FOREACH_SAFE(link, next, &channel->clients) {
        red_channel_client_send(SPICE_CONTAINEROF(link, RedChannelClient, channel_link));
    }
}

static inline int red_channel_client_waiting_for_ack(RedChannelClient *rcc)
{
    return (rcc->channel->handle_acks &&
            (rcc->ack_data.messages_window > rcc->ack_data.client_window * 2));
}

static inline PipeItem *red_channel_client_pipe_item_get(RedChannelClient *rcc)
{
    PipeItem *item;

    if (!rcc || rcc->send_data.blocked
             || red_channel_client_waiting_for_ack(rcc)
             || !(item = (PipeItem *)ring_get_tail(&rcc->pipe))) {
        return NULL;
    }
    red_channel_client_pipe_remove(rcc, item);
    return item;
}

void red_channel_client_push(RedChannelClient *rcc)
{
    PipeItem *pipe_item;

    if (!rcc->during_send) {
        rcc->during_send = TRUE;
    } else {
        return;
    }

    if (rcc->send_data.blocked) {
        red_channel_client_send(rcc);
    }

    if (!red_channel_client_no_item_being_sent(rcc) && !rcc->send_data.blocked) {
        rcc->send_data.blocked = TRUE;
        red_printf("ERROR: an item waiting to be sent and not blocked");
    }

    while ((pipe_item = red_channel_client_pipe_item_get(rcc))) {
        red_channel_client_send_item(rcc, pipe_item);
    }
    rcc->during_send = FALSE;
}

void red_channel_push(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;
    RedChannelClient *rcc;

    if (!channel) {
        return;
    }
    RING_FOREACH_SAFE(link, next, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        if (rcc->stream == NULL) {
            rcc->channel->channel_cbs.disconnect(rcc);
        } else {
            red_channel_client_push(rcc);
        }
    }
}

static void red_channel_client_init_outgoing_messages_window(RedChannelClient *rcc)
{
    rcc->ack_data.messages_window = 0;
    red_channel_client_push(rcc);
}

// TODO: this function doesn't make sense because the window should be client (WAN/LAN)
// specific
void red_channel_init_outgoing_messages_window(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;

    RING_FOREACH_SAFE(link, next, &channel->clients) {
        red_channel_client_init_outgoing_messages_window(
            SPICE_CONTAINEROF(link, RedChannelClient, channel_link));
    }
}

static void red_channel_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    if (rcc->channel->channel_cbs.handle_migrate_flush_mark) {
        rcc->channel->channel_cbs.handle_migrate_flush_mark(rcc);
    }
}

// TODO: the whole migration is broken with multiple clients. What do we want to do?
// basically just
//  1) source send mark to all
//  2) source gets at various times the data (waits for all)
//  3) source migrates to target
//  4) target sends data to all
// So need to make all the handlers work with per channel/client data (what data exactly?)
static void red_channel_handle_migrate_data(RedChannelClient *rcc, uint32_t size, void *message)
{
    if (!rcc->channel->channel_cbs.handle_migrate_data) {
        return;
    }
    ASSERT(red_channel_client_get_message_serial(rcc) == 0);
    red_channel_client_set_message_serial(rcc,
        rcc->channel->channel_cbs.handle_migrate_data_get_serial(rcc, size, message));
    rcc->channel->channel_cbs.handle_migrate_data(rcc, size, message);
}

int red_channel_client_handle_message(RedChannelClient *rcc, uint32_t size,
                               uint16_t type, void *message)
{
    switch (type) {
    case SPICE_MSGC_ACK_SYNC:
        if (size != sizeof(uint32_t)) {
            red_printf("bad message size");
            return FALSE;
        }
        rcc->ack_data.client_generation = *(uint32_t *)(message);
        break;
    case SPICE_MSGC_ACK:
        if (rcc->ack_data.client_generation == rcc->ack_data.generation) {
            rcc->ack_data.messages_window -= rcc->ack_data.client_window;
            red_channel_client_push(rcc);
        }
        break;
    case SPICE_MSGC_DISCONNECTING:
        break;
    case SPICE_MSGC_MIGRATE_FLUSH_MARK:
        red_channel_handle_migrate_flush_mark(rcc);
        break;
    case SPICE_MSGC_MIGRATE_DATA:
        red_channel_handle_migrate_data(rcc, size, message);
        break;
    default:
        red_printf("invalid message type %u", type);
        return FALSE;
    }
    return TRUE;
}

static void red_channel_client_event(int fd, int event, void *data)
{
    RedChannelClient *rcc = (RedChannelClient *)data;

    if (event & SPICE_WATCH_EVENT_READ) {
        red_channel_client_receive(rcc);
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        red_channel_client_push(rcc);
    }
}

void red_channel_client_init_send_data(RedChannelClient *rcc, uint16_t msg_type, PipeItem *item)
{
    ASSERT(red_channel_client_no_item_being_sent(rcc));
    ASSERT(msg_type != 0);
    rcc->send_data.header->type = msg_type;
    rcc->send_data.item = item;
    if (item) {
        rcc->channel->channel_cbs.hold_item(rcc, item);
    }
}

void red_channel_client_begin_send_message(RedChannelClient *rcc)
{
    SpiceMarshaller *m = rcc->send_data.marshaller;

    // TODO - better check: type in channel_allowed_types. Better: type in channel_allowed_types(channel_state)
    if (rcc->send_data.header->type == 0) {
        red_printf("BUG: header->type == 0");
        return;
    }
    spice_marshaller_flush(m);
    rcc->send_data.size = spice_marshaller_get_total_size(m);
    rcc->send_data.header->size = rcc->send_data.size - sizeof(SpiceDataHeader);
    rcc->ack_data.messages_window++;
    rcc->send_data.header = NULL; /* avoid writing to this until we have a new message */
    red_channel_client_send(rcc);
}

uint64_t red_channel_client_get_message_serial(RedChannelClient *rcc)
{
    return rcc->send_data.serial;
}

void red_channel_client_set_message_serial(RedChannelClient *rcc, uint64_t serial)
{
    rcc->send_data.serial = serial;
}

void red_channel_pipe_item_init(RedChannel *channel, PipeItem *item, int type)
{
    ring_item_init(&item->link);
    item->type = type;
}

void red_channel_client_pipe_add(RedChannelClient *rcc, PipeItem *item)
{
    ASSERT(rcc && item);
    rcc->pipe_size++;
    ring_add(&rcc->pipe, &item->link);
}

void red_channel_client_pipe_add_push(RedChannelClient *rcc, PipeItem *item)
{
    red_channel_client_pipe_add(rcc, item);
    red_channel_client_push(rcc);
}

void red_channel_client_pipe_add_after(RedChannelClient *rcc,
                                       PipeItem *item, PipeItem *pos)
{
    ASSERT(rcc);
    ASSERT(pos);
    ASSERT(item);

    rcc->pipe_size++;
    ring_add_after(&item->link, &pos->link);
}

int red_channel_client_pipe_item_is_linked(RedChannelClient *rcc,
                                           PipeItem *item)
{
    return ring_item_is_linked(&item->link);
}

void red_channel_client_pipe_add_tail_no_push(RedChannelClient *rcc,
                                              PipeItem *item)
{
    ASSERT(rcc);
    rcc->pipe_size++;
    ring_add_before(&item->link, &rcc->pipe);
}

void red_channel_client_pipe_add_tail(RedChannelClient *rcc, PipeItem *item)
{
    ASSERT(rcc);
    rcc->pipe_size++;
    ring_add_before(&item->link, &rcc->pipe);
    red_channel_client_push(rcc);
}

void red_channel_client_pipe_add_type(RedChannelClient *rcc, int pipe_item_type)
{
    PipeItem *item = spice_new(PipeItem, 1);

    red_channel_pipe_item_init(rcc->channel, item, pipe_item_type);
    red_channel_client_pipe_add(rcc, item);
    red_channel_client_push(rcc);
}

void red_channel_pipes_add_type(RedChannel *channel, int pipe_item_type)
{
    RingItem *link;

    RING_FOREACH(link, &channel->clients) {
        red_channel_client_pipe_add_type(
            SPICE_CONTAINEROF(link, RedChannelClient, channel_link),
            pipe_item_type);
    }
}

int red_channel_client_is_connected(RedChannelClient *rcc)
{
    return rcc->stream != NULL;
}

void red_channel_pipe_item_remove(RedChannel *channel, PipeItem *item)
{
    ring_remove(&item->link);
}

int red_channel_is_connected(RedChannel *channel)
{
    RingItem *link;

    if (!channel || channel->clients_num == 0) {
        return FALSE;
    }
    RING_FOREACH(link, &channel->clients) {
        if (red_channel_client_is_connected(
                SPICE_CONTAINEROF(link, RedChannelClient, channel_link))) {
            return TRUE;
        }
    }
    return FALSE;
}

void red_channel_client_clear_sent_item(RedChannelClient *rcc)
{
    if (rcc->send_data.item) {
        red_channel_client_release_item(rcc, rcc->send_data.item, TRUE);
        rcc->send_data.item = NULL;
    }
    rcc->send_data.blocked = FALSE;
    rcc->send_data.size = 0;
}

void red_channel_client_pipe_clear(RedChannelClient *rcc)
{
    PipeItem *item;

    if (rcc) {
        red_channel_client_clear_sent_item(rcc);
    }
    while ((item = (PipeItem *)ring_get_head(&rcc->pipe))) {
        ring_remove(&item->link);
        red_channel_client_release_item(rcc, item, FALSE);
    }
    rcc->pipe_size = 0;
}

void red_channel_pipes_clear(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;
    RedChannelClient *rcc;

    if (!channel) {
        return;
    }
    RING_FOREACH_SAFE(link, next, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        red_channel_client_pipe_clear(rcc);
    }
}

void red_channel_client_ack_zero_messages_window(RedChannelClient *rcc)
{
    rcc->ack_data.messages_window = 0;
}

void red_channel_client_ack_set_client_window(RedChannelClient *rcc, int client_window)
{
    rcc->ack_data.client_window = client_window;
}

static void red_channel_client_remove(RedChannelClient *rcc)
{
    ring_remove(&rcc->client_link);
    rcc->client->channels_num--;
    ring_remove(&rcc->channel_link);
    rcc->channel->clients_num--;
}

void red_channel_client_disconnect(RedChannelClient *rcc)
{
    red_printf("%p (channel %p)", rcc, rcc->channel);

    if (rcc->send_data.item) {
        rcc->channel->channel_cbs.release_item(rcc, rcc->send_data.item, FALSE);
    }
    red_channel_client_pipe_clear(rcc);
    reds_stream_free(rcc->stream);
    rcc->send_data.item = NULL;
    rcc->send_data.blocked = FALSE;
    rcc->send_data.size = 0;
    red_channel_client_remove(rcc);
}

void red_channel_disconnect(RedChannel *channel)
{
    RingItem *link;
    RingItem *next;

    red_channel_pipes_clear(channel);
    RING_FOREACH_SAFE(link, next, &channel->clients) {
        red_channel_client_disconnect(
            SPICE_CONTAINEROF(link, RedChannelClient, channel_link));
    }
}

int red_channel_all_clients_serials_are_zero(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        if (rcc->send_data.serial != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

void red_channel_apply_clients(RedChannel *channel, channel_client_callback cb)
{
    RingItem *link;
    RingItem *next;
    RedChannelClient *rcc;

    RING_FOREACH_SAFE(link, next, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        cb(rcc);
    }
}

void red_channel_apply_clients_data(RedChannel *channel, channel_client_callback_data cb, void *data)
{
    RingItem *link;
    RingItem *next;
    RedChannelClient *rcc;

    RING_FOREACH_SAFE(link, next, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        cb(rcc, data);
    }
}

void red_channel_set_shut(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        rcc->incoming.shut = TRUE;
    }
}

int red_channel_all_blocked(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;

    if (!channel || channel->clients_num == 0) {
        return FALSE;
    }
    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        if (!rcc->send_data.blocked) {
            return FALSE;
        }
    }
    return TRUE;
}

int red_channel_any_blocked(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        if (rcc->send_data.blocked) {
            return TRUE;
        }
    }
    return FALSE;
}

int red_channel_client_blocked(RedChannelClient *rcc)
{
    return rcc && rcc->send_data.blocked;
}

int red_channel_client_send_message_pending(RedChannelClient *rcc)
{
    return rcc->send_data.header->type != 0;
}

/* accessors for RedChannelClient */
SpiceMarshaller *red_channel_client_get_marshaller(RedChannelClient *rcc)
{
    return rcc->send_data.marshaller;
}

RedsStream *red_channel_client_get_stream(RedChannelClient *rcc)
{
    return rcc->stream;
}

RedClient *red_channel_client_get_client(RedChannelClient *rcc)
{
    return rcc->client;
}

SpiceDataHeader *red_channel_client_get_header(RedChannelClient *rcc)
{
    return rcc->send_data.header;
}
/* end of accessors */

int red_channel_get_first_socket(RedChannel *channel)
{
    if (!channel || channel->clients_num == 0) {
        return -1;
    }
    return SPICE_CONTAINEROF(ring_get_head(&channel->clients),
                             RedChannelClient, channel_link)->stream->socket;
}

int red_channel_client_item_being_sent(RedChannelClient *rcc, PipeItem *item)
{
    return rcc->send_data.item == item;
}

int red_channel_item_being_sent(RedChannel *channel, PipeItem *item)
{
    RingItem *link;
    RedChannelClient *rcc;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        if (rcc->send_data.item == item) {
            return TRUE;
        }
    }
    return FALSE;
}

int red_channel_no_item_being_sent(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        if (!red_channel_client_no_item_being_sent(rcc)) {
            return FALSE;
        }
    }
    return TRUE;
}

int red_channel_client_no_item_being_sent(RedChannelClient *rcc)
{
    return !rcc || (rcc->send_data.size == 0);
}

void red_channel_client_pipe_remove_and_release(RedChannelClient *rcc,
                                                PipeItem *item)
{
    red_channel_client_pipe_remove(rcc, item);
    red_channel_client_release_item(rcc, item, FALSE);
}

/*
 * RedClient implementation - kept in red_channel.c because they are
 * pretty tied together.
 */

RedClient *red_client_new()
{
    RedClient *client;

    client = spice_malloc0(sizeof(RedClient));
    ring_init(&client->channels);
    return client;
}

void red_client_shutdown(RedClient *client)
{
    RingItem *link, *next;

    red_printf("#channels %d", client->channels_num);
    RING_FOREACH_SAFE(link, next, &client->channels) {
        red_channel_client_shutdown(SPICE_CONTAINEROF(link, RedChannelClient, client_link));
    }
}

void red_client_destroy(RedClient *client)
{
    RingItem *link, *next;
    RedChannelClient *rcc;

    red_printf("destroy client with #channels %d", client->channels_num);
    RING_FOREACH_SAFE(link, next, &client->channels) {
        // some channels may be in other threads, so disconnection
        // is not synchronous.
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, client_link);
        rcc->channel->channel_cbs.disconnect(rcc); // this may call another thread. it also frees. (eventually - doesn't have to be in sync)
    }
    free(client);
}

void red_client_disconnect(RedClient *client)
{
    RingItem *link, *next;
    RedChannelClient *rcc;

    red_printf("#channels %d", client->channels_num);
    RING_FOREACH_SAFE(link, next, &client->channels) {
        // some channels may be in other threads, so disconnection
        // is not synchronous.
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, client_link);
        rcc->channel->channel_cbs.disconnect(rcc);
    }
}

static void red_client_add_channel(RedClient *client, RedChannelClient *rcc)
{
    ASSERT(rcc && client);
    ring_add(&client->channels, &rcc->client_link);
    client->channels_num++;
}

MainChannelClient *red_client_get_main(RedClient *client) {
    return client->mcc;
}

void red_client_set_main(RedClient *client, MainChannelClient *mcc) {
    client->mcc = mcc;
}

/*
 * Functions to push the same item to multiple pipes.
 */

/*
 * TODO: after convinced of correctness, add paths for single client
 * that avoid the whole loop. perhaps even have a function pointer table
 * later.
 * TODO - inline? macro? right now this is the simplest from code amount
 */

typedef void (*rcc_item_t)(RedChannelClient *rcc, PipeItem *item);
typedef int (*rcc_item_cond_t)(RedChannelClient *rcc, PipeItem *item);

static void red_channel_pipes_create_batch(RedChannel *channel,
                                new_pipe_item_t creator, void *data,
                                rcc_item_t callback)
{
    RingItem *link;
    RedChannelClient *rcc;
    PipeItem *item;
    int num = 0;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        item = (*creator)(rcc, data, num++);
        if (callback) {
            (*callback)(rcc, item);
        }
    }
}

void red_channel_pipes_new_add_push(RedChannel *channel,
                              new_pipe_item_t creator, void *data)
{
    red_channel_pipes_create_batch(channel, creator, data,
                                     red_channel_client_pipe_add);
    red_channel_push(channel);
}

void red_channel_pipes_new_add(RedChannel *channel, new_pipe_item_t creator, void *data)
{
    red_channel_pipes_create_batch(channel, creator, data,
                                     red_channel_client_pipe_add);
}

void red_channel_pipes_new_add_tail(RedChannel *channel, new_pipe_item_t creator, void *data)
{
    red_channel_pipes_create_batch(channel, creator, data,
                                     red_channel_client_pipe_add_tail_no_push);
}

uint32_t red_channel_max_pipe_size(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;
    uint32_t pipe_size = 0;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        pipe_size = pipe_size > rcc->pipe_size ? pipe_size : rcc->pipe_size;
    }
    return pipe_size;
}

uint32_t red_channel_min_pipe_size(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;
    uint32_t pipe_size = ~0;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        pipe_size = pipe_size < rcc->pipe_size ? pipe_size : rcc->pipe_size;
    }
    return pipe_size == ~0 ? 0 : pipe_size;
}

uint32_t red_channel_sum_pipes_size(RedChannel *channel)
{
    RingItem *link;
    RedChannelClient *rcc;
    uint32_t sum = 0;

    RING_FOREACH(link, &channel->clients) {
        rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);
        sum += rcc->pipe_size;
    }
    return sum;
}

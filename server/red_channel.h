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

#ifndef _H_RED_CHANNEL
#define _H_RED_CHANNEL

#include "red_common.h"
#include "reds.h"
#include "spice.h"
#include "ring.h"
#include "server/demarshallers.h"

#define MAX_SEND_BUFS 1000
#define MAX_SEND_VEC 100
#define CLIENT_ACK_WINDOW 20

/* Basic interface for channels, without using the RedChannel interface.
   The intention is to move towards one channel interface gradually.
   At the final stage, this interface shouldn't be exposed. Only RedChannel will use it. */

typedef int (*handle_message_proc)(void *opaque,
                                   SpiceDataHeader *header, uint8_t *msg);
typedef int (*handle_parsed_proc)(void *opaque, uint32_t size, uint16_t type, void *message);
typedef uint8_t *(*alloc_msg_recv_buf_proc)(void *opaque, SpiceDataHeader *msg_header);
typedef void (*release_msg_recv_buf_proc)(void *opaque,
                                          SpiceDataHeader *msg_header, uint8_t *msg);
typedef void (*on_incoming_error_proc)(void *opaque);

typedef struct IncomingHandler {
    void *opaque;
    SpiceDataHeader header;
    uint32_t header_pos;
    uint8_t *msg; // data of the msg following the header. allocated by alloc_msg_buf.
    uint32_t msg_pos;
    handle_message_proc handle_message;
    alloc_msg_recv_buf_proc alloc_msg_buf;
    on_incoming_error_proc on_error; // recv error or handle_message error
    release_msg_recv_buf_proc release_msg_buf; // for errors
    // The following is an optional alternative to handle_message, used if not null
    spice_parse_channel_func_t parser;
    handle_parsed_proc handle_parsed;
    int shut; // came here from inputs_channel. Not sure if it is really required or can be removed. XXX
} IncomingHandler;

typedef int (*get_outgoing_msg_size_proc)(void *opaque);
typedef void (*prepare_outgoing_proc)(void *opaque, struct iovec *vec, int *vec_size, int pos);
typedef void (*on_outgoing_error_proc)(void *opaque);
typedef void (*on_outgoing_block_proc)(void *opaque);
typedef void (*on_outgoing_msg_done_proc)(void *opaque);
typedef struct OutgoingHandler {
    void *opaque;
    struct iovec vec_buf[MAX_SEND_VEC];
    int vec_size;
    struct iovec *vec;
    int pos;
    int size;
    get_outgoing_msg_size_proc get_msg_size;
    prepare_outgoing_proc prepare;
    on_outgoing_error_proc on_error;
    on_outgoing_block_proc on_block;
    on_outgoing_msg_done_proc on_msg_done;
#ifdef RED_STATISTICS
    uint64_t *out_bytes_counter;
#endif
} OutgoingHandler;

/* Red Channel interface */

typedef struct BufDescriptor {
    uint32_t size;
    uint8_t *data;
} BufDescriptor;

/* Messages handled by red_channel
 * SET_ACK - sent to client on channel connection
 * Note that the numbers don't have to correspond to spice message types,
 * but we keep the 100 first allocated for base channel approach.
 * */
enum {
    PIPE_ITEM_TYPE_SET_ACK=1,
    PIPE_ITEM_TYPE_CHANNEL_BASE=101,
};

typedef struct PipeItem {
    RingItem link;
    int type;
} PipeItem;

typedef struct RedChannel RedChannel;

typedef uint8_t *(*channel_alloc_msg_recv_buf_proc)(RedChannel *channel,
                                                    SpiceDataHeader *msg_header);
typedef int (*channel_handle_parsed_proc)(RedChannel *channel, uint32_t size, uint16_t type,
                                        void *message);
typedef int (*channel_handle_message_proc)(RedChannel *channel,
                                           SpiceDataHeader *header, uint8_t *msg);
typedef void (*channel_release_msg_recv_buf_proc)(RedChannel *channel,
                                                  SpiceDataHeader *msg_header, uint8_t *msg);
typedef void (*channel_disconnect_proc)(RedChannel *channel);
typedef int (*channel_configure_socket_proc)(RedChannel *channel);
typedef void (*channel_send_pipe_item_proc)(RedChannel *channel, PipeItem *item);
typedef void (*channel_hold_pipe_item_proc)(RedChannel *channel, PipeItem *item);
typedef void (*channel_release_pipe_item_proc)(RedChannel *channel,
                                               PipeItem *item, int item_pushed);
typedef void (*channel_on_incoming_error_proc)(RedChannel *channel);
typedef void (*channel_on_outgoing_error_proc)(RedChannel *channel);

typedef int (*channel_handle_migrate_flush_mark)(RedChannel *channel);
typedef uint64_t (*channel_handle_migrate_data)(RedChannel *channel,
                                                uint32_t size, void *message);
typedef uint64_t (*channel_handle_migrate_data_get_serial)(RedChannel *channel,
                                            uint32_t size, void *message);

struct RedChannel {
    RedsStream *stream;
    SpiceCoreInterface *core;
    int migrate;
    int handle_acks;

    struct {
        uint32_t generation;
        uint32_t client_generation;
        uint32_t messages_window;
        uint32_t client_window;
    } ack_data;

    Ring pipe;
    uint32_t pipe_size;

    struct {
        SpiceMarshaller *marshaller;
        SpiceDataHeader *header;
        union {
            SpiceMsgSetAck ack;
            SpiceMsgMigrate migrate;
        } u;
        uint32_t size;
        PipeItem *item;
        int blocked;
        uint64_t serial;
    } send_data;

    OutgoingHandler outgoing;
    IncomingHandler incoming;

    channel_disconnect_proc disconnect;
    channel_send_pipe_item_proc send_item;
    channel_hold_pipe_item_proc hold_item;
    channel_release_pipe_item_proc release_item;

    int during_send;
    /* Stuff below added for Main and Inputs channels switch to RedChannel
     * (might be removed later) */
    channel_on_incoming_error_proc on_incoming_error; /* alternative to disconnect */
    channel_on_outgoing_error_proc on_outgoing_error;
    int shut; /* signal channel is to be closed */

    channel_handle_migrate_flush_mark handle_migrate_flush_mark;
    channel_handle_migrate_data handle_migrate_data;
    channel_handle_migrate_data_get_serial handle_migrate_data_get_serial;
};

/* if one of the callbacks should cause disconnect, use red_channel_shutdown and don't
   explicitly destroy the channel */
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
                               channel_handle_migrate_data_get_serial handle_migrate_data_get_serial);

/* alternative constructor, meant for marshaller based (inputs,main) channels,
 * will become default eventually */
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
                               channel_handle_migrate_data_get_serial handle_migrate_data_get_serial);

int red_channel_is_connected(RedChannel *channel);

void red_channel_destroy(RedChannel *channel);

void red_channel_shutdown(RedChannel *channel);
/* should be called when a new channel is ready to send messages */
void red_channel_init_outgoing_messages_window(RedChannel *channel);

/* handles general channel msgs from the client */
int red_channel_handle_message(RedChannel *channel, uint32_t size,
                               uint16_t type, void *message);

/* default error handler that disconnects channel */
void red_channel_default_peer_on_error(RedChannel *channel);

/* when preparing send_data: should call init and then add_buf per buffer that is
   being sent */
void red_channel_init_send_data(RedChannel *channel, uint16_t msg_type, PipeItem *item);
void red_channel_add_buf(RedChannel *channel, void *data, uint32_t size);

uint64_t red_channel_get_message_serial(RedChannel *channel);
void red_channel_set_message_serial(RedChannel *channel, uint64_t);

/* when sending a msg. should first call red_channel_begin_send_message */
void red_channel_begin_send_message(RedChannel *channel);

void red_channel_pipe_item_init(RedChannel *channel, PipeItem *item, int type);
void red_channel_pipe_add_push(RedChannel *channel, PipeItem *item);
void red_channel_pipe_add(RedChannel *channel, PipeItem *item);
void red_channel_pipe_add_after(RedChannel *channel, PipeItem *item, PipeItem *pos);
int red_channel_pipe_item_is_linked(RedChannel *channel, PipeItem *item);
void red_channel_pipe_item_remove(RedChannel *channel, PipeItem *item);
void red_channel_pipe_add_tail(RedChannel *channel, PipeItem *item);
/* for types that use this routine -> the pipe item should be freed */
void red_channel_pipe_add_type(RedChannel *channel, int pipe_item_type);

void red_channel_ack_zero_messages_window(RedChannel *channel);
void red_channel_ack_set_client_window(RedChannel *channel, int client_window);
void red_channel_push_set_ack(RedChannel *channel);

// TODO: unstaticed for display/cursor channels. they do some specific pushes not through
// adding elements or on events. but not sure if this is actually required (only result
// should be that they ""try"" a little harder, but if the event system is correct it
// should not make any difference.
void red_channel_push(RedChannel *channel);
// TODO: again - what is the context exactly? this happens in channel disconnect. but our
// current red_channel_shutdown also closes the socket - is there a socket to close?
// are we reading from an fd here? arghh
void red_channel_pipe_clear(RedChannel *channel);
// Again, used in various places outside of event handler context (or in other event handler
// contexts):
//  flush_display_commands/flush_cursor_commands
//  display_channel_wait_for_init
//  red_wait_outgoing_item
//  red_wait_pipe_item_sent
//  handle_channel_events - this is the only one that was used before, and was in red_channel.c
void red_channel_receive(RedChannel *channel);
void red_channel_send(RedChannel *channel);
SpiceMarshaller *red_channel_get_marshaller(RedChannel *channel);

#endif

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

typedef struct IncomingHandlerInterface {
    handle_message_proc handle_message;
    alloc_msg_recv_buf_proc alloc_msg_buf;
    on_incoming_error_proc on_error; // recv error or handle_message error
    release_msg_recv_buf_proc release_msg_buf; // for errors
    // The following is an optional alternative to handle_message, used if not null
    spice_parse_channel_func_t parser;
    handle_parsed_proc handle_parsed;
} IncomingHandlerInterface;

typedef struct IncomingHandler {
    IncomingHandlerInterface *cb;
    void *opaque;
    SpiceDataHeader header;
    uint32_t header_pos;
    uint8_t *msg; // data of the msg following the header. allocated by alloc_msg_buf.
    uint32_t msg_pos;
    int shut; // came here from inputs_channel. Not sure if it is really required or can be removed. XXX
} IncomingHandler;

typedef int (*get_outgoing_msg_size_proc)(void *opaque);
typedef void (*prepare_outgoing_proc)(void *opaque, struct iovec *vec, int *vec_size, int pos);
typedef void (*on_outgoing_error_proc)(void *opaque);
typedef void (*on_outgoing_block_proc)(void *opaque);
typedef void (*on_outgoing_msg_done_proc)(void *opaque);
typedef void (*on_output_proc)(void *opaque, int n);

typedef struct OutgoingHandlerInterface {
    get_outgoing_msg_size_proc get_msg_size;
    prepare_outgoing_proc prepare;
    on_outgoing_error_proc on_error;
    on_outgoing_block_proc on_block;
    on_outgoing_msg_done_proc on_msg_done;
    on_output_proc on_output;
} OutgoingHandlerInterface;

typedef struct OutgoingHandler {
    OutgoingHandlerInterface *cb;
    void *opaque;
    struct iovec vec_buf[MAX_SEND_VEC];
    int vec_size;
    struct iovec *vec;
    int pos;
    int size;
} OutgoingHandler;

/* Red Channel interface */

typedef struct BufDescriptor {
    uint32_t size;
    uint8_t *data;
} BufDescriptor;

typedef struct RedChannel RedChannel;
typedef struct RedChannelClient RedChannelClient;

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

typedef uint8_t *(*channel_alloc_msg_recv_buf_proc)(RedChannelClient *channel,
                                                    SpiceDataHeader *msg_header);
typedef int (*channel_handle_parsed_proc)(RedChannelClient *rcc, uint32_t size, uint16_t type,
                                        void *message);
typedef int (*channel_handle_message_proc)(RedChannelClient *rcc,
                                           SpiceDataHeader *header, uint8_t *msg);
typedef void (*channel_release_msg_recv_buf_proc)(RedChannelClient *channel,
                                                  SpiceDataHeader *msg_header, uint8_t *msg);
typedef void (*channel_disconnect_proc)(RedChannelClient *rcc);
typedef int (*channel_configure_socket_proc)(RedChannelClient *rcc);
typedef void (*channel_send_pipe_item_proc)(RedChannelClient *rcc, PipeItem *item);
typedef void (*channel_hold_pipe_item_proc)(RedChannelClient *rcc, PipeItem *item);
typedef void (*channel_release_pipe_item_proc)(RedChannelClient *rcc,
                                               PipeItem *item, int item_pushed);
typedef void (*channel_on_incoming_error_proc)(RedChannelClient *rcc);
typedef void (*channel_on_outgoing_error_proc)(RedChannelClient *rcc);

typedef int (*channel_handle_migrate_flush_mark_proc)(RedChannelClient *base);
typedef uint64_t (*channel_handle_migrate_data_proc)(RedChannelClient *base,
                                                uint32_t size, void *message);
typedef uint64_t (*channel_handle_migrate_data_get_serial_proc)(RedChannelClient *base,
                                            uint32_t size, void *message);

struct RedChannelClient {
    RingItem channel_link;
    RingItem client_link;
    RedChannel *channel;
    RedClient  *client;
    RedsStream *stream;
    struct {
        uint32_t generation;
        uint32_t client_generation;
        uint32_t messages_window;
        uint32_t client_window;
    } ack_data;

    struct {
        SpiceMarshaller *marshaller;
        SpiceDataHeader *header;
        uint32_t size;
        PipeItem *item;
        int blocked;
        uint64_t serial;
    } send_data;

    OutgoingHandler outgoing;
    IncomingHandler incoming;
    int during_send;
    int id; // debugging purposes
    Ring pipe;
    uint32_t pipe_size;
};

struct RedChannel {
    SpiceCoreInterface *core;
    int migrate;
    int handle_acks;

    RedChannelClient *rcc;
    uint32_t clients_num;

    OutgoingHandlerInterface outgoing_cb;
    IncomingHandlerInterface incoming_cb;

    channel_configure_socket_proc config_socket;
    channel_disconnect_proc disconnect;
    channel_send_pipe_item_proc send_item;
    channel_hold_pipe_item_proc hold_item;
    channel_release_pipe_item_proc release_item;

    /* Stuff below added for Main and Inputs channels switch to RedChannel
     * (might be removed later) */
    channel_on_incoming_error_proc on_incoming_error; /* alternative to disconnect */
    channel_on_outgoing_error_proc on_outgoing_error;
    int shut; /* signal channel is to be closed */

    channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark;
    channel_handle_migrate_data_proc handle_migrate_data;
    channel_handle_migrate_data_get_serial_proc handle_migrate_data_get_serial;
#ifdef RED_STATISTICS
    uint64_t *out_bytes_counter;
#endif
};

/* if one of the callbacks should cause disconnect, use red_channel_shutdown and don't
   explicitly destroy the channel */
RedChannel *red_channel_create(int size,
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
                               channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark,
                               channel_handle_migrate_data_proc handle_migrate_data,
                               channel_handle_migrate_data_get_serial_proc handle_migrate_data_get_serial);

/* alternative constructor, meant for marshaller based (inputs,main) channels,
 * will become default eventually */
RedChannel *red_channel_create_parser(int size,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_configure_socket_proc config_socket,
                               channel_disconnect_proc disconnect,
                               spice_parse_channel_func_t parser,
                               channel_handle_parsed_proc handle_parsed,
                               channel_alloc_msg_recv_buf_proc alloc_recv_buf,
                               channel_release_msg_recv_buf_proc release_recv_buf,
                               channel_hold_pipe_item_proc hold_item,
                               channel_send_pipe_item_proc send_item,
                               channel_release_pipe_item_proc release_item,
                               channel_on_incoming_error_proc incoming_error,
                               channel_on_outgoing_error_proc outgoing_error,
                               channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark,
                               channel_handle_migrate_data_proc handle_migrate_data,
                               channel_handle_migrate_data_get_serial_proc handle_migrate_data_get_serial);
RedChannelClient *red_channel_client_create(int size, RedChannel *channel, RedClient *client,
                                            RedsStream *stream);
int red_channel_is_connected(RedChannel *channel);

void red_channel_client_destroy(RedChannelClient *rcc);
void red_channel_destroy(RedChannel *channel);

/* shutdown is the only safe thing to do out of the client/channel
 * thread. It will not touch the rings, just shutdown the socket.
 * It should be followed by some way to gurantee a disconnection. */
void red_channel_client_shutdown(RedChannelClient *rcc);
void red_channel_shutdown(RedChannel *channel);

/* should be called when a new channel is ready to send messages */
void red_channel_init_outgoing_messages_window(RedChannel *channel);

/* handles general channel msgs from the client */
int red_channel_client_handle_message(RedChannelClient *rcc, uint32_t size,
                               uint16_t type, void *message);

/* default error handler that disconnects channel */
void red_channel_client_default_peer_on_error(RedChannelClient *rcc);

/* when preparing send_data: should call init and then use marshaller */
void red_channel_client_init_send_data(RedChannelClient *rcc, uint16_t msg_type, PipeItem *item);

uint64_t red_channel_client_get_message_serial(RedChannelClient *channel);
void red_channel_client_set_message_serial(RedChannelClient *channel, uint64_t);

/* when sending a msg. should first call red_channel_client_begin_send_message */
void red_channel_client_begin_send_message(RedChannelClient *rcc);

void red_channel_pipe_item_init(RedChannel *channel, PipeItem *item, int type);
void red_channel_client_pipe_add_push(RedChannelClient *rcc, PipeItem *item);
void red_channel_client_pipe_add(RedChannelClient *rcc, PipeItem *item);
void red_channel_client_pipe_add_after(RedChannelClient *rcc, PipeItem *item, PipeItem *pos);
int red_channel_pipe_item_is_linked(RedChannel *channel, PipeItem *item);
void red_channel_pipe_item_remove(RedChannel *channel, PipeItem *item);
void red_channel_client_pipe_remove_and_release(RedChannelClient *rcc, PipeItem *item);
void red_channel_client_pipe_add_tail(RedChannelClient *rcc, PipeItem *item);
/* for types that use this routine -> the pipe item should be freed */
void red_channel_client_pipe_add_type(RedChannelClient *rcc, int pipe_item_type);
void red_channel_pipes_add_type(RedChannel *channel, int pipe_item_type);

void red_channel_client_ack_zero_messages_window(RedChannelClient *rcc);
void red_channel_client_ack_set_client_window(RedChannelClient *rcc, int client_window);
void red_channel_client_push_set_ack(RedChannelClient *rcc);
void red_channel_ack_zero_messages_window(RedChannel *channel);
void red_channel_ack_set_client_window(RedChannel *channel, int client_window);
void red_channel_push_set_ack(RedChannel *channel);

/* TODO: This sets all clients to shut state - probably we want to close per channel */
void red_channel_shutdown(RedChannel *channel);

int red_channel_get_first_socket(RedChannel *channel);

/* return TRUE if all of the connected clients to this channel are blocked */
int red_channel_all_blocked(RedChannel *channel);

/* return TRUE if any of the connected clients to this channel are blocked */
int red_channel_any_blocked(RedChannel *channel);

int red_channel_client_blocked(RedChannelClient *rcc);

/* helper for channels that have complex logic that can possibly ready a send */
int red_channel_client_send_message_pending(RedChannelClient *rcc);

/* returns TRUE if item is being sent by one of the channel clients. This will
 * be true if someone called init_send_data but send has not completed (or perhaps
 * hasn't even begun, i.e. no one called begin_send_)
 * */
int red_channel_item_being_sent(RedChannel *channel, PipeItem *item);
int red_channel_client_item_being_sent(RedChannelClient *rcc, PipeItem *item);

int red_channel_no_item_being_sent(RedChannel *channel);
int red_channel_client_no_item_being_sent(RedChannelClient *rcc);

void red_channel_pipes_remove(RedChannel *channel, PipeItem *item);

// TODO: unstaticed for display/cursor channels. they do some specific pushes not through
// adding elements or on events. but not sure if this is actually required (only result
// should be that they ""try"" a little harder, but if the event system is correct it
// should not make any difference.
void red_channel_push(RedChannel *channel);
void red_channel_client_push(RedChannelClient *rcc);
// TODO: again - what is the context exactly? this happens in channel disconnect. but our
// current red_channel_shutdown also closes the socket - is there a socket to close?
// are we reading from an fd here? arghh
void red_channel_pipes_clear(RedChannel *channel);
void red_channel_client_pipe_clear(RedChannelClient *rcc);
// Again, used in various places outside of event handler context (or in other event handler
// contexts):
//  flush_display_commands/flush_cursor_commands
//  display_channel_wait_for_init
//  red_wait_outgoing_item
//  red_wait_pipe_item_sent
//  handle_channel_events - this is the only one that was used before, and was in red_channel.c
void red_channel_receive(RedChannel *channel);
void red_channel_client_receive(RedChannelClient *rcc);
// For red_worker
void red_channel_send(RedChannel *channel);
void red_channel_client_send(RedChannelClient *rcc);
// For red_worker
void red_channel_disconnect(RedChannel *channel);
void red_channel_client_disconnect(RedChannelClient *rcc);

/* accessors for RedChannelClient */
/* Note: the valid times to call red_channel_get_marshaller are just during send_item callback. */
SpiceMarshaller *red_channel_client_get_marshaller(RedChannelClient *rcc);
RedsStream *red_channel_client_get_stream(RedChannelClient *rcc);
RedClient *red_channel_client_get_client(RedChannelClient *rcc);

/* this is a convenience function for sending messages, sometimes (migration only?)
 * the serial from the header needs to be available for sending. Note that the header
 * pointer retrieved is not valid except between red_channel_reset_send_data and
 * red_channel_begin_send_message. red_channel_init_send_data changes the header (sets
 * the type in it) as a convenience function. It is preffered to do that through it and
 * not via the below accessor and direct header manipulation. */
SpiceDataHeader *red_channel_client_get_header(RedChannelClient *rcc);

/* apply given function to all connected clients */
typedef void (*channel_client_visitor)(RedChannelClient *rcc);
typedef void (*channel_client_visitor_data)(RedChannelClient *rcc, void *data);
void red_channel_apply_clients(RedChannel *channel, channel_client_visitor v);
void red_channel_apply_clients_data(RedChannel *channel, channel_client_visitor_data v, void *data);

struct RedClient {
    RingItem link;
    Ring channels;
    int channels_num;
    int disconnecting;
    MainChannelClient *mcc;
};

RedClient *red_client_new();
void red_client_destroy(RedClient *client);
void red_client_set_main(RedClient *client, MainChannelClient *mcc);
MainChannelClient *red_client_get_main(RedClient *client);

#endif

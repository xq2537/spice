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

#define MAX_SEND_BUFS 1000
#define MAX_SEND_VEC 50
#define CLIENT_ACK_WINDOW 20

/* Basic interface for channels, without using the RedChannel interface.
   The intention is to move towards one channel interface gradually.
   At the final stage, this interface shouldn't be exposed. Only RedChannel will use it. */

typedef int (*handle_message_proc)(void *opaque,
                                   SpiceDataHeader *header, uint8_t *msg);
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
} IncomingHandler;

typedef int (*get_outgoing_msg_size_proc)(void *opaque);
typedef void (*prepare_outgoing_proc)(void *opaque, struct iovec *vec, int *vec_size);
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
} OutgoingHandler;

/* Red Channel interface */

typedef struct BufDescriptor {
    uint32_t size;
    uint8_t *data;
} BufDescriptor;

typedef struct PipeItem {
    RingItem link;
    int type;
} PipeItem;

typedef struct RedChannel RedChannel;

typedef uint8_t *(*channel_alloc_msg_recv_buf_proc)(RedChannel *channel,
                                                    SpiceDataHeader *msg_header);
typedef int (*channel_handle_message_proc)(RedChannel *channel,
                                           SpiceDataHeader *header, uint8_t *msg);
typedef void (*channel_release_msg_recv_buf_proc)(RedChannel *channel,
                                                  SpiceDataHeader *msg_header, uint8_t *msg);
typedef void (*channel_disconnect_proc)(RedChannel *channel);
typedef int (*channel_configure_socket_proc)(RedChannel *channel);
typedef void (*channel_send_pipe_item_proc)(RedChannel *channel, PipeItem *item);
typedef void (*channel_release_pipe_item_proc)(RedChannel *channel,
                                               PipeItem *item, int item_pushed);

struct RedChannel {
    RedsStreamContext *peer;
    SpiceCoreInterface *core;
    int migrate;
    int handle_acks;

    struct {
        uint32_t generation;
        uint32_t client_generation;
        uint32_t messages_window;
    } ack_data;

    Ring pipe;
    uint32_t pipe_size;

    struct {
        SpiceDataHeader header;
        union {
            SpiceMsgSetAck ack;
            SpiceMsgMigrate migrate;
        } u;
        uint32_t n_bufs;
        BufDescriptor bufs[MAX_SEND_BUFS];
        uint32_t size;
        uint32_t not_sent_buf_head;

        PipeItem *item;
        int blocked;
    } send_data;

    OutgoingHandler outgoing;
    IncomingHandler incoming;

    channel_disconnect_proc disconnect;
    channel_send_pipe_item_proc send_item;
    channel_release_pipe_item_proc release_item;

    int during_send;
};

/* if one of the callbacks should cause disconnect, use red_channel_shutdown and don't
   explicitly destroy the channel */
RedChannel *red_channel_create(int size, RedsStreamContext *peer,
                               SpiceCoreInterface *core,
                               int migrate, int handle_acks,
                               channel_configure_socket_proc config_socket,
                               channel_disconnect_proc disconnect,
                               channel_handle_message_proc handle_message,
                               channel_alloc_msg_recv_buf_proc alloc_recv_buf,
                               channel_release_msg_recv_buf_proc release_recv_buf,
                               channel_send_pipe_item_proc send_item,
                               channel_release_pipe_item_proc release_item);

void red_channel_destroy(RedChannel *channel);

void red_channel_shutdown(RedChannel *channel);
/* should be called when a new channel is ready to send messages */
void red_channel_init_outgoing_messages_window(RedChannel *channel);

/* handles general channel msgs from the client */
int red_channel_handle_message(RedChannel *channel, SpiceDataHeader *header, uint8_t *msg);

/* when preparing send_data: should call reset, then init and then add_buf per buffer that is
   being sent */
void red_channel_reset_send_data(RedChannel *channel);
void red_channel_init_send_data(RedChannel *channel, uint16_t msg_type, PipeItem *item);
void red_channel_add_buf(RedChannel *channel, void *data, uint32_t size);

uint64_t red_channel_get_message_serial(RedChannel *channel);

/* when sending a msg. should first call red_channel_begin_send_message */
void red_channel_begin_send_message(RedChannel *channel);

void red_channel_pipe_item_init(RedChannel *channel, PipeItem *item, int type);
void red_channel_pipe_add(RedChannel *channel, PipeItem *item);
int red_channel_pipe_item_is_linked(RedChannel *channel, PipeItem *item);
void red_channel_pipe_item_remove(RedChannel *channel, PipeItem *item);
void red_channel_pipe_add_tail(RedChannel *channel, PipeItem *item);
/* for types that use this routine -> the pipe item should be freed */
void red_channel_pipe_add_type(RedChannel *channel, int pipe_item_type);

#endif

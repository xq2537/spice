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

#ifndef __MAIN_CHANNEL_H__
#define __MAIN_CHANNEL_H__

#include <stdint.h>
#include <spice/vd_agent.h>
#include "common/marshaller.h"

/* This is a temporary measure for reds/main split - should not be in a header,
 * but private (although only reds.c includes main_channel.h) */
struct MainMigrateData {
    uint32_t version;
    uint32_t serial;
    uint32_t ping_id;

    uint32_t agent_connected;
    uint32_t client_agent_started;
    uint32_t num_client_tokens;
    uint32_t send_tokens;

    uint32_t read_state;
    VDIChunkHeader vdi_chunk_header;
    uint32_t recive_len;
    uint32_t message_recive_len;
    uint32_t read_buf_len;

    uint32_t write_queue_size;
};

Channel *main_channel_init();
void main_channel_close(Channel *channel); // not destroy, just socket close
int main_channel_push_ping(Channel *channel, int size);
void main_channel_push_mouse_mode(Channel *channel, int current_mode, int is_client_mouse_allowed);
void main_channel_push_agent_connected(Channel *channel);
void main_channel_push_agent_disconnected(Channel *channel);
void main_channel_push_tokens(Channel *channel, uint32_t num_tokens);
void main_channel_push_agent_data(Channel *channel, uint8_t* data, size_t len,
           spice_marshaller_item_free_func free_data, void *opaque);
void main_channel_start_net_test(Channel *channel);
// TODO: huge. Consider making a reds_* interface for these functions
// and calling from main.
void main_channel_push_init(Channel *channel, int connection_id, int display_channels_hint,
    int current_mouse_mode, int is_client_mouse_allowed, int multi_media_time,
    int ram_hint);
void main_channel_push_notify(Channel *channel, uint8_t *mess, const int mess_len);
// TODO: consider exporting RedsMigSpice from reds.c
void main_channel_push_migrate_begin(Channel *channel, int port, int sport, char *host,
    uint16_t cert_pub_key_type, uint32_t cert_pub_key_len, uint8_t *cert_pub_key);
void main_channel_push_migrate(Channel *channel);
void main_channel_push_migrate_cancel(Channel *channel);
void main_channel_push_multi_media_time(Channel *channel, int time);
int main_channel_getsockname(Channel *channel, struct sockaddr *sa, socklen_t *salen);
int main_channel_getpeername(Channel *channel, struct sockaddr *sa, socklen_t *salen);

// TODO: Defines used to calculate receive buffer size, and also by reds.c
// other options: is to make a reds_main_consts.h, to duplicate defines.
#define REDS_AGENT_WINDOW_SIZE 10
#define REDS_NUM_INTERNAL_AGENT_MESSAGES 1

#endif


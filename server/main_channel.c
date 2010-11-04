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

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#include "server/red_common.h"
#include "server/demarshallers.h"
#include "common/ring.h"
#include "common/messages.h"
#include "reds.h"
#include "main_channel.h"
#include "red_channel.h"
#include "generated_marshallers.h"

#define ZERO_BUF_SIZE 4096

// approximate max receive message size for main channel
#define RECEIVE_BUF_SIZE \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

#define REDS_MAX_SEND_IOVEC 100

#define NET_TEST_WARMUP_BYTES 0
#define NET_TEST_BYTES (1024 * 250)

static uint8_t zero_page[ZERO_BUF_SIZE] = {0};

typedef struct RedsOutItem RedsOutItem;
struct RedsOutItem {
    PipeItem base;
};

typedef struct PingPipeItem {
    PipeItem base;
    int size;
} PingPipeItem;

typedef struct MouseModePipeItem {
    PipeItem base;
    int current_mode;
    int is_client_mouse_allowed;
} MouseModePipeItem;

typedef struct TokensPipeItem {
    PipeItem base;
    int tokens;
} TokensPipeItem;

typedef struct AgentDataPipeItem {
    PipeItem base;
    uint8_t* data;
    size_t len;
    spice_marshaller_item_free_func free_data;
    void *opaque;
} AgentDataPipeItem;

typedef struct InitPipeItem {
    PipeItem base;
    int connection_id;
    int display_channels_hint;
    int current_mouse_mode;
    int is_client_mouse_allowed;
    int multi_media_time;
    int ram_hint;
} InitPipeItem;

typedef struct NotifyPipeItem {
    PipeItem base;
    uint8_t *mess;
    int mess_len;
} NotifyPipeItem;

typedef struct MigrateBeginPipeItem {
    PipeItem base;
    int port;
    int sport;
    char *host;
    uint16_t cert_pub_key_type;
    uint32_t cert_pub_key_len;
    uint8_t *cert_pub_key;
} MigrateBeginPipeItem;

typedef struct MultiMediaTimePipeItem {
    PipeItem base;
    int time;
} MultiMediaTimePipeItem;

typedef struct MainChannel {
    RedChannel base;
    uint8_t recv_buf[RECEIVE_BUF_SIZE];
    uint32_t ping_id;
    uint32_t net_test_id;
    int net_test_stage;
} MainChannel;

enum NetTestStage {
    NET_TEST_STAGE_INVALID,
    NET_TEST_STAGE_WARMUP,
    NET_TEST_STAGE_LATENCY,
    NET_TEST_STAGE_RATE,
};

static uint64_t latency = 0;
uint64_t bitrate_per_sec = ~0;

static void main_disconnect(MainChannel *main_chan)
{
    main_chan->ping_id = 0;
    main_chan->net_test_id = 0;
    main_chan->net_test_stage = NET_TEST_STAGE_INVALID;
    red_channel_destroy(&main_chan->base);

    latency = 0;
    bitrate_per_sec = ~0;
}

void main_channel_start_net_test(Channel *channel)
{
    MainChannel *main_chan = channel->data;

    if (!main_chan || main_chan->net_test_id) {
        return;
    }

    if (main_channel_push_ping(channel, NET_TEST_WARMUP_BYTES)
        && main_channel_push_ping(channel, 0)
        && main_channel_push_ping(channel, NET_TEST_BYTES)) {
        main_chan->net_test_id = main_chan->ping_id - 2;
        main_chan->net_test_stage = NET_TEST_STAGE_WARMUP;
    }
}

static RedsOutItem *main_pipe_item_new(MainChannel *main_chan, int type)
{
    RedsOutItem *item = spice_malloc(sizeof(RedsOutItem));

    red_channel_pipe_item_init(&main_chan->base, &item->base, type);
    return item;
}

static MouseModePipeItem *main_mouse_mode_item_new(MainChannel *main_chan,
    int current_mode, int is_client_mouse_allowed)
{
    MouseModePipeItem *item = spice_malloc(sizeof(MouseModePipeItem));

    red_channel_pipe_item_init(&main_chan->base, &item->base,
                               SPICE_MSG_MAIN_MOUSE_MODE);
    item->current_mode = current_mode;
    item->is_client_mouse_allowed = is_client_mouse_allowed;
    return item;
}

static PingPipeItem *main_ping_item_new(MainChannel *channel, int size)
{
    PingPipeItem *item = spice_malloc(sizeof(PingPipeItem));

    red_channel_pipe_item_init(&channel->base, &item->base, SPICE_MSG_PING);
    item->size = size;
    return item;
}

static TokensPipeItem *main_tokens_item_new(MainChannel *main_chan, int tokens)
{
    TokensPipeItem *item = spice_malloc(sizeof(TokensPipeItem));

    red_channel_pipe_item_init(&main_chan->base, &item->base,
                               SPICE_MSG_MAIN_AGENT_TOKEN);
    item->tokens = tokens;
    return item;
}

static AgentDataPipeItem *main_agent_data_item_new(MainChannel *channel,
           uint8_t* data, size_t len,
           spice_marshaller_item_free_func free_data, void *opaque)
{
    AgentDataPipeItem *item = spice_malloc(sizeof(AgentDataPipeItem));

    red_channel_pipe_item_init(&channel->base, &item->base, SPICE_MSG_MAIN_AGENT_DATA);
    item->data = data;
    item->len = len;
    item->free_data = free_data;
    item->opaque = opaque;
    return item;
}

static InitPipeItem *main_init_item_new(MainChannel *main_chan,
    int connection_id, int display_channels_hint, int current_mouse_mode,
    int is_client_mouse_allowed, int multi_media_time,
    int ram_hint)
{
    InitPipeItem *item = spice_malloc(sizeof(InitPipeItem));

    red_channel_pipe_item_init(&main_chan->base, &item->base,
                               SPICE_MSG_MAIN_INIT);
    item->connection_id = connection_id;
    item->display_channels_hint = display_channels_hint;
    item->current_mouse_mode = current_mouse_mode;
    item->is_client_mouse_allowed = is_client_mouse_allowed;
    item->multi_media_time = multi_media_time;
    item->ram_hint = ram_hint;
    return item;
}

static NotifyPipeItem *main_notify_item_new(MainChannel *main_chan,
                                        uint8_t *mess, const int mess_len)
{
    NotifyPipeItem *item = spice_malloc(sizeof(NotifyPipeItem));

    red_channel_pipe_item_init(&main_chan->base, &item->base,
                               SPICE_MSG_NOTIFY);
    item->mess = mess;
    item->mess_len = mess_len;
    return item;
}

static MigrateBeginPipeItem *main_migrate_begin_item_new(
    MainChannel *main_chan, int port, int sport,
    char *host, uint16_t cert_pub_key_type, uint32_t cert_pub_key_len,
    uint8_t *cert_pub_key)
{
    MigrateBeginPipeItem *item = spice_malloc(sizeof(MigrateBeginPipeItem));

    red_channel_pipe_item_init(&main_chan->base, &item->base,
                               SPICE_MSG_MAIN_MIGRATE_BEGIN);
    item->port = port;
    item->sport = sport;
    item->host = host;
    item->cert_pub_key_type = cert_pub_key_type;
    item->cert_pub_key_len = cert_pub_key_len;
    item->cert_pub_key = cert_pub_key;
    return item;
}

static MultiMediaTimePipeItem *main_multi_media_time_item_new(
    MainChannel *main_chan, int time)
{
    MultiMediaTimePipeItem *item;

    item = spice_malloc(sizeof(MultiMediaTimePipeItem));
    red_channel_pipe_item_init(&main_chan->base, &item->base,
                               SPICE_MSG_MAIN_MULTI_MEDIA_TIME);
    item->time = time;
    return item;
}

static void main_channel_push_channels(MainChannel *main_chan)
{
    RedsOutItem *item;

    item = main_pipe_item_new(main_chan, SPICE_MSG_MAIN_CHANNELS_LIST);
    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_channels(MainChannel *main_chan)
{
    SpiceMsgChannels* channels_info;

    channels_info = (SpiceMsgChannels *)spice_malloc(sizeof(SpiceMsgChannels)
                            + reds_num_of_channels() * sizeof(SpiceChannelId));
    reds_fill_channels(channels_info);
    spice_marshall_msg_main_channels_list(
        main_chan->base.send_data.marshaller, channels_info);
    free(channels_info);
}

int main_channel_push_ping(Channel *channel, int size)
{
    MainChannel *main_chan = channel->data;
    PingPipeItem *item;
    
    if (main_chan == NULL) {
        return FALSE;
    }
    item = main_ping_item_new(main_chan, size);
    red_channel_pipe_add(&main_chan->base, &item->base);
    return TRUE;
}

static void main_channel_marshall_ping(MainChannel *main_chan, int size)
{
    struct timespec time_space;
    SpiceMsgPing ping;
    SpiceMarshaller *m = main_chan->base.send_data.marshaller;

    ping.id = ++main_chan->ping_id;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    ping.timestamp = time_space.tv_sec * 1000000LL + time_space.tv_nsec / 1000LL;
    spice_marshall_msg_ping(m, &ping);

    while (size > 0) {
        int now = MIN(ZERO_BUF_SIZE, size);
        size -= now;
        spice_marshaller_add_ref(m, zero_page, now);
    }
}

void main_channel_push_mouse_mode(Channel *channel, int current_mode,
                                  int is_client_mouse_allowed)
{
    MainChannel *main_chan = channel->data;
    MouseModePipeItem *item;

    item = main_mouse_mode_item_new(main_chan, current_mode,
                                    is_client_mouse_allowed);
    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_mouse_mode(MainChannel *main_chan, int current_mode, int is_client_mouse_allowed)
{
    SpiceMsgMainMouseMode mouse_mode;
    mouse_mode.supported_modes = SPICE_MOUSE_MODE_SERVER;
    if (is_client_mouse_allowed) {
        mouse_mode.supported_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    mouse_mode.current_mode = current_mode;
    spice_marshall_msg_main_mouse_mode(main_chan->base.send_data.marshaller,
                                       &mouse_mode);
}

void main_channel_push_agent_connected(Channel *channel)
{
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    item = main_pipe_item_new(main_chan, SPICE_MSG_MAIN_AGENT_CONNECTED);
    red_channel_pipe_add(&main_chan->base, &item->base);
}

void main_channel_push_agent_disconnected(Channel *channel)
{
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    item = main_pipe_item_new(main_chan, SPICE_MSG_MAIN_AGENT_DISCONNECTED);
    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_agent_disconnected(MainChannel *main_chan)
{
    SpiceMsgMainAgentDisconnect disconnect;

    disconnect.error_code = SPICE_LINK_ERR_OK;
    spice_marshall_msg_main_agent_disconnected(
        main_chan->base.send_data.marshaller, &disconnect);
}

void main_channel_push_tokens(Channel *channel, uint32_t num_tokens)
{
    MainChannel *main_chan = channel->data;
    TokensPipeItem *item = main_tokens_item_new(main_chan, num_tokens);

    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_tokens(MainChannel *main_chan, uint32_t num_tokens)
{
    SpiceMsgMainAgentTokens tokens;

    tokens.num_tokens = num_tokens;
    spice_marshall_msg_main_agent_token(
        main_chan->base.send_data.marshaller, &tokens);
}

void main_channel_push_agent_data(Channel *channel, uint8_t* data, size_t len,
           spice_marshaller_item_free_func free_data, void *opaque)
{
    MainChannel *main_chan = channel->data;
    AgentDataPipeItem *item;

    item = main_agent_data_item_new(main_chan, data, len, free_data, opaque);
    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_agent_data(MainChannel *main_chan,
                                  AgentDataPipeItem *item)
{
    spice_marshaller_add_ref_full(main_chan->base.send_data.marshaller,
        item->data, item->len, item->free_data, item->opaque);
}

static void main_channel_push_migrate_data_item(MainChannel *main_chan)
{
    RedsOutItem *item = main_pipe_item_new(main_chan, SPICE_MSG_MIGRATE_DATA);

    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_migrate_data_item(MainChannel *main_chan)
{
    SpiceMarshaller *m = main_chan->base.send_data.marshaller;
    MainMigrateData *data = (MainMigrateData *)spice_marshaller_reserve_space(m, sizeof(MainMigrateData));

    reds_marshall_migrate_data_item(m, data); // TODO: from reds split. ugly separation.
    data->serial = red_channel_get_message_serial(&main_chan->base);
    data->ping_id = main_chan->ping_id;
}

static void main_channel_receive_migrate_data(MainChannel *main_chan,
                                  MainMigrateData *data, uint8_t *end)
{
    red_channel_set_message_serial(&main_chan->base, data->serial);
    main_chan->ping_id = data->ping_id;
}

void main_channel_push_init(Channel *channel, int connection_id,
    int display_channels_hint, int current_mouse_mode,
    int is_client_mouse_allowed, int multi_media_time,
    int ram_hint)
{
    InitPipeItem *item;
    MainChannel *main_chan = channel->data;

    item = main_init_item_new(main_chan,
             connection_id, display_channels_hint, current_mouse_mode,
             is_client_mouse_allowed, multi_media_time, ram_hint);
    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_init(MainChannel *main_chan,
                                       InitPipeItem *item)
{
    SpiceMsgMainInit init;

    init.session_id = item->connection_id;
    init.display_channels_hint = item->display_channels_hint;
    init.current_mouse_mode = item->current_mouse_mode;
    init.supported_mouse_modes = SPICE_MOUSE_MODE_SERVER;
    if (item->is_client_mouse_allowed) {
        init.supported_mouse_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    init.agent_connected = reds_has_vdagent();
    init.agent_tokens = REDS_AGENT_WINDOW_SIZE;
    init.multi_media_time = item->multi_media_time;
    init.ram_hint = item->ram_hint;
    spice_marshall_msg_main_init(main_chan->base.send_data.marshaller, &init);
}

void main_channel_push_notify(Channel *channel, uint8_t *mess, const int mess_len)
{
    MainChannel *main_chan = channel->data;
    NotifyPipeItem *item = main_notify_item_new(main_chan, mess, mess_len);

    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_notify(MainChannel *main_chan, NotifyPipeItem *item)
{
    SpiceMsgNotify notify;
    SpiceMarshaller *m = main_chan->base.send_data.marshaller;

    notify.time_stamp = get_time_stamp(); // TODO - move to main_new_notify_item
    notify.severity = SPICE_NOTIFY_SEVERITY_WARN;
    notify.visibilty = SPICE_NOTIFY_VISIBILITY_HIGH;
    notify.what = SPICE_WARN_GENERAL;
    notify.message_len = item->mess_len;
    spice_marshall_msg_notify(m, &notify);
    spice_marshaller_add(m, item->mess, item->mess_len + 1);
}

void main_channel_push_migrate_begin(Channel *channel, int port, int sport,
    char *host, uint16_t cert_pub_key_type, uint32_t cert_pub_key_len,
    uint8_t *cert_pub_key)
{
    MainChannel *main_chan = channel->data;
    MigrateBeginPipeItem *item = main_migrate_begin_item_new(main_chan, port,
        sport, host, cert_pub_key_type, cert_pub_key_len, cert_pub_key);

    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_migrate_begin(MainChannel *main_chan,
    MigrateBeginPipeItem *item)
{
    SpiceMsgMainMigrationBegin migrate;

    migrate.port = item->port;
    migrate.sport = item->sport;
    migrate.host_size = strlen(item->host) + 1;
    migrate.host_data = (uint8_t *)item->host;
    migrate.pub_key_type = item->cert_pub_key_type;
    migrate.pub_key_size = item->cert_pub_key_len;
    migrate.pub_key_data = item->cert_pub_key;
    spice_marshall_msg_main_migrate_begin(main_chan->base.send_data.marshaller,
                                          &migrate);
}

void main_channel_push_migrate(Channel *channel)
{
    MainChannel *main_chan = channel->data;
    RedsOutItem *item = main_pipe_item_new(main_chan, SPICE_MSG_MIGRATE);

    red_channel_pipe_add(&main_chan->base, &item->base);
}

static void main_channel_marshall_migrate(MainChannel *main_chan)
{
    SpiceMsgMigrate migrate;

    migrate.flags = SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER;
    spice_marshall_msg_migrate(main_chan->base.send_data.marshaller, &migrate);
}

void main_channel_push_migrate_cancel(Channel *channel)
{
    MainChannel *main_chan = channel->data;
    RedsOutItem *item = main_pipe_item_new(main_chan,
                                           SPICE_MSG_MAIN_MIGRATE_CANCEL);

    red_channel_pipe_add(&main_chan->base, &item->base);
}

void main_channel_push_multi_media_time(Channel *channel, int time)
{
    MainChannel *main_chan = channel->data;

    MultiMediaTimePipeItem *item =
        main_multi_media_time_item_new(main_chan, time);
    red_channel_pipe_add(&main_chan->base, &item->base);
}

static PipeItem *main_migrate_switch_item_new(MainChannel *main_chan)
{
    PipeItem *item = spice_malloc(sizeof(*item));

    red_channel_pipe_item_init(&main_chan->base, item,
                               SPICE_MSG_MAIN_MIGRATE_SWITCH_HOST);
    return item;
}

void main_channel_push_migrate_switch(Channel *channel)
{
    MainChannel *main_chan = channel->data;

    red_channel_pipe_add(&main_chan->base,
        main_migrate_switch_item_new(main_chan));
}

static void main_channel_marshall_migrate_switch(MainChannel *main_chan)
{
    SpiceMsgMainMigrationSwitchHost migrate;

    red_printf("");

    reds_fill_mig_switch(&migrate);
    spice_marshall_msg_main_migrate_switch_host(
        main_chan->base.send_data.marshaller, &migrate);

    reds_mig_release();
}

static void main_channel_marshall_multi_media_time(MainChannel *main_chan,
    MultiMediaTimePipeItem *item)
{
    SpiceMsgMainMultiMediaTime time_mes;

    time_mes.time = item->time;
    spice_marshall_msg_main_multi_media_time(
        main_chan->base.send_data.marshaller, &time_mes);
}

static void main_channel_send_item(RedChannel *channel, PipeItem *base)
{
    MainChannel *main_chan = SPICE_CONTAINEROF(channel, MainChannel, base);

    red_channel_reset_send_data(channel);
    red_channel_init_send_data(channel, base->type, base);
    switch (base->type) {
        case SPICE_MSG_MAIN_CHANNELS_LIST:
            main_channel_marshall_channels(main_chan);
            break;
        case SPICE_MSG_PING:
            main_channel_marshall_ping(main_chan,
                SPICE_CONTAINEROF(base, PingPipeItem, base)->size);
            break;
        case SPICE_MSG_MAIN_MOUSE_MODE:
            {
                MouseModePipeItem *item =
                    SPICE_CONTAINEROF(base, MouseModePipeItem, base);
                main_channel_marshall_mouse_mode(main_chan,
                    item->current_mode, item->is_client_mouse_allowed);
                break;
            }
        case SPICE_MSG_MAIN_AGENT_DISCONNECTED:
            main_channel_marshall_agent_disconnected(main_chan);
            break;
        case SPICE_MSG_MAIN_AGENT_TOKEN:
            main_channel_marshall_tokens(main_chan,
                SPICE_CONTAINEROF(base, TokensPipeItem, base)->tokens);
            break;
        case SPICE_MSG_MAIN_AGENT_DATA:
            main_channel_marshall_agent_data(main_chan,
                SPICE_CONTAINEROF(base, AgentDataPipeItem, base));
            break;
        case SPICE_MSG_MIGRATE_DATA:
            main_channel_marshall_migrate_data_item(main_chan);
            break;
        case SPICE_MSG_MAIN_INIT:
            main_channel_marshall_init(main_chan,
                SPICE_CONTAINEROF(base, InitPipeItem, base));
            break;
        case SPICE_MSG_NOTIFY:
            main_channel_marshall_notify(main_chan,
                SPICE_CONTAINEROF(base, NotifyPipeItem, base));
            break;
        case SPICE_MSG_MIGRATE:
            main_channel_marshall_migrate(main_chan);
            break;
        case SPICE_MSG_MAIN_MIGRATE_BEGIN:
            main_channel_marshall_migrate_begin(main_chan,
                SPICE_CONTAINEROF(base, MigrateBeginPipeItem, base));
            break;
        case SPICE_MSG_MAIN_MULTI_MEDIA_TIME:
            main_channel_marshall_multi_media_time(main_chan,
                SPICE_CONTAINEROF(base, MultiMediaTimePipeItem, base));
            break;
        case SPICE_MSG_MAIN_MIGRATE_SWITCH_HOST:
            main_channel_marshall_migrate_switch(main_chan);
            break;
    };
    red_channel_begin_send_message(channel);
}

static void main_channel_release_pipe_item(RedChannel *channel,
    PipeItem *base, int item_pushed)
{
    free(base);
}

static int main_channel_handle_parsed(RedChannel *channel, size_t size, uint32_t type, void *message)
{
    MainChannel *main_chan = SPICE_CONTAINEROF(channel, MainChannel, base);

    switch (type) {
    case SPICE_MSGC_MAIN_AGENT_START:
        red_printf("agent start");
        if (!main_chan) {
            return FALSE;
        }
        reds_on_main_agent_start(main_chan);
        break;
    case SPICE_MSGC_MAIN_AGENT_DATA: {
        reds_on_main_agent_data(message, size);
        break;
    }
    case SPICE_MSGC_MAIN_AGENT_TOKEN:
        break;
    case SPICE_MSGC_MAIN_ATTACH_CHANNELS:
        main_channel_push_channels(main_chan);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECTED:
        red_printf("connected");
        reds_on_main_migrate_connected();
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECT_ERROR:
        red_printf("mig connect error");
        reds_on_main_migrate_connect_error();
        break;
    case SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST:
        reds_on_main_mouse_mode_request(message, size);
        break;
    case SPICE_MSGC_PONG: {
        SpiceMsgPing *ping = (SpiceMsgPing *)message;
        uint64_t roundtrip;
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        roundtrip = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL - ping->timestamp;

        if (ping->id == main_chan->net_test_id) {
            switch (main_chan->net_test_stage) {
            case NET_TEST_STAGE_WARMUP:
                main_chan->net_test_id++;
                main_chan->net_test_stage = NET_TEST_STAGE_LATENCY;
                break;
            case NET_TEST_STAGE_LATENCY:
                main_chan->net_test_id++;
                main_chan->net_test_stage = NET_TEST_STAGE_RATE;
                latency = roundtrip;
                break;
            case NET_TEST_STAGE_RATE:
                main_chan->net_test_id = 0;
                if (roundtrip <= latency) {
                    // probably high load on client or server result with incorrect values
                    latency = 0;
                    red_printf("net test: invalid values, latency %lu roundtrip %lu. assuming high"
                               "bandwidth", latency, roundtrip);
                    break;
                }
                bitrate_per_sec = (uint64_t)(NET_TEST_BYTES * 8) * 1000000 / (roundtrip - latency);
                red_printf("net test: latency %f ms, bitrate %lu bps (%f Mbps)%s",
                           (double)latency / 1000,
                           bitrate_per_sec,
                           (double)bitrate_per_sec / 1024 / 1024,
                           IS_LOW_BANDWIDTH() ? " LOW BANDWIDTH" : "");
                main_chan->net_test_stage = NET_TEST_STAGE_INVALID;
                break;
            default:
                red_printf("invalid net test stage, ping id %d test id %d stage %d",
                           ping->id,
                           main_chan->net_test_id,
                           main_chan->net_test_stage);
            }
            break;
        }
#ifdef RED_STATISTICS
        reds_update_stat_value(roundtrip);
#endif
        break;
    }
    case SPICE_MSGC_MIGRATE_FLUSH_MARK:
        main_channel_push_migrate_data_item(main_chan);
        break;
    case SPICE_MSGC_MIGRATE_DATA: {
            MainMigrateData *data = (MainMigrateData *)message;
            uint8_t *end = ((uint8_t *)message) + size;
            main_channel_receive_migrate_data(main_chan, data, end);
            reds_on_main_receive_migrate_data(data, end);
            break;
        }
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        red_printf("unexpected type %d", type);
    }
    return TRUE;
}

static void main_channel_on_error(RedChannel *channel)
{
    reds_disconnect();
}

static uint8_t *main_channel_alloc_msg_rcv_buf(RedChannel *channel, SpiceDataHeader *msg_header)
{
    MainChannel *main_chan = SPICE_CONTAINEROF(channel, MainChannel, base);

    return main_chan->recv_buf;
}

static void main_channel_release_msg_rcv_buf(RedChannel *channel, SpiceDataHeader *msg_header,
                                               uint8_t *msg)
{
}

static int main_channel_config_socket(RedChannel *channel)
{
    return TRUE;
}

static void main_channel_hold_pipe_item(PipeItem *item)
{
}

static void main_channel_link(Channel *channel, RedsStreamContext *peer, int migration,
                        int num_common_caps, uint32_t *common_caps, int num_caps,
                        uint32_t *caps)
{
    MainChannel *main_chan;
    red_printf("");
    ASSERT(channel->data == NULL);

    main_chan = (MainChannel*)red_channel_create_parser(
        sizeof(*main_chan), peer, core, migration, FALSE /* handle_acks */
        ,main_channel_config_socket
        ,spice_get_client_channel_parser(SPICE_CHANNEL_MAIN, NULL)
        ,main_channel_handle_parsed
        ,main_channel_alloc_msg_rcv_buf
        ,main_channel_release_msg_rcv_buf
        ,main_channel_hold_pipe_item
        ,main_channel_send_item
        ,main_channel_release_pipe_item
        ,main_channel_on_error
        ,main_channel_on_error);
    ASSERT(main_chan);
    channel->data = main_chan;
}

int main_channel_getsockname(Channel *channel, struct sockaddr *sa, socklen_t *salen)
{
    MainChannel *main_chan = channel->data;

    return main_chan ? getsockname(main_chan->base.peer->socket, sa, salen) : -1;
}

int main_channel_getpeername(Channel *channel, struct sockaddr *sa, socklen_t *salen)
{
    MainChannel *main_chan = channel->data;

    return main_chan ? getpeername(main_chan->base.peer->socket, sa, salen) : -1;
}

void main_channel_close(Channel *channel)
{
    MainChannel *main_chan = channel->data;

    if (main_chan && main_chan->base.peer) {
        close(main_chan->base.peer->socket);
    }
}

static void main_channel_shutdown(Channel *channel)
{
    MainChannel *main_chan = channel->data;

    if (main_chan != NULL) {
        main_disconnect(main_chan);
    }
}

static void main_channel_migrate()
{
}

Channel* main_channel_init(void)
{
    Channel *channel;

    channel = spice_new0(Channel, 1);
    channel->type = SPICE_CHANNEL_MAIN;
    channel->link = main_channel_link;
    channel->shutdown = main_channel_shutdown;
    channel->migrate = main_channel_migrate;
    return channel;
}


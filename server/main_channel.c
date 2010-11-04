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
    RingItem link;
    SpiceMarshaller *m;
    SpiceDataHeader *header;
};

typedef struct RedsOutgoingData {
    Ring pipe;
    RedsOutItem *item;
    int vec_size;
    struct iovec vec_buf[REDS_MAX_SEND_IOVEC];
    struct iovec *vec;
} RedsOutgoingData;

// TODO - remove and use red_channel.h
typedef struct IncomingHandler {
    spice_parse_channel_func_t parser;
    void *opaque;
    int shut;
    uint8_t buf[RECEIVE_BUF_SIZE];
    uint32_t end_pos;
    void (*handle_message)(void *opaque, size_t size, uint32_t type, void *message);
} IncomingHandler;

typedef struct MainChannel {
    RedsStreamContext *peer;
    IncomingHandler in_handler;
    RedsOutgoingData outgoing;
    uint64_t serial; //migrate me
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

static void main_channel_out_item_free(RedsOutItem *item);

static void main_reset_outgoing(MainChannel *main_chan)
{
    RedsOutgoingData *outgoing = &main_chan->outgoing;
    RingItem *ring_item;

    if (outgoing->item) {
        main_channel_out_item_free(outgoing->item);
        outgoing->item = NULL;
    }
    while ((ring_item = ring_get_tail(&outgoing->pipe))) {
        RedsOutItem *out_item = (RedsOutItem *)ring_item;
        ring_remove(ring_item);
        main_channel_out_item_free(out_item);
    }
    outgoing->vec_size = 0;
    outgoing->vec = outgoing->vec_buf;
}

// ALON from reds_disconnect
static void main_disconnect(MainChannel *main_chan)
{
    if (!main_chan || !main_chan->peer) {
        return;
    }
    main_reset_outgoing(main_chan);
    core->watch_remove(main_chan->peer->watch);
    main_chan->peer->watch = NULL;
    main_chan->peer->cb_free(main_chan->peer);
    main_chan->peer = NULL;
    main_chan->in_handler.shut = TRUE;
    main_chan->serial = 0;
    main_chan->ping_id = 0;
    main_chan->net_test_id = 0;
    main_chan->net_test_stage = NET_TEST_STAGE_INVALID;
    main_chan->in_handler.end_pos = 0;

    // TODO: Should probably reset these on the ping start, not here
    latency = 0;
    bitrate_per_sec = ~0;
}

void main_channel_start_net_test(Channel *channel)
{
    MainChannel *main_chan = channel->data;

    if (!main_chan || main_chan->net_test_id) {
        return;
    }

    if (main_channel_push_ping(channel, NET_TEST_WARMUP_BYTES) &&
                            main_channel_push_ping(channel, 0) &&
                            main_channel_push_ping(channel, NET_TEST_BYTES)) {
        main_chan->net_test_id = main_chan->ping_id - 2;
        main_chan->net_test_stage = NET_TEST_STAGE_WARMUP;
    }
}

static int handle_incoming(RedsStreamContext *peer, IncomingHandler *handler)
{
    for (;;) {
        uint8_t *buf = handler->buf;
        uint32_t pos = handler->end_pos;
        uint8_t *end = buf + pos;
        SpiceDataHeader *header;
        int n;
        n = peer->cb_read(peer->ctx, buf + pos, RECEIVE_BUF_SIZE - pos);
        if (n <= 0) {
            if (n == 0) {
                return -1;
            }
            switch (errno) {
            case EAGAIN:
                return 0;
            case EINTR:
                break;
            case EPIPE:
                return -1;
            default:
                red_printf("%s", strerror(errno));
                return -1;
            }
        } else {
            pos += n;
            end = buf + pos;
            while (buf + sizeof(SpiceDataHeader) <= end &&
                   buf + sizeof(SpiceDataHeader) + (header = (SpiceDataHeader *)buf)->size <= end) {
                uint8_t *data = (uint8_t *)(header+1);
                size_t parsed_size;
                uint8_t *parsed;
                message_destructor_t parsed_free;

                buf += sizeof(SpiceDataHeader) + header->size;
                parsed = handler->parser(data, data + header->size, header->type,
                                         SPICE_VERSION_MINOR, &parsed_size, &parsed_free);
                if (parsed == NULL) {
                    red_printf("failed to parse message type %d", header->type);
                    return -1;
                }
                handler->handle_message(handler->opaque, parsed_size, header->type, parsed);
                parsed_free(parsed);
                if (handler->shut) {
                    return -1;
                }
            }
            memmove(handler->buf, buf, (handler->end_pos = end - buf));
        }
    }
}

static RedsOutItem *new_out_item(MainChannel *main_chan, uint32_t type)
{
    RedsOutItem *item;

    item = spice_new(RedsOutItem, 1);
    ring_item_init(&item->link);

    item->m = spice_marshaller_new();
    item->header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(item->m, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(item->m, sizeof(SpiceDataHeader));

    item->header->serial = ++main_chan->serial;
    item->header->type = type;
    item->header->sub_list = 0;

    return item;
}

static void main_channel_out_item_free(RedsOutItem *item)
{
    spice_marshaller_destroy(item->m);
    free(item);
}

static struct iovec *main_channel_iovec_skip(struct iovec vec[], int skip, int *vec_size)
{
    struct iovec *now = vec;

    while (skip && skip >= now->iov_len) {
        skip -= now->iov_len;
        --*vec_size;
        now++;
    }
    now->iov_base = (uint8_t *)now->iov_base + skip;
    now->iov_len -= skip;
    return now;
}

static int main_channel_send_data(MainChannel *main_chan)
{
    RedsOutgoingData *outgoing = &main_chan->outgoing;
    int n;

    if (!outgoing->item) {
        return TRUE;
    }

    ASSERT(outgoing->vec_size);
    for (;;) {
        if ((n = main_chan->peer->cb_writev(main_chan->peer->ctx, outgoing->vec, outgoing->vec_size)) == -1) {
            switch (errno) {
            case EAGAIN:
                core->watch_update_mask(main_chan->peer->watch,
                                        SPICE_WATCH_EVENT_READ | SPICE_WATCH_EVENT_WRITE);
                return FALSE;
            case EINTR:
                break;
            case EPIPE:
                reds_disconnect();
                return FALSE;
            default:
                red_printf("%s", strerror(errno));
                reds_disconnect();
                return FALSE;
            }
        } else {
            outgoing->vec = main_channel_iovec_skip(outgoing->vec, n, &outgoing->vec_size);
            if (!outgoing->vec_size) {
                main_channel_out_item_free(outgoing->item);
                outgoing->item = NULL;
                outgoing->vec = outgoing->vec_buf;
                return TRUE;
            }
        }
    }
}

static void main_channel_push(MainChannel *main_chan)
{
    RedsOutgoingData *outgoing = &main_chan->outgoing;
    RingItem *ring_item;
    RedsOutItem *item;

    for (;;) {
        if (!main_chan->peer || outgoing->item || !(ring_item = ring_get_tail(&outgoing->pipe))) {
            return;
        }
        ring_remove(ring_item);
        outgoing->item = item = (RedsOutItem *)ring_item;

        spice_marshaller_flush(item->m);
        item->header->size = spice_marshaller_get_total_size(item->m) - sizeof(SpiceDataHeader);

        outgoing->vec_size = spice_marshaller_fill_iovec(item->m,
                                                         outgoing->vec_buf,
                                                         REDS_MAX_SEND_IOVEC, 0);
        main_channel_send_data(main_chan);
    }
}

static void main_channel_push_pipe_item(MainChannel *main_chan, RedsOutItem *item)
{
    ring_add(&main_chan->outgoing.pipe, &item->link);
    main_channel_push(main_chan);
}

static void main_channel_push_channels(MainChannel *main_chan)
{
    SpiceMsgChannels* channels_info;
    RedsOutItem *item;

    item = new_out_item(main_chan, SPICE_MSG_MAIN_CHANNELS_LIST);
    channels_info = (SpiceMsgChannels *)spice_malloc(sizeof(SpiceMsgChannels) + reds_num_of_channels() * sizeof(SpiceChannelId));
    reds_fill_channels(channels_info);
    spice_marshall_msg_main_channels_list(item->m, channels_info);
    free(channels_info);
    main_channel_push_pipe_item(main_chan, item);
}

int main_channel_push_ping(Channel *channel, int size)
{
    struct timespec time_space;
    RedsOutItem *item;
    SpiceMsgPing ping;
    MainChannel *main_chan = channel->data;

    if (!main_chan) {
        return FALSE;
    }
    item = new_out_item(main_chan, SPICE_MSG_PING);
    ping.id = ++main_chan->ping_id;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    ping.timestamp = time_space.tv_sec * 1000000LL + time_space.tv_nsec / 1000LL;
    spice_marshall_msg_ping(item->m, &ping);

    while (size > 0) {
        int now = MIN(ZERO_BUF_SIZE, size);
        size -= now;
        spice_marshaller_add_ref(item->m, zero_page, now);
    }

    main_channel_push_pipe_item(main_chan, item);

    return TRUE;
}

void main_channel_push_mouse_mode(Channel *channel, int current_mode, int is_client_mouse_allowed)
{
    SpiceMsgMainMouseMode mouse_mode;
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    if (!main_chan) {
        return;
    }

    item = new_out_item(main_chan, SPICE_MSG_MAIN_MOUSE_MODE);
    mouse_mode.supported_modes = SPICE_MOUSE_MODE_SERVER;
    if (is_client_mouse_allowed) {
        mouse_mode.supported_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    mouse_mode.current_mode = current_mode;

    spice_marshall_msg_main_mouse_mode(item->m, &mouse_mode);

    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_agent_connected(Channel *channel)
{
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    item = new_out_item(main_chan, SPICE_MSG_MAIN_AGENT_CONNECTED);
    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_agent_disconnected(Channel *channel)
{
    SpiceMsgMainAgentDisconnect disconnect;
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    item = new_out_item(main_chan, SPICE_MSG_MAIN_AGENT_DISCONNECTED);
    disconnect.error_code = SPICE_LINK_ERR_OK;
    spice_marshall_msg_main_agent_disconnected(item->m, &disconnect);
    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_tokens(Channel *channel, uint32_t num_tokens)
{
    SpiceMsgMainAgentTokens tokens;
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    if (!main_chan) {
        return;
    }
    item = new_out_item(main_chan, SPICE_MSG_MAIN_AGENT_TOKEN);
    tokens.num_tokens = num_tokens;
    spice_marshall_msg_main_agent_token(item->m, &tokens);
    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_agent_data(Channel *channel, uint8_t* data, size_t len,
           spice_marshaller_item_free_func free_data, void *opaque)
{
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    item = new_out_item(main_chan, SPICE_MSG_MAIN_AGENT_DATA);
    spice_marshaller_add_ref_full(item->m, data, len, free_data, opaque);
    main_channel_push_pipe_item(main_chan, item);
}

static void main_channel_push_migrate_data_item(MainChannel *main_chan)
{
    RedsOutItem *item = new_out_item(main_chan, SPICE_MSG_MIGRATE_DATA);
    SpiceMarshaller *m = item->m;
    MainMigrateData *data = (MainMigrateData *)spice_marshaller_reserve_space(m, sizeof(MainMigrateData));

    reds_marshall_migrate_data_item(m, data); // TODO: from reds split. ugly separation.
    data->serial = main_chan->serial;
    data->ping_id = main_chan->ping_id;
    main_channel_push_pipe_item(main_chan, item);
}

static void main_channel_receive_migrate_data(MainChannel *main_chan, MainMigrateData *data, uint8_t *end)
{
    main_chan->serial = data->serial;
    main_chan->ping_id = data->ping_id;
}

void main_channel_push_init(Channel *channel, int connection_id, int display_channels_hint,
    int current_mouse_mode, int is_client_mouse_allowed, int multi_media_time,
    int ram_hint)
{
    RedsOutItem *item;
    SpiceMsgMainInit init;
    MainChannel *main_chan = channel->data;

    item = new_out_item(main_chan, SPICE_MSG_MAIN_INIT);
    init.session_id = connection_id;
    init.display_channels_hint = display_channels_hint;
    init.current_mouse_mode = current_mouse_mode;
    init.supported_mouse_modes = SPICE_MOUSE_MODE_SERVER;
    if (is_client_mouse_allowed) {
        init.supported_mouse_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    init.agent_connected = reds_has_vdagent();
    init.agent_tokens = REDS_AGENT_WINDOW_SIZE;
    init.multi_media_time = multi_media_time;
    init.ram_hint = ram_hint;
    spice_marshall_msg_main_init(item->m, &init);
    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_notify(Channel *channel, uint8_t *mess, const int mess_len)
{
    // TODO possible free-then-use bug - caller frees mess after this, but is that pointer being
    // used by spice_marshaller?
    RedsOutItem *item;
    SpiceMsgNotify notify;
    MainChannel *main_chan = channel->data;

    item = new_out_item(main_chan, SPICE_MSG_NOTIFY);
    notify.time_stamp = get_time_stamp();
    notify.severity = SPICE_NOTIFY_SEVERITY_WARN;
    notify.visibilty = SPICE_NOTIFY_VISIBILITY_HIGH;
    notify.what = SPICE_WARN_GENERAL;
    notify.message_len = mess_len;
    spice_marshall_msg_notify(item->m, &notify);
    spice_marshaller_add(item->m, mess, mess_len + 1);
    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_migrate_begin(Channel *channel, int port, int sport, char *host,
    uint16_t cert_pub_key_type, uint32_t cert_pub_key_len, uint8_t *cert_pub_key)
{
    MainChannel *main_chan = channel->data;
    RedsOutItem *item = new_out_item(main_chan, SPICE_MSG_MAIN_MIGRATE_BEGIN);
    SpiceMsgMainMigrationBegin migrate;

    migrate.port = port;
    migrate.sport = sport;
    migrate.host_size = strlen(host) + 1;
    migrate.host_data = (uint8_t *)host;
    migrate.pub_key_type = cert_pub_key_type;
    migrate.pub_key_size = cert_pub_key_len;
    migrate.pub_key_data = cert_pub_key;
    spice_marshall_msg_main_migrate_begin(item->m, &migrate);
    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_migrate(Channel *channel)
{
    RedsOutItem *item;
    SpiceMsgMigrate migrate;
    MainChannel *main_chan = channel->data;

    item = new_out_item(main_chan, SPICE_MSG_MIGRATE);
    migrate.flags = SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER;
    spice_marshall_msg_migrate(item->m, &migrate);
    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_migrate_cancel(Channel *channel)
{
    MainChannel *main_chan = channel->data;
    RedsOutItem *item = new_out_item(main_chan, SPICE_MSG_MAIN_MIGRATE_CANCEL);

    main_channel_push_pipe_item(main_chan, item);
}

void main_channel_push_multi_media_time(Channel *channel, int time)
{
    SpiceMsgMainMultiMediaTime time_mes;
    RedsOutItem *item;
    MainChannel *main_chan = channel->data;

    item = new_out_item(main_chan, SPICE_MSG_MAIN_MULTI_MEDIA_TIME);
    time_mes.time = time;
    spice_marshall_msg_main_multi_media_time(item->m, &time_mes);
    main_channel_push_pipe_item(main_chan, item);
}

static void main_channel_handle_message(void *opaque, size_t size, uint32_t type, void *message)
{
    MainChannel *main_chan = opaque;

    switch (type) {
    case SPICE_MSGC_MAIN_AGENT_START:
        red_printf("agent start");
        if (!main_chan) {
            return;
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
}

static void main_channel_event(int fd, int event, void *data)
{
    MainChannel *main_chan = data;

    if (event & SPICE_WATCH_EVENT_READ) {
        if (handle_incoming(main_chan->peer, &main_chan->in_handler)) {
            main_disconnect(main_chan);
            reds_disconnect();
        }
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        RedsOutgoingData *outgoing = &main_chan->outgoing;
        if (main_channel_send_data(main_chan)) {
            main_channel_push(main_chan);
            if (!outgoing->item && main_chan->peer) {
                core->watch_update_mask(main_chan->peer->watch,
                                        SPICE_WATCH_EVENT_READ);
            }
        }
    }
}

static void main_channel_link(Channel *channel, RedsStreamContext *peer, int migration,
                        int num_common_caps, uint32_t *common_caps, int num_caps,
                        uint32_t *caps)
{
    MainChannel *main_chan;

    main_chan = spice_malloc0(sizeof(MainChannel));
    channel->data = main_chan;
    main_chan->peer = peer;
    main_chan->in_handler.shut = FALSE;
    main_chan->in_handler.parser = spice_get_client_channel_parser(SPICE_CHANNEL_MAIN, NULL);
    main_chan->in_handler.opaque = main_chan;
    main_chan->in_handler.handle_message = main_channel_handle_message;
    ring_init(&main_chan->outgoing.pipe);
    main_chan->outgoing.vec = main_chan->outgoing.vec_buf;
    peer->watch = core->watch_add(peer->socket,
                                  SPICE_WATCH_EVENT_READ,
                                  main_channel_event, main_chan);
}

int main_channel_getsockname(Channel *channel, struct sockaddr *sa, socklen_t *salen)
{
    MainChannel *main_chan = channel->data;

    return main_chan ? getsockname(main_chan->peer->socket, sa, salen) : -1;
}

int main_channel_getpeername(Channel *channel, struct sockaddr *sa, socklen_t *salen)
{
    MainChannel *main_chan = channel->data;

    return main_chan ? getpeername(main_chan->peer->socket, sa, salen) : -1;
}

void main_channel_close(Channel *channel)
{
    MainChannel *main_chan = channel->data;

    if (main_chan && main_chan->peer) {
        close(main_chan->peer->socket);
    }
}

static void main_channel_shutdown(Channel *channel)
{
    MainChannel *main_chan = channel->data;

    if (main_chan != NULL) {
        main_disconnect(main_chan); // TODO - really here? reset peer etc.
    }
    free(main_chan);
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


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

#include "config.h"

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

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#if HAVE_SASL
#include <sasl/sasl.h>
#endif

#include "spice.h"
#include "spice-experimental.h"
#include "reds.h"
#include <spice/protocol.h>
#include <spice/vd_agent.h>
#include "agent-msg-filter.h"

#include "red_common.h"
#include "red_dispatcher.h"
#include "snd_worker.h"
#include <spice/stats.h>
#include "stat.h"
#include "ring.h"
#include "demarshallers.h"
#include "marshaller.h"
#include "generated_marshallers.h"
#include "char_device.h"
#ifdef USE_TUNNEL
#include "red_tunnel_worker.h"
#endif
#ifdef USE_SMARTCARD
#include "smartcard.h"
#endif

SpiceCoreInterface *core = NULL;
static SpiceKbdInstance *keyboard = NULL;
static SpiceMouseInstance *mouse = NULL;
static SpiceTabletInstance *tablet = NULL;
static SpiceCharDeviceInstance *vdagent = NULL;

#define MIGRATION_NOTIFY_SPICE_KEY "spice_mig_ext"

#define REDS_MIG_VERSION 3
#define REDS_MIG_CONTINUE 1
#define REDS_MIG_ABORT 2
#define REDS_MIG_DIFF_VERSION 3

#define REDS_AGENT_WINDOW_SIZE 10
#define REDS_TOKENS_TO_SEND 5
#define REDS_NUM_INTERNAL_AGENT_MESSAGES 1
#define REDS_VDI_PORT_NUM_RECIVE_BUFFS 5
#define REDS_MAX_SEND_IOVEC 100

#define NET_TEST_WARMUP_BYTES 0
#define NET_TEST_BYTES (1024 * 250)

static int spice_port = -1;
static int spice_secure_port = -1;
static char spice_addr[256];
static int spice_family = PF_UNSPEC;
static char *default_renderer = "sw";
static int sasl_enabled = 0; // sasl disabled by default
#if HAVE_SASL
static char *sasl_appname = NULL; // default to "spice" if NULL
#endif

static int ticketing_enabled = 1; //Ticketing is enabled by default
static pthread_mutex_t *lock_cs;
static long *lock_count;
uint32_t streaming_video = STREAM_VIDEO_FILTER;
spice_image_compression_t image_compression = SPICE_IMAGE_COMPRESS_AUTO_GLZ;
spice_wan_compression_t jpeg_state = SPICE_WAN_COMPRESSION_AUTO;
spice_wan_compression_t zlib_glz_state = SPICE_WAN_COMPRESSION_AUTO;
#ifdef USE_TUNNEL
void *red_tunnel = NULL;
#endif
int agent_mouse = TRUE;
int agent_copypaste = TRUE;

static void openssl_init();

#define MIGRATE_TIMEOUT (1000 * 10) /* 10sec */
#define PING_INTERVAL (1000 * 10)
#define KEY_MODIFIERS_TTL (1000 * 2) /*2sec*/
#define MM_TIMER_GRANULARITY_MS (1000 / 30)
#define MM_TIME_DELTA 400 /*ms*/
#define VDI_PORT_WRITE_RETRY_TIMEOUT 100 /*ms*/

// approximate max receive message size
#define RECIVE_BUF_SIZE \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

#define SEND_BUF_SIZE 4096

#define SCROLL_LOCK_SCAN_CODE 0x46
#define NUM_LOCK_SCAN_CODE 0x45
#define CAPS_LOCK_SCAN_CODE 0x3a

typedef struct IncomingHandler {
    spice_parse_channel_func_t parser;
    void *opaque;
    int shut;
    uint8_t buf[RECIVE_BUF_SIZE];
    uint32_t end_pos;
    void (*handle_message)(void *opaque, size_t size, uint32_t type, void *message);
} IncomingHandler;

typedef struct OutgoingHandler {
    void *opaque;
    uint8_t buf[SEND_BUF_SIZE];
    uint8_t *now;
    uint32_t length;
    void (*select)(void *opaque, int select);
    void (*may_write)(void *opaque);
} OutgoingHandler;

typedef struct TicketAuthentication {
    char password[SPICE_MAX_PASSWORD_LENGTH];
    time_t expiration_time;
} TicketAuthentication;

static TicketAuthentication taTicket;

typedef struct TicketInfo {
    RSA *rsa;
    int rsa_size;
    BIGNUM *bn;
    SpiceLinkEncryptedTicket encrypted_ticket;
} TicketInfo;

typedef struct MonitorMode {
    uint32_t x_res;
    uint32_t y_res;
} MonitorMode;

typedef struct RedsOutItem RedsOutItem;
struct RedsOutItem {
    RingItem link;
    SpiceMarshaller *m;
    SpiceDataHeader *header;
};

typedef struct VDIReadBuf {
    RingItem link;
    int len;
    uint8_t data[SPICE_AGENT_MAX_DATA_SIZE];
} VDIReadBuf;

enum {
    VDI_PORT_READ_STATE_READ_HADER,
    VDI_PORT_READ_STATE_GET_BUFF,
    VDI_PORT_READ_STATE_READ_DATA,
};

void vdagent_char_device_wakeup(SpiceCharDeviceInstance *sin);
struct SpiceCharDeviceState vdagent_char_device_state = {
    .wakeup = &vdagent_char_device_wakeup,
};

typedef struct VDIPortState {
    int connected;
    uint32_t plug_generation;

    uint32_t num_tokens;
    uint32_t num_client_tokens;
    Ring external_bufs;
    Ring internal_bufs;
    Ring write_queue;
    AgentMsgFilter write_filter;

    Ring read_bufs;
    uint32_t read_state;
    uint32_t message_recive_len;
    uint8_t *recive_pos;
    uint32_t recive_len;
    VDIReadBuf *current_read_buf;
    AgentMsgFilter read_filter;

    VDIChunkHeader vdi_chunk_header;
} VDIPortState;

typedef struct InputsState {
    Channel *channel;
    RedsStream *stream;
    IncomingHandler in_handler;
    OutgoingHandler out_handler;
    VDAgentMouseState mouse_state;
    int pending_mouse_event;
    uint32_t motion_count;
    uint64_t serial; //migrate me
} InputsState;

typedef struct RedsOutgoingData {
    Ring pipe;
    RedsOutItem *item;
    int vec_size;
    struct iovec vec_buf[REDS_MAX_SEND_IOVEC];
    struct iovec *vec;
} RedsOutgoingData;

enum NetTestStage {
    NET_TEST_STAGE_INVALID,
    NET_TEST_STAGE_WARMUP,
    NET_TEST_STAGE_LATENCY,
    NET_TEST_STAGE_RATE,
};

#ifdef RED_STATISTICS

#define REDS_MAX_STAT_NODES 100
#define REDS_STAT_SHM_SIZE (sizeof(SpiceStat) + REDS_MAX_STAT_NODES * sizeof(SpiceStatNode))

typedef struct RedsStatValue {
    uint32_t value;
    uint32_t min;
    uint32_t max;
    uint32_t average;
    uint32_t count;
} RedsStatValue;

#endif

typedef struct RedsMigSpice RedsMigSpice;

typedef struct RedsState {
    int listen_socket;
    int secure_listen_socket;
    SpiceWatch *listen_watch;
    SpiceWatch *secure_listen_watch;
    RedsStream *stream;
    int disconnecting;
    uint32_t link_id;
    uint64_t serial; //migrate me
    VDIPortState agent_state;
    InputsState *inputs_state;

    int mig_wait_connect;
    int mig_wait_disconnect;
    int mig_inprogress;
    int mig_target;
    RedsMigSpice *mig_spice;
    int num_of_channels;
    IncomingHandler in_handler;
    RedsOutgoingData outgoing;
    Channel *channels;
    int mouse_mode;
    int is_client_mouse_allowed;
    int dispatcher_allows_client_mouse;
    MonitorMode monitor_mode;
    SpiceTimer *mig_timer;
    SpiceTimer *key_modifiers_timer;
    SpiceTimer *mm_timer;
    SpiceTimer *vdi_port_write_timer;
    int vdi_port_write_timer_started;

    TicketAuthentication taTicket;
    SSL_CTX *ctx;

#ifdef RED_STATISTICS
    char *stat_shm_name;
    SpiceStat *stat;
    pthread_mutex_t stat_lock;
    RedsStatValue roundtrip_stat;
    SpiceTimer *ping_timer;
    int ping_interval;
#endif
    uint32_t ping_id;
    uint32_t net_test_id;
    int net_test_stage;
    int peer_minor_version;
} RedsState;

uint64_t bitrate_per_sec = ~0;
static uint64_t latency = 0;

static RedsState *reds = NULL;

typedef struct AsyncRead {
    RedsStream *stream;
    void *opaque;
    uint8_t *now;
    uint8_t *end;
    void (*done)(void *opaque);
    void (*error)(void *opaque, int err);
} AsyncRead;

typedef struct RedLinkInfo {
    RedsStream *stream;
    AsyncRead asyc_read;
    SpiceLinkHeader link_header;
    SpiceLinkMess *link_mess;
    int mess_pos;
    TicketInfo tiTicketing;
    SpiceLinkAuthMechanism auth_mechanism;
} RedLinkInfo;

typedef struct VDIPortBuf VDIPortBuf;
struct  __attribute__ ((__packed__)) VDIPortBuf {
    RingItem link;
    uint8_t *now;
    int write_len;
    void (*free)(VDIPortBuf *buf);
    VDIChunkHeader chunk_header; //start send from &chunk_header
};

typedef struct __attribute__ ((__packed__)) VDAgentExtBuf {
    VDIPortBuf base;
    uint8_t buf[SPICE_AGENT_MAX_DATA_SIZE];
    VDIChunkHeader migrate_overflow;
} VDAgentExtBuf;

typedef struct __attribute__ ((__packed__)) VDInternalBuf {
    VDIPortBuf base;
    VDAgentMessage header;
    union {
        VDAgentMouseState mouse_state;
    }
    u;
    VDIChunkHeader migrate_overflow;
} VDInternalBuf;

typedef struct RedSSLParameters {
    char keyfile_password[256];
    char certs_file[256];
    char private_key_file[256];
    char ca_certificate_file[256];
    char dh_key_file[256];
    char ciphersuite[256];
} RedSSLParameters;

typedef struct ChannelSecurityOptions ChannelSecurityOptions;
struct ChannelSecurityOptions {
    uint32_t channel_id;
    uint32_t options;
    ChannelSecurityOptions *next;
};

#define ZERO_BUF_SIZE 4096

static uint8_t zero_page[ZERO_BUF_SIZE] = {0};

static void reds_push();
static void reds_out_item_free(RedsOutItem *item);

static ChannelSecurityOptions *channels_security = NULL;
static int default_channel_security =
    SPICE_CHANNEL_SECURITY_NONE | SPICE_CHANNEL_SECURITY_SSL;

static RedSSLParameters ssl_parameters;

static ChannelSecurityOptions *find_channel_security(int id)
{
    ChannelSecurityOptions *now = channels_security;
    while (now && now->channel_id != id) {
        now = now->next;
    }
    return now;
}

static void reds_channel_event(RedsStream *stream, int event)
{
    if (core->base.minor_version < 3 || core->channel_event == NULL)
        return;
    core->channel_event(event, &stream->info);
}

static ssize_t stream_write_cb(RedsStream *s, const void *buf, size_t size)
{
    return write(s->socket, buf, size);
}

static ssize_t stream_writev_cb(RedsStream *s, const struct iovec *iov, int iovcnt)
{
    return writev(s->socket, iov, iovcnt);
}

static ssize_t stream_read_cb(RedsStream *s, void *buf, size_t size)
{
    return read(s->socket, buf, size);
}

static ssize_t stream_ssl_write_cb(RedsStream *s, const void *buf, size_t size)
{
    int return_code;
    SPICE_GNUC_UNUSED int ssl_error;

    return_code = SSL_write(s->ssl, buf, size);

    if (return_code < 0) {
        ssl_error = SSL_get_error(s->ssl, return_code);
    }

    return return_code;
}

static ssize_t stream_ssl_read_cb(RedsStream *s, void *buf, size_t size)
{
    int return_code;
    SPICE_GNUC_UNUSED int ssl_error;

    return_code = SSL_read(s->ssl, buf, size);

    if (return_code < 0) {
        ssl_error = SSL_get_error(s->ssl, return_code);
    }

    return return_code;
}

static void reds_stream_remove_watch(RedsStream* s)
{
    if (s->watch) {
        core->watch_remove(s->watch);
        s->watch = NULL;
    }
}

static void reds_link_free(RedLinkInfo *link)
{
    reds_stream_free(link->stream);
    link->stream = NULL;

    free(link->link_mess);
    link->link_mess = NULL;

    BN_free(link->tiTicketing.bn);
    link->tiTicketing.bn = NULL;

    if (link->tiTicketing.rsa) {
        RSA_free(link->tiTicketing.rsa);
        link->tiTicketing.rsa = NULL;
    }

    free(link);
}

static struct iovec *reds_iovec_skip(struct iovec vec[], int skip, int *vec_size)
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

#ifdef RED_STATISTICS

void insert_stat_node(StatNodeRef parent, StatNodeRef ref)
{
    SpiceStatNode *node = &reds->stat->nodes[ref];
    uint32_t pos = INVALID_STAT_REF;
    uint32_t node_index;
    uint32_t *head;
    SpiceStatNode *n;

    node->first_child_index = INVALID_STAT_REF;
    head = (parent == INVALID_STAT_REF ? &reds->stat->root_index :
                                         &reds->stat->nodes[parent].first_child_index);
    node_index = *head;
    while (node_index != INVALID_STAT_REF && (n = &reds->stat->nodes[node_index]) &&
                                                     strcmp(node->name, n->name) > 0) {
        pos = node_index;
        node_index = n->next_sibling_index;
    }
    if (pos == INVALID_STAT_REF) {
        node->next_sibling_index = *head;
        *head = ref;
    } else {
        n = &reds->stat->nodes[pos];
        node->next_sibling_index = n->next_sibling_index;
        n->next_sibling_index = ref;
    }
}

StatNodeRef stat_add_node(StatNodeRef parent, const char *name, int visible)
{
    StatNodeRef ref;
    SpiceStatNode *node;

    ASSERT(name && strlen(name) > 0);
    if (strlen(name) >= sizeof(node->name)) {
        return INVALID_STAT_REF;
    }
    pthread_mutex_lock(&reds->stat_lock);
    ref = (parent == INVALID_STAT_REF ? reds->stat->root_index :
                                        reds->stat->nodes[parent].first_child_index);
    while (ref != INVALID_STAT_REF) {
        node = &reds->stat->nodes[ref];
        if (strcmp(name, node->name)) {
            ref = node->next_sibling_index;
        } else {
            pthread_mutex_unlock(&reds->stat_lock);
            return ref;
        }
    }
    if (reds->stat->num_of_nodes >= REDS_MAX_STAT_NODES || reds->stat == NULL) {
        pthread_mutex_unlock(&reds->stat_lock);
        return INVALID_STAT_REF;
    }
    reds->stat->generation++;
    reds->stat->num_of_nodes++;
    for (ref = 0; ref <= REDS_MAX_STAT_NODES; ref++) {
        node = &reds->stat->nodes[ref];
        if (!(node->flags & SPICE_STAT_NODE_FLAG_ENABLED)) {
            break;
        }
    }
    ASSERT(!(node->flags & SPICE_STAT_NODE_FLAG_ENABLED));
    node->value = 0;
    node->flags = SPICE_STAT_NODE_FLAG_ENABLED | (visible ? SPICE_STAT_NODE_FLAG_VISIBLE : 0);
    strncpy(node->name, name, sizeof(node->name));
    insert_stat_node(parent, ref);
    pthread_mutex_unlock(&reds->stat_lock);
    return ref;
}

void stat_remove(SpiceStatNode *node)
{
    pthread_mutex_lock(&reds->stat_lock);
    node->flags &= ~SPICE_STAT_NODE_FLAG_ENABLED;
    reds->stat->generation++;
    reds->stat->num_of_nodes--;
    pthread_mutex_unlock(&reds->stat_lock);
}

void stat_remove_node(StatNodeRef ref)
{
    stat_remove(&reds->stat->nodes[ref]);
}

uint64_t *stat_add_counter(StatNodeRef parent, const char *name, int visible)
{
    StatNodeRef ref = stat_add_node(parent, name, visible);
    SpiceStatNode *node;

    if (ref == INVALID_STAT_REF) {
        return NULL;
    }
    node = &reds->stat->nodes[ref];
    node->flags |= SPICE_STAT_NODE_FLAG_VALUE;
    return &node->value;
}

void stat_remove_counter(uint64_t *counter)
{
    stat_remove((SpiceStatNode *)(counter - offsetof(SpiceStatNode, value)));
}

static void reds_update_stat_value(RedsStatValue* stat_value, uint32_t value)
{
    stat_value->value = value;
    stat_value->min = (stat_value->count ? MIN(stat_value->min, value) : value);
    stat_value->max = MAX(stat_value->max, value);
    stat_value->average = (stat_value->average * stat_value->count + value) /
                          (stat_value->count + 1);
    stat_value->count++;
}

#endif

void reds_register_channel(Channel *channel)
{
    ASSERT(reds);
    channel->next = reds->channels;
    reds->channels = channel;
    reds->num_of_channels++;
}

void reds_unregister_channel(Channel *channel)
{
    Channel **now = &reds->channels;

    while (*now) {
        if (*now == channel) {
            *now = channel->next;
            reds->num_of_channels--;
            return;
        }
        now = &(*now)->next;
    }
    red_printf("not found");
}

static Channel *reds_find_channel(uint32_t type, uint32_t id)
{
    Channel *channel = reds->channels;
    while (channel && !(channel->type == type && channel->id == id)) {
        channel = channel->next;
    }
    return channel;
}

static void reds_shatdown_channels()
{
    Channel *channel = reds->channels;
    while (channel) {
        channel->shutdown(channel);
        channel = channel->next;
    }
}

static void reds_mig_cleanup()
{
    if (reds->mig_inprogress) {
        reds->mig_inprogress = FALSE;
        reds->mig_wait_connect = FALSE;
        reds->mig_wait_disconnect = FALSE;
        core->timer_cancel(reds->mig_timer);
    }
}

static void reds_reset_vdp()
{
    VDIPortState *state = &reds->agent_state;

    while (!ring_is_empty(&state->write_queue)) {
        VDIPortBuf *buf;
        RingItem *item;

        item = ring_get_tail(&state->write_queue);
        ring_remove(item);
        buf = (VDIPortBuf *)item;
        buf->free(buf);
    }
    state->read_state = VDI_PORT_READ_STATE_READ_HADER;
    state->recive_pos = (uint8_t *)&state->vdi_chunk_header;
    state->recive_len = sizeof(state->vdi_chunk_header);
    state->message_recive_len = 0;
    if (state->current_read_buf) {
        ring_add(&state->read_bufs, &state->current_read_buf->link);
        state->current_read_buf = NULL;
    }
    /* Reset read filter to start with clean state when the agent reconnects */
    agent_msg_filter_init(&state->read_filter, agent_copypaste, TRUE);
    /* Throw away pending chunks from the current (if any) and future
       messages written by the client */
    state->write_filter.result = AGENT_MSG_FILTER_DISCARD;
    state->write_filter.discard_all = TRUE;
}

static void reds_reset_outgoing()
{
    RedsOutgoingData *outgoing = &reds->outgoing;
    RingItem *ring_item;

    if (outgoing->item) {
        reds_out_item_free(outgoing->item);
        outgoing->item = NULL;
    }
    while ((ring_item = ring_get_tail(&outgoing->pipe))) {
        RedsOutItem *out_item = (RedsOutItem *)ring_item;
        ring_remove(ring_item);
        reds_out_item_free(out_item);
    }
    outgoing->vec_size = 0;
    outgoing->vec = outgoing->vec_buf;
}

static void reds_disconnect()
{
    if (!reds->stream || reds->disconnecting) {
        return;
    }

    red_printf("");
    reds->disconnecting = TRUE;
    reds_reset_outgoing();
    /* Reset write filter to start with clean state on client reconnect */
    agent_msg_filter_init(&reds->agent_state.write_filter, agent_copypaste,
                          TRUE);
    /* Throw away pending chunks from the current (if any) and future
       messages read from the agent */
    reds->agent_state.read_filter.result = AGENT_MSG_FILTER_DISCARD;
    reds->agent_state.read_filter.discard_all = TRUE;

    if (reds->agent_state.connected) {
        SpiceCharDeviceInterface *sif;
        sif = SPICE_CONTAINEROF(vdagent->base.sif, SpiceCharDeviceInterface, base);
        reds->agent_state.connected = 0;
        if (sif->state) {
            sif->state(vdagent, reds->agent_state.connected);
        }
    }

    reds_shatdown_channels();
    reds_stream_free(reds->stream);
    reds->stream = NULL;
    reds->in_handler.shut = TRUE;
    reds->link_id = 0;
    reds->serial = 0;
    reds->ping_id = 0;
    reds->net_test_id = 0;
    reds->net_test_stage = NET_TEST_STAGE_INVALID;
    reds->in_handler.end_pos = 0;

    bitrate_per_sec = ~0;
    latency = 0;

    reds_mig_cleanup();
    reds->disconnecting = FALSE;
}

static void reds_mig_disconnect()
{
    if (reds->stream) {
        reds_disconnect();
    } else {
        reds_mig_cleanup();
    }
}

static int handle_incoming(RedsStream *stream, IncomingHandler *handler)
{
    for (;;) {
        uint8_t *buf = handler->buf;
        uint32_t pos = handler->end_pos;
        uint8_t *end = buf + pos;
        SpiceDataHeader *header;
        int n;
        n = reds_stream_read(stream, buf + pos, RECIVE_BUF_SIZE - pos);
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

static int handle_outgoing(RedsStream *stream, OutgoingHandler *handler)
{
    if (!handler->length) {
        return 0;
    }

    while (handler->length) {
        int n;

        n = reds_stream_write(stream, handler->now, handler->length);
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
            handler->now += n;
            handler->length -= n;
        }
    }
    handler->select(handler->opaque, FALSE);
    handler->may_write(handler->opaque);
    return 0;
}

#define OUTGOING_OK 0
#define OUTGOING_FAILED -1
#define OUTGOING_BLOCKED 1

static int outgoing_write(RedsStream *stream, OutgoingHandler *handler, void *in_data,
                          int length)
{
    uint8_t *data = in_data;
    ASSERT(length <= SEND_BUF_SIZE);
    if (handler->length) {
        return OUTGOING_BLOCKED;
    }

    while (length) {
        int n = reds_stream_write(stream, data, length);
        if (n < 0) {
            switch (errno) {
            case EAGAIN:
                handler->length = length;
                memcpy(handler->buf, data, length);
                handler->select(handler->opaque, TRUE);
                return OUTGOING_OK;
            case EINTR:
                break;
            case EPIPE:
                return OUTGOING_FAILED;
            default:
                red_printf("%s", strerror(errno));
                return OUTGOING_FAILED;
            }
        } else {
            data += n;
            length -= n;
        }
    }
    return OUTGOING_OK;
}

static RedsOutItem *new_out_item(uint32_t type)
{
    RedsOutItem *item;

    item = spice_new(RedsOutItem, 1);
    ring_item_init(&item->link);

    item->m = spice_marshaller_new();
    item->header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(item->m, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(item->m, sizeof(SpiceDataHeader));

    item->header->serial = ++reds->serial;
    item->header->type = type;
    item->header->sub_list = 0;

    return item;
}

static void reds_out_item_free(RedsOutItem *item)
{
    spice_marshaller_destroy(item->m);
    free(item);
}

static void reds_push_pipe_item(RedsOutItem *item)
{
    ring_add(&reds->outgoing.pipe, &item->link);
    reds_push();
}

static void reds_send_channels()
{
    SpiceMsgChannels* channels_info;
    RedsOutItem *item;
    Channel *channel;
    int i;

    item = new_out_item(SPICE_MSG_MAIN_CHANNELS_LIST);
    channels_info = (SpiceMsgChannels *)spice_malloc(sizeof(SpiceMsgChannels) + reds->num_of_channels * sizeof(SpiceChannelId));
    channels_info->num_of_channels = reds->num_of_channels;
    channel = reds->channels;

    for (i = 0; i < reds->num_of_channels; i++) {
        ASSERT(channel);
        channels_info->channels[i].type = channel->type;
        channels_info->channels[i].id = channel->id;
        channel = channel->next;
    }
    spice_marshall_msg_main_channels_list(item->m, channels_info);
    free(channels_info);
    reds_push_pipe_item(item);
}

static int send_ping(int size)
{
    struct timespec time_space;
    RedsOutItem *item;
    SpiceMsgPing ping;

    if (!reds->stream) {
        return FALSE;
    }
    item = new_out_item(SPICE_MSG_PING);
    ping.id = ++reds->ping_id;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    ping.timestamp = time_space.tv_sec * 1000000LL + time_space.tv_nsec / 1000LL;
    spice_marshall_msg_ping(item->m, &ping);

    while (size > 0) {
        int now = MIN(ZERO_BUF_SIZE, size);
        size -= now;
        spice_marshaller_add_ref(item->m, zero_page, now);
    }

    reds_push_pipe_item(item);

    return TRUE;
}

#ifdef RED_STATISTICS

static void do_ping_client(const char *opt, int has_interval, int interval)
{
    if (!reds->stream) {
        red_printf("not connected to stream");
        return;
    }

    if (!opt) {
        send_ping(0);
    } else if (!strcmp(opt, "on")) {
        if (has_interval && interval > 0) {
            reds->ping_interval = interval * 1000;
        }
        core->timer_start(reds->ping_timer, reds->ping_interval);
    } else if (!strcmp(opt, "off")) {
        core->timer_cancel(reds->ping_timer);
    } else {
        return;
    }
}

static void ping_timer_cb()
{
    if (!reds->stream) {
        red_printf("not connected to stream, ping off");
        core->timer_cancel(reds->ping_timer);
        return;
    }
    do_ping_client(NULL, 0, 0);
    core->timer_start(reds->ping_timer, reds->ping_interval);
}

#endif

static void reds_send_mouse_mode()
{
    SpiceMsgMainMouseMode mouse_mode;
    RedsOutItem *item;

    if (!reds->stream) {
        return;
    }

    item = new_out_item(SPICE_MSG_MAIN_MOUSE_MODE);
    mouse_mode.supported_modes = SPICE_MOUSE_MODE_SERVER;
    if (reds->is_client_mouse_allowed) {
        mouse_mode.supported_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    mouse_mode.current_mode = reds->mouse_mode;

    spice_marshall_msg_main_mouse_mode(item->m, &mouse_mode);

    reds_push_pipe_item(item);
}

static void reds_set_mouse_mode(uint32_t mode)
{
    if (reds->mouse_mode == mode) {
        return;
    }
    reds->mouse_mode = mode;
    red_dispatcher_set_mouse_mode(reds->mouse_mode);
    reds_send_mouse_mode();
}

static void reds_update_mouse_mode()
{
    int allowed = 0;
    int qxl_count = red_dispatcher_qxl_count();

    if ((agent_mouse && vdagent) || (tablet && qxl_count == 1)) {
        allowed = reds->dispatcher_allows_client_mouse;
    }
    if (allowed == reds->is_client_mouse_allowed) {
        return;
    }
    reds->is_client_mouse_allowed = allowed;
    if (reds->mouse_mode == SPICE_MOUSE_MODE_CLIENT && !allowed) {
        reds_set_mouse_mode(SPICE_MOUSE_MODE_SERVER);
        return;
    }
    reds_send_mouse_mode();
}

static void reds_send_agent_connected()
{
    RedsOutItem *item;

    item = new_out_item(SPICE_MSG_MAIN_AGENT_CONNECTED);
    reds_push_pipe_item(item);
}

static void reds_send_agent_disconnected()
{
    SpiceMsgMainAgentDisconnect disconnect;
    RedsOutItem *item;

    item = new_out_item(SPICE_MSG_MAIN_AGENT_DISCONNECTED);
    disconnect.error_code = SPICE_LINK_ERR_OK;
    spice_marshall_msg_main_agent_disconnected(item->m, &disconnect);
    reds_push_pipe_item(item);
}

static void reds_agent_remove()
{
    SpiceCharDeviceInstance *sin = vdagent;
    SpiceCharDeviceInterface *sif;

    if (!reds->mig_target) {
        reds_reset_vdp();
    }

    reds->agent_state.connected = 0;
    vdagent = NULL;
    reds_update_mouse_mode();
    if (!reds->stream || !sin) {
        return;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);
    if (sif->state) {
        sif->state(sin, reds->agent_state.connected);
    }

    if (reds->mig_target) {
        return;
    }
    reds_send_agent_disconnected();
}

static void reds_send_tokens()
{
    SpiceMsgMainAgentTokens tokens;
    RedsOutItem *item;

    if (!reds->stream) {
        return;
    }

    item = new_out_item(SPICE_MSG_MAIN_AGENT_TOKEN);
    tokens.num_tokens = reds->agent_state.num_tokens;
    reds->agent_state.num_client_tokens += tokens.num_tokens;
    ASSERT(reds->agent_state.num_client_tokens <= REDS_AGENT_WINDOW_SIZE);
    reds->agent_state.num_tokens = 0;

    spice_marshall_msg_main_agent_token(item->m, &tokens);

    reds_push_pipe_item(item);
}

static int write_to_vdi_port();

static void vdi_port_write_timer_start()
{
    if (reds->vdi_port_write_timer_started) {
        return;
    }
    reds->vdi_port_write_timer_started = TRUE;
    core->timer_start(reds->vdi_port_write_timer,
                      VDI_PORT_WRITE_RETRY_TIMEOUT);
}

static void vdi_port_write_retry()
{
    reds->vdi_port_write_timer_started = FALSE;
    write_to_vdi_port();
}

static int write_to_vdi_port()
{
    VDIPortState *state = &reds->agent_state;
    SpiceCharDeviceInterface *sif;
    RingItem *ring_item;
    VDIPortBuf *buf;
    int total = 0;
    int n;

    if (!vdagent || reds->mig_target) {
        return 0;
    }

    sif = SPICE_CONTAINEROF(vdagent->base.sif, SpiceCharDeviceInterface, base);
    while (vdagent) {
        if (!(ring_item = ring_get_tail(&state->write_queue))) {
            break;
        }
        buf = (VDIPortBuf *)ring_item;
        n = sif->write(vdagent, buf->now, buf->write_len);
        if (n == 0) {
            break;
        }
        total += n;
        buf->write_len -= n;
        if (!buf->write_len) {
            ring_remove(ring_item);
            buf->free(buf);
            continue;
        }
        buf->now += n;
    }
    // Workaround for lack of proper sif write_possible callback (RHBZ 616772)
    if (ring_item != NULL) {
        vdi_port_write_timer_start();
    }
    return total;
}

static int read_from_vdi_port(void);

void vdi_read_buf_release(uint8_t *data, void *opaque)
{
    VDIReadBuf *buf = (VDIReadBuf *)opaque;

    ring_add(&reds->agent_state.read_bufs, &buf->link);
    /* read_from_vdi_port() may have never completed because the read_bufs
       ring was empty. So we call it again so it can complete its work if
       necessary. Note since we can be called from read_from_vdi_port ourselves
       this can cause recursion, read_from_vdi_port() contains code protecting
       it against this. */
    while (read_from_vdi_port());
}

static void dispatch_vdi_port_data(int port, VDIReadBuf *buf)
{
    VDIPortState *state = &reds->agent_state;
    RedsOutItem *item;
    int res;

    switch (port) {
    case VDP_CLIENT_PORT: {
        res = agent_msg_filter_process_data(&state->read_filter,
                                            buf->data, buf->len);
        switch (res) {
        case AGENT_MSG_FILTER_OK:
            break;
        case AGENT_MSG_FILTER_DISCARD:
            ring_add(&state->read_bufs, &buf->link);
            return;
        case AGENT_MSG_FILTER_PROTO_ERROR:
            ring_add(&state->read_bufs, &buf->link);
            reds_agent_remove();
            return;
        }
        item = new_out_item(SPICE_MSG_MAIN_AGENT_DATA);
        spice_marshaller_add_ref_full(item->m, buf->data, buf->len,
                                      vdi_read_buf_release, buf);
        reds_push_pipe_item(item);
        break;
    }
    case VDP_SERVER_PORT:
        ring_add(&state->read_bufs, &buf->link);
        break;
    default:
        ring_add(&state->read_bufs, &buf->link);
        red_printf("invalid port");
        reds_agent_remove();
    }
}

/* Note this function MUST always be called in a while loop until it
   returns 0. This is needed because it can cause new data available events
   and its recursion protection causes those to get lost. Calling it until
   it returns 0 ensures that all data has been consumed. */
static int read_from_vdi_port(void)
{
    /* There are 2 scenarios where we can get called recursively:
       1) spice-vmc vmc_read triggering flush of throttled data, recalling us
       2) the buf we push to the client may be send immediately without
          blocking, in which case its free function will recall us
       This messes up the state machine, so ignore recursive calls.
       This is why we always must be called in a loop. */
    static int inside_call = 0;
    int quit_loop = 0;
    VDIPortState *state = &reds->agent_state;
    SpiceCharDeviceInterface *sif;
    VDIReadBuf *dispatch_buf;
    int total = 0;
    int n;

    if (inside_call) {
        return 0;
    }
    inside_call = 1;

    if (reds->mig_target || !vdagent) {
        // discard data only if we are migrating or vdagent has not been
        // initialized.
        inside_call = 0;
        return 0;
    }

    sif = SPICE_CONTAINEROF(vdagent->base.sif, SpiceCharDeviceInterface, base);
    while (!quit_loop && vdagent) {
        switch (state->read_state) {
        case VDI_PORT_READ_STATE_READ_HADER:
            n = sif->read(vdagent, state->recive_pos, state->recive_len);
            if (!n) {
                quit_loop = 1;
                break;
            }
            total += n;
            if ((state->recive_len -= n)) {
                state->recive_pos += n;
                quit_loop = 1;
                break;
            }
            state->message_recive_len = state->vdi_chunk_header.size;
            state->read_state = VDI_PORT_READ_STATE_GET_BUFF;
        case VDI_PORT_READ_STATE_GET_BUFF: {
            RingItem *item;

            if (!(item = ring_get_head(&state->read_bufs))) {
                quit_loop = 1;
                break;
            }

            ring_remove(item);
            state->current_read_buf = (VDIReadBuf *)item;
            state->recive_pos = state->current_read_buf->data;
            state->recive_len = MIN(state->message_recive_len,
                                    sizeof(state->current_read_buf->data));
            state->current_read_buf->len = state->recive_len;
            state->message_recive_len -= state->recive_len;
            state->read_state = VDI_PORT_READ_STATE_READ_DATA;
        }
        case VDI_PORT_READ_STATE_READ_DATA:
            n = sif->read(vdagent, state->recive_pos, state->recive_len);
            if (!n) {
                quit_loop = 1;
                break;
            }
            total += n;
            if ((state->recive_len -= n)) {
                state->recive_pos += n;
                break;
            }
            dispatch_buf = state->current_read_buf;
            state->current_read_buf = NULL;
            state->recive_pos = NULL;
            if (state->message_recive_len == 0) {
                state->read_state = VDI_PORT_READ_STATE_READ_HADER;
                state->recive_pos = (uint8_t *)&state->vdi_chunk_header;
                state->recive_len = sizeof(state->vdi_chunk_header);
            } else {
                state->read_state = VDI_PORT_READ_STATE_GET_BUFF;
            }
            dispatch_vdi_port_data(state->vdi_chunk_header.port, dispatch_buf);
        }
    }
    inside_call = 0;
    return total;
}

void vdagent_char_device_wakeup(SpiceCharDeviceInstance *sin)
{
    while (read_from_vdi_port());
}

static void reds_handle_agent_mouse_event()
{
    RingItem *ring_item;
    VDInternalBuf *buf;

    if (!reds->inputs_state) {
        return;
    }
    if (reds->mig_target || !(ring_item = ring_get_head(&reds->agent_state.internal_bufs))) {
        reds->inputs_state->pending_mouse_event = TRUE;
        vdi_port_write_timer_start();
        return;
    }
    reds->inputs_state->pending_mouse_event = FALSE;
    ring_remove(ring_item);
    buf = (VDInternalBuf *)ring_item;
    buf->base.now = (uint8_t *)&buf->base.chunk_header;
    buf->base.write_len = sizeof(VDIChunkHeader) + sizeof(VDAgentMessage) +
                          sizeof(VDAgentMouseState);
    buf->u.mouse_state = reds->inputs_state->mouse_state;
    ring_add(&reds->agent_state.write_queue, &buf->base.link);
    write_to_vdi_port();
}

static void add_token()
{
    VDIPortState *state = &reds->agent_state;

    if (++state->num_tokens == REDS_TOKENS_TO_SEND) {
        reds_send_tokens();
    }
}

typedef struct MainMigrateData {
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
} MainMigrateData;

#define MAIN_CHANNEL_MIG_DATA_VERSION 1

typedef struct WriteQueueInfo {
    uint32_t port;
    uint32_t len;
} WriteQueueInfo;

static void main_channel_push_migrate_data_item()
{
    RedsOutItem *item;
    MainMigrateData *data;
    VDIPortState *state = &reds->agent_state;
    int buf_index;
    RingItem *now;

    item = new_out_item(SPICE_MSG_MIGRATE_DATA);

    data = (MainMigrateData *)spice_marshaller_reserve_space(item->m, sizeof(MainMigrateData));
    data->version = MAIN_CHANNEL_MIG_DATA_VERSION;
    data->serial = reds->serial;
    data->ping_id = reds->ping_id;

    data->agent_connected = !!state->connected;
    data->client_agent_started = !state->write_filter.discard_all;
    data->num_client_tokens = state->num_client_tokens;
    data->send_tokens = ~0;

    data->read_state = state->read_state;
    data->vdi_chunk_header = state->vdi_chunk_header;
    data->recive_len = state->recive_len;
    data->message_recive_len = state->message_recive_len;

    if (state->current_read_buf) {
        data->read_buf_len = state->current_read_buf->len;

        if (data->read_buf_len - data->recive_len) {
            spice_marshaller_add_ref(item->m,
                                     state->current_read_buf->data,
                                     data->read_buf_len - data->recive_len);
        }
    } else {
        data->read_buf_len = 0;
    }

    now = &state->write_queue;
    data->write_queue_size = 0;
    while ((now = ring_prev(&state->write_queue, now))) {
        data->write_queue_size++;
    }
    if (data->write_queue_size) {
        WriteQueueInfo *queue_info;

        queue_info = (WriteQueueInfo *)
            spice_marshaller_reserve_space(item->m,
                                           data->write_queue_size * sizeof(queue_info[0]));

        buf_index = 0;
        now = &state->write_queue;
        while ((now = ring_prev(&state->write_queue, now))) {
            VDIPortBuf *buf = (VDIPortBuf *)now;
            queue_info[buf_index].port = buf->chunk_header.port;
            queue_info[buf_index++].len = buf->write_len;
            spice_marshaller_add_ref(item->m, buf->now, buf->write_len);
        }
    }

    reds_push_pipe_item((RedsOutItem *)item);
}


static int main_channel_restore_vdi_read_state(MainMigrateData *data, uint8_t **in_pos,
                                               uint8_t *end)
{
    VDIPortState *state = &reds->agent_state;
    uint8_t *pos = *in_pos;
    RingItem *ring_item;

    state->read_state = data->read_state;
    state->vdi_chunk_header = data->vdi_chunk_header;
    state->recive_len = data->recive_len;
    state->message_recive_len = data->message_recive_len;

    switch (state->read_state) {
    case VDI_PORT_READ_STATE_READ_HADER:
        if (data->read_buf_len) {
            red_printf("unexpected receive buf");
            reds_disconnect();
            return FALSE;
        }
        state->recive_pos = (uint8_t *)(&state->vdi_chunk_header + 1) - state->recive_len;
        break;
    case VDI_PORT_READ_STATE_GET_BUFF:
        if (state->message_recive_len > state->vdi_chunk_header.size) {
            red_printf("invalid message receive len");
            reds_disconnect();
            return FALSE;
        }

        if (data->read_buf_len) {
            red_printf("unexpected receive buf");
            reds_disconnect();
            return FALSE;
        }
        break;
    case VDI_PORT_READ_STATE_READ_DATA: {
        VDIReadBuf *buff;
        uint32_t n;

        if (!data->read_buf_len) {
            red_printf("read state and read_buf_len == 0");
            reds_disconnect();
            return FALSE;
        }

        if (state->message_recive_len > state->vdi_chunk_header.size) {
            red_printf("invalid message receive len");
            reds_disconnect();
            return FALSE;
        }


        if (!(ring_item = ring_get_head(&state->read_bufs))) {
            red_printf("get read buf failed");
            reds_disconnect();
            return FALSE;
        }

        ring_remove(ring_item);
        buff = state->current_read_buf = (VDIReadBuf *)ring_item;
        buff->len = data->read_buf_len;
        n = buff->len - state->recive_len;
        if (buff->len > SPICE_AGENT_MAX_DATA_SIZE || n > SPICE_AGENT_MAX_DATA_SIZE) {
            red_printf("bad read position");
            reds_disconnect();
            return FALSE;
        }
        memcpy(buff->data, pos, n);
        pos += n;
        state->recive_pos = buff->data + n;
        break;
    }
    default:
        red_printf("invalid read state");
        reds_disconnect();
        return FALSE;
    }
    *in_pos = pos;
    return TRUE;
}

static void free_tmp_internal_buf(VDIPortBuf *buf)
{
    free(buf);
}

static int main_channel_restore_vdi_wqueue(MainMigrateData *data, uint8_t *pos, uint8_t *end)
{
    VDIPortState *state = &reds->agent_state;
    WriteQueueInfo *inf;
    WriteQueueInfo *inf_end;
    RingItem *ring_item;

    if (!data->write_queue_size) {
        return TRUE;
    }

    inf = (WriteQueueInfo *)pos;
    inf_end = inf + data->write_queue_size;
    pos = (uint8_t *)inf_end;
    if (pos > end) {
        red_printf("access violation");
        reds_disconnect();
        return FALSE;
    }

    for (; inf < inf_end; inf++) {
        if (pos + inf->len > end) {
            red_printf("access violation");
            reds_disconnect();
            return FALSE;
        }
        if (inf->port == VDP_SERVER_PORT) {
            VDInternalBuf *buf;

            if (inf->len > sizeof(*buf) - SPICE_OFFSETOF(VDInternalBuf, header)) {
                red_printf("bad buffer len");
                reds_disconnect();
                return FALSE;
            }
            buf = spice_new(VDInternalBuf, 1);
            ring_item_init(&buf->base.link);
            buf->base.free = free_tmp_internal_buf;
            buf->base.now = (uint8_t *)&buf->base.chunk_header;
            buf->base.write_len = inf->len;
            memcpy(buf->base.now, pos, buf->base.write_len);
            ring_add(&reds->agent_state.write_queue, &buf->base.link);
        } else if (inf->port == VDP_CLIENT_PORT) {
            VDAgentExtBuf *buf;

            state->num_tokens--;
            if (inf->len > sizeof(*buf) - SPICE_OFFSETOF(VDAgentExtBuf, buf)) {
                red_printf("bad buffer len");
                reds_disconnect();
                return FALSE;
            }
            if (!(ring_item = ring_get_head(&reds->agent_state.external_bufs))) {
                red_printf("no external buff");
                reds_disconnect();
                return FALSE;
            }
            ring_remove(ring_item);
            buf = (VDAgentExtBuf *)ring_item;
            memcpy(&buf->buf, pos, inf->len);
            buf->base.now = (uint8_t *)buf->buf;
            buf->base.write_len = inf->len;
            ring_add(&reds->agent_state.write_queue, &buf->base.link);
        } else {
            red_printf("invalid data");
            reds_disconnect();
            return FALSE;
        }
        pos += inf->len;
    }
    return TRUE;
}

static void main_channel_recive_migrate_data(MainMigrateData *data, uint8_t *end)
{
    VDIPortState *state = &reds->agent_state;
    uint8_t *pos;

    if (data->version != MAIN_CHANNEL_MIG_DATA_VERSION) {
        red_printf("version mismatch");
        reds_disconnect();
        return;
    }

    reds->serial = data->serial;
    reds->ping_id = data->ping_id;

    state->num_client_tokens = data->num_client_tokens;
    ASSERT(state->num_client_tokens + data->write_queue_size <= REDS_AGENT_WINDOW_SIZE +
                                                                REDS_NUM_INTERNAL_AGENT_MESSAGES);
    state->num_tokens = REDS_AGENT_WINDOW_SIZE - state->num_client_tokens;

    if (!data->agent_connected) {
        if (state->connected) {
            reds_send_agent_connected();
        }
        return;
    }

    if (!state->connected) {
        reds_send_agent_disconnected();
        return;
    }

    if (state->plug_generation > 1) {
        reds_send_agent_disconnected();
        reds_send_agent_connected();
        return;
    }

    state->write_filter.discard_all = !data->client_agent_started;

    pos = (uint8_t *)(data + 1);

    if (!main_channel_restore_vdi_read_state(data, &pos, end)) {
        return;
    }

    main_channel_restore_vdi_wqueue(data, pos, end);
    ASSERT(state->num_client_tokens + state->num_tokens == REDS_AGENT_WINDOW_SIZE);
}

static void reds_main_handle_message(void *opaque, size_t size, uint32_t type, void *message)
{
    int res;

    switch (type) {
    case SPICE_MSGC_MAIN_AGENT_START:
        red_printf("agent start");
        if (!reds->stream || !vdagent) {
            return;
        }
        reds->agent_state.write_filter.discard_all = FALSE;
        break;
    case SPICE_MSGC_MAIN_AGENT_DATA: {
        RingItem *ring_item;
        VDAgentExtBuf *buf;

        if (!reds->agent_state.num_client_tokens) {
            red_printf("token violation");
            reds_disconnect();
            break;
        }
        --reds->agent_state.num_client_tokens;

        res = agent_msg_filter_process_data(&reds->agent_state.write_filter,
                                            message, size);
        switch (res) {
        case AGENT_MSG_FILTER_OK:
            break;
        case AGENT_MSG_FILTER_DISCARD:
            add_token();
            return;
        case AGENT_MSG_FILTER_PROTO_ERROR:
            reds_disconnect();
            return;
        }

        if (!(ring_item = ring_get_head(&reds->agent_state.external_bufs))) {
            red_printf("no agent free bufs");
            reds_disconnect();
            break;
        }
        ring_remove(ring_item);
        buf = (VDAgentExtBuf *)ring_item;
        buf->base.now = (uint8_t *)&buf->base.chunk_header.port;
        buf->base.write_len = size + sizeof(VDIChunkHeader);
        buf->base.chunk_header.size = size;
        memcpy(buf->buf, message, size);
        ring_add(&reds->agent_state.write_queue, ring_item);
        write_to_vdi_port();
        break;
    }
    case SPICE_MSGC_MAIN_AGENT_TOKEN:
        break;
    case SPICE_MSGC_MAIN_ATTACH_CHANNELS:
        reds_send_channels();
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECTED:
        red_printf("connected");
        if (reds->mig_wait_connect) {
            reds_mig_cleanup();
        }
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECT_ERROR:
        red_printf("mig connect error");
        if (reds->mig_wait_connect) {
            reds_mig_cleanup();
        }
        break;
    case SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST: {
        switch (((SpiceMsgcMainMouseModeRequest *)message)->mode) {
        case SPICE_MOUSE_MODE_CLIENT:
            if (reds->is_client_mouse_allowed) {
                reds_set_mouse_mode(SPICE_MOUSE_MODE_CLIENT);
            } else {
                red_printf("client mouse is disabled");
            }
            break;
        case SPICE_MOUSE_MODE_SERVER:
            reds_set_mouse_mode(SPICE_MOUSE_MODE_SERVER);
            break;
        default:
            red_printf("unsupported mouse mode");
        }
        break;
    }
    case SPICE_MSGC_PONG: {
        SpiceMsgPing *ping = (SpiceMsgPing *)message;
        uint64_t roundtrip;
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        roundtrip = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL - ping->timestamp;

        if (ping->id == reds->net_test_id) {
            switch (reds->net_test_stage) {
            case NET_TEST_STAGE_WARMUP:
                reds->net_test_id++;
                reds->net_test_stage = NET_TEST_STAGE_LATENCY;
                break;
            case NET_TEST_STAGE_LATENCY:
                reds->net_test_id++;
                reds->net_test_stage = NET_TEST_STAGE_RATE;
                latency = roundtrip;
                break;
            case NET_TEST_STAGE_RATE:
                reds->net_test_id = 0;
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
                reds->net_test_stage = NET_TEST_STAGE_INVALID;
                break;
            default:
                red_printf("invalid net test stage, ping id %d test id %d stage %d",
                           ping->id,
                           reds->net_test_id,
                           reds->net_test_stage);
            }
            break;
        }
#ifdef RED_STATISTICS
        reds_update_stat_value(&reds->roundtrip_stat, roundtrip);
#endif
        break;
    }
    case SPICE_MSGC_MIGRATE_FLUSH_MARK:
        main_channel_push_migrate_data_item();
        break;
    case SPICE_MSGC_MIGRATE_DATA:
        main_channel_recive_migrate_data((MainMigrateData *)message,
                                         ((uint8_t *)message) + size);
        reds->mig_target = FALSE;
        while (write_to_vdi_port() || read_from_vdi_port());
        break;
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        red_printf("unexpected type %d", type);
    }
}

static int reds_send_data()
{
    RedsOutgoingData *outgoing = &reds->outgoing;
    int n;

    if (!outgoing->item) {
        return TRUE;
    }

    ASSERT(outgoing->vec_size);
    for (;;) {
        if ((n = reds_stream_writev(reds->stream, outgoing->vec, outgoing->vec_size)) == -1) {
            switch (errno) {
            case EAGAIN:
                core->watch_update_mask(reds->stream->watch,
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
            outgoing->vec = reds_iovec_skip(outgoing->vec, n, &outgoing->vec_size);
            if (!outgoing->vec_size) {
                reds_out_item_free(outgoing->item);
                outgoing->item = NULL;
                outgoing->vec = outgoing->vec_buf;
                return TRUE;
            }
        }
    }
}

static void reds_push()
{
    RedsOutgoingData *outgoing = &reds->outgoing;
    RingItem *ring_item;
    RedsOutItem *item;

    for (;;) {
        if (!reds->stream || outgoing->item || !(ring_item = ring_get_tail(&outgoing->pipe))) {
            return;
        }
        ring_remove(ring_item);
        outgoing->item = item = (RedsOutItem *)ring_item;

        spice_marshaller_flush(item->m);
        item->header->size = spice_marshaller_get_total_size(item->m) - sizeof(SpiceDataHeader);

        outgoing->vec_size = spice_marshaller_fill_iovec(item->m,
                                                         outgoing->vec_buf,
                                                         REDS_MAX_SEND_IOVEC, 0);
        reds_send_data();
    }
}

static void reds_main_event(int fd, int event, void *data)
{
    if (event & SPICE_WATCH_EVENT_READ) {
        if (handle_incoming(reds->stream, &reds->in_handler)) {
            reds_disconnect();
        }
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        RedsOutgoingData *outgoing = &reds->outgoing;
        if (reds_send_data()) {
            reds_push();
            if (!outgoing->item && reds->stream) {
                core->watch_update_mask(reds->stream->watch,
                                        SPICE_WATCH_EVENT_READ);
            }
        }
    }
}

static int sync_write(RedsStream *stream, const void *in_buf, size_t n)
{
    const uint8_t *buf = (uint8_t *)in_buf;
    while (n) {
        int now = reds_stream_write(stream, buf, n);
        if (now <= 0) {
            if (now == -1 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            return FALSE;
        }
        n -= now;
        buf += now;
    }
    return TRUE;
}

static void reds_channel_set_common_caps(Channel *channel, int cap, int active)
{
    int nbefore, n;

    nbefore = channel->num_common_caps;
    n = cap / 32;
    channel->num_common_caps = MAX(channel->num_common_caps, n + 1);
    channel->common_caps = spice_renew(uint32_t, channel->common_caps, channel->num_common_caps);
    memset(channel->common_caps + nbefore, 0,
           (channel->num_common_caps - nbefore) * sizeof(uint32_t));
    if (active) {
        channel->common_caps[n] |= (1 << cap);
    } else {
        channel->common_caps[n] &= ~(1 << cap);
    }
}

void reds_channel_init_auth_caps(Channel *channel)
{
    if (sasl_enabled) {
        reds_channel_set_common_caps(channel, SPICE_COMMON_CAP_AUTH_SASL, TRUE);
    } else {
        reds_channel_set_common_caps(channel, SPICE_COMMON_CAP_AUTH_SPICE, TRUE);
    }
    reds_channel_set_common_caps(channel, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION, TRUE);
}

void reds_channel_dispose(Channel *channel)
{
    free(channel->caps);
    channel->caps = NULL;
    channel->num_caps = 0;

    free(channel->common_caps);
    channel->common_caps = NULL;
    channel->num_common_caps = 0;
}

static int reds_send_link_ack(RedLinkInfo *link)
{
    SpiceLinkHeader header;
    SpiceLinkReply ack;
    Channel caps = { 0, };
    Channel *channel;
    BUF_MEM *bmBuf;
    BIO *bio;
    int ret = FALSE;

    header.magic = SPICE_MAGIC;
    header.size = sizeof(ack);
    header.major_version = SPICE_VERSION_MAJOR;
    header.minor_version = SPICE_VERSION_MINOR;

    ack.error = SPICE_LINK_ERR_OK;

    channel = reds_find_channel(link->link_mess->channel_type, 0);
    if (!channel) {
        channel = &caps;
    }

    reds_channel_init_auth_caps(channel); /* make sure common caps are set */

    ack.num_common_caps = channel->num_common_caps;
    ack.num_channel_caps = channel->num_caps;
    header.size += (ack.num_common_caps + ack.num_channel_caps) * sizeof(uint32_t);
    ack.caps_offset = sizeof(SpiceLinkReply);

    if (!(link->tiTicketing.rsa = RSA_new())) {
        red_printf("RSA nes failed");
        return FALSE;
    }

    if (!(bio = BIO_new(BIO_s_mem()))) {
        red_printf("BIO new failed");
        return FALSE;
    }

    RSA_generate_key_ex(link->tiTicketing.rsa, SPICE_TICKET_KEY_PAIR_LENGTH, link->tiTicketing.bn,
                        NULL);
    link->tiTicketing.rsa_size = RSA_size(link->tiTicketing.rsa);

    i2d_RSA_PUBKEY_bio(bio, link->tiTicketing.rsa);
    BIO_get_mem_ptr(bio, &bmBuf);
    memcpy(ack.pub_key, bmBuf->data, sizeof(ack.pub_key));


    if (!sync_write(link->stream, &header, sizeof(header)))
        goto end;
    if (!sync_write(link->stream, &ack, sizeof(ack)))
        goto end;
    if (!sync_write(link->stream, channel->common_caps, channel->num_common_caps * sizeof(uint32_t)))
        goto end;
    if (!sync_write(link->stream, channel->caps, channel->num_caps * sizeof(uint32_t)))
        goto end;

    ret = TRUE;

end:
    reds_channel_dispose(&caps);
    BIO_free(bio);
    return ret;
}

static int reds_send_link_error(RedLinkInfo *link, uint32_t error)
{
    SpiceLinkHeader header;
    SpiceLinkReply reply;

    header.magic = SPICE_MAGIC;
    header.size = sizeof(reply);
    header.major_version = SPICE_VERSION_MAJOR;
    header.minor_version = SPICE_VERSION_MINOR;
    memset(&reply, 0, sizeof(reply));
    reply.error = error;
    return sync_write(link->stream, &header, sizeof(header)) && sync_write(link->stream, &reply,
                                                                           sizeof(reply));
}

static void reds_show_new_channel(RedLinkInfo *link, int connection_id)
{
    red_printf("channel %d:%d, connected successfully, over %s link",
               link->link_mess->channel_type,
               link->link_mess->channel_id,
               link->stream->ssl == NULL ? "Non Secure" : "Secure");
    /* add info + send event */
    if (link->stream->ssl) {
        link->stream->info.flags |= SPICE_CHANNEL_EVENT_FLAG_TLS;
    }
    link->stream->info.connection_id = connection_id;
    link->stream->info.type = link->link_mess->channel_type;
    link->stream->info.id   = link->link_mess->channel_id;
    reds_channel_event(link->stream, SPICE_CHANNEL_EVENT_INITIALIZED);
}

static void reds_send_link_result(RedLinkInfo *link, uint32_t error)
{
    sync_write(link->stream, &error, sizeof(error));
}

static void reds_start_net_test()
{
    if (!reds->stream || reds->net_test_id) {
        return;
    }

    if (send_ping(NET_TEST_WARMUP_BYTES) && send_ping(0) && send_ping(NET_TEST_BYTES)) {
        reds->net_test_id = reds->ping_id - 2;
        reds->net_test_stage = NET_TEST_STAGE_WARMUP;
    }
}

static void reds_handle_main_link(RedLinkInfo *link)
{
    uint32_t connection_id;

    red_printf("");

    reds_disconnect();

    if (!link->link_mess->connection_id) {
        reds_send_link_result(link, SPICE_LINK_ERR_OK);
        while((connection_id = rand()) == 0);
        reds->agent_state.num_tokens = 0;
        memcpy(&(reds->taTicket), &taTicket, sizeof(reds->taTicket));
        reds->mig_target = FALSE;
    } else {
        if (link->link_mess->connection_id != reds->link_id) {
            reds_send_link_result(link, SPICE_LINK_ERR_BAD_CONNECTION_ID);
            reds_link_free(link);
            return;
        }
        reds_send_link_result(link, SPICE_LINK_ERR_OK);
        connection_id = link->link_mess->connection_id;
        reds->mig_target = TRUE;
    }

    reds->link_id = connection_id;
    reds->mig_inprogress = FALSE;
    reds->mig_wait_connect = FALSE;
    reds->mig_wait_disconnect = FALSE;
    reds->stream = link->stream;
    reds->in_handler.shut = FALSE;

    reds_show_new_channel(link, connection_id);
    reds_stream_remove_watch(link->stream);
    link->stream = NULL;
    link->link_mess = NULL;
    reds_link_free(link);
    if (vdagent) {
        SpiceCharDeviceInterface *sif;
        sif = SPICE_CONTAINEROF(vdagent->base.sif, SpiceCharDeviceInterface, base);
        reds->agent_state.connected = 1;
        reds->agent_state.read_filter.discard_all = FALSE;
        if (sif->state) {
            sif->state(vdagent, reds->agent_state.connected);
        }
        reds->agent_state.plug_generation++;
    }
    reds->stream->watch = core->watch_add(reds->stream->socket,
                                        SPICE_WATCH_EVENT_READ,
                                        reds_main_event, NULL);

    if (!reds->mig_target) {
        RedsOutItem *item;
        SpiceMsgMainInit init;

        item = new_out_item(SPICE_MSG_MAIN_INIT);
        init.session_id = connection_id;
        init.display_channels_hint = red_dispatcher_count();
        init.current_mouse_mode = reds->mouse_mode;
        init.supported_mouse_modes = SPICE_MOUSE_MODE_SERVER;
        if (reds->is_client_mouse_allowed) {
            init.supported_mouse_modes |= SPICE_MOUSE_MODE_CLIENT;
        }
        init.agent_connected = !!vdagent;
        init.agent_tokens = REDS_AGENT_WINDOW_SIZE;
        reds->agent_state.num_client_tokens = REDS_AGENT_WINDOW_SIZE;
        init.multi_media_time = reds_get_mm_time() - MM_TIME_DELTA;
        init.ram_hint = red_dispatcher_qxl_ram_size();

        spice_marshall_msg_main_init(item->m, &init);

        reds_push_pipe_item(item);
        reds_start_net_test();
        /* Now that we have a client, forward any pending agent data */
        while (read_from_vdi_port());
    }
}

#define RED_MOUSE_STATE_TO_LOCAL(state)     \
    ((state & SPICE_MOUSE_BUTTON_MASK_LEFT) |          \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) << 1) |   \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) >> 1))

#define RED_MOUSE_BUTTON_STATE_TO_AGENT(state)                      \
    (((state & SPICE_MOUSE_BUTTON_MASK_LEFT) ? VD_AGENT_LBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) ? VD_AGENT_MBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) ? VD_AGENT_RBUTTON_MASK : 0))

static void activate_modifiers_watch()
{
    core->timer_start(reds->key_modifiers_timer, KEY_MODIFIERS_TTL);
}

static void kbd_push_scan(SpiceKbdInstance *sin, uint8_t scan)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);
    sif->push_scan_freg(sin, scan);
}

static uint8_t kbd_get_leds(SpiceKbdInstance *sin)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return 0;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);
    return sif->get_leds(sin);
}

static SpiceMarshaller *marshaller_new_for_outgoing(InputsState *state, int type)
{
    SpiceMarshaller *m;
    SpiceDataHeader *header;

    m = spice_marshaller_new();
    header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(m, sizeof(SpiceDataHeader));
    header->serial = ++state->serial;
    header->type = type;
    header->sub_list = 0;

    return m;
}

static int marshaller_outgoing_write(SpiceMarshaller *m,
                                     InputsState *state)
{
    SpiceDataHeader *header = (SpiceDataHeader *)spice_marshaller_get_ptr(m);
    uint8_t *data;
    size_t len;
    int free_data;

    spice_marshaller_flush(m);
    header->size = spice_marshaller_get_total_size(m) - sizeof(SpiceDataHeader);

    data = spice_marshaller_linearize(m, 0, &len, &free_data);

    if (outgoing_write(state->stream, &state->out_handler, data, len) != OUTGOING_OK) {
        return FALSE;
    }

    if (free_data) {
        free(data);
    }

    return TRUE;
}


static void inputs_handle_input(void *opaque, size_t size, uint32_t type, void *message)
{
    InputsState *state = (InputsState *)opaque;
    uint8_t *buf = (uint8_t *)message;
    SpiceMarshaller *m;

    switch (type) {
    case SPICE_MSGC_INPUTS_KEY_DOWN: {
        SpiceMsgcKeyDown *key_up = (SpiceMsgcKeyDown *)buf;
        if (key_up->code == CAPS_LOCK_SCAN_CODE || key_up->code == NUM_LOCK_SCAN_CODE ||
            key_up->code == SCROLL_LOCK_SCAN_CODE) {
            activate_modifiers_watch();
        }
    }
    case SPICE_MSGC_INPUTS_KEY_UP: {
        SpiceMsgcKeyDown *key_down = (SpiceMsgcKeyDown *)buf;
        uint8_t *now = (uint8_t *)&key_down->code;
        uint8_t *end = now + sizeof(key_down->code);
        for (; now < end && *now; now++) {
            kbd_push_scan(keyboard, *now);
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_MOTION: {
        SpiceMsgcMouseMotion *mouse_motion = (SpiceMsgcMouseMotion *)buf;

        if (++state->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0) {
            m = marshaller_new_for_outgoing(state, SPICE_MSG_INPUTS_MOUSE_MOTION_ACK);
            if (!marshaller_outgoing_write(m, state)) {
                red_printf("motion ack failed");
                reds_disconnect();
            }
            spice_marshaller_destroy(m);
        }
        if (mouse && reds->mouse_mode == SPICE_MOUSE_MODE_SERVER) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->motion(mouse,
                        mouse_motion->dx, mouse_motion->dy, 0,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_motion->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_POSITION: {
        SpiceMsgcMousePosition *pos = (SpiceMsgcMousePosition *)buf;

        if (++state->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0) {
            m = marshaller_new_for_outgoing(state, SPICE_MSG_INPUTS_MOUSE_MOTION_ACK);
            if (!marshaller_outgoing_write(m, state)) {
                red_printf("position ack failed");
                reds_disconnect();
            }
            spice_marshaller_destroy(m);
        }
        if (reds->mouse_mode != SPICE_MOUSE_MODE_CLIENT) {
            break;
        }
        ASSERT((agent_mouse && vdagent) || tablet);
        if (!agent_mouse || !vdagent) {
            SpiceTabletInterface *sif;
            sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
            sif->position(tablet, pos->x, pos->y, RED_MOUSE_STATE_TO_LOCAL(pos->buttons_state));
            break;
        }
        VDAgentMouseState *mouse_state = &state->mouse_state;
        mouse_state->x = pos->x;
        mouse_state->y = pos->y;
        mouse_state->buttons = RED_MOUSE_BUTTON_STATE_TO_AGENT(pos->buttons_state);
        mouse_state->display_id = pos->display_id;
        reds_handle_agent_mouse_event();
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_PRESS: {
        SpiceMsgcMousePress *mouse_press = (SpiceMsgcMousePress *)buf;
        int dz = 0;
        if (mouse_press->button == SPICE_MOUSE_BUTTON_UP) {
            dz = -1;
        } else if (mouse_press->button == SPICE_MOUSE_BUTTON_DOWN) {
            dz = 1;
        }
        if (reds->mouse_mode == SPICE_MOUSE_MODE_CLIENT) {
            if (agent_mouse && vdagent) {
                reds->inputs_state->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_press->buttons_state) |
                    (dz == -1 ? VD_AGENT_UBUTTON_MASK : 0) |
                    (dz == 1 ? VD_AGENT_DBUTTON_MASK : 0);
                reds_handle_agent_mouse_event();
            } else if (tablet) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
                sif->wheel(tablet, dz, RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
            }
        } else if (mouse) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->motion(mouse, 0, 0, dz,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_RELEASE: {
        SpiceMsgcMouseRelease *mouse_release = (SpiceMsgcMouseRelease *)buf;
        if (reds->mouse_mode == SPICE_MOUSE_MODE_CLIENT) {
            if (agent_mouse && vdagent) {
                reds->inputs_state->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_release->buttons_state);
                reds_handle_agent_mouse_event();
            } else if (tablet) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
                sif->buttons(tablet, RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
            }
        } else if (mouse) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->buttons(mouse,
                         RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_KEY_MODIFIERS: {
        SpiceMsgcKeyModifiers *modifiers = (SpiceMsgcKeyModifiers *)buf;
        uint8_t leds;

        if (!keyboard) {
            break;
        }
        leds = kbd_get_leds(keyboard);
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK)) {
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK)) {
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK)) {
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE | 0x80);
        }
        activate_modifiers_watch();
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        red_printf("unexpected type %d", type);
    }
}

void reds_set_client_mouse_allowed(int is_client_mouse_allowed, int x_res, int y_res)
{
    reds->monitor_mode.x_res = x_res;
    reds->monitor_mode.y_res = y_res;
    reds->dispatcher_allows_client_mouse = is_client_mouse_allowed;
    reds_update_mouse_mode();
    if (reds->is_client_mouse_allowed && tablet) {
        SpiceTabletInterface *sif;
        sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
        sif->set_logical_size(tablet, reds->monitor_mode.x_res, reds->monitor_mode.y_res);
    }
}

static void inputs_relase_keys(void)
{
    kbd_push_scan(keyboard, 0x2a | 0x80); //LSHIFT
    kbd_push_scan(keyboard, 0x36 | 0x80); //RSHIFT
    kbd_push_scan(keyboard, 0xe0); kbd_push_scan(keyboard, 0x1d | 0x80); //RCTRL
    kbd_push_scan(keyboard, 0x1d | 0x80); //LCTRL
    kbd_push_scan(keyboard, 0xe0); kbd_push_scan(keyboard, 0x38 | 0x80); //RALT
    kbd_push_scan(keyboard, 0x38 | 0x80); //LALT
}

static void inputs_event(int fd, int event, void *data)
{
    InputsState *inputs_state = data;

    if (event & SPICE_WATCH_EVENT_READ) {
        if (handle_incoming(inputs_state->stream, &inputs_state->in_handler)) {
            inputs_relase_keys();
            if (inputs_state->channel) {
                inputs_state->channel->data = NULL;
                reds->inputs_state = NULL;
            }
            reds_stream_free(inputs_state->stream);
            free(inputs_state);
        }
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        if (handle_outgoing(inputs_state->stream, &inputs_state->out_handler)) {
            reds_disconnect();
        }
    }
}


static void inputs_shutdown(Channel *channel)
{
    InputsState *state = (InputsState *)channel->data;
    if (state) {
        state->in_handler.shut = TRUE;
        shutdown(state->stream->socket, SHUT_RDWR);
        channel->data = NULL;
        state->channel = NULL;
        reds->inputs_state = NULL;
    }
}

static void inputs_migrate(Channel *channel)
{
    InputsState *state = (InputsState *)channel->data;
    SpiceMarshaller *m;
    SpiceMsgMigrate migrate;

    m = marshaller_new_for_outgoing(state, SPICE_MSG_MIGRATE);

    migrate.flags = 0;
    spice_marshall_msg_migrate(m, &migrate);

    if (!marshaller_outgoing_write(m, state)) {
        red_printf("write failed");
    }
    spice_marshaller_destroy(m);
}

static void inputs_select(void *opaque, int select)
{
    InputsState *inputs_state;
    int eventmask = SPICE_WATCH_EVENT_READ;
    red_printf("");

    inputs_state = (InputsState *)opaque;
    if (select) {
        eventmask |= SPICE_WATCH_EVENT_WRITE;
    }
    core->watch_update_mask(inputs_state->stream->watch, eventmask);
}

static void inputs_may_write(void *opaque)
{
    red_printf("");
}

static void inputs_link(Channel *channel, RedsStream *stream, int migration,
                        int num_common_caps, uint32_t *common_caps, int num_caps,
                        uint32_t *caps)
{
    InputsState *inputs_state;
    int delay_val;
    int flags;

    red_printf("");
    ASSERT(channel->data == NULL);

    inputs_state = spice_new0(InputsState, 1);

    delay_val = 1;
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    if ((flags = fcntl(stream->socket, F_GETFL)) == -1 ||
                                            fcntl(stream->socket, F_SETFL, flags | O_ASYNC) == -1) {
        red_printf("fcntl failed, %s", strerror(errno));
    }

    inputs_state->stream = stream;
    inputs_state->channel = channel;
    inputs_state->in_handler.parser = spice_get_client_channel_parser(SPICE_CHANNEL_INPUTS, NULL);
    inputs_state->in_handler.opaque = inputs_state;
    inputs_state->in_handler.handle_message = inputs_handle_input;
    inputs_state->out_handler.length = 0;
    inputs_state->out_handler.opaque = inputs_state;
    inputs_state->out_handler.select = inputs_select;
    inputs_state->out_handler.may_write = inputs_may_write;
    inputs_state->pending_mouse_event = FALSE;
    channel->data = inputs_state;
    reds->inputs_state = inputs_state;
    stream->watch = core->watch_add(stream->socket, SPICE_WATCH_EVENT_READ,
                                  inputs_event, inputs_state);

    SpiceMarshaller *m;
    SpiceMsgInputsInit inputs_init;
    m = marshaller_new_for_outgoing(inputs_state, SPICE_MSG_INPUTS_INIT);
    inputs_init.keyboard_modifiers = kbd_get_leds(keyboard);
    spice_marshall_msg_inputs_init(m, &inputs_init);
    if (!marshaller_outgoing_write(m, inputs_state)) {
        red_printf("failed to send modifiers state");
        reds_disconnect();
    }
    spice_marshaller_destroy(m);
}

static void reds_send_keyboard_modifiers(uint8_t modifiers)
{
    Channel *channel = reds_find_channel(SPICE_CHANNEL_INPUTS, 0);
    InputsState *state;
    SpiceMsgInputsKeyModifiers key_modifiers;
    SpiceMarshaller *m;

    if (!channel || !(state = (InputsState *)channel->data)) {
        return;
    }
    ASSERT(state->stream);

    m = marshaller_new_for_outgoing(state, SPICE_MSG_INPUTS_KEY_MODIFIERS);

    key_modifiers.modifiers = modifiers;
    spice_marshall_msg_inputs_key_modifiers(m, &key_modifiers);

    if (!marshaller_outgoing_write(m, state)) {
        red_printf("failed to send modifiers state");
        reds_disconnect();
    }
    spice_marshaller_destroy(m);
}

static void reds_on_keyboard_leds_change(void *opaque, uint8_t leds)
{
    reds_send_keyboard_modifiers(leds);
}

static void openssl_init(RedLinkInfo *link)
{
    unsigned long f4 = RSA_F4;
    link->tiTicketing.bn = BN_new();

    if (!link->tiTicketing.bn) {
        red_error("OpenSSL BIGNUMS alloc failed");
    }

    BN_set_word(link->tiTicketing.bn, f4);
}

static void inputs_init()
{
    Channel *channel;

    channel = spice_new0(Channel, 1);
    channel->type = SPICE_CHANNEL_INPUTS;
    channel->link = inputs_link;
    channel->shutdown = inputs_shutdown;
    channel->migrate = inputs_migrate;
    reds_register_channel(channel);
}

static void reds_handle_other_links(RedLinkInfo *link)
{
    Channel *channel;
    RedsStream *stream;
    SpiceLinkMess *link_mess;
    uint32_t *caps;

    link_mess = link->link_mess;

    if (!reds->link_id || reds->link_id != link_mess->connection_id) {
        reds_send_link_result(link, SPICE_LINK_ERR_BAD_CONNECTION_ID);
        reds_link_free(link);
        return;
    }

    if (!(channel = reds_find_channel(link_mess->channel_type,
                                      link_mess->channel_id))) {
        reds_send_link_result(link, SPICE_LINK_ERR_CHANNEL_NOT_AVAILABLE);
        reds_link_free(link);
        return;
    }

    reds_send_link_result(link, SPICE_LINK_ERR_OK);
    reds_show_new_channel(link, reds->link_id);
    if (link_mess->channel_type == SPICE_CHANNEL_INPUTS && !link->stream->ssl) {
        RedsOutItem *item;
        SpiceMsgNotify notify;
        char *mess = "keyboard channel is insecure";
        const int mess_len = strlen(mess);

        item = new_out_item(SPICE_MSG_NOTIFY);

        notify.time_stamp = get_time_stamp();
        notify.severity = SPICE_NOTIFY_SEVERITY_WARN;
        notify.visibilty = SPICE_NOTIFY_VISIBILITY_HIGH;
        notify.what = SPICE_WARN_GENERAL;
        notify.message_len = mess_len;

        spice_marshall_msg_notify(item->m, &notify);
        spice_marshaller_add(item->m, (uint8_t *)mess, mess_len + 1);

        reds_push_pipe_item(item);
    }
    stream = link->stream;
    reds_stream_remove_watch(stream);
    link->stream = NULL;
    link->link_mess = NULL;
    reds_link_free(link);
    caps = (uint32_t *)((uint8_t *)link_mess + link_mess->caps_offset);
    channel->link(channel, stream, reds->mig_target, link_mess->num_common_caps,
                  link_mess->num_common_caps ? caps : NULL, link_mess->num_channel_caps,
                  link_mess->num_channel_caps ? caps + link_mess->num_common_caps : NULL);
    free(link_mess);
}

static void reds_handle_link(RedLinkInfo *link)
{
    if (link->link_mess->channel_type == SPICE_CHANNEL_MAIN) {
        reds_handle_main_link(link);
    } else {
        reds_handle_other_links(link);
    }
}

static void reds_handle_ticket(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    char password[SPICE_MAX_PASSWORD_LENGTH];
    time_t ltime;

    //todo: use monotonic time
    time(&ltime);
    RSA_private_decrypt(link->tiTicketing.rsa_size,
                        link->tiTicketing.encrypted_ticket.encrypted_data,
                        (unsigned char *)password, link->tiTicketing.rsa, RSA_PKCS1_OAEP_PADDING);

    if (ticketing_enabled) {
        int expired = !link->link_mess->connection_id && taTicket.expiration_time < ltime;
        char *actual_sever_pass = link->link_mess->connection_id ? reds->taTicket.password :
                                                                   taTicket.password;
        if (strlen(actual_sever_pass) == 0) {
            reds_send_link_result(link, SPICE_LINK_ERR_PERMISSION_DENIED);
            red_printf("Ticketing is enabled, but no password is set. "
                       "please set a ticket first");
            reds_link_free(link);
            return;
        }

        if (expired || strncmp(password, actual_sever_pass, SPICE_MAX_PASSWORD_LENGTH) != 0) {
            reds_send_link_result(link, SPICE_LINK_ERR_PERMISSION_DENIED);
            reds_link_free(link);
            return;
        }
    }

    reds_handle_link(link);
}

static inline void async_read_clear_handlers(AsyncRead *obj)
{
    if (!obj->stream->watch) {
        return;
    }
    reds_stream_remove_watch(obj->stream);
}

#if HAVE_SASL
static int sync_write_u8(RedsStream *s, uint8_t n)
{
    return sync_write(s, &n, sizeof(uint8_t));
}

static int sync_write_u32(RedsStream *s, uint32_t n)
{
    return sync_write(s, &n, sizeof(uint32_t));
}

ssize_t reds_stream_sasl_write(RedsStream *s, const void *buf, size_t nbyte)
{
    ssize_t ret;

    if (!s->sasl.encoded) {
        int err;
        err = sasl_encode(s->sasl.conn, (char *)buf, nbyte,
                          (const char **)&s->sasl.encoded,
                          &s->sasl.encodedLength);
        if (err != SASL_OK) {
            red_printf("sasl_encode error: %d", err);
            return -1;
        }

        if (s->sasl.encodedLength == 0) {
            return 0;
        }

        if (!s->sasl.encoded) {
            red_printf("sasl_encode didn't return a buffer!");
            return 0;
        }

        s->sasl.encodedOffset = 0;
    }

    ret = s->write(s, s->sasl.encoded + s->sasl.encodedOffset,
                   s->sasl.encodedLength - s->sasl.encodedOffset);

    if (ret <= 0) {
        return ret;
    }

    s->sasl.encodedOffset += ret;
    if (s->sasl.encodedOffset == s->sasl.encodedLength) {
        s->sasl.encoded = NULL;
        s->sasl.encodedOffset = s->sasl.encodedLength = 0;
        return nbyte;
    }

    /* we didn't flush the encoded buffer */
    errno = EAGAIN;
    return -1;
}

static ssize_t reds_stream_sasl_read(RedsStream *s, void *buf, size_t nbyte)
{
    uint8_t encoded[4096];
    const char *decoded;
    unsigned int decodedlen;
    int err;
    int n;

    n = spice_buffer_copy(&s->sasl.inbuffer, buf, nbyte);
    if (n > 0) {
        spice_buffer_remove(&s->sasl.inbuffer, n);
        if (n == nbyte)
            return n;
        nbyte -= n;
        buf += n;
    }

    n = s->read(s, encoded, sizeof(encoded));
    if (n <= 0) {
        return n;
    }

    err = sasl_decode(s->sasl.conn,
                      (char *)encoded, n,
                      &decoded, &decodedlen);
    if (err != SASL_OK) {
        red_printf("sasl_decode error: %d", err);
        return -1;
    }

    if (decodedlen == 0) {
        errno = EAGAIN;
        return -1;
    }

    n = MIN(nbyte, decodedlen);
    memcpy(buf, decoded, n);
    spice_buffer_append(&s->sasl.inbuffer, decoded + n, decodedlen - n);
    return n;
}
#endif

static void async_read_handler(int fd, int event, void *data)
{
    AsyncRead *obj = (AsyncRead *)data;

    for (;;) {
        int n = obj->end - obj->now;

        ASSERT(n > 0);
        n = reds_stream_read(obj->stream, obj->now, n);
        if (n <= 0) {
            if (n < 0) {
                switch (errno) {
                case EAGAIN:
                    if (!obj->stream->watch) {
                        obj->stream->watch = core->watch_add(obj->stream->socket,
                                                             SPICE_WATCH_EVENT_READ,
                                                             async_read_handler, obj);
                    }
                    return;
                case EINTR:
                    break;
                default:
                    async_read_clear_handlers(obj);
                    obj->error(obj->opaque, errno);
                    return;
                }
            } else {
                async_read_clear_handlers(obj);
                obj->error(obj->opaque, 0);
                return;
            }
        } else {
            obj->now += n;
            if (obj->now == obj->end) {
                async_read_clear_handlers(obj);
                obj->done(obj->opaque);
                return;
            }
        }
    }
}

static void reds_get_spice_ticket(RedLinkInfo *link)
{
    AsyncRead *obj = &link->asyc_read;

    obj->now = (uint8_t *)&link->tiTicketing.encrypted_ticket.encrypted_data;
    obj->end = obj->now + link->tiTicketing.rsa_size;
    obj->done = reds_handle_ticket;
    async_read_handler(0, 0, &link->asyc_read);
}

#if HAVE_SASL
static char *addr_to_string(const char *format,
                            struct sockaddr *sa,
                            socklen_t salen) {
    char *addr;
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int err;
    size_t addrlen;

    if ((err = getnameinfo(sa, salen,
                           host, sizeof(host),
                           serv, sizeof(serv),
                           NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        red_printf("Cannot resolve address %d: %s",
                   err, gai_strerror(err));
        return NULL;
    }

    /* Enough for the existing format + the 2 vars we're
     * substituting in. */
    addrlen = strlen(format) + strlen(host) + strlen(serv);
    addr = spice_malloc(addrlen + 1);
    snprintf(addr, addrlen, format, host, serv);
    addr[addrlen] = '\0';

    return addr;
}

static int auth_sasl_check_ssf(RedsSASL *sasl, int *runSSF)
{
    const void *val;
    int err, ssf;

    *runSSF = 0;
    if (!sasl->wantSSF) {
        return 1;
    }

    err = sasl_getprop(sasl->conn, SASL_SSF, &val);
    if (err != SASL_OK) {
        return 0;
    }

    ssf = *(const int *)val;
    red_printf("negotiated an SSF of %d", ssf);
    if (ssf < 56) {
        return 0; /* 56 is good for Kerberos */
    }

    *runSSF = 1;

    /* We have a SSF that's good enough */
    return 1;
}

/*
 * Step Msg
 *
 * Input from client:
 *
 * u32 clientin-length
 * u8-array clientin-string
 *
 * Output to client:
 *
 * u32 serverout-length
 * u8-array serverout-strin
 * u8 continue
 */
#define SASL_DATA_MAX_LEN (1024 * 1024)

static void reds_handle_auth_sasl_steplen(void *opaque);

static void reds_handle_auth_sasl_step(void *opaque)
{
    const char *serverout;
    unsigned int serveroutlen;
    int err;
    char *clientdata = NULL;
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    RedsSASL *sasl = &link->stream->sasl;
    uint32_t datalen = sasl->len;
    AsyncRead *obj = &link->asyc_read;

    /* NB, distinction of NULL vs "" is *critical* in SASL */
    if (datalen) {
        clientdata = sasl->data;
        clientdata[datalen - 1] = '\0'; /* Wire includes '\0', but make sure */
        datalen--; /* Don't count NULL byte when passing to _start() */
    }

    red_printf("Step using SASL Data %p (%d bytes)",
               clientdata, datalen);
    err = sasl_server_step(sasl->conn,
                           clientdata,
                           datalen,
                           &serverout,
                           &serveroutlen);
    if (err != SASL_OK &&
        err != SASL_CONTINUE) {
        red_printf("sasl step failed %d (%s)",
                   err, sasl_errdetail(sasl->conn));
        goto authabort;
    }

    if (serveroutlen > SASL_DATA_MAX_LEN) {
        red_printf("sasl step reply data too long %d",
                   serveroutlen);
        goto authabort;
    }

    red_printf("SASL return data %d bytes, %p", serveroutlen, serverout);

    if (serveroutlen) {
        serveroutlen += 1;
        sync_write(link->stream, &serveroutlen, sizeof(uint32_t));
        sync_write(link->stream, serverout, serveroutlen);
    } else {
        sync_write(link->stream, &serveroutlen, sizeof(uint32_t));
    }

    /* Whether auth is complete */
    sync_write_u8(link->stream, err == SASL_CONTINUE ? 0 : 1);

    if (err == SASL_CONTINUE) {
        red_printf("%s", "Authentication must continue (step)");
        /* Wait for step length */
        obj->now = (uint8_t *)&sasl->len;
        obj->end = obj->now + sizeof(uint32_t);
        obj->done = reds_handle_auth_sasl_steplen;
        async_read_handler(0, 0, &link->asyc_read);
    } else {
        int ssf;

        if (auth_sasl_check_ssf(sasl, &ssf) == 0) {
            red_printf("Authentication rejected for weak SSF");
            goto authreject;
        }

        red_printf("Authentication successful");
        sync_write_u32(link->stream, SPICE_LINK_ERR_OK); /* Accept auth */

        /*
         * Delay writing in SSF encoded until now
         */
        sasl->runSSF = ssf;
        link->stream->writev = NULL; /* make sure writev isn't called directly anymore */

        reds_handle_link(link);
    }

    return;

authreject:
    sync_write_u32(link->stream, 1); /* Reject auth */
    sync_write_u32(link->stream, sizeof("Authentication failed"));
    sync_write(link->stream, "Authentication failed", sizeof("Authentication failed"));

authabort:
    reds_link_free(link);
    return;
}

static void reds_handle_auth_sasl_steplen(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    AsyncRead *obj = &link->asyc_read;
    RedsSASL *sasl = &link->stream->sasl;

    red_printf("Got steplen %d", sasl->len);
    if (sasl->len > SASL_DATA_MAX_LEN) {
        red_printf("Too much SASL data %d", sasl->len);
        reds_link_free(link);
        return;
    }

    if (sasl->len == 0) {
        return reds_handle_auth_sasl_step(opaque);
    } else {
        sasl->data = spice_realloc(sasl->data, sasl->len);
        obj->now = (uint8_t *)sasl->data;
        obj->end = obj->now + sasl->len;
        obj->done = reds_handle_auth_sasl_step;
        async_read_handler(0, 0, &link->asyc_read);
    }
}

/*
 * Start Msg
 *
 * Input from client:
 *
 * u32 clientin-length
 * u8-array clientin-string
 *
 * Output to client:
 *
 * u32 serverout-length
 * u8-array serverout-strin
 * u8 continue
 */


static void reds_handle_auth_sasl_start(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    AsyncRead *obj = &link->asyc_read;
    const char *serverout;
    unsigned int serveroutlen;
    int err;
    char *clientdata = NULL;
    RedsSASL *sasl = &link->stream->sasl;
    uint32_t datalen = sasl->len;

    /* NB, distinction of NULL vs "" is *critical* in SASL */
    if (datalen) {
        clientdata = sasl->data;
        clientdata[datalen - 1] = '\0'; /* Should be on wire, but make sure */
        datalen--; /* Don't count NULL byte when passing to _start() */
    }

    red_printf("Start SASL auth with mechanism %s. Data %p (%d bytes)",
              sasl->mechlist, clientdata, datalen);
    err = sasl_server_start(sasl->conn,
                            sasl->mechlist,
                            clientdata,
                            datalen,
                            &serverout,
                            &serveroutlen);
    if (err != SASL_OK &&
        err != SASL_CONTINUE) {
        red_printf("sasl start failed %d (%s)",
                   err, sasl_errdetail(sasl->conn));
        goto authabort;
    }

    if (serveroutlen > SASL_DATA_MAX_LEN) {
        red_printf("sasl start reply data too long %d",
                   serveroutlen);
        goto authabort;
    }

    red_printf("SASL return data %d bytes, %p", serveroutlen, serverout);

    if (serveroutlen) {
        serveroutlen += 1;
        sync_write(link->stream, &serveroutlen, sizeof(uint32_t));
        sync_write(link->stream, serverout, serveroutlen);
    } else {
        sync_write(link->stream, &serveroutlen, sizeof(uint32_t));
    }

    /* Whether auth is complete */
    sync_write_u8(link->stream, err == SASL_CONTINUE ? 0 : 1);

    if (err == SASL_CONTINUE) {
        red_printf("%s", "Authentication must continue (start)");
        /* Wait for step length */
        obj->now = (uint8_t *)&sasl->len;
        obj->end = obj->now + sizeof(uint32_t);
        obj->done = reds_handle_auth_sasl_steplen;
        async_read_handler(0, 0, &link->asyc_read);
    } else {
        int ssf;

        if (auth_sasl_check_ssf(sasl, &ssf) == 0) {
            red_printf("Authentication rejected for weak SSF");
            goto authreject;
        }

        red_printf("Authentication successful");
        sync_write_u32(link->stream, SPICE_LINK_ERR_OK); /* Accept auth */

        /*
         * Delay writing in SSF encoded until now
         */
        sasl->runSSF = ssf;
        link->stream->writev = NULL; /* make sure writev isn't called directly anymore */

        reds_handle_link(link);
    }

    return;

authreject:
    sync_write_u32(link->stream, 1); /* Reject auth */
    sync_write_u32(link->stream, sizeof("Authentication failed"));
    sync_write(link->stream, "Authentication failed", sizeof("Authentication failed"));

authabort:
    reds_link_free(link);
    return;
}

static void reds_handle_auth_startlen(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    AsyncRead *obj = &link->asyc_read;
    RedsSASL *sasl = &link->stream->sasl;

    red_printf("Got client start len %d", sasl->len);
    if (sasl->len > SASL_DATA_MAX_LEN) {
        red_printf("Too much SASL data %d", sasl->len);
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_link_free(link);
        return;
    }

    if (sasl->len == 0) {
        reds_handle_auth_sasl_start(opaque);
        return;
    }

    sasl->data = spice_realloc(sasl->data, sasl->len);
    obj->now = (uint8_t *)sasl->data;
    obj->end = obj->now + sasl->len;
    obj->done = reds_handle_auth_sasl_start;
    async_read_handler(0, 0, &link->asyc_read);
}

static void reds_handle_auth_mechname(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    AsyncRead *obj = &link->asyc_read;
    RedsSASL *sasl = &link->stream->sasl;

    sasl->mechname[sasl->len] = '\0';
    red_printf("Got client mechname '%s' check against '%s'",
               sasl->mechname, sasl->mechlist);

    if (strncmp(sasl->mechlist, sasl->mechname, sasl->len) == 0) {
        if (sasl->mechlist[sasl->len] != '\0' &&
            sasl->mechlist[sasl->len] != ',') {
            red_printf("One %d", sasl->mechlist[sasl->len]);
            reds_link_free(link);
            return;
        }
    } else {
        char *offset = strstr(sasl->mechlist, sasl->mechname);
        red_printf("Two %p", offset);
        if (!offset) {
            reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
            return;
        }
        red_printf("Two '%s'", offset);
        if (offset[-1] != ',' ||
            (offset[sasl->len] != '\0'&&
             offset[sasl->len] != ',')) {
            reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
            return;
        }
    }

    free(sasl->mechlist);
    sasl->mechlist = strdup(sasl->mechname);

    red_printf("Validated mechname '%s'", sasl->mechname);

    obj->now = (uint8_t *)&sasl->len;
    obj->end = obj->now + sizeof(uint32_t);
    obj->done = reds_handle_auth_startlen;
    async_read_handler(0, 0, &link->asyc_read);

    return;
}

static void reds_handle_auth_mechlen(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    AsyncRead *obj = &link->asyc_read;
    RedsSASL *sasl = &link->stream->sasl;

    if (sasl->len < 1 || sasl->len > 100) {
        red_printf("Got bad client mechname len %d", sasl->len);
        reds_link_free(link);
        return;
    }

    sasl->mechname = spice_malloc(sasl->len + 1);

    red_printf("Wait for client mechname");
    obj->now = (uint8_t *)sasl->mechname;
    obj->end = obj->now + sasl->len;
    obj->done = reds_handle_auth_mechname;
    async_read_handler(0, 0, &link->asyc_read);
}

static void reds_start_auth_sasl(RedLinkInfo *link)
{
    const char *mechlist = NULL;
    sasl_security_properties_t secprops;
    int err;
    char *localAddr, *remoteAddr;
    int mechlistlen;
    AsyncRead *obj = &link->asyc_read;
    RedsSASL *sasl = &link->stream->sasl;

    /* Get local & remote client addresses in form  IPADDR;PORT */
    if (!(localAddr = addr_to_string("%s;%s", &link->stream->info.laddr, link->stream->info.llen))) {
        goto error;
    }

    if (!(remoteAddr = addr_to_string("%s;%s", &link->stream->info.paddr, link->stream->info.plen))) {
        free(localAddr);
        goto error;
    }

    err = sasl_server_new("spice",
                          NULL, /* FQDN - just delegates to gethostname */
                          NULL, /* User realm */
                          localAddr,
                          remoteAddr,
                          NULL, /* Callbacks, not needed */
                          SASL_SUCCESS_DATA,
                          &sasl->conn);
    free(localAddr);
    free(remoteAddr);
    localAddr = remoteAddr = NULL;

    if (err != SASL_OK) {
        red_printf("sasl context setup failed %d (%s)",
                   err, sasl_errstring(err, NULL, NULL));
        sasl->conn = NULL;
        goto error;
    }

    /* Inform SASL that we've got an external SSF layer from TLS */
    if (link->stream->ssl) {
        sasl_ssf_t ssf;

        ssf = SSL_get_cipher_bits(link->stream->ssl, NULL);
        err = sasl_setprop(sasl->conn, SASL_SSF_EXTERNAL, &ssf);
        if (err != SASL_OK) {
            red_printf("cannot set SASL external SSF %d (%s)",
                       err, sasl_errstring(err, NULL, NULL));
            sasl_dispose(&sasl->conn);
            sasl->conn = NULL;
            goto error;
        }
    } else {
        sasl->wantSSF = 1;
    }

    memset(&secprops, 0, sizeof secprops);
    /* Inform SASL that we've got an external SSF layer from TLS */
    if (link->stream->ssl) {
        /* If we've got TLS (or UNIX domain sock), we don't care about SSF */
        secprops.min_ssf = 0;
        secprops.max_ssf = 0;
        secprops.maxbufsize = 8192;
        secprops.security_flags = 0;
    } else {
        /* Plain TCP, better get an SSF layer */
        secprops.min_ssf = 56; /* Good enough to require kerberos */
        secprops.max_ssf = 100000; /* Arbitrary big number */
        secprops.maxbufsize = 8192;
        /* Forbid any anonymous or trivially crackable auth */
        secprops.security_flags =
            SASL_SEC_NOANONYMOUS | SASL_SEC_NOPLAINTEXT;
    }

    err = sasl_setprop(sasl->conn, SASL_SEC_PROPS, &secprops);
    if (err != SASL_OK) {
        red_printf("cannot set SASL security props %d (%s)",
                   err, sasl_errstring(err, NULL, NULL));
        sasl_dispose(&sasl->conn);
        sasl->conn = NULL;
        goto error;
    }

    err = sasl_listmech(sasl->conn,
                        NULL, /* Don't need to set user */
                        "", /* Prefix */
                        ",", /* Separator */
                        "", /* Suffix */
                        &mechlist,
                        NULL,
                        NULL);
    if (err != SASL_OK) {
        red_printf("cannot list SASL mechanisms %d (%s)",
                   err, sasl_errdetail(sasl->conn));
        sasl_dispose(&sasl->conn);
        sasl->conn = NULL;
        goto error;
    }
    red_printf("Available mechanisms for client: '%s'", mechlist);

    sasl->mechlist = strdup(mechlist);

    mechlistlen = strlen(mechlist);
    if (!sync_write(link->stream, &mechlistlen, sizeof(uint32_t))
        || !sync_write(link->stream, sasl->mechlist, mechlistlen)) {
        red_printf("SASL mechanisms write error");
        goto error;
    }

    red_printf("Wait for client mechname length");
    obj->now = (uint8_t *)&sasl->len;
    obj->end = obj->now + sizeof(uint32_t);
    obj->done = reds_handle_auth_mechlen;
    async_read_handler(0, 0, &link->asyc_read);

    return;

error:
    reds_link_free(link);
    return;
}
#endif

static void reds_handle_auth_mechanism(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;

    red_printf("Auth method: %d", link->auth_mechanism.auth_mechanism);

    if (link->auth_mechanism.auth_mechanism == SPICE_COMMON_CAP_AUTH_SPICE
        && !sasl_enabled
        ) {
        reds_get_spice_ticket(link);
#if HAVE_SASL
    } else if (link->auth_mechanism.auth_mechanism == SPICE_COMMON_CAP_AUTH_SASL) {
        red_printf("Starting SASL");
        reds_start_auth_sasl(link);
#endif
    } else {
        red_printf("Unknown auth method, disconnecting");
        if (sasl_enabled) {
            red_printf("Your client doesn't handle SASL?");
        }
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_link_free(link);
    }
}

static int reds_security_check(RedLinkInfo *link)
{
    ChannelSecurityOptions *security_option = find_channel_security(link->link_mess->channel_type);
    uint32_t security = security_option ? security_option->options : default_channel_security;
    return (link->stream->ssl && (security & SPICE_CHANNEL_SECURITY_SSL)) ||
        (!link->stream->ssl && (security & SPICE_CHANNEL_SECURITY_NONE));
}

static void reds_handle_read_link_done(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    SpiceLinkMess *link_mess = link->link_mess;
    AsyncRead *obj = &link->asyc_read;
    uint32_t num_caps = link_mess->num_common_caps + link_mess->num_channel_caps;
    uint32_t *caps = (uint32_t *)((uint8_t *)link_mess + link_mess->caps_offset);
    int auth_selection;

    if (num_caps && (num_caps * sizeof(uint32_t) + link_mess->caps_offset >
                     link->link_header.size ||
                     link_mess->caps_offset < sizeof(*link_mess))) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_link_free(link);
        return;
    }

    auth_selection = link_mess->num_common_caps > 0 &&
        (caps[0] & (1 << SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION));;

    if (!reds_security_check(link)) {
        if (link->stream->ssl) {
            red_printf("spice channels %d should not be encrypted", link_mess->channel_type);
            reds_send_link_error(link, SPICE_LINK_ERR_NEED_UNSECURED);
        } else {
            red_printf("spice channels %d should be encrypted", link_mess->channel_type);
            reds_send_link_error(link, SPICE_LINK_ERR_NEED_SECURED);
        }
        reds_link_free(link);
        return;
    }

    if (!reds_send_link_ack(link)) {
        reds_link_free(link);
        return;
    }

    if (!auth_selection) {
        if (sasl_enabled) {
            red_printf("SASL enabled, but peer supports only spice authentication");
            reds_send_link_error(link, SPICE_LINK_ERR_VERSION_MISMATCH);
            return;
        }
        red_printf("Peer doesn't support AUTH selection");
        reds_get_spice_ticket(link);
    } else {
        obj->now = (uint8_t *)&link->auth_mechanism;
        obj->end = obj->now + sizeof(SpiceLinkAuthMechanism);
        obj->done = reds_handle_auth_mechanism;
        async_read_handler(0, 0, &link->asyc_read);
    }
}

static void reds_handle_link_error(void *opaque, int err)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    switch (err) {
    case 0:
    case EPIPE:
        break;
    default:
        red_printf("%s", strerror(errno));
        break;
    }
    reds_link_free(link);
}

static void reds_handle_read_header_done(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    SpiceLinkHeader *header = &link->link_header;
    AsyncRead *obj = &link->asyc_read;

    if (header->magic != SPICE_MAGIC) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_MAGIC);
        reds_link_free(link);
        return;
    }

    if (header->major_version != SPICE_VERSION_MAJOR) {
        if (header->major_version > 0) {
            reds_send_link_error(link, SPICE_LINK_ERR_VERSION_MISMATCH);
        }

        red_printf("version mismatch");
        reds_link_free(link);
        return;
    }

    reds->peer_minor_version = header->minor_version;

    if (header->size < sizeof(SpiceLinkMess)) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        red_printf("bad size %u", header->size);
        reds_link_free(link);
        return;
    }

    link->link_mess = spice_malloc(header->size);

    obj->now = (uint8_t *)link->link_mess;
    obj->end = obj->now + header->size;
    obj->done = reds_handle_read_link_done;
    async_read_handler(0, 0, &link->asyc_read);
}

static void reds_handle_new_link(RedLinkInfo *link)
{
    AsyncRead *obj = &link->asyc_read;
    obj->opaque = link;
    obj->stream = link->stream;
    obj->now = (uint8_t *)&link->link_header;
    obj->end = (uint8_t *)((SpiceLinkHeader *)&link->link_header + 1);
    obj->done = reds_handle_read_header_done;
    obj->error = reds_handle_link_error;
    async_read_handler(0, 0, &link->asyc_read);
}

static void reds_handle_ssl_accept(int fd, int event, void *data)
{
    RedLinkInfo *link = (RedLinkInfo *)data;
    int return_code;

    if ((return_code = SSL_accept(link->stream->ssl)) != 1) {
        int ssl_error = SSL_get_error(link->stream->ssl, return_code);
        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
            red_printf("SSL_accept failed, error=%d", ssl_error);
            reds_link_free(link);
        } else {
            if (ssl_error == SSL_ERROR_WANT_READ) {
                core->watch_update_mask(link->stream->watch, SPICE_WATCH_EVENT_READ);
            } else {
                core->watch_update_mask(link->stream->watch, SPICE_WATCH_EVENT_WRITE);
            }
        }
        return;
    }
    reds_stream_remove_watch(link->stream);
    reds_handle_new_link(link);
}

static RedLinkInfo *__reds_accept_connection(int listen_socket)
{
    RedLinkInfo *link;
    RedsStream *stream;
    int delay_val = 1;
    int flags;
    int socket;

    if ((socket = accept(listen_socket, NULL, 0)) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        return NULL;
    }

    if ((flags = fcntl(socket, F_GETFL)) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error;
    }

    if (fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error;
    }

    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    link = spice_new0(RedLinkInfo, 1);
    stream = spice_new0(RedsStream, 1);
    link->stream = stream;
    stream->socket = socket;

    /* gather info + send event */
    stream->info.llen = sizeof(stream->info.laddr);
    stream->info.plen = sizeof(stream->info.paddr);
    getsockname(stream->socket, (struct sockaddr*)(&stream->info.laddr), &stream->info.llen);
    getpeername(stream->socket, (struct sockaddr*)(&stream->info.paddr), &stream->info.plen);
    reds_channel_event(stream, SPICE_CHANNEL_EVENT_CONNECTED);

    openssl_init(link);

    return link;

error:
    close(socket);

    return NULL;
}

static RedLinkInfo *reds_accept_connection(int listen_socket)
{
    RedLinkInfo *link;
    RedsStream *stream;

    if (!(link = __reds_accept_connection(listen_socket))) {
        return NULL;
    }

    stream = link->stream;
    stream->read = stream_read_cb;
    stream->write = stream_write_cb;
    stream->writev = stream_writev_cb;

    return link;
}

static void reds_accept_ssl_connection(int fd, int event, void *data)
{
    RedLinkInfo *link;
    int return_code;
    int ssl_error;
    BIO *sbio;

    link = __reds_accept_connection(reds->secure_listen_socket);
    if (link == NULL) {
        return;
    }

    // Handle SSL handshaking
    if (!(sbio = BIO_new_socket(link->stream->socket, BIO_NOCLOSE))) {
        red_printf("could not allocate ssl bio socket");
        goto error;
    }

    link->stream->ssl = SSL_new(reds->ctx);
    if (!link->stream->ssl) {
        red_printf("could not allocate ssl context");
        BIO_free(sbio);
        goto error;
    }

    SSL_set_bio(link->stream->ssl, sbio, sbio);

    link->stream->write = stream_ssl_write_cb;
    link->stream->read = stream_ssl_read_cb;
    link->stream->writev = NULL;

    return_code = SSL_accept(link->stream->ssl);
    if (return_code == 1) {
        reds_handle_new_link(link);
        return;
    }

    ssl_error = SSL_get_error(link->stream->ssl, return_code);
    if (return_code == -1 && (ssl_error == SSL_ERROR_WANT_READ ||
                              ssl_error == SSL_ERROR_WANT_WRITE)) {
        int eventmask = ssl_error == SSL_ERROR_WANT_READ ?
            SPICE_WATCH_EVENT_READ : SPICE_WATCH_EVENT_WRITE;
        link->stream->watch = core->watch_add(link->stream->socket, eventmask,
                                            reds_handle_ssl_accept, link);
        return;
    }

    ERR_print_errors_fp(stderr);
    red_printf("SSL_accept failed, error=%d", ssl_error);
    SSL_free(link->stream->ssl);

error:
    close(link->stream->socket);
    free(link->stream);
    BN_free(link->tiTicketing.bn);
    free(link);
}

static void reds_accept(int fd, int event, void *data)
{
    RedLinkInfo *link;

    link = reds_accept_connection(reds->listen_socket);
    if (link == NULL) {
        red_printf("accept failed");
        return;
    }
    reds_handle_new_link(link);
}

static int reds_init_socket(const char *addr, int portnr, int family)
{
    static const int on=1, off=0;
    struct addrinfo ai,*res,*e;
    char port[33];
    char uaddr[INET6_ADDRSTRLEN+1];
    char uport[33];
    int slisten,rc;

    memset(&ai,0, sizeof(ai));
    ai.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_family = family;

    snprintf(port, sizeof(port), "%d", portnr);
    rc = getaddrinfo(strlen(addr) ? addr : NULL, port, &ai, &res);
    if (rc != 0) {
        red_error("getaddrinfo(%s,%s): %s", addr, port,
                  gai_strerror(rc));
    }

    for (e = res; e != NULL; e = e->ai_next) {
        getnameinfo((struct sockaddr*)e->ai_addr,e->ai_addrlen,
                    uaddr,INET6_ADDRSTRLEN, uport,32,
                    NI_NUMERICHOST | NI_NUMERICSERV);
        slisten = socket(e->ai_family, e->ai_socktype, e->ai_protocol);
        if (slisten < 0) {
            continue;
        }

        setsockopt(slisten,SOL_SOCKET,SO_REUSEADDR,(void*)&on,sizeof(on));
#ifdef IPV6_V6ONLY
        if (e->ai_family == PF_INET6) {
            /* listen on both ipv4 and ipv6 */
            setsockopt(slisten,IPPROTO_IPV6,IPV6_V6ONLY,(void*)&off,
                       sizeof(off));
        }
#endif
        if (bind(slisten, e->ai_addr, e->ai_addrlen) == 0) {
            goto listen;
        }
        close(slisten);
    }
    red_printf("%s: binding socket to %s:%d failed", __FUNCTION__,
               addr, portnr);
    freeaddrinfo(res);
    return -1;

listen:
    freeaddrinfo(res);
    if (listen(slisten,1) != 0) {
        red_error("%s: listen: %s", __FUNCTION__, strerror(errno));
        close(slisten);
        return -1;
    }
    return slisten;
}

static int reds_init_net(void)
{
    if (spice_port != -1) {
        reds->listen_socket = reds_init_socket(spice_addr, spice_port, spice_family);
        if (-1 == reds->listen_socket) {
            return -1;
        }
        reds->listen_watch = core->watch_add(reds->listen_socket,
                                             SPICE_WATCH_EVENT_READ,
                                             reds_accept, NULL);
        if (reds->listen_watch == NULL) {
            red_error("set fd handle failed");
        }
    }

    if (spice_secure_port != -1) {
        reds->secure_listen_socket = reds_init_socket(spice_addr, spice_secure_port,
                                                      spice_family);
        if (-1 == reds->secure_listen_socket) {
            return -1;
        }
        reds->secure_listen_watch = core->watch_add(reds->secure_listen_socket,
                                                    SPICE_WATCH_EVENT_READ,
                                                    reds_accept_ssl_connection, NULL);
        if (reds->secure_listen_watch == NULL) {
            red_error("set fd handle failed");
        }
    }
    return 0;
}

static void load_dh_params(SSL_CTX *ctx, char *file)
{
    DH *ret = 0;
    BIO *bio;

    if ((bio = BIO_new_file(file, "r")) == NULL) {
        red_error("Could not open DH file");
    }

    ret = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
    if (ret == 0) {
        red_error("Could not read DH params");
    }

    BIO_free(bio);

    if (SSL_CTX_set_tmp_dh(ctx, ret) < 0) {
        red_error("Could not set DH params");
    }
}

/*The password code is not thread safe*/
static int ssl_password_cb(char *buf, int size, int flags, void *userdata)
{
    char *pass = ssl_parameters.keyfile_password;
    if (size < strlen(pass) + 1) {
        return (0);
    }

    strcpy(buf, pass);
    return (strlen(pass));
}

static unsigned long pthreads_thread_id(void)
{
    unsigned long ret;

    ret = (unsigned long)pthread_self();
    return (ret);
}

static void pthreads_locking_callback(int mode, int type, char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        pthread_mutex_lock(&(lock_cs[type]));
        lock_count[type]++;
    } else {
        pthread_mutex_unlock(&(lock_cs[type]));
    }
}

static void openssl_thread_setup()
{
    int i;

    lock_cs = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));
    lock_count = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(long));

    for (i = 0; i < CRYPTO_num_locks(); i++) {
        lock_count[i] = 0;
        pthread_mutex_init(&(lock_cs[i]), NULL);
    }

    CRYPTO_set_id_callback((unsigned long (*)())pthreads_thread_id);
    CRYPTO_set_locking_callback((void (*)())pthreads_locking_callback);
}

static void reds_init_ssl()
{
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
    const SSL_METHOD *ssl_method;
#else
    SSL_METHOD *ssl_method;
#endif
    int return_code;
    long ssl_options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;

    /* Global system initialization*/
    SSL_library_init();
    SSL_load_error_strings();

    /* Create our context*/
    ssl_method = TLSv1_method();
    reds->ctx = SSL_CTX_new(ssl_method);
    if (!reds->ctx) {
        red_error("Could not allocate new SSL context");
    }

    /* Limit connection to TLSv1 only */
#ifdef SSL_OP_NO_COMPRESSION
    ssl_options |= SSL_OP_NO_COMPRESSION;
#endif
    SSL_CTX_set_options(reds->ctx, ssl_options);

    /* Load our keys and certificates*/
    return_code = SSL_CTX_use_certificate_chain_file(reds->ctx, ssl_parameters.certs_file);
    if (return_code != 1) {
        red_error("Could not load certificates from %s", ssl_parameters.certs_file);
    }

    SSL_CTX_set_default_passwd_cb(reds->ctx, ssl_password_cb);

    return_code = SSL_CTX_use_PrivateKey_file(reds->ctx, ssl_parameters.private_key_file,
                                              SSL_FILETYPE_PEM);
    if (return_code != 1) {
        red_error("Could not use private key file");
    }

    /* Load the CAs we trust*/
    return_code = SSL_CTX_load_verify_locations(reds->ctx, ssl_parameters.ca_certificate_file, 0);
    if (return_code != 1) {
        red_error("Could not use ca file");
    }

#if (OPENSSL_VERSION_NUMBER < 0x00905100L)
    SSL_CTX_set_verify_depth(reds->ctx, 1);
#endif

    if (strlen(ssl_parameters.dh_key_file) > 0) {
        load_dh_params(reds->ctx, ssl_parameters.dh_key_file);
    }

    SSL_CTX_set_session_id_context(reds->ctx, (const unsigned char *)"SPICE", 5);
    if (strlen(ssl_parameters.ciphersuite) > 0) {
        SSL_CTX_set_cipher_list(reds->ctx, ssl_parameters.ciphersuite);
    }

    openssl_thread_setup();

#ifndef SSL_OP_NO_COMPRESSION
    STACK *cmp_stack = SSL_COMP_get_compression_methods();
    sk_zero(cmp_stack);
#endif
}

static void reds_exit()
{
    if (reds->stream) {
        close(reds->stream->socket);
    }
#ifdef RED_STATISTICS
    shm_unlink(reds->stat_shm_name);
    free(reds->stat_shm_name);
#endif
}

enum {
    SPICE_OPTION_INVALID,
    SPICE_OPTION_PORT,
    SPICE_OPTION_SPORT,
    SPICE_OPTION_HOST,
    SPICE_OPTION_IMAGE_COMPRESSION,
    SPICE_OPTION_PASSWORD,
    SPICE_OPTION_DISABLE_TICKET,
    SPICE_OPTION_RENDERER,
    SPICE_OPTION_SSLKEY,
    SPICE_OPTION_SSLCERTS,
    SPICE_OPTION_SSLCAFILE,
    SPICE_OPTION_SSLDHFILE,
    SPICE_OPTION_SSLPASSWORD,
    SPICE_OPTION_SSLCIPHERSUITE,
    SPICE_SECURED_CHANNELS,
    SPICE_UNSECURED_CHANNELS,
    SPICE_OPTION_STREAMING_VIDEO,
    SPICE_OPTION_AGENT_MOUSE,
    SPICE_OPTION_PLAYBACK_COMPRESSION,
};

typedef struct OptionsMap {
    const char *name;
    int val;
} OptionsMap;

enum {
    SPICE_TICKET_OPTION_INVALID,
    SPICE_TICKET_OPTION_EXPIRATION,
    SPICE_TICKET_OPTION_CONNECTED,
};

static inline void on_activating_ticketing()
{
    if (!ticketing_enabled && reds->stream) {
        red_printf("disconnecting");
        reds_disconnect();
    }
}

static void set_image_compression(spice_image_compression_t val)
{
    if (val == image_compression) {
        return;
    }
    image_compression = val;
    red_dispatcher_on_ic_change();
}

static void set_one_channel_security(int id, uint32_t security)
{
    ChannelSecurityOptions *security_options;

    if ((security_options = find_channel_security(id))) {
        security_options->options = security;
        return;
    }
    security_options = spice_new(ChannelSecurityOptions, 1);
    security_options->channel_id = id;
    security_options->options = security;
    security_options->next = channels_security;
    channels_security = security_options;
}

#define REDS_SAVE_VERSION 1

struct RedsMigSpice {
    char pub_key[SPICE_TICKET_PUBKEY_BYTES];
    uint32_t mig_key;
    char *host;
    char *cert_subject;
    int port;
    int sport;
    uint16_t cert_pub_key_type;
    uint32_t cert_pub_key_len;
    uint8_t* cert_pub_key;
};

typedef struct RedsMigSpiceMessage {
    uint32_t link_id;
} RedsMigSpiceMessage;

typedef struct RedsMigCertPubKeyInfo {
    uint16_t type;
    uint32_t len;
} RedsMigCertPubKeyInfo;

static void reds_mig_release(void)
{
    if (reds->mig_spice) {
        free(reds->mig_spice->cert_subject);
        free(reds->mig_spice->host);
        free(reds->mig_spice);
        reds->mig_spice = NULL;
    }
}

static void reds_mig_continue(void)
{
    RedsMigSpice *s = reds->mig_spice;
    SpiceMsgMainMigrationBegin migrate;
    RedsOutItem *item;

    red_printf("");
    item = new_out_item(SPICE_MSG_MAIN_MIGRATE_BEGIN);

    migrate.port = s->port;
    migrate.sport = s->sport;
    migrate.host_size = strlen(s->host) + 1;
    migrate.host_data = (uint8_t *)s->host;
    migrate.pub_key_type = s->cert_pub_key_type;
    migrate.pub_key_size = s->cert_pub_key_len;
    migrate.pub_key_data = s->cert_pub_key;
    spice_marshall_msg_main_migrate_begin(item->m, &migrate);

    reds_push_pipe_item(item);

    reds_mig_release();

    reds->mig_wait_connect = TRUE;
    core->timer_start(reds->mig_timer, MIGRATE_TIMEOUT);
}

static void reds_mig_started(void)
{
    red_printf("");

    reds->mig_inprogress = TRUE;

    if (reds->listen_watch != NULL) {
        core->watch_update_mask(reds->listen_watch, 0);
    }

    if (reds->secure_listen_watch != NULL) {
        core->watch_update_mask(reds->secure_listen_watch, 0);
    }

    if (reds->stream == NULL) {
        red_printf("not connected to stream");
        goto error;
    }

    if ((SPICE_VERSION_MAJOR == 1) && (reds->peer_minor_version < 2)) {
        red_printf("minor version mismatch client %u server %u",
                   reds->peer_minor_version, SPICE_VERSION_MINOR);
        goto error;
    }

    reds_mig_continue();
    return;

error:
    reds_mig_release();
    reds_mig_disconnect();
}

static void reds_mig_finished(int completed)
{
    RedsOutItem *item;

    red_printf("");
    if (reds->listen_watch != NULL) {
        core->watch_update_mask(reds->listen_watch, SPICE_WATCH_EVENT_READ);
    }

    if (reds->secure_listen_watch != NULL) {
        core->watch_update_mask(reds->secure_listen_watch, SPICE_WATCH_EVENT_READ);
    }

    if (reds->stream == NULL) {
        red_printf("no stream connected");
        return;
    }
    reds->mig_inprogress = TRUE;

    if (completed) {
        Channel *channel;
        SpiceMsgMigrate migrate;

        reds->mig_wait_disconnect = TRUE;
        core->timer_start(reds->mig_timer, MIGRATE_TIMEOUT);

        item = new_out_item(SPICE_MSG_MIGRATE);
        migrate.flags = SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER;
        spice_marshall_msg_migrate(item->m, &migrate);

        reds_push_pipe_item(item);
        channel = reds->channels;
        while (channel) {
            channel->migrate(channel);
            channel = channel->next;
        }
    } else {
        item = new_out_item(SPICE_MSG_MAIN_MIGRATE_CANCEL);
        reds_push_pipe_item(item);
        reds_mig_cleanup();
    }
}

static void reds_mig_switch(void)
{
    RedsMigSpice *s = reds->mig_spice;
    SpiceMsgMainMigrationSwitchHost migrate;
    RedsOutItem *item;

    if (s == NULL) {
        return;
    }

    red_printf("");
    item = new_out_item(SPICE_MSG_MAIN_MIGRATE_SWITCH_HOST);

    migrate.port = s->port;
    migrate.sport = s->sport;
    migrate.host_size = strlen(s->host) + 1;
    migrate.host_data = (uint8_t *)s->host;
    if (s->cert_subject) {
        migrate.cert_subject_size = strlen(s->cert_subject) + 1;
        migrate.cert_subject_data = (uint8_t *)s->cert_subject;
    } else {
        migrate.cert_subject_size = 0;
        migrate.cert_subject_data = NULL;
    }
    spice_marshall_msg_main_migrate_switch_host(item->m, &migrate);

    reds_push_pipe_item(item);

    reds_mig_release();
}

static void migrate_timout(void *opaque)
{
    red_printf("");
    ASSERT(reds->mig_wait_connect || reds->mig_wait_disconnect);
    reds_mig_disconnect();
}

static void key_modifiers_sender(void *opaque)
{
    reds_send_keyboard_modifiers(kbd_get_leds(keyboard));
}

uint32_t reds_get_mm_time(void)
{
    struct timespec time_space;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    return time_space.tv_sec * 1000 + time_space.tv_nsec / 1000 / 1000;
}

void reds_update_mm_timer(uint32_t mm_time)
{
    red_dispatcher_set_mm_time(mm_time);
}

void reds_enable_mm_timer(void)
{
    SpiceMsgMainMultiMediaTime time_mes;
    RedsOutItem *item;

    core->timer_start(reds->mm_timer, MM_TIMER_GRANULARITY_MS);
    if (!reds->stream) {
        return;
    }

    item = new_out_item(SPICE_MSG_MAIN_MULTI_MEDIA_TIME);
    time_mes.time = reds_get_mm_time() - MM_TIME_DELTA;
    spice_marshall_msg_main_multi_media_time(item->m, &time_mes);
    reds_push_pipe_item(item);
}

void reds_desable_mm_timer(void)
{
    core->timer_cancel(reds->mm_timer);
}

static void mm_timer_proc(void *opaque)
{
    red_dispatcher_set_mm_time(reds_get_mm_time());
    core->timer_start(reds->mm_timer, MM_TIMER_GRANULARITY_MS);
}

static void attach_to_red_agent(SpiceCharDeviceInstance *sin)
{
    VDIPortState *state = &reds->agent_state;
    SpiceCharDeviceInterface *sif;

    vdagent = sin;
    reds_update_mouse_mode();
    if (!reds->stream) {
        return;
    }
    sif = SPICE_CONTAINEROF(vdagent->base.sif, SpiceCharDeviceInterface, base);
    state->connected = 1;
    state->read_filter.discard_all = FALSE;
    if (sif->state) {
        sif->state(vdagent, state->connected);
    }
    reds->agent_state.plug_generation++;

    if (reds->mig_target) {
        return;
    }

    reds_send_agent_connected();
}

SPICE_GNUC_VISIBLE void spice_server_char_device_wakeup(SpiceCharDeviceInstance* sin)
{
    (*sin->st->wakeup)(sin);
}

#define SUBTYPE_VDAGENT "vdagent"
#define SUBTYPE_SMARTCARD "smartcard"

const char *spice_server_char_device_recognized_subtypes_list[] = {
    SUBTYPE_VDAGENT,
#ifdef USE_SMARTCARD
    SUBTYPE_SMARTCARD,
#endif
    NULL,
};

SPICE_GNUC_VISIBLE const char** spice_server_char_device_recognized_subtypes()
{
    return spice_server_char_device_recognized_subtypes_list;
}

static int spice_server_char_device_add_interface(SpiceServer *s,
                                           SpiceBaseInstance *sin)
{
    SpiceCharDeviceInstance* char_device =
            SPICE_CONTAINEROF(sin, SpiceCharDeviceInstance, base);

    red_printf("CHAR_DEVICE %s", char_device->subtype);
    if (strcmp(char_device->subtype, SUBTYPE_VDAGENT) == 0) {
        if (vdagent) {
            red_printf("vdagent already attached");
            return -1;
        }
        char_device->st = &vdagent_char_device_state;
        attach_to_red_agent(char_device);
    }
#ifdef USE_SMARTCARD
    else if (strcmp(char_device->subtype, SUBTYPE_SMARTCARD) == 0) {
        if (smartcard_device_connect(char_device) == -1) {
            return -1;
        }
    }
#endif
    return 0;
}

static void spice_server_char_device_remove_interface(SpiceBaseInstance *sin)
{
    SpiceCharDeviceInstance* char_device =
            SPICE_CONTAINEROF(sin, SpiceCharDeviceInstance, base);

    red_printf("remove CHAR_DEVICE %s", char_device->subtype);
    if (strcmp(char_device->subtype, SUBTYPE_VDAGENT) == 0) {
        if (vdagent) {
            reds_agent_remove();
        }
    }
#ifdef USE_SMARTCARD
    else if (strcmp(char_device->subtype, SUBTYPE_SMARTCARD) == 0) {
        smartcard_device_disconnect(char_device);
    }
#endif
}

SPICE_GNUC_VISIBLE int spice_server_add_interface(SpiceServer *s,
                                                  SpiceBaseInstance *sin)
{
    const SpiceBaseInterface *interface = sin->sif;

    ASSERT(reds == s);

    if (strcmp(interface->type, SPICE_INTERFACE_KEYBOARD) == 0) {
        red_printf("SPICE_INTERFACE_KEYBOARD");
        if (keyboard) {
            red_printf("already have keyboard");
            return -1;
        }
        if (interface->major_version != SPICE_INTERFACE_KEYBOARD_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_KEYBOARD_MINOR) {
            red_printf("unsupported keyboard interface");
            return -1;
        }
        keyboard = SPICE_CONTAINEROF(sin, SpiceKbdInstance, base);
        keyboard->st = spice_new0(SpiceKbdState, 1);

    } else if (strcmp(interface->type, SPICE_INTERFACE_MOUSE) == 0) {
        red_printf("SPICE_INTERFACE_MOUSE");
        if (mouse) {
            red_printf("already have mouse");
            return -1;
        }
        if (interface->major_version != SPICE_INTERFACE_MOUSE_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_MOUSE_MINOR) {
            red_printf("unsupported mouse interface");
            return -1;
        }
        mouse = SPICE_CONTAINEROF(sin, SpiceMouseInstance, base);
        mouse->st = spice_new0(SpiceMouseState, 1);

    } else if (strcmp(interface->type, SPICE_INTERFACE_QXL) == 0) {
        QXLInstance *qxl;

        red_printf("SPICE_INTERFACE_QXL");
        if (interface->major_version != SPICE_INTERFACE_QXL_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_QXL_MINOR) {
            red_printf("unsupported qxl interface");
            return -1;
        }

        qxl = SPICE_CONTAINEROF(sin, QXLInstance, base);
        qxl->st = spice_new0(QXLState, 1);
        qxl->st->qif = SPICE_CONTAINEROF(interface, QXLInterface, base);
        qxl->st->dispatcher = red_dispatcher_init(qxl);

    } else if (strcmp(interface->type, SPICE_INTERFACE_TABLET) == 0) {
        red_printf("SPICE_INTERFACE_TABLET");
        if (tablet) {
            red_printf("already have tablet");
            return -1;
        }
        if (interface->major_version != SPICE_INTERFACE_TABLET_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_TABLET_MINOR) {
            red_printf("unsupported tablet interface");
            return -1;
        }
        tablet = SPICE_CONTAINEROF(sin, SpiceTabletInstance, base);
        tablet->st = spice_new0(SpiceTabletState, 1);
        reds_update_mouse_mode();
        if (reds->is_client_mouse_allowed) {
            SpiceTabletInterface *sif;
            sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
            sif->set_logical_size(tablet, reds->monitor_mode.x_res,
                                  reds->monitor_mode.y_res);
        }

    } else if (strcmp(interface->type, SPICE_INTERFACE_PLAYBACK) == 0) {
        red_printf("SPICE_INTERFACE_PLAYBACK");
        if (interface->major_version != SPICE_INTERFACE_PLAYBACK_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_PLAYBACK_MINOR) {
            red_printf("unsupported playback interface");
            return -1;
        }
        snd_attach_playback(SPICE_CONTAINEROF(sin, SpicePlaybackInstance, base));

    } else if (strcmp(interface->type, SPICE_INTERFACE_RECORD) == 0) {
        red_printf("SPICE_INTERFACE_RECORD");
        if (interface->major_version != SPICE_INTERFACE_RECORD_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_RECORD_MINOR) {
            red_printf("unsupported record interface");
            return -1;
        }
        snd_attach_record(SPICE_CONTAINEROF(sin, SpiceRecordInstance, base));

    } else if (strcmp(interface->type, SPICE_INTERFACE_CHAR_DEVICE) == 0) {
        if (interface->major_version != SPICE_INTERFACE_CHAR_DEVICE_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_CHAR_DEVICE_MINOR) {
            red_printf("unsupported char device interface");
            return -1;
        }
        spice_server_char_device_add_interface(s, sin);

    } else if (strcmp(interface->type, SPICE_INTERFACE_NET_WIRE) == 0) {
#ifdef USE_TUNNEL
        SpiceNetWireInstance *net;
        red_printf("SPICE_INTERFACE_NET_WIRE");
        if (red_tunnel) {
            red_printf("net wire already attached");
            return -1;
        }
        if (interface->major_version != SPICE_INTERFACE_NET_WIRE_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_NET_WIRE_MINOR) {
            red_printf("unsupported net wire interface");
            return -1;
        }
        net = SPICE_CONTAINEROF(sin, SpiceNetWireInstance, base);
        net->st = spice_new0(SpiceNetWireState, 1);
        red_tunnel = red_tunnel_attach(core, net);
#else
        red_printf("unsupported net wire interface");
        return -1;
#endif
    }

    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_remove_interface(SpiceBaseInstance *sin)
{
    const SpiceBaseInterface *interface = sin->sif;

    if (strcmp(interface->type, SPICE_INTERFACE_TABLET) == 0) {
        red_printf("remove SPICE_INTERFACE_TABLET");
        if (sin == &tablet->base) {
            tablet = NULL;
            reds_update_mouse_mode();
        }

    } else if (strcmp(interface->type, SPICE_INTERFACE_PLAYBACK) == 0) {
        red_printf("remove SPICE_INTERFACE_PLAYBACK");
        snd_detach_playback(SPICE_CONTAINEROF(sin, SpicePlaybackInstance, base));

    } else if (strcmp(interface->type, SPICE_INTERFACE_RECORD) == 0) {
        red_printf("remove SPICE_INTERFACE_RECORD");
        snd_detach_record(SPICE_CONTAINEROF(sin, SpiceRecordInstance, base));

    } else if (strcmp(interface->type, SPICE_INTERFACE_CHAR_DEVICE) == 0) {
        spice_server_char_device_remove_interface(sin);
    } else {
        red_error("VD_INTERFACE_REMOVING unsupported");
        return -1;
    }

    return 0;
}

static void free_external_agent_buff(VDIPortBuf *in_buf)
{
    VDIPortState *state = &reds->agent_state;

    ring_add(&state->external_bufs, &in_buf->link);
    add_token();
}

static void free_internal_agent_buff(VDIPortBuf *in_buf)
{
    VDIPortState *state = &reds->agent_state;

    ring_add(&state->internal_bufs, &in_buf->link);
    if (reds->inputs_state && reds->inputs_state->pending_mouse_event) {
        reds_handle_agent_mouse_event();
    }
}

static void init_vd_agent_resources()
{
    VDIPortState *state = &reds->agent_state;
    int i;

    ring_init(&state->external_bufs);
    ring_init(&state->internal_bufs);
    ring_init(&state->write_queue);
    ring_init(&state->read_bufs);
    agent_msg_filter_init(&state->write_filter, agent_copypaste, TRUE);
    agent_msg_filter_init(&state->read_filter, agent_copypaste, TRUE);

    state->read_state = VDI_PORT_READ_STATE_READ_HADER;
    state->recive_pos = (uint8_t *)&state->vdi_chunk_header;
    state->recive_len = sizeof(state->vdi_chunk_header);

    for (i = 0; i < REDS_AGENT_WINDOW_SIZE; i++) {
        VDAgentExtBuf *buf = spice_new0(VDAgentExtBuf, 1);
        ring_item_init(&buf->base.link);
        buf->base.chunk_header.port = VDP_CLIENT_PORT;
        buf->base.free = free_external_agent_buff;
        ring_add(&reds->agent_state.external_bufs, &buf->base.link);
    }

    for (i = 0; i < REDS_NUM_INTERNAL_AGENT_MESSAGES; i++) {
        VDInternalBuf *buf = spice_new0(VDInternalBuf, 1);
        ring_item_init(&buf->base.link);
        buf->base.free = free_internal_agent_buff;
        buf->base.chunk_header.port = VDP_SERVER_PORT;
        buf->base.chunk_header.size = sizeof(VDAgentMessage) + sizeof(VDAgentMouseState);
        buf->header.protocol = VD_AGENT_PROTOCOL;
        buf->header.type = VD_AGENT_MOUSE_STATE;
        buf->header.opaque = 0;
        buf->header.size = sizeof(VDAgentMouseState);
        ring_add(&reds->agent_state.internal_bufs, &buf->base.link);
    }

    for (i = 0; i < REDS_VDI_PORT_NUM_RECIVE_BUFFS; i++) {
        VDIReadBuf *buf = spice_new0(VDIReadBuf, 1);
        ring_item_init(&buf->link);
        ring_add(&reds->agent_state.read_bufs, &buf->link);
    }
}

const char *version_string = VERSION;

static int do_spice_init(SpiceCoreInterface *core_interface)
{
    red_printf("starting %s", version_string);

    if (core_interface->base.major_version != SPICE_INTERFACE_CORE_MAJOR) {
        red_printf("bad core interface version");
        goto err;
    }
    core = core_interface;
    reds->listen_socket = -1;
    reds->secure_listen_socket = -1;
    reds->stream = NULL;
    reds->in_handler.parser = spice_get_client_channel_parser(SPICE_CHANNEL_MAIN, NULL);
    reds->in_handler.handle_message = reds_main_handle_message;
    ring_init(&reds->outgoing.pipe);
    reds->outgoing.vec = reds->outgoing.vec_buf;

    init_vd_agent_resources();

    if (!(reds->mig_timer = core->timer_add(migrate_timout, NULL))) {
        red_error("migration timer create failed");
    }
    if (!(reds->key_modifiers_timer = core->timer_add(key_modifiers_sender, NULL))) {
        red_error("key modifiers timer create failed");
    }
    if (!(reds->vdi_port_write_timer = core->timer_add(vdi_port_write_retry, NULL)))
    {
        red_error("vdi port write timer create failed");
    }
    reds->vdi_port_write_timer_started = FALSE;

#ifdef RED_STATISTICS
    int shm_name_len = strlen(SPICE_STAT_SHM_NAME) + 20;
    int fd;

    reds->stat_shm_name = (char *)spice_malloc(shm_name_len);
    snprintf(reds->stat_shm_name, shm_name_len, SPICE_STAT_SHM_NAME, getpid());
    if ((fd = shm_open(reds->stat_shm_name, O_CREAT | O_RDWR, 0444)) == -1) {
        red_error("statistics shm_open failed, %s", strerror(errno));
    }
    if (ftruncate(fd, REDS_STAT_SHM_SIZE) == -1) {
        red_error("statistics ftruncate failed, %s", strerror(errno));
    }
    reds->stat = mmap(NULL, REDS_STAT_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (reds->stat == (SpiceStat *)MAP_FAILED) {
        red_error("statistics mmap failed, %s", strerror(errno));
    }
    memset(reds->stat, 0, REDS_STAT_SHM_SIZE);
    reds->stat->magic = SPICE_STAT_MAGIC;
    reds->stat->version = SPICE_STAT_VERSION;
    reds->stat->root_index = INVALID_STAT_REF;
    if (pthread_mutex_init(&reds->stat_lock, NULL)) {
        red_error("mutex init failed");
    }
    if (!(reds->ping_timer = core->timer_add(ping_timer_cb, NULL))) {
        red_error("ping timer create failed");
    }
    reds->ping_interval = PING_INTERVAL;
#endif

    if (!(reds->mm_timer = core->timer_add(mm_timer_proc, NULL))) {
        red_error("mm timer create failed");
    }
    core->timer_start(reds->mm_timer, MM_TIMER_GRANULARITY_MS);

    if (reds_init_net() < 0) {
        goto err;
    }
    if (reds->secure_listen_socket != -1) {
        reds_init_ssl();
    }

#if HAVE_SASL
    int saslerr;
    if ((saslerr = sasl_server_init(NULL, sasl_appname ?
                                    sasl_appname : "spice")) != SASL_OK) {
        red_error("Failed to initialize SASL auth %s",
                  sasl_errstring(saslerr, NULL, NULL));
        goto err;
    }
#endif

    inputs_init();

    reds->mouse_mode = SPICE_MOUSE_MODE_SERVER;
    atexit(reds_exit);
    return 0;

err:
    return -1;
}

/* new interface */
SPICE_GNUC_VISIBLE SpiceServer *spice_server_new(void)
{
    /* we can't handle multiple instances (yet) */
    ASSERT(reds == NULL);

    reds = spice_new0(RedsState, 1);
    return reds;
}

SPICE_GNUC_VISIBLE int spice_server_init(SpiceServer *s, SpiceCoreInterface *core)
{
    int ret;

    ASSERT(reds == s);
    ret = do_spice_init(core);
    if (default_renderer) {
        red_dispatcher_add_renderer(default_renderer);
    }
    return ret;
}

SPICE_GNUC_VISIBLE void spice_server_destroy(SpiceServer *s)
{
    ASSERT(reds == s);
    reds_exit();
}

SPICE_GNUC_VISIBLE spice_compat_version_t spice_get_current_compat_version(void)
{
    return SPICE_COMPAT_VERSION_CURRENT;
}

SPICE_GNUC_VISIBLE int spice_server_set_compat_version(SpiceServer *s,
                                                       spice_compat_version_t version)
{
    if (version < SPICE_COMPAT_VERSION_0_6) {
        /* We don't support 0.4 compat mode atm */
        return -1;
    }

    if (version > SPICE_COMPAT_VERSION_CURRENT) {
        /* Not compatible with future versions */
        return -1;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_port(SpiceServer *s, int port)
{
    ASSERT(reds == s);
    if (port < 0 || port > 0xffff) {
        return -1;
    }
    spice_port = port;
    return 0;
}

SPICE_GNUC_VISIBLE void spice_server_set_addr(SpiceServer *s, const char *addr, int flags)
{
    ASSERT(reds == s);
    strncpy(spice_addr, addr, sizeof(spice_addr));
    if (flags & SPICE_ADDR_FLAG_IPV4_ONLY) {
        spice_family = PF_INET;
    }
    if (flags & SPICE_ADDR_FLAG_IPV6_ONLY) {
        spice_family = PF_INET6;
    }
}

SPICE_GNUC_VISIBLE int spice_server_set_noauth(SpiceServer *s)
{
    ASSERT(reds == s);
    memset(taTicket.password, 0, sizeof(taTicket.password));
    ticketing_enabled = 0;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_sasl(SpiceServer *s, int enabled)
{
    ASSERT(reds == s);
#if HAVE_SASL
    sasl_enabled = enabled;
    return 0;
#else
    return -1;
#endif
}

SPICE_GNUC_VISIBLE int spice_server_set_sasl_appname(SpiceServer *s, const char *appname)
{
    ASSERT(reds == s);
#if HAVE_SASL
    free(sasl_appname);
    sasl_appname = strdup(appname);
    return 0;
#else
    return -1;
#endif
}

SPICE_GNUC_VISIBLE int spice_server_set_ticket(SpiceServer *s,
                                               const char *passwd, int lifetime,
                                               int fail_if_connected,
                                               int disconnect_if_connected)
{
    ASSERT(reds == s);

    if (reds->stream) {
        if (fail_if_connected) {
            return -1;
        }
        if (disconnect_if_connected) {
            reds_disconnect();
        }
    }

    on_activating_ticketing();
    ticketing_enabled = 1;
    if (lifetime == 0) {
        taTicket.expiration_time = INT_MAX;
    } else {
        time_t now = time(NULL);
        taTicket.expiration_time = now + lifetime;
    }
    if (passwd != NULL) {
        strncpy(taTicket.password, passwd, sizeof(taTicket.password));
    } else {
        memset(taTicket.password, 0, sizeof(taTicket.password));
        taTicket.expiration_time = 0;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_tls(SpiceServer *s, int port,
                                            const char *ca_cert_file, const char *certs_file,
                                            const char *private_key_file, const char *key_passwd,
                                            const char *dh_key_file, const char *ciphersuite)
{
    ASSERT(reds == s);
    if (port == 0 || ca_cert_file == NULL || certs_file == NULL ||
        private_key_file == NULL) {
        return -1;
    }
    if (port < 0 || port > 0xffff) {
        return -1;
    }
    memset(&ssl_parameters, 0, sizeof(ssl_parameters));

    spice_secure_port = port;
    strncpy(ssl_parameters.ca_certificate_file, ca_cert_file,
            sizeof(ssl_parameters.ca_certificate_file)-1);
    strncpy(ssl_parameters.certs_file, certs_file,
            sizeof(ssl_parameters.certs_file)-1);
    strncpy(ssl_parameters.private_key_file, private_key_file,
            sizeof(ssl_parameters.private_key_file)-1);

    if (key_passwd) {
        strncpy(ssl_parameters.keyfile_password, key_passwd,
                sizeof(ssl_parameters.keyfile_password)-1);
    }
    if (ciphersuite) {
        strncpy(ssl_parameters.ciphersuite, ciphersuite,
                sizeof(ssl_parameters.ciphersuite)-1);
    }
    if (dh_key_file) {
        strncpy(ssl_parameters.dh_key_file, dh_key_file,
                sizeof(ssl_parameters.dh_key_file)-1);
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_image_compression(SpiceServer *s,
                                                          spice_image_compression_t comp)
{
    ASSERT(reds == s);
    set_image_compression(comp);
    return 0;
}

SPICE_GNUC_VISIBLE spice_image_compression_t spice_server_get_image_compression(SpiceServer *s)
{
    ASSERT(reds == s);
    return image_compression;
}

SPICE_GNUC_VISIBLE int spice_server_set_jpeg_compression(SpiceServer *s, spice_wan_compression_t comp)
{
    ASSERT(reds == s);
    if (comp == SPICE_WAN_COMPRESSION_INVALID) {
        red_printf("invalid jpeg state");
        return -1;
    }
    // todo: support dynamically changing the state
    jpeg_state = comp;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_zlib_glz_compression(SpiceServer *s, spice_wan_compression_t comp)
{
    ASSERT(reds == s);
    if (comp == SPICE_WAN_COMPRESSION_INVALID) {
        red_printf("invalid zlib_glz state");
        return -1;
    }
    // todo: support dynamically changing the state
    zlib_glz_state = comp;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_channel_security(SpiceServer *s, const char *channel, int security)
{
    static const char *names[] = {
        [ SPICE_CHANNEL_MAIN     ] = "main",
        [ SPICE_CHANNEL_DISPLAY  ] = "display",
        [ SPICE_CHANNEL_INPUTS   ] = "inputs",
        [ SPICE_CHANNEL_CURSOR   ] = "cursor",
        [ SPICE_CHANNEL_PLAYBACK ] = "playback",
        [ SPICE_CHANNEL_RECORD   ] = "record",
#ifdef USE_TUNNEL
        [ SPICE_CHANNEL_TUNNEL   ] = "tunnel",
#endif
#ifdef USE_SMARTCARD
        [ SPICE_CHANNEL_SMARTCARD] = "smartcard",
#endif
    };
    int i;

    ASSERT(reds == s);

    if (channel == NULL) {
        default_channel_security = security;
        return 0;
    }
    for (i = 0; i < SPICE_N_ELEMENTS(names); i++) {
        if (names[i] && strcmp(names[i], channel) == 0) {
            set_one_channel_security(i, security);
            return 0;
        }
    }
    return -1;
}

SPICE_GNUC_VISIBLE int spice_server_get_sock_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen)
{
    ASSERT(reds == s);
    if (!reds->stream) {
        return -1;
    }
    if (getsockname(reds->stream->socket, sa, salen) < 0) {
        return -1;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_get_peer_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen)
{
    ASSERT(reds == s);
    if (!reds->stream) {
        return -1;
    }
    if (getpeername(reds->stream->socket, sa, salen) < 0) {
        return -1;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_add_renderer(SpiceServer *s, const char *name)
{
    ASSERT(reds == s);
    if (!red_dispatcher_add_renderer(name)) {
        return -1;
    }
    default_renderer = NULL;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_kbd_leds(SpiceKbdInstance *sin, int leds)
{
    reds_on_keyboard_leds_change(NULL, leds);
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_streaming_video(SpiceServer *s, int value)
{
    ASSERT(reds == s);
    if (value != SPICE_STREAM_VIDEO_OFF &&
        value != SPICE_STREAM_VIDEO_ALL &&
        value != SPICE_STREAM_VIDEO_FILTER)
        return -1;
    streaming_video = value;
    red_dispatcher_on_sv_change();
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_playback_compression(SpiceServer *s, int enable)
{
    ASSERT(reds == s);
    snd_set_playback_compression(enable);
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_agent_mouse(SpiceServer *s, int enable)
{
    ASSERT(reds == s);
    agent_mouse = enable;
    reds_update_mouse_mode();
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_agent_copypaste(SpiceServer *s, int enable)
{
    ASSERT(reds == s);
    agent_copypaste = enable;
    reds->agent_state.write_filter.copy_paste_enabled = agent_copypaste;
    reds->agent_state.read_filter.copy_paste_enabled = agent_copypaste;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_migrate_info(SpiceServer *s, const char* dest,
                                          int port, int secure_port,
                                          const char* cert_subject)
{
    RedsMigSpice *spice_migration = NULL;

    ASSERT(reds == s);

    if ((port == -1 && secure_port == -1) || !dest)
        return -1;

    spice_migration = spice_new0(RedsMigSpice, 1);
    spice_migration->port = port;
    spice_migration->sport = secure_port;
    spice_migration->host = strdup(dest);
    if (cert_subject) {
        spice_migration->cert_subject = strdup(cert_subject);
    }

    reds_mig_release();
    reds->mig_spice = spice_migration;
    return 0;
}

/* interface for seamless migration */
SPICE_GNUC_VISIBLE int spice_server_migrate_start(SpiceServer *s)
{
    ASSERT(reds == s);

    if (1) {
        /* seamless doesn't work, fixing needs protocol change. */
        return -1;
    }

    if (!reds->mig_spice) {
        return -1;
    }
    reds_mig_started();
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_migrate_client_state(SpiceServer *s)
{
    ASSERT(reds == s);

    if (!reds->stream) {
        return SPICE_MIGRATE_CLIENT_NONE;
    } else if (reds->mig_wait_connect) {
        return SPICE_MIGRATE_CLIENT_WAITING;
    } else {
        return SPICE_MIGRATE_CLIENT_READY;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_migrate_end(SpiceServer *s, int completed)
{
    ASSERT(reds == s);
    reds_mig_finished(completed);
    return 0;
}

/* interface for switch-host migration */
SPICE_GNUC_VISIBLE int spice_server_migrate_switch(SpiceServer *s)
{
    ASSERT(reds == s);
    reds_mig_switch();
    return 0;
}

ssize_t reds_stream_read(RedsStream *s, void *buf, size_t nbyte)
{
    ssize_t ret;

#if HAVE_SASL
    if (s->sasl.conn && s->sasl.runSSF) {
        ret = reds_stream_sasl_read(s, buf, nbyte);
    } else
#endif
        ret = s->read(s, buf, nbyte);

    return ret;
}

ssize_t reds_stream_write(RedsStream *s, const void *buf, size_t nbyte)
{
    ssize_t ret;

#if HAVE_SASL
    if (s->sasl.conn && s->sasl.runSSF) {
        ret = reds_stream_sasl_write(s, buf, nbyte);
    } else
#endif
        ret = s->write(s, buf, nbyte);

    return ret;
}

ssize_t reds_stream_writev(RedsStream *s, const struct iovec *iov, int iovcnt)
{
    int i;
    int n;
    ssize_t ret = 0;

    if (s->writev != NULL) {
        return s->writev(s, iov, iovcnt);
    }

    for (i = 0; i < iovcnt; ++i) {
        n = reds_stream_write(s, iov[i].iov_base, iov[i].iov_len);
        if (n <= 0)
            return ret == 0 ? n : ret;
        ret += n;
    }

    return ret;
}

void reds_stream_free(RedsStream *s)
{
    if (!s) {
        return;
    }

    reds_channel_event(s, SPICE_CHANNEL_EVENT_DISCONNECTED);

#if HAVE_SASL
    if (s->sasl.conn) {
        s->sasl.runSSF = s->sasl.wantSSF = 0;
        s->sasl.len = 0;
        s->sasl.encodedLength = s->sasl.encodedOffset = 0;
        s->sasl.encoded = NULL;
        free(s->sasl.mechlist);
        free(s->sasl.mechname);
        s->sasl.mechlist = NULL;
        sasl_dispose(&s->sasl.conn);
        s->sasl.conn = NULL;
    }
#endif

    if (s->ssl) {
        SSL_free(s->ssl);
    }

    reds_stream_remove_watch(s);
    close(s->socket);

    free(s);
}

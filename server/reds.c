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

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "spice.h"
#include "reds.h"
#include <spice/protocol.h>
#include <spice/vd_agent.h>

#include "red_common.h"
#include "red_dispatcher.h"
#include "snd_worker.h"
#include <spice/stats.h>
#include "stat.h"
#include "ring.h"
#include "config.h"
#ifdef HAVE_SLIRP
#include "red_tunnel_worker.h"
#endif

CoreInterface *core = NULL;
static MigrationInterface *mig = NULL;
static KeyboardInterface *keyboard = NULL;
static MouseInterface *mouse = NULL;
static TabletInterface *tablet = NULL;
static VDIPortInterface *vdagent = NULL;

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

static int ticketing_enabled = 1; //Ticketing is enabled by default
static pthread_mutex_t *lock_cs;
static long *lock_count;
uint32_t streaming_video = STREAM_VIDEO_FILTER;
spice_image_compression_t image_compression = SPICE_IMAGE_COMPRESS_AUTO_GLZ;
void *red_tunnel = NULL;
int agent_mouse = TRUE;

static void openssl_init();

#define MIGRATE_TIMEOUT (1000 * 10) /* 10sec */
#define PING_INTERVAL (1000 * 10)
#define KEY_MODIFIERS_TTL (1000 * 2) /*2sec*/
#define MM_TIMER_GRANULARITY_MS (1000 / 30)
#define MM_TIME_DELTA 400 /*ms*/

// approximate max recive message size
#define RECIVE_BUF_SIZE \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

#define SEND_BUF_SIZE 4096

#define SCROLL_LOCK_SCAN_CODE 0x46
#define NUM_LOCK_SCAN_CODE 0x45
#define CAPS_LOCK_SCAN_CODE 0x3a

typedef struct IncomingHandler {
    void *opaque;
    int shut;
    uint8_t buf[RECIVE_BUF_SIZE];
    uint32_t end_pos;
    void (*handle_message)(void *opaque, SpiceDataHeader *message);
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
    void (*prepare)(RedsOutItem *item, struct iovec* vec, int *len);
    void (*release)(RedsOutItem *item);
};

typedef struct VDIReadBuf {
    RedsOutItem out_item;
    int len;
    SpiceDataHeader header;
    uint8_t data[SPICE_AGENT_MAX_DATA_SIZE];
} VDIReadBuf;

enum {
    VDI_PORT_READ_STATE_READ_HADER,
    VDI_PORT_READ_STATE_GET_BUFF,
    VDI_PORT_READ_STATE_READ_DATA,
};

enum {
    VDP_CLIENT_PORT = 1,
    VDP_SERVER_PORT,
};

typedef struct __attribute__ ((__packed__)) VDIChunkHeader {
    uint32_t port;
    uint32_t size;
} VDIChunkHeader;

typedef struct VDIPortState {
    VDIPortPlug plug;
    VDObjectRef plug_ref;
    uint32_t plug_generation;

    uint32_t num_tokens;
    uint32_t num_client_tokens;
    Ring external_bufs;
    Ring internal_bufs;
    Ring write_queue;

    Ring read_bufs;
    uint32_t read_state;
    uint32_t message_recive_len;
    uint8_t *recive_pos;
    uint32_t recive_len;
    VDIReadBuf *current_read_buf;

    VDIChunkHeader vdi_chunk_header;

    int client_agent_started;
    uint32_t send_tokens;
} VDIPortState;

typedef struct InputsState {
    Channel *channel;
    RedsStreamContext *peer;
    uint8_t buf[RECIVE_BUF_SIZE];
    uint32_t end_pos;
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

typedef struct RedsState {
    int listen_socket;
    int secure_listen_socket;
    RedsStreamContext *peer;
    int disconnecting;
    uint32_t link_id;
    uint64_t serial; //migrate me
    VDIPortState agent_state;
    InputsState *inputs_state;

    VDObjectRef mig_notifier;
    int mig_wait_connect;
    int mig_wait_disconnect;
    int mig_inprogress;
    int mig_target;
    int num_of_channels;
    IncomingHandler in_handler;
    RedsOutgoingData outgoing;
    Channel *channels;
    int mouse_mode;
    int is_client_mouse_allowed;
    int dispatcher_allows_client_mouse;
    MonitorMode monitor_mode;
    VDObjectRef mig_timer;
    VDObjectRef key_modifiers_timer;
    VDObjectRef mm_timer;

    TicketAuthentication taTicket;
    SSL_CTX *ctx;

#ifdef RED_STATISTICS
    char *stat_shm_name;
    SpiceStat *stat;
    pthread_mutex_t stat_lock;
    RedsStatValue roundtrip_stat;
    VDObjectRef ping_timer;
    int ping_interval;
#endif
    uint32_t ping_id;
    uint32_t net_test_id;
    int net_test_stage;
    int peer_minor_version;
} RedsState;

uint64_t bitrate_per_sec = ~0;
static uint64_t letancy = 0;

static RedsState *reds = NULL;

typedef struct AsyncRead {
    RedsStreamContext *peer;
    void *opaque;
    uint8_t *now;
    uint8_t *end;
    int active_file_handlers;
    void (*done)(void *opaque);
    void (*error)(void *opaque, int err);
} AsyncRead;

typedef struct RedLinkInfo {
    RedsStreamContext *peer;
    AsyncRead asyc_read;
    SpiceLinkHeader link_header;
    SpiceLinkMess *link_mess;
    int mess_pos;
    TicketInfo tiTicketing;
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

typedef struct PingItem {
    RedsOutItem base;
    SpiceDataHeader header;
    SpiceMsgPing ping;
    int size;
} PingItem;


#define ZERO_BUF_SIZE 4096

static uint8_t zero_page[ZERO_BUF_SIZE] = {0};

static void reds_main_write(void *data);
static void reds_push();

static ChannelSecurityOptions *channels_security = NULL;
static int default_channel_security =
    SPICE_CHANNEL_SECURITY_NONE | SPICE_CHANNEL_SECURITY_SSL;

static RedSSLParameters ssl_parameters;


void (*log_proc)(CoreInterface *core, LogLevel level, const char* component,
                 const char* format, ...) = NULL;

#define LOG_MESSAGE(level, format, ...) {                           \
    if (log_proc) {                                                 \
        log_proc(core, level, "spice", format, ## __VA_ARGS__ );    \
    }                                                               \
}

static int args_is_empty(const VDICmdArg* args)
{
    return !args || args[0].descriptor.type == ARG_TYPE_INVALID;
}

const int args_is_string(const VDICmdArg* args)
{
    return !args_is_empty(args) && args->descriptor.type == ARG_TYPE_STRING;
}

const int args_is_int(const VDICmdArg* args)
{
    return !args_is_empty(args) && args->descriptor.type == ARG_TYPE_INT;
}

static ChannelSecurityOptions *find_channel_security(int id)
{
    ChannelSecurityOptions *now = channels_security;
    while (now && now->channel_id != id) {
        now = now->next;
    }
    return now;
}

static int reds_write(void *ctx, void *buf, size_t size)
{
    int return_code;
    int sock = (long)ctx;
    size_t count = size;

    return_code = write(sock, buf, count);

    return (return_code);
}

static int reds_read(void *ctx, void *buf, size_t size)
{
    int return_code;
    int sock = (long)ctx;
    size_t count = size;

    return_code = read(sock, buf, count);

    return (return_code);
}

static int reds_free(RedsStreamContext *peer)
{
    close(peer->socket);
    free(peer);
    return 0;
}

static int reds_ssl_write(void *ctx, void *buf, size_t size)
{
    int return_code;
    int ssl_error;
    SSL *ssl = ctx;

    return_code = SSL_write(ssl, buf, size);

    if (return_code < 0) {
        ssl_error = SSL_get_error(ssl, return_code);
    }

    return (return_code);
}

static int reds_ssl_read(void *ctx, void *buf, size_t size)
{
    int return_code;
    int ssl_error;
    SSL *ssl = ctx;

    return_code = SSL_read(ssl, buf, size);

    if (return_code < 0) {
        ssl_error = SSL_get_error(ssl, return_code);
    }

    return (return_code);
}

static int reds_ssl_writev(void *ctx, const struct iovec *vector, int count)
{
    int i;
    int n;
    int return_code = 0;
    int ssl_error;
    SSL *ssl = ctx;

    for (i = 0; i < count; ++i) {
        n = SSL_write(ssl, vector[i].iov_base, vector[i].iov_len);
        if (n <= 0) {
            ssl_error = SSL_get_error(ssl, n);
            if (return_code <= 0) {
                return n;
            } else {
                break;
            }
        } else {
            return_code += n;
        }
    }

    return return_code;
}

static int reds_ssl_free(RedsStreamContext *peer)
{
    SSL_free(peer->ssl);
    close(peer->socket);
    free(peer);
    return 0;
}

static void __reds_release_link(RedLinkInfo *link)
{
    ASSERT(link->peer);
    core->set_file_handlers(core, link->peer->socket, NULL, NULL, NULL);
    free(link->link_mess);
    BN_free(link->tiTicketing.bn);
    if (link->tiTicketing.rsa) {
        RSA_free(link->tiTicketing.rsa);
    }
    free(link);
}

static inline void reds_release_link(RedLinkInfo *link)
{
    RedsStreamContext *peer = link->peer;
    __reds_release_link(link);
    peer->cb_free(peer);
}

static void reds_do_disable_ticketing(void)
{
    ticketing_enabled = 0;
    memset(taTicket.password, 0, sizeof(taTicket.password));
    core->term_printf(core, "Ticketing is now disabled.\n");
}

static void reds_do_disable_ticketing_2(const VDICmdArg* args)
{
    if (!args_is_empty(args)) {
        red_printf("invalid args");
        return;
    }

    reds_do_disable_ticketing();
}

static char *base64decode(const char *input, int length)
{
    BIO *b64;
    BIO *bmem;
    int n;
    char *buffer = (char *)spice_malloc0(length);
    char *inbuffer = (char *)spice_malloc0(length + 1);

    memcpy(inbuffer, input, length);
    inbuffer[length] = '\n';

    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new_mem_buf(inbuffer, length + 1);

    if (b64 != NULL && bmem != NULL) {
        bmem = BIO_push(b64, bmem);

        n = BIO_read(bmem, buffer, length);

        if (n != 0) {
            buffer[n - 1] = '\0';
        } else {
            free(buffer);
            buffer = NULL;
        }
    } else {
        free(buffer);
        buffer = NULL;
    }

    BIO_free_all(bmem);

    return buffer;
}

static void reds_do_info_ticket(void)
{
    core->term_printf(core, "Ticket Information:");
    if (ticketing_enabled) {
        if (strlen(taTicket.password) == 0) {
            core->term_printf(core, " blocked\n");
        } else {
            if (taTicket.expiration_time == INT_MAX) {
                core->term_printf(core, " expiration NEVER\n");
            } else {
                time_t now;

                time(&now);
                int expired = taTicket.expiration_time < now;
                if (expired) {
                    core->term_printf(core, " expiration EXPIRED\n");
                } else {
                    core->term_printf(core, " expiration %s\n",
                                      ctime((time_t *)&(taTicket.expiration_time)));
                }
            }
        }
    } else {
        core->term_printf(core, " disabled\n");
    }
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

#define STAT_TAB_LEN 4
#define STAT_VALUE_TABS 7

static void print_stat_tree(uint32_t node_index, int depth)
{
    SpiceStatNode *node = &reds->stat->nodes[node_index];

    if ((node->flags & SPICE_STAT_NODE_MASK_SHOW) == SPICE_STAT_NODE_MASK_SHOW) {
        core->term_printf(core, "%*s%s", depth * STAT_TAB_LEN, "", node->name);
        if (node->flags & SPICE_STAT_NODE_FLAG_VALUE) {
            core->term_printf(core, ":%*s%llu\n",
                              (STAT_VALUE_TABS - depth) * STAT_TAB_LEN - strlen(node->name) - 1, "",
                              node->value);
        } else {
            core->term_printf(core, "\n");
            if (node->first_child_index != INVALID_STAT_REF) {
                print_stat_tree(node->first_child_index, depth + 1);
            }
        }
    }
    if (node->next_sibling_index != INVALID_STAT_REF) {
        print_stat_tree(node->next_sibling_index, depth);
    }
}

static void do_info_statistics()
{
    core->term_printf(core, "Spice Statistics:\n");
    print_stat_tree(reds->stat->root_index, 0);
}

static void do_reset_statistics()
{
    SpiceStatNode *node;
    int i;

    for (i = 0; i <= REDS_MAX_STAT_NODES; i++) {
        node = &reds->stat->nodes[i];
        if (node->flags & SPICE_STAT_NODE_FLAG_VALUE) {
            node->value = 0;
        }
    }
}

static void do_reset_statistics_2(const VDICmdArg* args)
{
    if (!args_is_empty(args)) {
        red_printf("invalid args");
        return;
    }

    do_reset_statistics();
}

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
        core->disarm_timer(core, reds->mig_timer);
        mig->notifier_done(mig, reds->mig_notifier);
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
        ring_add(&state->read_bufs, &state->current_read_buf->out_item.link);
        state->current_read_buf = NULL;
    }
    state->client_agent_started = FALSE;
    state->send_tokens = 0;
}

static void reds_reset_outgoing()
{
    RedsOutgoingData *outgoing = &reds->outgoing;
    RingItem *ring_item;

    if (outgoing->item) {
        outgoing->item->release(outgoing->item);
        outgoing->item = NULL;
    }
    while ((ring_item = ring_get_tail(&outgoing->pipe))) {
        RedsOutItem *out_item = (RedsOutItem *)ring_item;
        ring_remove(ring_item);
        out_item->release(out_item);
    }
    outgoing->vec_size = 0;
    outgoing->vec = outgoing->vec_buf;
}

static void reds_disconnect()
{
    if (!reds->peer || reds->disconnecting) {
        return;
    }

    red_printf("");
    LOG_MESSAGE(VD_LOG_INFO, "user disconnected");
    reds->disconnecting = TRUE;
    reds_reset_outgoing();

    if (reds->agent_state.plug_ref != INVALID_VD_OBJECT_REF) {
        ASSERT(vdagent);
        vdagent->unplug(vdagent, reds->agent_state.plug_ref);
        reds->agent_state.plug_ref = INVALID_VD_OBJECT_REF;
        reds_reset_vdp();
    }

    reds_shatdown_channels();
    core->set_file_handlers(core, reds->peer->socket, NULL, NULL, NULL);
    reds->peer->cb_free(reds->peer);
    reds->peer = NULL;
    reds->in_handler.shut = TRUE;
    reds->link_id = 0;
    reds->serial = 0;
    reds->ping_id = 0;
    reds->net_test_id = 0;
    reds->net_test_stage = NET_TEST_STAGE_INVALID;
    reds->in_handler.end_pos = 0;

    bitrate_per_sec = ~0;
    letancy = 0;

    reds_mig_cleanup();
    reds->disconnecting = FALSE;
}

static void reds_mig_disconnect()
{
    if (reds->peer) {
        reds_disconnect();
    } else {
        reds_mig_cleanup();
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
        n = peer->cb_read(peer->ctx, buf + pos, RECIVE_BUF_SIZE - pos);
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
                buf += sizeof(SpiceDataHeader) + header->size;
                handler->handle_message(handler->opaque, header);

                if (handler->shut) {
                    return -1;
                }
            }
            memmove(handler->buf, buf, (handler->end_pos = end - buf));
        }
    }
}

static int handle_outgoing(RedsStreamContext *peer, OutgoingHandler *handler)
{
    if (!handler->length) {
        return 0;
    }

    while (handler->length) {
        int n;

        n = peer->cb_write(peer->ctx, handler->now, handler->length);
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

static int outgoing_write(RedsStreamContext *peer, OutgoingHandler *handler, void *in_data,
                          int length)
{
    uint8_t *data = in_data;
    ASSERT(length <= SEND_BUF_SIZE);
    if (handler->length) {
        return OUTGOING_BLOCKED;
    }

    while (length) {
        int n = peer->cb_write(peer->ctx, data, length);
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

typedef struct SimpleOutItem {
    RedsOutItem base;
    SpiceDataHeader header;
    uint8_t data[0];
} SimpleOutItem;

static void reds_prepare_basic_out_item(RedsOutItem *in_item, struct iovec* vec, int *len)
{
    SimpleOutItem *item = (SimpleOutItem *)in_item;

    vec[0].iov_base = &item->header;
    vec[0].iov_len = sizeof(item->header);
    if (item->header.size) {
        vec[1].iov_base = item->data;
        vec[1].iov_len = item->header.size;
        *len = 2;
    } else {
        *len = 1;
    }
}

static void reds_free_basic_out_item(RedsOutItem *item)
{
    free(item);
}

static SimpleOutItem *new_simple_out_item(uint32_t type, int message_size)
{
    SimpleOutItem *item;

    item = (SimpleOutItem *)spice_malloc(sizeof(*item) + message_size);
    ring_item_init(&item->base.link);
    item->base.prepare = reds_prepare_basic_out_item;
    item->base.release = reds_free_basic_out_item;

    item->header.serial = ++reds->serial;
    item->header.type = type;
    item->header.size = message_size;
    item->header.sub_list = 0;

    return item;
}

static void reds_push_pipe_item(RedsOutItem *item)
{
    ring_add(&reds->outgoing.pipe, &item->link);
    reds_push();
}

static void reds_send_channels()
{
    SpiceMsgChannels* channels_info;
    SimpleOutItem *item;
    int message_size;
    Channel *channel;
    int i;

    message_size = sizeof(SpiceMsgChannels) + reds->num_of_channels * sizeof(SpiceChannelId);
    item = new_simple_out_item(SPICE_MSG_MAIN_CHANNELS_LIST, message_size);
    channels_info = (SpiceMsgChannels *)item->data;
    channels_info->num_of_channels = reds->num_of_channels;
    channel = reds->channels;

    for (i = 0; i < reds->num_of_channels; i++) {
        ASSERT(channel);
        channels_info->channels[i].type = channel->type;
        channels_info->channels[i].id = channel->id;
        channel = channel->next;
    }
    reds_push_pipe_item(&item->base);
}

static void reds_prepare_ping_item(RedsOutItem *in_item, struct iovec* vec, int *len)
{
    PingItem *item = (PingItem *)in_item;

    vec[0].iov_base = &item->header;
    vec[0].iov_len = sizeof(item->header);
    vec[1].iov_base = &item->ping;
    vec[1].iov_len = sizeof(item->ping);
    int size = item->size;
    int pos = 2;
    while (size) {
        ASSERT(pos < REDS_MAX_SEND_IOVEC);
        int now = MIN(ZERO_BUF_SIZE, size);
        size -= now;
        vec[pos].iov_base = zero_page;
        vec[pos].iov_len = now;
        pos++;
    }
    *len = pos;
}

static void reds_free_ping_item(RedsOutItem *item)
{
    free(item);
}

static int send_ping(int size)
{
    struct timespec time_space;
    PingItem *item;

    if (!reds->peer) {
        return FALSE;
    }
    item = spice_new(PingItem, 1);
    ring_item_init(&item->base.link);
    item->base.prepare = reds_prepare_ping_item;
    item->base.release = reds_free_ping_item;

    item->header.serial = ++reds->serial;
    item->header.type = SPICE_MSG_PING;
    item->header.size = sizeof(item->ping) + size;
    item->header.sub_list = 0;

    item->ping.id = ++reds->ping_id;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    item->ping.timestamp = time_space.tv_sec * 1000000LL + time_space.tv_nsec / 1000LL;

    item->size = size;
    reds_push_pipe_item(&item->base);
    return TRUE;
}

#ifdef RED_STATISTICS

static void do_ping_client(const char *opt, int has_interval, int interval)
{
    if (!reds->peer) {
        red_printf("not connected to peer");
        return;
    }

    if (!opt) {
        send_ping(0);
    } else if (!strcmp(opt, "on")) {
        if (has_interval && interval > 0) {
            reds->ping_interval = interval * 1000;
        }
        core->arm_timer(core, reds->ping_timer, reds->ping_interval);
        core->term_printf(core, "ping on, interval %u s\n", reds->ping_interval / 1000);
    } else if (!strcmp(opt, "off")) {
        core->disarm_timer(core, reds->ping_timer);
        core->term_printf(core, "ping off\n");
    } else {
        core->term_printf(core, "ping invalid option: %s\n", opt);
        return;
    }
}

static void do_ping_client_2(const VDICmdArg* args)
{
    if (args_is_empty(args)) {
        do_ping_client(NULL, FALSE, 0);
        return;
    }

    if (!args_is_string(args)) {
        red_printf("invalid args");
        return;
    }

    if (args_is_empty(&args[1])) {
        do_ping_client(args[0].string_val, FALSE, 0);
        return;
    }

    if (!args_is_int(&args[1])) {
        red_printf("invalid args");
        return;
    }

    do_ping_client(args[0].string_val, TRUE, args[1].int_val);
}

static void ping_timer_cb()
{
    if (!reds->peer) {
        red_printf("not connected to peer, ping off");
        core->disarm_timer(core, reds->ping_timer);
        return;
    }
    do_ping_client(NULL, 0, 0);
    core->arm_timer(core, reds->ping_timer, reds->ping_interval);
}

static void do_info_rtt_client()
{
    core->term_printf(core, "rtt=%uus, min/max/avg=%u/%u/%uus\n", reds->roundtrip_stat.value,
                      reds->roundtrip_stat.min, reds->roundtrip_stat.max,
                      reds->roundtrip_stat.average);
}

#endif

static void reds_send_mouse_mode()
{
    SpiceMsgMainMouseMode *mouse_mode;
    SimpleOutItem *item;

    if (!reds->peer) {
        return;
    }

    item = new_simple_out_item(SPICE_MSG_MAIN_MOUSE_MODE, sizeof(SpiceMsgMainMouseMode));
    mouse_mode = (SpiceMsgMainMouseMode *)item->data;
    mouse_mode->supported_modes = SPICE_MOUSE_MODE_SERVER;
    if (reds->is_client_mouse_allowed) {
        mouse_mode->supported_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    mouse_mode->current_mode = reds->mouse_mode;
    reds_push_pipe_item(&item->base);
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
    SimpleOutItem *item;
    item = new_simple_out_item(SPICE_MSG_MAIN_AGENT_CONNECTED, 0);
    reds_push_pipe_item(&item->base);
}

static void reds_send_agent_disconnected()
{
    SpiceMsgMainAgentDisconnect *disconnect;
    SimpleOutItem *item;

    item = new_simple_out_item(SPICE_MSG_MAIN_AGENT_DISCONNECTED, sizeof(SpiceMsgMainAgentDisconnect));
    disconnect = (SpiceMsgMainAgentDisconnect *)item->data;
    disconnect->error_code = SPICE_LINK_ERR_OK;
    reds_push_pipe_item(&item->base);
}

static void reds_agent_remove()
{
    VDIPortInterface *interface = vdagent;

    vdagent = NULL;
    reds_update_mouse_mode();

    if (!reds->peer || !interface) {
        return;
    }

    ASSERT(reds->agent_state.plug_ref != INVALID_VD_OBJECT_REF);
    interface->unplug(interface, reds->agent_state.plug_ref);
    reds->agent_state.plug_ref = INVALID_VD_OBJECT_REF;

    if (reds->mig_target) {
        return;
    }

    reds_reset_vdp();
    reds_send_agent_disconnected();
}

static void reds_send_tokens()
{
    SpiceMsgMainAgentTokens *tokens;
    SimpleOutItem *item;

    if (!reds->peer) {
        return;
    }

    item = new_simple_out_item(SPICE_MSG_MAIN_AGENT_TOKEN, sizeof(SpiceMsgMainAgentTokens));
    tokens = (SpiceMsgMainAgentTokens *)item->data;
    tokens->num_tokens = reds->agent_state.num_tokens;
    reds->agent_state.num_client_tokens += tokens->num_tokens;
    ASSERT(reds->agent_state.num_client_tokens <= REDS_AGENT_WINDOW_SIZE);
    reds->agent_state.num_tokens = 0;
    reds_push_pipe_item(&item->base);
}

static int write_to_vdi_port()
{
    VDIPortState *state = &reds->agent_state;
    RingItem *ring_item;
    VDIPortBuf *buf;
    int total = 0;
    int n;

    if (reds->agent_state.plug_ref == INVALID_VD_OBJECT_REF || reds->mig_target) {
        return 0;
    }

    for (;;) {
        if (!(ring_item = ring_get_tail(&state->write_queue))) {
            break;
        }
        buf = (VDIPortBuf *)ring_item;
        n = vdagent->write(vdagent, state->plug_ref, buf->now, buf->write_len);
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
    return total;
}

static void dispatch_vdi_port_data(int port, VDIReadBuf *buf)
{
    VDIPortState *state = &reds->agent_state;
    switch (port) {
    case VDP_CLIENT_PORT: {
        buf->header.serial = ++reds->serial;
        buf->header.size = buf->len;
        reds_push_pipe_item(&buf->out_item);
        break;
    }
    case VDP_SERVER_PORT:
        ring_add(&state->read_bufs, &buf->out_item.link);
        break;
    default:
        ring_add(&state->read_bufs, &buf->out_item.link);
        red_printf("invalid port");
        reds_agent_remove();
    }
}

static int read_from_vdi_port()
{
    VDIPortState *state = &reds->agent_state;
    VDIReadBuf *dispatch_buf;
    int total = 0;
    int n;

    if (reds->mig_target) {
        return 0;
    }

    while (reds->agent_state.plug_ref != INVALID_VD_OBJECT_REF) {
        switch (state->read_state) {
        case VDI_PORT_READ_STATE_READ_HADER:
            n = vdagent->read(vdagent, state->plug_ref, state->recive_pos, state->recive_len);
            if (!n) {
                return total;
            }
            total += n;
            if ((state->recive_len -= n)) {
                state->recive_pos += n;
                break;
            }
            state->message_recive_len = state->vdi_chunk_header.size;
            state->read_state = VDI_PORT_READ_STATE_GET_BUFF;
        case VDI_PORT_READ_STATE_GET_BUFF: {
            RingItem *item;

            if (!(item = ring_get_head(&state->read_bufs))) {
                return total;
            }

            if (state->vdi_chunk_header.port == VDP_CLIENT_PORT) {
                if (!state->send_tokens) {
                    return total;
                }
                --state->send_tokens;
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
            n = vdagent->read(vdagent, state->plug_ref, state->recive_pos, state->recive_len);
            if (!n) {
                return total;
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
    return total;
}

static void reds_agent_wakeup(VDIPortPlug *plug)
{
    while (write_to_vdi_port() || read_from_vdi_port());
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

typedef struct SendMainMigrateItem {
    RedsOutItem base;
    SpiceDataHeader header;
    MainMigrateData data;
    WriteQueueInfo queue_info[REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES];
} SendMainMigrateItem;

static void main_channel_send_migrate_data_item(RedsOutItem *in_item, struct iovec* vec_start,
                                                int *len)
{
    SendMainMigrateItem *item = (SendMainMigrateItem *)in_item;
    VDIPortState *state = &reds->agent_state;
    struct iovec* vec;
    int buf_index;
    RingItem *now;

    vec = vec_start;

    item->header.serial = ++reds->serial;
    item->header.type = SPICE_MSG_MIGRATE_DATA;
    item->header.size = sizeof(item->data);
    item->header.sub_list = 0;

    vec[0].iov_base = &item->header;
    vec[0].iov_len = sizeof(item->header);
    vec[1].iov_base = &item->data;
    vec[1].iov_len = sizeof(item->data);

    vec += 2;
    *len = 2;

    item->data.version = MAIN_CHANNEL_MIG_DATA_VERSION;
    item->data.serial = reds->serial;
    item->data.ping_id = reds->ping_id;

    item->data.agent_connected = !!state->plug_ref;
    item->data.client_agent_started = state->client_agent_started;
    item->data.num_client_tokens = state->num_client_tokens;
    item->data.send_tokens = state->send_tokens;

    item->data.read_state = state->read_state;
    item->data.vdi_chunk_header = state->vdi_chunk_header;
    item->data.recive_len = state->recive_len;
    item->data.message_recive_len = state->message_recive_len;


    if (state->current_read_buf) {
        item->data.read_buf_len = state->current_read_buf->len;
        if ((vec->iov_len = item->data.read_buf_len - item->data.recive_len)) {
            vec->iov_base = state->current_read_buf->data;
            item->header.size += vec->iov_len;
            vec++;
            (*len)++;
        }
    } else {
        item->data.read_buf_len = 0;
    }

    now = &state->write_queue;
    item->data.write_queue_size = 0;
    while ((now = ring_prev(&state->write_queue, now))) {
        item->data.write_queue_size++;
    }
    if (!item->data.write_queue_size) {
        return;
    }
    ASSERT(item->data.write_queue_size <= sizeof(item->queue_info) / sizeof(item->queue_info[0]));
    vec->iov_base = item->queue_info;
    vec->iov_len = item->data.write_queue_size * sizeof(item->queue_info[0]);
    item->header.size += vec->iov_len;
    vec++;
    (*len)++;

    buf_index = 0;
    now = &state->write_queue;
    while ((now = ring_prev(&state->write_queue, now))) {
        VDIPortBuf *buf = (VDIPortBuf *)now;
        item->queue_info[buf_index].port = buf->chunk_header.port;
        item->queue_info[buf_index++].len = buf->write_len;
        ASSERT(vec - vec_start < REDS_MAX_SEND_IOVEC);
        vec->iov_base = buf->now;
        vec->iov_len = buf->write_len;
        item->header.size += vec->iov_len;
        vec++;
        (*len)++;
    }
}

static void main_channelrelease_migrate_data_item(RedsOutItem *in_item)
{
    SendMainMigrateItem *item = (SendMainMigrateItem *)in_item;
    free(item);
}

static void main_channel_push_migrate_data_item()
{
    SendMainMigrateItem *item;

    item = spice_new0(SendMainMigrateItem, 1);
    ring_item_init(&item->base.link);
    item->base.prepare = main_channel_send_migrate_data_item;
    item->base.release = main_channelrelease_migrate_data_item;

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
            red_printf("unexpected recive buf");
            reds_disconnect();
            return FALSE;
        }
        state->recive_pos = (uint8_t *)(&state->vdi_chunk_header + 1) - state->recive_len;
        break;
    case VDI_PORT_READ_STATE_GET_BUFF:
        if (state->message_recive_len > state->vdi_chunk_header.size) {
            red_printf("invalid message recive len");
            reds_disconnect();
            return FALSE;
        }

        if (data->read_buf_len) {
            red_printf("unexpected recive buf");
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
            red_printf("invalid message recive len");
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
    state->send_tokens = data->send_tokens;


    if (!data->agent_connected) {
        if (state->plug_ref) {
            reds_send_agent_connected();
        }
        return;
    }

    if (state->plug_ref == INVALID_VD_OBJECT_REF) {
        reds_send_agent_disconnected();
        return;
    }

    if (state->plug_generation > 1) {
        reds_send_agent_disconnected();
        reds_send_agent_connected();
        return;
    }

    state->client_agent_started = data->client_agent_started;

    pos = (uint8_t *)(data + 1);

    if (!main_channel_restore_vdi_read_state(data, &pos, end)) {
        return;
    }

    main_channel_restore_vdi_wqueue(data, pos, end);
    ASSERT(state->num_client_tokens + state->num_tokens == REDS_AGENT_WINDOW_SIZE);
}

static void reds_main_handle_message(void *opaque, SpiceDataHeader *message)
{
    switch (message->type) {
    case SPICE_MSGC_MAIN_AGENT_START: {
        SpiceMsgcMainAgentTokens *agent_start;

        red_printf("agent start");
        if (!reds->peer) {
            return;
        }
        agent_start = (SpiceMsgcMainAgentTokens *)(message + 1);
        reds->agent_state.client_agent_started = TRUE;
        reds->agent_state.send_tokens = agent_start->num_tokens;
        read_from_vdi_port();
        break;
    }
    case SPICE_MSGC_MAIN_AGENT_DATA: {
        RingItem *ring_item;
        VDAgentExtBuf *buf;

        if (!reds->agent_state.num_client_tokens) {
            red_printf("token vailoation");
            reds_disconnect();
            break;
        }
        --reds->agent_state.num_client_tokens;

        if (!vdagent) {
            add_token();
            break;
        }

        if (!reds->agent_state.client_agent_started) {
            red_printf("SPICE_MSGC_MAIN_AGENT_DATA race");
            add_token();
            break;
        }

        if (message->size > SPICE_AGENT_MAX_DATA_SIZE) {
            red_printf("invalid agent message");
            reds_disconnect();
            break;
        }

        if (!(ring_item = ring_get_head(&reds->agent_state.external_bufs))) {
            red_printf("no agent free bufs");
            reds_disconnect();
            break;
        }
        ring_remove(ring_item);
        buf = (VDAgentExtBuf *)ring_item;
        buf->base.now = (uint8_t *)&buf->base.chunk_header.port;
        buf->base.write_len = message->size + sizeof(VDIChunkHeader);
        buf->base.chunk_header.size = message->size;
        memcpy(buf->buf, message + 1, message->size);
        ring_add(&reds->agent_state.write_queue, ring_item);
        write_to_vdi_port();
        break;
    }
    case SPICE_MSGC_MAIN_AGENT_TOKEN: {
        SpiceMsgcMainAgentTokens *token;

        if (!reds->agent_state.client_agent_started) {
            red_printf("SPICE_MSGC_MAIN_AGENT_TOKEN race");
            break;
        }

        token = (SpiceMsgcMainAgentTokens *)(message + 1);
        reds->agent_state.send_tokens += token->num_tokens;
        read_from_vdi_port();
        break;
    }
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
        switch (((SpiceMsgcMainMouseModeRequest *)(message + 1))->mode) {
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
        SpiceMsgPing *ping = (SpiceMsgPing *)(message + 1);
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
                letancy = roundtrip;
                break;
            case NET_TEST_STAGE_RATE:
                reds->net_test_id = 0;
                if (roundtrip <= letancy) {
                    // probably high load on client or server result with incorrect values
                    letancy = 0;
                    red_printf("net test: invalid values, letancy %lu roundtrip %lu. assuming high"
                               "bendwidth", letancy, roundtrip);
                    break;
                }
                bitrate_per_sec = (uint64_t)(NET_TEST_BYTES * 8) * 1000000 / (roundtrip - letancy);
                red_printf("net test: letancy %f ms, bitrate %lu bps (%f Mbps)%s",
                           (double)letancy / 1000,
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
        do_info_rtt_client();
#endif
        break;
    }
    case SPICE_MSGC_MIGRATE_FLUSH_MARK:
        main_channel_push_migrate_data_item();
        break;
    case SPICE_MSGC_MIGRATE_DATA:
        main_channel_recive_migrate_data((MainMigrateData *)(message + 1),
                                         (uint8_t *)(message + 1) + message->size);
        reds->mig_target = FALSE;
        while (write_to_vdi_port() || read_from_vdi_port());
        break;
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        red_printf("unexpected type %d", message->type);
    }
}

static void reds_main_read(void *data)
{
    if (handle_incoming(reds->peer, &reds->in_handler)) {
        reds_disconnect();
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
        if ((n = reds->peer->cb_writev(reds->peer->ctx, outgoing->vec, outgoing->vec_size)) == -1) {
            switch (errno) {
            case EAGAIN:
                core->set_file_handlers(core, reds->peer->socket, reds_main_read, reds_main_write,
                                        NULL);
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
                outgoing->item->release(outgoing->item);
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
    RingItem *item;

    for (;;) {
        if (!reds->peer || outgoing->item || !(item = ring_get_tail(&outgoing->pipe))) {
            return;
        }
        ring_remove(item);
        outgoing->item = (RedsOutItem *)item;
        outgoing->item->prepare(outgoing->item, outgoing->vec_buf, &outgoing->vec_size);
        reds_send_data();
    }
}

static void reds_main_write(void *data)
{
    RedsOutgoingData *outgoing = &reds->outgoing;

    if (reds_send_data()) {
        reds_push();
        if (!outgoing->item) {
            core->set_file_handlers(core, reds->peer->socket, reds_main_read, NULL, NULL);
        }
    }
}

static int sync_write(RedsStreamContext *peer, void *in_buf, size_t n)
{
    uint8_t *buf = (uint8_t *)in_buf;
    while (n) {
        int now = peer->cb_write(peer->ctx, buf, n);
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

static int reds_send_link_ack(RedLinkInfo *link)
{
    SpiceLinkHeader header;
    SpiceLinkReply ack;
    Channel *channel;
    BUF_MEM *bmBuf;
    BIO *bio;
    int ret;

    header.magic = SPICE_MAGIC;
    header.size = sizeof(ack);
    header.major_version = SPICE_VERSION_MAJOR;
    header.minor_version = SPICE_VERSION_MINOR;

    ack.error = SPICE_LINK_ERR_OK;

    if ((channel = reds_find_channel(link->link_mess->channel_type, 0))) {
        ack.num_common_caps = channel->num_common_caps;
        ack.num_channel_caps = channel->num_caps;
        header.size += (ack.num_common_caps + ack.num_channel_caps) * sizeof(uint32_t);
    } else {
        ack.num_common_caps = 0;
        ack.num_channel_caps = 0;
    }
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

    ret = sync_write(link->peer, &header, sizeof(header)) && sync_write(link->peer, &ack,
                                                                        sizeof(ack));
    if (channel) {
        ret = ret && sync_write(link->peer, channel->common_caps,
                                channel->num_common_caps * sizeof(uint32_t)) &&
              sync_write(link->peer, channel->caps, channel->num_caps * sizeof(uint32_t));
    }
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
    return sync_write(link->peer, &header, sizeof(header)) && sync_write(link->peer, &reply,
                                                                         sizeof(reply));
}

static void reds_show_new_channel(RedLinkInfo *link)
{
    red_printf("channel %d:%d, connected sucessfully, over %s link",
               link->link_mess->channel_type,
               link->link_mess->channel_id,
               link->peer->ssl == NULL ? "Non Secure" : "Secure");
}

static void reds_send_link_result(RedLinkInfo *link, uint32_t error)
{
    sync_write(link->peer, &error, sizeof(error));
}

static void reds_start_net_test()
{
    if (!reds->peer || reds->net_test_id) {
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
        reds->agent_state.send_tokens = 0;
        memcpy(&(reds->taTicket), &taTicket, sizeof(reds->taTicket));
        reds->mig_target = FALSE;
    } else {
        if (link->link_mess->connection_id != reds->link_id) {
            reds_send_link_result(link, SPICE_LINK_ERR_BAD_CONNECTION_ID);
            reds_release_link(link);
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
    reds->peer = link->peer;
    reds->in_handler.shut = FALSE;
    if (reds->mig_target) {
        LOG_MESSAGE(VD_LOG_INFO, "migrate connection");
    } else {
        LOG_MESSAGE(VD_LOG_INFO, "new user connection");
    }

    reds_show_new_channel(link);
    __reds_release_link(link);
    if (vdagent) {
        reds->agent_state.plug_ref = vdagent->plug(vdagent, &reds->agent_state.plug);
        if (reds->agent_state.plug_ref == INVALID_VD_OBJECT_REF) {
            PANIC("vdagent plug failed");
        }
        reds->agent_state.plug_generation++;
    }
    core->set_file_handlers(core, reds->peer->socket, reds_main_read, NULL, NULL);

    if (!reds->mig_target) {
        SimpleOutItem *item;
        SpiceMsgMainInit *init;

        item = new_simple_out_item(SPICE_MSG_MAIN_INIT, sizeof(SpiceMsgMainInit));
        init = (SpiceMsgMainInit *)item->data;
        init->session_id = connection_id;
        init->display_channels_hint = red_dispatcher_count();
        init->current_mouse_mode = reds->mouse_mode;
        init->supported_mouse_modes = SPICE_MOUSE_MODE_SERVER;
        if (reds->is_client_mouse_allowed) {
            init->supported_mouse_modes |= SPICE_MOUSE_MODE_CLIENT;
        }
        init->agent_connected = !!vdagent;
        init->agent_tokens = REDS_AGENT_WINDOW_SIZE;
        reds->agent_state.num_client_tokens = REDS_AGENT_WINDOW_SIZE;
        init->multi_media_time = reds_get_mm_time() - MM_TIME_DELTA;
        init->ram_hint = red_dispatcher_qxl_ram_size();
        reds_push_pipe_item(&item->base);
        reds_start_net_test();
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
    core->arm_timer(core, reds->key_modifiers_timer, KEY_MODIFIERS_TTL);
}

static void push_key_scan(uint8_t scan)
{
    if (!keyboard) {
        return;
    }
    keyboard->push_scan_freg(keyboard, scan);
}

static void inputs_handle_input(void *opaque, SpiceDataHeader *header)
{
    InputsState *state = (InputsState *)opaque;
    uint8_t *buf = (uint8_t *)(header + 1);

    switch (header->type) {
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
            push_key_scan(*now);
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_MOTION: {
        SpiceMsgcMouseMotion *mouse_motion = (SpiceMsgcMouseMotion *)buf;

        if (++state->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0) {
            SpiceDataHeader header;

            header.serial = ++state->serial;
            header.type = SPICE_MSG_INPUTS_MOUSE_MOTION_ACK;
            header.size = 0;
            header.sub_list = 0;
            if (outgoing_write(state->peer, &state->out_handler, &header, sizeof(SpiceDataHeader))
                                                                                != OUTGOING_OK) {
                red_printf("motion ack failed");
                reds_disconnect();
            }
        }
        if (mouse && reds->mouse_mode == SPICE_MOUSE_MODE_SERVER) {
            mouse->moution(mouse, mouse_motion->dx, mouse_motion->dy, 0,
                           RED_MOUSE_STATE_TO_LOCAL(mouse_motion->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_POSITION: {
        SpiceMsgcMousePosition *pos = (SpiceMsgcMousePosition *)buf;

        if (++state->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0) {
            SpiceDataHeader header;

            header.serial = ++state->serial;
            header.type = SPICE_MSG_INPUTS_MOUSE_MOTION_ACK;
            header.size = 0;
            header.sub_list = 0;
            if (outgoing_write(state->peer, &state->out_handler, &header, sizeof(SpiceDataHeader))
                                                                                != OUTGOING_OK) {
                red_printf("position ack failed");
                reds_disconnect();
            }
        }
        if (reds->mouse_mode != SPICE_MOUSE_MODE_CLIENT) {
            break;
        }
        ASSERT((agent_mouse && vdagent) || tablet);
        if (!agent_mouse || !vdagent) {
            tablet->position(tablet, pos->x, pos->y, RED_MOUSE_STATE_TO_LOCAL(pos->buttons_state));
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
                tablet->wheel(tablet, dz, RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
            }
        } else if (mouse) {
            mouse->moution(mouse, 0, 0, dz, RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
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
                tablet->buttons(tablet, RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
            }
        } else if (mouse) {
            mouse->buttons(mouse, RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_KEY_MODIFIERS: {
        SpiceMsgcKeyModifiers *modifiers = (SpiceMsgcKeyModifiers *)buf;
        if (!keyboard) {
            break;
        }
        uint8_t leds = keyboard->get_leds(keyboard);
        if ((modifiers->modifiers & SPICE_SCROLL_LOCK_MODIFIER) !=
                                                                (leds & SPICE_SCROLL_LOCK_MODIFIER)) {
            push_key_scan(SCROLL_LOCK_SCAN_CODE);
            push_key_scan(SCROLL_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_NUM_LOCK_MODIFIER) != (leds & SPICE_NUM_LOCK_MODIFIER)) {
            push_key_scan(NUM_LOCK_SCAN_CODE);
            push_key_scan(NUM_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_CAPS_LOCK_MODIFIER) != (leds & SPICE_CAPS_LOCK_MODIFIER)) {
            push_key_scan(CAPS_LOCK_SCAN_CODE);
            push_key_scan(CAPS_LOCK_SCAN_CODE | 0x80);
        }
        activate_modifiers_watch();
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        red_printf("unexpected type %d", header->type);
    }
}

void reds_set_client_mouse_allowed(int is_client_mouse_allowed, int x_res, int y_res)
{
    reds->monitor_mode.x_res = x_res;
    reds->monitor_mode.y_res = y_res;
    reds->dispatcher_allows_client_mouse = is_client_mouse_allowed;
    reds_update_mouse_mode();
    if (reds->is_client_mouse_allowed && tablet) {
        tablet->set_logical_size(tablet, reds->monitor_mode.x_res, reds->monitor_mode.y_res);
    }
}

static void inputs_relase_keys(void)
{
    push_key_scan(0x2a | 0x80); //LSHIFT
    push_key_scan(0x36 | 0x80); //RSHIFT
    push_key_scan(0xe0); push_key_scan(0x1d | 0x80); //RCTRL
    push_key_scan(0x1d | 0x80); //LCTRL
    push_key_scan(0xe0); push_key_scan(0x38 | 0x80); //RALT
    push_key_scan(0x38 | 0x80); //LALT
}

static void inputs_read(void *data)
{
    InputsState *inputs_state = (InputsState *)data;
    if (handle_incoming(inputs_state->peer, &inputs_state->in_handler)) {
        inputs_relase_keys();
        core->set_file_handlers(core, inputs_state->peer->socket, NULL, NULL, NULL);
        if (inputs_state->channel) {
            inputs_state->channel->data = NULL;
            reds->inputs_state = NULL;
        }
        inputs_state->peer->cb_free(inputs_state->peer);
        free(inputs_state);
    }
}

static void inputs_write(void *data)
{
    InputsState *inputs_state = (InputsState *)data;

    red_printf("");
    if (handle_outgoing(inputs_state->peer, &inputs_state->out_handler)) {
        reds_disconnect();
    }
}

static void inputs_shutdown(Channel *channel)
{
    InputsState *state = (InputsState *)channel->data;
    if (state) {
        state->in_handler.shut = TRUE;
        shutdown(state->peer->socket, SHUT_RDWR);
        channel->data = NULL;
        state->channel = NULL;
        reds->inputs_state = NULL;
    }
}

static void inputs_migrate(Channel *channel)
{
    InputsState *state = (InputsState *)channel->data;
    SpiceDataHeader header;
    SpiceMsgMigrate migrate;

    red_printf("");
    header.serial = ++state->serial;
    header.type = SPICE_MSG_MIGRATE;
    header.size = sizeof(migrate);
    header.sub_list = 0;
    migrate.flags = 0;
    if (outgoing_write(state->peer, &state->out_handler, &header, sizeof(header))
                                                                            != OUTGOING_OK ||
        outgoing_write(state->peer, &state->out_handler, &migrate, sizeof(migrate))
                                                                            != OUTGOING_OK) {
        red_printf("write failed");
    }
}

static void inputs_select(void *opaque, int select)
{
    InputsState *inputs_state;
    red_printf("");

    inputs_state = (InputsState *)opaque;
    if (select) {
        core->set_file_handlers(core, inputs_state->peer->socket, inputs_read, inputs_write,
                                inputs_state);
    } else {
        core->set_file_handlers(core, inputs_state->peer->socket, inputs_read, NULL, inputs_state);
    }
}

static void inputs_may_write(void *opaque)
{
    red_printf("");
}

static void inputs_link(Channel *channel, RedsStreamContext *peer, int migration,
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
    if (setsockopt(peer->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    if ((flags = fcntl(peer->socket, F_GETFL)) == -1 ||
                                            fcntl(peer->socket, F_SETFL, flags | O_ASYNC) == -1) {
        red_printf("fcntl failed, %s", strerror(errno));
    }

    inputs_state->peer = peer;
    inputs_state->end_pos = 0;
    inputs_state->channel = channel;
    inputs_state->in_handler.opaque = inputs_state;
    inputs_state->in_handler.handle_message = inputs_handle_input;
    inputs_state->out_handler.length = 0;
    inputs_state->out_handler.opaque = inputs_state;
    inputs_state->out_handler.select = inputs_select;
    inputs_state->out_handler.may_write = inputs_may_write;
    inputs_state->pending_mouse_event = FALSE;
    channel->data = inputs_state;
    reds->inputs_state = inputs_state;
    core->set_file_handlers(core, peer->socket, inputs_read, NULL, inputs_state);

    SpiceDataHeader header;
    SpiceMsgInputsInit inputs_init;
    header.serial = ++inputs_state->serial;
    header.type = SPICE_MSG_INPUTS_INIT;
    header.size = sizeof(SpiceMsgInputsInit);
    header.sub_list = 0;
    inputs_init.keyboard_modifiers = keyboard ? keyboard->get_leds(keyboard) : 0;
    if (outgoing_write(inputs_state->peer, &inputs_state->out_handler, &header,
                       sizeof(SpiceDataHeader)) != OUTGOING_OK ||
        outgoing_write(inputs_state->peer, &inputs_state->out_handler, &inputs_init,
                       sizeof(SpiceMsgInputsInit)) != OUTGOING_OK) {
        red_printf("failed to send modifiers state");
        reds_disconnect();
    }
}

static void reds_send_keyboard_modifiers(uint8_t modifiers)
{
    Channel *channel = reds_find_channel(SPICE_CHANNEL_INPUTS, 0);
    InputsState *state;

    if (!channel || !(state = (InputsState *)channel->data)) {
        return;
    }
    ASSERT(state->peer);
    SpiceDataHeader header;
    SpiceMsgInputsKeyModifiers key_modifiers;
    header.serial = ++state->serial;
    header.type = SPICE_MSG_INPUTS_KEY_MODIFIERS;
    header.size = sizeof(SpiceMsgInputsKeyModifiers);
    header.sub_list = 0;
    key_modifiers.modifiers = modifiers;

    if (outgoing_write(state->peer, &state->out_handler, &header, sizeof(SpiceDataHeader))
                                                                                != OUTGOING_OK ||
        outgoing_write(state->peer, &state->out_handler, &key_modifiers, sizeof(SpiceMsgInputsKeyModifiers))
                                                                                != OUTGOING_OK) {
        red_printf("failed to send modifiers state");
        reds_disconnect();
    }
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
    RedsStreamContext *peer;
    SpiceLinkMess *link_mess;
    uint32_t *caps;

    link_mess = link->link_mess;

    if (!reds->link_id || reds->link_id != link_mess->connection_id) {
        reds_send_link_result(link, SPICE_LINK_ERR_BAD_CONNECTION_ID);
        reds_release_link(link);
        return;
    }

    if (!(channel = reds_find_channel(link_mess->channel_type,
                                      link_mess->channel_id))) {
        reds_send_link_result(link, SPICE_LINK_ERR_CHANNEL_NOT_AVAILABLE);
        reds_release_link(link);
        return;
    }

    reds_send_link_result(link, SPICE_LINK_ERR_OK);
    reds_show_new_channel(link);
    if (link_mess->channel_type == SPICE_CHANNEL_INPUTS && !link->peer->ssl) {
        SimpleOutItem *item;
        SpiceMsgNotify *notify;
        char *mess = "keybord channel is unsecure";
        const int mess_len = strlen(mess);

        LOG_MESSAGE(VD_LOG_WARN, "%s", mess);

        item = new_simple_out_item(SPICE_MSG_NOTIFY, sizeof(SpiceMsgNotify) + mess_len + 1);
        notify = (SpiceMsgNotify *)item->data;
        notify->time_stamp = get_time_stamp();
        notify->severty = SPICE_NOTIFY_SEVERITY_WARN;
        notify->visibilty = SPICE_NOTIFY_VISIBILITY_HIGH;
        notify->what = SPICE_WARN_GENERAL;
        notify->message_len = mess_len;
        memcpy(notify->message, mess, mess_len + 1);
        reds_push_pipe_item(&item->base);
    }
    peer = link->peer;
    link->link_mess = NULL;
    __reds_release_link(link);
    caps = (uint32_t *)((uint8_t *)link_mess + link_mess->caps_offset);
    channel->link(channel, peer, reds->mig_target, link_mess->num_common_caps,
                  link_mess->num_common_caps ? caps : NULL, link_mess->num_channel_caps,
                  link_mess->num_channel_caps ? caps + link_mess->num_common_caps : NULL);
    free(link_mess);
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
            reds_release_link(link);
            return;
        }

        if (expired || strncmp(password, actual_sever_pass, SPICE_MAX_PASSWORD_LENGTH) != 0) {
            reds_send_link_result(link, SPICE_LINK_ERR_PERMISSION_DENIED);
            LOG_MESSAGE(VD_LOG_WARN, "bad connection password or time expired");
            reds_release_link(link);
            return;
        }
    }
    if (link->link_mess->channel_type == SPICE_CHANNEL_MAIN) {
        reds_handle_main_link(link);
    } else {
        reds_handle_other_links(link);
    }
}

static inline void async_read_clear_handlers(AsyncRead *obj)
{
    if (!obj->active_file_handlers) {
        return;
    }
    obj->active_file_handlers = FALSE;
    core->set_file_handlers(core, obj->peer->socket, NULL, NULL, NULL);
}

static void async_read_handler(void *data)
{
    AsyncRead *obj = (AsyncRead *)data;

    for (;;) {
        int n = obj->end - obj->now;

        ASSERT(n > 0);
        if ((n = obj->peer->cb_read(obj->peer->ctx, obj->now, n)) <= 0) {
            if (n < 0) {
                switch (errno) {
                case EAGAIN:
                    if (!obj->active_file_handlers) {
                        obj->active_file_handlers = TRUE;
                        core->set_file_handlers(core, obj->peer->socket, async_read_handler, NULL,
                                                obj);
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

static int reds_security_check(RedLinkInfo *link)
{
    ChannelSecurityOptions *security_option = find_channel_security(link->link_mess->channel_type);
    uint32_t security = security_option ? security_option->options : default_channel_security;
    return (link->peer->ssl && (security & SPICE_CHANNEL_SECURITY_SSL)) ||
        (!link->peer->ssl && (security & SPICE_CHANNEL_SECURITY_NONE));
}

static void reds_handle_read_link_done(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    SpiceLinkMess *link_mess = link->link_mess;
    AsyncRead *obj = &link->asyc_read;
    uint32_t num_caps = link_mess->num_common_caps + link_mess->num_channel_caps;

    if (num_caps && (num_caps * sizeof(uint32_t) + link_mess->caps_offset >
                                                   link->link_header.size ||
                                                     link_mess->caps_offset < sizeof(*link_mess))) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_release_link(link);
        return;
    }

    if (!reds_security_check(link)) {
        if (link->peer->ssl) {
            LOG_MESSAGE(VD_LOG_INFO, "channels of type %d should connect only over "
                                     "a non secure link", link_mess->channel_type);
            red_printf("spice channels %d should not be encrypted", link_mess->channel_type);
            reds_send_link_error(link, SPICE_LINK_ERR_NEED_UNSECURED);
        } else {
            LOG_MESSAGE(VD_LOG_INFO, "channels of type %d should connect only over "
                                     "a secure link", link_mess->channel_type);
            red_printf("spice channels %d should be encrypted", link_mess->channel_type);
            reds_send_link_error(link, SPICE_LINK_ERR_NEED_SECURED);
        }
        reds_release_link(link);
        return;
    }

    if (!reds_send_link_ack(link)) {
        reds_release_link(link);
        return;
    }

    obj->now = (uint8_t *)&link->tiTicketing.encrypted_ticket.encrypted_data;
    obj->end = obj->now + link->tiTicketing.rsa_size;
    obj->done = reds_handle_ticket;
    async_read_handler(&link->asyc_read);
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
    reds_release_link(link);
}

static void reds_handle_read_header_done(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    SpiceLinkHeader *header = &link->link_header;
    AsyncRead *obj = &link->asyc_read;

    if (header->magic != SPICE_MAGIC) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_MAGIC);
        LOG_MESSAGE(VD_LOG_ERROR, "bad magic %u", header->magic);
        reds_release_link(link);
        return;
    }

    if (header->major_version != SPICE_VERSION_MAJOR) {
        if (header->major_version > 0) {
            reds_send_link_error(link, SPICE_LINK_ERR_VERSION_MISMATCH);
        }
        LOG_MESSAGE(VD_LOG_INFO, "version mismatch client %u.%u server %u.%u",
                    header->major_version,
                    header->minor_version,
                    SPICE_VERSION_MAJOR,
                    SPICE_VERSION_MINOR);

        red_printf("version mismatch");
        reds_release_link(link);
        return;
    }

    reds->peer_minor_version = header->minor_version;

    if (header->size < sizeof(SpiceLinkMess)) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        red_printf("bad size %u", header->size);
        reds_release_link(link);
        return;
    }

    link->link_mess = spice_malloc(header->size);

    obj->now = (uint8_t *)link->link_mess;
    obj->end = obj->now + header->size;
    obj->done = reds_handle_read_link_done;
    async_read_handler(&link->asyc_read);
}

static void reds_handle_new_link(RedLinkInfo *link)
{
    AsyncRead *obj = &link->asyc_read;
    obj->opaque = link;
    obj->peer = link->peer;
    obj->now = (uint8_t *)&link->link_header;
    obj->end = (uint8_t *)((SpiceLinkHeader *)&link->link_header + 1);
    obj->active_file_handlers = FALSE;
    obj->done = reds_handle_read_header_done;
    obj->error = reds_handle_link_error;
    async_read_handler(&link->asyc_read);
}

static void reds_handle_ssl_accept(void *data)
{
    RedLinkInfo *link = (RedLinkInfo *)data;
    int return_code;

    if ((return_code = SSL_accept(link->peer->ssl)) != 1) {
        int ssl_error = SSL_get_error(link->peer->ssl, return_code);

        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
            red_printf("SSL_accept failed, error=%d", ssl_error);
            reds_release_link(link);
        }
        return;
    }
    reds_handle_new_link(link);
}

static RedLinkInfo *__reds_accept_connection(int listen_socket)
{
    RedLinkInfo *link;
    RedsStreamContext *peer;
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
    peer = spice_new0(RedsStreamContext, 1);
    link->peer = peer;
    peer->socket = socket;
    openssl_init(link);

    return link;

error:
    close(socket);

    return NULL;
}

static RedLinkInfo *reds_accept_connection(int listen_socket)
{
    RedLinkInfo *link;
    RedsStreamContext *peer;

    if (!(link = __reds_accept_connection(listen_socket))) {
        return NULL;
    }
    peer = link->peer;
    peer->ctx = (void *)((unsigned long)link->peer->socket);
    peer->cb_read = (int (*)(void *, void *, int))reds_read;
    peer->cb_write = (int (*)(void *, void *, int))reds_write;
    peer->cb_readv = (int (*)(void *, const struct iovec *vector, int count))readv;
    peer->cb_writev = (int (*)(void *, const struct iovec *vector, int count))writev;
    peer->cb_free = (int (*)(RedsStreamContext *))reds_free;

    return link;
}

static void reds_accept_ssl_connection(void *data)
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
    if (!(sbio = BIO_new_socket(link->peer->socket, BIO_NOCLOSE))) {
        red_printf("could not allocate ssl bio socket");
        goto error;
    }

    link->peer->ssl = SSL_new(reds->ctx);
    if (!link->peer->ssl) {
        red_printf("could not allocate ssl context");
        BIO_free(sbio);
        goto error;
    }

    SSL_set_bio(link->peer->ssl, sbio, sbio);

    link->peer->ctx = (void *)(link->peer->ssl);
    link->peer->cb_write = (int (*)(void *, void *, int))reds_ssl_write;
    link->peer->cb_read = (int (*)(void *, void *, int))reds_ssl_read;
    link->peer->cb_readv = NULL;
    link->peer->cb_writev = reds_ssl_writev;
    link->peer->cb_free = (int (*)(RedsStreamContext *))reds_ssl_free;

    return_code = SSL_accept(link->peer->ssl);
    if (return_code == 1) {
        reds_handle_new_link(link);
        return;
    }

    ssl_error = SSL_get_error(link->peer->ssl, return_code);
    if (return_code == -1 && (ssl_error == SSL_ERROR_WANT_READ ||
                              ssl_error == SSL_ERROR_WANT_WRITE)) {
        core->set_file_handlers(core, link->peer->socket, reds_handle_ssl_accept,
                                reds_handle_ssl_accept, link);
        return;
    }

    ERR_print_errors_fp(stderr);
    red_printf("SSL_accept failed, error=%d", ssl_error);
    SSL_free(link->peer->ssl);

error:
    close(link->peer->socket);
    free(link->peer);
    BN_free(link->tiTicketing.bn);
    free(link);
}

static void reds_accept(void *data)
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
        red_error("getaddrinfo(%s,%s): %s\n", addr, port,
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
    red_error("%s: binding socket to %s:%d failed\n", __FUNCTION__,
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

static void reds_init_net()
{
    if (spice_port != -1) {
        reds->listen_socket = reds_init_socket(spice_addr, spice_port, spice_family);
        if (core->set_file_handlers(core, reds->listen_socket, reds_accept, NULL, NULL)) {
            red_error("set fd handle failed");
        }
    }

    if (spice_secure_port != -1) {
        reds->secure_listen_socket = reds_init_socket(spice_addr, spice_secure_port,
                                                      spice_family);
        if (core->set_file_handlers(core, reds->secure_listen_socket,
                                    reds_accept_ssl_connection, NULL, NULL)) {
            red_error("set fd handle failed");
        }
    }
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
    if (reds->peer) {
        close(reds->peer->socket);
    }
#ifdef RED_STATISTICS
    shm_unlink(reds->stat_shm_name);
    free(reds->stat_shm_name);
#endif
    unsetenv("QEMU_AUDIO_DRV");
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

static int find_option(const char *str, OptionsMap *options_map)
{
    int i = 0;

    for (i = 0; options_map[i].name != NULL; i++) {
        if (strcmp(str, options_map[i].name) == 0) {
            return options_map[i].val;
        }
    }
    return SPICE_OPTION_INVALID;
}

static void clear_blanks(char **ptr)
{
    char *str = *ptr;
    while (isspace(*str)) {
        str++;
    }
    while (isspace(str[strlen(str) - 1])) {
        str[strlen(str) - 1] = 0;
    }
    *ptr = str;
}

static int get_option(char **args, char **out_val, OptionsMap *map, char seperator)
{
    char *p;
    char *next;
    char *val;

    ASSERT(args && out_val);

    p = *args;
    if ((next = strchr(p, seperator))) {
        *next = 0;
        *args = next + 1;
    } else {
        *args = NULL;
    }

    if ((val = strchr(p, '='))) {
        *(val++) = 0;
        clear_blanks(&val);
        *out_val = (strlen(val) == 0) ? NULL : val;
    } else {
        *out_val = NULL;
    }

    clear_blanks(&p);
    return find_option(p, map);
}

enum {
    SPICE_TICKET_OPTION_INVALID,
    SPICE_TICKET_OPTION_EXPIRATION,
    SPICE_TICKET_OPTION_CONNECTED,
};

static OptionsMap _spice_ticket_options[] = {
    {"expiration", SPICE_TICKET_OPTION_EXPIRATION},
    {"connected", SPICE_TICKET_OPTION_CONNECTED},
    {NULL, 0},
};

static inline void on_activating_ticketing()
{
    if (!ticketing_enabled && reds->peer) {
        red_printf("disconnecting");
        reds_disconnect();
    }
}

static void reds_reset_ticketing()
{
    on_activating_ticketing();
    ticketing_enabled = 1;
    taTicket.expiration_time = 0;
    memset(taTicket.password, 0, sizeof(taTicket.password));
}

static void reds_set_ticketing(const char *pass, long expiration)
{
    ASSERT(expiration >= 0);
    on_activating_ticketing();
    ticketing_enabled = 1;
    if (expiration == 0) {
        taTicket.expiration_time = INT_MAX;
    } else {
        time_t ltime;

        time(&ltime);
        taTicket.expiration_time = ltime + expiration;
    }
    strncpy(taTicket.password, pass, sizeof(taTicket.password));
}

static void reds_do_set_ticket(const char *password, const char *args)
{
    long expiration = 0;
    char *local_args = NULL;
    const char *term_str = "invalid args";
    int disconnect = FALSE;
    int fail = FALSE;

    if (!password) {
        term_str = "unexpected NULL password";
        goto error;
    }

    if (args) {
        char *in_args;
        int option;
        char *val;

        in_args = local_args = spice_strdup(args);
        do {
            switch (option = get_option(&in_args, &val, _spice_ticket_options, ',')) {
            case SPICE_TICKET_OPTION_EXPIRATION: {
                char *endptr;

                if (!val) {
                    goto error;
                }
                expiration = strtol(val, &endptr, 0);
                if (endptr != val + strlen(val) || expiration < 0) {
                    term_str = "invalid expiration";
                    goto error;
                }
                break;
            }
            case SPICE_TICKET_OPTION_CONNECTED:
                if (!val) {
                    goto error;
                }

                if (strcmp(val, "disconnect") == 0) {
                    disconnect = TRUE;
                    fail = FALSE;
                } else if (strcmp(val, "fail") == 0) {
                    fail = TRUE;
                    disconnect = FALSE;
                } else if (strcmp(val, "keep") == 0) {
                    fail = FALSE;
                    disconnect = FALSE;
                } else {
                    goto error;
                }
                break;
            default:
                goto error;
            }
        } while (in_args);
    }

    if (fail && reds->peer) {
        term_str = "Ticket set failed";
    } else {
        if (disconnect) {
            reds_disconnect();
        }
        reds_set_ticketing(password, expiration);
        term_str = "Ticket set successfully";
    }
    core->term_printf(core, "%s\n", term_str);
    free(local_args);
    return;

error:
    reds_reset_ticketing();
    core->term_printf(core, "%s\n", term_str);
    free(local_args);
}

static void reds_do_set_ticket_2(const VDICmdArg *args)
{
    const char *arg2 = NULL;

    if (!args_is_string(args)) {
        red_printf("invalid args");
        return;
    }

    if (!args_is_empty(&args[1])) {
        if (!args_is_string(&args[1])) {
            red_printf("invalid args");
            return;
        }
        arg2 = args[1].string_val;
    }

    reds_do_set_ticket(args[0].string_val, arg2);
}

static void reds_do_set_ticket64(const char *password64, const char *args)
{
    char *password;

    if (!password64) {
        reds_reset_ticketing();
        core->term_printf(core, "unexpected NULL password\n");
        return;
    }

    if (!(password = base64decode(password64, strlen(password64)))) {
        reds_reset_ticketing();
        core->term_printf(core, "set_ticket64 failed!\n");
        return;
    }
    reds_do_set_ticket(password, args);
    free(password);
}

static void reds_do_set_ticket64_2(const VDICmdArg *args)
{
    const char *arg2 = NULL;

    if (!args_is_string(args)) {
        red_printf("invalid args");
        return;
    }

    if (!args_is_empty(&args[1])) {
        if (!args_is_string(&args[1])) {
            red_printf("invalid args");
            return;
        }
        arg2 = args[1].string_val;
    }

    reds_do_set_ticket64(args[0].string_val, arg2);
}

static void reds_do_info_spice()
{
    core->term_printf(core, "spice info:");
    if (reds->peer) {
        char *ip = NULL;
        struct sockaddr_in sock_addr;
        socklen_t len = sizeof(sock_addr);
        if (getpeername(reds->peer->socket, (struct sockaddr *)&sock_addr, &len) != -1) {
            ip = inet_ntoa(sock_addr.sin_addr);
        }
        core->term_printf(core, " client=%s", ip);
    } else {
        core->term_printf(core, " disconnected");
    }
    core->term_printf(core, " ticketing=%s", ticketing_enabled ? "on" : "off");
    switch (image_compression) {
    case SPICE_IMAGE_COMPRESS_AUTO_GLZ:
        core->term_printf(core, " ic=auto_glz");
        break;
    case SPICE_IMAGE_COMPRESS_AUTO_LZ:
        core->term_printf(core, " ic=auto_lz");
        break;
    case SPICE_IMAGE_COMPRESS_QUIC:
        core->term_printf(core, " ic=quic");
        break;
    case SPICE_IMAGE_COMPRESS_LZ:
        core->term_printf(core, " ic=lz");
        break;
    case SPICE_IMAGE_COMPRESS_GLZ:
        core->term_printf(core, " ic=glz");
        break;
    case SPICE_IMAGE_COMPRESS_OFF:
        core->term_printf(core, " ic=off");
        break;
    case SPICE_IMAGE_COMPRESS_INVALID:
    default:
        core->term_printf(core, " ic=invalid");
    }

    switch (streaming_video) {
        case STREAM_VIDEO_ALL:
            core->term_printf(core, " sv=all");
            break;
        case STREAM_VIDEO_FILTER:
            core->term_printf(core, " sv=filter");
            break;
        case STREAM_VIDEO_OFF:
            core->term_printf(core, " sv=off");
            break;
        case STREAM_VIDEO_INVALID:
        default:
            core->term_printf(core, " sv=invalid");

    }
    core->term_printf(core, " playback-compression=%s\n",
                      snd_get_playback_compression() ? "on" : "off");
}

static void set_image_compression(spice_image_compression_t val)
{
    if (val == image_compression) {
        return;
    }
    image_compression = val;
    red_dispatcher_on_ic_change();
}

static spice_image_compression_t reds_get_image_compression(const char *val)
{
    if ((strcmp(val, "on") == 0) || (strcmp(val, "auto_glz") == 0)) {
        return SPICE_IMAGE_COMPRESS_AUTO_GLZ;
    } else if (strcmp(val, "auto_lz") == 0) {
        return SPICE_IMAGE_COMPRESS_AUTO_LZ;
    } else if (strcmp(val, "quic") == 0) {
        return SPICE_IMAGE_COMPRESS_QUIC;
    } else if (strcmp(val, "glz") == 0) {
        return SPICE_IMAGE_COMPRESS_GLZ;
    } else if (strcmp(val, "lz") == 0) {
        return SPICE_IMAGE_COMPRESS_LZ;
    } else if (strcmp(val, "off") == 0) {
        return SPICE_IMAGE_COMPRESS_OFF;
    }
    return SPICE_IMAGE_COMPRESS_INVALID;
}

static void reds_do_set_image_compression(const char *val)
{
    spice_image_compression_t real_val = reds_get_image_compression(val);
    if (real_val == SPICE_IMAGE_COMPRESS_INVALID) {
        core->term_printf(core, "bad image compression arg\n");
        return;
    }
    set_image_compression(real_val);
}

static void reds_do_set_image_compression_2(const VDICmdArg *args)
{
    if (!args_is_string(args)) {
        red_printf("invalid args");
        return;
    }

    reds_do_set_image_compression(args[0].string_val);
}

static int reds_get_streaming_video(const char *val)
{
    if (strcmp(val, "on") == 0) {
        return STREAM_VIDEO_FILTER;
    } else if (strcmp(val, "filter") == 0) {
        return STREAM_VIDEO_FILTER;
    } else if (strcmp(val, "all") == 0) {
        return STREAM_VIDEO_ALL;
    } else if (strcmp(val, "off") == 0){
        return STREAM_VIDEO_OFF;
    } else {
        return STREAM_VIDEO_INVALID;
    }
}

static void reds_do_set_streaming_video(const char *val)
{
    uint32_t new_val = reds_get_streaming_video(val);
    if (new_val == STREAM_VIDEO_INVALID) {
        core->term_printf(core, "bad streaming video arg\n");
        return;
    }

    if (new_val == streaming_video) {
        return;
    }
    streaming_video = new_val;
    red_dispatcher_on_sv_change();
}

static void reds_do_set_streaming_video_2(const VDICmdArg *args)
{
    if (!args_is_string(args)) {
        red_printf("invalid args");
        return;
    }

    reds_do_set_streaming_video(args[0].string_val);
}

static void reds_do_set_agent_mouse(const char *val)
{
    int new_val;
    if (strcmp(val, "on") == 0) {
        new_val = TRUE;
    } else if (strcmp(val, "off") == 0) {
        new_val = FALSE;
    } else {
        core->term_printf(core, "bad agent mouse arg\n");
        return;
    }
    if (new_val == agent_mouse) {
        return;
    }
    agent_mouse = new_val;
    reds_update_mouse_mode();
}

static void reds_do_set_agent_mouse_2(const VDICmdArg *args)
{
    if (!args_is_string(args)) {
        red_printf("invalid args");
        return;
    }

    reds_do_set_agent_mouse(args[0].string_val);
}

static void reds_do_set_playback_compression(const char *val)
{
    int on;
    if (strcmp(val, "on") == 0) {
        on = TRUE;
    } else if (strcmp(val, "off") == 0) {
        on = FALSE;
    } else {
        core->term_printf(core, "bad playback compression arg\n");
        return;
    }
    snd_set_playback_compression(on);
}

static void reds_do_set_playback_compression_2(const VDICmdArg *args)
{
    if (!args_is_string(args)) {
        red_printf("invalid args");
        return;
    }

    reds_do_set_playback_compression(args[0].string_val);
}

static OptionsMap _spice_options[] = {
    {"port", SPICE_OPTION_PORT},
    {"sport", SPICE_OPTION_SPORT},
    {"host", SPICE_OPTION_HOST},
    {"ic", SPICE_OPTION_IMAGE_COMPRESSION},
    {"password", SPICE_OPTION_PASSWORD},
    {"disable-ticketing", SPICE_OPTION_DISABLE_TICKET},
    {"renderer", SPICE_OPTION_RENDERER},
    {"sslkey", SPICE_OPTION_SSLKEY},
    {"sslcert", SPICE_OPTION_SSLCERTS},
    {"sslcafile", SPICE_OPTION_SSLCAFILE},
    {"ssldhfile", SPICE_OPTION_SSLDHFILE},
    {"sslpassword", SPICE_OPTION_SSLPASSWORD},
    {"sslciphersuite", SPICE_OPTION_SSLCIPHERSUITE},
    {"secure-channels", SPICE_SECURED_CHANNELS},
    {"unsecure-channels", SPICE_UNSECURED_CHANNELS},
    {"sv", SPICE_OPTION_STREAMING_VIDEO},
    {"agent-mouse", SPICE_OPTION_AGENT_MOUSE},
    {"playback-compression", SPICE_OPTION_PLAYBACK_COMPRESSION},
    {NULL, 0},
};

static OptionsMap _channel_map[] = {
    {"all", SPICE_CHANNEL_NAME_ALL},
    {"main", SPICE_CHANNEL_NAME_MAIN},
    {"display", SPICE_CHANNEL_NAME_DISPLAY},
    {"inputs", SPICE_CHANNEL_NAME_INPUTS},
    {"cursor", SPICE_CHANNEL_NAME_CURSOR},
    {"playback", SPICE_CHANNEL_NAME_PLAYBACK},
    {"record", SPICE_CHANNEL_NAME_RECORD},
    {NULL, 0},
};

static void set_all_channels_security(uint32_t security)
{
    while (channels_security) {
        ChannelSecurityOptions *temp = channels_security;
        channels_security = channels_security->next;
        free(temp);
    }
    default_channel_security = security;
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

static int set_channels_security(const char *channels, uint32_t security)
{
    char *local_str;
    int channel_name;
    char *str;
    char *val;
    int all = 0;
    int specific = 0;

    local_str = spice_strdup(channels);
    str = local_str;
    do {
        switch (channel_name = get_option(&str, &val, _channel_map, '+')) {
        case SPICE_CHANNEL_NAME_ALL:
            all++;
            break;
        case SPICE_CHANNEL_NAME_MAIN:
            specific++;
            set_one_channel_security(SPICE_CHANNEL_MAIN, security);
            break;
        case SPICE_CHANNEL_NAME_DISPLAY:
            specific++;
            set_one_channel_security(SPICE_CHANNEL_DISPLAY, security);
            break;
        case SPICE_CHANNEL_NAME_INPUTS:
            specific++;
            set_one_channel_security(SPICE_CHANNEL_INPUTS, security);
            break;
        case SPICE_CHANNEL_NAME_CURSOR:
            specific++;
            set_one_channel_security(SPICE_CHANNEL_CURSOR, security);
            break;
        case SPICE_CHANNEL_NAME_PLAYBACK:
            specific++;
            set_one_channel_security(SPICE_CHANNEL_PLAYBACK, security);
            break;
        case SPICE_CHANNEL_NAME_RECORD:
            specific++;
            set_one_channel_security(SPICE_CHANNEL_RECORD, security);
            break;
        default:
            goto error;
        }
        if (val) {
            goto error;
        }
    } while (str);

    if (all) {
        if (specific || all > 1) {
            goto error;
        }
        set_all_channels_security(security);
        return TRUE;
    }
    return TRUE;

error:
    free(local_str);
    return FALSE;
}

int __attribute__ ((visibility ("default"))) spice_parse_args(const char *in_args)
{
    char *local_args;
    char *args;
    int option;
    char *val;
    int renderers_opt = FALSE;

    int ssl_port = FALSE;
    int ssl_key = FALSE;
    int ssl_certs = FALSE;
    int ssl_ciphersuite = FALSE;
    int ssl_cafile = FALSE;
    int ssl_dhfile = FALSE;

    memset(&ssl_parameters, 0, sizeof(ssl_parameters));

    local_args = spice_strdup(in_args);

    args = local_args;
    do {
        switch (option = get_option(&args, &val, _spice_options, ',')) {
        case SPICE_OPTION_PORT: {
            char *endptr;
            long int port;

            if (!val) {
                goto error;
            }
            port = strtol(val, &endptr, 0);
            if (endptr != val + strlen(val) || port < 0 || port > 0xffff) {
                goto error;
            }
            spice_port = port;
            break;
        }
        case SPICE_OPTION_SPORT: {
            char *endptr;
            long int port;

            if (!val) {
                goto error;
            }
            port = strtol(val, &endptr, 0);
            if (endptr != val + strlen(val) || port < 0 || port > 0xffff) {
                goto error;
            }

            ssl_port = TRUE;
            spice_secure_port = port;
            break;
        }
        case SPICE_OPTION_HOST: {
            if (val) {
                strncpy(spice_addr, val, sizeof(spice_addr));
                /* force ipv4 here for backward compatibility */
                spice_family = PF_INET;
            }
            break;
        }
        case SPICE_OPTION_IMAGE_COMPRESSION:
            if (!val) {
                goto error;
            }
            image_compression = reds_get_image_compression(val);
            if (image_compression == SPICE_IMAGE_COMPRESS_INVALID) {
                goto error;
            }
            break;
        case SPICE_OPTION_PASSWORD:
            ticketing_enabled = 1;

            if (val) {
                strncpy(taTicket.password, val, sizeof taTicket.password);
                //todo: add expiration option
                taTicket.expiration_time = INT_MAX;
            }

            break;
        case SPICE_OPTION_DISABLE_TICKET:
            ticketing_enabled = 0;
            break;
        case SPICE_OPTION_RENDERER:
            renderers_opt = TRUE;
            if (!val) {
                goto error;
            }
            while (val) {
                char *now = val;
                if ((val = strchr(now, '+'))) {
                    *val++ = 0;
                }
                if (!red_dispatcher_add_renderer(now)) {
                    goto error;
                }
            }

            break;
        case SPICE_OPTION_SSLCIPHERSUITE:
            ssl_ciphersuite = TRUE;

            if (val) {
                strncpy(ssl_parameters.ciphersuite, val, sizeof(ssl_parameters.ciphersuite));
            }

            break;
        case SPICE_OPTION_SSLPASSWORD:
            if (val) {
                strncpy(ssl_parameters.keyfile_password, val,
                        sizeof(ssl_parameters.keyfile_password));
            }
            break;
        case SPICE_OPTION_SSLKEY:
            ssl_key = TRUE;

            if (val) {
                strncpy(ssl_parameters.private_key_file, val,
                        sizeof(ssl_parameters.private_key_file));
            }
            break;
        case SPICE_OPTION_SSLCERTS:
            ssl_certs = TRUE;

            if (val) {
                strncpy(ssl_parameters.certs_file, val, sizeof(ssl_parameters.certs_file));
            }
            break;
        case SPICE_OPTION_SSLCAFILE:
            ssl_cafile = TRUE;

            if (val) {
                strncpy(ssl_parameters.ca_certificate_file, val,
                        sizeof(ssl_parameters.ca_certificate_file));
            }
            break;
        case SPICE_OPTION_SSLDHFILE:
            ssl_dhfile = TRUE;

            if (val) {
                strncpy(ssl_parameters.dh_key_file, val, sizeof(ssl_parameters.dh_key_file));
            }
            break;
        case SPICE_SECURED_CHANNELS:
            if (!val || !set_channels_security(val, SPICE_CHANNEL_SECURITY_SSL)) {
                goto error;
            }
            break;
        case SPICE_UNSECURED_CHANNELS:
            if (!val || !set_channels_security(val, SPICE_CHANNEL_SECURITY_NONE)) {
                goto error;
            }
            break;
        case SPICE_OPTION_STREAMING_VIDEO:
            if (!val) {
                goto error;
            }
            streaming_video = reds_get_streaming_video(val);
            if (streaming_video == STREAM_VIDEO_INVALID) {
                goto error;
            }
            break;
        case SPICE_OPTION_PLAYBACK_COMPRESSION:
            if (!val) {
                goto error;
            }
            if (strcmp(val, "on") == 0) {
                snd_set_playback_compression(TRUE);
            } else if (strcmp(val, "off") == 0) {
                snd_set_playback_compression(FALSE);
            } else {
                goto error;
            }
            break;
        case SPICE_OPTION_AGENT_MOUSE:
            if (!val) {
                goto error;
            }
            if (strcmp(val, "on") == 0) {
                agent_mouse = TRUE;
            } else if (strcmp(val, "off") == 0) {
                agent_mouse = FALSE;
            } else {
                goto error;
            }
            break;
        default:
            goto error;
        }
    } while (args);

    if (!renderers_opt && !red_dispatcher_add_renderer("sw")) {
        goto error;
    }

    // All SSL parameters should be either on or off.
    if (ssl_port != ssl_key || ssl_key != ssl_certs || ssl_certs != ssl_cafile ||
            ssl_cafile != ssl_dhfile || ssl_dhfile != ssl_ciphersuite) {

        goto error;
    }
    free(local_args);
    return TRUE;

error:
    free(local_args);
    return FALSE;
}

const char *spice_usage_str[] __attribute__ ((visibility ("default"))) = {
    "[port=<port>][,sport=<port>][,host=<host>]",
    "[,ic=on|auto_glz|auto_lz|quic|glz|lz|off]",
    "[,playback-compression=on|off]",
    "[,password=password][,disable-ticketing]",
    "[,renderer=oglpbuf+oglpixmap+sw]",
    "[,sslkeys=key directory,sslcerts=certs directory,sslpassword=pem password,",
    "                                              sslciphersuite=cipher suite]",
    "[,secure-channels=all|channel+channel+...]",
    "[,unsecure-channels=all|channel+channel+...]",
    "[,vs=on|off] [,ac=on|off]",
    "    listen on interface address <host> port <port> and/or sport <port>",
    "    setting ticket password using \"ticket\" option",
    "    setting image compression using \"ic\" option [default=auto_local]",
    "    setting playback compression using \"playback-compression\" option [default=on]",
    "    select renderers using \"renderer\" option",
    "    sslkeys - set directory where ssl key file resides.",
    "    sslcerts - set directory where ssl cert file resides.",
    "    sslpassword - set the password to open the private key file.",
    "    sslciphersuite - set the cipher suite to use.",
    "    setting streaming video using \"sv\" option [default=on]",
    "    setting audio compression codec using \"ac\" option [default=off]",
    "    secure-channels - force secure connection on all/specific chnnels.",
    "                       channels names: main, inputs, display, cursor,",
    "                                       playback and record.",
    "    unsecure-channels - force unsecure connection on all/specific chnnels.",
    "                         channels names as in secure-channels.",
    NULL,
};

#define REDS_SAVE_VERSION 1

static OptionsMap spice_mig_options[] = {
    {"spicesport", SPICE_OPTION_SPORT},
    {"spiceport", SPICE_OPTION_PORT},
    {"spicehost", SPICE_OPTION_HOST},
    {NULL, 0},
};

struct RedsMigSpice;

typedef struct RedsMigRead {
    uint8_t buf[RECIVE_BUF_SIZE];
    uint32_t end_pos;
    uint32_t size;

    void (*handle_data)(struct RedsMigSpice *message);
} RedsMigRead;

typedef struct RedsMigWrite {
    uint8_t buf[SEND_BUF_SIZE];
    uint8_t *now;
    uint32_t length;

    void (*handle_done)(struct RedsMigSpice *s);
} RedsMigWrite;

typedef struct RedsMigSpice {
    int fd;
    RedsMigWrite write;
    RedsMigRead read;

    char pub_key[SPICE_TICKET_PUBKEY_BYTES];
    uint32_t mig_key;

    char *local_args;
    char *host;
    int port;
    int sport;
    uint16_t cert_pub_key_type;
    uint32_t cert_pub_key_len;
    uint8_t* cert_pub_key;
} RedsMigSpice;

typedef struct RedsMigSpiceMessage {
    uint32_t link_id;
} RedsMigSpiceMessage;

typedef struct RedsMigCertPubKeyInfo {
    uint16_t type;
    uint32_t len;
} RedsMigCertPubKeyInfo;

static int reds_mig_actual_read(RedsMigSpice *s)
{
    for (;;) {
        uint8_t *buf = s->read.buf;
        uint32_t pos = s->read.end_pos;
        int n;
        n = read(s->fd, buf + pos, s->read.size - pos);
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
            s->read.end_pos += n;
            if (s->read.end_pos == s->read.size) {
                s->read.handle_data(s);
                return 0;
            }
        }
    }
}

static int reds_mig_actual_write(RedsMigSpice *s)
{
    if (!s->write.length) {
        return 0;
    }

    while (s->write.length) {
        int n;

        n = write(s->fd, s->write.now, s->write.length);
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
            s->write.now += n;
            s->write.length -= n;
        }
    }

    s->write.handle_done(s);
    return 0;
}

static void reds_mig_failed(RedsMigSpice *s)
{
    red_printf("");
    core->set_file_handlers(core, s->fd, NULL, NULL, NULL);
    if (s->local_args) {
        free(s->local_args);
    }
    free(s);

    reds_mig_disconnect();
}

static void reds_mig_write(void *data)
{
    RedsMigSpice *s = data;

    if (reds_mig_actual_write((RedsMigSpice *)data)) {
        red_printf("write error cannot continue spice migration");
        reds_mig_failed(s);
    }
}

static void reds_mig_read(void *data)
{
    RedsMigSpice *s = data;

    if (reds_mig_actual_read((RedsMigSpice *)data)) {
        red_printf("read error cannot continue spice migration");
        reds_mig_failed(s);
    }
}

static void reds_mig_continue(RedsMigSpice *s)
{
    SpiceMsgMainMigrationBegin *migrate;
    SimpleOutItem *item;
    int host_len;

    red_printf("");
    core->set_file_handlers(core, s->fd, NULL, NULL, NULL);
    host_len = strlen(s->host) + 1;
    item = new_simple_out_item(SPICE_MSG_MAIN_MIGRATE_BEGIN,
                               sizeof(SpiceMsgMainMigrationBegin) + host_len + s->cert_pub_key_len);
    migrate = (SpiceMsgMainMigrationBegin *)item->data;
    migrate->port = s->port;
    migrate->sport = s->sport;
    migrate->host_offset = sizeof(SpiceMsgMainMigrationBegin);
    migrate->host_size = host_len;
    migrate->pub_key_type = s->cert_pub_key_type;
    migrate->pub_key_offset = sizeof(SpiceMsgMainMigrationBegin) + host_len;
    migrate->pub_key_size = s->cert_pub_key_len;
    memcpy((uint8_t*)(migrate) + migrate->host_offset , s->host, host_len);
    memcpy((uint8_t*)(migrate) + migrate->pub_key_offset, s->cert_pub_key, s->cert_pub_key_len);
    reds_push_pipe_item(&item->base);

    free(s->local_args);
    free(s);
    reds->mig_wait_connect = TRUE;
    core->arm_timer(core, reds->mig_timer, MIGRATE_TIMEOUT);
}

static void reds_mig_receive_ack(RedsMigSpice *s)
{
    s->read.size = sizeof(uint32_t);
    s->read.end_pos = 0;
    s->read.handle_data = reds_mig_continue;

    core->set_file_handlers(core, s->fd, reds_mig_read, NULL, s);
}

static void reds_mig_send_link_id(RedsMigSpice *s)
{
    RedsMigSpiceMessage *data = (RedsMigSpiceMessage *)s->write.buf;

    memcpy(&data->link_id, &reds->link_id, sizeof(reds->link_id));

    s->write.now = s->write.buf;
    s->write.length = sizeof(RedsMigSpiceMessage);
    s->write.handle_done = reds_mig_receive_ack;

    core->set_file_handlers(core, s->fd, reds_mig_write, reds_mig_write, s);
}

static void reds_mig_send_ticket(RedsMigSpice *s)
{
    EVP_PKEY *pubkey = NULL;
    BIO *bio_key;
    RSA *rsa;
    int rsa_size = 0;

    red_printf("");

    bio_key = BIO_new(BIO_s_mem());
    if (bio_key != NULL) {
        BIO_write(bio_key, s->read.buf, SPICE_TICKET_PUBKEY_BYTES);
        pubkey = d2i_PUBKEY_bio(bio_key, NULL);
        rsa = pubkey->pkey.rsa;
        rsa_size = RSA_size(rsa);
        if (RSA_public_encrypt(strlen(reds->taTicket.password) + 1,
                               (unsigned char *)reds->taTicket.password,
                               (uint8_t *)(s->write.buf),
                               rsa, RSA_PKCS1_OAEP_PADDING) > 0) {
            s->write.length = RSA_size(rsa);
            s->write.now = s->write.buf;
            s->write.handle_done = reds_mig_send_link_id;
            core->set_file_handlers(core, s->fd, reds_mig_write, reds_mig_write, s);
        } else {
            reds_mig_failed(s);
        }
    } else {
        reds_mig_failed(s);
    }

    EVP_PKEY_free(pubkey);
    BIO_free(bio_key);
}

static void reds_mig_receive_cert_public_key(RedsMigSpice *s)
{
    s->cert_pub_key = spice_memdup(s->read.buf, s->cert_pub_key_len);

    s->read.size = SPICE_TICKET_PUBKEY_BYTES;
    s->read.end_pos = 0;
    s->read.handle_data = reds_mig_send_ticket;

    core->set_file_handlers(core, s->fd, reds_mig_read, NULL, s);
}

static void reds_mig_receive_cert_public_key_info(RedsMigSpice *s)
{
    RedsMigCertPubKeyInfo* pubkey_info = (RedsMigCertPubKeyInfo*)s->read.buf;
    s->cert_pub_key_type = pubkey_info->type;
    s->cert_pub_key_len = pubkey_info->len;

    if (s->cert_pub_key_len > RECIVE_BUF_SIZE) {
        red_printf("certificate public key length exceeds buffer size");
        reds_mig_failed(s);
        return;
    }

    if (s->cert_pub_key_len) {
        s->read.size = s->cert_pub_key_len;
        s->read.end_pos = 0;
        s->read.handle_data = reds_mig_receive_cert_public_key;
    } else {
        s->cert_pub_key = NULL;
        s->read.size = SPICE_TICKET_PUBKEY_BYTES;
        s->read.end_pos = 0;
        s->read.handle_data = reds_mig_send_ticket;
    }

    core->set_file_handlers(core, s->fd, reds_mig_read, NULL, s);
}

static void reds_mig_handle_send_abort_done(RedsMigSpice *s)
{
    reds_mig_failed(s);
}

static void reds_mig_receive_version(RedsMigSpice *s)
{
    uint32_t* dest_version;
    uint32_t resault;
    dest_version = (uint32_t*)s->read.buf;
    resault = REDS_MIG_ABORT;
    memcpy(s->write.buf, &resault, sizeof(resault));
    s->write.length = sizeof(resault);
    s->write.now = s->write.buf;
    s->write.handle_done = reds_mig_handle_send_abort_done;
    core->set_file_handlers(core, s->fd, reds_mig_write, reds_mig_write, s);
}

static void reds_mig_control(RedsMigSpice *spice_migration)
{
    uint32_t *control;

    core->set_file_handlers(core, spice_migration->fd, NULL, NULL, NULL);
    control = (uint32_t *)spice_migration->read.buf;

    switch (*control) {
    case REDS_MIG_CONTINUE:
        spice_migration->read.size = sizeof(RedsMigCertPubKeyInfo);
        spice_migration->read.end_pos = 0;
        spice_migration->read.handle_data = reds_mig_receive_cert_public_key_info;

        core->set_file_handlers(core, spice_migration->fd, reds_mig_read,
                                NULL, spice_migration);
        break;
    case REDS_MIG_ABORT:
        red_printf("abort");
        reds_mig_failed(spice_migration);
        break;
    case REDS_MIG_DIFF_VERSION:
        red_printf("different versions");
        spice_migration->read.size = sizeof(uint32_t);
        spice_migration->read.end_pos = 0;
        spice_migration->read.handle_data = reds_mig_receive_version;

        core->set_file_handlers(core, spice_migration->fd, reds_mig_read,
                                NULL, spice_migration);
        break;
    default:
        red_printf("invalid control");
        reds_mig_failed(spice_migration);
    }
}

static void reds_mig_receive_control(RedsMigSpice *spice_migration)
{
    spice_migration->read.size = sizeof(uint32_t);
    spice_migration->read.end_pos = 0;
    spice_migration->read.handle_data = reds_mig_control;

    core->set_file_handlers(core, spice_migration->fd, reds_mig_read, NULL, spice_migration);
}

static void reds_mig_started(void *opaque, const char *in_args)
{
    RedsMigSpice *spice_migration = NULL;
    uint32_t *version;
    char *val;
    char *args;
    int option;

    ASSERT(in_args);
    red_printf("");

    reds->mig_inprogress = TRUE;

    if (reds->listen_socket != -1) {
        core->set_file_handlers(core, reds->listen_socket, NULL, NULL, NULL);
    }

    if (reds->secure_listen_socket != -1) {
        core->set_file_handlers(core, reds->secure_listen_socket, NULL, NULL, NULL);
    }

    if (reds->peer == NULL) {
        red_printf("not connected to peer");
        goto error;
    }

    if ((SPICE_VERSION_MAJOR == 1) && (reds->peer_minor_version < 2)) {
        red_printf("minor version mismatch client %u server %u",
                   reds->peer_minor_version, SPICE_VERSION_MINOR);
        goto error;
    }

    spice_migration = spice_new0(RedsMigSpice, 1);
    spice_migration->port = -1;
    spice_migration->sport = -1;

    spice_migration->local_args = spice_strdup(in_args);

    args = spice_migration->local_args;
    do {
        switch (option = get_option(&args, &val, spice_mig_options, ',')) {
        case SPICE_OPTION_SPORT: {
            char *endptr;

            if (!val) {
                goto error;
            }
            spice_migration->sport = strtol(val, &endptr, 0);
            if (endptr != val + strlen(val) || spice_migration->sport < 0 ||
                                                                  spice_migration->sport > 0xffff) {
                goto error;
            }
            break;
        }
        case SPICE_OPTION_PORT: {
            char *endptr;

            if (!val) {
                goto error;
            }
            spice_migration->port = strtol(val, &endptr, 0);
            if (
                endptr != val + strlen(val) ||
                spice_migration->port < 0 ||
                spice_migration->port > 0xffff
                ) {
                goto error;
            }
            break;
        }
        case SPICE_OPTION_HOST:
            if (!val) {
                goto error;
            }
            spice_migration->host = val;
            break;
        }
    } while (args);

    if ((spice_migration->sport == -1 && spice_migration->port == -1) || !spice_migration->host) {
        red_printf("invalid args port %d sport %d host %s",
                   spice_migration->port,
                   spice_migration->sport,
                   (spice_migration->host) ? spice_migration->host : "NULL");
        goto error;
    }

    spice_migration->fd = mig->begin_hook(mig, reds->mig_notifier);

    if (spice_migration->fd == -1) {
        goto error;
    }

    spice_migration->write.now = spice_migration->write.buf;
    spice_migration->write.length = sizeof(uint32_t);
    version = (uint32_t *)spice_migration->write.buf;
    *version = REDS_MIG_VERSION;
    spice_migration->write.handle_done = reds_mig_receive_control;
    core->set_file_handlers(core, spice_migration->fd, reds_mig_write,
                            reds_mig_write, spice_migration);
    return;

error:
    if (spice_migration) {
        if (spice_migration->local_args) {
            free(spice_migration->local_args);
        }
        free(spice_migration);
    }

    reds_mig_disconnect();
}

static void reds_mig_finished(void *opaque, int completed)
{
    SimpleOutItem *item;

    red_printf("");
    if (reds->listen_socket != -1) {
        core->set_file_handlers(core, reds->listen_socket, reds_accept, NULL, NULL);
    }

    if (reds->secure_listen_socket != -1) {
        core->set_file_handlers(core, reds->secure_listen_socket, reds_accept_ssl_connection,
                                NULL, NULL);
    }

    if (reds->peer == NULL) {
        red_printf("no peer connected");
        mig->notifier_done(mig, reds->mig_notifier);
        return;
    }
    reds->mig_inprogress = TRUE;

    if (completed) {
        Channel *channel;
        SpiceMsgMigrate *migrate;

        reds->mig_wait_disconnect = TRUE;
        core->arm_timer(core, reds->mig_timer, MIGRATE_TIMEOUT);

        item = new_simple_out_item(SPICE_MSG_MIGRATE, sizeof(SpiceMsgMigrate));
        migrate = (SpiceMsgMigrate *)item->data;
        migrate->flags = SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER;
        reds_push_pipe_item(&item->base);
        channel = reds->channels;
        while (channel) {
            channel->migrate(channel);
            channel = channel->next;
        }
    } else {
        item = new_simple_out_item(SPICE_MSG_MAIN_MIGRATE_CANCEL, 0);
        reds_push_pipe_item(&item->base);
        reds_mig_cleanup();
    }
}

static int write_all(int fd, const void *in_buf, int len1)
{
    int ret, len;
    uint8_t *buf = (uint8_t *)in_buf;

    len = len1;
    while (len > 0) {
        ret = write(fd, buf, len);
        if (ret < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                return -1;
            }
        } else if (ret == 0) {
            break;
        } else {
            buf += ret;
            len -= ret;
        }
    }
    return len1 - len;
}

static int read_all(int fd, void *in_nuf, int lenl)
{
    int ret, len;
    uint8_t *buf = in_nuf;

    len = lenl;
    while (len > 0) {
        ret = read(fd, buf, len);
        if (ret < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                return -1;
            }
        } else if (ret == 0) {
            break;
        } else {
            buf += ret;
            len -= ret;
        }
    }
    return lenl - len;
}

static void reds_mig_read_all(int fd, void *buf, int len, const char *name)
{
    int n = read_all(fd, buf, len);
    if (n != len) {
        red_error("read %s failed, n=%d (%s)", name, n, strerror(errno));
    }
}

static void reds_mig_write_all(int fd, void *buf, int len, const char *name)
{
    int n = write_all(fd, buf, len);
    if (n != len) {
        red_error("write %s faile, n=%d (%s)", name, n, strerror(errno));
    }
}

static void reds_mig_send_cert_public_key(int fd)
{
    FILE* cert_file;
    X509* x509;
    EVP_PKEY* pub_key;
    unsigned char* pp = NULL;
    int length;
    BIO* mem_bio;
    RedsMigCertPubKeyInfo pub_key_info_msg;

    if (spice_secure_port == -1) {
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_INVALID;
        pub_key_info_msg.len = 0;
        reds_mig_write_all(fd, &pub_key_info_msg, sizeof(pub_key_info_msg), "cert public key info");
        return;
    }

    cert_file =  fopen(ssl_parameters.certs_file, "r");
    if (!cert_file) {
        red_error("opening certificate failed");
    }

    x509 = PEM_read_X509_AUX(cert_file, NULL, NULL, NULL);
    if (!x509) {
        red_error("reading x509 cert failed");
    }
    pub_key = X509_get_pubkey(x509);
    if (!pub_key) {
        red_error("reading public key failed");
    }

    mem_bio = BIO_new(BIO_s_mem());
    i2d_PUBKEY_bio(mem_bio, pub_key);
    if (BIO_flush(mem_bio) != 1) {
        red_error("bio flush failed");
    }
    length = BIO_get_mem_data(mem_bio, &pp);

    switch(pub_key->type) {
    case EVP_PKEY_RSA:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_RSA;
        break;
    case EVP_PKEY_RSA2:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_RSA2;
        break;
    case EVP_PKEY_DSA:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_DSA;
        break;
    case EVP_PKEY_DSA1:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_DSA1;
        break;
    case EVP_PKEY_DSA2:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_DSA2;
        break;
    case EVP_PKEY_DSA3:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_DSA3;
        break;
    case EVP_PKEY_DSA4:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_DSA4;
        break;
    case EVP_PKEY_DH:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_DH;
        break;
    case EVP_PKEY_EC:
        pub_key_info_msg.type = SPICE_PUBKEY_TYPE_EC;
        break;
    default:
        red_error("invalid public key type");
    }
    pub_key_info_msg.len = length;
    reds_mig_write_all(fd, &pub_key_info_msg, sizeof(pub_key_info_msg), "cert public key info");
    reds_mig_write_all(fd, pp, length, "cert public key");

    BIO_free(mem_bio);
    fclose(cert_file);
    EVP_PKEY_free(pub_key);
    X509_free(x509);
}

static void reds_mig_recv(void *opaque, int fd)
{
    uint32_t ack_message = *(uint32_t *)"ack_";
    char password[SPICE_MAX_PASSWORD_LENGTH];
    RedsMigSpiceMessage mig_message;
    unsigned long f4 = RSA_F4;
    TicketInfo ticketing_info;
    uint32_t version;
    uint32_t resault;
    BIO *bio;

    BUF_MEM *buff;

    reds_mig_read_all(fd, &version, sizeof(version), "version");
    // starting from version 3, if the version of the src is bigger
    // than ours, we send our version to the src.
    if (version < REDS_MIG_VERSION) {
        resault = REDS_MIG_ABORT;
        reds_mig_write_all(fd, &resault, sizeof(resault), "resault");
        mig->notifier_done(mig, reds->mig_notifier);
        return;
    } else if (version > REDS_MIG_VERSION) {
        uint32_t src_resault;
        uint32_t self_version = REDS_MIG_VERSION;
        resault = REDS_MIG_DIFF_VERSION;
        reds_mig_write_all(fd, &resault, sizeof(resault), "resault");
        reds_mig_write_all(fd, &self_version, sizeof(self_version), "dest-version");
        reds_mig_read_all(fd, &src_resault, sizeof(src_resault), "src resault");

        if (src_resault == REDS_MIG_ABORT) {
            red_printf("abort (response to REDS_MIG_DIFF_VERSION)");
            mig->notifier_done(mig, reds->mig_notifier);
            return;
        } else if (src_resault != REDS_MIG_CONTINUE) {
            red_printf("invalid response to REDS_MIG_DIFF_VERSION");
            mig->notifier_done(mig, reds->mig_notifier);
            return;
        }
    } else {
        resault = REDS_MIG_CONTINUE;
        reds_mig_write_all(fd, &resault, sizeof(resault), "resault");
    }

    reds_mig_send_cert_public_key(fd);

    ticketing_info.bn = BN_new();
    if (!ticketing_info.bn) {
        red_error("OpenSSL BIGNUMS alloc failed");
    }

    BN_set_word(ticketing_info.bn, f4);
    if (!(ticketing_info.rsa = RSA_new())) {
        red_error("OpenSSL RSA alloc failed");
    }

    RSA_generate_key_ex(ticketing_info.rsa, SPICE_TICKET_KEY_PAIR_LENGTH, ticketing_info.bn, NULL);
    ticketing_info.rsa_size = RSA_size(ticketing_info.rsa);

    if (!(bio = BIO_new(BIO_s_mem()))) {
        red_error("OpenSSL BIO alloc failed");
    }

    i2d_RSA_PUBKEY_bio(bio, ticketing_info.rsa);
    BIO_get_mem_ptr(bio, &buff);

    reds_mig_write_all(fd, buff->data, SPICE_TICKET_PUBKEY_BYTES, "publick key");
    reds_mig_read_all(fd, ticketing_info.encrypted_ticket.encrypted_data, ticketing_info.rsa_size,
                      "ticket");

    RSA_private_decrypt(ticketing_info.rsa_size, ticketing_info.encrypted_ticket.encrypted_data,
                        (unsigned char *)password, ticketing_info.rsa, RSA_PKCS1_OAEP_PADDING);

    BN_free(ticketing_info.bn);
    BIO_free(bio);
    RSA_free(ticketing_info.rsa);

    memcpy(reds->taTicket.password, password, sizeof(reds->taTicket.password));
    reds_mig_read_all(fd, &mig_message, sizeof(mig_message), "mig data");
    reds->link_id = mig_message.link_id;
    reds_mig_write_all(fd, &ack_message, sizeof(uint32_t), "ack");
    mig->notifier_done(mig, reds->mig_notifier);
}

static void migrate_timout(void *opaque)
{
    red_printf("");
    ASSERT(reds->mig_wait_connect || reds->mig_wait_disconnect);
    reds_mig_disconnect();
}

static void key_modifiers_sender(void *opaque)
{
    reds_send_keyboard_modifiers(keyboard ? keyboard->get_leds(keyboard) : 0);
}

uint32_t reds_get_mm_time()
{
    struct timespec time_space;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    return time_space.tv_sec * 1000 + time_space.tv_nsec / 1000 / 1000;
}

void reds_update_mm_timer(uint32_t mm_time)
{
    red_dispatcher_set_mm_time(mm_time);
}

void reds_enable_mm_timer()
{
    SpiceMsgMainMultiMediaTime *time_mes;
    SimpleOutItem *item;

    core->arm_timer(core, reds->mm_timer, MM_TIMER_GRANULARITY_MS);
    if (!reds->peer) {
        return;
    }

    if (!(item = new_simple_out_item(SPICE_MSG_MAIN_MULTI_MEDIA_TIME, sizeof(SpiceMsgMainMultiMediaTime)))) {
        red_printf("alloc item failed");
        reds_disconnect();
        return;
    }
    time_mes = (SpiceMsgMainMultiMediaTime *)item->data;
    time_mes->time = reds_get_mm_time() - MM_TIME_DELTA;
    reds_push_pipe_item(&item->base);
}

void reds_desable_mm_timer()
{
    core->disarm_timer(core, reds->mm_timer);
}

static void mm_timer_proc(void *opaque)
{
    red_dispatcher_set_mm_time(reds_get_mm_time());
    core->arm_timer(core, reds->mm_timer, MM_TIMER_GRANULARITY_MS);
}

static void add_monitor_action_commands(QTermInterface *mon)
{
    mon->add_action_command_handler(mon, "spice", "set_image_compression", "s",
                                    reds_do_set_image_compression,
                                    "",
                                    "<[on|auto_glz|auto_lz|quic|glz|lz|off]>");
    mon->add_action_command_handler(mon, "spice", "set_streaming_video", "s",
                                    reds_do_set_streaming_video,
                                    "",
                                    "<on|filter|all|off>");
    mon->add_action_command_handler(mon, "spice", "set_playback_compression", "s",
                                    reds_do_set_playback_compression,
                                    "",
                                    "<on|off>");
    mon->add_action_command_handler(mon, "spice", "set_ticket", "ss?",
                                    reds_do_set_ticket,
                                    "<password> [expiration=<seconds>]"
                                    "[,connected=keep|disconnect|fail]",
                                    "set the spice connection ticket");
    mon->add_action_command_handler(mon, "spice", "set_ticket64", "ss?",
                                    reds_do_set_ticket64,
                                    "<password> [expiration=<seconds>]"
                                    "[,connected=keep|disconnect|fail]",
                                    "set the spice connection ticket");
    mon->add_action_command_handler(mon, "spice", "disable_ticketing", "",
                                    reds_do_disable_ticketing,
                                    "",
                                    "entirely disables OTP");
    mon->add_action_command_handler(mon, "spice", "set_agent_mouse", "s",
                                    reds_do_set_agent_mouse,
                                    "",
                                    "<on|off>");
#ifdef RED_STATISTICS
    mon->add_action_command_handler(mon, "spice", "reset_stat", "",
                                    do_reset_statistics,
                                    "",
                                    "reset spice statistics");
    mon->add_action_command_handler(mon, "spice", "ping_client", "s?i?",
                                    do_ping_client,
                                    "[on [interval]|off]",
                                    "ping spice client to measure roundtrip");
#endif
}

static void add_monitor_action_commands_2(QTerm2Interface *mon)
{
    VDIArgDescriptor s[] = {
        { "arg1", ARG_TYPE_STRING, FALSE},
        { NULL, 0, 0},
    };

    VDIArgDescriptor empty[] = {
        { NULL, 0, 0}
    };

    VDIArgDescriptor s_s_o[] = {
        { "arg1", ARG_TYPE_STRING, FALSE},
        { "arg2", ARG_TYPE_STRING, TRUE},
        { NULL, 0, 0}
    };

    VDIArgDescriptor s_o_i_o[] = {
        { "arg1", ARG_TYPE_STRING, TRUE},
        { "arg2", ARG_TYPE_INT, TRUE},
        { NULL, 0, 0}
    };

    mon->add_action_command_handler(mon, "spice", "set_image_compression", s,
                                    reds_do_set_image_compression_2,
                                    "<[on|auto_glz|auto_lz|quic|glz|lz|off]>",
                                    "");

    mon->add_action_command_handler(mon, "spice", "set_streaming_video", s,
                                    reds_do_set_streaming_video_2,
                                    "<on|filter|all|off>",
                                    "");

    mon->add_action_command_handler(mon, "spice", "set_playback_compression", s,
                                    reds_do_set_playback_compression_2,
                                    "<on|off>",
                                    "");

    mon->add_action_command_handler(mon, "spice", "set_ticket", s_s_o,
                                    reds_do_set_ticket_2,
                                    "<password> [expiration=<seconds>]"
                                    "[,connected=keep|disconnect|fail]",
                                    "set the spice connection ticket");
    mon->add_action_command_handler(mon, "spice", "set_ticket64", s_s_o,
                                    reds_do_set_ticket64_2,
                                    "<password> [expiration=<seconds>]"
                                    "[,connected=keep|disconnect|fail]",
                                    "set the spice connection ticket");
    mon->add_action_command_handler(mon, "spice", "disable_ticketing", empty,
                                    reds_do_disable_ticketing_2,
                                    "",
                                    "entirely disables OTP");
    mon->add_action_command_handler(mon, "spice", "set_agent_mouse", s,
                                    reds_do_set_agent_mouse_2,
                                    "<on|off>",
                                    "");
#ifdef RED_STATISTICS
    mon->add_action_command_handler(mon, "spice", "reset_stat", empty,
                                    do_reset_statistics_2,
                                    "",
                                    "reset spice statistics");
    mon->add_action_command_handler(mon, "spice", "ping_client", s_o_i_o,
                                    do_ping_client_2,
                                    "[on [interval]|off]",
                                    "ping spice client to measure roundtrip");
#endif
}

static void add_monitor_info_commands(QTermInterface *mon)
{
    mon->add_info_command_handler(mon, "spice", "state",
                                  reds_do_info_spice,
                                  "show spice state");
    mon->add_info_command_handler(mon, "spice", "ticket",
                                  reds_do_info_ticket,
                                  "show ticket");
#ifdef RED_STATISTICS
    mon->add_info_command_handler(mon, "spice", "stat",
                                  do_info_statistics,
                                  "show spice statistics");
    mon->add_info_command_handler(mon, "spice", "rtt_client",
                                  do_info_rtt_client,
                                  "show rtt to spice client");
#endif
}

static void add_monitor_info_commands_2(QTerm2Interface *mon)
{
    mon->add_info_command_handler(mon, "spice", "state",
                                  reds_do_info_spice,
                                  "show spice state");
    mon->add_info_command_handler(mon, "spice", "ticket",
                                  reds_do_info_ticket,
                                  "show ticket");
#ifdef RED_STATISTICS
    mon->add_info_command_handler(mon, "spice", "stat",
                                  do_info_statistics,
                                  "show spice statistics");
    mon->add_info_command_handler(mon, "spice", "rtt_client",
                                  do_info_rtt_client,
                                  "show rtt to spice client");
#endif
}

static void attach_to_red_agent(VDIPortInterface *interface)
{
    VDIPortState *state = &reds->agent_state;

    vdagent = interface;
    reds_update_mouse_mode();
    if (!reds->peer) {
        return;
    }
    state->plug_ref = vdagent->plug(vdagent, &state->plug);
    reds->agent_state.plug_generation++;

    if (reds->mig_target) {
        return;
    }

    reds_send_agent_connected();
}

static void interface_change_notifier(void *opaque, VDInterface *interface,
                                      VDInterfaceChangeType change)
{
    if (interface->base_version != VM_INTERFACE_VERSION) {
        red_printf("unsuported base interface version");
        return;
    }
    switch (change) {
    case VD_INTERFACE_ADDING:
        if (strcmp(interface->type, VD_INTERFACE_KEYBOARD) == 0) {
            red_printf("VD_INTERFACE_KEYBOARD");
            if (keyboard) {
                red_printf("already have keyboard");
                return;
            }
            if (interface->major_version != VD_INTERFACE_KEYBOARD_MAJOR ||
                interface->minor_version < VD_INTERFACE_KEYBOARD_MINOR) {
                red_printf("unsuported keyboard interface");
                return;
            }
            keyboard = (KeyboardInterface *)interface;
            if (keyboard->register_leds_notifier) {
                if (!keyboard->register_leds_notifier(keyboard, reds_on_keyboard_leds_change, NULL)) {
                    red_error("register leds notifier failed");
                }
            }
        } else if (strcmp(interface->type, VD_INTERFACE_MOUSE) == 0) {
            red_printf("VD_INTERFACE_MOUSE");
            if (mouse) {
                red_printf("already have mouse");
                return;
            }
            if (interface->major_version != VD_INTERFACE_MOUSE_MAJOR ||
                interface->minor_version < VD_INTERFACE_MOUSE_MINOR) {
                red_printf("unsuported mouse interface");
                return;
            }
            mouse = (MouseInterface *)interface;
        } else if (strcmp(interface->type, VD_INTERFACE_MIGRATION) == 0) {
            red_printf("VD_INTERFACE_MIGRATION");
            if (mig) {
                red_printf("already have migration");
                return;
            }
            if (interface->major_version != VD_INTERFACE_MIGRATION_MAJOR ||
                interface->minor_version < VD_INTERFACE_MIGRATION_MINOR) {
                red_printf("unsuported migration interface");
                return;
            }
            mig = (MigrationInterface *)interface;
            reds->mig_notifier = mig->register_notifiers(mig, MIGRATION_NOTIFY_SPICE_KEY,
                                                         reds_mig_started, reds_mig_finished,
                                                         reds_mig_recv, NULL);
            if (reds->mig_notifier == INVALID_VD_OBJECT_REF) {
                red_error("migration register failed");
            }
        } else if (strcmp(interface->type, VD_INTERFACE_QXL) == 0) {
            QXLInterface *qxl_interface;

            red_printf("VD_INTERFACE_QXL");
            if (interface->major_version != VD_INTERFACE_QXL_MAJOR ||
                interface->minor_version < VD_INTERFACE_QXL_MINOR) {
                red_printf("unsuported qxl interface");
                return;
            }
            qxl_interface = (QXLInterface *)interface;
            red_dispatcher_init(qxl_interface);
        } else if (strcmp(interface->type, VD_INTERFACE_QTERM) == 0) {
            static int was_here = FALSE;
            red_printf("VD_INTERFACE_QTERM");
            if (was_here) {
                return;
            }
            was_here = TRUE;
            if (interface->major_version != VD_INTERFACE_QTERM_MAJOR ||
                interface->minor_version < VD_INTERFACE_QTERM_MINOR) {
                red_printf("unsuported qterm interface");
                return;
            }
            add_monitor_action_commands((QTermInterface *)interface);
            add_monitor_info_commands((QTermInterface *)interface);
        } else if (strcmp(interface->type, VD_INTERFACE_QTERM2) == 0) {
            static int was_here = FALSE;
            red_printf("VD_INTERFACE_QTERM2");
            if (was_here) {
                return;
            }
            was_here = TRUE;
            if (interface->major_version != VD_INTERFACE_QTERM2_MAJOR ||
                interface->minor_version < VD_INTERFACE_QTERM2_MINOR) {
                red_printf("unsuported qterm interface");
                return;
            }
            add_monitor_action_commands_2((QTerm2Interface *)interface);
            add_monitor_info_commands_2((QTerm2Interface *)interface);
        } else if (strcmp(interface->type, VD_INTERFACE_TABLET) == 0) {
            red_printf("VD_INTERFACE_TABLET");
            if (tablet) {
                red_printf("already have tablet");
                return;
            }
            if (interface->major_version != VD_INTERFACE_TABLET_MAJOR ||
                interface->minor_version < VD_INTERFACE_TABLET_MINOR) {
                red_printf("unsuported tablet interface");
                return;
            }
            tablet = (TabletInterface *)interface;
            reds_update_mouse_mode();
            if (reds->is_client_mouse_allowed) {
                tablet->set_logical_size(tablet, reds->monitor_mode.x_res,
                                         reds->monitor_mode.y_res);
            }
        } else if (strcmp(interface->type, VD_INTERFACE_PLAYBACK) == 0) {
            red_printf("VD_INTERFACE_PLAYBACK");
            if (interface->major_version != VD_INTERFACE_PLAYBACK_MAJOR ||
                interface->minor_version < VD_INTERFACE_PLAYBACK_MINOR) {
                red_printf("unsuported playback interface");
                return;
            }
            snd_attach_playback((PlaybackInterface *)interface);
        } else if (strcmp(interface->type, VD_INTERFACE_RECORD) == 0) {
            red_printf("VD_INTERFACE_RECORD");
            if (interface->major_version != VD_INTERFACE_RECORD_MAJOR ||
                interface->minor_version < VD_INTERFACE_RECORD_MINOR) {
                red_printf("unsuported record interface");
                return;
            }
            snd_attach_record((RecordInterface *)interface);
        } else if (strcmp(interface->type, VD_INTERFACE_VDI_PORT) == 0) {
            red_printf("VD_INTERFACE_VDI_PORT");
            if (vdagent) {
                red_printf("vdi port already attached");
                return;
            }
            if (interface->major_version != VD_INTERFACE_VDI_PORT_MAJOR ||
                interface->minor_version < VD_INTERFACE_VDI_PORT_MINOR) {
                red_printf("unsuported vdi port interface");
                return;
            }
            attach_to_red_agent((VDIPortInterface *)interface);
        } else if (strcmp(interface->type, VD_INTERFACE_NET_WIRE) == 0) {
#ifdef HAVE_SLIRP
            NetWireInterface * net_wire = (NetWireInterface *)interface;
            red_printf("VD_INTERFACE_NET_WIRE");
            if (red_tunnel) {
                red_printf("net wire already attached");
                return;
            }
            if (interface->major_version != VD_INTERFACE_NET_WIRE_MAJOR ||
                interface->minor_version < VD_INTERFACE_NET_WIRE_MINOR) {
                red_printf("unsuported net wire interface");
                return;
            }
            red_tunnel = red_tunnel_attach(core, net_wire);
#else
            red_printf("unsupported net wire interface");
#endif
        }
        break;
    case VD_INTERFACE_REMOVING:
        if (strcmp(interface->type, VD_INTERFACE_TABLET) == 0) {
            red_printf("remove VD_INTERFACE_TABLET");
            if (interface == (VDInterface *)tablet) {
                tablet = NULL;
                reds_update_mouse_mode();
            }
            break;
        } else if (strcmp(interface->type, VD_INTERFACE_PLAYBACK) == 0) {
            red_printf("remove VD_INTERFACE_PLAYBACK");
            snd_detach_playback((PlaybackInterface *)interface);
            break;
        } else if (strcmp(interface->type, VD_INTERFACE_RECORD) == 0) {
            red_printf("remove VD_INTERFACE_RECORD");
            snd_detach_record((RecordInterface *)interface);
            break;
        } else if (strcmp(interface->type, VD_INTERFACE_VDI_PORT) == 0) {
            red_printf("remove VD_INTERFACE_VDI_PORT");
            if (interface == (VDInterface *)vdagent) {
                reds_agent_remove();
            }
            break;
        }
        red_error("VD_INTERFACE_REMOVING unsupported");
        break;
    }
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

void reds_prepare_read_buf(RedsOutItem *in_nuf, struct iovec* vec, int *len)
{
    VDIReadBuf *buf = (VDIReadBuf *)in_nuf;

    vec[0].iov_base = &buf->header;
    vec[0].iov_len = sizeof(buf->header);
    vec[1].iov_base = buf->data;
    vec[1].iov_len = buf->len;
    *len = 2;
}

void reds_release_read_buf(RedsOutItem *in_nuf)
{
    VDIReadBuf *buf = (VDIReadBuf *)in_nuf;

    ring_add(&reds->agent_state.read_bufs, &buf->out_item.link);
    read_from_vdi_port();
}

static void init_vd_agent_resources()
{
    VDIPortState *state = &reds->agent_state;
    int i;

    ring_init(&state->external_bufs);
    ring_init(&state->internal_bufs);
    ring_init(&state->write_queue);
    ring_init(&state->read_bufs);

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
        buf->out_item.prepare = reds_prepare_read_buf;
        buf->out_item.release = reds_release_read_buf;
        buf->header.type = SPICE_MSG_MAIN_AGENT_DATA;
        buf->header.sub_list = 0;
        ring_item_init(&buf->out_item.link);
        ring_add(&reds->agent_state.read_bufs, &buf->out_item.link);
    }

    state->plug.major_version = VD_INTERFACE_VDI_PORT_MAJOR;
    state->plug.minor_version = VD_INTERFACE_VDI_PORT_MINOR;
    state->plug.wakeup = reds_agent_wakeup;
}

const char *version_string = VERSION;

static void do_spice_init(CoreInterface *core_interface)
{
    VDInterface *interface = NULL;

    red_printf("starting %s", version_string);

    if (core_interface->base.base_version != VM_INTERFACE_VERSION) {
        red_error("bad base interface version");
    }

    if (core_interface->base.major_version != VD_INTERFACE_CORE_MAJOR) {
        red_error("bad core interface version");
    }
    core = core_interface;
    if (core_interface->base.minor_version > 1) {
        log_proc = core->log;
    }
    reds->listen_socket = -1;
    reds->secure_listen_socket = -1;
    reds->peer = NULL;
    reds->in_handler.handle_message = reds_main_handle_message;
    ring_init(&reds->outgoing.pipe);
    reds->outgoing.vec = reds->outgoing.vec_buf;

    init_vd_agent_resources();

    if (!(reds->mig_timer = core->create_timer(core, migrate_timout, NULL))) {
        red_error("migration timer create failed");
    }
    if (!(reds->key_modifiers_timer = core->create_timer(core, key_modifiers_sender, NULL))) {
        red_error("key modifiers timer create failed");
    }

    if (core->next) {
        while ((interface = core->next(core, interface))) {
            interface_change_notifier(&reds, interface, VD_INTERFACE_ADDING);
        }
    }
    if (core->register_change_notifiers) {
        core->register_change_notifiers(core, &reds, interface_change_notifier);
    }

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
    if (!(reds->ping_timer = core->create_timer(core, ping_timer_cb, NULL))) {
        red_error("ping timer create failed");
    }
    reds->ping_interval = PING_INTERVAL;
#endif

    if (!(reds->mm_timer = core->create_timer(core, mm_timer_proc, NULL))) {
        red_error("mm timer create failed");
    }
    core->arm_timer(core, reds->mm_timer, MM_TIMER_GRANULARITY_MS);

    reds_init_net();
    if (reds->secure_listen_socket != -1) {
        reds_init_ssl();
    }
    inputs_init();

    reds->mouse_mode = SPICE_MOUSE_MODE_SERVER;
    atexit(reds_exit);
}

void __attribute__ ((visibility ("default"))) spice_init(CoreInterface *core_interface)
{
    spice_server_new();
    do_spice_init(core_interface);
}

/* new interface */
SpiceServer *spice_server_new(void)
{
    /* we can't handle multiple instances (yet) */
    ASSERT(reds == NULL);

    reds = spice_new0(RedsState, 1);
    return reds;
}

int spice_server_init(SpiceServer *s, CoreInterface *core)
{
    ASSERT(reds == s);
    do_spice_init(core);
    if (default_renderer) {
        red_dispatcher_add_renderer(default_renderer);
    }
    return 0;
}

void spice_server_destroy(SpiceServer *s)
{
    ASSERT(reds == s);
    reds_exit();
}

int spice_server_set_port(SpiceServer *s, int port)
{
    ASSERT(reds == s);
    if (port < 0 || port > 0xffff) {
        return -1;
    }
    spice_port = port;
    return 0;
}

void spice_server_set_addr(SpiceServer *s, const char *addr, int flags)
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

int spice_server_set_noauth(SpiceServer *s)
{
    ASSERT(reds == s);
    memset(taTicket.password, 0, sizeof(taTicket.password));
    ticketing_enabled = 0;
    return 0;
}

int spice_server_set_ticket(SpiceServer *s, const char *passwd, int lifetime,
                            int fail_if_connected, int disconnect_if_connected)
{
    ASSERT(reds == s);

    if (reds->peer) {
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

int spice_server_set_tls(SpiceServer *s, int port,
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

int spice_server_set_image_compression(SpiceServer *s,
                                       spice_image_compression_t comp)
{
    ASSERT(reds == s);
    set_image_compression(comp);
    return 0;
}

spice_image_compression_t spice_server_get_image_compression(SpiceServer *s)
{
    ASSERT(reds == s);
    return image_compression;
}

int spice_server_set_channel_security(SpiceServer *s,
                                      spice_channel_name_t channel,
                                      int security)
{
    ASSERT(reds == s);
    if (channel == SPICE_CHANNEL_NAME_ALL) {
        set_all_channels_security(security);
    } else {
        set_one_channel_security(channel, security);
    }
    return 0;
}

int spice_server_set_mouse_absolute(SpiceServer *s, int absolute)
{
    uint32_t mode = absolute ? SPICE_MOUSE_MODE_CLIENT : SPICE_MOUSE_MODE_SERVER;

    ASSERT(reds == s);
    reds_set_mouse_mode(mode);
    return 0;
}

int spice_server_get_sock_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen)
{
    ASSERT(reds == s);
    if (!reds->peer) {
        return -1;
    }
    if (getsockname(reds->peer->socket, sa, salen) < 0) {
        return -1;
    }
    return 0;
}

int spice_server_get_peer_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen)
{
    ASSERT(reds == s);
    if (!reds->peer) {
        return -1;
    }
    if (getpeername(reds->peer->socket, sa, salen) < 0) {
        return -1;
    }
    return 0;
}

int spice_server_add_renderer(SpiceServer *s, const char *name)
{
    ASSERT(reds == s);
    if (!red_dispatcher_add_renderer(name)) {
        return -1;
    }
    default_renderer = NULL;
    return 0;
}

int spice_server_add_interface(SpiceServer *s, VDInterface *interface)
{
    ASSERT(reds == s);
    interface_change_notifier(NULL, interface, VD_INTERFACE_ADDING);
    return 0;
}

int spice_server_remove_interface(SpiceServer *s, VDInterface *interface)
{
    ASSERT(reds == s);
    interface_change_notifier(NULL, interface, VD_INTERFACE_REMOVING);
    return 0;
}

int spice_server_kbd_leds(SpiceServer *s, KeyboardInterface *kbd, int leds)
{
    ASSERT(reds == s);
    reds_on_keyboard_leds_change(NULL, leds);
    return 0;
}

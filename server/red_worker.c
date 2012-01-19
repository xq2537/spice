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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* Common variable abberiviations:
 *
 * rcc - RedChannelClient
 * ccc - CursorChannelClient (not to be confused with common_cc)
 * common_cc - CommonChannelClient
 * dcc - DisplayChannelClient
 * cursor_red_channel - downcast of CursorChannel to RedChannel
 * display_red_channel - downcast of DisplayChannel to RedChannel
 */

#include <stdio.h>
#include <stdarg.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <setjmp.h>
#include <openssl/ssl.h>

#include <spice/qxl_dev.h>
#include "spice.h"
#include "region.h"
#include <spice/protocol.h>
#include "red_worker.h"
#include "reds_sw_canvas.h"
#ifdef USE_OPENGL
#include "reds_gl_canvas.h"
#include "ogl_ctx.h"
#endif /* USE_OPENGL */
#include "quic.h"
#include "lz.h"
#include "glz_encoder_dictionary.h"
#include "glz_encoder.h"
#include "stat.h"
#include "reds.h"
#include "ring.h"
#include "mjpeg_encoder.h"
#include "red_memslots.h"
#include "red_parse_qxl.h"
#include "jpeg_encoder.h"
#include "rect.h"
#include "marshaller.h"
#include "demarshallers.h"
#include "generated_marshallers.h"
#include "zlib_encoder.h"
#include "red_channel.h"
#include "red_dispatcher.h"
#include "dispatcher.h"
#include "main_channel.h"

//#define COMPRESS_STAT
//#define DUMP_BITMAP
//#define PIPE_DEBUG
//#define RED_WORKER_STAT
//#define DRAW_ALL
//#define COMPRESS_DEBUG
//#define ACYCLIC_SURFACE_DEBUG
//#define DEBUG_CURSORS

//#define UPDATE_AREA_BY_TREE

#define CMD_RING_POLL_TIMEOUT 10 //milli
#define CMD_RING_POLL_RETRIES 200

#define DETACH_TIMEOUT 15000000000ULL //nano
#define DETACH_SLEEP_DURATION 10000 //micro

#define CHANNEL_PUSH_TIMEOUT 30000000000ULL //nano
#define CHANNEL_PUSH_SLEEP_DURATION 10000 //micro

#define DISPLAY_CLIENT_TIMEOUT 15000000000ULL //nano
#define DISPLAY_CLIENT_RETRY_INTERVAL 10000 //micro

#define DISPLAY_FREE_LIST_DEFAULT_SIZE 128

#define RED_STREAM_DETACTION_MAX_DELTA ((1000 * 1000 * 1000) / 5) // 1/5 sec
#define RED_STREAM_CONTINUS_MAX_DELTA ((1000 * 1000 * 1000) / 2) // 1/2 sec
#define RED_STREAM_TIMOUT (1000 * 1000 * 1000)
#define RED_STREAM_FRAMES_START_CONDITION 20
#define RED_STREAM_GRADUAL_FRAMES_START_CONDITION 0.2
#define RED_STREAM_FRAMES_RESET_CONDITION 100
#define RED_STREAM_MIN_SIZE (96 * 96)

#define FPS_TEST_INTERVAL 1
#define MAX_FPS 30

//best bit rate per pixel base on 13000000 bps for frame size 720x576 pixels and 25 fps
#define BEST_BIT_RATE_PER_PIXEL 38
#define WORST_BIT_RATE_PER_PIXEL 4

#define RED_COMPRESS_BUF_SIZE (1024 * 64)

#define ZLIB_DEFAULT_COMPRESSION_LEVEL 3
#define MIN_GLZ_SIZE_FOR_ZLIB 100

typedef int64_t red_time_t;

static inline red_time_t timespec_to_red_time(struct timespec *time)
{
    return time->tv_sec * (1000 * 1000 * 1000) + time->tv_nsec;
}

#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
static clockid_t clock_id;

typedef unsigned long stat_time_t;

static stat_time_t stat_now(void)
{
    struct timespec ts;

    clock_gettime(clock_id, &ts);
    return ts.tv_nsec + ts.tv_sec * 1000 * 1000 * 1000;
}

double stat_cpu_time_to_sec(stat_time_t time)
{
    return (double)time / (1000 * 1000 * 1000);
}

typedef struct stat_info_s {
    const char *name;
    uint32_t count;
    stat_time_t max;
    stat_time_t min;
    stat_time_t total;
#ifdef COMPRESS_STAT
    uint64_t orig_size;
    uint64_t comp_size;
#endif
} stat_info_t;

static inline void stat_reset(stat_info_t *info)
{
    info->count = info->max = info->total = 0;
    info->min = ~(stat_time_t)0;
#ifdef COMPRESS_STAT
    info->orig_size = info->comp_size = 0;
#endif
}

#endif

#ifdef RED_WORKER_STAT
static const char *add_stat_name = "add";
static const char *exclude_stat_name = "exclude";
static const char *__exclude_stat_name = "__exclude";

static inline void stat_init(stat_info_t *info, const char *name)
{
    info->name = name;
    stat_reset(info);
}

static inline void stat_add(stat_info_t *info, stat_time_t start)
{
    stat_time_t time;
    ++info->count;
    time = stat_now() - start;
    info->total += time;
    info->max = MAX(info->max, time);
    info->min = MIN(info->min, time);
}

#else
#define stat_add(a, b)
#define stat_init(a, b)
#endif

#ifdef COMPRESS_STAT
static const char *lz_stat_name = "lz";
static const char *glz_stat_name = "glz";
static const char *quic_stat_name = "quic";
static const char *jpeg_stat_name = "jpeg";
static const char *zlib_stat_name = "zlib_glz";
static const char *jpeg_alpha_stat_name = "jpeg_alpha";

static inline void stat_compress_init(stat_info_t *info, const char *name)
{
    info->name = name;
    stat_reset(info);
}

static inline void stat_compress_add(stat_info_t *info, stat_time_t start, int orig_size,
                                     int comp_size)
{
    stat_time_t time;
    ++info->count;
    time = stat_now() - start;
    info->total += time;
    info->max = MAX(info->max, time);
    info->min = MIN(info->min, time);
    info->orig_size += orig_size;
    info->comp_size += comp_size;
}

double inline stat_byte_to_mega(uint64_t size)
{
    return (double)size / (1000 * 1000);
}

#else
#define stat_compress_init(a, b)
#define stat_compress_add(a, b, c, d)
#endif

#define MAX_EPOLL_SOURCES 10
#define INF_EPOLL_WAIT ~0


typedef struct EventListener EventListener;
typedef void (*event_listener_action_proc)(EventListener *ctx, uint32_t events);
typedef void (*event_listener_free_proc)(EventListener *ctx);
struct EventListener {
    uint32_t refs;
    event_listener_action_proc action;
    event_listener_free_proc free;
};

enum {
    BUF_TYPE_RAW = 1,
};

enum {
    PIPE_ITEM_TYPE_DRAW = PIPE_ITEM_TYPE_CHANNEL_BASE,
    PIPE_ITEM_TYPE_INVAL_ONE,
    PIPE_ITEM_TYPE_CURSOR,
    PIPE_ITEM_TYPE_MIGRATE,
    PIPE_ITEM_TYPE_CURSOR_INIT,
    PIPE_ITEM_TYPE_IMAGE,
    PIPE_ITEM_TYPE_STREAM_CREATE,
    PIPE_ITEM_TYPE_STREAM_CLIP,
    PIPE_ITEM_TYPE_STREAM_DESTROY,
    PIPE_ITEM_TYPE_UPGRADE,
    PIPE_ITEM_TYPE_VERB,
    PIPE_ITEM_TYPE_MIGRATE_DATA,
    PIPE_ITEM_TYPE_PIXMAP_SYNC,
    PIPE_ITEM_TYPE_PIXMAP_RESET,
    PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE,
    PIPE_ITEM_TYPE_INVAL_PALLET_CACHE,
    PIPE_ITEM_TYPE_CREATE_SURFACE,
    PIPE_ITEM_TYPE_DESTROY_SURFACE,
};

typedef struct VerbItem {
    PipeItem base;
    uint16_t verb;
} VerbItem;

#define MAX_CACHE_CLIENTS 4
#define MAX_LZ_ENCODERS MAX_CACHE_CLIENTS

typedef struct NewCacheItem NewCacheItem;

struct NewCacheItem {
    RingItem lru_link;
    NewCacheItem *next;
    uint64_t id;
    uint64_t sync[MAX_CACHE_CLIENTS];
    size_t size;
    int lossy;
};

typedef struct CacheItem CacheItem;

struct CacheItem {
    union {
        PipeItem pipe_data;
        struct {
            RingItem lru_link;
            CacheItem *next;
        } cache_data;
    } u;
    uint64_t id;
    size_t size;
    uint32_t inval_type;
};

typedef struct SurfaceCreateItem {
    SpiceMsgSurfaceCreate surface_create;
    PipeItem pipe_item;
} SurfaceCreateItem;

typedef struct SurfaceDestroyItem {
    SpiceMsgSurfaceDestroy surface_destroy;
    PipeItem pipe_item;
} SurfaceDestroyItem;

typedef struct CursorItem {
    uint32_t group_id;
    int refs;
    RedCursorCmd *red_cursor;
} CursorItem;

typedef struct CursorPipeItem {
    PipeItem base;
    CursorItem *cursor_item;
    int refs;
} CursorPipeItem;

typedef struct LocalCursor {
    CursorItem base;
    SpicePoint16 position;
    uint32_t data_size;
    SpiceCursor red_cursor;
} LocalCursor;

#define MAX_PIPE_SIZE 50
#define RECIVE_BUF_SIZE 1024

#define WIDE_CLIENT_ACK_WINDOW 40
#define NARROW_CLIENT_ACK_WINDOW 20

#define BITS_CACHE_HASH_SHIFT 10
#define BITS_CACHE_HASH_SIZE (1 << BITS_CACHE_HASH_SHIFT)
#define BITS_CACHE_HASH_MASK (BITS_CACHE_HASH_SIZE - 1)
#define BITS_CACHE_HASH_KEY(id) ((id) & BITS_CACHE_HASH_MASK)

#define CLIENT_CURSOR_CACHE_SIZE 256

#define CURSOR_CACHE_HASH_SHIFT 8
#define CURSOR_CACHE_HASH_SIZE (1 << CURSOR_CACHE_HASH_SHIFT)
#define CURSOR_CACHE_HASH_MASK (CURSOR_CACHE_HASH_SIZE - 1)
#define CURSOR_CACHE_HASH_KEY(id) ((id) & CURSOR_CACHE_HASH_MASK)

#define CLIENT_PALETTE_CACHE_SIZE 128

#define PALETTE_CACHE_HASH_SHIFT 8
#define PALETTE_CACHE_HASH_SIZE (1 << PALETTE_CACHE_HASH_SHIFT)
#define PALETTE_CACHE_HASH_MASK (PALETTE_CACHE_HASH_SIZE - 1)
#define PALETTE_CACHE_HASH_KEY(id) ((id) & PALETTE_CACHE_HASH_MASK)

typedef struct ImageItem {
    PipeItem link;
    int refs;
    SpicePoint pos;
    int width;
    int height;
    int stride;
    int top_down;
    int surface_id;
    int image_format;
    uint32_t image_flags;
    int can_lossy;
    uint8_t data[0];
} ImageItem;

typedef struct Drawable Drawable;

typedef struct Stream Stream;
struct Stream {
    uint8_t refs;
    Drawable *current;
    red_time_t last_time;
    int width;
    int height;
    SpiceRect dest_area;
    MJpegEncoder *mjpeg_encoder;
    int top_down;
    Stream *next;
    RingItem link;
    int bit_rate;
};

typedef struct StreamAgent {
    QRegion vis_region;
    PipeItem create_item;
    PipeItem destroy_item;
    Stream *stream;
    uint64_t last_send_time;

    int frames;
    int drops;
    int fps;
} StreamAgent;

typedef struct StreamClipItem {
    PipeItem base;
    int refs;
    StreamAgent *stream_agent;
    int clip_type;
    SpiceClipRects *rects;
} StreamClipItem;

typedef struct RedCompressBuf RedCompressBuf;
struct RedCompressBuf {
    uint32_t buf[RED_COMPRESS_BUF_SIZE / 4];
    RedCompressBuf *next;
    RedCompressBuf *send_next;
};

static const int BITMAP_FMT_IS_PLT[] = {0, 1, 1, 1, 1, 1, 0, 0, 0, 0};
static const int BITMAP_FMT_IS_RGB[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
static const int BITMAP_FMP_BYTES_PER_PIXEL[] = {0, 0, 0, 0, 0, 1, 2, 3, 4, 4};

pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
Ring pixmap_cache_list = {&pixmap_cache_list, &pixmap_cache_list};

typedef struct PixmapCache PixmapCache;
struct PixmapCache {
    RingItem base;
    pthread_mutex_t lock;
    uint8_t id;
    uint32_t refs;
    NewCacheItem *hash_table[BITS_CACHE_HASH_SIZE];
    Ring lru;
    int64_t available;
    int64_t size;
    int32_t items;

    int freezed;
    RingItem *freezed_head;
    RingItem *freezed_tail;

    uint32_t generation;
    struct {
        uint8_t client;
        uint64_t message;
    } generation_initiator;
    uint64_t sync[MAX_CACHE_CLIENTS]; // here CLIENTS refer to different channel
                                      // clients of the same client
    RedClient *client;
};

#define NUM_STREAMS 50

#define DISPLAY_MIGRATE_DATA_MAGIC (*(uint32_t*)"DMDA")
#define DISPLAY_MIGRATE_DATA_VERSION 2

typedef struct __attribute__ ((__packed__)) DisplayChannelMigrateData {
    //todo: add ack_generation + move common to generic migration data
    uint32_t magic;
    uint32_t version;
    uint64_t message_serial;

    uint8_t pixmap_cache_freezer;
    uint8_t pixmap_cache_id;
    int64_t pixmap_cache_size;
    uint64_t pixmap_cache_clients[MAX_CACHE_CLIENTS];

    uint8_t glz_dict_id;
    GlzEncDictRestoreData glz_dict_restore_data;
} DisplayChannelMigrateData;

typedef struct WaitForChannels {
    SpiceMsgWaitForChannels header;
    SpiceWaitForChannel buf[MAX_CACHE_CLIENTS];
} WaitForChannels;

typedef struct FreeList {
    int res_size;
    SpiceResourceList *res;
    uint64_t sync[MAX_CACHE_CLIENTS];
    WaitForChannels wait;
} FreeList;

typedef struct DisplayChannel DisplayChannel;
typedef struct DisplayChannelClient DisplayChannelClient;

typedef struct  {
    DisplayChannelClient *dcc;
    RedCompressBuf *bufs_head;
    RedCompressBuf *bufs_tail;
    jmp_buf jmp_env;
    union {
        struct {
            SpiceChunks *chunks;
            int next;
            int stride;
            int reverse;
        } lines_data;
        struct {
            RedCompressBuf* next;
            int size_left;
        } compressed_data; // for encoding data that was already compressed by another method
    } u;
    char message_buf[512];
} EncoderData;

typedef struct {
    QuicUsrContext usr;
    EncoderData data;
} QuicData;

typedef struct {
    LzUsrContext usr;
    EncoderData data;
} LzData;

typedef struct {
    GlzEncoderUsrContext usr;
    EncoderData data;
} GlzData;

typedef struct {
    JpegEncoderUsrContext usr;
    EncoderData data;
} JpegData;

typedef struct {
    ZlibEncoderUsrContext usr;
    EncoderData data;
} ZlibData;

/**********************************/
/* LZ dictionary related entities */
/**********************************/
#define MAX_GLZ_DRAWABLE_INSTANCES 2

typedef struct RedGlzDrawable RedGlzDrawable;

/* for each qxl drawable, there may be several instances of lz drawables */
/* TODO - reuse this stuff for the top level. I just added a second level of multiplicity
 * at the Drawable by keeping a ring, so:
 * Drawable -> (ring of) RedGlzDrawable -> (up to 2) GlzDrawableInstanceItem
 * and it should probably (but need to be sure...) be
 * Drawable -> ring of GlzDrawableInstanceItem.
 */
typedef struct GlzDrawableInstanceItem {
    RingItem glz_link;
    RingItem free_link;
    GlzEncDictImageContext *glz_instance;
    RedGlzDrawable         *red_glz_drawable;
} GlzDrawableInstanceItem;

struct RedGlzDrawable {
    RingItem link;    // ordered by the time it was encoded
    RingItem drawable_link;
    RedDrawable *red_drawable;
    Drawable    *drawable;
    uint32_t     group_id;
    SpiceImage  *self_bitmap;
    GlzDrawableInstanceItem instances_pool[MAX_GLZ_DRAWABLE_INSTANCES];
    Ring instances;
    uint8_t instances_count;
    DisplayChannelClient *dcc;
};

pthread_mutex_t glz_dictionary_list_lock = PTHREAD_MUTEX_INITIALIZER;
Ring glz_dictionary_list = {&glz_dictionary_list, &glz_dictionary_list};

typedef struct GlzSharedDictionary {
    RingItem base;
    GlzEncDictContext *dict;
    uint32_t refs;
    uint8_t id;
    pthread_rwlock_t encode_lock;
    int migrate_freeze;
    RedClient *client; // channel clients of the same client share the dict
} GlzSharedDictionary;

#define NUM_SURFACES 10000

typedef struct CommonChannel {
    RedChannel base; // Must be the first thing
    event_listener_action_proc listener_action;
    struct RedWorker *worker;
    uint8_t recv_buf[RECIVE_BUF_SIZE];
    uint32_t id_alloc; // bitfield. TODO - use this instead of shift scheme.
} CommonChannel;

typedef struct CommonChannelClient {
    RedChannelClient base;
    EventListener listener;
    uint32_t id;
    struct RedWorker *worker;
} CommonChannelClient;

/* Each drawable can refer to at most 3 images: src, brush and mask */
#define MAX_DRAWABLE_PIXMAP_CACHE_ITEMS 3

struct DisplayChannelClient {
    CommonChannelClient common;

    int expect_init;
    int expect_migrate_mark;
    int expect_migrate_data;

    PixmapCache *pixmap_cache;
    uint32_t pixmap_cache_generation;
    int pending_pixmaps_sync;

    CacheItem *palette_cache[PALETTE_CACHE_HASH_SIZE];
    Ring palette_cache_lru;
    long palette_cache_available;
    uint32_t palette_cache_items;

    struct {
        uint32_t stream_outbuf_size;
        uint8_t *stream_outbuf; // caution stream buffer is also used as compress bufs!!!

        RedCompressBuf *used_compress_bufs;

        FreeList free_list;
        uint64_t pixmap_cache_items[MAX_DRAWABLE_PIXMAP_CACHE_ITEMS];
        int num_pixmap_cache_items;
    } send_data;

    /* global lz encoding entities */
    GlzSharedDictionary *glz_dict;
    GlzEncoderContext   *glz;
    GlzData glz_data;

    Ring glz_drawables;               // all the living lz drawable, ordered by encoding time
    Ring glz_drawables_inst_to_free;               // list of instances to be freed
    pthread_mutex_t glz_drawables_inst_to_free_lock;

    uint8_t surface_client_created[NUM_SURFACES];
    QRegion surface_client_lossy_region[NUM_SURFACES];

    StreamAgent stream_agents[NUM_STREAMS];
};

struct DisplayChannel {
    CommonChannel common; // Must be the first thing

    // only required for one client, can be the first (or choose it by speed
    // and keep a pointer to it here?)
    int expect_migrate_mark;
    int expect_migrate_data;

    int enable_jpeg;
    int jpeg_quality;
    int enable_zlib_glz_wrap;
    int zlib_level;

    RedCompressBuf *free_compress_bufs;

#ifdef RED_STATISTICS
    StatNodeRef stat;
    uint64_t *cache_hits_counter;
    uint64_t *add_to_cache_counter;
    uint64_t *non_cache_counter;
#endif
#ifdef COMPRESS_STAT
    stat_info_t lz_stat;
    stat_info_t glz_stat;
    stat_info_t quic_stat;
    stat_info_t jpeg_stat;
    stat_info_t zlib_glz_stat;
    stat_info_t jpeg_alpha_stat;
#endif
};

typedef struct CursorChannelClient {
    CommonChannelClient common;

    CacheItem *cursor_cache[CURSOR_CACHE_HASH_SIZE];
    Ring cursor_cache_lru;
    long cursor_cache_available;
    uint32_t cursor_cache_items;
} CursorChannelClient;

typedef struct CursorChannel {
    CommonChannel common; // Must be the first thing

#ifdef RED_STATISTICS
    StatNodeRef stat;
#endif
} CursorChannel;

typedef struct ImageCacheItem {
    RingItem lru_link;
    uint64_t id;
#ifdef IMAGE_CACHE_AGE
    uint32_t age;
#endif
    struct ImageCacheItem *next;
    pixman_image_t *image;
} ImageCacheItem;

#define IMAGE_CACHE_HASH_SIZE 1024

typedef struct ImageCache {
    SpiceImageCache base;
    ImageCacheItem *hash_table[IMAGE_CACHE_HASH_SIZE];
    Ring lru;
#ifdef IMAGE_CACHE_AGE
    uint32_t age;
#else
    uint32_t num_items;
#endif
} ImageCache;

enum {
    TREE_ITEM_TYPE_DRAWABLE,
    TREE_ITEM_TYPE_CONTAINER,
    TREE_ITEM_TYPE_SHADOW,
};

typedef struct TreeItem {
    RingItem siblings_link;
    uint32_t type;
    struct Container *container;
    QRegion rgn;
#ifdef PIPE_DEBUG
    uint32_t id;
#endif
} TreeItem;

#define IS_DRAW_ITEM(item) ((item)->type == TREE_ITEM_TYPE_DRAWABLE)

typedef struct Shadow {
    TreeItem base;
    QRegion on_hold;
    struct DrawItem* owner;
} Shadow;

typedef struct Container {
    TreeItem base;
    Ring items;
} Container;

typedef struct DrawItem {
    TreeItem base;
    uint8_t effect;
    uint8_t container_root;
    Shadow *shadow;
} DrawItem;

typedef enum {
    BITMAP_GRADUAL_INVALID,
    BITMAP_GRADUAL_NOT_AVAIL,
    BITMAP_GRADUAL_LOW,
    BITMAP_GRADUAL_MEDIUM,
    BITMAP_GRADUAL_HIGH,
} BitmapGradualType;

typedef struct DependItem {
    Drawable *drawable;
    RingItem ring_item;
} DependItem;

typedef struct DrawablePipeItem {
    RingItem base;  /* link for a list of pipe items held by Drawable */
    PipeItem dpi_pipe_item; /* link for the client's pipe itself */
    Drawable *drawable;
    DisplayChannelClient *dcc;
    uint8_t refs;
} DrawablePipeItem;

struct Drawable {
    uint8_t refs;
    RingItem surface_list_link;
    RingItem list_link;
    DrawItem tree_item;
    Ring pipes;
    PipeItem *pipe_item_rest;
    uint32_t size_pipe_item_rest;
#ifdef UPDATE_AREA_BY_TREE
    RingItem collect_link;
#endif
    RedDrawable *red_drawable;

    Ring glz_ring;

    red_time_t creation_time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    Stream *stream;
    int streamable;
    BitmapGradualType copy_bitmap_graduality;
    uint32_t group_id;
    SpiceImage *self_bitmap;
    DependItem depend_items[3];

    uint8_t *backed_surface_data;
    DependItem pipe_depend_items[3];

    int surface_id;
    int surfaces_dest[3];
};

typedef struct _Drawable _Drawable;
struct _Drawable {
    union {
        Drawable drawable;
        _Drawable *next;
    } u;
};

typedef struct _CursorItem _CursorItem;
struct _CursorItem {
    union {
        CursorItem cursor_item;
        _CursorItem *next;
    } u;
};

typedef struct UpgradeItem {
    PipeItem base;
    int refs;
    Drawable *drawable;
    SpiceClipRects *rects;
} UpgradeItem;

typedef struct DrawContext {
    SpiceCanvas *canvas;
    int canvas_draws_on_surface;
    int top_down;
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint32_t format;
    void *line_0;
} DrawContext;

typedef struct RedSurface {
    uint32_t refs;
    Ring current;
    Ring current_list;
#ifdef ACYCLIC_SURFACE_DEBUG
    int current_gn;
#endif
    DrawContext context;

    Ring depend_on_me;
    QRegion draw_dirty_region;

    //fix me - better handling here
    QXLReleaseInfoExt create, destroy;
} RedSurface;

typedef struct ItemTrace {
    red_time_t time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    int width;
    int height;
    SpiceRect dest_area;
} ItemTrace;

#define TRACE_ITEMS_SHIFT 3
#define NUM_TRACE_ITEMS (1 << TRACE_ITEMS_SHIFT)
#define ITEMS_TRACE_MASK (NUM_TRACE_ITEMS - 1)

#define NUM_DRAWABLES 1000
#define NUM_CURSORS 100

typedef struct RedWorker {
    EventListener dev_listener;
    DisplayChannel *display_channel;
    CursorChannel *cursor_channel;
    QXLInstance *qxl;
    RedDispatcher *red_dispatcher;

    int channel;
    int id;
    int running;
    uint32_t *pending;
    int epoll;
    unsigned int epoll_timeout;
    uint32_t repoll_cmd_ring;
    uint32_t repoll_cursor_ring;
    uint32_t num_renderers;
    uint32_t renderers[RED_MAX_RENDERERS];
    uint32_t renderer;

    RedSurface surfaces[NUM_SURFACES];
    uint32_t n_surfaces;
    SpiceImageSurfaces image_surfaces;

    Ring current_list;
    uint32_t current_size;
    uint32_t drawable_count;
    uint32_t red_drawable_count;
    uint32_t glz_drawable_count;
    uint32_t transparent_count;

    uint32_t shadows_count;
    uint32_t containers_count;
    uint32_t stream_count;

    uint32_t bits_unique;

    CursorItem *cursor;
    int cursor_visible;
    SpicePoint16 cursor_position;
    uint16_t cursor_trail_length;
    uint16_t cursor_trail_frequency;

    _Drawable drawables[NUM_DRAWABLES];
    _Drawable *free_drawables;

    _CursorItem cursor_items[NUM_CURSORS];
    _CursorItem *free_cursor_items;

    RedMemSlotInfo mem_slots;

    uint32_t preload_group_id;

    ImageCache image_cache;

    spice_image_compression_t image_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;

    uint32_t mouse_mode;

    uint32_t streaming_video;
    Stream streams_buf[NUM_STREAMS];
    Stream *free_streams;
    Ring streams;
    ItemTrace items_trace[NUM_TRACE_ITEMS];
    uint32_t next_item_trace;

    QuicData quic_data;
    QuicContext *quic;

    LzData lz_data;
    LzContext  *lz;

    JpegData jpeg_data;
    JpegEncoderContext *jpeg;

    ZlibData zlib_data;
    ZlibEncoder *zlib;

#ifdef PIPE_DEBUG
    uint32_t last_id;
#endif
#ifdef RED_WORKER_STAT
    stat_info_t add_stat;
    stat_info_t exclude_stat;
    stat_info_t __exclude_stat;
    uint32_t add_count;
    uint32_t add_with_shadow_count;
#endif
#ifdef RED_STATISTICS
    StatNodeRef stat;
    uint64_t *wakeup_counter;
    uint64_t *command_counter;
#endif
} RedWorker;

typedef enum {
    BITMAP_DATA_TYPE_INVALID,
    BITMAP_DATA_TYPE_CACHE,
    BITMAP_DATA_TYPE_SURFACE,
    BITMAP_DATA_TYPE_BITMAP,
    BITMAP_DATA_TYPE_BITMAP_TO_CACHE,
} BitmapDataType;

typedef struct BitmapData {
    BitmapDataType type;
    uint64_t id; // surface id or cache item id
    SpiceRect lossy_rect;
} BitmapData;

static void red_draw_qxl_drawable(RedWorker *worker, Drawable *drawable);
static void red_current_flush(RedWorker *worker, int surface_id);
#ifdef DRAW_ALL
#define red_update_area(worker, rect, surface_id)
#define red_draw_drawable(worker, item)
#else
static void red_draw_drawable(RedWorker *worker, Drawable *item);
static void red_update_area(RedWorker *worker, const SpiceRect *area, int surface_id);
#endif
static void red_release_cursor(RedWorker *worker, CursorItem *cursor);
static inline void release_drawable(RedWorker *worker, Drawable *item);
static void red_display_release_stream(RedWorker *worker, StreamAgent *agent);
static inline void red_detach_stream(RedWorker *worker, Stream *stream);
static void red_stop_stream(RedWorker *worker, Stream *stream);
static inline void red_stream_maintenance(RedWorker *worker, Drawable *candidate, Drawable *sect);
static inline void display_begin_send_message(RedChannelClient *rcc);
static void red_release_pixmap_cache(DisplayChannelClient *dcc);
static void red_release_glz(DisplayChannelClient *dcc);
static void red_freeze_glz(DisplayChannelClient *dcc);
static void display_channel_push_release(DisplayChannelClient *dcc, uint8_t type, uint64_t id,
                                         uint64_t* sync_data);
static void red_display_release_stream_clip(RedWorker *worker, StreamClipItem *item);
static int red_display_free_some_independent_glz_drawables(DisplayChannelClient *dcc);
static void red_display_free_glz_drawable(DisplayChannelClient *dcc, RedGlzDrawable *drawable);
static void reset_rate(DisplayChannelClient *dcc, StreamAgent *stream_agent);
static BitmapGradualType _get_bitmap_graduality_level(RedWorker *worker, SpiceBitmap *bitmap,
                                                      uint32_t group_id);
static inline int _stride_is_extra(SpiceBitmap *bitmap);
static void red_disconnect_cursor(RedChannel *channel);
static void display_channel_client_release_item_before_push(DisplayChannelClient *dcc,
                                                            PipeItem *item);
static void display_channel_client_release_item_after_push(DisplayChannelClient *dcc,
                                                           PipeItem *item);
static void cursor_channel_client_release_item_before_push(CursorChannelClient *ccc,
                                                           PipeItem *item);
static void cursor_channel_client_release_item_after_push(CursorChannelClient *ccc,
                                                          PipeItem *item);
static void red_wait_pipe_item_sent(RedChannelClient *rcc, PipeItem *item);

#ifdef DUMP_BITMAP
static void dump_bitmap(RedWorker *worker, SpiceBitmap *bitmap, uint32_t group_id);
#endif

/*
 * Macros to make iterating over stuff easier
 * The two collections we iterate over:
 *  given a channel, iterate over it's clients
 */

#define RCC_FOREACH(link, rcc, channel) \
    for (link = ring_get_head(&(channel)->clients),\
         rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link);\
            (link);                              \
            (link) = ring_next(&(channel)->clients, link),\
            rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link))

#define RCC_FOREACH_SAFE(link, next, rcc, channel) \
    for (link = ring_get_head(&(channel)->clients),                         \
         rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link),     \
         (next) = (link) ? ring_next(&(channel)->clients, (link)) : NULL;      \
            (link);                                            \
            (link) = (next),                                   \
            (next) = (link) ? ring_next(&(channel)->clients, (link)) : NULL,    \
            rcc = SPICE_CONTAINEROF(link, RedChannelClient, channel_link))

#define DCC_FOREACH(link, dcc, channel) \
    for (link = channel ? ring_get_head(&(channel)->clients) : NULL,\
         dcc = link ? SPICE_CONTAINEROF((link), DisplayChannelClient,\
                                        common.base.channel_link) : NULL;\
            (link);                              \
            (link) = ring_next(&(channel)->clients, link),\
            dcc = SPICE_CONTAINEROF((link), DisplayChannelClient, common.base.channel_link))

#define WORKER_FOREACH_DCC(worker, link, dcc) \
    for (link = ((worker) && (worker)->display_channel) ?\
            ring_get_head(&(worker)->display_channel->common.base.clients) : NULL,\
         dcc = link ? SPICE_CONTAINEROF((link), DisplayChannelClient,\
                                        common.base.channel_link) : NULL;\
            (link);                              \
            (link) = ring_next(&(worker)->display_channel->common.base.clients, link),\
            dcc = SPICE_CONTAINEROF((link), DisplayChannelClient, common.base.channel_link))

#define DRAWABLE_FOREACH_DPI(drawable, link, dpi) \
    for (link = (drawable) ? ring_get_head(&(drawable)->pipes) : NULL,\
         dpi = (link) ? SPICE_CONTAINEROF((link), DrawablePipeItem, base) : NULL; \
         (link);\
         (link) = ring_next(&(drawable)->pipes, (link)),\
         dpi = (link) ? SPICE_CONTAINEROF((link), DrawablePipeItem, base) : NULL)

#define DRAWABLE_FOREACH_GLZ(drawable, link, glz) \
    for (link = (drawable) ? ring_get_head(&drawable->glz_ring) : NULL,\
        glz = (link) ? SPICE_CONTAINEROF((link), RedGlzDrawable, drawable_link) : NULL;\
        (link);\
        (link) = ring_next(&drawable->glz_ring, (link)),\
        glz = (link) ? SPICE_CONTAINEROF((link), RedGlzDrawable, drawable_link) : NULL)

#define DRAWABLE_FOREACH_GLZ_SAFE(drawable, link, next, glz) \
    for (link = (drawable) ? ring_get_head(&drawable->glz_ring) : NULL,\
        next = (link) ? ring_next(&drawable->glz_ring, link) : NULL,\
        glz = (link) ? SPICE_CONTAINEROF((link), RedGlzDrawable, drawable_link) : NULL;\
        (link);\
        (link) = (next),\
        (next) = (link) ? ring_next(&drawable->glz_ring, (link)) : NULL,\
        glz = (link) ? SPICE_CONTAINEROF((link), RedGlzDrawable, drawable_link) : NULL)

#define CCC_FOREACH(link, ccc, channel) \
    for (link = ring_get_head(&(channel)->clients),\
         ccc = SPICE_CONTAINEROF(link, CommonChannelClient, base.channel_link);\
            (link);                              \
            (link) = ring_next(&(channel)->clients, link),\
            ccc = SPICE_CONTAINEROF(link, CommonChannelClient, base.channel_link))

#define DCC_TO_WORKER(dcc) \
    (SPICE_CONTAINEROF((dcc)->common.base.channel, CommonChannel, base)->worker)

// TODO: replace with DCC_FOREACH when it is introduced
#define WORKER_TO_DCC(worker) \
    (worker->display_channel ? SPICE_CONTAINEROF(worker->display_channel->common.base.rcc,\
                       DisplayChannelClient, common.base) : NULL)

#define DCC_TO_DC(dcc) SPICE_CONTAINEROF((dcc)->common.base.channel,\
                                         DisplayChannel, common.base)

#define RCC_TO_DCC(rcc) SPICE_CONTAINEROF((rcc), DisplayChannelClient, common.base)
#define RCC_TO_CCC(rcc) SPICE_CONTAINEROF((rcc), CursorChannelClient, common.base)



#ifdef COMPRESS_STAT
static void print_compress_stats(DisplayChannel *display_channel)
{
    uint64_t glz_enc_size;

    if (!display_channel) {
        return;
    }

    glz_enc_size = display_channel->enable_zlib_glz_wrap ?
                       display_channel->zlib_glz_stat.comp_size :
                       display_channel->glz_stat.comp_size;

    red_printf("==> Compression stats for display %u", display_channel->common.id);
    red_printf("Method   \t  count  \torig_size(MB)\tenc_size(MB)\tenc_time(s)");
    red_printf("QUIC     \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->quic_stat.count,
               stat_byte_to_mega(display_channel->quic_stat.orig_size),
               stat_byte_to_mega(display_channel->quic_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->quic_stat.total)
               );
    red_printf("GLZ      \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->glz_stat.count,
               stat_byte_to_mega(display_channel->glz_stat.orig_size),
               stat_byte_to_mega(display_channel->glz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->glz_stat.total)
               );
    red_printf("ZLIB GLZ \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->zlib_glz_stat.count,
               stat_byte_to_mega(display_channel->zlib_glz_stat.orig_size),
               stat_byte_to_mega(display_channel->zlib_glz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->zlib_glz_stat.total)
               );
    red_printf("LZ       \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total)
               );
    red_printf("JPEG     \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->jpeg_stat.count,
               stat_byte_to_mega(display_channel->jpeg_stat.orig_size),
               stat_byte_to_mega(display_channel->jpeg_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->jpeg_stat.total)
               );
    red_printf("JPEG-RGBA\t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->jpeg_alpha_stat.count,
               stat_byte_to_mega(display_channel->jpeg_alpha_stat.orig_size),
               stat_byte_to_mega(display_channel->jpeg_alpha_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->jpeg_alpha_stat.total)
               );
    red_printf("-------------------------------------------------------------------");
    red_printf("Total    \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count + display_channel->glz_stat.count +
                                                display_channel->quic_stat.count +
                                                display_channel->jpeg_stat.count +
                                                display_channel->jpeg_alpha_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size +
                                 display_channel->glz_stat.orig_size +
                                 display_channel->quic_stat.orig_size +
                                 display_channel->jpeg_stat.orig_size +
                                 display_channel->jpeg_alpha_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size +
                                 glz_enc_size +
                                 display_channel->quic_stat.comp_size +
                                 display_channel->jpeg_stat.comp_size +
                                 display_channel->jpeg_alpha_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total +
                                    display_channel->glz_stat.total +
                                    display_channel->zlib_glz_stat.total +
                                    display_channel->quic_stat.total +
                                    display_channel->jpeg_stat.total +
                                    display_channel->jpeg_alpha_stat.total)
               );
}

#endif

static inline int is_primary_surface(RedWorker *worker, uint32_t surface_id)
{
    if (surface_id == 0) {
        return TRUE;
    }
    return FALSE;
}

static inline void __validate_surface(RedWorker *worker, uint32_t surface_id)
{
    PANIC_ON(surface_id >= worker->n_surfaces);
}

static inline void validate_surface(RedWorker *worker, uint32_t surface_id)
{
    PANIC_ON(surface_id >= worker->n_surfaces);
    if (!worker->surfaces[surface_id].context.canvas) {
        red_printf("failed on %d", surface_id);
        PANIC_ON(!worker->surfaces[surface_id].context.canvas);
    }
}

static char *draw_type_to_str(uint8_t type)
{
    switch (type) {
    case QXL_DRAW_FILL:
        return "QXL_DRAW_FILL";
    case QXL_DRAW_OPAQUE:
        return "QXL_DRAW_OPAQUE";
    case QXL_DRAW_COPY:
        return "QXL_DRAW_COPY";
    case QXL_DRAW_TRANSPARENT:
        return "QXL_DRAW_TRANSPARENT";
    case QXL_DRAW_ALPHA_BLEND:
        return "QXL_DRAW_ALPHA_BLEND";
    case QXL_COPY_BITS:
        return "QXL_COPY_BITS";
    case QXL_DRAW_BLEND:
        return "QXL_DRAW_BLEND";
    case QXL_DRAW_BLACKNESS:
        return "QXL_DRAW_BLACKNESS";
    case QXL_DRAW_WHITENESS:
        return "QXL_DRAW_WHITENESS";
    case QXL_DRAW_INVERS:
        return "QXL_DRAW_INVERS";
    case QXL_DRAW_ROP3:
        return "QXL_DRAW_ROP3";
    case QXL_DRAW_STROKE:
        return "QXL_DRAW_STROKE";
    case QXL_DRAW_TEXT:
        return "QXL_DRAW_TEXT";
    default:
        return "?";
    }
}

static void show_red_drawable(RedWorker *worker, RedDrawable *drawable, const char *prefix)
{
    if (prefix) {
        printf("%s: ", prefix);
    }

    printf("%s effect %d bbox(%d %d %d %d)",
           draw_type_to_str(drawable->type),
           drawable->effect,
           drawable->bbox.top,
           drawable->bbox.left,
           drawable->bbox.bottom,
           drawable->bbox.right);

    switch (drawable->type) {
    case QXL_DRAW_FILL:
    case QXL_DRAW_OPAQUE:
    case QXL_DRAW_COPY:
    case QXL_DRAW_TRANSPARENT:
    case QXL_DRAW_ALPHA_BLEND:
    case QXL_COPY_BITS:
    case QXL_DRAW_BLEND:
    case QXL_DRAW_BLACKNESS:
    case QXL_DRAW_WHITENESS:
    case QXL_DRAW_INVERS:
    case QXL_DRAW_ROP3:
    case QXL_DRAW_STROKE:
    case QXL_DRAW_TEXT:
        break;
    default:
        red_error("bad drawable type");
    }
    printf("\n");
}

static void show_draw_item(RedWorker *worker, DrawItem *draw_item, const char *prefix)
{
    if (prefix) {
        printf("%s: ", prefix);
    }
    printf("effect %d bbox(%d %d %d %d)\n",
           draw_item->effect,
           draw_item->base.rgn.extents.x1,
           draw_item->base.rgn.extents.y1,
           draw_item->base.rgn.extents.x2,
           draw_item->base.rgn.extents.y2);
}

static inline int pipe_item_is_linked(PipeItem *item)
{
    return ring_item_is_linked(&item->link);
}

static inline void pipe_item_remove(PipeItem *item)
{
    ring_remove(&item->link);
}

static void red_pipe_add_verb(RedChannelClient* rcc, uint16_t verb)
{
    VerbItem *item = spice_new(VerbItem, 1);

    red_channel_pipe_item_init(rcc->channel, &item->base, PIPE_ITEM_TYPE_VERB);
    item->verb = verb;
    red_channel_client_pipe_add(rcc, &item->base);
}

static inline void red_create_surface_item(DisplayChannelClient *dcc, int surface_id);
static void red_push_surface_image(DisplayChannelClient *dcc, int surface_id);

static void red_pipes_add_verb(RedChannel *channel, uint16_t verb)
{
    RedChannelClient *rcc;
    RingItem *link;

    RCC_FOREACH(link, rcc, channel) {
        red_pipe_add_verb(rcc, verb);
    }
}

static inline void red_handle_drawable_surfaces_client_synced(
                        DisplayChannelClient *dcc, Drawable *drawable)
{
    RedWorker *worker = DCC_TO_WORKER(dcc);
    int x;

    for (x = 0; x < 3; ++x) {
        int surface_id;

        surface_id = drawable->surfaces_dest[x];
        if (surface_id != -1) {
            if (dcc->surface_client_created[surface_id] == TRUE) {
                continue;
            }
            red_create_surface_item(dcc, surface_id);
            red_current_flush(worker, surface_id);
            red_push_surface_image(dcc, surface_id);
        }
    }

    if (dcc->surface_client_created[drawable->surface_id] == TRUE) {
        return;
    }

    red_create_surface_item(dcc, drawable->surface_id);
    red_current_flush(worker, drawable->surface_id);
    red_push_surface_image(dcc, drawable->surface_id);
}

static int display_is_connected(RedWorker *worker)
{
    return (worker->display_channel && red_channel_is_connected(
        &worker->display_channel->common.base));
}

static int cursor_is_connected(RedWorker *worker)
{
    return (worker->cursor_channel && red_channel_is_connected(
        &worker->cursor_channel->common.base));
}

static void put_drawable_pipe_item(DrawablePipeItem *dpi)
{
    RedWorker *worker = DCC_TO_WORKER(dpi->dcc);

    if (--dpi->refs) {
        return;
    }

    ASSERT(!ring_item_is_linked(&dpi->dpi_pipe_item.link));
    ASSERT(!ring_item_is_linked(&dpi->base));
    release_drawable(worker, dpi->drawable);
    free(dpi);
}

static inline DrawablePipeItem *get_drawable_pipe_item(DisplayChannelClient *dcc,
                                                       Drawable *drawable)
{
    DrawablePipeItem *dpi;

    dpi = spice_malloc0(sizeof(*dpi));
    dpi->drawable = drawable;
    dpi->dcc = dcc;
    ring_item_init(&dpi->base);
    ring_add(&drawable->pipes, &dpi->base);
    red_channel_pipe_item_init(dcc->common.base.channel, &dpi->dpi_pipe_item, PIPE_ITEM_TYPE_DRAW);
    dpi->refs++;
    drawable->refs++;
    return dpi;
}

static inline DrawablePipeItem *ref_drawable_pipe_item(DrawablePipeItem *dpi)
{
    ASSERT(dpi->drawable);
    dpi->refs++;
    return dpi;
}

static inline void red_pipe_add_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;

    red_handle_drawable_surfaces_client_synced(dcc, drawable);
    dpi = get_drawable_pipe_item(dcc, drawable);
    red_channel_client_pipe_add(&dcc->common.base, &dpi->dpi_pipe_item);
}

static inline void red_pipes_add_drawable(RedWorker *worker, Drawable *drawable)
{
    DisplayChannelClient *dcc;
    RingItem *dcc_ring_item;

    PANIC_ON(!ring_is_empty(&drawable->pipes));
    WORKER_FOREACH_DCC(worker, dcc_ring_item, dcc) {
        red_pipe_add_drawable(dcc, drawable);
    }
}

static inline void red_pipe_add_drawable_to_tail(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;

    if (!dcc) {
        return;
    }
    red_handle_drawable_surfaces_client_synced(dcc, drawable);
    dpi = get_drawable_pipe_item(dcc, drawable);
    red_channel_client_pipe_add_tail(&dcc->common.base, &dpi->dpi_pipe_item);
}

static inline void red_pipes_add_drawable_after(RedWorker *worker,
                                                Drawable *drawable, Drawable *pos_after)
{
    DrawablePipeItem *dpi, *dpi_pos_after;
    RingItem *dpi_link;
    DisplayChannelClient *dcc;
    int num_other_linked = 0;

    DRAWABLE_FOREACH_DPI(pos_after, dpi_link, dpi_pos_after) {
        num_other_linked++;
        dcc = dpi_pos_after->dcc;
        red_handle_drawable_surfaces_client_synced(dcc, drawable);
        dpi = get_drawable_pipe_item(dcc, drawable);
        red_channel_client_pipe_add_after(&dcc->common.base, &dpi->dpi_pipe_item,
                                          &dpi_pos_after->dpi_pipe_item);
    }
    if (num_other_linked == 0) {
        red_pipes_add_drawable(worker, drawable);
        return;
    }
    if (num_other_linked != worker->display_channel->common.base.clients_num) {
        RingItem *worker_item;
        red_printf("TODO: not O(n^2)");
        WORKER_FOREACH_DCC(worker, worker_item, dcc) {
            int sent = 0;
            DRAWABLE_FOREACH_DPI(pos_after, dpi_link, dpi_pos_after) {
                if (dpi_pos_after->dcc == dcc) {
                    sent = 1;
                    break;
                }
            }
            if (!sent) {
                red_pipe_add_drawable(dcc, drawable);
            }
        }
    }
}

static inline PipeItem *red_pipe_get_tail(DisplayChannelClient *dcc)
{
    if (!dcc) {
        return NULL;
    }

    return (PipeItem*)ring_get_tail(&dcc->common.base.pipe);
}

static inline void red_destroy_surface(RedWorker *worker, uint32_t surface_id);

static inline void red_pipes_remove_drawable(Drawable *drawable)
{
    DrawablePipeItem *dpi;
    RingItem *item, *next;

    RING_FOREACH_SAFE(item, next, &drawable->pipes) {
        dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, base);
        if (pipe_item_is_linked(&dpi->dpi_pipe_item)) {
            red_channel_client_pipe_remove_and_release(&dpi->dcc->common.base,
                                                       &dpi->dpi_pipe_item);
        }
    }
}

static inline void red_pipe_add_image_item(DisplayChannelClient *dcc, ImageItem *item)
{
    if (!dcc) {
        return;
    }
    item->refs++;
    red_channel_client_pipe_add(&dcc->common.base, &item->link);
}

static inline void red_pipe_add_image_item_after(DisplayChannelClient *dcc, ImageItem *item,
                                                 PipeItem *pos)
{
    if (!dcc) {
        return;
    }
    item->refs++;
    red_channel_client_pipe_add_after(&dcc->common.base, &item->link, pos);
}

static void release_image_item(ImageItem *item)
{
    if (!--item->refs) {
        free(item);
    }
}

static void release_upgrade_item(RedWorker* worker, UpgradeItem *item)
{
    if (!--item->refs) {
        release_drawable(worker, item->drawable);
        free(item->rects);
        free(item);
    }
}

static uint8_t *common_alloc_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    CommonChannel *common = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base);

    return common->recv_buf;
}

static void common_release_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size,
                                    uint8_t* msg)
{
}

#define CLIENT_PIXMAPS_CACHE
#include "red_client_shared_cache.h"
#undef CLIENT_PIXMAPS_CACHE

#define CLIENT_CURSOR_CACHE
#include "red_client_cache.h"
#undef CLIENT_CURSOR_CACHE

#define CLIENT_PALETTE_CACHE
#include "red_client_cache.h"
#undef CLIENT_PALETTE_CACHE

static void red_reset_palette_cache(DisplayChannelClient *dcc)
{
    red_palette_cache_reset(dcc, CLIENT_PALETTE_CACHE_SIZE);
}

static void red_reset_cursor_cache(RedChannelClient *rcc)
{
    red_cursor_cache_reset(RCC_TO_CCC(rcc), CLIENT_CURSOR_CACHE_SIZE);
}

static inline Drawable *alloc_drawable(RedWorker *worker)
{
    Drawable *drawable;
    if (!worker->free_drawables) {
        return NULL;
    }
    drawable = &worker->free_drawables->u.drawable;
    worker->free_drawables = worker->free_drawables->u.next;
    return drawable;
}

static inline void free_drawable(RedWorker *worker, Drawable *item)
{
    ((_Drawable *)item)->u.next = worker->free_drawables;
    worker->free_drawables = (_Drawable *)item;
}

static void drawables_init(RedWorker *worker)
{
    int i;

    worker->free_drawables = NULL;
    for (i = 0; i < NUM_DRAWABLES; i++) {
        free_drawable(worker, &worker->drawables[i].u.drawable);
    }
}


static void red_reset_stream_trace(RedWorker *worker);

static SurfaceDestroyItem *get_surface_destroy_item(RedChannel *channel,
                                                    uint32_t surface_id)
{
    SurfaceDestroyItem *destroy;

    destroy = (SurfaceDestroyItem *)malloc(sizeof(SurfaceDestroyItem));
    PANIC_ON(!destroy);

    destroy->surface_destroy.surface_id = surface_id;

    red_channel_pipe_item_init(channel,
        &destroy->pipe_item, PIPE_ITEM_TYPE_DESTROY_SURFACE);

    return destroy;
}

static inline void red_destroy_surface_item(RedWorker *worker,
    DisplayChannelClient *dcc, uint32_t surface_id)
{
    SurfaceDestroyItem *destroy;
    RedChannel *channel;

    if (!dcc || !dcc->surface_client_created[surface_id]) {
        return;
    }
    dcc->surface_client_created[surface_id] = FALSE;
    channel = &worker->display_channel->common.base;
    destroy = get_surface_destroy_item(channel, surface_id);
    red_channel_client_pipe_add(&dcc->common.base, &destroy->pipe_item);
}

static inline void red_destroy_surface(RedWorker *worker, uint32_t surface_id)
{
    RedSurface *surface = &worker->surfaces[surface_id];
    DisplayChannelClient *dcc;
    RingItem *link;

    if (!--surface->refs) {
        // only primary surface streams are supported
        if (is_primary_surface(worker, surface_id)) {
            red_reset_stream_trace(worker);
        }
        ASSERT(surface->context.canvas);

        surface->context.canvas->ops->destroy(surface->context.canvas);
        if (surface->create.info) {
            worker->qxl->st->qif->release_resource(worker->qxl, surface->create);
        }
        if (surface->destroy.info) {
            worker->qxl->st->qif->release_resource(worker->qxl, surface->destroy);
        }

        region_destroy(&surface->draw_dirty_region);
        surface->context.canvas = NULL;
        WORKER_FOREACH_DCC(worker, link, dcc) {
            red_destroy_surface_item(worker, dcc, surface_id);
        }

        PANIC_ON(!ring_is_empty(&surface->depend_on_me));
    }
}

static inline void set_surface_release_info(RedWorker *worker, uint32_t surface_id, int is_create,
                                            QXLReleaseInfo *release_info, uint32_t group_id)
{
    RedSurface *surface;

    surface = &worker->surfaces[surface_id];

    if (is_create) {
        surface->create.info = release_info;
        surface->create.group_id = group_id;
    } else {
        surface->destroy.info = release_info;
        surface->destroy.group_id = group_id;
    }
}

static RedDrawable *ref_red_drawable(RedDrawable *drawable)
{
    drawable->refs++;
    return drawable;
}


static inline void put_red_drawable(RedWorker *worker, RedDrawable *drawable, uint32_t group_id,
                                     SpiceImage *self_bitmap)
{
    QXLReleaseInfoExt release_info_ext;

    if (self_bitmap) {
        red_put_image(self_bitmap);
    }
    if (--drawable->refs) {
        return;
    }

    worker->red_drawable_count--;
    release_info_ext.group_id = group_id;
    release_info_ext.info = drawable->release_info;
    worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
    red_put_drawable(drawable);
    free(drawable);
}

static void remove_depended_item(DependItem *item)
{
    ASSERT(item->drawable);
    ASSERT(ring_item_is_linked(&item->ring_item));
    item->drawable = NULL;
    ring_remove(&item->ring_item);
}

static inline void red_dec_surfaces_drawable_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id == -1) {
            continue;
        }
        red_destroy_surface(worker, surface_id);
    }
}

static void remove_drawable_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            remove_depended_item(&drawable->depend_items[x]);
        }
    }
}

static inline void release_drawable(RedWorker *worker, Drawable *drawable)
{
    RingItem *item, *next;

    if (!--drawable->refs) {
        ASSERT(!drawable->stream);
        ASSERT(!drawable->tree_item.shadow);
        ASSERT(ring_is_empty(&drawable->pipes));
        region_destroy(&drawable->tree_item.base.rgn);

        remove_drawable_dependencies(worker, drawable);
        red_dec_surfaces_drawable_dependencies(worker, drawable);
        red_destroy_surface(worker, drawable->surface_id);

        RING_FOREACH_SAFE(item, next, &drawable->glz_ring) {
            SPICE_CONTAINEROF(item, RedGlzDrawable, drawable_link)->drawable = NULL;
            ring_remove(item);
        }
        put_red_drawable(worker, drawable->red_drawable,
                          drawable->group_id, drawable->self_bitmap);
        free_drawable(worker, drawable);
        worker->drawable_count--;
    }
}

static inline void remove_shadow(RedWorker *worker, DrawItem *item)
{
    Shadow *shadow;

    if (!item->shadow) {
        return;
    }
    shadow = item->shadow;
    item->shadow = NULL;
    ring_remove(&shadow->base.siblings_link);
    region_destroy(&shadow->base.rgn);
    region_destroy(&shadow->on_hold);
    free(shadow);
    worker->shadows_count--;
}

static inline void current_remove_container(RedWorker *worker, Container *container)
{
    ASSERT(ring_is_empty(&container->items));
    worker->containers_count--;
    ring_remove(&container->base.siblings_link);
    region_destroy(&container->base.rgn);
    free(container);
}

static inline void container_cleanup(RedWorker *worker, Container *container)
{
    while (container && container->items.next == container->items.prev) {
        Container *next = container->base.container;
        if (container->items.next != &container->items) {
            TreeItem *item = (TreeItem *)ring_get_head(&container->items);
            ASSERT(item);
            ring_remove(&item->siblings_link);
            ring_add_after(&item->siblings_link, &container->base.siblings_link);
            item->container = container->base.container;
        }
        current_remove_container(worker, container);
        container = next;
    }
}

static inline void red_add_item_trace(RedWorker *worker, Drawable *item)
{
    ItemTrace *trace;
    if (!item->streamable) {
        return;
    }

    trace = &worker->items_trace[worker->next_item_trace++ & ITEMS_TRACE_MASK];
    trace->time = item->creation_time;
    trace->frames_count = item->frames_count;
    trace->gradual_frames_count = item->gradual_frames_count;
    trace->last_gradual_frame = item->last_gradual_frame;
    SpiceRect* src_area = &item->red_drawable->u.copy.src_area;
    trace->width = src_area->right - src_area->left;
    trace->height = src_area->bottom - src_area->top;
    trace->dest_area = item->red_drawable->bbox;
}

static void surface_flush(RedWorker *worker, int surface_id, SpiceRect *rect)
{
    red_update_area(worker, rect, surface_id);
}

static void red_flush_source_surfaces(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            remove_depended_item(&drawable->depend_items[x]);
            surface_flush(worker, surface_id, &drawable->red_drawable->surfaces_rects[x]);
        }
    }
}

static inline void current_remove_drawable(RedWorker *worker, Drawable *item)
{
    if (item->tree_item.effect != QXL_EFFECT_OPAQUE) {
        worker->transparent_count--;
    }
    if (item->stream) {
        red_detach_stream(worker, item->stream);
    } else {
        red_add_item_trace(worker, item);
    }
    remove_shadow(worker, &item->tree_item);
    ring_remove(&item->tree_item.base.siblings_link);
    ring_remove(&item->list_link);
    ring_remove(&item->surface_list_link);
    release_drawable(worker, item);
    worker->current_size--;
}

static void remove_drawable(RedWorker *worker, Drawable *drawable)
{
    red_pipes_remove_drawable(drawable);
    current_remove_drawable(worker, drawable);
}

static inline void current_remove(RedWorker *worker, TreeItem *item)
{
    TreeItem *now = item;

    for (;;) {
        Container *container = now->container;
        RingItem *ring_item;

        if (now->type == TREE_ITEM_TYPE_DRAWABLE) {
            ring_item = now->siblings_link.prev;
            remove_drawable(worker, SPICE_CONTAINEROF(now, Drawable, tree_item));
        } else {
            Container *container = (Container *)now;

            ASSERT(now->type == TREE_ITEM_TYPE_CONTAINER);

            if ((ring_item = ring_get_head(&container->items))) {
                now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
                continue;
            }
            ring_item = now->siblings_link.prev;
            current_remove_container(worker, container);
        }
        if (now == item) {
            return;
        }

        if ((ring_item = ring_next(&container->items, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        } else {
            now = (TreeItem *)container;
        }
    }
}

static void current_tree_for_each(Ring *ring, void (*f)(TreeItem *, void *), void * data)
{
    RingItem *ring_item;
    Ring *top_ring;

    if (!(ring_item = ring_get_head(ring))) {
        return;
    }
    top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);

        f(now, data);

        if (now->type == TREE_ITEM_TYPE_CONTAINER) {
            Container *container = (Container *)now;

            if ((ring_item = ring_get_head(&container->items))) {
                ring = &container->items;
                continue;
            }
        }
        for (;;) {
            ring_item = ring_next(ring, &now->siblings_link);
            if (ring_item) {
                break;
            }
            if (ring == top_ring) {
                return;
            }
            now = (TreeItem *)now->container;
            ring = (now->container) ? &now->container->items : top_ring;
        }
    }
}

static void red_current_clear(RedWorker *worker, int surface_id)
{
    RingItem *ring_item;

    while ((ring_item = ring_get_head(&worker->surfaces[surface_id].current))) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        current_remove(worker, now);
    }
}

static void red_clear_surface_drawables_from_pipe(DisplayChannelClient *dcc, int surface_id,
                                                  int force)
{
    Ring *ring;
    PipeItem *item;
    int x;
    RedChannelClient *rcc;

    if (!dcc) {
        return;
    }

    /* removing the newest drawables that their destination is surface_id and
       no other drawable depends on them */

    rcc = &dcc->common.base;
    ring = &dcc->common.base.pipe;
    item = (PipeItem *) ring;
    while ((item = (PipeItem *)ring_next(ring, (RingItem *)item))) {
        Drawable *drawable;
        DrawablePipeItem *dpi = NULL;
        int depend_found = FALSE;

        if (item->type == PIPE_ITEM_TYPE_DRAW) {
            dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item);
            drawable = dpi->drawable;
        } else if (item->type == PIPE_ITEM_TYPE_UPGRADE) {
            drawable = ((UpgradeItem *)item)->drawable;
        } else {
            continue;
        }

        if (drawable->surface_id == surface_id) {
            PipeItem *tmp_item = item;
            item = (PipeItem *)ring_prev(ring, (RingItem *)item);
            red_channel_client_pipe_remove_and_release(rcc, tmp_item);
            if (!item) {
                item = (PipeItem *)ring;
            }
            continue;
        }

        for (x = 0; x < 3; ++x) {
            if (drawable->surfaces_dest[x] == surface_id) {
                depend_found = TRUE;
                break;
            }
        }

        if (depend_found) {
            if (force) {
                break;
            } else {
                return;
            }
        }
    }

    if (item) {
        red_wait_pipe_item_sent(&dcc->common.base, item);
    }
}

static void red_wait_outgoing_item(RedChannelClient *rcc);
static void red_wait_outgoing_items(RedChannel *channel);

static void red_clear_surface_drawables_from_pipes(RedWorker *worker, int surface_id,
    int force, int wait_for_outgoing_item)
{
    RingItem *item;
    DisplayChannelClient *dcc;

    WORKER_FOREACH_DCC(worker, item, dcc) {
        red_clear_surface_drawables_from_pipe(dcc, surface_id, force);
        if (wait_for_outgoing_item) {
            // in case that the pipe didn't contain any item that is dependent on the surface, but
            // there is one during sending.
            red_wait_outgoing_item(&dcc->common.base);
        }
    }
}

#ifdef PIPE_DEBUG

static void print_rgn(const char* prefix, const QRegion* rgn)
{
    int i;
    printf("TEST: %s: RGN: bbox %u %u %u %u\n",
           prefix,
           rgn->bbox.top,
           rgn->bbox.left,
           rgn->bbox.bottom,
           rgn->bbox.right);

    for (i = 0; i < rgn->num_rects; i++) {
        printf("TEST: %s: RECT %u %u %u %u\n",
               prefix,
               rgn->rects[i].top,
               rgn->rects[i].left,
               rgn->rects[i].bottom,
               rgn->rects[i].right);
    }
}

static void print_draw_item(const char* prefix, const DrawItem *draw_item)
{
    const TreeItem *base = &draw_item->base;
    const Drawable *drawable = SPICE_CONTAINEROF(draw_item, Drawable, tree_item);
    printf("TEST: %s: draw id %u container %u effect %u",
           prefix,
           base->id, base->container ? base->container->base.id : 0,
           draw_item->effect);
    if (draw_item->shadow) {
        printf(" shadow %u\n", draw_item->shadow->base.id);
    } else {
        printf("\n");
    }
    print_rgn(prefix, &base->rgn);
}

static void print_shadow_item(const char* prefix, const Shadow *item)
{
    printf("TEST: %s: shadow %p id %d\n", prefix, item, item->base.id);
    print_rgn(prefix, &item->base.rgn);
}

static void print_container_item(const char* prefix, const Container *item)
{
    printf("TEST: %s: container %p id %d\n", prefix, item, item->base.id);
    print_rgn(prefix, &item->base.rgn);
}

static void print_base_item(const char* prefix, const TreeItem *base)
{
    switch (base->type) {
    case TREE_ITEM_TYPE_DRAWABLE:
        print_draw_item(prefix, (const DrawItem *)base);
        break;
    case TREE_ITEM_TYPE_SHADOW:
        print_shadow_item(prefix, (const Shadow *)base);
        break;
    case TREE_ITEM_TYPE_CONTAINER:
        print_container_item(prefix, (const Container *)base);
        break;
    default:
        red_error("invalid type %u", base->type);
    }
}

void __show_current(TreeItem *item, void *data)
{
    print_base_item("TREE", item);
}

static void show_current(RedWorker *worker, Ring *ring)
{
    if (ring_is_empty(ring)) {
        red_printf("TEST: TREE: EMPTY");
        return;
    }
    current_tree_for_each(ring, __show_current, NULL);
}

#else
#define print_rgn(a, b)
#define print_draw_private(a, b)
#define show_current(a, r)
#define print_shadow_item(a, b)
#define print_base_item(a, b)
#endif

static inline Shadow *__find_shadow(TreeItem *item)
{
    while (item->type == TREE_ITEM_TYPE_CONTAINER) {
        if (!(item = (TreeItem *)ring_get_tail(&((Container *)item)->items))) {
            return NULL;
        }
    }

    if (item->type != TREE_ITEM_TYPE_DRAWABLE) {
        return NULL;
    }

    return ((DrawItem *)item)->shadow;
}

static inline Ring *ring_of(RedWorker *worker, Ring *ring, TreeItem *item)
{
    return (item->container) ? &item->container->items : ring;
}

static inline int __contained_by(RedWorker *worker, TreeItem *item, Ring *ring)
{
    ASSERT(item && ring);
    do {
        Ring *now = ring_of(worker, ring, item);
        if (now == ring) {
            return TRUE;
        }
    } while ((item = (TreeItem *)item->container));

    return FALSE;
}

static inline void __exclude_region(RedWorker *worker, Ring *ring, TreeItem *item, QRegion *rgn,
                                    Ring **top_ring, Drawable *frame_candidate)
{
    QRegion and_rgn;
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif

    region_clone(&and_rgn, rgn);
    region_and(&and_rgn, &item->rgn);
    if (!region_is_empty(&and_rgn)) {
        if (IS_DRAW_ITEM(item)) {
            DrawItem *draw = (DrawItem *)item;

            if (draw->effect == QXL_EFFECT_OPAQUE) {
                region_exclude(rgn, &and_rgn);
            }

            if (draw->shadow) {
                Shadow *shadow;
                int32_t x = item->rgn.extents.x1;
                int32_t y = item->rgn.extents.y1;

                region_exclude(&draw->base.rgn, &and_rgn);
                shadow = draw->shadow;
                region_offset(&and_rgn, shadow->base.rgn.extents.x1 - x,
                              shadow->base.rgn.extents.y1 - y);
                region_exclude(&shadow->base.rgn, &and_rgn);
                region_and(&and_rgn, &shadow->on_hold);
                if (!region_is_empty(&and_rgn)) {
                    region_exclude(&shadow->on_hold, &and_rgn);
                    region_or(rgn, &and_rgn);
                    // in flat representation of current, shadow is always his owner next
                    if (!__contained_by(worker, (TreeItem*)shadow, *top_ring)) {
                        *top_ring = ring_of(worker, ring, (TreeItem*)shadow);
                    }
                }
            } else {
                if (frame_candidate) {
                    Drawable *drawable = SPICE_CONTAINEROF(draw, Drawable, tree_item);
                    red_stream_maintenance(worker, frame_candidate, drawable);
                }
                region_exclude(&draw->base.rgn, &and_rgn);
            }
        } else if (item->type == TREE_ITEM_TYPE_CONTAINER) {
            region_exclude(&item->rgn, &and_rgn);

            if (region_is_empty(&item->rgn)) {  //assume container removal will follow
                Shadow *shadow;

                region_exclude(rgn, &and_rgn);
                if ((shadow = __find_shadow(item))) {
                    region_or(rgn, &shadow->on_hold);
                    if (!__contained_by(worker, (TreeItem*)shadow, *top_ring)) {
                        *top_ring = ring_of(worker, ring, (TreeItem*)shadow);
                    }
                }
            }
        } else {
            Shadow *shadow;

            ASSERT(item->type == TREE_ITEM_TYPE_SHADOW);
            shadow = (Shadow *)item;
            region_exclude(rgn, &and_rgn);
            region_or(&shadow->on_hold, &and_rgn);
        }
    }
    region_destroy(&and_rgn);
    stat_add(&worker->__exclude_stat, start_time);
}

static void exclude_region(RedWorker *worker, Ring *ring, RingItem *ring_item, QRegion *rgn,
                           TreeItem **last, Drawable *frame_candidate)
{
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif
    Ring *top_ring;

    if (!ring_item) {
        return;
    }

    top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;

        ASSERT(!region_is_empty(&now->rgn));

        if (region_intersects(rgn, &now->rgn)) {
            print_base_item("EXCLUDE2", now);
            __exclude_region(worker, ring, now, rgn, &top_ring, frame_candidate);
            print_base_item("EXCLUDE3", now);

            if (region_is_empty(&now->rgn)) {
                ASSERT(now->type != TREE_ITEM_TYPE_SHADOW);
                ring_item = now->siblings_link.prev;
                print_base_item("EXCLUDE_REMOVE", now);
                current_remove(worker, now);
                if (last && *last == now) {
                    *last = (TreeItem *)ring_next(ring, ring_item);
                }
            } else if (now->type == TREE_ITEM_TYPE_CONTAINER) {
                Container *container = (Container *)now;
                if ((ring_item = ring_get_head(&container->items))) {
                    ring = &container->items;
                    ASSERT(((TreeItem *)ring_item)->container);
                    continue;
                }
                ring_item = &now->siblings_link;
            }

            if (region_is_empty(rgn)) {
                stat_add(&worker->exclude_stat, start_time);
                return;
            }
        }

        while ((last && *last == (TreeItem *)ring_item) ||
               !(ring_item = ring_next(ring, ring_item))) {
            if (ring == top_ring) {
                stat_add(&worker->exclude_stat, start_time);
                return;
            }
            ring_item = &container->base.siblings_link;
            container = container->base.container;
            ring = (container) ? &container->items : top_ring;
        }
    }
}

static inline Container *__new_container(RedWorker *worker, DrawItem *item)
{
    Container *container = spice_new(Container, 1);
    worker->containers_count++;
#ifdef PIPE_DEBUG
    container->base.id = ++worker->last_id;
#endif
    container->base.type = TREE_ITEM_TYPE_CONTAINER;
    container->base.container = item->base.container;
    item->base.container = container;
    item->container_root = TRUE;
    region_clone(&container->base.rgn, &item->base.rgn);
    ring_item_init(&container->base.siblings_link);
    ring_add_after(&container->base.siblings_link, &item->base.siblings_link);
    ring_remove(&item->base.siblings_link);
    ring_init(&container->items);
    ring_add(&container->items, &item->base.siblings_link);

    return container;
}

static inline int is_opaque_item(TreeItem *item)
{
    return item->type == TREE_ITEM_TYPE_CONTAINER ||
           (IS_DRAW_ITEM(item) && ((DrawItem *)item)->effect == QXL_EFFECT_OPAQUE);
}

static inline void __current_add_drawable(RedWorker *worker, Drawable *drawable, RingItem *pos)
{
    RedSurface *surface;
    uint32_t surface_id = drawable->surface_id;

    surface = &worker->surfaces[surface_id];
    ring_add_after(&drawable->tree_item.base.siblings_link, pos);
    ring_add(&worker->current_list, &drawable->list_link);
    ring_add(&surface->current_list, &drawable->surface_list_link);
    worker->current_size++;
    drawable->refs++;
}

static int is_equal_path(RedWorker *worker, SpicePath *path1, SpicePath *path2)
{
    SpicePathSeg *seg1, *seg2;
    int i, j;

    if (path1->num_segments != path2->num_segments)
        return FALSE;

    for (i = 0; i < path1->num_segments; i++) {
        seg1 = path1->segments[i];
        seg2 = path2->segments[i];

        if (seg1->flags != seg2->flags ||
            seg1->count != seg2->count) {
            return FALSE;
        }
        for (j = 0; j < seg1->count; j++) {
            if (seg1->points[j].x != seg2->points[j].x ||
                seg1->points[j].y != seg2->points[j].y) {
                return FALSE;
            }
        }
    }

    return TRUE;
}

// partial imp
static int is_equal_brush(SpiceBrush *b1, SpiceBrush *b2)
{
    return b1->type == b2->type &&
           b1->type == SPICE_BRUSH_TYPE_SOLID &&
           b1->u.color == b1->u.color;
}

// partial imp
static int is_equal_line_attr(SpiceLineAttr *a1, SpiceLineAttr *a2)
{
    return a1->flags == a2->flags &&
           a1->style_nseg == a2->style_nseg &&
           a1->style_nseg == 0;
}

// partial imp
static int is_same_geometry(RedWorker *worker, Drawable *d1, Drawable *d2)
{
    if (d1->red_drawable->type != d2->red_drawable->type) {
        return FALSE;
    }

    switch (d1->red_drawable->type) {
    case QXL_DRAW_STROKE:
        return is_equal_line_attr(&d1->red_drawable->u.stroke.attr,
                                  &d2->red_drawable->u.stroke.attr) &&
               is_equal_path(worker, d1->red_drawable->u.stroke.path,
                             d2->red_drawable->u.stroke.path);
    case QXL_DRAW_FILL:
        return rect_is_equal(&d1->red_drawable->bbox, &d2->red_drawable->bbox);
    default:
        return FALSE;
    }
}

static int is_same_drawable(RedWorker *worker, Drawable *d1, Drawable *d2)
{
    if (!is_same_geometry(worker, d1, d2)) {
        return FALSE;
    }

    switch (d1->red_drawable->type) {
    case QXL_DRAW_STROKE:
        return is_equal_brush(&d1->red_drawable->u.stroke.brush,
                              &d2->red_drawable->u.stroke.brush);
    case QXL_DRAW_FILL:
        return is_equal_brush(&d1->red_drawable->u.fill.brush,
                              &d2->red_drawable->u.fill.brush);
    default:
        return FALSE;
    }
}

static inline void red_free_stream(RedWorker *worker, Stream *stream)
{
    stream->next = worker->free_streams;
    worker->free_streams = stream;
}

static void red_release_stream(RedWorker *worker, Stream *stream)
{
    if (!--stream->refs) {
        ASSERT(!ring_item_is_linked(&stream->link));
        if (stream->mjpeg_encoder) {
            mjpeg_encoder_destroy(stream->mjpeg_encoder);
        }
        red_free_stream(worker, stream);
        worker->stream_count--;
    }
}

static inline void red_detach_stream(RedWorker *worker, Stream *stream)
{
    ASSERT(stream->current && stream->current->stream);
    ASSERT(stream->current->stream == stream);
    stream->current->stream = NULL;
    stream->current = NULL;
}

static StreamClipItem *__new_stream_clip(DisplayChannelClient* dcc, StreamAgent *agent)
{
    StreamClipItem *item = spice_new(StreamClipItem, 1);
    red_channel_pipe_item_init(dcc->common.base.channel,
                    (PipeItem *)item, PIPE_ITEM_TYPE_STREAM_CLIP);

    item->stream_agent = agent;
    agent->stream->refs++;
    item->refs = 1;
    return item;
}

static void push_stream_clip_by_drawable(DisplayChannelClient* dcc, StreamAgent *agent,
                                         Drawable *drawable)
{
    StreamClipItem *item = __new_stream_clip(dcc, agent);
    int n_rects;

    if (!item) {
        PANIC("alloc failed");
    }

    if (drawable->red_drawable->clip.type == SPICE_CLIP_TYPE_NONE) {
        item->rects = NULL;
        item->clip_type = SPICE_CLIP_TYPE_NONE;
        item->rects = NULL;
    } else {
        item->clip_type = SPICE_CLIP_TYPE_RECTS;
        n_rects = pixman_region32_n_rects(&drawable->tree_item.base.rgn);

        item->rects = spice_malloc_n_m(n_rects, sizeof(SpiceRect), sizeof(SpiceClipRects));
        item->rects->num_rects = n_rects;
        region_ret_rects(&drawable->tree_item.base.rgn, item->rects->rects, n_rects);
    }
    red_channel_client_pipe_add(&dcc->common.base, (PipeItem *)item);
}

static void push_stream_clip(DisplayChannelClient* dcc, StreamAgent *agent)
{
    StreamClipItem *item = __new_stream_clip(dcc, agent);
    int n_rects;

    if (!item) {
        PANIC("alloc failed");
    }
    item->clip_type = SPICE_CLIP_TYPE_RECTS;

    n_rects = pixman_region32_n_rects(&agent->vis_region);

    item->rects = spice_malloc_n_m(n_rects, sizeof(SpiceRect), sizeof(SpiceClipRects));
    item->rects->num_rects = n_rects;
    region_ret_rects(&agent->vis_region, item->rects->rects, n_rects);
    red_channel_client_pipe_add(&dcc->common.base, (PipeItem *)item);
}

static void red_display_release_stream_clip(RedWorker *worker, StreamClipItem *item)
{
    if (!--item->refs) {
        red_display_release_stream(worker, item->stream_agent);
        free(item->rects);
        free(item);
    }
}

static void red_attach_stream(RedWorker *worker, Drawable *drawable, Stream *stream)
{
    DisplayChannelClient *dcc;
    StreamAgent *agent;
    RingItem *item;

    ASSERT(!drawable->stream && !stream->current);
    ASSERT(drawable && stream);
    stream->current = drawable;
    drawable->stream = stream;
    stream->last_time = drawable->creation_time;

    WORKER_FOREACH_DCC(worker, item, dcc) {
        agent = &dcc->stream_agents[stream - worker->streams_buf];
        if (!region_is_equal(&agent->vis_region, &drawable->tree_item.base.rgn)) {
            region_destroy(&agent->vis_region);
            region_clone(&agent->vis_region, &drawable->tree_item.base.rgn);
            push_stream_clip_by_drawable(dcc, agent, drawable);
        }
    }
}

static void red_stop_stream(RedWorker *worker, Stream *stream)
{
    DisplayChannelClient *dcc;
    RingItem *item;

    ASSERT(ring_item_is_linked(&stream->link));
    ASSERT(!stream->current);
    WORKER_FOREACH_DCC(worker, item, dcc) {
        StreamAgent *stream_agent;
        stream_agent = &dcc->stream_agents[stream - worker->streams_buf];
        region_clear(&stream_agent->vis_region);
        ASSERT(!pipe_item_is_linked(&stream_agent->destroy_item));
        stream->refs++;
        red_channel_client_pipe_add(&dcc->common.base, &stream_agent->destroy_item);
    }
    ring_remove(&stream->link);
    red_release_stream(worker, stream);
}

static int drawable_is_linked(Drawable *drawable)
{
    return !ring_is_empty(&drawable->pipes);
}

static inline void red_detach_stream_gracefully(RedWorker *worker, Stream *stream)
{
    RedChannel *channel;
    RingItem *item;
    RedChannelClient *rcc;
    DisplayChannelClient *dcc;

    ASSERT(stream->current);
    WORKER_FOREACH_DCC(worker, item, dcc) {
        UpgradeItem *upgrade_item;
        int n_rects;
        if (drawable_is_linked(stream->current)) {
            continue;
        }
        rcc = &dcc->common.base;
        channel = rcc->channel;
        upgrade_item = spice_new(UpgradeItem, 1);
        upgrade_item->refs = 1;
        red_channel_pipe_item_init(channel,
                &upgrade_item->base, PIPE_ITEM_TYPE_UPGRADE);
        upgrade_item->drawable = stream->current;
        upgrade_item->drawable->refs++;
        n_rects = pixman_region32_n_rects(&upgrade_item->drawable->tree_item.base.rgn);
        upgrade_item->rects = spice_malloc_n_m(n_rects, sizeof(SpiceRect), sizeof(SpiceClipRects));
        upgrade_item->rects->num_rects = n_rects;
        region_ret_rects(&upgrade_item->drawable->tree_item.base.rgn,
                         upgrade_item->rects->rects, n_rects);
        red_channel_client_pipe_add(rcc, &upgrade_item->base);
    }
    red_detach_stream(worker, stream);
}

// region should be a primary surface region
static void red_detach_streams_behind(RedWorker *worker, QRegion *region)
{
    Ring *ring = &worker->streams;
    RingItem *item = ring_get_head(ring);
    RingItem *dcc_ring_item;
    DisplayChannelClient *dcc;
    int has_clients = display_is_connected(worker);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        int detach_stream = 0;
        item = ring_next(ring, item);

        WORKER_FOREACH_DCC(worker, dcc_ring_item, dcc) {
            StreamAgent *agent = &dcc->stream_agents[stream - worker->streams_buf];
            if (region_intersects(&agent->vis_region, region)) {
                region_clear(&agent->vis_region);
                push_stream_clip(dcc, agent);
                detach_stream = 1;
            }
        }
        if (!has_clients) {
            if (stream->current &&
                region_intersects(&stream->current->tree_item.base.rgn, region)) {
                red_detach_stream(worker, stream);
            }
        }
        if (detach_stream) {
            if (stream->current) {
                red_detach_stream_gracefully(worker, stream);
            }
        }
    }
}

static void red_streams_update_clip(RedWorker *worker, Drawable *drawable)
{
    Ring *ring;
    RingItem *item;
    RingItem *dcc_ring_item;
    DisplayChannelClient *dcc;

    if (!display_is_connected(worker)) {
        return;
    }

    if (!is_primary_surface(worker, drawable->surface_id)) {
        return;
    }

    ring = &worker->streams;
    item = ring_get_head(ring);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        StreamAgent *agent;

        item = ring_next(ring, item);

        if (stream->current == drawable) {
            continue;
        }

        WORKER_FOREACH_DCC(worker, dcc_ring_item, dcc) {
            agent = &dcc->stream_agents[stream - worker->streams_buf];

            if (region_intersects(&agent->vis_region, &drawable->tree_item.base.rgn)) {
                region_exclude(&agent->vis_region, &drawable->tree_item.base.rgn);
                push_stream_clip(dcc, agent);
            }
        }
    }
}

static inline unsigned int red_get_streams_timout(RedWorker *worker)
{
    unsigned int timout = -1;
    Ring *ring = &worker->streams;
    RingItem *item = ring;
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    red_time_t now = timespec_to_red_time(&time);
    while ((item = ring_next(ring, item))) {
        Stream *stream;

        stream = SPICE_CONTAINEROF(item, Stream, link);
        red_time_t delta = (stream->last_time + RED_STREAM_TIMOUT) - now;

        if (delta < 1000 * 1000) {
            return 0;
        }
        timout = MIN(timout, (unsigned int)(delta / (1000 * 1000)));
    }
    return timout;
}

static inline void red_handle_streams_timout(RedWorker *worker)
{
    Ring *ring = &worker->streams;
    struct timespec time;
    RingItem *item;

    clock_gettime(CLOCK_MONOTONIC, &time);
    red_time_t now = timespec_to_red_time(&time);
    item = ring_get_head(ring);
    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        item = ring_next(ring, item);
        if (now >= (stream->last_time + RED_STREAM_TIMOUT)) {
            if (stream->current) {
                red_detach_stream_gracefully(worker, stream);
            }
            red_stop_stream(worker, stream);
        }
    }
}

static void red_display_release_stream(RedWorker *worker, StreamAgent *agent)
{
    ASSERT(agent->stream);
    red_release_stream(worker, agent->stream);
}

static inline Stream *red_alloc_stream(RedWorker *worker)
{
    Stream *stream;
    if (!worker->free_streams) {
        return NULL;
    }
    stream = worker->free_streams;
    worker->free_streams = worker->free_streams->next;
    return stream;
}

static int get_bit_rate(DisplayChannelClient *dcc,
    int width, int height)
{
    uint64_t bit_rate = width * height * BEST_BIT_RATE_PER_PIXEL;
    MainChannelClient *mcc;
    int is_low_bandwidth = 0;

    if (dcc) {
        mcc = red_client_get_main(dcc->common.base.client);
        is_low_bandwidth = main_channel_client_is_low_bandwidth(mcc);
    }

    if (is_low_bandwidth) {
        bit_rate = MIN(main_channel_client_get_bitrate_per_sec(mcc) * 70 / 100, bit_rate);
        bit_rate = MAX(bit_rate, width * height * WORST_BIT_RATE_PER_PIXEL);
    }
    return bit_rate;
}

static int get_minimal_bit_rate(RedWorker *worker, int width, int height)
{
    RingItem *item;
    DisplayChannelClient *dcc;
    int ret = INT_MAX;

    WORKER_FOREACH_DCC(worker, item, dcc) {
        int bit_rate = get_bit_rate(dcc, width, height);
        if (bit_rate < ret) {
            ret = bit_rate;
        }
    }
    return ret;
}

static void red_display_create_stream(DisplayChannelClient *dcc, Stream *stream)
{
    StreamAgent *agent = &dcc->stream_agents[stream - dcc->common.worker->streams_buf];

    stream->refs++;
    ASSERT(region_is_empty(&agent->vis_region));
    if (stream->current) {
        agent->frames = 1;
        region_clone(&agent->vis_region, &stream->current->tree_item.base.rgn);
    } else {
        agent->frames = 0;
    }
    agent->drops = 0;
    agent->fps = MAX_FPS;
    reset_rate(dcc, agent);
    red_channel_client_pipe_add(&dcc->common.base, &agent->create_item);
}

/* TODO: we create the stream even if dcc is NULL, i.e. no client - or
 * maybe we can't reach this function in that case? question: do we want to? */
static void red_create_stream(RedWorker *worker, Drawable *drawable)
{
    DisplayChannelClient *dcc;
    RingItem *dcc_ring_item;
    Stream *stream;
    SpiceRect* src_rect;
    int stream_width;
    int stream_height;

    ASSERT(!drawable->stream);

    if (!(stream = red_alloc_stream(worker))) {
        return;
    }

    ASSERT(drawable->red_drawable->type == QXL_DRAW_COPY);
    src_rect = &drawable->red_drawable->u.copy.src_area;
    stream_width = src_rect->right - src_rect->left;
    stream_height = src_rect->bottom - src_rect->top;

    stream->mjpeg_encoder = mjpeg_encoder_new(stream_width, stream_height);

    ring_add(&worker->streams, &stream->link);
    stream->current = drawable;
    stream->last_time = drawable->creation_time;
    stream->width = src_rect->right - src_rect->left;
    stream->height = src_rect->bottom - src_rect->top;
    stream->dest_area = drawable->red_drawable->bbox;
    stream->refs = 1;
    stream->bit_rate = get_minimal_bit_rate(worker, stream_width, stream_height);
    SpiceBitmap *bitmap = &drawable->red_drawable->u.copy.src_bitmap->u.bitmap;
    stream->top_down = !!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN);
    drawable->stream = stream;

    WORKER_FOREACH_DCC(worker, dcc_ring_item, dcc) {
        red_display_create_stream(dcc, stream);
    }
    worker->stream_count++;

    return;
}

static void red_disply_start_streams(DisplayChannelClient *dcc)
{
    Ring *ring = &dcc->common.worker->streams;
    RingItem *item = ring;

    while ((item = ring_next(ring, item))) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        red_display_create_stream(dcc, stream);
    }
}

static void red_display_client_init_streams(DisplayChannelClient *dcc)
{
    int i;
    RedWorker *worker = dcc->common.worker;
    RedChannel *channel = dcc->common.base.channel;

    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &dcc->stream_agents[i];
        agent->stream = &worker->streams_buf[i];
        region_init(&agent->vis_region);
        red_channel_pipe_item_init(channel, &agent->create_item, PIPE_ITEM_TYPE_STREAM_CREATE);
        red_channel_pipe_item_init(channel, &agent->destroy_item, PIPE_ITEM_TYPE_STREAM_DESTROY);
    }
}

static void red_display_destroy_streams(DisplayChannelClient *dcc)
{
    int i;

    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &dcc->stream_agents[i];
        region_destroy(&agent->vis_region);
    }
}

static void red_init_streams(RedWorker *worker)
{
    int i;

    ring_init(&worker->streams);
    worker->free_streams = NULL;
    for (i = 0; i < NUM_STREAMS; i++) {
        Stream *stream = &worker->streams_buf[i];
        ring_item_init(&stream->link);
        red_free_stream(worker, stream);
    }
}

static inline int __red_is_next_stream_frame(RedWorker *worker,
                                             const Drawable *candidate,
                                             const int other_src_width,
                                             const int other_src_height,
                                             const SpiceRect *other_dest,
                                             const red_time_t other_time,
                                             const Stream *stream)
{
    RedDrawable *red_drawable;

    if (candidate->creation_time - other_time >
            (stream ? RED_STREAM_CONTINUS_MAX_DELTA : RED_STREAM_DETACTION_MAX_DELTA)) {
        return FALSE;
    }

    red_drawable = candidate->red_drawable;

    if (!rect_is_equal(&red_drawable->bbox, other_dest)) {
        return FALSE;
    }

    SpiceRect* candidate_src = &red_drawable->u.copy.src_area;
    if (candidate_src->right - candidate_src->left != other_src_width ||
        candidate_src->bottom - candidate_src->top != other_src_height) {
        return FALSE;
    }

    if (stream) {
        SpiceBitmap *bitmap = &red_drawable->u.copy.src_bitmap->u.bitmap;
        if (stream->top_down != !!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
            return FALSE;
        }
    }
    return TRUE;
}

static inline int red_is_next_stream_frame(RedWorker *worker, const Drawable *candidate,
                                           const Drawable *prev)
{
    if (!candidate->streamable) {
        return FALSE;
    }

    SpiceRect* prev_src = &prev->red_drawable->u.copy.src_area;
    return __red_is_next_stream_frame(worker, candidate, prev_src->right - prev_src->left,
                                      prev_src->bottom - prev_src->top,
                                      &prev->red_drawable->bbox, prev->creation_time,
                                      prev->stream);
}

static void reset_rate(DisplayChannelClient *dcc, StreamAgent *stream_agent)
{
    Stream *stream = stream_agent->stream;
    int rate;

    rate = get_bit_rate(dcc, stream->width, stream->height);
    if (rate == stream->bit_rate) {
        return;
    }

    /* MJpeg has no rate limiting anyway, so do nothing */
}

static int display_channel_client_is_low_bandwidth(DisplayChannelClient *dcc)
{
    return main_channel_client_is_low_bandwidth(
        red_client_get_main(red_channel_client_get_client(&dcc->common.base)));
}

static inline void pre_stream_item_swap(RedWorker *worker, Stream *stream)
{
    DrawablePipeItem *dpi;
    DisplayChannelClient *dcc;
    int index;
    StreamAgent *agent;
    RingItem *ring_item;

    ASSERT(stream->current);

    if (!display_is_connected(worker)) {
        return;
    }

    index = stream - worker->streams_buf;
    DRAWABLE_FOREACH_DPI(stream->current, ring_item, dpi) {
        dcc = dpi->dcc;
        if (!display_channel_client_is_low_bandwidth(dcc)) {
            continue;
        }
        agent = &dcc->stream_agents[index];

        if (pipe_item_is_linked(&dpi->dpi_pipe_item)) {
            ++agent->drops;
        }

        if (agent->frames / agent->fps < FPS_TEST_INTERVAL) {
            agent->frames++;
            return;
        }

        double drop_factor = ((double)agent->frames - (double)agent->drops) /
                             (double)agent->frames;

        if (drop_factor == 1) {
            if (agent->fps < MAX_FPS) {
                agent->fps++;
            }
        } else if (drop_factor < 0.9) {
            if (agent->fps > 1) {
                agent->fps--;
            }
        }
        agent->frames = 1;
        agent->drops = 0;
    }
}

static inline void red_update_copy_graduality(RedWorker* worker, Drawable *drawable)
{
    SpiceBitmap *bitmap;
    ASSERT(drawable->red_drawable->type == QXL_DRAW_COPY);

    if (worker->streaming_video != STREAM_VIDEO_FILTER) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_INVALID;
        return;
    }

    if (drawable->copy_bitmap_graduality != BITMAP_GRADUAL_INVALID) {
        return; // already set
    }

    bitmap = &drawable->red_drawable->u.copy.src_bitmap->u.bitmap;

    if (!BITMAP_FMT_IS_RGB[bitmap->format] || _stride_is_extra(bitmap) ||
        (bitmap->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE)) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_NOT_AVAIL;
    } else  {
        drawable->copy_bitmap_graduality =
            _get_bitmap_graduality_level(worker, bitmap,drawable->group_id);
    }
}

static inline int red_is_stream_start(Drawable *drawable)
{
    return ((drawable->frames_count >= RED_STREAM_FRAMES_START_CONDITION) &&
            (drawable->gradual_frames_count >=
            (RED_STREAM_GRADUAL_FRAMES_START_CONDITION * drawable->frames_count)));
}

// returns whether a stream was created
static int red_stream_add_frame(RedWorker *worker,
                                Drawable *frame_drawable,
                                int frames_count,
                                int gradual_frames_count,
                                int last_gradual_frame)
{
    red_update_copy_graduality(worker, frame_drawable);
    frame_drawable->frames_count = frames_count + 1;
    frame_drawable->gradual_frames_count  = gradual_frames_count;

    if (frame_drawable->copy_bitmap_graduality != BITMAP_GRADUAL_LOW) {
        if ((frame_drawable->frames_count - last_gradual_frame) >
            RED_STREAM_FRAMES_RESET_CONDITION) {
            frame_drawable->frames_count = 1;
            frame_drawable->gradual_frames_count = 1;
        } else {
            frame_drawable->gradual_frames_count++;
        }

        frame_drawable->last_gradual_frame = frame_drawable->frames_count;
    } else {
        frame_drawable->last_gradual_frame = last_gradual_frame;
    }

    if (red_is_stream_start(frame_drawable)) {
        red_create_stream(worker, frame_drawable);
        return TRUE;
    }
    return FALSE;
}

static inline void red_stream_maintenance(RedWorker *worker, Drawable *candidate, Drawable *prev)
{
    Stream *stream;

    if (candidate->stream) {
        return;
    }

    if (!red_is_next_stream_frame(worker, candidate, prev)) {
        return;
    }

    if ((stream = prev->stream)) {
        pre_stream_item_swap(worker, stream);
        red_detach_stream(worker, stream);
        prev->streamable = FALSE; //prevent item trace
        red_attach_stream(worker, candidate, stream);
    } else {
        red_stream_add_frame(worker, candidate,
                             prev->frames_count,
                             prev->gradual_frames_count,
                             prev->last_gradual_frame);
    }
}

static inline int is_drawable_independent_from_surfaces(Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        if (drawable->surfaces_dest[x] != -1) {
            return FALSE;
        }
    }
    return TRUE;
}

static inline int red_current_add_equal(RedWorker *worker, DrawItem *item, TreeItem *other)
{
    DrawItem *other_draw_item;
    Drawable *drawable;
    Drawable *other_drawable;

    if (other->type != TREE_ITEM_TYPE_DRAWABLE) {
        return FALSE;
    }
    other_draw_item = (DrawItem *)other;

    if (item->shadow || other_draw_item->shadow || item->effect != other_draw_item->effect) {
        return FALSE;
    }

    drawable = SPICE_CONTAINEROF(item, Drawable, tree_item);
    other_drawable = SPICE_CONTAINEROF(other_draw_item, Drawable, tree_item);

    if (item->effect == QXL_EFFECT_OPAQUE) {
        int add_after = !!other_drawable->stream &&
                        is_drawable_independent_from_surfaces(drawable);
        red_stream_maintenance(worker, drawable, other_drawable);
        __current_add_drawable(worker, drawable, &other->siblings_link);
        other_drawable->refs++;
        current_remove_drawable(worker, other_drawable);
        if (add_after) {
            red_pipes_add_drawable_after(worker, drawable, other_drawable);
        } else {
            red_pipes_add_drawable(worker, drawable);
        }
        red_pipes_remove_drawable(other_drawable);
        release_drawable(worker, other_drawable);
        return TRUE;
    }

    switch (item->effect) {
    case QXL_EFFECT_REVERT_ON_DUP:
        if (is_same_drawable(worker, drawable, other_drawable)) {

            DisplayChannelClient *dcc;
            DrawablePipeItem *dpi;
            RingItem *worker_ring_item, *dpi_ring_item;

            other_drawable->refs++;
            current_remove_drawable(worker, other_drawable);

            /* sending the drawable to clients that already received
             * (or will receive) other_drawable */
            worker_ring_item = ring_get_head(&worker->display_channel->common.base.clients);
            dpi_ring_item = ring_get_head(&other_drawable->pipes);
            /* dpi contains a sublist of dcc's, ordered the same */
            while (worker_ring_item) {
                dcc = SPICE_CONTAINEROF(worker_ring_item, DisplayChannelClient,
                                        common.base.channel_link);
                dpi = SPICE_CONTAINEROF(dpi_ring_item, DrawablePipeItem, base);
                while (worker_ring_item && (!dpi || dcc != dpi->dcc)) {
                    red_pipe_add_drawable(dcc, drawable);
                    worker_ring_item = ring_next(&worker->display_channel->common.base.clients,
                                                 worker_ring_item);
                    dcc = SPICE_CONTAINEROF(worker_ring_item, DisplayChannelClient,
                                            common.base.channel_link);
                }

                if (dpi_ring_item) {
                    dpi_ring_item = ring_next(&other_drawable->pipes, dpi_ring_item);
                }
                if (worker_ring_item) {
                    worker_ring_item = ring_next(&worker->display_channel->common.base.clients,
                                                 worker_ring_item);
                }
            }
            /* not sending other_drawable where possible */
            red_pipes_remove_drawable(other_drawable);

            release_drawable(worker, other_drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_OPAQUE_BRUSH:
        if (is_same_geometry(worker, drawable, other_drawable)) {
            __current_add_drawable(worker, drawable, &other->siblings_link);
            remove_drawable(worker, other_drawable);
            red_pipes_add_drawable(worker, drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_NOP_ON_DUP:
        if (is_same_drawable(worker, drawable, other_drawable)) {
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static inline void red_use_stream_trace(RedWorker *worker, Drawable *drawable)
{
    ItemTrace *trace;
    ItemTrace *trace_end;
    Ring *ring;
    RingItem *item;

    if (drawable->stream || !drawable->streamable || drawable->frames_count) {
        return;
    }


    ring = &worker->streams;
    item = ring_get_head(ring);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        if (!stream->current && __red_is_next_stream_frame(worker,
                                                           drawable,
                                                           stream->width,
                                                           stream->height,
                                                           &stream->dest_area,
                                                           stream->last_time,
                                                           stream)) {
            red_attach_stream(worker, drawable, stream);
            return;
        }
        item = ring_next(ring, item);
    }

    trace = worker->items_trace;
    trace_end = trace + NUM_TRACE_ITEMS;
    for (; trace < trace_end; trace++) {
        if (__red_is_next_stream_frame(worker, drawable, trace->width, trace->height,
                                       &trace->dest_area, trace->time, NULL)) {
            if (red_stream_add_frame(worker, drawable,
                                     trace->frames_count,
                                     trace->gradual_frames_count,
                                     trace->last_gradual_frame)) {
                return;
            }
        }
    }
}

static void red_reset_stream_trace(RedWorker *worker)
{
    Ring *ring = &worker->streams;
    RingItem *item = ring_get_head(ring);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        item = ring_next(ring, item);
        if (!stream->current) {
            red_stop_stream(worker, stream);
        } else {
            red_printf("attached stream");
        }
    }

    worker->next_item_trace = 0;
    memset(worker->items_trace, 0, sizeof(worker->items_trace));
}

static inline int red_current_add(RedWorker *worker, Ring *ring, Drawable *drawable)
{
    DrawItem *item = &drawable->tree_item;
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif
    RingItem *now;
    QRegion exclude_rgn;
    RingItem *exclude_base = NULL;

    print_base_item("ADD", &item->base);
    ASSERT(!region_is_empty(&item->base.rgn));
    region_init(&exclude_rgn);
    now = ring_next(ring, ring);

    while (now) {
        TreeItem *sibling = SPICE_CONTAINEROF(now, TreeItem, siblings_link);
        int test_res;

        if (!region_bounds_intersects(&item->base.rgn, &sibling->rgn)) {
            print_base_item("EMPTY", sibling);
            now = ring_next(ring, now);
            continue;
        }
        test_res = region_test(&item->base.rgn, &sibling->rgn, REGION_TEST_ALL);
        if (!(test_res & REGION_TEST_SHARED)) {
            print_base_item("EMPTY", sibling);
            now = ring_next(ring, now);
            continue;
        } else if (sibling->type != TREE_ITEM_TYPE_SHADOW) {
            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) &&
                                                   !(test_res & REGION_TEST_LEFT_EXCLUSIVE) &&
                                                   red_current_add_equal(worker, item, sibling)) {
                stat_add(&worker->add_stat, start_time);
                return FALSE;
            }

            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) && item->effect == QXL_EFFECT_OPAQUE) {
                Shadow *shadow;
                int skip = now == exclude_base;
                print_base_item("CONTAIN", sibling);

                if ((shadow = __find_shadow(sibling))) {
                    if (exclude_base) {
                        TreeItem *next = sibling;
                        exclude_region(worker, ring, exclude_base, &exclude_rgn, &next, NULL);
                        if (next != sibling) {
                            now = next ? &next->siblings_link : NULL;
                            exclude_base = NULL;
                            continue;
                        }
                    }
                    region_or(&exclude_rgn, &shadow->on_hold);
                }
                now = now->prev;
                current_remove(worker, sibling);
                now = ring_next(ring, now);
                if (shadow || skip) {
                    exclude_base = now;
                }
                continue;
            }

            if (!(test_res & REGION_TEST_LEFT_EXCLUSIVE) && is_opaque_item(sibling)) {
                Container *container;

                if (exclude_base) {
                    exclude_region(worker, ring, exclude_base, &exclude_rgn, NULL, NULL);
                    region_clear(&exclude_rgn);
                    exclude_base = NULL;
                }
                print_base_item("IN", sibling);
                if (sibling->type == TREE_ITEM_TYPE_CONTAINER) {
                    container = (Container *)sibling;
                    ring = &container->items;
                    item->base.container = container;
                    now = ring_next(ring, ring);
                    continue;
                }
                ASSERT(IS_DRAW_ITEM(sibling));
                if (!((DrawItem *)sibling)->container_root) {
                    container = __new_container(worker, (DrawItem *)sibling);
                    if (!container) {
                        red_printf("create new container failed");
                        region_destroy(&exclude_rgn);
                        return FALSE;
                    }
                    item->base.container = container;
                    ring = &container->items;
                }
            }
        }
        if (!exclude_base) {
            exclude_base = now;
        }
        break;
    }
    if (item->effect == QXL_EFFECT_OPAQUE) {
        region_or(&exclude_rgn, &item->base.rgn);
        exclude_region(worker, ring, exclude_base, &exclude_rgn, NULL, drawable);
        red_use_stream_trace(worker, drawable);
        red_streams_update_clip(worker, drawable);
    } else {
        if (drawable->surface_id == 0) {
            red_detach_streams_behind(worker, &drawable->tree_item.base.rgn);
        }
    }
    region_destroy(&exclude_rgn);
    __current_add_drawable(worker, drawable, ring);
    stat_add(&worker->add_stat, start_time);
    return TRUE;
}

static void add_clip_rects(QRegion *rgn, SpiceClipRects *data)
{
    int i;

    for (i = 0; i < data->num_rects; i++) {
        region_add(rgn, data->rects + i);
    }
}

static inline Shadow *__new_shadow(RedWorker *worker, Drawable *item, SpicePoint *delta)
{
    if (!delta->x && !delta->y) {
        return NULL;
    }

    Shadow *shadow = spice_new(Shadow, 1);
    worker->shadows_count++;
#ifdef PIPE_DEBUG
    shadow->base.id = ++worker->last_id;
#endif
    shadow->base.type = TREE_ITEM_TYPE_SHADOW;
    shadow->base.container = NULL;
    shadow->owner = &item->tree_item;
    region_clone(&shadow->base.rgn, &item->tree_item.base.rgn);
    region_offset(&shadow->base.rgn, delta->x, delta->y);
    ring_item_init(&shadow->base.siblings_link);
    region_init(&shadow->on_hold);
    item->tree_item.shadow = shadow;
    return shadow;
}

static inline int red_current_add_with_shadow(RedWorker *worker, Ring *ring, Drawable *item,
                                              SpicePoint *delta)
{
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif

    Shadow *shadow = __new_shadow(worker, item, delta);
    if (!shadow) {
        stat_add(&worker->add_stat, start_time);
        return FALSE;
    }
    print_base_item("ADDSHADOW", &item->tree_item.base);
    // item and his shadow must initially be placed in the same container.
    // for now putting them on root.

    // only primary surface streams are supported
    if (is_primary_surface(worker, item->surface_id)) {
        red_detach_streams_behind(worker, &shadow->base.rgn);
    }
    ring_add(ring, &shadow->base.siblings_link);
    __current_add_drawable(worker, item, ring);
    if (item->tree_item.effect == QXL_EFFECT_OPAQUE) {
        QRegion exclude_rgn;
        region_clone(&exclude_rgn, &item->tree_item.base.rgn);
        exclude_region(worker, ring, &shadow->base.siblings_link, &exclude_rgn, NULL, NULL);
        region_destroy(&exclude_rgn);
        red_streams_update_clip(worker, item);
    } else {
        if (item->surface_id == 0) {
            red_detach_streams_behind(worker, &item->tree_item.base.rgn);
        }
    }
    stat_add(&worker->add_stat, start_time);
    return TRUE;
}

static inline int has_shadow(RedDrawable *drawable)
{
    return drawable->type == QXL_COPY_BITS;
}

static inline void red_update_streamable(RedWorker *worker, Drawable *drawable,
                                         RedDrawable *red_drawable)
{
    SpiceImage *image;

    if (worker->streaming_video == STREAM_VIDEO_OFF) {
        return;
    }

    if (!is_primary_surface(worker, drawable->surface_id)) {
        return;
    }

    if (drawable->tree_item.effect != QXL_EFFECT_OPAQUE ||
                                        red_drawable->type != QXL_DRAW_COPY ||
                                        red_drawable->u.copy.rop_descriptor != SPICE_ROPD_OP_PUT) {
        return;
    }

    image = red_drawable->u.copy.src_bitmap;
    if (image == NULL ||
        image->descriptor.type != SPICE_IMAGE_TYPE_BITMAP) {
        return;
    }

    if (worker->streaming_video == STREAM_VIDEO_FILTER) {
        SpiceRect* rect;
        int size;

        rect = &drawable->red_drawable->u.copy.src_area;
        size = (rect->right - rect->left) * (rect->bottom - rect->top);
        if (size < RED_STREAM_MIN_SIZE) {
            return;
        }
    }

    drawable->streamable = TRUE;
}

static inline int red_current_add_qxl(RedWorker *worker, Ring *ring, Drawable *drawable,
                                      RedDrawable *red_drawable)
{
    int ret;

    if (has_shadow(red_drawable)) {
        SpicePoint delta;

#ifdef RED_WORKER_STAT
        ++worker->add_with_shadow_count;
#endif
        delta.x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left;
        delta.y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top;
        ret = red_current_add_with_shadow(worker, ring, drawable, &delta);
    } else {
        red_update_streamable(worker, drawable, red_drawable);
        ret = red_current_add(worker, ring, drawable);
    }
#ifdef RED_WORKER_STAT
    if ((++worker->add_count % 100) == 0) {
        stat_time_t total = worker->add_stat.total;
        red_printf("add with shadow count %u",
                   worker->add_with_shadow_count);
        worker->add_with_shadow_count = 0;
        red_printf("add[%u] %f exclude[%u] %f __exclude[%u] %f",
                   worker->add_stat.count,
                   stat_cpu_time_to_sec(total),
                   worker->exclude_stat.count,
                   stat_cpu_time_to_sec(worker->exclude_stat.total),
                   worker->__exclude_stat.count,
                   stat_cpu_time_to_sec(worker->__exclude_stat.total));
        red_printf("add %f%% exclude %f%% exclude2 %f%% __exclude %f%%",
                   (double)(total - worker->exclude_stat.total) / total * 100,
                   (double)(worker->exclude_stat.total) / total * 100,
                   (double)(worker->exclude_stat.total -
                            worker->__exclude_stat.total) / worker->exclude_stat.total * 100,
                   (double)(worker->__exclude_stat.total) / worker->exclude_stat.total * 100);
        stat_reset(&worker->add_stat);
        stat_reset(&worker->exclude_stat);
        stat_reset(&worker->__exclude_stat);
    }
#endif
    return ret;
}

static void red_get_area(RedWorker *worker, int surface_id, const SpiceRect *area, uint8_t *dest,
                         int dest_stride, int update)
{
    SpiceCanvas *canvas;
    RedSurface *surface;

    surface = &worker->surfaces[surface_id];
    if (update) {
        red_update_area(worker, area, surface_id);
    }

    canvas = surface->context.canvas;
    canvas->ops->read_bits(canvas, dest, dest_stride, area);
}

static int surface_format_to_image_type(uint32_t surface_format)
{
    switch (surface_format) {
    case SPICE_SURFACE_FMT_16_555:
        return SPICE_BITMAP_FMT_16BIT;
    case SPICE_SURFACE_FMT_32_xRGB:
        return SPICE_BITMAP_FMT_32BIT;
    case SPICE_SURFACE_FMT_32_ARGB:
        return SPICE_BITMAP_FMT_RGBA;
    default:
        PANIC("Unsupported surface format");
    }
    return 0;
}

static int rgb32_data_has_alpha(int width, int height, size_t stride,
                                uint8_t *data, int *all_set_out)
{
    uint32_t *line, *end, alpha;
    int has_alpha;

    has_alpha = FALSE;
    while (height-- > 0) {
        line = (uint32_t *)data;
        end = line + width;
        data += stride;
        while (line != end) {
            alpha = *line & 0xff000000U;
            if (alpha != 0) {
                has_alpha = TRUE;
                if (alpha != 0xff000000U) {
                    *all_set_out = FALSE;
                    return TRUE;
                }
            }
            line++;
        }
    }

    *all_set_out = has_alpha;
    return has_alpha;
}

static inline int red_handle_self_bitmap(RedWorker *worker, Drawable *drawable)
{
    SpiceImage *image;
    int32_t width;
    int32_t height;
    uint8_t *dest;
    int dest_stride;
    RedSurface *surface;
    int bpp;
    int all_set;

    if (!drawable->red_drawable->self_bitmap) {
        return TRUE;
    }


    surface = &worker->surfaces[drawable->surface_id];

    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;

    width = drawable->red_drawable->self_bitmap_area.right
            - drawable->red_drawable->self_bitmap_area.left;
    height = drawable->red_drawable->self_bitmap_area.bottom
            - drawable->red_drawable->self_bitmap_area.top;
    dest_stride = SPICE_ALIGN(width * bpp, 4);

    image = spice_new0(SpiceImage, 1);
    image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    image->descriptor.flags = 0;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    image->u.bitmap.flags = surface->context.top_down ? SPICE_BITMAP_FLAGS_TOP_DOWN : 0;
    image->u.bitmap.format = surface_format_to_image_type(surface->context.format);
    image->u.bitmap.stride = dest_stride;
    image->descriptor.width = image->u.bitmap.x = width;
    image->descriptor.height = image->u.bitmap.y = height;
    image->u.bitmap.palette = NULL;

    dest = (uint8_t *)spice_malloc_n(height, dest_stride);
    image->u.bitmap.data = spice_chunks_new_linear(dest, height * dest_stride);
    image->u.bitmap.data->flags |= SPICE_CHUNKS_FLAGS_FREE;

    red_get_area(worker, drawable->surface_id,
                 &drawable->red_drawable->self_bitmap_area, dest, dest_stride, TRUE);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(worker, drawable->surface_id) &&
        image->u.bitmap.format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(width, height, dest_stride, dest, &all_set)) {
        if (all_set) {
            image->descriptor.flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            image->u.bitmap.format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    drawable->self_bitmap = image;
    return TRUE;
}

static void free_one_drawable(RedWorker *worker, int force_glz_free)
{
    RingItem *ring_item = ring_get_tail(&worker->current_list);
    Drawable *drawable;
    Container *container;

    ASSERT(ring_item);
    drawable = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
    if (force_glz_free) {
        RingItem *glz_item, *next_item;
        RedGlzDrawable *glz;
        DRAWABLE_FOREACH_GLZ_SAFE(drawable, glz_item, next_item, glz) {
            red_display_free_glz_drawable(glz->dcc, glz);
        }
    }
    red_draw_drawable(worker, drawable);
    container = drawable->tree_item.base.container;

    current_remove_drawable(worker, drawable);
    container_cleanup(worker, container);
}

static Drawable *get_drawable(RedWorker *worker, uint8_t effect, RedDrawable *red_drawable,
                              uint32_t group_id) {
    Drawable *drawable;
    struct timespec time;
    int x;

    while (!(drawable = alloc_drawable(worker))) {
        free_one_drawable(worker, FALSE);
    }
    worker->drawable_count++;
    worker->red_drawable_count++;
    memset(drawable, 0, sizeof(Drawable));
    drawable->refs = 1;
    clock_gettime(CLOCK_MONOTONIC, &time);
    drawable->creation_time = timespec_to_red_time(&time);
#ifdef PIPE_DEBUG
    drawable->tree_item.base.id = ++worker->last_id;
#endif
    ring_item_init(&drawable->list_link);
    ring_item_init(&drawable->surface_list_link);
#ifdef UPDATE_AREA_BY_TREE
    ring_item_init(&drawable->collect_link);
#endif
    ring_item_init(&drawable->tree_item.base.siblings_link);
    drawable->tree_item.base.type = TREE_ITEM_TYPE_DRAWABLE;
    region_init(&drawable->tree_item.base.rgn);
    drawable->tree_item.effect = effect;
    drawable->red_drawable = ref_red_drawable(red_drawable);
    drawable->group_id = group_id;

    drawable->surface_id = red_drawable->surface_id;
    validate_surface(worker, drawable->surface_id);
    for (x = 0; x < 3; ++x) {
        drawable->surfaces_dest[x] = red_drawable->surfaces_dest[x];
        if (drawable->surfaces_dest[x] != -1) {
            validate_surface(worker, drawable->surfaces_dest[x]);
        }
    }
    ring_init(&drawable->pipes);
    ring_init(&drawable->glz_ring);

    return drawable;
}

static inline int red_handle_depends_on_target_surface(RedWorker *worker, uint32_t surface_id)
{
    RedSurface *surface;
    RingItem *ring_item;

    surface = &worker->surfaces[surface_id];

    while ((ring_item = ring_get_tail(&surface->depend_on_me))) {
        Drawable *drawable;
        DependItem *depended_item = SPICE_CONTAINEROF(ring_item, DependItem, ring_item);
        drawable = depended_item->drawable;
        surface_flush(worker, drawable->surface_id, &drawable->red_drawable->bbox);
    }

    return TRUE;
}

static inline void add_to_surface_dependency(RedWorker *worker, int depend_on_surface_id,
                                             DependItem *depend_item, Drawable *drawable)
{
    RedSurface *surface;

    if (depend_on_surface_id == -1) {
        depend_item->drawable = NULL;
        return;
    }

    surface = &worker->surfaces[depend_on_surface_id];

    depend_item->drawable = drawable;
    ring_add(&surface->depend_on_me, &depend_item->ring_item);
}

static inline int red_handle_surfaces_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        // surface self dependency is handled by shadows in "current", or by
        // handle_self_bitmap
        if (drawable->surfaces_dest[x] != drawable->surface_id) {
            add_to_surface_dependency(worker, drawable->surfaces_dest[x],
                                      &drawable->depend_items[x], drawable);

            if (drawable->surfaces_dest[x] == 0) {
                QRegion depend_region;
                region_init(&depend_region);
                region_add(&depend_region, &drawable->red_drawable->surfaces_rects[x]);
                red_detach_streams_behind(worker, &depend_region);
            }
        }
    }

    return TRUE;
}

static inline void red_inc_surfaces_drawable_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;
    RedSurface *surface;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id == -1) {
            continue;
        }
        surface = &worker->surfaces[surface_id];
        surface->refs++;
    }
}

static inline void red_process_drawable(RedWorker *worker, RedDrawable *drawable,
                                        uint32_t group_id)
{
    int surface_id;
    Drawable *item = get_drawable(worker, drawable->effect, drawable, group_id);

    ASSERT(item);

    surface_id = item->surface_id;

    worker->surfaces[surface_id].refs++;

    region_add(&item->tree_item.base.rgn, &drawable->bbox);
#ifdef PIPE_DEBUG
    printf("TEST: DRAWABLE: id %u type %s effect %u bbox %u %u %u %u\n",
           item->tree_item.base.id,
           draw_type_to_str(drawable->type),
           item->tree_item.effect,
           drawable->bbox.top, drawable->bbox.left, drawable->bbox.bottom, drawable->bbox.right);
#endif

    if (drawable->clip.type == SPICE_CLIP_TYPE_RECTS) {
        QRegion rgn;

        region_init(&rgn);
        add_clip_rects(&rgn, drawable->clip.rects);
        region_and(&item->tree_item.base.rgn, &rgn);
        region_destroy(&rgn);
    }
    /*
        surface->refs is affected by a drawable (that is
        dependent on the surface) as long as the drawable is alive.
        However, surface->depend_on_me is affected by a drawable only
        as long as it is in the current tree (hasn't been rendered yet).
    */
    red_inc_surfaces_drawable_dependencies(worker, item);

    if (region_is_empty(&item->tree_item.base.rgn)) {
        release_drawable(worker, item);
        return;
    }

    if (!red_handle_self_bitmap(worker, item)) {
        release_drawable(worker, item);
        return;
    }

    if (!red_handle_depends_on_target_surface(worker, surface_id)) {
        release_drawable(worker, item);
        return;
    }

    if (!red_handle_surfaces_dependencies(worker, item)) {
        release_drawable(worker, item);
        return;
    }

    if (red_current_add_qxl(worker, &worker->surfaces[surface_id].current, item,
                            drawable)) {
        if (item->tree_item.effect != QXL_EFFECT_OPAQUE) {
            worker->transparent_count++;
        }
        red_pipes_add_drawable(worker, item);
#ifdef DRAW_ALL
        red_draw_qxl_drawable(worker, item);
#endif
    }
    release_drawable(worker, item);
}

static inline void red_create_surface(RedWorker *worker, uint32_t surface_id,uint32_t width,
                                      uint32_t height, int32_t stride, uint32_t format,
                                      void *line_0, int data_is_valid, int send_client);

static inline void red_process_surface(RedWorker *worker, RedSurfaceCmd *surface,
                                       uint32_t group_id, int loadvm)
{
    int surface_id;
    RedSurface *red_surface;
    uint8_t *data;

    surface_id = surface->surface_id;
    __validate_surface(worker, surface_id);

    red_surface = &worker->surfaces[surface_id];

    switch (surface->type) {
    case QXL_SURFACE_CMD_CREATE: {
        uint32_t height = surface->u.surface_create.height;
        int32_t stride = surface->u.surface_create.stride;
        int reloaded_surface = loadvm || (surface->flags & QXL_SURF_FLAG_KEEP_DATA);

        data = surface->u.surface_create.data;
        if (stride < 0) {
            data -= (int32_t)(stride * (height - 1));
        }
        red_create_surface(worker, surface_id, surface->u.surface_create.width,
                           height, stride, surface->u.surface_create.format, data,
                           reloaded_surface,
                           // reloaded surfaces will be sent on demand
                           !reloaded_surface);
        set_surface_release_info(worker, surface_id, 1, surface->release_info, group_id);
        break;
    }
    case QXL_SURFACE_CMD_DESTROY:
        PANIC_ON(!red_surface->context.canvas);
        set_surface_release_info(worker, surface_id, 0, surface->release_info, group_id);
        red_handle_depends_on_target_surface(worker, surface_id);
        /* note that red_handle_depends_on_target_surface must be called before red_current_clear.
           otherwise "current" will hold items that other drawables may depend on, and then
           red_current_clear will remove them from the pipe. */
        red_current_clear(worker, surface_id);
        red_clear_surface_drawables_from_pipes(worker, surface_id, FALSE, FALSE);
        red_destroy_surface(worker, surface_id);
        break;
    default:
            red_error("unknown surface command");
    };
    red_put_surface_cmd(surface);
    free(surface);
}

static SpiceCanvas *image_surfaces_get(SpiceImageSurfaces *surfaces,
                                       uint32_t surface_id)
{
    RedWorker *worker;

    worker = SPICE_CONTAINEROF(surfaces, RedWorker, image_surfaces);
    validate_surface(worker, surface_id);

    return worker->surfaces[surface_id].context.canvas;
}

static void image_surface_init(RedWorker *worker)
{
    static SpiceImageSurfacesOps image_surfaces_ops = {
        image_surfaces_get,
    };

    worker->image_surfaces.ops = &image_surfaces_ops;
}

static ImageCacheItem *image_cache_find(ImageCache *cache, uint64_t id)
{
    ImageCacheItem *item = cache->hash_table[id % IMAGE_CACHE_HASH_SIZE];

    while (item) {
        if (item->id == id) {
            return item;
        }
        item = item->next;
    }
    return NULL;
}

static int image_cache_hit(ImageCache *cache, uint64_t id)
{
    ImageCacheItem *item;
    if (!(item = image_cache_find(cache, id))) {
        return FALSE;
    }
#ifdef IMAGE_CACHE_AGE
    item->age = cache->age;
#endif
    ring_remove(&item->lru_link);
    ring_add(&cache->lru, &item->lru_link);
    return TRUE;
}

static void image_cache_remove(ImageCache *cache, ImageCacheItem *item)
{
    ImageCacheItem **now;

    now = &cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE];
    for (;;) {
        ASSERT(*now);
        if (*now == item) {
            *now = item->next;
            break;
        }
        now = &(*now)->next;
    }
    ring_remove(&item->lru_link);
    pixman_image_unref(item->image);
    free(item);
#ifndef IMAGE_CACHE_AGE
    cache->num_items--;
#endif
}

#define IMAGE_CACHE_MAX_ITEMS 2

static void image_cache_put(SpiceImageCache *spice_cache, uint64_t id, pixman_image_t *image)
{
    ImageCache *cache = (ImageCache *)spice_cache;
    ImageCacheItem *item;

#ifndef IMAGE_CACHE_AGE
    if (cache->num_items == IMAGE_CACHE_MAX_ITEMS) {
        ImageCacheItem *tail = (ImageCacheItem *)ring_get_tail(&cache->lru);
        ASSERT(tail);
        image_cache_remove(cache, tail);
    }
#endif

    item = spice_new(ImageCacheItem, 1);
    item->id = id;
#ifdef IMAGE_CACHE_AGE
    item->age = cache->age;
#else
    cache->num_items++;
#endif
    item->image = pixman_image_ref(image);
    ring_item_init(&item->lru_link);

    item->next = cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE];
    cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE] = item;

    ring_add(&cache->lru, &item->lru_link);
}

static pixman_image_t *image_cache_get(SpiceImageCache *spice_cache, uint64_t id)
{
    ImageCache *cache = (ImageCache *)spice_cache;

    ImageCacheItem *item = image_cache_find(cache, id);
    if (!item) {
        red_error("not found");
    }
    return pixman_image_ref(item->image);
}

static void image_cache_init(ImageCache *cache)
{
    static SpiceImageCacheOps image_cache_ops = {
        image_cache_put,
        image_cache_get,
    };

    cache->base.ops = &image_cache_ops;
    memset(cache->hash_table, 0, sizeof(cache->hash_table));
    ring_init(&cache->lru);
#ifdef IMAGE_CACHE_AGE
    cache->age = 0;
#else
    cache->num_items = 0;
#endif
}

static void image_cache_reset(ImageCache *cache)
{
    ImageCacheItem *item;

    while ((item = (ImageCacheItem *)ring_get_head(&cache->lru))) {
        image_cache_remove(cache, item);
    }
#ifdef IMAGE_CACHE_AGE
    cache->age = 0;
#endif
}

#define IMAGE_CACHE_DEPTH 4

static void image_cache_eaging(ImageCache *cache)
{
#ifdef IMAGE_CACHE_AGE
    ImageCacheItem *item;

    cache->age++;
    while ((item = (ImageCacheItem *)ring_get_tail(&cache->lru)) &&
           cache->age - item->age > IMAGE_CACHE_DEPTH) {
        image_cache_remove(cache, item);
    }
#endif
}

static void localize_bitmap(RedWorker *worker, SpiceImage **image_ptr, SpiceImage *image_store,
                            Drawable *drawable)
{
    SpiceImage *image = *image_ptr;

    if (image == NULL) {
        ASSERT(drawable != NULL);
        ASSERT(drawable->self_bitmap != NULL);
        *image_ptr = drawable->self_bitmap;
        return;
    }

    if (image_cache_hit(&worker->image_cache, image->descriptor.id)) {
        image_store->descriptor = image->descriptor;
        image_store->descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE;
        image_store->descriptor.flags = 0;
        *image_ptr = image_store;
        return;
    }

    switch (image->descriptor.type) {
    case SPICE_IMAGE_TYPE_QUIC: {
        image_store->descriptor = image->descriptor;
        image_store->u.quic = image->u.quic;
        *image_ptr = image_store;
#ifdef IMAGE_CACHE_AGE
        image_store->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
#else
        if (image_store->descriptor.width * image->descriptor.height >= 640 * 480) {
            image_store->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
        }
#endif
        break;
    }
    case SPICE_IMAGE_TYPE_BITMAP:
    case SPICE_IMAGE_TYPE_SURFACE:
        /* nothing */
        break;
    default:
        red_error("invalid image type");
    }
}

static void localize_brush(RedWorker *worker, SpiceBrush *brush, SpiceImage *image_store)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        localize_bitmap(worker, &brush->u.pattern.pat, image_store, NULL);
    }
}

static void localize_mask(RedWorker *worker, SpiceQMask *mask, SpiceImage *image_store)
{
    if (mask->bitmap) {
        localize_bitmap(worker, &mask->bitmap, image_store, NULL);
    }
}

static void red_draw_qxl_drawable(RedWorker *worker, Drawable *drawable)
{
    RedSurface *surface;
    SpiceCanvas *canvas;
    SpiceClip clip = drawable->red_drawable->clip;

    surface = &worker->surfaces[drawable->surface_id];
    canvas = surface->context.canvas;

    image_cache_eaging(&worker->image_cache);

    worker->preload_group_id = drawable->group_id;

    region_add(&surface->draw_dirty_region, &drawable->red_drawable->bbox);

    switch (drawable->red_drawable->type) {
    case QXL_DRAW_FILL: {
        SpiceFill fill = drawable->red_drawable->u.fill;
        SpiceImage img1, img2;
        localize_brush(worker, &fill.brush, &img1);
        localize_mask(worker, &fill.mask, &img2);
        canvas->ops->draw_fill(canvas, &drawable->red_drawable->bbox,
                               &clip, &fill);
        break;
    }
    case QXL_DRAW_OPAQUE: {
        SpiceOpaque opaque = drawable->red_drawable->u.opaque;
        SpiceImage img1, img2, img3;
        localize_brush(worker, &opaque.brush, &img1);
        localize_bitmap(worker, &opaque.src_bitmap, &img2, drawable);
        localize_mask(worker, &opaque.mask, &img3);
        canvas->ops->draw_opaque(canvas, &drawable->red_drawable->bbox, &clip, &opaque);
        break;
    }
    case QXL_DRAW_COPY: {
        SpiceCopy copy = drawable->red_drawable->u.copy;
        SpiceImage img1, img2;
        localize_bitmap(worker, &copy.src_bitmap, &img1, drawable);
        localize_mask(worker, &copy.mask, &img2);
        canvas->ops->draw_copy(canvas, &drawable->red_drawable->bbox,
                               &clip, &copy);
        break;
    }
    case QXL_DRAW_TRANSPARENT: {
        SpiceTransparent transparent = drawable->red_drawable->u.transparent;
        SpiceImage img1;
        localize_bitmap(worker, &transparent.src_bitmap, &img1, drawable);
        canvas->ops->draw_transparent(canvas,
                                      &drawable->red_drawable->bbox, &clip, &transparent);
        break;
    }
    case QXL_DRAW_ALPHA_BLEND: {
        SpiceAlphaBlend alpha_blend = drawable->red_drawable->u.alpha_blend;
        SpiceImage img1;
        localize_bitmap(worker, &alpha_blend.src_bitmap, &img1, drawable);
        canvas->ops->draw_alpha_blend(canvas,
                                      &drawable->red_drawable->bbox, &clip, &alpha_blend);
        break;
    }
    case QXL_COPY_BITS: {
        canvas->ops->copy_bits(canvas, &drawable->red_drawable->bbox,
                               &clip, &drawable->red_drawable->u.copy_bits.src_pos);
        break;
    }
    case QXL_DRAW_BLEND: {
        SpiceBlend blend = drawable->red_drawable->u.blend;
        SpiceImage img1, img2;
        localize_bitmap(worker, &blend.src_bitmap, &img1, drawable);
        localize_mask(worker, &blend.mask, &img2);
        canvas->ops->draw_blend(canvas, &drawable->red_drawable->bbox,
                                &clip, &blend);
        break;
    }
    case QXL_DRAW_BLACKNESS: {
        SpiceBlackness blackness = drawable->red_drawable->u.blackness;
        SpiceImage img1;
        localize_mask(worker, &blackness.mask, &img1);
        canvas->ops->draw_blackness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &blackness);
        break;
    }
    case QXL_DRAW_WHITENESS: {
        SpiceWhiteness whiteness = drawable->red_drawable->u.whiteness;
        SpiceImage img1;
        localize_mask(worker, &whiteness.mask, &img1);
        canvas->ops->draw_whiteness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &whiteness);
        break;
    }
    case QXL_DRAW_INVERS: {
        SpiceInvers invers = drawable->red_drawable->u.invers;
        SpiceImage img1;
        localize_mask(worker, &invers.mask, &img1);
        canvas->ops->draw_invers(canvas,
                                 &drawable->red_drawable->bbox, &clip, &invers);
        break;
    }
    case QXL_DRAW_ROP3: {
        SpiceRop3 rop3 = drawable->red_drawable->u.rop3;
        SpiceImage img1, img2, img3;
        localize_brush(worker, &rop3.brush, &img1);
        localize_bitmap(worker, &rop3.src_bitmap, &img2, drawable);
        localize_mask(worker, &rop3.mask, &img3);
        canvas->ops->draw_rop3(canvas, &drawable->red_drawable->bbox,
                               &clip, &rop3);
        break;
    }
    case QXL_DRAW_STROKE: {
        SpiceStroke stroke = drawable->red_drawable->u.stroke;
        SpiceImage img1;
        localize_brush(worker, &stroke.brush, &img1);
        canvas->ops->draw_stroke(canvas,
                                 &drawable->red_drawable->bbox, &clip, &stroke);
        break;
    }
    case QXL_DRAW_TEXT: {
        SpiceText text = drawable->red_drawable->u.text;
        SpiceImage img1, img2;
        localize_brush(worker, &text.fore_brush, &img1);
        localize_brush(worker, &text.back_brush, &img2);
        canvas->ops->draw_text(canvas, &drawable->red_drawable->bbox,
                               &clip, &text);
        break;
    }
    default:
        red_printf("invalid type");
    }
}

#ifndef DRAW_ALL

static void red_draw_drawable(RedWorker *worker, Drawable *drawable)
{
#ifdef UPDATE_AREA_BY_TREE
    SpiceCanvas *canvas;

    canvas = surface->context.canvas;
    //todo: add need top mask flag
    canvas->ops->group_start(canvas,
                             &drawable->tree_item.base.rgn);
#endif
    red_flush_source_surfaces(worker, drawable);
    red_draw_qxl_drawable(worker, drawable);
#ifdef UPDATE_AREA_BY_TREE
    canvas->ops->group_end(canvas);
#endif
}

static void validate_area(RedWorker *worker, const SpiceRect *area, uint32_t surface_id)
{
    RedSurface *surface;

    surface = &worker->surfaces[surface_id];
    if (!surface->context.canvas_draws_on_surface) {
        SpiceCanvas *canvas = surface->context.canvas;
        int h;
        int stride = surface->context.stride;
        uint8_t *line_0 = surface->context.line_0;

        if (!(h = area->bottom - area->top)) {
            return;
        }

        ASSERT(stride < 0);
        uint8_t *dest = line_0 + (area->top * stride) + area->left * sizeof(uint32_t);
        dest += (h - 1) * stride;
        canvas->ops->read_bits(canvas, dest, -stride, area);
    }
}

#ifdef UPDATE_AREA_BY_TREE

static inline void __red_collect_for_update(RedWorker *worker, Ring *ring, RingItem *ring_item,
                                            QRegion *rgn, Ring *items)
{
    Ring *top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;
        if (region_intersects(rgn, &now->rgn)) {
            if (IS_DRAW_ITEM(now)) {
                Drawable *drawable = SPICE_CONTAINEROF(now, Drawable, tree_item);

                ring_add(items, &drawable->collect_link);
                region_or(rgn, &now->rgn);
                if (drawable->tree_item.shadow) {
                    region_or(rgn, &drawable->tree_item.shadow->base.rgn);
                }
            } else if (now->type == TREE_ITEM_TYPE_SHADOW) {
                Drawable *owner = SPICE_CONTAINEROF(((Shadow *)now)->owner, Drawable, tree_item);
                if (!ring_item_is_linked(&owner->collect_link)) {
                    region_or(rgn, &now->rgn);
                    region_or(rgn, &owner->tree_item.base.rgn);
                    ring_add(items, &owner->collect_link);
                }
            } else if (now->type == TREE_ITEM_TYPE_CONTAINER) {
                Container *container = (Container *)now;

                if ((ring_item = ring_get_head(&container->items))) {
                    ring = &container->items;
                    ASSERT(((TreeItem *)ring_item)->container);
                    continue;
                }
                ring_item = &now->siblings_link;
            }
        }

        while (!(ring_item = ring_next(ring, ring_item))) {
            if (ring == top_ring) {
                return;
            }
            ring_item = &container->base.siblings_link;
            container = container->base.container;
            ring = (container) ? &container->items : top_ring;
        }
    }
}

static void red_update_area(RedWorker *worker, const SpiceRect *area, int surface_id)
{
    RedSurface *surface;
    Ring *ring;
    RingItem *ring_item;
    Ring items;
    QRegion rgn;

    surface = &worker->surfaces[surface_id];
    ring = &surface->current;

    if (!(ring_item = ring_get_head(ring))) {
        worker->draw_context.validate_area(surface->context.canvas,
                                           &worker->dev_info.surface0_area, area);
        return;
    }

    region_init(&rgn);
    region_add(&rgn, area);
    ring_init(&items);
    __red_collect_for_update(worker, ring, ring_item, &rgn, &items);
    region_destroy(&rgn);

    while ((ring_item = ring_get_head(&items))) {
        Drawable *drawable = SPICE_CONTAINEROF(ring_item, Drawable, collect_link);
        Container *container;

        ring_remove(ring_item);
        red_draw_drawable(worker, drawable);
        container = drawable->tree_item.base.container;
        current_remove_drawable(worker, drawable);
        container_cleanup(worker, container);
    }
    validate_area(worker, area, surface_id);
}

#else

/*
    Renders drawables for updating the requested area, but only drawables that are older
    than 'last' (exclusive).
*/
static void red_update_area_till(RedWorker *worker, const SpiceRect *area, int surface_id,
                                 Drawable *last)
{
    // TODO: if we use UPDATE_AREA_BY_TREE, a corresponding red_update_area_till
    // should be implemented

    RedSurface *surface;
    Drawable *surface_last = NULL;
    Ring *ring;
    RingItem *ring_item;
    Drawable *now;
    QRegion rgn;

    ASSERT(last);
    ASSERT(ring_item_is_linked(&last->list_link));

    surface = &worker->surfaces[surface_id];

    if (surface_id != last->surface_id) {
        // find the nearest older drawable from the appropriate surface
        ring = &worker->current_list;
        ring_item = &last->list_link;
        while ((ring_item = ring_next(ring, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
            if (now->surface_id == surface_id) {
                surface_last = now;
                break;
            }
        }
    } else {
        ring_item = ring_next(&surface->current_list, &last->surface_list_link);
        if (ring_item) {
            surface_last = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        }
    }

    if (!surface_last) {
        return;
    }

    ring = &surface->current_list;
    ring_item = &surface_last->surface_list_link;

    region_init(&rgn);
    region_add(&rgn, area);

    // find the first older drawable that intersects with the area
    do {
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            surface_last = now;
            break;
        }
    } while ((ring_item = ring_next(ring, ring_item)));

    region_destroy(&rgn);

    if (!surface_last) {
        return;
    }

    do {
        Container *container;

        ring_item = ring_get_tail(&surface->current_list);
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        now->refs++;
        container = now->tree_item.base.container;
        current_remove_drawable(worker, now);
        container_cleanup(worker, container);
        /* red_draw_drawable may call red_update_area for the surfaces 'now' depends on. Notice,
           that it is valid to call red_update_area in this case and not red_update_area_till:
           It is impossible that there was newer item then 'last' in one of the surfaces
           that red_update_area is called for, Otherwise, 'now' would have already been rendered.
           See the call for red_handle_depends_on_target_surface in red_process_drawable */
        red_draw_drawable(worker, now);
        release_drawable(worker, now);
    } while (now != surface_last);
    validate_area(worker, area, surface_id);
}

static void red_update_area(RedWorker *worker, const SpiceRect *area, int surface_id)
{
    RedSurface *surface;
    Ring *ring;
    RingItem *ring_item;
    QRegion rgn;
    Drawable *last;
    Drawable *now;
#ifdef ACYCLIC_SURFACE_DEBUG
    int gn;
#endif

    surface = &worker->surfaces[surface_id];

    last = NULL;
#ifdef ACYCLIC_SURFACE_DEBUG
    gn = ++surface->current_gn;
#endif
    ring = &surface->current_list;
    ring_item = ring;

    region_init(&rgn);
    region_add(&rgn, area);
    while ((ring_item = ring_next(ring, ring_item))) {
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            last = now;
            break;
        }
    }
    region_destroy(&rgn);

    if (!last) {
        validate_area(worker, area, surface_id);
        return;
    }

    do {
        Container *container;

        ring_item = ring_get_tail(&surface->current_list);
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        now->refs++;
        container = now->tree_item.base.container;
        current_remove_drawable(worker, now);
        container_cleanup(worker, container);
        red_draw_drawable(worker, now);
        release_drawable(worker, now);
#ifdef ACYCLIC_SURFACE_DEBUG
        if (gn != surface->current_gn) {
            red_error("cyclic surface dependencies");
        }
#endif
    } while (now != last);
    validate_area(worker, area, surface_id);
}

#endif
#endif

static inline void free_cursor_item(RedWorker *worker, CursorItem *item);

static void red_release_cursor(RedWorker *worker, CursorItem *cursor)
{
    if (!--cursor->refs) {
        QXLReleaseInfoExt release_info_ext;
        RedCursorCmd *cursor_cmd;

        cursor_cmd = cursor->red_cursor;
        release_info_ext.group_id = cursor->group_id;
        release_info_ext.info = cursor_cmd->release_info;
        worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
        free_cursor_item(worker, cursor);
        red_put_cursor_cmd(cursor_cmd);
        free(cursor_cmd);
    }
}

static void red_set_cursor(RedWorker *worker, CursorItem *cursor)
{
    if (worker->cursor) {
        red_release_cursor(worker, worker->cursor);
    }
    ++cursor->refs;
    worker->cursor = cursor;
}

#ifdef DEBUG_CURSORS
static int _cursor_count = 0;
#endif

static inline CursorItem *alloc_cursor_item(RedWorker *worker)
{
    CursorItem *cursor;

    if (!worker->free_cursor_items) {
        return NULL;
    }
#ifdef DEBUG_CURSORS
    --_cursor_count;
#endif
    cursor = &worker->free_cursor_items->u.cursor_item;
    worker->free_cursor_items = worker->free_cursor_items->u.next;
    return cursor;
}

static inline void free_cursor_item(RedWorker *worker, CursorItem *item)
{
    ((_CursorItem *)item)->u.next = worker->free_cursor_items;
    worker->free_cursor_items = (_CursorItem *)item;
#ifdef DEBUG_CURSORS
    ++_cursor_count;
    ASSERT(_cursor_count <= NUM_CURSORS);
#endif
}

static void cursor_items_init(RedWorker *worker)
{
    int i;

    worker->free_cursor_items = NULL;
    for (i = 0; i < NUM_CURSORS; i++) {
        free_cursor_item(worker, &worker->cursor_items[i].u.cursor_item);
    }
}

static CursorItem *get_cursor_item(RedWorker *worker, RedCursorCmd *cmd, uint32_t group_id)
{
    CursorItem *cursor_item;

    PANIC_ON(!(cursor_item = alloc_cursor_item(worker)));

    cursor_item->refs = 1;
    cursor_item->group_id = group_id;
    cursor_item->red_cursor = cmd;

    return cursor_item;
}

static CursorPipeItem *ref_cursor_pipe_item(CursorPipeItem *item)
{
    ASSERT(item);
    item->refs++;
    return item;
}

static PipeItem *new_cursor_pipe_item(RedChannelClient *rcc, void *data, int num)
{
    CursorPipeItem *item = spice_malloc0(sizeof(CursorPipeItem));

    red_channel_pipe_item_init(rcc->channel, &item->base, PIPE_ITEM_TYPE_CURSOR);
    item->refs = 1;
    item->cursor_item = data;
    item->cursor_item->refs++;
    return &item->base;
}

static void put_cursor_pipe_item(CursorChannelClient *ccc, CursorPipeItem *pipe_item)
{
    ASSERT(pipe_item);

    if (--pipe_item->refs) {
        return;
    }

    ASSERT(!pipe_item_is_linked(&pipe_item->base));

    red_release_cursor(ccc->common.worker, pipe_item->cursor_item);
    free(pipe_item);
}

static void qxl_process_cursor(RedWorker *worker, RedCursorCmd *cursor_cmd, uint32_t group_id)
{
    CursorItem *cursor_item;
    int cursor_show = FALSE;

    cursor_item = get_cursor_item(worker, cursor_cmd, group_id);

    switch (cursor_cmd->type) {
    case QXL_CURSOR_SET:
        worker->cursor_visible = cursor_cmd->u.set.visible;
        red_set_cursor(worker, cursor_item);
        break;
    case QXL_CURSOR_MOVE:
        cursor_show = !worker->cursor_visible;
        worker->cursor_visible = TRUE;
        worker->cursor_position = cursor_cmd->u.position;
        break;
    case QXL_CURSOR_HIDE:
        worker->cursor_visible = FALSE;
        break;
    case QXL_CURSOR_TRAIL:
        worker->cursor_trail_length = cursor_cmd->u.trail.length;
        worker->cursor_trail_frequency = cursor_cmd->u.trail.frequency;
        break;
    default:
        red_error("invalid cursor command %u", cursor_cmd->type);
    }

    if (cursor_is_connected(worker) && (worker->mouse_mode == SPICE_MOUSE_MODE_SERVER ||
                                   cursor_cmd->type != QXL_CURSOR_MOVE || cursor_show)) {
        red_channel_pipes_new_add(&worker->cursor_channel->common.base, new_cursor_pipe_item,
                                  (void*)cursor_item);
    }
    red_release_cursor(worker, cursor_item);
}

static inline uint64_t red_now(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);

    return time.tv_sec * 1000000000 + time.tv_nsec;
}

static int red_process_cursor(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }

    *ring_is_empty = FALSE;
    while (!cursor_is_connected(worker) ||
           red_channel_min_pipe_size(&worker->cursor_channel->common.base) <= max_pipe_size) {
        if (!worker->qxl->st->qif->get_cursor_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;
            if (worker->repoll_cursor_ring < CMD_RING_POLL_RETRIES) {
                worker->repoll_cursor_ring++;
                worker->epoll_timeout = MIN(worker->epoll_timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->repoll_cursor_ring > CMD_RING_POLL_RETRIES ||
                worker->qxl->st->qif->req_cursor_notification(worker->qxl)) {
                worker->repoll_cursor_ring++;
                break;
            }
            continue;
        }
        worker->repoll_cursor_ring = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_CURSOR: {
            RedCursorCmd *cursor = spice_new0(RedCursorCmd, 1);

            red_get_cursor_cmd(&worker->mem_slots, ext_cmd.group_id,
                               cursor, ext_cmd.cmd.data);
            qxl_process_cursor(worker, cursor, ext_cmd.group_id);
            break;
        }
        default:
            red_error("bad command type");
        }
        n++;
    }
    return n;
}

static RedDrawable *red_drawable_new(void)
{
    RedDrawable * red = spice_new0(RedDrawable, 1);

    red->refs = 1;
    return red;
}

static int red_process_commands(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;
    uint64_t start = red_now();

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }

    *ring_is_empty = FALSE;
    while (!display_is_connected(worker) ||
           // TODO: change to average pipe size?
           red_channel_min_pipe_size(&worker->display_channel->common.base) <= max_pipe_size) {
        if (!worker->qxl->st->qif->get_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;;
            if (worker->repoll_cmd_ring < CMD_RING_POLL_RETRIES) {
                worker->repoll_cmd_ring++;
                worker->epoll_timeout = MIN(worker->epoll_timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->repoll_cmd_ring > CMD_RING_POLL_RETRIES ||
                         worker->qxl->st->qif->req_cmd_notification(worker->qxl)) {
                worker->repoll_cmd_ring++;
                break;
            }
            continue;
        }
        stat_inc_counter(worker->command_counter, 1);
        worker->repoll_cmd_ring = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_DRAW: {
            RedDrawable *drawable = red_drawable_new(); // returns with 1 ref

            red_get_drawable(&worker->mem_slots, ext_cmd.group_id,
                             drawable, ext_cmd.cmd.data, ext_cmd.flags);
            red_process_drawable(worker, drawable, ext_cmd.group_id);
            // release the red_drawable
            put_red_drawable(worker, drawable, ext_cmd.group_id, NULL);
            break;
        }
        case QXL_CMD_UPDATE: {
            RedUpdateCmd update;
            QXLReleaseInfoExt release_info_ext;

            red_get_update_cmd(&worker->mem_slots, ext_cmd.group_id,
                               &update, ext_cmd.cmd.data);
            validate_surface(worker, update.surface_id);
            red_update_area(worker, &update.area, update.surface_id);
            worker->qxl->st->qif->notify_update(worker->qxl, update.update_id);
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = update.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_update_cmd(&update);
            break;
        }
        case QXL_CMD_MESSAGE: {
            RedMessage message;
            QXLReleaseInfoExt release_info_ext;

            red_get_message(&worker->mem_slots, ext_cmd.group_id,
                            &message, ext_cmd.cmd.data);
#ifdef DEBUG
            /* alert: accessing message.data is insecure */
            red_printf("MESSAGE: %s", message.data);
#endif
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = message.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_message(&message);
            break;
        }
        case QXL_CMD_SURFACE: {
            RedSurfaceCmd *surface = spice_new0(RedSurfaceCmd, 1);

            red_get_surface_cmd(&worker->mem_slots, ext_cmd.group_id,
                                surface, ext_cmd.cmd.data);
            red_process_surface(worker, surface, ext_cmd.group_id, FALSE);
            break;
        }
        default:
            red_error("bad command type");
        }
        n++;
        if ((worker->display_channel &&
             red_channel_all_blocked(&worker->display_channel->common.base))
            || red_now() - start > 10 * 1000 * 1000) {
            worker->epoll_timeout = 0;
            return n;
        }
    }
    return n;
}

#define RED_RELEASE_BUNCH_SIZE 64

static void red_free_some(RedWorker *worker)
{
    int n = 0;
    DisplayChannelClient *dcc;
    RingItem *item;

    red_printf_debug(3, "WORKER",
                     "#draw=%d, #red_draw=%d, #glz_draw=%d", worker->drawable_count,
                     worker->red_drawable_count, worker->glz_drawable_count);
    WORKER_FOREACH_DCC(worker, item, dcc) {
        GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

        if (glz_dict) {
            // encoding using the dictionary is prevented since the following operations might
            // change the dictionary
            pthread_rwlock_wrlock(&glz_dict->encode_lock);
            n = red_display_free_some_independent_glz_drawables(dcc);
        }
    }

    while (!ring_is_empty(&worker->current_list) && n++ < RED_RELEASE_BUNCH_SIZE) {
        free_one_drawable(worker, TRUE);
    }

    WORKER_FOREACH_DCC(worker, item, dcc) {
        GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

        if (glz_dict) {
            pthread_rwlock_unlock(&glz_dict->encode_lock);
        }
    }
}

static void red_current_flush(RedWorker *worker, int surface_id)
{
    while (!ring_is_empty(&worker->surfaces[surface_id].current_list)) {
        free_one_drawable(worker, FALSE);
    }
    red_current_clear(worker, surface_id);
}

// adding the pipe item after pos. If pos == NULL, adding to head.
static ImageItem *red_add_surface_area_image(DisplayChannelClient *dcc, int surface_id,
                                             SpiceRect *area, PipeItem *pos, int can_lossy)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    RedChannel *channel = &display_channel->common.base;
    RedSurface *surface = &worker->surfaces[surface_id];
    SpiceCanvas *canvas = surface->context.canvas;
    ImageItem *item;
    int stride;
    int width;
    int height;
    int bpp;
    int all_set;

    ASSERT(area);

    width = area->right - area->left;
    height = area->bottom - area->top;
    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;
    stride = SPICE_ALIGN(width * bpp, 4);

    item = (ImageItem *)spice_malloc_n_m(height, stride, sizeof(ImageItem));

    red_channel_pipe_item_init(channel, &item->link, PIPE_ITEM_TYPE_IMAGE);

    item->refs = 1;
    item->surface_id = surface_id;
    item->image_format =
        surface_format_to_image_type(surface->context.format);
    item->image_flags = 0;
    item->pos.x = area->left;
    item->pos.y = area->top;
    item->width = width;
    item->height = height;
    item->stride = stride;
    item->top_down = surface->context.top_down;
    item->can_lossy = can_lossy;

    canvas->ops->read_bits(canvas, item->data, stride, area);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(worker, surface_id) &&
        item->image_format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(item->width, item->height, item->stride, item->data, &all_set)) {
        if (all_set) {
            item->image_flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            item->image_format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    if (!pos) {
        red_pipe_add_image_item(dcc, item);
    } else {
        red_pipe_add_image_item_after(dcc, item, pos);
    }

    release_image_item(item);

    return item;
}

static void red_push_surface_image(DisplayChannelClient *dcc, int surface_id)
{
    SpiceRect area;
    RedSurface *surface;
    RedWorker *worker;

    if (!dcc) {
        return;
    }
    worker = DCC_TO_WORKER(dcc);
    surface = &worker->surfaces[surface_id];
    if (!surface->context.canvas) {
        return;
    }
    area.top = area.left = 0;
    area.right = surface->context.width;
    area.bottom = surface->context.height;

    /* not allowing lossy compression because probably, especially if it is a primary surface,
       it combines both "picture-like" areas with areas that are more "artificial"*/
    red_add_surface_area_image(dcc, surface_id, &area, NULL, FALSE);
    red_channel_client_push(&dcc->common.base);
}

typedef struct {
    uint32_t type;
    void *data;
    uint32_t size;
} AddBufInfo;

static void marshaller_add_compressed(SpiceMarshaller *m,
                                      RedCompressBuf *comp_buf, size_t size)
{
    size_t max = size;
    size_t now;
    do {
        ASSERT(comp_buf);
        now = MIN(sizeof(comp_buf->buf), max);
        max -= now;
        spice_marshaller_add_ref(m, (uint8_t*)comp_buf->buf, now);
        comp_buf = comp_buf->send_next;
    } while (max);
}


static void add_buf_from_info(SpiceMarshaller *m, AddBufInfo *info)
{
    if (info->data) {
        switch (info->type) {
        case BUF_TYPE_RAW:
            spice_marshaller_add_ref(m, info->data, info->size);
            break;
        }
    }
}


static inline void fill_rects_clip(SpiceMarshaller *m, SpiceClipRects *data)
{
    int i;

    spice_marshaller_add_uint32(m, data->num_rects);
    for (i = 0; i < data->num_rects; i++) {
        spice_marshall_Rect(m, data->rects + i);
    }
}

static void fill_base(SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    SpiceMsgDisplayBase base;

    base.surface_id = drawable->surface_id;
    base.box = drawable->red_drawable->bbox;
    base.clip = drawable->red_drawable->clip;

    spice_marshall_DisplayBase(base_marshaller, &base);
}

static inline void fill_palette(DisplayChannelClient *dcc,
                                SpicePalette *palette,
                                uint8_t *flags)
{
    if (palette == NULL) {
        return;
    }
    if (palette->unique) {
        if (red_palette_cache_find(dcc, palette->unique)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_FROM_CACHE;
            return;
        }
        if (red_palette_cache_add(dcc, palette->unique, 1)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_CACHE_ME;
        }
    }
}

static inline RedCompressBuf *red_display_alloc_compress_buf(DisplayChannelClient *dcc)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedCompressBuf *ret;

    if (display_channel->free_compress_bufs) {
        ret = display_channel->free_compress_bufs;
        display_channel->free_compress_bufs = ret->next;
    } else {
        ret = spice_new(RedCompressBuf, 1);
    }

    ret->next = dcc->send_data.used_compress_bufs;
    dcc->send_data.used_compress_bufs = ret;
    return ret;
}

static inline void __red_display_free_compress_buf(DisplayChannel *dc,
                                                   RedCompressBuf *buf)
{
    buf->next = dc->free_compress_bufs;
    dc->free_compress_bufs = buf;
}

static void red_display_free_compress_buf(DisplayChannelClient *dcc,
                                          RedCompressBuf *buf)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedCompressBuf **curr_used = &dcc->send_data.used_compress_bufs;

    for (;;) {
        ASSERT(*curr_used);
        if (*curr_used == buf) {
            *curr_used = buf->next;
            break;
        }
        curr_used = &(*curr_used)->next;
    }
    __red_display_free_compress_buf(display_channel, buf);
}

static void red_display_reset_compress_buf(DisplayChannelClient *dcc)
{
    while (dcc->send_data.used_compress_bufs) {
        RedCompressBuf *buf = dcc->send_data.used_compress_bufs;
        dcc->send_data.used_compress_bufs = buf->next;
        __red_display_free_compress_buf(DCC_TO_DC(dcc), buf);
    }
}

static void red_display_destroy_compress_bufs(DisplayChannel *display_channel)
{
    ASSERT(!red_channel_is_connected(&display_channel->common.base));
    while (display_channel->free_compress_bufs) {
        RedCompressBuf *buf = display_channel->free_compress_bufs;
        display_channel->free_compress_bufs = buf->next;
        free(buf);
    }
}

/******************************************************
 *      Global lz red drawables routines
*******************************************************/

/* if already exists, returns it. Otherwise allocates and adds it (1) to the ring tail
   in the channel (2) to the Drawable*/
static RedGlzDrawable *red_display_get_glz_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    RedGlzDrawable *ret;
    RingItem *item;

    // TODO - I don't really understand what's going on here, so doing the technical equivalent
    // now that we have multiple glz_dicts, so the only way to go from dcc to drawable glz is to go
    // over the glz_ring (unless adding some better data structure then a ring)
    DRAWABLE_FOREACH_GLZ(drawable, item, ret) {
        if (ret->dcc == dcc) {
            return ret;
        }
    }

    ret = spice_new(RedGlzDrawable, 1);

    ret->dcc = dcc;
    ret->red_drawable = ref_red_drawable(drawable->red_drawable);
    ret->drawable = drawable;
    ret->group_id = drawable->group_id;
    ret->self_bitmap = drawable->self_bitmap;
    ret->instances_count = 0;
    ring_init(&ret->instances);

    ring_item_init(&ret->link);
    ring_item_init(&ret->drawable_link);
    ring_add_before(&ret->link, &dcc->glz_drawables);
    ring_add(&drawable->glz_ring, &ret->drawable_link);
    dcc->common.worker->glz_drawable_count++;
    return ret;
}

/* allocates new instance and adds it to instances in the given drawable.
   NOTE - the caller should set the glz_instance returned by the encoder by itself.*/
static GlzDrawableInstanceItem *red_display_add_glz_drawable_instance(RedGlzDrawable *glz_drawable)
{
    ASSERT(glz_drawable->instances_count < MAX_GLZ_DRAWABLE_INSTANCES);
    // NOTE: We assume the additions are performed consecutively, without removals in the middle
    GlzDrawableInstanceItem *ret = glz_drawable->instances_pool + glz_drawable->instances_count;
    glz_drawable->instances_count++;

    ring_item_init(&ret->free_link);
    ring_item_init(&ret->glz_link);
    ring_add(&glz_drawable->instances, &ret->glz_link);
    ret->glz_instance = NULL;
    ret->red_glz_drawable = glz_drawable;

    return ret;
}

/* Remove from the to_free list and the instances_list.
   When no instance is left - the RedGlzDrawable is released too. (and the qxl drawable too, if
   it is not used by Drawable).
   NOTE - 1) can be called only by the display channel that created the drawable
          2) it is assumed that the instance was already removed from the dictionary*/
static void red_display_free_glz_drawable_instance(DisplayChannelClient *dcc,
                                                   GlzDrawableInstanceItem *glz_drawable_instance)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    RedGlzDrawable *glz_drawable;

    ASSERT(glz_drawable_instance);
    ASSERT(glz_drawable_instance->red_glz_drawable);

    glz_drawable = glz_drawable_instance->red_glz_drawable;

    ASSERT(glz_drawable->dcc == dcc);
    ASSERT(glz_drawable->instances_count);

    ring_remove(&glz_drawable_instance->glz_link);
    glz_drawable->instances_count--;
    // when the remove callback is performed from the channel that the
    // drawable belongs to, the instance is not added to the 'to_free' list
    if (ring_item_is_linked(&glz_drawable_instance->free_link)) {
        ring_remove(&glz_drawable_instance->free_link);
    }

    if (ring_is_empty(&glz_drawable->instances)) {
        ASSERT(!glz_drawable->instances_count);

        Drawable *drawable = glz_drawable->drawable;

        if (drawable) {
            ring_remove(&glz_drawable->drawable_link);
        }
        put_red_drawable(worker, glz_drawable->red_drawable,
                          glz_drawable->group_id, glz_drawable->self_bitmap);
        worker->glz_drawable_count--;
        if (ring_item_is_linked(&glz_drawable->link)) {
            ring_remove(&glz_drawable->link);
        }
        free(glz_drawable);
    }
}

static void red_display_handle_glz_drawables_to_free(DisplayChannelClient* dcc)
{
    RingItem *ring_link;

    if (!dcc->glz_dict) {
        return;
    }
    pthread_mutex_lock(&dcc->glz_drawables_inst_to_free_lock);
    while ((ring_link = ring_get_head(&dcc->glz_drawables_inst_to_free))) {
        GlzDrawableInstanceItem *drawable_instance = SPICE_CONTAINEROF(ring_link,
                                                                 GlzDrawableInstanceItem,
                                                                 free_link);
        red_display_free_glz_drawable_instance(dcc, drawable_instance);
    }
    pthread_mutex_unlock(&dcc->glz_drawables_inst_to_free_lock);
}

/*
 * Releases all the instances of the drawable from the dictionary and the display channel client.
 * The release of the last instance will also release the drawable itself and the qxl drawable
 * if possible.
 * NOTE - the caller should prevent encoding using the dictionary during this operation
 */
static void red_display_free_glz_drawable(DisplayChannelClient *dcc, RedGlzDrawable *drawable)
{
    RingItem *head_instance = ring_get_head(&drawable->instances);
    int cont = (head_instance != NULL);

    while (cont) {
        if (drawable->instances_count == 1) {
            /* Last instance: red_display_free_glz_drawable_instance will free the drawable */
            cont = FALSE;
        }
        GlzDrawableInstanceItem *instance = SPICE_CONTAINEROF(head_instance,
                                                        GlzDrawableInstanceItem,
                                                        glz_link);
        if (!ring_item_is_linked(&instance->free_link)) {
            // the instance didn't get out from window yet
            glz_enc_dictionary_remove_image(dcc->glz_dict->dict,
                                            instance->glz_instance,
                                            &dcc->glz_data.usr);
        }
        red_display_free_glz_drawable_instance(dcc, instance);

        if (cont) {
            head_instance = ring_get_head(&drawable->instances);
        }
    }
}

/* Clear all lz drawables - enforce their removal from the global dictionary.
   NOTE - prevents encoding using the dictionary during the operation*/
static void red_display_client_clear_glz_drawables(DisplayChannelClient *dcc)
{
    RingItem *ring_link;
    GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

    if (!glz_dict) {
        return;
    }

    // assure no display channel is during global lz encoding
    pthread_rwlock_wrlock(&glz_dict->encode_lock);
    while ((ring_link = ring_get_head(&dcc->glz_drawables))) {
        RedGlzDrawable *drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        // no need to lock the to_free list, since we assured no other thread is encoding and
        // thus not other thread access the to_free list of the channel
        red_display_free_glz_drawable(dcc, drawable);
    }
    pthread_rwlock_unlock(&glz_dict->encode_lock);
}

static void red_display_clear_glz_drawables(DisplayChannel *display_channel)
{
    RingItem *link;
    DisplayChannelClient *dcc;

    if (!display_channel) {
        return;
    }
    DCC_FOREACH(link, dcc, &display_channel->common.base) {
        red_display_client_clear_glz_drawables(dcc);
    }
}

/*
 * Remove from the global lz dictionary some glz_drawables that have no reference to
 * Drawable (their qxl drawables are released too).
 * NOTE - the caller should prevent encoding using the dictionary during the operation
 */
static int red_display_free_some_independent_glz_drawables(DisplayChannelClient *dcc)
{
    RingItem *ring_link;
    int n = 0;

    if (!dcc) {
        return 0;
    }
    ring_link = ring_get_head(&dcc->glz_drawables);
    while ((n < RED_RELEASE_BUNCH_SIZE) && (ring_link != NULL)) {
        RedGlzDrawable *glz_drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        ring_link = ring_next(&dcc->glz_drawables, ring_link);
        if (!glz_drawable->drawable) {
            red_display_free_glz_drawable(dcc, glz_drawable);
            n++;
        }
    }
    return n;
}

/******************************************************
 *              Encoders callbacks
*******************************************************/
static void quic_usr_error(QuicUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    red_printf("%s", usr_data->message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static void lz_usr_error(LzUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    red_printf("%s", usr_data->message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static void glz_usr_error(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);

    PANIC("%s", usr_data->message_buf); // if global lz fails in the middle
                                        // the consequences are not predictable since the window
                                        // can turn to be unsynchronized between the server and
                                        // and the client
}

static void quic_usr_warn(QuicUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    red_printf("%s", usr_data->message_buf);
}

static void lz_usr_warn(LzUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    red_printf("%s", usr_data->message_buf);
}

static void glz_usr_warn(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    red_printf("%s", usr_data->message_buf);
}

static void *quic_usr_malloc(QuicUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void *lz_usr_malloc(LzUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void *glz_usr_malloc(GlzEncoderUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void quic_usr_free(QuicUsrContext *usr, void *ptr)
{
    free(ptr);
}

static void lz_usr_free(LzUsrContext *usr, void *ptr)
{
    free(ptr);
}

static void glz_usr_free(GlzEncoderUsrContext *usr, void *ptr)
{
    free(ptr);
}

static inline int encoder_usr_more_space(EncoderData *enc_data, uint32_t **io_ptr)
{
    RedCompressBuf *buf;

    if (!(buf = red_display_alloc_compress_buf(enc_data->dcc))) {
        return 0;
    }
    enc_data->bufs_tail->send_next = buf;
    enc_data->bufs_tail = buf;
    buf->send_next = NULL;
    *io_ptr = buf->buf;
    return sizeof(buf->buf) >> 2;
}

static int quic_usr_more_space(QuicUsrContext *usr, uint32_t **io_ptr, int rows_completed)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static int lz_usr_more_space(LzUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static int glz_usr_more_space(GlzEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static int jpeg_usr_more_space(JpegEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static int zlib_usr_more_space(ZlibEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static inline int encoder_usr_more_lines(EncoderData *enc_data, uint8_t **lines)
{
    struct SpiceChunk *chunk;

    if (enc_data->u.lines_data.reverse) {
        if (!(enc_data->u.lines_data.next >= 0)) {
            return 0;
        }
    } else {
        if (!(enc_data->u.lines_data.next < enc_data->u.lines_data.chunks->num_chunks)) {
            return 0;
        }
    }

    chunk = &enc_data->u.lines_data.chunks->chunk[enc_data->u.lines_data.next];
    if (chunk->len % enc_data->u.lines_data.stride) {
        return 0;
    }

    if (enc_data->u.lines_data.reverse) {
        enc_data->u.lines_data.next--;
        *lines = chunk->data + chunk->len - enc_data->u.lines_data.stride;
    } else {
        enc_data->u.lines_data.next++;
        *lines = chunk->data;
    }

    return chunk->len / enc_data->u.lines_data.stride;
}

static int quic_usr_more_lines(QuicUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int lz_usr_more_lines(LzUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int glz_usr_more_lines(GlzEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int jpeg_usr_more_lines(JpegEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int zlib_usr_more_input(ZlibEncoderUsrContext *usr, uint8_t** input)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    int buf_size;

    if (!usr_data->u.compressed_data.next) {
        ASSERT(usr_data->u.compressed_data.size_left == 0);
        return 0;
    }

    *input = (uint8_t*)usr_data->u.compressed_data.next->buf;
    buf_size = MIN(sizeof(usr_data->u.compressed_data.next->buf),
                   usr_data->u.compressed_data.size_left);

    usr_data->u.compressed_data.next = usr_data->u.compressed_data.next->send_next;
    usr_data->u.compressed_data.size_left -= buf_size;
    return buf_size;
}

static void glz_usr_free_image(GlzEncoderUsrContext *usr, GlzUsrImageContext *image)
{
    GlzData *lz_data = (GlzData *)usr;
    GlzDrawableInstanceItem *glz_drawable_instance = (GlzDrawableInstanceItem *)image;
    DisplayChannelClient *drawable_cc = glz_drawable_instance->red_glz_drawable->dcc;
    DisplayChannelClient *this_cc = SPICE_CONTAINEROF(lz_data, DisplayChannelClient, glz_data);
    if (this_cc == drawable_cc) {
        red_display_free_glz_drawable_instance(drawable_cc, glz_drawable_instance);
    } else {
        /* The glz dictionary is shared between all DisplayChannelClient
         * instances that belong to the same client, and glz_usr_free_image
         * can be called by the dictionary code
         * (glz_dictionary_window_remove_head). Thus this function can be
         * called from any DisplayChannelClient thread, hence the need for
         * this check.
         */
        pthread_mutex_lock(&drawable_cc->glz_drawables_inst_to_free_lock);
        ring_add_before(&glz_drawable_instance->free_link,
                        &drawable_cc->glz_drawables_inst_to_free);
        pthread_mutex_unlock(&drawable_cc->glz_drawables_inst_to_free_lock);
    }
}

static inline void red_init_quic(RedWorker *worker)
{
    worker->quic_data.usr.error = quic_usr_error;
    worker->quic_data.usr.warn = quic_usr_warn;
    worker->quic_data.usr.info = quic_usr_warn;
    worker->quic_data.usr.malloc = quic_usr_malloc;
    worker->quic_data.usr.free = quic_usr_free;
    worker->quic_data.usr.more_space = quic_usr_more_space;
    worker->quic_data.usr.more_lines = quic_usr_more_lines;

    worker->quic = quic_create(&worker->quic_data.usr);

    if (!worker->quic) {
        PANIC("create quic failed");
    }
}

static inline void red_init_lz(RedWorker *worker)
{
    worker->lz_data.usr.error = lz_usr_error;
    worker->lz_data.usr.warn = lz_usr_warn;
    worker->lz_data.usr.info = lz_usr_warn;
    worker->lz_data.usr.malloc = lz_usr_malloc;
    worker->lz_data.usr.free = lz_usr_free;
    worker->lz_data.usr.more_space = lz_usr_more_space;
    worker->lz_data.usr.more_lines = lz_usr_more_lines;

    worker->lz = lz_create(&worker->lz_data.usr);

    if (!worker->lz) {
        PANIC("create lz failed");
    }
}

/* TODO: split off to DisplayChannel? avoid just copying those cb pointers */
static inline void red_display_init_glz_data(DisplayChannelClient *dcc)
{
    dcc->glz_data.usr.error = glz_usr_error;
    dcc->glz_data.usr.warn = glz_usr_warn;
    dcc->glz_data.usr.info = glz_usr_warn;
    dcc->glz_data.usr.malloc = glz_usr_malloc;
    dcc->glz_data.usr.free = glz_usr_free;
    dcc->glz_data.usr.more_space = glz_usr_more_space;
    dcc->glz_data.usr.more_lines = glz_usr_more_lines;
    dcc->glz_data.usr.free_image = glz_usr_free_image;
}

static inline void red_init_jpeg(RedWorker *worker)
{
    worker->jpeg_data.usr.more_space = jpeg_usr_more_space;
    worker->jpeg_data.usr.more_lines = jpeg_usr_more_lines;

    worker->jpeg = jpeg_encoder_create(&worker->jpeg_data.usr);

    if (!worker->jpeg) {
        PANIC("create jpeg encoder failed");
    }
}

static inline void red_init_zlib(RedWorker *worker)
{
    worker->zlib_data.usr.more_space = zlib_usr_more_space;
    worker->zlib_data.usr.more_input = zlib_usr_more_input;

    worker->zlib = zlib_encoder_create(&worker->zlib_data.usr, ZLIB_DEFAULT_COMPRESSION_LEVEL);

    if (!worker->zlib) {
        PANIC("create zlib encoder failed");
    }
}

#ifdef __GNUC__
#define ATTR_PACKED __attribute__ ((__packed__))
#else
#define ATTR_PACKED
#pragma pack(push)
#pragma pack(1)
#endif


typedef struct ATTR_PACKED rgb32_pixel_t {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t pad;
} rgb32_pixel_t;

typedef struct ATTR_PACKED rgb24_pixel_t {
    uint8_t b;
    uint8_t g;
    uint8_t r;
} rgb24_pixel_t;

typedef uint16_t rgb16_pixel_t;

#ifndef __GNUC__
#pragma pack(pop)
#endif

#undef ATTR_PACKED

#define RED_BITMAP_UTILS_RGB16
#include "red_bitmap_utils.h"
#define RED_BITMAP_UTILS_RGB24
#include "red_bitmap_utils.h"
#define RED_BITMAP_UTILS_RGB32
#include "red_bitmap_utils.h"

#define GRADUAL_HIGH_RGB24_TH -0.03
#define GRADUAL_HIGH_RGB16_TH 0

// setting a more permissive threshold for stream identification in order
// not to miss streams that were artificially scaled on the guest (e.g., full screen view
// in window media player 12). see red_stream_add_frame
#define GRADUAL_MEDIUM_SCORE_TH 0.002

// assumes that stride doesn't overflow
static BitmapGradualType _get_bitmap_graduality_level(RedWorker *worker, SpiceBitmap *bitmap,
                                                      uint32_t group_id)
{
    double score = 0.0;
    int num_samples = 0;
    int num_lines;
    double chunk_score = 0.0;
    int chunk_num_samples = 0;
    uint32_t x, i;
    SpiceChunk *chunk;

    chunk = bitmap->data->chunk;
    for (i = 0; i < bitmap->data->num_chunks; i++) {
        num_lines = chunk[i].len / bitmap->stride;
        x = bitmap->x;
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_16BIT:
            compute_lines_gradual_score_rgb16((rgb16_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_24BIT:
            compute_lines_gradual_score_rgb24((rgb24_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_32BIT:
        case SPICE_BITMAP_FMT_RGBA:
            compute_lines_gradual_score_rgb32((rgb32_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        default:
            red_error("invalid bitmap format (not RGB) %u", bitmap->format);
        }
        score += chunk_score;
        num_samples += chunk_num_samples;
    }

    ASSERT(num_samples);
    score /= num_samples;

    if (bitmap->format == SPICE_BITMAP_FMT_16BIT) {
        if (score < GRADUAL_HIGH_RGB16_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    } else {
        if (score < GRADUAL_HIGH_RGB24_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    }

    if (score < GRADUAL_MEDIUM_SCORE_TH) {
        return BITMAP_GRADUAL_MEDIUM;
    } else {
        return BITMAP_GRADUAL_LOW;
    }
}

static inline int _stride_is_extra(SpiceBitmap *bitmap)
{
    ASSERT(bitmap);
    if (BITMAP_FMT_IS_RGB[bitmap->format]) {
        return ((bitmap->x * BITMAP_FMP_BYTES_PER_PIXEL[bitmap->format]) < bitmap->stride);
    } else {
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_8BIT:
            return (bitmap->x < bitmap->stride);
        case SPICE_BITMAP_FMT_4BIT_BE:
        case SPICE_BITMAP_FMT_4BIT_LE: {
            int bytes_width = SPICE_ALIGN(bitmap->x, 2) >> 1;
            return bytes_width < bitmap->stride;
        }
        case SPICE_BITMAP_FMT_1BIT_BE:
        case SPICE_BITMAP_FMT_1BIT_LE: {
            int bytes_width = SPICE_ALIGN(bitmap->x, 8) >> 3;
            return bytes_width < bitmap->stride;
        }
        default:
            red_error("invalid image type %u", bitmap->format);
        }
    }
}

static const LzImageType MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[] = {
    LZ_IMAGE_TYPE_INVALID,
    LZ_IMAGE_TYPE_PLT1_LE,
    LZ_IMAGE_TYPE_PLT1_BE,
    LZ_IMAGE_TYPE_PLT4_LE,
    LZ_IMAGE_TYPE_PLT4_BE,
    LZ_IMAGE_TYPE_PLT8,
    LZ_IMAGE_TYPE_RGB16,
    LZ_IMAGE_TYPE_RGB24,
    LZ_IMAGE_TYPE_RGB32,
    LZ_IMAGE_TYPE_RGBA
};

typedef struct compress_send_data_t {
    void*    comp_buf;
    uint32_t comp_buf_size;
    SpicePalette *lzplt_palette;
    int is_lossy;
} compress_send_data_t;


static inline int red_glz_compress_image(DisplayChannelClient *dcc,
                                         SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                                         compress_send_data_t* o_comp_data)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif
    ASSERT(BITMAP_FMT_IS_RGB[src->format]);
    GlzData *glz_data = &dcc->glz_data;
    ZlibData *zlib_data;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    RedGlzDrawable *glz_drawable;
    GlzDrawableInstanceItem *glz_drawable_instance;
    int glz_size;
    int zlib_size;

    glz_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    glz_data->data.bufs_head = glz_data->data.bufs_tail;

    if (!glz_data->data.bufs_head) {
        return FALSE;
    }

    glz_data->data.bufs_head->send_next = NULL;
    glz_data->data.dcc = dcc;

    glz_drawable = red_display_get_glz_drawable(dcc, drawable);
    glz_drawable_instance = red_display_add_glz_drawable_instance(glz_drawable);

    glz_data->data.u.lines_data.chunks = src->data;
    glz_data->data.u.lines_data.stride = src->stride;
    glz_data->data.u.lines_data.next = 0;
    glz_data->data.u.lines_data.reverse = 0;
    glz_data->usr.more_lines = glz_usr_more_lines;

    glz_size = glz_encode(dcc->glz, type, src->x, src->y,
                          (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN), NULL, 0,
                          src->stride, (uint8_t*)glz_data->data.bufs_head->buf,
                          sizeof(glz_data->data.bufs_head->buf),
                          glz_drawable_instance,
                          &glz_drawable_instance->glz_instance);

    stat_compress_add(&display_channel->glz_stat, start_time, src->stride * src->y, glz_size);

    if (!display_channel->enable_zlib_glz_wrap || (glz_size < MIN_GLZ_SIZE_FOR_ZLIB)) {
        goto glz;
    }
#ifdef COMPRESS_STAT
    start_time = stat_now();
#endif
    zlib_data = &worker->zlib_data;

    zlib_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    zlib_data->data.bufs_head = zlib_data->data.bufs_tail;

    if (!zlib_data->data.bufs_head) {
        red_printf("failed to allocate zlib compress buffer");
        goto glz;
    }

    zlib_data->data.bufs_head->send_next = NULL;
    zlib_data->data.dcc = dcc;

    zlib_data->data.u.compressed_data.next = glz_data->data.bufs_head;
    zlib_data->data.u.compressed_data.size_left = glz_size;

    zlib_size = zlib_encode(worker->zlib, display_channel->zlib_level,
                            glz_size, (uint8_t*)zlib_data->data.bufs_head->buf,
                            sizeof(zlib_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (zlib_size >= glz_size) {
        while (zlib_data->data.bufs_head) {
            RedCompressBuf *buf = zlib_data->data.bufs_head;
            zlib_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        goto glz;
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_ZLIB_GLZ_RGB;
    dest->u.zlib_glz.glz_data_size = glz_size;
    dest->u.zlib_glz.data_size = zlib_size;

    o_comp_data->comp_buf = zlib_data->data.bufs_head;
    o_comp_data->comp_buf_size = zlib_size;

    stat_compress_add(&display_channel->zlib_glz_stat, start_time, glz_size, zlib_size);
    return TRUE;
glz:
    dest->descriptor.type = SPICE_IMAGE_TYPE_GLZ_RGB;
    dest->u.lz_rgb.data_size = glz_size;

    o_comp_data->comp_buf = glz_data->data.bufs_head;
    o_comp_data->comp_buf_size = glz_size;

    return TRUE;
}

static inline int red_lz_compress_image(DisplayChannelClient *dcc,
                                        SpiceImage *dest, SpiceBitmap *src,
                                        compress_send_data_t* o_comp_data, uint32_t group_id)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    LzData *lz_data = &worker->lz_data;
    LzContext *lz = worker->lz;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    int size;            // size of the compressed data

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif

    lz_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    lz_data->data.bufs_head = lz_data->data.bufs_tail;

    if (!lz_data->data.bufs_head) {
        return FALSE;
    }

    lz_data->data.bufs_head->send_next = NULL;
    lz_data->data.dcc = dcc;

    if (setjmp(lz_data->data.jmp_env)) {
        while (lz_data->data.bufs_head) {
            RedCompressBuf *buf = lz_data->data.bufs_head;
            lz_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        return FALSE;
    }

    lz_data->data.u.lines_data.chunks = src->data;
    lz_data->data.u.lines_data.stride = src->stride;
    lz_data->data.u.lines_data.next = 0;
    lz_data->data.u.lines_data.reverse = 0;
    lz_data->usr.more_lines = lz_usr_more_lines;

    size = lz_encode(lz, type, src->x, src->y,
                     !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN),
                     NULL, 0, src->stride,
                     (uint8_t*)lz_data->data.bufs_head->buf,
                     sizeof(lz_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (size > (src->y * src->stride)) {
        longjmp(lz_data->data.jmp_env, 1);
    }

    if (BITMAP_FMT_IS_RGB[src->format]) {
        dest->descriptor.type = SPICE_IMAGE_TYPE_LZ_RGB;
        dest->u.lz_rgb.data_size = size;

        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;
    } else {
        dest->descriptor.type = SPICE_IMAGE_TYPE_LZ_PLT;
        dest->u.lz_plt.data_size = size;
        dest->u.lz_plt.flags = src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;
        dest->u.lz_plt.palette = src->palette;
        dest->u.lz_plt.palette_id = src->palette->unique;

        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;

        fill_palette(dcc, dest->u.lz_plt.palette, &(dest->u.lz_plt.flags));
        o_comp_data->lzplt_palette = dest->u.lz_plt.palette;
    }

    stat_compress_add(&display_channel->lz_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

static int red_jpeg_compress_image(DisplayChannelClient *dcc, SpiceImage *dest,
                                   SpiceBitmap *src, compress_send_data_t* o_comp_data,
                                   uint32_t group_id)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    JpegData *jpeg_data = &worker->jpeg_data;
    LzData *lz_data = &worker->lz_data;
    JpegEncoderContext *jpeg = worker->jpeg;
    LzContext *lz = worker->lz;
    JpegEncoderImageType jpeg_in_type;
    int jpeg_size = 0;
    int has_alpha = FALSE;
    int alpha_lz_size = 0;
    int comp_head_filled;
    int comp_head_left;
    int stride;
    uint8_t *lz_out_start_byte;

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif
    switch (src->format) {
    case SPICE_BITMAP_FMT_16BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGR24;
        break;
    case SPICE_BITMAP_FMT_32BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        has_alpha = TRUE;
        break;
    default:
        return FALSE;
    }

    jpeg_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    jpeg_data->data.bufs_head = jpeg_data->data.bufs_tail;

    if (!jpeg_data->data.bufs_head) {
        red_printf("failed to allocate compress buffer");
        return FALSE;
    }

    jpeg_data->data.bufs_head->send_next = NULL;
    jpeg_data->data.dcc = dcc;

    if (setjmp(jpeg_data->data.jmp_env)) {
        while (jpeg_data->data.bufs_head) {
            RedCompressBuf *buf = jpeg_data->data.bufs_head;
            jpeg_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    jpeg_data->data.u.lines_data.chunks = src->data;
    jpeg_data->data.u.lines_data.stride = src->stride;
    jpeg_data->usr.more_lines = jpeg_usr_more_lines;
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        jpeg_data->data.u.lines_data.next = 0;
        jpeg_data->data.u.lines_data.reverse = 0;
        stride = src->stride;
    } else {
        jpeg_data->data.u.lines_data.next = src->data->num_chunks - 1;
        jpeg_data->data.u.lines_data.reverse = 1;
        stride = -src->stride;
    }
    jpeg_size = jpeg_encode(jpeg, display_channel->jpeg_quality, jpeg_in_type,
                            src->x, src->y, NULL,
                            0, stride, (uint8_t*)jpeg_data->data.bufs_head->buf,
                            sizeof(jpeg_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (jpeg_size > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    if (!has_alpha) {
        dest->descriptor.type = SPICE_IMAGE_TYPE_JPEG;
        dest->u.jpeg.data_size = jpeg_size;

        o_comp_data->comp_buf = jpeg_data->data.bufs_head;
        o_comp_data->comp_buf_size = jpeg_size;
        o_comp_data->is_lossy = TRUE;

        stat_compress_add(&display_channel->jpeg_stat, start_time, src->stride * src->y,
                          o_comp_data->comp_buf_size);
        return TRUE;
    }

     lz_data->data.bufs_head = jpeg_data->data.bufs_tail;
     lz_data->data.bufs_tail = lz_data->data.bufs_head;

     comp_head_filled = jpeg_size % sizeof(lz_data->data.bufs_head->buf);
     comp_head_left = sizeof(lz_data->data.bufs_head->buf) - comp_head_filled;
     lz_out_start_byte = ((uint8_t *)lz_data->data.bufs_head->buf) + comp_head_filled;

     lz_data->data.dcc = dcc;

     lz_data->data.u.lines_data.chunks = src->data;
     lz_data->data.u.lines_data.stride = src->stride;
     lz_data->data.u.lines_data.next = 0;
     lz_data->data.u.lines_data.reverse = 0;
     lz_data->usr.more_lines = lz_usr_more_lines;

     alpha_lz_size = lz_encode(lz, LZ_IMAGE_TYPE_XXXA, src->x, src->y,
                               !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN),
                               NULL, 0, src->stride,
                               lz_out_start_byte,
                               comp_head_left);

    // the compressed buffer is bigger than the original data
    if ((jpeg_size + alpha_lz_size) > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_JPEG_ALPHA;
    dest->u.jpeg_alpha.flags = 0;
    if (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN) {
        dest->u.jpeg_alpha.flags |= SPICE_JPEG_ALPHA_FLAGS_TOP_DOWN;
    }

    dest->u.jpeg_alpha.jpeg_size = jpeg_size;
    dest->u.jpeg_alpha.data_size = jpeg_size + alpha_lz_size;

    o_comp_data->comp_buf = jpeg_data->data.bufs_head;
    o_comp_data->comp_buf_size = jpeg_size + alpha_lz_size;
    o_comp_data->is_lossy = TRUE;
    stat_compress_add(&display_channel->jpeg_alpha_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

static inline int red_quic_compress_image(DisplayChannelClient *dcc, SpiceImage *dest,
                                          SpiceBitmap *src, compress_send_data_t* o_comp_data,
                                          uint32_t group_id)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    QuicData *quic_data = &worker->quic_data;
    QuicContext *quic = worker->quic;
    QuicImageType type;
    int size, stride;

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif

    switch (src->format) {
    case SPICE_BITMAP_FMT_32BIT:
        type = QUIC_IMAGE_TYPE_RGB32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        type = QUIC_IMAGE_TYPE_RGBA;
        break;
    case SPICE_BITMAP_FMT_16BIT:
        type = QUIC_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        type = QUIC_IMAGE_TYPE_RGB24;
        break;
    default:
        return FALSE;
    }

    quic_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    quic_data->data.bufs_head = quic_data->data.bufs_tail;

    if (!quic_data->data.bufs_head) {
        return FALSE;
    }

    quic_data->data.bufs_head->send_next = NULL;
    quic_data->data.dcc = dcc;

    if (setjmp(quic_data->data.jmp_env)) {
        while (quic_data->data.bufs_head) {
            RedCompressBuf *buf = quic_data->data.bufs_head;
            quic_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    quic_data->data.u.lines_data.chunks = src->data;
    quic_data->data.u.lines_data.stride = src->stride;
    quic_data->usr.more_lines = quic_usr_more_lines;
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        quic_data->data.u.lines_data.next = 0;
        quic_data->data.u.lines_data.reverse = 0;
        stride = src->stride;
    } else {
        quic_data->data.u.lines_data.next = src->data->num_chunks - 1;
        quic_data->data.u.lines_data.reverse = 1;
        stride = -src->stride;
    }
    size = quic_encode(quic, type, src->x, src->y, NULL, 0, stride,
                       quic_data->data.bufs_head->buf,
                       sizeof(quic_data->data.bufs_head->buf) >> 2);

    // the compressed buffer is bigger than the original data
    if ((size << 2) > (src->y * src->stride)) {
        longjmp(quic_data->data.jmp_env, 1);
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_QUIC;
    dest->u.quic.data_size = size << 2;

    o_comp_data->comp_buf = quic_data->data.bufs_head;
    o_comp_data->comp_buf_size = size << 2;

    stat_compress_add(&display_channel->quic_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

#define MIN_SIZE_TO_COMPRESS 54
#define MIN_DIMENSION_TO_QUIC 3
static inline int red_compress_image(DisplayChannelClient *dcc,
                                     SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                                     int can_lossy,
                                     compress_send_data_t* o_comp_data)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    spice_image_compression_t image_compression =
        display_channel->common.worker->image_compression;
    int quic_compress = FALSE;

    if ((image_compression == SPICE_IMAGE_COMPRESS_OFF) ||
        ((src->y * src->stride) < MIN_SIZE_TO_COMPRESS)) { // TODO: change the size cond
        return FALSE;
    } else if (image_compression == SPICE_IMAGE_COMPRESS_QUIC) {
        if (BITMAP_FMT_IS_PLT[src->format]) {
            return FALSE;
        } else {
            quic_compress = TRUE;
        }
    } else {
        /*
            lz doesn't handle (1) bitmaps with strides that are larger than the width
            of the image in bytes (2) unstable bitmaps
        */
        if (_stride_is_extra(src) || (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE)) {
            if ((image_compression == SPICE_IMAGE_COMPRESS_LZ) ||
                (image_compression == SPICE_IMAGE_COMPRESS_GLZ) ||
                BITMAP_FMT_IS_PLT[src->format]) {
                return FALSE;
            } else {
                quic_compress = TRUE;
            }
        } else {
            if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
                (image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ)) {
                if ((src->x < MIN_DIMENSION_TO_QUIC) || (src->y < MIN_DIMENSION_TO_QUIC)) {
                    quic_compress = FALSE;
                } else {
                    if (drawable->copy_bitmap_graduality == BITMAP_GRADUAL_INVALID) {
                        quic_compress = BITMAP_FMT_IS_RGB[src->format] &&
                            (_get_bitmap_graduality_level(display_channel->common.worker, src,
                                                          drawable->group_id) ==
                             BITMAP_GRADUAL_HIGH);
                    } else {
                        quic_compress = (drawable->copy_bitmap_graduality == BITMAP_GRADUAL_HIGH);
                    }
                }
            } else {
                quic_compress = FALSE;
            }
        }
    }

    if (quic_compress) {
#ifdef COMPRESS_DEBUG
        red_printf("QUIC compress");
#endif
        // if bitmaps is picture-like, compress it using jpeg
        if (can_lossy && display_channel->enable_jpeg &&
            ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
            (image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ))) {
            // if we use lz for alpha, the stride can't be extra
            if (src->format != SPICE_BITMAP_FMT_RGBA || !_stride_is_extra(src)) {
                return red_jpeg_compress_image(dcc, dest,
                                               src, o_comp_data, drawable->group_id);
            }
        }
        return red_quic_compress_image(dcc, dest,
                                       src, o_comp_data, drawable->group_id);
    } else {
        int glz;
        int ret;
        if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ) ||
            (image_compression == SPICE_IMAGE_COMPRESS_GLZ)) {
            glz = BITMAP_FMT_IS_RGB[src->format] && (
                    (src->x * src->y) < glz_enc_dictionary_get_size(
                        dcc->glz_dict->dict));
        } else if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
                   (image_compression == SPICE_IMAGE_COMPRESS_LZ)) {
            glz = FALSE;
        } else {
            red_error("invalid image compression type %u", image_compression);
        }

        if (glz) {
            /* using the global dictionary only if it is not frozen */
            pthread_rwlock_rdlock(&dcc->glz_dict->encode_lock);
            if (!dcc->glz_dict->migrate_freeze) {
                ret = red_glz_compress_image(dcc,
                                             dest, src,
                                             drawable, o_comp_data);
            } else {
                glz = FALSE;
            }
            pthread_rwlock_unlock(&dcc->glz_dict->encode_lock);
        }

        if (!glz) {
            ret = red_lz_compress_image(dcc, dest, src, o_comp_data,
                                        drawable->group_id);
#ifdef COMPRESS_DEBUG
            red_printf("LZ LOCAL compress");
#endif
        }
#ifdef COMPRESS_DEBUG
        else {
            red_printf("LZ global compress fmt=%d", src->format);
        }
#endif
        return ret;
    }
}

static inline void red_display_add_image_to_pixmap_cache(RedChannelClient *rcc,
                                                         SpiceImage *image, SpiceImage *io_image,
                                                         int is_lossy)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        ASSERT(image->descriptor.width * image->descriptor.height > 0);
        if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME)) {
            if (pixmap_cache_add(dcc->pixmap_cache, image->descriptor.id,
                                 image->descriptor.width * image->descriptor.height, is_lossy,
                                 dcc)) {
                io_image->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
                dcc->send_data.pixmap_cache_items[dcc->send_data.num_pixmap_cache_items++] =
                                                                               image->descriptor.id;
                stat_inc_counter(display_channel->add_to_cache_counter, 1);
            }
        }
    }

    if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        stat_inc_counter(display_channel->non_cache_counter, 1);
    }
}

typedef enum {
    FILL_BITS_TYPE_INVALID,
    FILL_BITS_TYPE_CACHE,
    FILL_BITS_TYPE_SURFACE,
    FILL_BITS_TYPE_COMPRESS_LOSSLESS,
    FILL_BITS_TYPE_COMPRESS_LOSSY,
    FILL_BITS_TYPE_BITMAP,
} FillBitsType;

/* if the number of times fill_bits can be called per one qxl_drawable increases -
   MAX_LZ_DRAWABLE_INSTANCES must be increased as well */
static FillBitsType fill_bits(DisplayChannelClient *dcc, SpiceMarshaller *m,
                              SpiceImage *simage, Drawable *drawable, int can_lossy)
{
    RedChannelClient *rcc = &dcc->common.base;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    RedWorker *worker = dcc->common.worker;
    SpiceImage image;
    compress_send_data_t comp_send_data = {0};
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    if (simage == NULL) {
        ASSERT(drawable->self_bitmap);
        simage = drawable->self_bitmap;
    }

    image.descriptor = simage->descriptor;

    if ((simage->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int lossy_cache_item;
        if (pixmap_cache_hit(dcc->pixmap_cache, image.descriptor.id,
                             &lossy_cache_item, dcc)) {
            dcc->send_data.pixmap_cache_items[dcc->send_data.num_pixmap_cache_items++] =
                                                                               image.descriptor.id;
            if (can_lossy || !lossy_cache_item) {
                if (!display_channel->enable_jpeg || lossy_cache_item) {
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE;
                } else {
                    // making sure, in multiple monitor scenario, that lossy items that
                    // should have been replaced with lossless data by one display channel,
                    // will be retrieved as lossless by another display channel.
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE_LOSSLESS;
                }
                spice_marshall_Image(m, &image,
                                     &bitmap_palette_out, &lzplt_palette_out);
                ASSERT(bitmap_palette_out == NULL);
                ASSERT(lzplt_palette_out == NULL);
                stat_inc_counter(display_channel->cache_hits_counter, 1);
                return FILL_BITS_TYPE_CACHE;
            } else {
                pixmap_cache_set_lossy(dcc->pixmap_cache, simage->descriptor.id,
                                       FALSE);
                image.descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME;
            }
        }
    }

    switch (simage->descriptor.type) {
    case SPICE_IMAGE_TYPE_SURFACE: {
        int surface_id;
        RedSurface *surface;

        surface_id = simage->u.surface.surface_id;
        validate_surface(worker, surface_id);

        surface = &worker->surfaces[surface_id];
        image.descriptor.type = SPICE_IMAGE_TYPE_SURFACE;
        image.descriptor.flags = 0;
        image.descriptor.width = surface->context.width;
        image.descriptor.height = surface->context.height;

        image.u.surface.surface_id = surface_id;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        ASSERT(bitmap_palette_out == NULL);
        ASSERT(lzplt_palette_out == NULL);
        return FILL_BITS_TYPE_SURFACE;
    }
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmap *bitmap = &image.u.bitmap;
#ifdef DUMP_BITMAP
        dump_bitmap(display_channel->common.worker, &simage->u.bitmap, drawable->group_id);
#endif
        /* Images must be added to the cache only after they are compressed
           in order to prevent starvation in the client between pixmap_cache and
           global dictionary (in cases of multiple monitors) */
        if (!red_compress_image(dcc, &image, &simage->u.bitmap,
                                drawable, can_lossy, &comp_send_data)) {
            SpicePalette *palette;

            red_display_add_image_to_pixmap_cache(rcc, simage, &image, FALSE);

            *bitmap = simage->u.bitmap;
            bitmap->flags = bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;

            palette = bitmap->palette;
            fill_palette(dcc, palette, &bitmap->flags);
            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            ASSERT(lzplt_palette_out == NULL);

            if (bitmap_palette_out && palette) {
                spice_marshall_Palette(bitmap_palette_out, palette);
            }

            spice_marshaller_add_ref_chunks(m, bitmap->data);
            return FILL_BITS_TYPE_BITMAP;
        } else {
            red_display_add_image_to_pixmap_cache(rcc, simage, &image,
                                                  comp_send_data.is_lossy);

            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            ASSERT(bitmap_palette_out == NULL);

            marshaller_add_compressed(m, comp_send_data.comp_buf,
                                      comp_send_data.comp_buf_size);

            if (lzplt_palette_out && comp_send_data.lzplt_palette) {
                spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
            }

            ASSERT(!comp_send_data.is_lossy || can_lossy);
            return (comp_send_data.is_lossy ? FILL_BITS_TYPE_COMPRESS_LOSSY :
                                              FILL_BITS_TYPE_COMPRESS_LOSSLESS);
        }
        break;
    }
    case SPICE_IMAGE_TYPE_QUIC:
        red_display_add_image_to_pixmap_cache(rcc, simage, &image, FALSE);
        image.u.quic = simage->u.quic;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        ASSERT(bitmap_palette_out == NULL);
        ASSERT(lzplt_palette_out == NULL);
        spice_marshaller_add_ref_chunks(m, image.u.quic.data);
        return FILL_BITS_TYPE_COMPRESS_LOSSLESS;
    default:
        red_error("invalid image type %u", image.descriptor.type);
    }
}

static void fill_mask(RedChannelClient *rcc, SpiceMarshaller *m,
                      SpiceImage *mask_bitmap, Drawable *drawable)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if (mask_bitmap && m) {
        if (display_channel->common.worker->image_compression != SPICE_IMAGE_COMPRESS_OFF) {
            spice_image_compression_t save_img_comp =
                display_channel->common.worker->image_compression;
            display_channel->common.worker->image_compression = SPICE_IMAGE_COMPRESS_OFF;
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
            display_channel->common.worker->image_compression = save_img_comp;
        } else {
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
        }
    }
}

static void fill_attr(SpiceMarshaller *m, SpiceLineAttr *attr, uint32_t group_id)
{
    int i;

    if (m && attr->style_nseg) {
        for (i = 0 ; i < attr->style_nseg; i++) {
            spice_marshaller_add_uint32(m, attr->style[i]);
        }
    }
}

static void fill_cursor(CursorChannelClient *ccc, SpiceCursor *red_cursor,
                        CursorItem *cursor, AddBufInfo *addbuf)
{
    RedCursorCmd *cursor_cmd;
    addbuf->data = NULL;

    if (!cursor) {
        red_cursor->flags = SPICE_CURSOR_FLAGS_NONE;
        return;
    }

    cursor_cmd = cursor->red_cursor;
    *red_cursor = cursor_cmd->u.set.shape;

    if (red_cursor->header.unique) {
        if (red_cursor_cache_find(ccc, red_cursor->header.unique)) {
            red_cursor->flags |= SPICE_CURSOR_FLAGS_FROM_CACHE;
            return;
        }
        if (red_cursor_cache_add(ccc, red_cursor->header.unique, 1)) {
            red_cursor->flags |= SPICE_CURSOR_FLAGS_CACHE_ME;
        }
    }

    if (red_cursor->data_size) {
        addbuf->type = BUF_TYPE_RAW;
        addbuf->data = red_cursor->data;
        addbuf->size = red_cursor->data_size;
    }
}

static inline void red_display_reset_send_data(DisplayChannelClient *dcc)
{
    red_display_reset_compress_buf(dcc);
    dcc->send_data.free_list.res->count = 0;
    dcc->send_data.num_pixmap_cache_items = 0;
    memset(dcc->send_data.free_list.sync, 0, sizeof(dcc->send_data.free_list.sync));
}

/* set area=NULL for testing the whole surface */
static int is_surface_area_lossy(DisplayChannelClient *dcc, uint32_t surface_id,
                                 const SpiceRect *area, SpiceRect *out_lossy_area)
{
    RedSurface *surface;
    QRegion *surface_lossy_region;
    QRegion lossy_region;
    RedWorker *worker = dcc->common.worker;

    validate_surface(worker, surface_id);
    surface = &worker->surfaces[surface_id];
    surface_lossy_region = &dcc->surface_client_lossy_region[surface_id];

    if (!area) {
        if (region_is_empty(surface_lossy_region)) {
            return FALSE;
        } else {
            out_lossy_area->top = 0;
            out_lossy_area->left = 0;
            out_lossy_area->bottom = surface->context.height;
            out_lossy_area->right = surface->context.width;
            return TRUE;
        }
    }

    region_init(&lossy_region);
    region_add(&lossy_region, area);
    region_and(&lossy_region, surface_lossy_region);
    if (!region_is_empty(&lossy_region)) {
        out_lossy_area->left = lossy_region.extents.x1;
        out_lossy_area->top = lossy_region.extents.y1;
        out_lossy_area->right = lossy_region.extents.x2;
        out_lossy_area->bottom = lossy_region.extents.y2;
        region_destroy(&lossy_region);
        return TRUE;
    } else {
        return FALSE;
    }
}
/* returns if the bitmap was already sent lossy to the client. If the bitmap hasn't been sent yet
   to the client, returns false. "area" is for surfaces. If area = NULL,
   all the surface is considered. out_lossy_data will hold info about the bitmap, and its lossy
   area in case it is lossy and part of a surface. */
static int is_bitmap_lossy(RedChannelClient *rcc, SpiceImage *image, SpiceRect *area,
                           Drawable *drawable, BitmapData *out_data)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if (image == NULL) {
        // self bitmap
        out_data->type = BITMAP_DATA_TYPE_BITMAP;
        return FALSE;
    }

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int is_hit_lossy;

        out_data->id = image->descriptor.id;
        if (pixmap_cache_hit(dcc->pixmap_cache, image->descriptor.id,
                             &is_hit_lossy, dcc)) {
            out_data->type = BITMAP_DATA_TYPE_CACHE;
            if (is_hit_lossy) {
                return TRUE;
            } else {
                return FALSE;
            }
        } else {
            out_data->type = BITMAP_DATA_TYPE_BITMAP_TO_CACHE;
        }
    } else {
         out_data->type = BITMAP_DATA_TYPE_BITMAP;
    }

    if (image->descriptor.type != SPICE_IMAGE_TYPE_SURFACE) {
        return FALSE;
    }

    out_data->type = BITMAP_DATA_TYPE_SURFACE;
    out_data->id = image->u.surface.surface_id;

    if (is_surface_area_lossy(dcc, out_data->id,
                              area, &out_data->lossy_rect))
    {
        return TRUE;
    } else {
        return FALSE;
    }
}

static int is_brush_lossy(RedChannelClient *rcc, SpiceBrush *brush,
                          Drawable *drawable, BitmapData *out_data)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        return is_bitmap_lossy(rcc, brush->u.pattern.pat, NULL,
                               drawable, out_data);
    } else {
        out_data->type = BITMAP_DATA_TYPE_INVALID;
        return FALSE;
    }
}

static void surface_lossy_region_update(RedWorker *worker, DisplayChannelClient *dcc,
                                        Drawable *item, int has_mask, int lossy)
{
    QRegion *surface_lossy_region;
    RedDrawable *drawable;

    if (has_mask && !lossy) {
        return;
    }

    surface_lossy_region = &dcc->surface_client_lossy_region[item->surface_id];
    drawable = item->red_drawable;

    if (drawable->clip.type == SPICE_CLIP_TYPE_RECTS ) {
        QRegion clip_rgn;
        QRegion draw_region;
        region_init(&clip_rgn);
        region_init(&draw_region);
        region_add(&draw_region, &drawable->bbox);
        add_clip_rects(&clip_rgn, drawable->clip.rects);
        region_and(&draw_region, &clip_rgn);
        if (lossy) {
            region_or(surface_lossy_region, &draw_region);
        } else {
            region_exclude(surface_lossy_region, &draw_region);
        }

        region_destroy(&clip_rgn);
        region_destroy(&draw_region);
    } else { /* no clip */
        if (!lossy) {
            region_remove(surface_lossy_region, &drawable->bbox);
        } else {
            region_add(surface_lossy_region, &drawable->bbox);
        }
    }
}

static inline int drawable_intersects_with_areas(Drawable *drawable, int surface_ids[],
                                                 SpiceRect *surface_areas[],
                                                 int num_surfaces)
{
    int i;
    for (i = 0; i < num_surfaces; i++) {
        if (surface_ids[i] == drawable->red_drawable->surface_id) {
            if (rect_intersects(surface_areas[i], &drawable->red_drawable->bbox)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static inline int drawable_depends_on_areas(Drawable *drawable,
                                            int surface_ids[],
                                            SpiceRect surface_areas[],
                                            int num_surfaces)
{
    int i;
    RedDrawable *red_drawable;
    int drawable_has_shadow;
    SpiceRect shadow_rect = {0, 0, 0, 0};

    red_drawable = drawable->red_drawable;
    drawable_has_shadow = has_shadow(red_drawable);

    if (drawable_has_shadow) {
       int delta_x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left;
       int delta_y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top;

       shadow_rect.left = red_drawable->u.copy_bits.src_pos.x;
       shadow_rect.top = red_drawable->u.copy_bits.src_pos.y;
       shadow_rect.right = red_drawable->bbox.right + delta_x;
       shadow_rect.bottom = red_drawable->bbox.bottom + delta_y;
    }

    for (i = 0; i < num_surfaces; i++) {
        int x;
        int dep_surface_id;

         for (x = 0; x < 3; ++x) {
            dep_surface_id = drawable->surfaces_dest[x];
            if (dep_surface_id == surface_ids[i]) {
                if (rect_intersects(&surface_areas[i], &red_drawable->surfaces_rects[x])) {
                    return TRUE;
                }
            }
        }

        if (surface_ids[i] == red_drawable->surface_id) {
            if (drawable_has_shadow) {
                if (rect_intersects(&surface_areas[i], &shadow_rect)) {
                    return TRUE;
                }
            }

            // not dependent on dest
            if (red_drawable->effect == QXL_EFFECT_OPAQUE) {
                continue;
            }

            if (rect_intersects(&surface_areas[i], &red_drawable->bbox)) {
                return TRUE;
            }
        }

    }
    return FALSE;
}


static int pipe_rendered_drawables_intersect_with_areas(RedWorker *worker,
                                                        DisplayChannelClient *dcc,
                                                        int surface_ids[],
                                                        SpiceRect *surface_areas[],
                                                        int num_surfaces)
{
    PipeItem *pipe_item;
    Ring *pipe;

    ASSERT(num_surfaces);
    pipe = &dcc->common.base.pipe;

    for (pipe_item = (PipeItem *)ring_get_head(pipe);
         pipe_item;
         pipe_item = (PipeItem *)ring_next(pipe, &pipe_item->link))
    {
        Drawable *drawable;

        if (pipe_item->type != PIPE_ITEM_TYPE_DRAW)
            continue;
        drawable = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item)->drawable;

        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        if (drawable_intersects_with_areas(drawable, surface_ids, surface_areas, num_surfaces)) {
            return TRUE;
        }
    }

    return FALSE;
}

static void red_pipe_replace_rendered_drawables_with_images(RedWorker *worker,
                                                            DisplayChannelClient *dcc,
                                                            int first_surface_id,
                                                            SpiceRect *first_area)
{
    /* TODO: can't have those statics with multiple clients */
    static int resent_surface_ids[MAX_PIPE_SIZE];
    static SpiceRect resent_areas[MAX_PIPE_SIZE]; // not pointers since drawbales may be released
    int num_resent;
    PipeItem *pipe_item;
    Ring *pipe;

    resent_surface_ids[0] = first_surface_id;
    resent_areas[0] = *first_area;
    num_resent = 1;

    pipe = &dcc->common.base.pipe;

    // going from the oldest to the newest
    for (pipe_item = (PipeItem *)ring_get_tail(pipe);
         pipe_item;
         pipe_item = (PipeItem *)ring_prev(pipe, &pipe_item->link)) {
        Drawable *drawable;
        DrawablePipeItem *dpi;
        ImageItem *image;

        if (pipe_item->type != PIPE_ITEM_TYPE_DRAW)
            continue;
        dpi = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item);
        drawable = dpi->drawable;
        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        // When a drawable command, X, depends on bitmaps that were resent,
        // these bitmaps state at the client might not be synchronized with X
        // (i.e., the bitmaps can be more futuristic w.r.t X). Thus, X shouldn't
        // be rendered at the client, and we replace it with an image as well.
        if (!drawable_depends_on_areas(drawable,
                                       resent_surface_ids,
                                       resent_areas,
                                       num_resent)) {
            continue;
        }

        image = red_add_surface_area_image(dcc, drawable->red_drawable->surface_id,
                                           &drawable->red_drawable->bbox, pipe_item, TRUE);
        resent_surface_ids[num_resent] = drawable->red_drawable->surface_id;
        resent_areas[num_resent] = drawable->red_drawable->bbox;
        num_resent++;

        ASSERT(image);
        red_channel_client_pipe_remove_and_release(&dcc->common.base, &dpi->dpi_pipe_item);
        pipe_item = &image->link;
    }
}

static void red_add_lossless_drawable_dependencies(RedWorker *worker,
                                                   RedChannelClient *rcc,
                                                   Drawable *item,
                                                   int deps_surfaces_ids[],
                                                   SpiceRect *deps_areas[],
                                                   int num_deps)
{
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    int sync_rendered = FALSE;
    int i;

    if (!ring_item_is_linked(&item->list_link)) {
        /* drawable was already rendered, we may not be able to retrieve the lossless data
           for the lossy areas */
        sync_rendered = TRUE;

        // checking if the drawable itself or one of the other commands
        // that were rendered, affected the areas that need to be resent
        if (!drawable_intersects_with_areas(item, deps_surfaces_ids,
                                            deps_areas, num_deps)) {
            if (pipe_rendered_drawables_intersect_with_areas(worker, dcc,
                                                             deps_surfaces_ids,
                                                             deps_areas,
                                                             num_deps)) {
                sync_rendered = TRUE;
            }
        } else {
            sync_rendered = TRUE;
        }
    } else {
        sync_rendered = FALSE;
        for (i = 0; i < num_deps; i++) {
            red_update_area_till(worker, deps_areas[i],
                                 deps_surfaces_ids[i], item);
        }
    }

    if (!sync_rendered) {
        // pushing the pipe item back to the pipe
        red_pipe_add_drawable_to_tail(dcc, item);
        // the surfaces areas will be sent as DRAW_COPY commands, that
        // will be executed before the current drawable
        for (i = 0; i < num_deps; i++) {
            red_add_surface_area_image(dcc, deps_surfaces_ids[i], deps_areas[i],
                                       red_pipe_get_tail(dcc), FALSE);

        }
    } else {
        int drawable_surface_id[1];
        SpiceRect *drawable_bbox[1];

        drawable_surface_id[0] = drawable->surface_id;
        drawable_bbox[0] = &drawable->bbox;

        // check if the other rendered images in the pipe have updated the drawable bbox
        if (pipe_rendered_drawables_intersect_with_areas(worker, dcc,
                                                         drawable_surface_id,
                                                         drawable_bbox,
                                                         1)) {
            red_pipe_replace_rendered_drawables_with_images(worker, dcc,
                                                            drawable->surface_id,
                                                            &drawable->bbox);
        }

        red_add_surface_area_image(dcc, drawable->surface_id, &drawable->bbox,
                                   red_pipe_get_tail(dcc), TRUE);
    }
}

static void red_marshall_qxl_draw_fill(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceFill fill;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_FILL, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    fill = drawable->u.fill;
    spice_marshall_Fill(base_marshaller,
                        &fill,
                        &brush_pat_out,
                        &mask_bitmap_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, fill.brush.u.pattern.pat, item, FALSE);
    }

    fill_mask(rcc, mask_bitmap_out, fill.mask.bitmap, item);
}


static void red_lossy_marshall_qxl_draw_fill(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *m,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;

    int dest_allowed_lossy = FALSE;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    uint16_t rop;

    rop = drawable->u.fill.rop_descriptor;

    dest_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                           (rop & SPICE_ROPD_OP_AND) ||
                           (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.fill.brush, item,
                                    &brush_bitmap_data);
    if (!dest_allowed_lossy) {
        dest_is_lossy = is_surface_area_lossy(dcc, item->surface_id, &drawable->bbox,
                                              &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        !(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        int has_mask = !!drawable->u.fill.mask.bitmap;

        red_marshall_qxl_draw_fill(worker, rcc, m, dpi);
        // either the brush operation is opaque, or the dest is not lossy
        surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_opaque(RedWorker *worker,
                                             RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             DrawablePipeItem *dpi, int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceOpaque opaque;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_OPAQUE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    opaque = drawable->u.opaque;
    spice_marshall_Opaque(base_marshaller,
                          &opaque,
                          &src_bitmap_out,
                          &brush_pat_out,
                          &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, opaque.src_bitmap, item,
                              src_allowed_lossy);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, opaque.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(rcc, mask_bitmap_out, opaque.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_opaque(RedWorker *worker,
                                           RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;

    int src_allowed_lossy;
    int rop;
    int src_is_lossy = FALSE;
    int brush_is_lossy = FALSE;
    BitmapData src_bitmap_data;
    BitmapData brush_bitmap_data;

    rop = drawable->u.opaque.rop_descriptor;
    src_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                          (rop & SPICE_ROPD_OP_AND) ||
                          (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.opaque.brush, item,
                                    &brush_bitmap_data);

    if (!src_allowed_lossy) {
        src_is_lossy = is_bitmap_lossy(rcc, drawable->u.opaque.src_bitmap,
                                       &drawable->u.opaque.src_area,
                                       item,
                                       &src_bitmap_data);
    }

    if (!(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) &&
        !(src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        FillBitsType src_send_type;
        int has_mask = !!drawable->u.opaque.mask.bitmap;

        src_send_type = red_marshall_qxl_draw_opaque(worker, rcc, m, dpi, src_allowed_lossy);
        if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
            src_is_lossy = TRUE;
        } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
            src_is_lossy = FALSE;
        }

        surface_lossy_region_update(worker, dcc, item, has_mask, src_is_lossy);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_copy(RedWorker *worker,
                                           RedChannelClient *rcc,
                                           SpiceMarshaller *base_marshaller,
                                           DrawablePipeItem *dpi, int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceCopy copy;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    copy = drawable->u.copy;
    spice_marshall_Copy(base_marshaller,
                        &copy,
                        &src_bitmap_out,
                        &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, copy.src_bitmap, item, src_allowed_lossy);
    fill_mask(rcc, mask_bitmap_out, copy.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_copy(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.copy.mask.bitmap;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.copy.src_bitmap,
                                   &drawable->u.copy.src_area, item, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_copy(worker, rcc, base_marshaller, dpi, TRUE);
    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }
    surface_lossy_region_update(worker, dcc, item, has_mask,
                                src_is_lossy);
}

static void red_marshall_qxl_draw_transparent(RedWorker *worker,
                                          RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceTransparent transparent;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TRANSPARENT,
                                      &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    transparent = drawable->u.transparent;
    spice_marshall_Transparent(base_marshaller,
                               &transparent,
                               &src_bitmap_out);
    fill_bits(dcc, src_bitmap_out, transparent.src_bitmap, item, FALSE);
}

static void red_lossy_marshall_qxl_draw_transparent(RedWorker *worker,
                                                RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.transparent.src_bitmap,
                                   &drawable->u.transparent.src_area, item, &src_bitmap_data);

    if (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) {
        red_marshall_qxl_draw_transparent(worker, rcc, base_marshaller, dpi);
        // don't update surface lossy region since transperent areas might be lossy
    } else {
        int resend_surface_ids[1];
        SpiceRect *resend_areas[1];

        resend_surface_ids[0] = src_bitmap_data.id;
        resend_areas[0] = &src_bitmap_data.lossy_rect;

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, 1);
    }
}

static FillBitsType red_marshall_qxl_draw_alpha_blend(RedWorker *worker,
                                                  RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  DrawablePipeItem *dpi,
                                                  int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceAlphaBlend alpha_blend;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND,
                                      &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    alpha_blend = drawable->u.alpha_blend;
    spice_marshall_AlphaBlend(base_marshaller,
                              &alpha_blend,
                              &src_bitmap_out);
    src_send_type = fill_bits(dcc, src_bitmap_out, alpha_blend.src_bitmap, item,
                              src_allowed_lossy);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_alpha_blend(RedWorker *worker,
                                                RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.alpha_blend.src_bitmap,
                                   &drawable->u.alpha_blend.src_area, item, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_alpha_blend(worker, rcc, base_marshaller, dpi, TRUE);

    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }

    if (src_is_lossy) {
        surface_lossy_region_update(worker, dcc, item, FALSE, src_is_lossy);
    } // else, the area stays lossy/lossless as the destination
}

static void red_marshall_qxl_copy_bits(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpicePoint copy_bits;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_COPY_BITS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    copy_bits = drawable->u.copy_bits.src_pos;
    spice_marshall_Point(base_marshaller,
                         &copy_bits);
}

static void red_lossy_marshall_qxl_copy_bits(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRect src_rect;
    int horz_offset;
    int vert_offset;
    int src_is_lossy;
    SpiceRect src_lossy_area;

    red_marshall_qxl_copy_bits(worker, rcc, base_marshaller, dpi);

    horz_offset = drawable->u.copy_bits.src_pos.x - drawable->bbox.left;
    vert_offset = drawable->u.copy_bits.src_pos.y - drawable->bbox.top;

    src_rect.left = drawable->u.copy_bits.src_pos.x;
    src_rect.top = drawable->u.copy_bits.src_pos.y;
    src_rect.right = drawable->bbox.right + horz_offset;
    src_rect.bottom = drawable->bbox.bottom + vert_offset;

    src_is_lossy = is_surface_area_lossy(dcc, item->surface_id,
                                         &src_rect, &src_lossy_area);

    surface_lossy_region_update(worker, dcc, item, FALSE,
                                src_is_lossy);
}

static void red_marshall_qxl_draw_blend(RedWorker *worker,
                                    RedChannelClient *rcc,
                                    SpiceMarshaller *base_marshaller,
                                    DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlend blend;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLEND, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    blend = drawable->u.blend;
    spice_marshall_Blend(base_marshaller,
                         &blend,
                         &src_bitmap_out,
                         &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, blend.src_bitmap, item, FALSE);

    fill_mask(rcc, mask_bitmap_out, blend.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blend(RedWorker *worker,
                                          RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.blend.src_bitmap,
                                   &drawable->u.blend.src_area, item, &src_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if (!dest_is_lossy &&
        (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_blend(worker, rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_blackness(RedWorker *worker,
                                        RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlackness blackness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLACKNESS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    blackness = drawable->u.blackness;

    spice_marshall_Blackness(base_marshaller,
                             &blackness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, blackness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blackness(RedWorker *worker,
                                              RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.blackness.mask.bitmap;

    red_marshall_qxl_draw_blackness(worker, rcc, base_marshaller, dpi);

    surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_whiteness(RedWorker *worker,
                                        RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceWhiteness whiteness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_WHITENESS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    whiteness = drawable->u.whiteness;

    spice_marshall_Whiteness(base_marshaller,
                             &whiteness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, whiteness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_whiteness(RedWorker *worker,
                                              RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.whiteness.mask.bitmap;

    red_marshall_qxl_draw_whiteness(worker, rcc, base_marshaller, dpi);

    surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_inverse(RedWorker *worker,
                                        RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        Drawable *item)
{
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceInvers inverse;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_INVERS, NULL);
    fill_base(base_marshaller, item);
    inverse = drawable->u.invers;

    spice_marshall_Invers(base_marshaller,
                          &inverse,
                          &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, inverse.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_inverse(RedWorker *worker,
                                            RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            Drawable *item)
{
    red_marshall_qxl_draw_inverse(worker, rcc, base_marshaller, item);
}

static void red_marshall_qxl_draw_rop3(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRop3 rop3;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ROP3, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    rop3 = drawable->u.rop3;
    spice_marshall_Rop3(base_marshaller,
                        &rop3,
                        &src_bitmap_out,
                        &brush_pat_out,
                        &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, rop3.src_bitmap, item, FALSE);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, rop3.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(rcc, mask_bitmap_out, rop3.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_rop3(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.rop3.src_bitmap,
                                   &drawable->u.rop3.src_area, item, &src_bitmap_data);
    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.rop3.brush, item,
                                    &brush_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        int has_mask = !!drawable->u.rop3.mask.bitmap;
        red_marshall_qxl_draw_rop3(worker, rcc, base_marshaller, dpi);
        surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_stroke(RedWorker *worker,
                                     RedChannelClient *rcc,
                                     SpiceMarshaller *base_marshaller,
                                     DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceStroke stroke;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *style_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_STROKE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    stroke = drawable->u.stroke;
    spice_marshall_Stroke(base_marshaller,
                          &stroke,
                          &style_out,
                          &brush_pat_out);

    fill_attr(style_out, &stroke.attr, item->group_id);
    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, stroke.brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_stroke(RedWorker *worker,
                                           RedChannelClient *rcc,
                                           SpiceMarshaller *base_marshaller,
                                           DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop;

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.stroke.brush, item,
                                    &brush_bitmap_data);

    // back_mode is not used at the client. Ignoring.
    rop = drawable->u.stroke.fore_mode;

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.stroke.brush.type != SPICE_BRUSH_TYPE_SOLID &&
        ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR))) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)))
    {
        red_marshall_qxl_draw_stroke(worker, rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        // TODO: use the path in order to resend smaller areas
        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_text(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceText text;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *back_brush_pat_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TEXT, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    text = drawable->u.text;
    spice_marshall_Text(base_marshaller,
                        &text,
                        &brush_pat_out,
                        &back_brush_pat_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, text.fore_brush.u.pattern.pat, item, FALSE);
    }
    if (back_brush_pat_out) {
        fill_bits(dcc, back_brush_pat_out, text.back_brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_text(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int fg_is_lossy;
    BitmapData fg_bitmap_data;
    int bg_is_lossy;
    BitmapData bg_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop = 0;

    fg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.fore_brush, item,
                                 &fg_bitmap_data);
    bg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.back_brush, item,
                                 &bg_bitmap_data);

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.text.fore_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop = drawable->u.text.fore_mode;
    }

    if (drawable->u.text.back_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop |= drawable->u.text.back_mode;
    }

    if ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR)) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!fg_is_lossy || (fg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!bg_is_lossy || (bg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_text(worker, rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (fg_is_lossy && (fg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = fg_bitmap_data.id;
            resend_areas[num_resend] = &fg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (bg_is_lossy && (bg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = bg_bitmap_data.id;
            resend_areas[num_resend] = &bg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }
        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_lossy_marshall_qxl_drawable(RedWorker *worker, RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller, DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    switch (item->red_drawable->type) {
    case QXL_DRAW_FILL:
        red_lossy_marshall_qxl_draw_fill(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_lossy_marshall_qxl_draw_opaque(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COPY:
        red_lossy_marshall_qxl_draw_copy(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_lossy_marshall_qxl_draw_transparent(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_lossy_marshall_qxl_draw_alpha_blend(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_COPY_BITS:
        red_lossy_marshall_qxl_copy_bits(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_lossy_marshall_qxl_draw_blend(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_lossy_marshall_qxl_draw_blackness(worker, rcc, base_marshaller, dpi);
        break;
     case QXL_DRAW_WHITENESS:
        red_lossy_marshall_qxl_draw_whiteness(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_lossy_marshall_qxl_draw_inverse(worker, rcc, base_marshaller, item);
        break;
    case QXL_DRAW_ROP3:
        red_lossy_marshall_qxl_draw_rop3(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_lossy_marshall_qxl_draw_stroke(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_lossy_marshall_qxl_draw_text(worker, rcc, base_marshaller, dpi);
        break;
    default:
        red_error("invalid type");
    }
}

static inline void red_marshall_qxl_drawable(RedWorker *worker, RedChannelClient *rcc,
                                SpiceMarshaller *m, DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    switch (drawable->type) {
    case QXL_DRAW_FILL:
        red_marshall_qxl_draw_fill(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_marshall_qxl_draw_opaque(worker, rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_COPY:
        red_marshall_qxl_draw_copy(worker, rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_marshall_qxl_draw_transparent(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_marshall_qxl_draw_alpha_blend(worker, rcc, m, dpi, FALSE);
        break;
    case QXL_COPY_BITS:
        red_marshall_qxl_copy_bits(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_marshall_qxl_draw_blend(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_marshall_qxl_draw_blackness(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_marshall_qxl_draw_whiteness(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_marshall_qxl_draw_inverse(worker, rcc, m, item);
        break;
    case QXL_DRAW_ROP3:
        red_marshall_qxl_draw_rop3(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_marshall_qxl_draw_stroke(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_marshall_qxl_draw_text(worker, rcc, m, dpi);
        break;
    default:
        red_error("invalid type");
    }
}

static void display_channel_push_release(DisplayChannelClient *dcc, uint8_t type, uint64_t id,
                                         uint64_t* sync_data)
{
    FreeList *free_list = &dcc->send_data.free_list;
    int i;

    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        free_list->sync[i] = MAX(free_list->sync[i], sync_data[i]);
    }

    if (free_list->res->count == free_list->res_size) {
        SpiceResourceList *new_list;
        new_list = spice_malloc(sizeof(*new_list) +
                                free_list->res_size * sizeof(SpiceResourceID) * 2);
        new_list->count = free_list->res->count;
        memcpy(new_list->resources, free_list->res->resources,
               new_list->count * sizeof(SpiceResourceID));
        free(free_list->res);
        free_list->res = new_list;
        free_list->res_size *= 2;
    }
    free_list->res->resources[free_list->res->count].type = type;
    free_list->res->resources[free_list->res->count++].id = id;
}

static inline void display_marshal_sub_msg_inval_list(SpiceMarshaller *m,
                                                       FreeList *free_list)
{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_DISPLAY_INVAL_LIST);
    spice_marshaller_add_uint32(m, sizeof(*free_list->res) +
                                free_list->res->count * sizeof(free_list->res->resources[0]));
    spice_marshall_msg_display_inval_list(m, free_list->res);
}

static inline void display_marshal_sub_msg_inval_list_wait(SpiceMarshaller *m,
                                                            FreeList *free_list)

{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_WAIT_FOR_CHANNELS);
    spice_marshaller_add_uint32(m, sizeof(free_list->wait.header) +
                                free_list->wait.header.wait_count * sizeof(free_list->wait.buf[0]));
    spice_marshall_msg_wait_for_channels(m, &free_list->wait.header);
}

/* use legacy SpiceDataHeader (with sub_list) */
static inline void display_channel_send_free_list_legacy(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;
    SpiceMarshaller *marshaller;
    int sub_list_len = 1;
    SpiceMarshaller *wait_m = NULL;
    SpiceMarshaller *inval_m;
    SpiceMarshaller *sub_list_m;

    marshaller = red_channel_client_get_marshaller(rcc);
    inval_m = spice_marshaller_get_submarshaller(marshaller);

    display_marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(marshaller);
        display_marshal_sub_msg_inval_list_wait(wait_m, free_list);
        sub_list_len++;
    }

    sub_list_m = spice_marshaller_get_submarshaller(marshaller);
    spice_marshaller_add_uint16(sub_list_m, sub_list_len);
    if (wait_m) {
        spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(wait_m));
    }
    spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(inval_m));
    red_channel_client_set_header_sub_list(rcc, spice_marshaller_get_offset(sub_list_m));
}

/* use mini header and SPICE_MSG_LIST */
static inline void display_channel_send_free_list(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;
    int sub_list_len = 1;
    SpiceMarshaller *urgent_marshaller;
    SpiceMarshaller *wait_m = NULL;
    SpiceMarshaller *inval_m;
    uint32_t sub_arr_offset;
    uint32_t wait_offset = 0;
    uint32_t inval_offset = 0;
    int i;

    urgent_marshaller = red_channel_client_switch_to_urgent_sender(rcc);
    for (i = 0; i < dcc->send_data.num_pixmap_cache_items; i++) {
        int dummy;
        /* When using the urgent marshaller, the serial number of the message that is
         * going to be sent right after the SPICE_MSG_LIST, is increased by one.
         * But all this message pixmaps cache references used its old serial.
         * we use pixmap_cache_items to collect these pixmaps, and we update their serial by calling pixmap_cache_hit.*/
        pixmap_cache_hit(dcc->pixmap_cache, dcc->send_data.pixmap_cache_items[i],
                         &dummy, dcc);
    }

    if (free_list->wait.header.wait_count) {
        red_channel_client_init_send_data(rcc, SPICE_MSG_LIST, NULL);
    } else { /* only one message, no need for a list */
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_LIST, NULL);
        spice_marshall_msg_display_inval_list(urgent_marshaller, free_list->res);
        return;
    }

    inval_m = spice_marshaller_get_submarshaller(urgent_marshaller);
    display_marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(urgent_marshaller);
        display_marshal_sub_msg_inval_list_wait(wait_m, free_list);
        sub_list_len++;
    }

    sub_arr_offset = sub_list_len * sizeof(uint32_t);

    spice_marshaller_add_uint16(urgent_marshaller, sub_list_len);
    inval_offset = spice_marshaller_get_offset(inval_m); // calc the offset before
                                                         // adding the sub list
                                                         // offsets array to the marshaller
    /* adding the array of offsets */
    if (wait_m) {
        wait_offset = spice_marshaller_get_offset(wait_m);
        spice_marshaller_add_uint32(urgent_marshaller, wait_offset + sub_arr_offset);
    }
    spice_marshaller_add_uint32(urgent_marshaller, inval_offset + sub_arr_offset);
}

static inline void display_begin_send_message(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;

    if (free_list->res->count) {
        int sync_count = 0;
        int i;

        for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
            if (i != dcc->common.id && free_list->sync[i] != 0) {
                free_list->wait.header.wait_list[sync_count].channel_type = SPICE_CHANNEL_DISPLAY;
                free_list->wait.header.wait_list[sync_count].channel_id = i;
                free_list->wait.header.wait_list[sync_count++].message_serial = free_list->sync[i];
            }
        }
        free_list->wait.header.wait_count = sync_count;

        if (rcc->is_mini_header) {
            display_channel_send_free_list(rcc);
        } else {
            display_channel_send_free_list_legacy(rcc);
        }
    }
    red_channel_client_begin_send_message(rcc);
}

static inline uint8_t *red_get_image_line(RedWorker *worker, SpiceChunks *chunks, size_t *offset,
                                          int *chunk_nr, int stride)
{
    uint8_t *ret;
    SpiceChunk *chunk;

    chunk = &chunks->chunk[*chunk_nr];

    if (*offset == chunk->len) {
        if (*chunk_nr == chunks->num_chunks - 1) {
            return NULL; /* Last chunk */
        }
        *offset = 0;
        (*chunk_nr)++;
        chunk = &chunks->chunk[*chunk_nr];
    }

    if (chunk->len - *offset < stride) {
        red_printf("bad chunk alignment");
        return NULL;
    }
    ret = chunk->data + *offset;
    *offset += stride;
    return ret;
}

static int encode_frame (RedWorker *worker, const SpiceRect *src,
                         const SpiceBitmap *image, Stream *stream)
{
    SpiceChunks *chunks;
    uint32_t image_stride;
    size_t offset;
    int i, chunk;

    chunks = image->data;
    offset = 0;
    chunk = 0;
    image_stride = image->stride;

    const int skip_lines = stream->top_down ? src->top : image->y - (src->bottom - 0);
    for (i = 0; i < skip_lines; i++) {
        red_get_image_line(worker, chunks, &offset, &chunk, image_stride);
    }

    const unsigned int stream_height = src->bottom - src->top;
    const unsigned int stream_width = src->right - src->left;

    for (i = 0; i < stream_height; i++) {
        uint8_t *src_line =
            (uint8_t *)red_get_image_line(worker, chunks, &offset, &chunk, image_stride);

        if (!src_line) {
            return FALSE;
        }

        src_line += src->left * mjpeg_encoder_get_bytes_per_pixel(stream->mjpeg_encoder);
        if (mjpeg_encoder_encode_scanline(stream->mjpeg_encoder, src_line, stream_width) == 0)
            return FALSE;
    }

    return TRUE;
}

static inline int red_marshall_stream_data(RedChannelClient *rcc,
                  SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    Stream *stream = drawable->stream;
    SpiceImage *image;
    RedWorker *worker = dcc->common.worker;
    int n;

    ASSERT(stream);
    ASSERT(drawable->red_drawable->type == QXL_DRAW_COPY);

    worker = display_channel->common.worker;
    image = drawable->red_drawable->u.copy.src_bitmap;

    if (image->descriptor.type != SPICE_IMAGE_TYPE_BITMAP) {
        return FALSE;
    }

    StreamAgent *agent = &dcc->stream_agents[stream - worker->streams_buf];
    uint64_t time_now = red_now();
    size_t outbuf_size;
    if (time_now - agent->last_send_time < (1000 * 1000 * 1000) / agent->fps) {
        agent->frames--;
        return TRUE;
    }

    outbuf_size = dcc->send_data.stream_outbuf_size;
    if (!mjpeg_encoder_start_frame(stream->mjpeg_encoder, image->u.bitmap.format,
                                   &dcc->send_data.stream_outbuf,
                                   &outbuf_size)) {
        return FALSE;
    }
    if (!encode_frame(worker, &drawable->red_drawable->u.copy.src_area,
                      &image->u.bitmap, stream)) {
        return FALSE;
    }
    n = mjpeg_encoder_end_frame(stream->mjpeg_encoder);
    dcc->send_data.stream_outbuf_size = outbuf_size;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA, NULL);

    SpiceMsgDisplayStreamData stream_data;

    stream_data.id = stream - worker->streams_buf;
    stream_data.multi_media_time = drawable->red_drawable->mm_time;
    stream_data.data_size = n;
    spice_marshall_msg_display_stream_data(base_marshaller, &stream_data);
    spice_marshaller_add_ref(base_marshaller,
                             dcc->send_data.stream_outbuf, n);
    agent->last_send_time = time_now;
    return TRUE;
}

static inline void marshall_qxl_drawable(RedChannelClient *rcc,
    SpiceMarshaller *m, DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);

    ASSERT(display_channel && rcc);
    if (item->stream && red_marshall_stream_data(rcc, m, item)) {
        return;
    }
    if (!display_channel->enable_jpeg)
        red_marshall_qxl_drawable(display_channel->common.worker, rcc, m, dpi);
    else
        red_lossy_marshall_qxl_drawable(display_channel->common.worker, rcc, m, dpi);
}

static inline void red_marshall_verb(RedChannelClient *rcc, uint16_t verb)
{
    ASSERT(rcc);
    red_channel_client_init_send_data(rcc, verb, NULL);
}

static inline void red_marshall_inval(RedChannelClient *rcc,
        SpiceMarshaller *base_marshaller, CacheItem *cach_item)
{
    SpiceMsgDisplayInvalOne inval_one;

    red_channel_client_init_send_data(rcc, cach_item->inval_type, NULL);
    inval_one.id = *(uint64_t *)&cach_item->id;

    spice_marshall_msg_cursor_inval_one(base_marshaller, &inval_one);
}

static void display_channel_marshall_migrate(RedChannelClient *rcc,
    SpiceMarshaller *base_marshaller)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    SpiceMsgMigrate migrate;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE, NULL);
    migrate.flags = SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER;
    spice_marshall_msg_migrate(base_marshaller, &migrate);
    display_channel->expect_migrate_mark = TRUE;
}

static void display_channel_marshall_migrate_data(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannelMigrateData display_data;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA, NULL);

    ASSERT(dcc->pixmap_cache);
    display_data.magic = DISPLAY_MIGRATE_DATA_MAGIC;
    ASSERT(MAX_CACHE_CLIENTS == 4); //MIGRATE_DATA_VERSION dependent
    display_data.version = DISPLAY_MIGRATE_DATA_VERSION;

    display_data.message_serial = red_channel_client_get_message_serial(rcc);

    display_data.pixmap_cache_freezer = pixmap_cache_freeze(dcc->pixmap_cache);
    display_data.pixmap_cache_id = dcc->pixmap_cache->id;
    display_data.pixmap_cache_size = dcc->pixmap_cache->size;
    memcpy(display_data.pixmap_cache_clients, dcc->pixmap_cache->sync,
           sizeof(display_data.pixmap_cache_clients));

    ASSERT(dcc->glz_dict);
    red_freeze_glz(dcc);
    display_data.glz_dict_id = dcc->glz_dict->id;
    glz_enc_dictionary_get_restore_data(dcc->glz_dict->dict,
                                        &display_data.glz_dict_restore_data,
                                        &dcc->glz_data.usr);

    spice_marshaller_add_ref(base_marshaller,
                             (uint8_t *)&display_data, sizeof(display_data));
}

static void display_channel_marshall_pixmap_sync(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgWaitForChannels wait;
    PixmapCache *pixmap_cache;

    red_channel_client_init_send_data(rcc, SPICE_MSG_WAIT_FOR_CHANNELS, NULL);
    pixmap_cache = dcc->pixmap_cache;

    pthread_mutex_lock(&pixmap_cache->lock);

    wait.wait_count = 1;
    wait.wait_list[0].channel_type = SPICE_CHANNEL_DISPLAY;
    wait.wait_list[0].channel_id = pixmap_cache->generation_initiator.client;
    wait.wait_list[0].message_serial = pixmap_cache->generation_initiator.message;
    dcc->pixmap_cache_generation = pixmap_cache->generation;
    dcc->pending_pixmaps_sync = FALSE;

    pthread_mutex_unlock(&pixmap_cache->lock);

    spice_marshall_msg_wait_for_channels(base_marshaller, &wait);
}

static void display_channel_marshall_reset_cache(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgWaitForChannels wait;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PIXMAPS, NULL);
    pixmap_cache_reset(dcc->pixmap_cache, dcc, &wait);

    spice_marshall_msg_display_inval_all_pixmaps(base_marshaller,
                                                 &wait);
}

static void red_marshall_image(RedChannelClient *rcc, SpiceMarshaller *m, ImageItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    SpiceImage red_image;
    RedWorker *worker;
    SpiceBitmap bitmap;
    SpiceChunks *chunks;
    QRegion *surface_lossy_region;
    int comp_succeeded;
    int lossy_comp = FALSE;
    int lz_comp = FALSE;
    spice_image_compression_t comp_mode;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    ASSERT(rcc && display_channel && item);
    worker = display_channel->common.worker;

    QXL_SET_IMAGE_ID(&red_image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    red_image.descriptor.flags = item->image_flags;
    red_image.descriptor.width = item->width;
    red_image.descriptor.height = item->height;

    bitmap.format = item->image_format;
    bitmap.flags = 0;
    if (item->top_down) {
        bitmap.flags |= SPICE_BITMAP_FLAGS_TOP_DOWN;
    }
    bitmap.x = item->width;
    bitmap.y = item->height;
    bitmap.stride = item->stride;
    bitmap.palette = 0;
    bitmap.palette_id = 0;

    chunks = spice_chunks_new_linear(item->data, bitmap.stride * bitmap.y);
    bitmap.data = chunks;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &item->link);

    copy.base.surface_id = item->surface_id;
    copy.base.box.left = item->pos.x;
    copy.base.box.top = item->pos.y;
    copy.base.box.right = item->pos.x + bitmap.x;
    copy.base.box.bottom = item->pos.y + bitmap.y;
    copy.base.clip.type = SPICE_CLIP_TYPE_NONE;
    copy.data.rop_descriptor = SPICE_ROPD_OP_PUT;
    copy.data.src_area.left = 0;
    copy.data.src_area.top = 0;
    copy.data.src_area.right = bitmap.x;
    copy.data.src_area.bottom = bitmap.y;
    copy.data.scale_mode = 0;
    copy.data.src_bitmap = 0;
    copy.data.mask.flags = 0;
    copy.data.mask.flags = 0;
    copy.data.mask.pos.x = 0;
    copy.data.mask.pos.y = 0;
    copy.data.mask.bitmap = 0;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    compress_send_data_t comp_send_data = {0};

    comp_mode = display_channel->common.worker->image_compression;

    if ((comp_mode == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
        (comp_mode == SPICE_IMAGE_COMPRESS_AUTO_GLZ)) {
        if (BITMAP_FMT_IS_RGB[item->image_format]) {
            if (!_stride_is_extra(&bitmap)) {
                BitmapGradualType grad_level;
                grad_level = _get_bitmap_graduality_level(display_channel->common.worker,
                                                          &bitmap,
                                                          worker->mem_slots.internal_groupslot_id);
                if (grad_level == BITMAP_GRADUAL_HIGH) {
                    // if we use lz for alpha, the stride can't be extra
                    lossy_comp = display_channel->enable_jpeg && item->can_lossy;
                } else {
                    lz_comp = TRUE;
                }
            }
        } else {
            lz_comp = TRUE;
        }
    }

    if (lossy_comp) {
        comp_succeeded = red_jpeg_compress_image(dcc, &red_image,
                                                 &bitmap, &comp_send_data,
                                                 worker->mem_slots.internal_groupslot_id);
    } else {
        if (!lz_comp) {
            comp_succeeded = red_quic_compress_image(dcc, &red_image, &bitmap,
                                                     &comp_send_data,
                                                     worker->mem_slots.internal_groupslot_id);
        } else {
            comp_succeeded = red_lz_compress_image(dcc, &red_image, &bitmap,
                                                   &comp_send_data,
                                                   worker->mem_slots.internal_groupslot_id);
        }
    }

    surface_lossy_region = &dcc->surface_client_lossy_region[item->surface_id];
    if (comp_succeeded) {
        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);

        marshaller_add_compressed(src_bitmap_out,
                                  comp_send_data.comp_buf, comp_send_data.comp_buf_size);

        if (lzplt_palette_out && comp_send_data.lzplt_palette) {
            spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
        }

        if (lossy_comp) {
            region_add(surface_lossy_region, &copy.base.box);
        } else {
            region_remove(surface_lossy_region, &copy.base.box);
        }
    } else {
        red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
        red_image.u.bitmap = bitmap;

        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_marshaller_add_ref(src_bitmap_out, item->data,
                                 bitmap.y * bitmap.stride);
        region_remove(surface_lossy_region, &copy.base.box);
    }
    spice_chunks_destroy(chunks);
}

static void red_display_marshall_upgrade(RedChannelClient *rcc, SpiceMarshaller *m,
                                         UpgradeItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *red_drawable;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;

    ASSERT(rcc && rcc->channel && item && item->drawable);

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &item->base);

    red_drawable = item->drawable->red_drawable;
    ASSERT(red_drawable->type == QXL_DRAW_COPY);
    ASSERT(red_drawable->u.copy.rop_descriptor == SPICE_ROPD_OP_PUT);
    ASSERT(red_drawable->u.copy.mask.bitmap == 0);

    copy.base.surface_id = 0;
    copy.base.box = red_drawable->bbox;
    copy.base.clip.type = SPICE_CLIP_TYPE_RECTS;
    copy.base.clip.rects = item->rects;
    copy.data = red_drawable->u.copy;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, copy.data.src_bitmap, item->drawable, FALSE);
}

static void red_display_marshall_stream_start(RedChannelClient *rcc,
                     SpiceMarshaller *base_marshaller, StreamAgent *agent)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Stream *stream = agent->stream;

    agent->last_send_time = 0;
    ASSERT(stream);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CREATE, &agent->create_item);
    SpiceMsgDisplayStreamCreate stream_create;
    SpiceClipRects clip_rects;

    stream_create.surface_id = 0;
    stream_create.id = agent - dcc->stream_agents;
    stream_create.flags = stream->top_down ? SPICE_STREAM_FLAGS_TOP_DOWN : 0;
    stream_create.codec_type = SPICE_VIDEO_CODEC_TYPE_MJPEG;

    stream_create.src_width = stream->width;
    stream_create.src_height = stream->height;
    stream_create.stream_width = stream_create.src_width;
    stream_create.stream_height = stream_create.src_height;
    stream_create.dest = stream->dest_area;

    if (stream->current) {
        RedDrawable *red_drawable = stream->current->red_drawable;
        stream_create.clip = red_drawable->clip;
    } else {
        stream_create.clip.type = SPICE_CLIP_TYPE_RECTS;
        clip_rects.num_rects = 0;
        stream_create.clip.rects = &clip_rects;
    }

    stream_create.stamp = 0;

    spice_marshall_msg_display_stream_create(base_marshaller, &stream_create);
}

static void red_display_marshall_stream_clip(RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         StreamClipItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    StreamAgent *agent = item->stream_agent;
    Stream *stream = agent->stream;

    ASSERT(stream);

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CLIP, &item->base);
    SpiceMsgDisplayStreamClip stream_clip;

    stream_clip.id = agent - dcc->stream_agents;
    stream_clip.clip.type = item->clip_type;
    stream_clip.clip.rects = item->rects;

    spice_marshall_msg_display_stream_clip(base_marshaller, &stream_clip);
}

static void red_display_marshall_stream_end(RedChannelClient *rcc,
                   SpiceMarshaller *base_marshaller, StreamAgent* agent)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgDisplayStreamDestroy destroy;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DESTROY, NULL);
    destroy.id = agent - dcc->stream_agents;

    spice_marshall_msg_display_stream_destroy(base_marshaller, &destroy);
}

static void red_cursor_marshall_inval(RedChannelClient *rcc,
                SpiceMarshaller *m, CacheItem *cach_item)
{
    ASSERT(rcc);
    red_marshall_inval(rcc, m, cach_item);
}

static void red_marshall_cursor_init(RedChannelClient *rcc, SpiceMarshaller *base_marshaller,
                                     PipeItem *pipe_item)
{
    CursorChannel *cursor_channel;
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);
    RedWorker *worker;
    SpiceMsgCursorInit msg;
    AddBufInfo info;

    ASSERT(rcc);
    cursor_channel = SPICE_CONTAINEROF(rcc->channel, CursorChannel, common.base);
    worker = cursor_channel->common.worker;

    red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_INIT, NULL);
    msg.visible = worker->cursor_visible;
    msg.position = worker->cursor_position;
    msg.trail_length = worker->cursor_trail_length;
    msg.trail_frequency = worker->cursor_trail_frequency;

    fill_cursor(ccc, &msg.cursor, worker->cursor, &info);
    spice_marshall_msg_cursor_init(base_marshaller, &msg);
    add_buf_from_info(base_marshaller, &info);
}

static void cursor_channel_marshall_migrate(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller)
{
    SpiceMsgMigrate migrate;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE, NULL);
    migrate.flags = 0;

    spice_marshall_msg_migrate(base_marshaller, &migrate);
}

static void red_marshall_cursor(RedChannelClient *rcc,
                   SpiceMarshaller *m, CursorPipeItem *cursor_pipe_item)
{
    CursorChannel *cursor_channel = SPICE_CONTAINEROF(rcc->channel, CursorChannel, common.base);
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);
    CursorItem *cursor = cursor_pipe_item->cursor_item;
    PipeItem *pipe_item = &cursor_pipe_item->base;
    RedCursorCmd *cmd;
    RedWorker *worker;

    ASSERT(cursor_channel);

    worker = cursor_channel->common.worker;

    cmd = cursor->red_cursor;
    switch (cmd->type) {
    case QXL_CURSOR_MOVE:
        {
            SpiceMsgCursorMove cursor_move;
            red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_MOVE, pipe_item);
            cursor_move.position = cmd->u.position;
            spice_marshall_msg_cursor_move(m, &cursor_move);
            break;
        }
    case QXL_CURSOR_SET:
        {
            SpiceMsgCursorSet cursor_set;
            AddBufInfo info;

            red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_SET, pipe_item);
            cursor_set.position = cmd->u.set.position;
            cursor_set.visible = worker->cursor_visible;

            fill_cursor(ccc, &cursor_set.cursor, cursor, &info);
            spice_marshall_msg_cursor_set(m, &cursor_set);
            add_buf_from_info(m, &info);
            break;
        }
    case QXL_CURSOR_HIDE:
        red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_HIDE, pipe_item);
        break;
    case QXL_CURSOR_TRAIL:
        {
            SpiceMsgCursorTrail cursor_trail;

            red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_TRAIL, pipe_item);
            cursor_trail.length = cmd->u.trail.length;
            cursor_trail.frequency = cmd->u.trail.frequency;
            spice_marshall_msg_cursor_trail(m, &cursor_trail);
        }
        break;
    default:
        red_error("bad cursor command %d", cmd->type);
    }
}

static void red_marshall_surface_create(RedChannelClient *rcc,
    SpiceMarshaller *base_marshaller, SpiceMsgSurfaceCreate *surface_create)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    region_init(&dcc->surface_client_lossy_region[surface_create->surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_CREATE, NULL);

    spice_marshall_msg_display_surface_create(base_marshaller, surface_create);
}

static void red_marshall_surface_destroy(RedChannelClient *rcc,
       SpiceMarshaller *base_marshaller, uint32_t surface_id)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgSurfaceDestroy surface_destroy;

    region_destroy(&dcc->surface_client_lossy_region[surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_DESTROY, NULL);

    surface_destroy.surface_id = surface_id;

    spice_marshall_msg_display_surface_destroy(base_marshaller, &surface_destroy);
}

static void display_channel_send_item(RedChannelClient *rcc, PipeItem *pipe_item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    red_display_reset_send_data(dcc);
    switch (pipe_item->type) {
    case PIPE_ITEM_TYPE_DRAW: {
        DrawablePipeItem *dpi = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item);
        marshall_qxl_drawable(rcc, m, dpi);
        break;
    }
    case PIPE_ITEM_TYPE_INVAL_ONE:
        red_marshall_inval(rcc, m, (CacheItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *agent = SPICE_CONTAINEROF(pipe_item, StreamAgent, create_item);
        red_display_marshall_stream_start(rcc, m, agent);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP: {
        StreamClipItem* clip_item = (StreamClipItem *)pipe_item;
        red_display_marshall_stream_clip(rcc, m, clip_item);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_DESTROY: {
        StreamAgent *agent = SPICE_CONTAINEROF(pipe_item, StreamAgent, destroy_item);
        red_display_marshall_stream_end(rcc, m, agent);
        break;
    }
    case PIPE_ITEM_TYPE_UPGRADE:
        red_display_marshall_upgrade(rcc, m, (UpgradeItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        red_marshall_verb(rcc, ((VerbItem*)pipe_item)->verb);
        break;
    case PIPE_ITEM_TYPE_MIGRATE:
        red_printf("PIPE_ITEM_TYPE_MIGRATE");
        display_channel_marshall_migrate(rcc, m);
        break;
    case PIPE_ITEM_TYPE_MIGRATE_DATA:
        display_channel_marshall_migrate_data(rcc, m);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        red_marshall_image(rcc, m, (ImageItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_PIXMAP_SYNC:
        display_channel_marshall_pixmap_sync(rcc, m);
        break;
    case PIPE_ITEM_TYPE_PIXMAP_RESET:
        display_channel_marshall_reset_cache(rcc, m);
        break;
    case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
        red_reset_palette_cache(dcc);
        red_marshall_verb(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES);
        break;
    case PIPE_ITEM_TYPE_CREATE_SURFACE: {
        SurfaceCreateItem *surface_create = SPICE_CONTAINEROF(pipe_item, SurfaceCreateItem,
                                                              pipe_item);
        red_marshall_surface_create(rcc, m, &surface_create->surface_create);
        break;
    }
    case PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        SurfaceDestroyItem *surface_destroy = SPICE_CONTAINEROF(pipe_item, SurfaceDestroyItem,
                                                                pipe_item);
        red_marshall_surface_destroy(rcc, m, surface_destroy->surface_destroy.surface_id);
        break;
    }
    default:
        red_error("invalid pipe item type");
    }

    display_channel_client_release_item_before_push(dcc, pipe_item);

    // a message is pending
    if (red_channel_client_send_message_pending(rcc)) {
        display_begin_send_message(rcc);
    }
}

static void cursor_channel_send_item(RedChannelClient *rcc, PipeItem *pipe_item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);

    switch (pipe_item->type) {
    case PIPE_ITEM_TYPE_CURSOR:
        red_marshall_cursor(rcc, m, SPICE_CONTAINEROF(pipe_item, CursorPipeItem, base));
        break;
    case PIPE_ITEM_TYPE_INVAL_ONE:
        red_cursor_marshall_inval(rcc, m, (CacheItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        red_marshall_verb(rcc, ((VerbItem*)pipe_item)->verb);
        break;
    case PIPE_ITEM_TYPE_MIGRATE:
        red_printf("PIPE_ITEM_TYPE_MIGRATE");
        cursor_channel_marshall_migrate(rcc, m);
        break;
    case PIPE_ITEM_TYPE_CURSOR_INIT:
        red_reset_cursor_cache(rcc);
        red_marshall_cursor_init(rcc, m, pipe_item);
        break;
    case PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE:
        red_reset_cursor_cache(rcc);
        red_marshall_verb(rcc, SPICE_MSG_CURSOR_INVAL_ALL);
        break;
    default:
        red_error("invalid pipe item type");
    }

    cursor_channel_client_release_item_before_push(ccc, pipe_item);
    red_channel_client_begin_send_message(rcc);
}

static inline void red_push(RedWorker *worker)
{
    if (worker->cursor_channel) {
        red_channel_push(&worker->cursor_channel->common.base);
    }
    if (worker->display_channel) {
        red_channel_push(&worker->display_channel->common.base);
    }
}

typedef struct ShowTreeData {
    RedWorker *worker;
    int level;
    Container *container;
} ShowTreeData;

static void __show_tree_call(TreeItem *item, void *data)
{
    ShowTreeData *tree_data = data;
    const char *item_prefix = "|--";
    int i;

    while (tree_data->container != item->container) {
        ASSERT(tree_data->container);
        tree_data->level--;
        tree_data->container = tree_data->container->base.container;
    }

    switch (item->type) {
    case TREE_ITEM_TYPE_DRAWABLE: {
        Drawable *drawable = SPICE_CONTAINEROF(item, Drawable, tree_item);
        const int max_indent = 200;
        char indent_str[max_indent + 1];
        int indent_str_len;

        for (i = 0; i < tree_data->level; i++) {
            printf("  ");
        }
        printf(item_prefix, 0);
        show_red_drawable(tree_data->worker, drawable->red_drawable, NULL);
        for (i = 0; i < tree_data->level; i++) {
            printf("  ");
        }
        printf("|  ");
        show_draw_item(tree_data->worker, &drawable->tree_item, NULL);
        indent_str_len = MIN(max_indent, strlen(item_prefix) + tree_data->level * 2);
        memset(indent_str, ' ', indent_str_len);
        indent_str[indent_str_len] = 0;
        region_dump(&item->rgn, indent_str);
        printf("\n");
        break;
    }
    case TREE_ITEM_TYPE_CONTAINER:
        tree_data->level++;
        tree_data->container = (Container *)item;
        break;
    case TREE_ITEM_TYPE_SHADOW:
        break;
    }
}

void red_show_tree(RedWorker *worker)
{
    int x;

    ShowTreeData show_tree_data;
    show_tree_data.worker = worker;
    show_tree_data.level = 0;
    show_tree_data.container = NULL;
    for (x = 0; x < NUM_SURFACES; ++x) {
        if (worker->surfaces[x].context.canvas) {
            current_tree_for_each(&worker->surfaces[x].current, __show_tree_call,
                                  &show_tree_data);
        }
    }
}

// TODO: move to red_channel
static void red_disconnect_channel(RedChannel *channel)
{
    red_channel_disconnect(channel);
}

static void display_channel_client_disconnect(RedChannelClient *rcc)
{
    red_channel_client_disconnect(rcc);
}

static void display_channel_client_on_disconnect(RedChannelClient *rcc)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    CommonChannel *common;
    RedWorker *worker;

    if (!rcc) {
        return;
    }
    red_printf("");
    common = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base);
    worker = common->worker;
    display_channel = (DisplayChannel *)rcc->channel;
    ASSERT(display_channel == worker->display_channel);
#ifdef COMPRESS_STAT
    print_compress_stats(display_channel);
#endif
    red_release_pixmap_cache(dcc);
    red_release_glz(dcc);
    red_reset_palette_cache(dcc);
    free(dcc->send_data.stream_outbuf);
    red_display_reset_compress_buf(dcc);
    free(dcc->send_data.free_list.res);
    red_display_destroy_streams(dcc);

    // this was the last channel client
    if (!red_channel_is_connected(rcc->channel)) {
        red_display_destroy_compress_bufs(display_channel);
    }
    red_printf_debug(3, "WORKER", "#draw=%d, #red_draw=%d, #glz_draw=%d",
                     worker->drawable_count, worker->red_drawable_count,
                     worker->glz_drawable_count);
}

void red_disconnect_all_display_TODO_remove_me(RedChannel *channel)
{
    // TODO: we need to record the client that actually causes the timeout. So
    // we need to check the locations of the various pipe heads when counting,
    // and disconnect only those/that.
    if (!channel) {
        return;
    }
    red_channel_apply_clients(channel, display_channel_client_disconnect);
}

static void red_migrate_display(RedWorker *worker, RedChannelClient *rcc)
{
    // TODO: replace all worker->display_channel tests with
    // is_connected
    if (red_channel_client_is_connected(rcc)) {
        red_pipe_add_verb(rcc, PIPE_ITEM_TYPE_MIGRATE);
//        red_pipes_add_verb(&worker->display_channel->common.base,
//                           SPICE_MSG_DISPLAY_STREAM_DESTROY_ALL);
//        red_channel_pipes_add_type(&worker->display_channel->common.base,
//                                   PIPE_ITEM_TYPE_MIGRATE);
    }
}

#ifdef USE_OPENGL
static SpiceCanvas *create_ogl_context_common(RedWorker *worker, OGLCtx *ctx, uint32_t width,
                                              uint32_t height, int32_t stride, uint8_t depth)
{
    SpiceCanvas *canvas;

    oglctx_make_current(ctx);
    if (!(canvas = gl_canvas_create(width, height, depth, &worker->image_cache.base,
                                    &worker->image_surfaces, NULL, NULL, NULL))) {
        return NULL;
    }

    spice_canvas_set_usr_data(canvas, ctx, (spice_destroy_fn_t)oglctx_destroy);

    canvas->ops->clear(canvas);

    return canvas;
}

static SpiceCanvas *create_ogl_pbuf_context(RedWorker *worker, uint32_t width, uint32_t height,
                                         int32_t stride, uint8_t depth)
{
    OGLCtx *ctx;
    SpiceCanvas *canvas;

    if (!(ctx = pbuf_create(width, height))) {
        return NULL;
    }

    if (!(canvas = create_ogl_context_common(worker, ctx, width, height, stride, depth))) {
        oglctx_destroy(ctx);
        return NULL;
    }

    return canvas;
}

static SpiceCanvas *create_ogl_pixmap_context(RedWorker *worker, uint32_t width, uint32_t height,
                                              int32_t stride, uint8_t depth) {
    OGLCtx *ctx;
    SpiceCanvas *canvas;

    if (!(ctx = pixmap_create(width, height))) {
        return NULL;
    }

    if (!(canvas = create_ogl_context_common(worker, ctx, width, height, stride, depth))) {
        oglctx_destroy(ctx);
        return NULL;
    }

    return canvas;
}
#endif

static inline void *create_canvas_for_surface(RedWorker *worker, RedSurface *surface,
                                              uint32_t renderer, uint32_t width, uint32_t height,
                                              int32_t stride, uint32_t format, void *line_0)
{
    SpiceCanvas *canvas;

    switch (renderer) {
    case RED_RENDERER_SW:
        canvas = canvas_create_for_data(width, height, format,
                                        line_0, stride,
                                        &worker->image_cache.base,
                                        &worker->image_surfaces, NULL, NULL, NULL);
        surface->context.top_down = TRUE;
        surface->context.canvas_draws_on_surface = TRUE;
        return canvas;
#ifdef USE_OPENGL
    case RED_RENDERER_OGL_PBUF:
        canvas = create_ogl_pbuf_context(worker, width, height, stride,
                                         SPICE_SURFACE_FMT_DEPTH(format));
        surface->context.top_down = FALSE;
        return canvas;
    case RED_RENDERER_OGL_PIXMAP:
        canvas = create_ogl_pixmap_context(worker, width, height, stride,
                                           SPICE_SURFACE_FMT_DEPTH(format));
        surface->context.top_down = FALSE;
        return canvas;
#endif
    default:
        red_error("invalid renderer type");
    };
}

static SurfaceCreateItem *get_surface_create_item(
    RedChannel* channel,
    uint32_t surface_id, uint32_t width,
    uint32_t height, uint32_t format, uint32_t flags)
{
    SurfaceCreateItem *create;

    create = (SurfaceCreateItem *)malloc(sizeof(SurfaceCreateItem));
    PANIC_ON(!create);

    create->surface_create.surface_id = surface_id;
    create->surface_create.width = width;
    create->surface_create.height = height;
    create->surface_create.flags = flags;
    create->surface_create.format = format;

    red_channel_pipe_item_init(channel,
            &create->pipe_item, PIPE_ITEM_TYPE_CREATE_SURFACE);
    return create;
}

static inline void red_create_surface_item(DisplayChannelClient *dcc, int surface_id)
{
    RedSurface *surface;
    SurfaceCreateItem *create;
    RedWorker *worker = dcc ? dcc->common.worker : NULL;
    uint32_t flags = is_primary_surface(worker, surface_id) ? SPICE_SURFACE_FLAGS_PRIMARY : 0;

    /* don't send redundant create surface commands to client */
    if (!dcc || dcc->surface_client_created[surface_id]) {
        return;
    }
    surface = &worker->surfaces[surface_id];
    create = get_surface_create_item(dcc->common.base.channel,
            surface_id, surface->context.width, surface->context.height,
                                     surface->context.format, flags);
    dcc->surface_client_created[surface_id] = TRUE;
    red_channel_client_pipe_add(&dcc->common.base, &create->pipe_item);
}

static void red_worker_create_surface_item(RedWorker *worker, int surface_id)
{
    DisplayChannelClient *dcc;
    RingItem *item;

    WORKER_FOREACH_DCC(worker, item, dcc) {
        red_create_surface_item(dcc, surface_id);
    }
}


static void red_worker_push_surface_image(RedWorker *worker, int surface_id)
{
    DisplayChannelClient *dcc;
    RingItem *item;

    WORKER_FOREACH_DCC(worker, item, dcc) {
        red_push_surface_image(dcc, surface_id);
    }
}

static inline void red_create_surface(RedWorker *worker, uint32_t surface_id, uint32_t width,
                                      uint32_t height, int32_t stride, uint32_t format,
                                      void *line_0, int data_is_valid, int send_client)
{
    RedSurface *surface = &worker->surfaces[surface_id];
    uint32_t i;

    if (stride >= 0) {
        PANIC("Untested path stride >= 0");
    }
    PANIC_ON(surface->context.canvas);

    surface->context.canvas_draws_on_surface = FALSE;
    surface->context.width = width;
    surface->context.height = height;
    surface->context.format = format;
    surface->context.stride = stride;
    surface->context.line_0 = line_0;
    if (!data_is_valid) {
        memset((char *)line_0 + (int32_t)(stride * (height - 1)), 0, height*abs(stride));
    }
    surface->create.info = NULL;
    surface->destroy.info = NULL;
    ring_init(&surface->current);
    ring_init(&surface->current_list);
    ring_init(&surface->depend_on_me);
    region_init(&surface->draw_dirty_region);
    surface->refs = 1;
    if (worker->renderer != RED_RENDERER_INVALID) {
        surface->context.canvas = create_canvas_for_surface(worker, surface, worker->renderer,
                                                            width, height, stride,
                                                            surface->context.format, line_0);
        if (!surface->context.canvas) {
            PANIC("drawing canvas creating failed - can`t create same type canvas");
        }

        if (send_client) {
            red_worker_create_surface_item(worker, surface_id);
            if (data_is_valid) {
                red_worker_push_surface_image(worker, surface_id);
            }
        }
        return;
    }

    for (i = 0; i < worker->num_renderers; i++) {
        surface->context.canvas = create_canvas_for_surface(worker, surface, worker->renderers[i],
                                                            width, height, stride,
                                                            surface->context.format, line_0);
        if (surface->context.canvas) { //no need canvas check
            worker->renderer = worker->renderers[i];
            if (send_client) {
                red_worker_create_surface_item(worker, surface_id);
                if (data_is_valid) {
                    red_worker_push_surface_image(worker, surface_id);
                }
            }
            return;
        }
    }

    PANIC("unable to create drawing canvas");
}

static inline void flush_display_commands(RedWorker *worker)
{
    RedChannel *display_red_channel = &worker->display_channel->common.base;

    for (;;) {
        uint64_t end_time;
        int ring_is_empty;

        red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(&worker->display_channel->common.base);
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_now() + DISPLAY_CLIENT_TIMEOUT * 10;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(&worker->display_channel->common.base);
            if (!display_is_connected(worker) ||
                red_channel_max_pipe_size(display_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->display_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            // TODO: MC: the whole timeout will break since it takes lowest timeout, should
            // do it client by client.
            if (red_now() >= end_time) {
                red_printf("update timeout");
                red_disconnect_all_display_TODO_remove_me(channel);
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

static inline void flush_cursor_commands(RedWorker *worker)
{
    RedChannel *cursor_red_channel = &worker->cursor_channel->common.base;

    for (;;) {
        uint64_t end_time;
        int ring_is_empty = FALSE;

        red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(&worker->cursor_channel->common.base);
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_now() + DISPLAY_CLIENT_TIMEOUT * 10;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(&worker->cursor_channel->common.base);
            if (!cursor_is_connected(worker)
                || red_channel_min_pipe_size(cursor_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->cursor_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            if (red_now() >= end_time) {
                red_printf("flush cursor timeout");
                red_disconnect_cursor(channel);
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

// TODO: on timeout, don't disconnect all channeld immeduiatly - try to disconnect the slowest ones
// first and maybe turn timeouts to several timeouts in order to disconnect channels gradually.
// Should use disconnect or shutdown?
static inline void flush_all_qxl_commands(RedWorker *worker)
{
    flush_display_commands(worker);
    flush_cursor_commands(worker);
}

static void push_new_primary_surface(DisplayChannelClient *dcc)
{
    RedWorker *worker = DCC_TO_WORKER(dcc);
    RedChannelClient *rcc = &dcc->common.base;

    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
    if (!worker->display_channel->common.base.migrate) {
        red_create_surface_item(dcc, 0);
    }
    red_channel_client_push(rcc);
}

/* TODO: this function is evil^Wsynchronous, fix */
static int display_channel_client_wait_for_init(DisplayChannelClient *dcc)
{
    dcc->expect_init = TRUE;
    uint64_t end_time = red_now() + DISPLAY_CLIENT_TIMEOUT;
    for (;;) {
        red_channel_client_receive(&dcc->common.base);
        if (!red_channel_client_is_connected(&dcc->common.base)) {
            break;
        }
        if (dcc->pixmap_cache && dcc->glz_dict) {
            dcc->pixmap_cache_generation = dcc->pixmap_cache->generation;
            /* TODO: move common.id? if it's used for a per client structure.. */
            red_printf("creating encoder with id == %d", dcc->common.id);
            dcc->glz = glz_encoder_create(dcc->common.id, dcc->glz_dict->dict, &dcc->glz_data.usr);
            if (!dcc->glz) {
                PANIC("create global lz failed");
            }
            return TRUE;
        }
        if (red_now() > end_time) {
            red_printf("timeout");
            display_channel_client_disconnect(&dcc->common.base);
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    return FALSE;
}

static void on_new_display_channel_client(DisplayChannelClient *dcc)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    RedChannelClient *rcc = &dcc->common.base;

    red_channel_push_set_ack(&display_channel->common.base);

    if (display_channel->common.base.migrate) {
        display_channel->expect_migrate_data = TRUE;
        return;
    }

    if (!display_channel_client_wait_for_init(dcc)) {
        return;
    }
    red_channel_client_ack_zero_messages_window(&dcc->common.base);
    red_channel_client_push_set_ack(&dcc->common.base);
    if (worker->surfaces[0].context.canvas) {
        red_current_flush(worker, 0);
        push_new_primary_surface(dcc);
        red_push_surface_image(dcc, 0);
        red_pipe_add_verb(rcc, SPICE_MSG_DISPLAY_MARK);
        red_disply_start_streams(dcc);
    }
}

static GlzSharedDictionary *_red_find_glz_dictionary(RedClient *client, uint8_t dict_id)
{
    RingItem *now;
    GlzSharedDictionary *ret = NULL;

    now = &glz_dictionary_list;
    while ((now = ring_next(&glz_dictionary_list, now))) {
        GlzSharedDictionary *dict = (GlzSharedDictionary *)now;
        if ((dict->client == client) && (dict->id == dict_id)) {
            ret = dict;
            break;
        }
    }

    return ret;
}

static GlzSharedDictionary *_red_create_glz_dictionary(RedClient *client, uint8_t id,
                                                       GlzEncDictContext *opaque_dict)
{
    GlzSharedDictionary *shared_dict = spice_new0(GlzSharedDictionary, 1);
    shared_dict->dict = opaque_dict;
    shared_dict->id = id;
    shared_dict->refs = 1;
    shared_dict->migrate_freeze = FALSE;
    shared_dict->client = client;
    ring_item_init(&shared_dict->base);
    pthread_rwlock_init(&shared_dict->encode_lock, NULL);
    return shared_dict;
}

static GlzSharedDictionary *red_create_glz_dictionary(DisplayChannelClient *dcc,
                                                      uint8_t id, int window_size)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_create(window_size,
                                                            MAX_LZ_ENCODERS,
                                                            &dcc->glz_data.usr);
#ifdef COMPRESS_DEBUG
    red_printf("Lz Window %d Size=%d", id, window_size);
#endif
    if (!glz_dict) {
        PANIC("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(dcc->common.base.client, id, glz_dict);
}

static GlzSharedDictionary *red_create_restored_glz_dictionary(DisplayChannelClient *dcc,
                                                               uint8_t id,
                                                               GlzEncDictRestoreData *restore_data)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_restore(restore_data,
                                                             &dcc->glz_data.usr);
    if (!glz_dict) {
        PANIC("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(dcc->common.base.client, id, glz_dict);
}

static GlzSharedDictionary *red_get_glz_dictionary(DisplayChannelClient *dcc,
                                                   uint8_t id, int window_size)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(dcc->common.base.client, id);

    if (!shared_dict) {
        shared_dict = red_create_glz_dictionary(dcc, id, window_size);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static GlzSharedDictionary *red_restore_glz_dictionary(DisplayChannelClient *dcc,
                                                       uint8_t id,
                                                       GlzEncDictRestoreData *restore_data)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(dcc->common.base.client, id);

    if (!shared_dict) {
        shared_dict = red_create_restored_glz_dictionary(dcc, id, restore_data);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static void red_freeze_glz(DisplayChannelClient *dcc)
{
    pthread_rwlock_wrlock(&dcc->glz_dict->encode_lock);
    if (!dcc->glz_dict->migrate_freeze) {
        dcc->glz_dict->migrate_freeze = TRUE;
    }
    pthread_rwlock_unlock(&dcc->glz_dict->encode_lock);
}

/* destroy encoder, and dictionary if no one uses it*/
static void red_release_glz(DisplayChannelClient *dcc)
{
    GlzSharedDictionary *shared_dict;

    red_display_client_clear_glz_drawables(dcc);

    glz_encoder_destroy(dcc->glz);
    dcc->glz = NULL;

    if (!(shared_dict = dcc->glz_dict)) {
        return;
    }

    dcc->glz_dict = NULL;
    pthread_mutex_lock(&glz_dictionary_list_lock);
    if (--shared_dict->refs) {
        pthread_mutex_unlock(&glz_dictionary_list_lock);
        return;
    }
    ring_remove(&shared_dict->base);
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    glz_enc_dictionary_destroy(shared_dict->dict, &dcc->glz_data.usr);
    free(shared_dict);
}

static PixmapCache *red_create_pixmap_cache(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *cache = spice_new0(PixmapCache, 1);
    ring_item_init(&cache->base);
    pthread_mutex_init(&cache->lock, NULL);
    cache->id = id;
    cache->refs = 1;
    ring_init(&cache->lru);
    cache->available = size;
    cache->size = size;
    cache->client = client;
    return cache;
}

static PixmapCache *red_get_pixmap_cache(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *ret = NULL;
    RingItem *now;
    pthread_mutex_lock(&cache_lock);

    now = &pixmap_cache_list;
    while ((now = ring_next(&pixmap_cache_list, now))) {
        PixmapCache *cache = (PixmapCache *)now;
        if ((cache->client == client) && (cache->id == id)) {
            ret = cache;
            ret->refs++;
            break;
        }
    }
    if (!ret) {
        ret = red_create_pixmap_cache(client, id, size);
        ring_add(&pixmap_cache_list, &ret->base);
    }
    pthread_mutex_unlock(&cache_lock);
    return ret;
}

static void red_release_pixmap_cache(DisplayChannelClient *dcc)
{
    PixmapCache *cache;
    if (!(cache = dcc->pixmap_cache)) {
        return;
    }
    dcc->pixmap_cache = NULL;
    pthread_mutex_lock(&cache_lock);
    if (--cache->refs) {
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    ring_remove(&cache->base);
    pthread_mutex_unlock(&cache_lock);
    pixmap_cache_destroy(cache);
    free(cache);
}

static int display_channel_init_cache(DisplayChannelClient *dcc, SpiceMsgcDisplayInit *init_info)
{
    ASSERT(!dcc->pixmap_cache);
    return !!(dcc->pixmap_cache = red_get_pixmap_cache(dcc->common.base.client,
                                                       init_info->pixmap_cache_id,
                                                       init_info->pixmap_cache_size));
}

static int display_channel_init_glz_dictionary(DisplayChannelClient *dcc,
                                               SpiceMsgcDisplayInit *init_info)
{
    ASSERT(!dcc->glz_dict);
    ring_init(&dcc->glz_drawables);
    ring_init(&dcc->glz_drawables_inst_to_free);
    pthread_mutex_init(&dcc->glz_drawables_inst_to_free_lock, NULL);
    return !!(dcc->glz_dict = red_get_glz_dictionary(dcc,
                                                     init_info->glz_dictionary_id,
                                                     init_info->glz_dictionary_window_size));
}

static int display_channel_init(DisplayChannelClient *dcc, SpiceMsgcDisplayInit *init_info)
{
    return (display_channel_init_cache(dcc, init_info) &&
            display_channel_init_glz_dictionary(dcc, init_info));
}

static int display_channel_handle_migrate_glz_dictionary(DisplayChannelClient *dcc,
                                                         DisplayChannelMigrateData *migrate_info)
{
    ASSERT(!dcc->glz_dict);
    ring_init(&dcc->glz_drawables);
    ring_init(&dcc->glz_drawables_inst_to_free);
    pthread_mutex_init(&dcc->glz_drawables_inst_to_free_lock, NULL);
    return !!(dcc->glz_dict = red_restore_glz_dictionary(dcc,
                                                         migrate_info->glz_dict_id,
                                                         &migrate_info->glz_dict_restore_data));
}

static int display_channel_handle_migrate_mark(RedChannelClient *rcc)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    RedChannel *channel = &display_channel->common.base;

    if (!display_channel->expect_migrate_mark) {
        red_printf("unexpected");
        return FALSE;
    }
    display_channel->expect_migrate_mark = FALSE;
    red_channel_pipes_add_type(channel, PIPE_ITEM_TYPE_MIGRATE_DATA);
    return TRUE;
}

static uint64_t display_channel_handle_migrate_data_get_serial(
                RedChannelClient *rcc, uint32_t size, void *message)
{
    DisplayChannelMigrateData *migrate_data = message;

    if (size < sizeof(*migrate_data)) {
        red_printf("bad message size");
        return 0;
    }
    if (migrate_data->magic != DISPLAY_MIGRATE_DATA_MAGIC ||
        migrate_data->version != DISPLAY_MIGRATE_DATA_VERSION) {
        red_printf("invalid content");
        return 0;
    }
    return migrate_data->message_serial;
}

static uint64_t display_channel_handle_migrate_data(RedChannelClient *rcc, uint32_t size,
                                                    void *message)
{
    DisplayChannelMigrateData *migrate_data;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedChannel *channel = &display_channel->common.base;
    int i;

    if (size < sizeof(*migrate_data)) {
        red_printf("bad message size");
        return FALSE;
    }
    migrate_data = (DisplayChannelMigrateData *)message;
    if (migrate_data->magic != DISPLAY_MIGRATE_DATA_MAGIC ||
        migrate_data->version != DISPLAY_MIGRATE_DATA_VERSION) {
        red_printf("invalid content");
        return FALSE;
    }
    if (!display_channel->expect_migrate_data) {
        red_printf("unexpected");
        return FALSE;
    }
    display_channel->expect_migrate_data = FALSE;
    dcc->pixmap_cache = red_get_pixmap_cache(dcc->common.base.client,
                                            migrate_data->pixmap_cache_id, -1);
    if (!dcc->pixmap_cache) {
        return FALSE;
    }
    pthread_mutex_lock(&dcc->pixmap_cache->lock);
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        dcc->pixmap_cache->sync[i] = MAX(dcc->pixmap_cache->sync[i],
                                         migrate_data->pixmap_cache_clients[i]);
    }
    pthread_mutex_unlock(&dcc->pixmap_cache->lock);

    if (migrate_data->pixmap_cache_freezer) {
        dcc->pixmap_cache->size = migrate_data->pixmap_cache_size;
        // TODO - should this be red_channel_client_pipe_add_type?
        red_channel_pipes_add_type(channel,
                                   PIPE_ITEM_TYPE_PIXMAP_RESET);
    }

    if (display_channel_handle_migrate_glz_dictionary(dcc, migrate_data)) {
        dcc->glz = glz_encoder_create(dcc->common.id,
                                      dcc->glz_dict->dict, &dcc->glz_data.usr);
        if (!dcc->glz) {
            PANIC("create global lz failed");
        }
    } else {
        PANIC("restoring global lz dictionary failed");
    }

    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
    red_channel_client_ack_zero_messages_window(rcc);
    return TRUE;
}

static int display_channel_handle_message(RedChannelClient *rcc, uint32_t size, uint16_t type,
                                          void *message)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    switch (type) {
    case SPICE_MSGC_DISPLAY_INIT:
        if (!dcc->expect_init) {
            red_printf("unexpected SPICE_MSGC_DISPLAY_INIT");
            return FALSE;
        }
        dcc->expect_init = FALSE;
        return display_channel_init(dcc, (SpiceMsgcDisplayInit *)message);
    default:
        return red_channel_client_handle_message(rcc, size, type, message);
    }
}

static int common_channel_config_socket(RedChannelClient *rcc)
{
    RedClient *client = red_channel_client_get_client(rcc);
    MainChannelClient *mcc = red_client_get_main(client);
    RedsStream *stream = red_channel_client_get_stream(rcc);
    int flags;
    int delay_val;

    if ((flags = fcntl(stream->socket, F_GETFL)) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        return FALSE;
    }

    if (fcntl(stream->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        return FALSE;
    }

    // TODO - this should be dynamic, not one time at channel creation
    delay_val = main_channel_client_is_low_bandwidth(mcc) ? 0 : 1;
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val,
                   sizeof(delay_val)) == -1) {
        if (errno != ENOTSUP) {
            red_printf("setsockopt failed, %s", strerror(errno));
        }
    }
    return TRUE;
}

static void free_common_cc_from_listener(EventListener *ctx)
{
    CommonChannelClient* common_cc = SPICE_CONTAINEROF(ctx, CommonChannelClient, listener);

    red_printf("");
    free(common_cc);
}

static void worker_watch_update_mask(SpiceWatch *watch, int event_mask)
{
}

static SpiceWatch *worker_watch_add(int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    return NULL; // apparently allowed?
}

static void worker_watch_remove(SpiceWatch *watch)
{
}

SpiceCoreInterface worker_core = {
    .watch_update_mask = worker_watch_update_mask,
    .watch_add = worker_watch_add,
    .watch_remove = worker_watch_remove,
};

static CommonChannelClient *common_channel_client_create(int size,
                                                         CommonChannel *common,
                                                         RedClient *client,
                                                         RedsStream *stream,
                                                         uint32_t *common_caps,
                                                         int num_common_caps,
                                                         uint32_t *caps,
                                                         int num_caps)
{
    MainChannelClient *mcc = red_client_get_main(client);
    RedChannelClient *rcc =
        red_channel_client_create(size, &common->base, client, stream,
                                  num_common_caps, common_caps, num_caps, caps);
    CommonChannelClient *common_cc = (CommonChannelClient*)rcc;
    common_cc->worker = common->worker;

    // TODO: move wide/narrow ack setting to red_channel.
    red_channel_client_ack_set_client_window(rcc,
        main_channel_client_is_low_bandwidth(mcc) ?
        WIDE_CLIENT_ACK_WINDOW : NARROW_CLIENT_ACK_WINDOW);
    return common_cc;
}


DisplayChannelClient *display_channel_client_create(CommonChannel *common,
                                                    RedClient *client, RedsStream *stream,
                                                    uint32_t *common_caps, int num_common_caps,
                                                    uint32_t *caps, int num_caps)
{
    DisplayChannelClient *dcc =
        (DisplayChannelClient*)common_channel_client_create(
            sizeof(DisplayChannelClient), common, client, stream,
            common_caps, num_common_caps,
            caps, num_caps);

    if (!dcc) {
        return NULL;
    }
    ring_init(&dcc->palette_cache_lru);
    dcc->palette_cache_available = CLIENT_PALETTE_CACHE_SIZE;
    return dcc;
}

CursorChannelClient *cursor_channel_create_rcc(CommonChannel *common,
                                               RedClient *client, RedsStream *stream,
                                               uint32_t *common_caps, int num_common_caps,
                                               uint32_t *caps, int num_caps)
{
    CursorChannelClient *ccc =
        (CursorChannelClient*)common_channel_client_create(
            sizeof(CursorChannelClient), common, client, stream,
            common_caps,
            num_common_caps,
            caps,
            num_caps);

    if (!ccc) {
        return NULL;
    }
    ring_init(&ccc->cursor_cache_lru);
    ccc->cursor_cache_available = CLIENT_CURSOR_CACHE_SIZE;
    return ccc;
}

static int listen_to_new_client_channel(CommonChannel *common,
    CommonChannelClient *common_cc, RedsStream *stream)
{
    struct epoll_event event;

    common_cc->listener.refs = 1;
    common_cc->listener.action = common->listener_action;
    common_cc->listener.free = free_common_cc_from_listener;
    ASSERT(common->base.clients_num);
    common_cc->id = common->worker->id;
    red_printf("NEW ID = %d", common_cc->id);
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    event.data.ptr = &common_cc->listener;
    if (epoll_ctl(common->worker->epoll, EPOLL_CTL_ADD, stream->socket, &event) == -1) {
        red_printf("epoll_ctl failed, %s", strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static RedChannel *__new_channel(RedWorker *worker, int size, uint32_t channel_type, int migrate,
                                 event_listener_action_proc handler,
                                 channel_disconnect_proc on_disconnect,
                                 channel_send_pipe_item_proc send_item,
                                 channel_hold_pipe_item_proc hold_item,
                                 channel_release_pipe_item_proc release_item,
                                 channel_handle_parsed_proc handle_parsed,
                                 channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark,
                                 channel_handle_migrate_data_proc handle_migrate_data,
                                 channel_handle_migrate_data_get_serial_proc migrate_get_serial)
{
    RedChannel *channel = NULL;
    CommonChannel *common;
    ChannelCbs channel_cbs;

    channel_cbs.config_socket = common_channel_config_socket;
    channel_cbs.on_disconnect = on_disconnect;
    channel_cbs.send_item = send_item;
    channel_cbs.hold_item = hold_item;
    channel_cbs.release_item = release_item;
    channel_cbs.alloc_recv_buf = common_alloc_recv_buf;
    channel_cbs.release_recv_buf = common_release_recv_buf;
    channel_cbs.handle_migrate_flush_mark = handle_migrate_flush_mark;
    channel_cbs.handle_migrate_data = handle_migrate_data;
    channel_cbs.handle_migrate_data_get_serial = migrate_get_serial;

    channel = red_channel_create_parser(size, &worker_core,
                                        channel_type, worker->id,
                                        migrate,
                                        TRUE /* handle_acks */,
                                        spice_get_client_channel_parser(channel_type, NULL),
                                        handle_parsed,
                                        &channel_cbs);
    common = (CommonChannel *)channel;
    if (!channel) {
        goto error;
    }
    common->worker = worker;
    common->listener_action = handler;
    return channel;

error:
    free(channel);
    return NULL;
}

static void handle_channel_events(EventListener *in_listener, uint32_t events)
{
    CommonChannelClient *common_cc = SPICE_CONTAINEROF(in_listener, CommonChannelClient, listener);
    RedChannelClient *rcc = &common_cc->base;

    if ((events & EPOLLIN) && red_channel_client_is_connected(rcc)) {
        red_channel_client_receive(rcc);
    }

    if (rcc->send_data.blocked && red_channel_client_is_connected(rcc)) {
        red_channel_client_push(rcc);
    }
}

static void display_channel_hold_pipe_item(RedChannelClient *rcc, PipeItem *item)
{
    ASSERT(item);
    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW:
        ref_drawable_pipe_item(SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item));
        break;
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *stream_agent = SPICE_CONTAINEROF(item, StreamAgent, create_item);
        if (stream_agent->stream->current) {
            stream_agent->stream->current->refs++;
        }
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        ((StreamClipItem *)item)->refs++;
        break;
    case PIPE_ITEM_TYPE_UPGRADE:
        ((UpgradeItem *)item)->refs++;
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        ((ImageItem *)item)->refs++;
        break;
    default:
        PANIC("invalid item type");
    }
}

static void display_channel_client_release_item_after_push(DisplayChannelClient *dcc,
                                                           PipeItem *item)
{
    RedWorker *worker = dcc->common.worker;

    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW:
        put_drawable_pipe_item(SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item));
        break;
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *stream_agent = SPICE_CONTAINEROF(item, StreamAgent, create_item);
        if (stream_agent->stream->current) {
            release_drawable(worker, stream_agent->stream->current);
        }
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        red_display_release_stream_clip(worker, (StreamClipItem *)item);
        break;
    case PIPE_ITEM_TYPE_UPGRADE:
        release_upgrade_item(worker, (UpgradeItem *)item);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        release_image_item((ImageItem *)item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        free(item);
        break;
    default:
        PANIC("invalid item type");
    }
}

// TODO: share code between before/after_push since most of the items need the same
// release
static void display_channel_client_release_item_before_push(DisplayChannelClient *dcc,
                                                            PipeItem *item)
{
    RedWorker *worker = dcc->common.worker;

    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW: {
        DrawablePipeItem *dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item);
        ring_remove(&dpi->base);
        put_drawable_pipe_item(dpi);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *agent = SPICE_CONTAINEROF(item, StreamAgent, create_item);
        red_display_release_stream(worker, agent);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        red_display_release_stream_clip(worker, (StreamClipItem *)item);
        break;
    case PIPE_ITEM_TYPE_STREAM_DESTROY: {
        StreamAgent *agent = SPICE_CONTAINEROF(item, StreamAgent, destroy_item);
        red_display_release_stream(worker, agent);
        break;
    }
    case PIPE_ITEM_TYPE_UPGRADE:
        release_upgrade_item(worker, (UpgradeItem *)item);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        release_image_item((ImageItem *)item);
        break;
    case PIPE_ITEM_TYPE_CREATE_SURFACE: {
        SurfaceCreateItem *surface_create = SPICE_CONTAINEROF(item, SurfaceCreateItem,
                                                              pipe_item);
        free(surface_create);
        break;
    }
    case PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        SurfaceDestroyItem *surface_destroy = SPICE_CONTAINEROF(item, SurfaceDestroyItem,
                                                                pipe_item);
        free(surface_destroy);
        break;
    }
    case PIPE_ITEM_TYPE_INVAL_ONE:
    case PIPE_ITEM_TYPE_VERB:
    case PIPE_ITEM_TYPE_MIGRATE:
    case PIPE_ITEM_TYPE_MIGRATE_DATA:
    case PIPE_ITEM_TYPE_PIXMAP_SYNC:
    case PIPE_ITEM_TYPE_PIXMAP_RESET:
    case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
        free(item);
        break;
    default:
        PANIC("invalid item type");
    }
}

static void display_channel_release_item(RedChannelClient *rcc, PipeItem *item, int item_pushed)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    ASSERT(item);
    if (item_pushed) {
        display_channel_client_release_item_after_push(dcc, item);
    } else {
        red_printf_once("not pushed (%d)", item->type);
        display_channel_client_release_item_before_push(dcc, item);
    }
}

static void display_channel_create(RedWorker *worker, int migrate)
{
    DisplayChannel *display_channel;

    if (worker->display_channel) {
        return;
    }

    red_printf("create display channel");
    if (!(worker->display_channel = (DisplayChannel *)__new_channel(
            worker, sizeof(*display_channel),
            SPICE_CHANNEL_DISPLAY, migrate,
            handle_channel_events,
            display_channel_client_on_disconnect,
            display_channel_send_item,
            display_channel_hold_pipe_item,
            display_channel_release_item,
            display_channel_handle_message,
            display_channel_handle_migrate_mark,
            display_channel_handle_migrate_data,
            display_channel_handle_migrate_data_get_serial
            ))) {
        red_printf("failed to create display channel");
        return;
    }
    display_channel = worker->display_channel;
#ifdef RED_STATISTICS
    display_channel->stat = stat_add_node(worker->stat, "display_channel", TRUE);
    display_channel->common.base.out_bytes_counter = stat_add_counter(display_channel->stat,
                                                               "out_bytes", TRUE);
    display_channel->cache_hits_counter = stat_add_counter(display_channel->stat,
                                                           "cache_hits", TRUE);
    display_channel->add_to_cache_counter = stat_add_counter(display_channel->stat,
                                                             "add_to_cache", TRUE);
    display_channel->non_cache_counter = stat_add_counter(display_channel->stat,
                                                          "non_cache", TRUE);
#endif
    stat_compress_init(&display_channel->lz_stat, lz_stat_name);
    stat_compress_init(&display_channel->glz_stat, glz_stat_name);
    stat_compress_init(&display_channel->quic_stat, quic_stat_name);
    stat_compress_init(&display_channel->jpeg_stat, jpeg_stat_name);
    stat_compress_init(&display_channel->zlib_glz_stat, zlib_stat_name);
    stat_compress_init(&display_channel->jpeg_alpha_stat, jpeg_alpha_stat_name);
}


static void handle_new_display_channel(RedWorker *worker, RedClient *client, RedsStream *stream,
                                       int migrate,
                                       uint32_t *common_caps, int num_common_caps,
                                       uint32_t *caps, int num_caps)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc;
    size_t stream_buf_size;
    int is_low_bandwidth = main_channel_client_is_low_bandwidth(red_client_get_main(client));

    if (!worker->display_channel) {
        red_printf("Warning: Display channel was not created");
        return;
    }
    display_channel = worker->display_channel;
    red_printf("add display channel client");
    dcc = display_channel_client_create(&display_channel->common, client, stream,
                                        common_caps, num_common_caps,
                                        caps, num_caps);
    if (!dcc) {
        return;
    }
    red_printf("New display (client %p) dcc %p stream %p", client, dcc, stream);
    stream_buf_size = 32*1024;
    dcc->send_data.stream_outbuf = spice_malloc(stream_buf_size);
    dcc->send_data.stream_outbuf_size = stream_buf_size;
    red_display_init_glz_data(dcc);

    dcc->send_data.free_list.res =
        spice_malloc(sizeof(SpiceResourceList) +
                     DISPLAY_FREE_LIST_DEFAULT_SIZE * sizeof(SpiceResourceID));
    dcc->send_data.free_list.res_size = DISPLAY_FREE_LIST_DEFAULT_SIZE;

    if (worker->jpeg_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_jpeg = is_low_bandwidth;
    } else {
        display_channel->enable_jpeg = (worker->jpeg_state == SPICE_WAN_COMPRESSION_ALWAYS);
    }

    // todo: tune quality according to bandwidth
    display_channel->jpeg_quality = 85;

    if (worker->zlib_glz_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_zlib_glz_wrap = is_low_bandwidth;
    } else {
        display_channel->enable_zlib_glz_wrap = (worker->zlib_glz_state ==
                                                 SPICE_WAN_COMPRESSION_ALWAYS);
    }

    red_printf("jpeg %s", display_channel->enable_jpeg ? "enabled" : "disabled");
    red_printf("zlib-over-glz %s", display_channel->enable_zlib_glz_wrap ? "enabled" : "disabled");

    // todo: tune level according to bandwidth
    display_channel->zlib_level = ZLIB_DEFAULT_COMPRESSION_LEVEL;
    if (!listen_to_new_client_channel(&display_channel->common, &dcc->common, stream)) {
        goto error;
    }
    red_display_client_init_streams(dcc);
    on_new_display_channel_client(dcc);
    return;

error:
    red_channel_client_destroy(&dcc->common.base);
}

static void cursor_channel_client_disconnect(RedChannelClient *rcc)
{
    red_channel_client_disconnect(rcc);
}

static void cursor_channel_client_on_disconnect(RedChannelClient *rcc)
{
    if (!rcc) {
        return;
    }
    red_reset_cursor_cache(rcc);
}

static void red_disconnect_cursor(RedChannel *channel)
{
    CommonChannel *common;

    if (!channel || !red_channel_is_connected(channel)) {
        return;
    }
    common = SPICE_CONTAINEROF(channel, CommonChannel, base);
    ASSERT(channel == (RedChannel *)common->worker->cursor_channel);
    common->worker->cursor_channel = NULL;
    red_channel_apply_clients(channel, red_reset_cursor_cache);
    red_disconnect_channel(channel);
}

static void red_migrate_cursor(RedWorker *worker, RedChannelClient *rcc)
{
//    if (cursor_is_connected(worker)) {
    if (red_channel_client_is_connected(rcc)) {
        red_channel_client_pipe_add_type(rcc,
                                         PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
        red_channel_client_pipe_add_type(rcc,
                                         PIPE_ITEM_TYPE_MIGRATE);
    }
}

static void on_new_cursor_channel(RedWorker *worker, RedChannelClient *rcc)
{
    CursorChannel *channel = worker->cursor_channel;

    ASSERT(channel);
    red_channel_client_ack_zero_messages_window(rcc);
    red_channel_client_push_set_ack(rcc);
    // TODO: why do we check for context.canvas? defer this to after display cc is connected
    // and test it's canvas? this is just a test to see if there is an active renderer?
    if (worker->surfaces[0].context.canvas && !channel->common.base.migrate) {
        red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_CURSOR_INIT);
    }
}

static void cursor_channel_hold_pipe_item(RedChannelClient *rcc, PipeItem *item)
{
    CursorPipeItem *cursor_pipe_item;
    ASSERT(item);
    cursor_pipe_item = SPICE_CONTAINEROF(item, CursorPipeItem, base);
    ref_cursor_pipe_item(cursor_pipe_item);
}

// TODO: share code between before/after_push since most of the items need the same
// release
static void cursor_channel_client_release_item_before_push(CursorChannelClient *ccc,
                                                           PipeItem *item)
{
    switch (item->type) {
    case PIPE_ITEM_TYPE_CURSOR: {
        CursorPipeItem *cursor_pipe_item = SPICE_CONTAINEROF(item, CursorPipeItem, base);
        put_cursor_pipe_item(ccc, cursor_pipe_item);
        break;
    }
    case PIPE_ITEM_TYPE_INVAL_ONE:
    case PIPE_ITEM_TYPE_VERB:
    case PIPE_ITEM_TYPE_MIGRATE:
    case PIPE_ITEM_TYPE_CURSOR_INIT:
    case PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE:
        free(item);
        break;
    default:
        red_error("invalid pipe item type");
    }
}

static void cursor_channel_client_release_item_after_push(CursorChannelClient *ccc,
                                                          PipeItem *item)
{
    switch (item->type) {
        case PIPE_ITEM_TYPE_CURSOR: {
            CursorPipeItem *cursor_pipe_item = SPICE_CONTAINEROF(item, CursorPipeItem, base);
            put_cursor_pipe_item(ccc, cursor_pipe_item);
            break;
        }
        default:
            PANIC("invalid item type");
    }
}

static void cursor_channel_release_item(RedChannelClient *rcc, PipeItem *item, int item_pushed)
{
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);

    ASSERT(item);

    if (item_pushed) {
        cursor_channel_client_release_item_after_push(ccc, item);
    } else {
        red_printf_once("not pushed (%d)", item->type);
        cursor_channel_client_release_item_before_push(ccc, item);
    }
}

static void cursor_channel_create(RedWorker *worker, int migrate)
{
    if (worker->cursor_channel != NULL) {
        return;
    }
    red_printf("create cursor channel");
    worker->cursor_channel = (CursorChannel *)__new_channel(
        worker, sizeof(*worker->cursor_channel),
        SPICE_CHANNEL_CURSOR, migrate,
        handle_channel_events,
        cursor_channel_client_on_disconnect,
        cursor_channel_send_item,
        cursor_channel_hold_pipe_item,
        cursor_channel_release_item,
        red_channel_client_handle_message,
        NULL,
        NULL,
        NULL);
}

static void red_connect_cursor(RedWorker *worker, RedClient *client, RedsStream *stream,
                               int migrate,
                               uint32_t *common_caps, int num_common_caps,
                               uint32_t *caps, int num_caps)
{
    CursorChannel *channel;
    CursorChannelClient *ccc;

    if (worker->cursor_channel == NULL) {
        red_printf("Warning: cursor channel was not created");
        return;
    }
    channel = worker->cursor_channel;
    red_printf("add cursor channel client");
    ccc = cursor_channel_create_rcc(&channel->common, client, stream,
                                    common_caps, num_common_caps,
                                    caps, num_caps);
    if (!ccc) {
        return;
    }
#ifdef RED_STATISTICS
    channel->stat = stat_add_node(worker->stat, "cursor_channel", TRUE);
    channel->common.base.out_bytes_counter = stat_add_counter(channel->stat, "out_bytes", TRUE);
#endif
    listen_to_new_client_channel(&channel->common, &ccc->common, stream);
    on_new_cursor_channel(worker, &ccc->common.base);
}

typedef struct __attribute__ ((__packed__)) CursorData {
    uint32_t visible;
    SpicePoint16 position;
    uint16_t trail_length;
    uint16_t trail_frequency;
    uint32_t data_size;
    SpiceCursor _cursor;
} CursorData;

static void red_wait_outgoing_item(RedChannelClient *rcc)
{
    uint64_t end_time;
    int blocked;

    if (!red_channel_client_blocked(rcc)) {
        return;
    }
    end_time = red_now() + DETACH_TIMEOUT;
    red_printf("blocked");

    do {
        usleep(DETACH_SLEEP_DURATION);
        red_channel_client_receive(rcc);
        red_channel_client_send(rcc);
    } while ((blocked = red_channel_client_blocked(rcc)) && red_now() < end_time);

    if (blocked) {
        red_printf("timeout");
        // TODO - shutting down the socket but we still need to trigger
        // disconnection. Right now we wait for main channel to error for that.
        red_channel_client_shutdown(rcc);
    } else {
        ASSERT(red_channel_client_no_item_being_sent(rcc));
    }
}

static void rcc_shutdown_if_blocked(RedChannelClient *rcc)
{
    if (red_channel_client_blocked(rcc)) {
        red_channel_client_shutdown(rcc);
    } else {
        ASSERT(red_channel_client_no_item_being_sent(rcc));
    }
}

static void red_wait_outgoing_items(RedChannel *channel)
{
    uint64_t end_time;
    int blocked;

    if (!red_channel_any_blocked(channel)) {
        return;
    }

    end_time = red_now() + DETACH_TIMEOUT;
    red_printf("blocked");

    do {
        usleep(DETACH_SLEEP_DURATION);
        red_channel_receive(channel);
        red_channel_send(channel);
    } while ((blocked = red_channel_any_blocked(channel)) && red_now() < end_time);

    if (blocked) {
        red_printf("timeout");
        red_channel_apply_clients(channel, rcc_shutdown_if_blocked);
    } else {
        ASSERT(red_channel_no_item_being_sent(channel));
    }
}

/* TODO: more evil sync stuff. anything with the word wait in it's name. */
static void red_wait_pipe_item_sent(RedChannelClient *rcc, PipeItem *item)
{
    RedChannel *channel = rcc->channel;
    uint64_t end_time;
    int item_in_pipe;

    if (!red_channel_client_blocked(rcc)) {
        return;
    }

    red_printf("");
    channel->channel_cbs.hold_item(rcc, item);

    end_time = red_now() + CHANNEL_PUSH_TIMEOUT;

    if (red_channel_client_blocked(rcc)) {
        red_channel_client_receive(rcc);
        red_channel_client_send(rcc);
    }
    red_channel_client_push(rcc);

    while((item_in_pipe = ring_item_is_linked(&item->link)) && (red_now() < end_time)) {
        usleep(CHANNEL_PUSH_SLEEP_DURATION);
        red_channel_client_receive(rcc);
        red_channel_client_send(rcc);
        red_channel_client_push(rcc);
    }

    if (item_in_pipe) {
        red_printf("timeout");
        red_channel_client_disconnect(rcc);
    } else {
        if (red_channel_client_item_being_sent(rcc, item)) {
            red_wait_outgoing_item(rcc);
        }
    }
    channel->channel_cbs.release_item(rcc, item, FALSE);
}

static void surface_dirty_region_to_rects(RedSurface *surface,
                                          QXLRect *qxl_dirty_rects,
                                          uint32_t num_dirty_rects,
                                          int clear_dirty_region)
{
    QRegion *surface_dirty_region;
    SpiceRect *dirty_rects;
    int i;

    surface_dirty_region = &surface->draw_dirty_region;
    dirty_rects = spice_new0(SpiceRect, num_dirty_rects);
    region_ret_rects(surface_dirty_region, dirty_rects, num_dirty_rects);
    if (clear_dirty_region) {
        region_clear(surface_dirty_region);
    }
    for (i = 0; i < num_dirty_rects; i++) {
        qxl_dirty_rects[i].top    = dirty_rects[i].top;
        qxl_dirty_rects[i].left   = dirty_rects[i].left;
        qxl_dirty_rects[i].bottom = dirty_rects[i].bottom;
        qxl_dirty_rects[i].right  = dirty_rects[i].right;
    }
    free(dirty_rects);
}

void handle_dev_update_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdateAsync *msg = payload;
    SpiceRect rect;
    QXLRect *qxl_dirty_rects;
    uint32_t num_dirty_rects;
    RedSurface *surface;
    uint32_t surface_id = msg->surface_id;
    QXLRect qxl_area = msg->qxl_area;
    uint32_t clear_dirty_region = msg->clear_dirty_region;

    red_get_rect_ptr(&rect, &qxl_area);
    flush_display_commands(worker);

    ASSERT(worker->running);

    validate_surface(worker, surface_id);
    red_update_area(worker, &rect, surface_id);
    if (!worker->qxl->st->qif->update_area_complete) {
        return;
    }
    surface = &worker->surfaces[surface_id];
    num_dirty_rects = pixman_region32_n_rects(&surface->draw_dirty_region);
    if (num_dirty_rects == 0) {
        return;
    }
    qxl_dirty_rects = spice_new0(QXLRect, num_dirty_rects);
    surface_dirty_region_to_rects(surface, qxl_dirty_rects, num_dirty_rects,
                                  clear_dirty_region);
    worker->qxl->st->qif->update_area_complete(worker->qxl, surface_id,
                                          qxl_dirty_rects, num_dirty_rects);
    free(qxl_dirty_rects);
}

void handle_dev_update(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdate *msg = payload;
    SpiceRect *rect = spice_new0(SpiceRect, 1);
    RedSurface *surface;
    uint32_t surface_id = msg->surface_id;
    const QXLRect *qxl_area = msg->qxl_area;
    uint32_t num_dirty_rects = msg->num_dirty_rects;
    QXLRect *qxl_dirty_rects = msg->qxl_dirty_rects;
    uint32_t clear_dirty_region = msg->clear_dirty_region;

    surface = &worker->surfaces[surface_id];
    red_get_rect_ptr(rect, qxl_area);
    flush_display_commands(worker);

    ASSERT(worker->running);

    validate_surface(worker, surface_id);
    red_update_area(worker, rect, surface_id);
    free(rect);

    surface_dirty_region_to_rects(surface, qxl_dirty_rects, num_dirty_rects,
                                  clear_dirty_region);
}

static void dev_add_memslot(RedWorker *worker, QXLDevMemSlot mem_slot)
{
    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

void handle_dev_add_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAddMemslot *msg = payload;
    QXLDevMemSlot mem_slot = msg->mem_slot;

    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

void handle_dev_del_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageDelMemslot *msg = payload;
    uint32_t slot_id = msg->slot_id;
    uint32_t slot_group_id = msg->slot_group_id;

    red_memslot_info_del_slot(&worker->mem_slots, slot_group_id, slot_id);
}

/* TODO: destroy_surface_wait, dev_destroy_surface_wait - confusing. one asserts
 * surface_id == 0, maybe move the assert upward and merge the two functions? */
static inline void destroy_surface_wait(RedWorker *worker, int surface_id)
{
    if (!worker->surfaces[surface_id].context.canvas) {
        return;
    }

    red_handle_depends_on_target_surface(worker, surface_id);
    /* note that red_handle_depends_on_target_surface must be called before red_current_clear.
       otherwise "current" will hold items that other drawables may depend on, and then
       red_current_clear will remove them from the pipe. */
    red_current_clear(worker, surface_id);
    red_clear_surface_drawables_from_pipes(worker, surface_id, TRUE, TRUE);
}

static void dev_destroy_surface_wait(RedWorker *worker, uint32_t surface_id)
{
    ASSERT(surface_id == 0);

    flush_all_qxl_commands(worker);

    if (worker->surfaces[0].context.canvas) {
        destroy_surface_wait(worker, 0);
    }
}

void handle_dev_destroy_surface_wait(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWait *msg = payload;
    RedWorker *worker = opaque;

    dev_destroy_surface_wait(worker, msg->surface_id);
}

static inline void red_cursor_reset(RedWorker *worker)
{
    if (worker->cursor) {
        red_release_cursor(worker, worker->cursor);
        worker->cursor = NULL;
    }

    worker->cursor_visible = TRUE;
    worker->cursor_position.x = worker->cursor_position.y = 0;
    worker->cursor_trail_length = worker->cursor_trail_frequency = 0;

    if (cursor_is_connected(worker)) {
        red_channel_pipes_add_type(&worker->cursor_channel->common.base,
                                   PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
        if (!worker->cursor_channel->common.base.migrate) {
            red_pipes_add_verb(&worker->cursor_channel->common.base, SPICE_MSG_CURSOR_RESET);
        }
        red_wait_outgoing_items(&worker->cursor_channel->common.base);
    }
}

/* called upon device reset */

/* TODO: split me*/
static inline void dev_destroy_surfaces(RedWorker *worker)
{
    int i;

    flush_all_qxl_commands(worker);
    //to handle better
    for (i = 0; i < NUM_SURFACES; ++i) {
        if (worker->surfaces[i].context.canvas) {
            destroy_surface_wait(worker, i);
            if (worker->surfaces[i].context.canvas) {
                red_destroy_surface(worker, i);
            }
            ASSERT(!worker->surfaces[i].context.canvas);
        }
    }
    ASSERT(ring_is_empty(&worker->streams));

    if (display_is_connected(worker)) {
        red_channel_pipes_add_type(&worker->display_channel->common.base,
                                   PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
        red_pipes_add_verb(&worker->display_channel->common.base,
                           SPICE_MSG_DISPLAY_STREAM_DESTROY_ALL);
    }

    red_display_clear_glz_drawables(worker->display_channel);

    red_cursor_reset(worker);
}

void handle_dev_destroy_surfaces(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_destroy_surfaces(worker);
}

static void dev_create_primary_surface(RedWorker *worker, uint32_t surface_id,
                                       QXLDevSurfaceCreate surface)
{
    uint8_t *line_0;

    PANIC_ON(surface_id != 0);
    PANIC_ON(surface.height == 0);
    PANIC_ON(((uint64_t)abs(surface.stride) * (uint64_t)surface.height) !=
             abs(surface.stride) * surface.height);

    line_0 = (uint8_t*)get_virt(&worker->mem_slots, surface.mem,
                                surface.height * abs(surface.stride), surface.group_id);
    if (surface.stride < 0) {
        line_0 -= (int32_t)(surface.stride * (surface.height -1));
    }

    red_create_surface(worker, 0, surface.width, surface.height, surface.stride, surface.format,
                       line_0, surface.flags & QXL_SURF_FLAG_KEEP_DATA, TRUE);

    if (display_is_connected(worker)) {
        red_pipes_add_verb(&worker->display_channel->common.base,
                           SPICE_MSG_DISPLAY_MARK);
        red_channel_push(&worker->display_channel->common.base);
    }

    if (cursor_is_connected(worker)) {
        red_channel_pipes_add_type(&worker->cursor_channel->common.base,
                                   PIPE_ITEM_TYPE_CURSOR_INIT);
    }
}

void handle_dev_create_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurface *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

static void dev_destroy_primary_surface(RedWorker *worker, uint32_t surface_id)
{
    PANIC_ON(surface_id != 0);

    if (!worker->surfaces[surface_id].context.canvas) {
        red_printf("double destroy of primary surface\n");
        return;
    }

    flush_all_qxl_commands(worker);
    dev_destroy_surface_wait(worker, 0);
    red_destroy_surface(worker, 0);
    ASSERT(ring_is_empty(&worker->streams));

    ASSERT(!worker->surfaces[surface_id].context.canvas);

    red_cursor_reset(worker);
}

void handle_dev_destroy_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurface *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    dev_destroy_primary_surface(worker, surface_id);
}

void handle_dev_destroy_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    dev_destroy_primary_surface(worker, surface_id);
}

static void flush_all_surfaces(RedWorker *worker)
{
    int x;

    for (x = 0; x < NUM_SURFACES; ++x) {
        if (worker->surfaces[x].context.canvas) {
            red_current_flush(worker, x);
        }
    }
}

static void dev_flush_surfaces(RedWorker *worker)
{
    flush_all_qxl_commands(worker);
    flush_all_surfaces(worker);
    red_wait_outgoing_items(&worker->display_channel->common.base);
    red_wait_outgoing_items(&worker->cursor_channel->common.base);
}

void handle_dev_flush_surfaces(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_flush_surfaces(worker);
}

void handle_dev_flush_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_flush_surfaces(worker);
}

void handle_dev_stop(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    red_printf("stop");
    ASSERT(worker->running);
    worker->running = FALSE;
    red_display_clear_glz_drawables(worker->display_channel);
    flush_all_surfaces(worker);
    red_wait_outgoing_items(&worker->display_channel->common.base);
    red_wait_outgoing_items(&worker->cursor_channel->common.base);
}

void handle_dev_start(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedChannel *cursor_red_channel = &worker->cursor_channel->common.base;
    RedChannel *display_red_channel = &worker->display_channel->common.base;

    ASSERT(!worker->running);
    if (worker->cursor_channel) {
        cursor_red_channel->migrate = FALSE;
    }
    if (worker->display_channel) {
        display_red_channel->migrate = FALSE;
    }
    worker->running = TRUE;
}

void handle_dev_wakeup(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    clear_bit(RED_WORKER_PENDING_WAKEUP, worker->pending);
    stat_inc_counter(worker->wakeup_counter, 1);
}

void handle_dev_oom(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    RedChannel *display_red_channel = &worker->display_channel->common.base;
    int ring_is_empty;

    ASSERT(worker->running);
    // streams? but without streams also leak
    red_printf_debug(1, "WORKER",
                     "OOM1 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                     worker->drawable_count,
                     worker->red_drawable_count,
                     worker->glz_drawable_count,
                     worker->current_size,
                     worker->display_channel ?
                     red_channel_sum_pipes_size(display_red_channel) : 0);
    while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
        red_channel_push(&worker->display_channel->common.base);
    }
    if (worker->qxl->st->qif->flush_resources(worker->qxl) == 0) {
        red_free_some(worker);
        worker->qxl->st->qif->flush_resources(worker->qxl);
    }
    red_printf_debug(1, "WORKER",
                     "OOM2 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                     worker->drawable_count,
                     worker->red_drawable_count,
                     worker->glz_drawable_count,
                     worker->current_size,
                     worker->display_channel ?
                     red_channel_sum_pipes_size(display_red_channel) : 0);
    clear_bit(RED_WORKER_PENDING_OOM, worker->pending);
}

void handle_dev_reset_cursor(void *opaque, void *payload)
{
    red_cursor_reset((RedWorker *)opaque);
}

void handle_dev_reset_image_cache(void *opaque, void *payload)
{
    image_cache_reset(&((RedWorker *)opaque)->image_cache);
}

void handle_dev_destroy_surface_wait_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWaitAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_destroy_surface_wait(worker, msg->surface_id);
}

void handle_dev_destroy_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_destroy_surfaces(worker);
}

void handle_dev_create_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

/* exception for Dispatcher, data going from red_worker to main thread,
 * TODO: use a different dispatcher?
 * TODO: leave direct usage of channel(fd)? It's only used right after the
 * pthread is created, since the channel duration is the lifetime of the spice
 * server. */

void handle_dev_display_channel_create(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    RedChannel *red_channel;
    // TODO: handle seemless migration. Temp, setting migrate to FALSE
    display_channel_create(worker, FALSE);
    red_channel = &worker->display_channel->common.base;
    send_data(worker->channel, &red_channel, sizeof(RedChannel *));
}

void handle_dev_display_connect(void *opaque, void *payload)
{
    RedWorkerMessageDisplayConnect *msg = payload;
    RedWorker *worker = opaque;
    RedsStream *stream = msg->stream;
    RedClient *client = msg->client;
    int migration = msg->migration;

    red_printf("connect");
    handle_new_display_channel(worker, client, stream, migration,
                               msg->common_caps, msg->num_common_caps,
                               msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

void handle_dev_display_disconnect(void *opaque, void *payload)
{
    RedWorkerMessageDisplayDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;

    red_printf("disconnect display client");
    ASSERT(rcc);
    display_channel_client_disconnect(rcc);
}

void handle_dev_display_migrate(void *opaque, void *payload)
{
    RedWorkerMessageDisplayMigrate *msg = payload;
    RedWorker *worker = opaque;

    RedChannelClient *rcc = msg->rcc;
    red_printf("migrate display client");
    ASSERT(rcc);
    red_migrate_display(worker, rcc);
}

/* TODO: special, perhaps use another dispatcher? */
void handle_dev_cursor_channel_create(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedChannel *red_channel;

    // TODO: handle seemless migration. Temp, setting migrate to FALSE
    cursor_channel_create(worker, FALSE);
    red_channel = &worker->cursor_channel->common.base;
    send_data(worker->channel, &red_channel, sizeof(RedChannel *));
}

void handle_dev_cursor_connect(void *opaque, void *payload)
{
    RedWorkerMessageCursorConnect *msg = payload;
    RedWorker *worker = opaque;
    RedsStream *stream = msg->stream;
    RedClient *client = msg->client;
    int migration = msg->migration;

    red_printf("cursor connect");
    red_connect_cursor(worker, client, stream, migration,
                       msg->common_caps, msg->num_common_caps,
                       msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

void handle_dev_cursor_disconnect(void *opaque, void *payload)
{
    RedWorkerMessageCursorDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;

    red_printf("disconnect cursor client");
    ASSERT(rcc);
    cursor_channel_client_disconnect(rcc);
}

void handle_dev_cursor_migrate(void *opaque, void *payload)
{
    RedWorkerMessageCursorMigrate *msg = payload;
    RedWorker *worker = opaque;
    RedChannelClient *rcc = msg->rcc;

    red_printf("migrate cursor client");
    ASSERT(rcc);
    red_migrate_cursor(worker, rcc);
}

void handle_dev_set_compression(void *opaque, void *payload)
{
    RedWorkerMessageSetCompression *msg = payload;
    RedWorker *worker = opaque;

    worker->image_compression = msg->image_compression;
    switch (worker->image_compression) {
    case SPICE_IMAGE_COMPRESS_AUTO_LZ:
        red_printf("ic auto_lz");
        break;
    case SPICE_IMAGE_COMPRESS_AUTO_GLZ:
        red_printf("ic auto_glz");
        break;
    case SPICE_IMAGE_COMPRESS_QUIC:
        red_printf("ic quic");
        break;
    case SPICE_IMAGE_COMPRESS_LZ:
        red_printf("ic lz");
        break;
    case SPICE_IMAGE_COMPRESS_GLZ:
        red_printf("ic glz");
        break;
    case SPICE_IMAGE_COMPRESS_OFF:
        red_printf("ic off");
        break;
    default:
        red_printf("ic invalid");
    }
#ifdef COMPRESS_STAT
    print_compress_stats(worker->display_channel);
    if (worker->display_channel) {
        stat_reset(&worker->display_channel->quic_stat);
        stat_reset(&worker->display_channel->lz_stat);
        stat_reset(&worker->display_channel->glz_stat);
        stat_reset(&worker->display_channel->jpeg_stat);
        stat_reset(&worker->display_channel->zlib_glz_stat);
        stat_reset(&worker->display_channel->jpeg_alpha_stat);
    }
#endif
}

void handle_dev_set_streaming_video(void *opaque, void *payload)
{
    RedWorkerMessageSetStreamingVideo *msg = payload;
    RedWorker *worker = opaque;

    worker->streaming_video = msg->streaming_video;
    ASSERT(worker->streaming_video != STREAM_VIDEO_INVALID);
    switch(worker->streaming_video) {
        case STREAM_VIDEO_ALL:
            red_printf("sv all");
            break;
        case STREAM_VIDEO_FILTER:
            red_printf("sv filter");
            break;
        case STREAM_VIDEO_OFF:
            red_printf("sv off");
            break;
        default:
            red_printf("sv invalid");
    }
}

void handle_dev_set_mouse_mode(void *opaque, void *payload)
{
    RedWorkerMessageSetMouseMode *msg = payload;
    RedWorker *worker = opaque;

    worker->mouse_mode = msg->mode;
    red_printf("mouse mode %u", worker->mouse_mode);
}

void handle_dev_add_memslot_async(void *opaque, void *payload)
{
    RedWorkerMessageAddMemslotAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_add_memslot(worker, msg->mem_slot);
}

void handle_dev_reset_memslots(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    red_memslot_info_reset(&worker->mem_slots);
}

void handle_dev_loadvm_commands(void *opaque, void *payload)
{
    RedWorkerMessageLoadvmCommands *msg = payload;
    RedWorker *worker = opaque;
    uint32_t i;
    RedCursorCmd *cursor_cmd;
    RedSurfaceCmd *surface_cmd;
    uint32_t count = msg->count;
    QXLCommandExt *ext = msg->ext;

    red_printf("loadvm_commands");
    for (i = 0 ; i < count ; ++i) {
        switch (ext[i].cmd.type) {
        case QXL_CMD_CURSOR:
            cursor_cmd = spice_new0(RedCursorCmd, 1);
            red_get_cursor_cmd(&worker->mem_slots, ext[i].group_id,
                               cursor_cmd, ext[i].cmd.data);
            qxl_process_cursor(worker, cursor_cmd, ext[i].group_id);
            break;
        case QXL_CMD_SURFACE:
            surface_cmd = spice_new0(RedSurfaceCmd, 1);
            red_get_surface_cmd(&worker->mem_slots, ext[i].group_id,
                                surface_cmd, ext[i].cmd.data);
            red_process_surface(worker, surface_cmd, ext[i].group_id, TRUE);
            break;
        default:
            red_printf("unhandled loadvm command type (%d)", ext[i].cmd.type);
            break;
        }
    }
}

static void worker_handle_dispatcher_async_done(void *opaque,
                                                uint32_t message_type,
                                                void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAsync *msg_async = payload;

    red_printf_debug(2, "WORKER", "");
    red_dispatcher_async_complete(worker->red_dispatcher, msg_async->cmd);
}

static void register_callbacks(Dispatcher *dispatcher)
{
    dispatcher_register_async_done_callback(
                                    dispatcher,
                                    worker_handle_dispatcher_async_done);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_CONNECT,
                                handle_dev_display_connect,
                                sizeof(RedWorkerMessageDisplayConnect),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_DISCONNECT,
                                handle_dev_display_disconnect,
                                sizeof(RedWorkerMessageDisplayDisconnect),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_MIGRATE,
                                handle_dev_display_migrate,
                                sizeof(RedWorkerMessageDisplayMigrate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_CONNECT,
                                handle_dev_cursor_connect,
                                sizeof(RedWorkerMessageCursorConnect),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_DISCONNECT,
                                handle_dev_cursor_disconnect,
                                sizeof(RedWorkerMessageCursorDisconnect),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_MIGRATE,
                                handle_dev_cursor_migrate,
                                sizeof(RedWorkerMessageCursorMigrate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE,
                                handle_dev_update,
                                sizeof(RedWorkerMessageUpdate),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE_ASYNC,
                                handle_dev_update_async,
                                sizeof(RedWorkerMessageUpdateAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT,
                                handle_dev_add_memslot,
                                sizeof(RedWorkerMessageAddMemslot),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC,
                                handle_dev_add_memslot_async,
                                sizeof(RedWorkerMessageAddMemslotAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DEL_MEMSLOT,
                                handle_dev_del_memslot,
                                sizeof(RedWorkerMessageDelMemslot),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES,
                                handle_dev_destroy_surfaces,
                                sizeof(RedWorkerMessageDestroySurfaces),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC,
                                handle_dev_destroy_surfaces_async,
                                sizeof(RedWorkerMessageDestroySurfacesAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
                                handle_dev_destroy_primary_surface,
                                sizeof(RedWorkerMessageDestroyPrimarySurface),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC,
                                handle_dev_destroy_primary_surface_async,
                                sizeof(RedWorkerMessageDestroyPrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC,
                                handle_dev_create_primary_surface_async,
                                sizeof(RedWorkerMessageCreatePrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
                                handle_dev_create_primary_surface,
                                sizeof(RedWorkerMessageCreatePrimarySurface),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
                                handle_dev_reset_image_cache,
                                sizeof(RedWorkerMessageResetImageCache),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_CURSOR,
                                handle_dev_reset_cursor,
                                sizeof(RedWorkerMessageResetCursor),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_WAKEUP,
                                handle_dev_wakeup,
                                sizeof(RedWorkerMessageWakeup),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_OOM,
                                handle_dev_oom,
                                sizeof(RedWorkerMessageOom),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_START,
                                handle_dev_start,
                                sizeof(RedWorkerMessageStart),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC,
                                handle_dev_flush_surfaces_async,
                                sizeof(RedWorkerMessageFlushSurfacesAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_STOP,
                                handle_dev_stop,
                                sizeof(RedWorkerMessageStop),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_LOADVM_COMMANDS,
                                handle_dev_loadvm_commands,
                                sizeof(RedWorkerMessageLoadvmCommands),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_COMPRESSION,
                                handle_dev_set_compression,
                                sizeof(RedWorkerMessageSetCompression),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
                                handle_dev_set_streaming_video,
                                sizeof(RedWorkerMessageSetStreamingVideo),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_MOUSE_MODE,
                                handle_dev_set_mouse_mode,
                                sizeof(RedWorkerMessageSetMouseMode),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_CHANNEL_CREATE,
                                handle_dev_display_channel_create,
                                sizeof(RedWorkerMessageDisplayChannelCreate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_CHANNEL_CREATE,
                                handle_dev_cursor_channel_create,
                                sizeof(RedWorkerMessageCursorChannelCreate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
                                handle_dev_destroy_surface_wait,
                                sizeof(RedWorkerMessageDestroySurfaceWait),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC,
                                handle_dev_destroy_surface_wait_async,
                                sizeof(RedWorkerMessageDestroySurfaceWaitAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_MEMSLOTS,
                                handle_dev_reset_memslots,
                                sizeof(RedWorkerMessageResetMemslots),
                                DISPATCHER_NONE);
}



static void handle_dev_input(EventListener *listener, uint32_t events)
{
    RedWorker *worker = SPICE_CONTAINEROF(listener, RedWorker, dev_listener);

    dispatcher_handle_recv_read(red_dispatcher_get_dispatcher(worker->red_dispatcher));
}

static void handle_dev_free(EventListener *ctx)
{
    free(ctx);
}

static void red_init(RedWorker *worker, WorkerInitData *init_data)
{
    struct epoll_event event;
    RedWorkerMessage message;
    int epoll;
    Dispatcher *dispatcher;

    ASSERT(sizeof(CursorItem) <= QXL_CURSUR_DEVICE_DATA_SIZE);

    memset(worker, 0, sizeof(RedWorker));
    dispatcher = red_dispatcher_get_dispatcher(init_data->red_dispatcher);
    dispatcher_set_opaque(dispatcher, worker);
    worker->red_dispatcher = init_data->red_dispatcher;
    worker->qxl = init_data->qxl;
    worker->id = init_data->id;
    worker->channel = dispatcher_get_recv_fd(dispatcher);
    register_callbacks(dispatcher);
    worker->pending = init_data->pending;
    worker->dev_listener.refs = 1;
    worker->dev_listener.action = handle_dev_input;
    worker->dev_listener.free = handle_dev_free;
    worker->cursor_visible = TRUE;
    ASSERT(init_data->num_renderers > 0);
    worker->num_renderers = init_data->num_renderers;
    memcpy(worker->renderers, init_data->renderers, sizeof(worker->renderers));
    worker->renderer = RED_RENDERER_INVALID;
    worker->mouse_mode = SPICE_MOUSE_MODE_SERVER;
    worker->image_compression = init_data->image_compression;
    worker->jpeg_state = init_data->jpeg_state;
    worker->zlib_glz_state = init_data->zlib_glz_state;
    worker->streaming_video = init_data->streaming_video;
    ring_init(&worker->current_list);
    image_cache_init(&worker->image_cache);
    image_surface_init(worker);
    drawables_init(worker);
    cursor_items_init(worker);
    red_init_streams(worker);
    stat_init(&worker->add_stat, add_stat_name);
    stat_init(&worker->exclude_stat, exclude_stat_name);
    stat_init(&worker->__exclude_stat, __exclude_stat_name);
#ifdef RED_STATISTICS
    char worker_str[20];
    sprintf(worker_str, "display[%d]", worker->id);
    worker->stat = stat_add_node(INVALID_STAT_REF, worker_str, TRUE);
    worker->wakeup_counter = stat_add_counter(worker->stat, "wakeups", TRUE);
    worker->command_counter = stat_add_counter(worker->stat, "commands", TRUE);
#endif
    if ((epoll = epoll_create(MAX_EPOLL_SOURCES)) == -1) {
        red_error("epoll_create failed, %s", strerror(errno));
    }
    worker->epoll = epoll;

    event.events = EPOLLIN;
    event.data.ptr = &worker->dev_listener;

    if (epoll_ctl(epoll, EPOLL_CTL_ADD, worker->channel, &event) == -1) {
        red_error("add channel failed, %s", strerror(errno));
    }

    red_memslot_info_init(&worker->mem_slots,
                          init_data->num_memslots_groups,
                          init_data->num_memslots,
                          init_data->memslot_gen_bits,
                          init_data->memslot_id_bits,
                          init_data->internal_groupslot_id);

    PANIC_ON(init_data->n_surfaces > NUM_SURFACES);
    worker->n_surfaces = init_data->n_surfaces;

    message = RED_WORKER_MESSAGE_READY;
    write_message(worker->channel, &message);
}

static void red_display_cc_free_glz_drawables(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    red_display_handle_glz_drawables_to_free(dcc);
}

void *red_worker_main(void *arg)
{
    RedWorker worker;

    red_printf("begin");
    ASSERT(MAX_PIPE_SIZE > WIDE_CLIENT_ACK_WINDOW &&
           MAX_PIPE_SIZE > NARROW_CLIENT_ACK_WINDOW); //ensure wakeup by ack message

#if  defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    if (pthread_getcpuclockid(pthread_self(), &clock_id)) {
        red_error("pthread_getcpuclockid failed");
    }
#endif

    red_init(&worker, (WorkerInitData *)arg);
    red_init_quic(&worker);
    red_init_lz(&worker);
    red_init_jpeg(&worker);
    red_init_zlib(&worker);
    worker.epoll_timeout = INF_EPOLL_WAIT;
    for (;;) {
        struct epoll_event events[MAX_EPOLL_SOURCES];
        int num_events;
        struct epoll_event *event;
        struct epoll_event *end;

        worker.epoll_timeout = MIN(red_get_streams_timout(&worker), worker.epoll_timeout);
        num_events = epoll_wait(worker.epoll, events, MAX_EPOLL_SOURCES, worker.epoll_timeout);
        red_handle_streams_timout(&worker);

        if (worker.display_channel) {
            /* during migration, in the dest, the display channel can be initialized
               while the global lz data not since migrate data msg hasn't been
               received yet */
            red_channel_apply_clients(&worker.display_channel->common.base,
                red_display_cc_free_glz_drawables);
        }

        worker.epoll_timeout = INF_EPOLL_WAIT;
        if (num_events == -1) {
            if (errno != EINTR) {
                red_error("poll_wait failed, %s", strerror(errno));
            }
            num_events = 0;
        }

        for (event = events, end = event + num_events;  event < end; event++) {
            EventListener *evt_listener = (EventListener *)event->data.ptr;
            evt_listener->refs++;
        }

        for (event = events, end = event + num_events; event < end; event++) {
            EventListener *evt_listener = (EventListener *)event->data.ptr;

            if (evt_listener->refs > 1) {
                evt_listener->action(evt_listener, event->events);
                if (--evt_listener->refs) {
                    continue;
                }
            }
            red_printf("freeing event listener");
            evt_listener->free(evt_listener);
        }

        if (worker.running) {
            int ring_is_empty;
            red_process_cursor(&worker, MAX_PIPE_SIZE, &ring_is_empty);
            red_process_commands(&worker, MAX_PIPE_SIZE, &ring_is_empty);
        }
        red_push(&worker);
    }
    red_printf("exit");
    return 0;
}

#ifdef DUMP_BITMAP
#include <stdio.h>
static void dump_palette(FILE *f, SpicePalette* plt)
{
    int i;
    for (i = 0; i < plt->num_ents; i++) {
        fwrite(plt->ents + i, sizeof(uint32_t), 1, f);
    }
}

static void dump_line(FILE *f, uint8_t* line, uint16_t n_pixel_bits, int width, int row_size)
{
    int i;
    int copy_bytes_size = SPICE_ALIGN(n_pixel_bits * width, 8) / 8;

    fwrite(line, 1, copy_bytes_size, f);
    if (row_size > copy_bytes_size) {
        // each line should be 4 bytes aligned
        for (i = copy_bytes_size; i < row_size; i++) {
            fprintf(f, "%c", 0);
        }
    }
}

#define RAM_PATH "/tmp/tmpfs"

static void dump_bitmap(RedWorker *worker, SpiceBitmap *bitmap, uint32_t group_id)
{
    static uint32_t file_id = 0;

    char file_str[200];
    int rgb = TRUE;
    uint16_t n_pixel_bits;
    SpicePalette *plt = NULL;
    uint32_t id;
    int row_size;
    uint32_t file_size;
    int alpha = 0;
    uint32_t header_size = 14 + 40;
    uint32_t bitmap_data_offset;
    uint32_t tmp_u32;
    int32_t tmp_32;
    uint16_t tmp_u16;
    FILE *f;
    int i;

    switch (bitmap->format) {
    case SPICE_BITMAP_FMT_1BIT_BE:
    case SPICE_BITMAP_FMT_1BIT_LE:
        rgb = FALSE;
        n_pixel_bits = 1;
        break;
    case SPICE_BITMAP_FMT_4BIT_BE:
    case SPICE_BITMAP_FMT_4BIT_LE:
        rgb = FALSE;
        n_pixel_bits = 4;
        break;
    case SPICE_BITMAP_FMT_8BIT:
        rgb = FALSE;
        n_pixel_bits = 8;
        break;
    case SPICE_BITMAP_FMT_16BIT:
        n_pixel_bits = 16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        n_pixel_bits = 24;
        break;
    case SPICE_BITMAP_FMT_32BIT:
        n_pixel_bits = 32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        n_pixel_bits = 32;
        alpha = 1;
        break;
    default:
        red_error("invalid bitmap format  %u", bitmap->format);
    }

    if (!rgb) {
        if (!bitmap->palette) {
            return; // dont dump masks.
        }
        plt = bitmap->palette;
    }
    row_size = (((bitmap->x * n_pixel_bits) + 31) / 32) * 4;
    bitmap_data_offset = header_size;

    if (plt) {
        bitmap_data_offset += plt->num_ents * 4;
    }
    file_size = bitmap_data_offset + (bitmap->y * row_size);

    id = ++file_id;
    sprintf(file_str, "%s/%u.bmp", RAM_PATH, id);

    f = fopen(file_str, "wb");
    if (!f) {
        red_error("Error creating bmp\n");
        return;
    }

    /* writing the bmp v3 header */
    fprintf(f, "BM");
    fwrite(&file_size, sizeof(file_size), 1, f);
    tmp_u16 = alpha ? 1 : 0;
    fwrite(&tmp_u16, sizeof(tmp_u16), 1, f); // reserved for application
    tmp_u16 = 0;
    fwrite(&tmp_u16, sizeof(tmp_u16), 1, f);
    fwrite(&bitmap_data_offset, sizeof(bitmap_data_offset), 1, f);
    tmp_u32 = header_size - 14;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f); // sub header size
    tmp_32 = bitmap->x;
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);
    tmp_32 = bitmap->y;
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);

    tmp_u16 = 1;
    fwrite(&tmp_u16, sizeof(tmp_u16), 1, f); // color plane
    fwrite(&n_pixel_bits, sizeof(n_pixel_bits), 1, f); // pixel depth

    tmp_u32 = 0;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f); // compression method

    tmp_u32 = 0; //file_size - bitmap_data_offset;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f); // image size
    tmp_32 = 0;
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);
    tmp_u32 = (!plt) ? 0 : plt->num_ents; // plt entries
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f);
    tmp_u32 = 0;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f);

    if (plt) {
        dump_palette(f, plt);
    }
    /* writing the data */
    for (i = 0; i < bitmap->data->num_chunks; i++) {
        SpiceChunk *chunk = &bitmap->data->chunk[i];
        int num_lines = chunk->len / bitmap->stride;
        for (i = 0; i < num_lines; i++) {
            dump_line(f, chunk->data + (i * bitmap->stride), n_pixel_bits, bitmap->x, row_size);
        }
    }
    fclose(f);
}

#endif

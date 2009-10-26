/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
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

#include "qxl_dev.h"
#include "vd_interface.h"
#include "region.h"
#include "red.h"
#include "red_worker.h"
#include "cairo.h"
#include "cairo_canvas.h"
#include "gl_canvas.h"
#include "ogl_ctx.h"
#include "ffmpeg_inc.h"
#include "quic.h"
#include "lz.h"
#include "glz_encoder_dictionary.h"
#include "glz_encoder.h"
#include "stat.h"
#include "reds.h"
#include "ring.h"

//#define COMPRESS_STAT
//#define DUMP_BITMAP
#define STREAM_TRACE
//#define PIPE_DEBUG
#define USE_EXCLUDE_RGN
//#define RED_WORKER_STAT
//#define DRAW_ALL
//#define COMPRESS_DEBUG

//#define UPDATE_AREA_BY_TREE

#define CMD_RING_POLL_TIMEOUT 10 //milli
#define CMD_RING_POLL_RETRIES 200

#define DETACH_TIMEOUT 4000000000 //nano
#define DETACH_SLEEP_DURATION 10000 //micro

#define DISPLAY_CLIENT_TIMEOUT 4000000000 //nano
#define DISPLAY_CLIENT_RETRY_INTERVAL 10000 //micro
#define DISPLAY_MAX_SUB_MESSAGES 10

#define DISPLAY_FREE_LIST_DEFAULT_SIZE 128

#define RED_STREAM_DETACTION_MAX_DELTA ((1000 * 1000 * 1000) / 5) // 1/5 sec
#define RED_STREAM_CONTINUS_MAX_DELTA ((1000 * 1000 * 1000) / 2) // 1/2 sec
#define RED_STREAM_TIMOUT (1000 * 1000 * 1000)
#define RED_STREAM_START_CONDITION 20

#define FPS_TEST_INTERVAL 1
#define MAX_FPS 30

//best bit rate per pixel base on 13000000 bps for frame size 720x576 pixels and 25 fps
#define BEST_BIT_RATE_PER_PIXEL 38
#define WARST_BIT_RATE_PER_PIXEL 4

#define RED_COMPRESS_BUF_SIZE (1024 * 64)

typedef int64_t red_time_t;

static inline red_time_t timespec_to_red_time(struct timespec *time)
{
    return time->tv_sec * (1000 * 1000 * 1000) + time->tv_nsec;
}

#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
static clockid_t clock_id;

typedef unsigned long stat_time_t;

inline stat_time_t stat_now()
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
struct EventListener {
    uint32_t refs;
    event_listener_action_proc action;
};

enum {
    BUF_TYPE_RAW = 1,
    BUF_TYPE_COMPRESS_BUF,
    BUF_TYPE_CHUNK,
};

typedef struct BufDescriptor {
    uint32_t type;
    uint32_t size;
    uint8_t *data;
} BufDescriptor;

enum {
    PIPE_ITEM_TYPE_DRAW,
    PIPE_ITEM_TYPE_MODE,
    PIPE_ITEM_TYPE_INVAL_ONE,
    PIPE_ITEM_TYPE_CURSOR,
    PIPE_ITEM_TYPE_MIGRATE,
    PIPE_ITEM_TYPE_LOCAL_CURSOR,
    PIPE_ITEM_TYPE_SET_ACK,
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
};

typedef struct PipeItem {
    RingItem link;
    int type;
} PipeItem;

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

enum {
    CURSOR_TYPE_INVALID,
    CURSOR_TYPE_DEV,
    CURSOR_TYPE_LOCAL,
};

typedef struct CursorItem {
    PipeItem pipe_data;
    int refs;
    int type;
} CursorItem;

typedef struct LocalCursor {
    CursorItem base;
    Point16 position;
    uint32_t data_size;
    RedCursor red_cursor;
} LocalCursor;

#define MAX_SEND_BUFS 20
#define MAX_BITMAPS 4
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

typedef struct RedChannel RedChannel;
typedef void (*disconnect_channel_proc)(RedChannel *channel);
typedef void (*hold_item_proc)(void *item);
typedef void (*release_item_proc)(RedChannel *channel, void *item);
typedef int (*handle_message_proc)(RedChannel *channel, RedDataHeader *message);

struct RedChannel {
    EventListener listener;
    uint32_t id;
    struct RedWorker *worker;
    RedsStreamContext *peer;
    int migrate;

    Ring pipe;
    uint32_t pipe_size;

    uint32_t client_ack_window;
    uint32_t ack_generation;
    uint32_t client_ack_generation;
    uint32_t messages_window;

    struct {
        int blocked;
        RedDataHeader header;
        union {
            RedSetAck ack;
        } u;
        uint32_t n_bufs;
        BufDescriptor bufs[MAX_SEND_BUFS];
        uint32_t size;
        uint32_t pos;
        void *item;
    } send_data;

    struct {
        uint8_t buf[RECIVE_BUF_SIZE];
        RedDataHeader *message;
        uint8_t *now;
        uint8_t *end;
    } recive_data;

    disconnect_channel_proc disconnect;
    hold_item_proc hold_item;
    release_item_proc release_item;
    handle_message_proc handle_message;
#ifdef RED_STATISTICS
    uint64_t *out_bytes_counter;
#endif
};

typedef struct ImageItem {
    PipeItem link;
    int refs;
    Point pos;
    int width;
    int height;
    int stride;
    int top_down;
    uint8_t data[0];
} ImageItem;

typedef struct Drawable Drawable;

typedef struct Stream Stream;
struct Stream {
    uint8_t refs;
    Drawable *current;
#ifdef STREAM_TRACE
    red_time_t last_time;
    int width;
    int height;
    Rect dest_area;
#endif
    int top_down;
    Stream *next;
    RingItem link;
    AVCodecContext *av_ctx;
    AVFrame *av_frame;
    uint8_t* frame_buf;
    uint8_t* frame_buf_end;
    int bit_rate;
};

typedef struct StreamAgent {
    QRegion vis_region;
    PipeItem create_item;
    PipeItem destroy_item;
    Stream *stream;
    uint64_t lats_send_time;

    int frames;
    int drops;
    int fps;
} StreamAgent;

typedef struct StreamClipItem {
    PipeItem base;
    int refs;
    StreamAgent *stream_agent;
    int clip_type;
    QRegion region;
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

typedef struct  __attribute__ ((__packed__)) RedImage {
    ImageDescriptor descriptor;
    union { // variable length
        Bitmap bitmap;
        QUICData quic;
        LZ_RGBData lz_rgb;
        LZ_PLTData lz_plt;
    };
} RedImage;

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
    uint64_t sync[MAX_CACHE_CLIENTS];
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
    RedWaitForChannels header;
    RedWaitForChannel buf[MAX_CACHE_CLIENTS];
} WaitForChannels;

typedef struct FreeList {
    int res_size;
    RedResorceList *res;
    uint64_t sync[MAX_CACHE_CLIENTS];
    WaitForChannels wait;
} FreeList;

typedef struct DisplayChannel DisplayChannel;

typedef struct  {
    DisplayChannel *display_channel;
    RedCompressBuf *bufs_head;
    RedCompressBuf *bufs_tail;
    jmp_buf jmp_env;
    union {
        struct {
            ADDRESS next;
            long address_delta;
            uint32_t stride;
        } lines_data;
        struct {
            uint8_t* next;
            int src_stride;
            uint32_t dest_stride;
            int lines;
            int max_lines_bunch;
            int input_bufs_pos;
            RedCompressBuf *input_bufs[2];
        } unstable_lines_data;
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

/**********************************/
/* LZ dictionary related entities */
/**********************************/
#define MAX_GLZ_DRAWABLE_INSTANCES 2

typedef struct RedGlzDrawable RedGlzDrawable;

/* for each qxl drawable, there may be serveral instances of lz drawables */
typedef struct GlzDrawableInstanceItem {
    RingItem glz_link;
    RingItem free_link;
    GlzEncDictImageContext *glz_instance;
    RedGlzDrawable         *red_glz_drawable;
} GlzDrawableInstanceItem;

struct RedGlzDrawable {
    RingItem link;    // ordered by the time it was encoded
    QXLDrawable *qxl_drawable;
    Drawable    *drawable;
    GlzDrawableInstanceItem instances_pool[MAX_GLZ_DRAWABLE_INSTANCES];
    Ring instances;
    uint8_t instances_count;
    DisplayChannel *display_channel;
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
} GlzSharedDictionary;

struct DisplayChannel {
    RedChannel base;

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

    StreamAgent stream_agents[NUM_STREAMS];

    /* global lz encoding entities */
    GlzSharedDictionary *glz_dict;
    GlzEncoderContext   *glz;
    GlzData glz_data;

    Ring glz_drawables;               // all the living lz drawable, ordered by encoding time
    Ring glz_drawables_inst_to_free;               // list of instances to be freed
    pthread_mutex_t glz_drawables_inst_to_free_lock;

    struct {
        union {
            RedFill fill;
            RedOpaque opaque;
            RedCopy copy;
            RedTransparent transparent;
            RedAlphaBlend alpha_blend;
            RedCopyBits copy_bits;
            RedBlend blend;
            RedRop3 rop3;
            RedBlackness blackness;
            RedWhiteness whiteness;
            RedInvers invers;
            RedStroke stroke;
            RedText text;
            RedMode mode;
            RedInvalOne inval_one;
            WaitForChannels wait;
            struct {
                RedStreamCreate message;
                uint32_t num_rects;
            } stream_create;
            RedStreamClip stream_clip;
            RedStreamData stream_data;
            RedStreamDestroy stream_destroy;
            RedMigrate migrate;
            DisplayChannelMigrateData migrate_data;
        } u;

        uint32_t bitmap_pos;
        RedImage images[MAX_BITMAPS];

        uint32_t stream_outbuf_size;
        uint8_t *stream_outbuf; // caution stream buffer is also used as compress bufs!!!

        RedCompressBuf *free_compress_bufs;
        RedCompressBuf *used_compress_bufs;

        struct {
            RedSubMessageList sub_list;
            uint32_t sub_messages[DISPLAY_MAX_SUB_MESSAGES];
        } sub_list;
        RedSubMessage sub_header[DISPLAY_MAX_SUB_MESSAGES];

        FreeList free_list;
    } send_data;

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
#endif
};

typedef struct CursorChannel {
    RedChannel base;

    CacheItem *cursor_cache[CURSOR_CACHE_HASH_SIZE];
    Ring cursor_cache_lru;
    long cursor_cache_available;
    uint32_t cursor_cache_items;

    struct {
        union {
            RedCursorInit cursor_init;
            RedCursorSet cursor_set;
            RedCursorMove cursor_move;
            RedCursorTrail cursor_trail;
            RedInvalOne inval_one;
            RedMigrate migrate;
        } u;
    } send_data;
#ifdef RED_STATISTICS
    StatNodeRef stat;
#endif
} CursorChannel;

typedef struct __attribute__ ((__packed__)) LocalImage {
    QXLImage qxl_image;
    uint8_t buf[sizeof(QXLDataChunk *)]; // quic data area
} LocalImage;

typedef struct ImageCacheItem {
    RingItem lru_link;
    uint64_t id;
#ifdef IMAGE_CACHE_AGE
    uint32_t age;
#endif
    struct ImageCacheItem *next;
    cairo_surface_t *surf;
} ImageCacheItem;

#define IMAGE_CACHE_HASH_SIZE 1024

typedef struct ImageCache {
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

struct Drawable {
    uint8_t refs;
    RingItem list_link;
    DrawItem tree_item;
    PipeItem pipe_item;
#ifdef UPDATE_AREA_BY_TREE
    RingItem collect_link;
#endif
    QXLDrawable *qxl_drawable;

    RedGlzDrawable *red_glz_drawable;

    red_time_t creation_time;
    int frames_count;
    Stream *stream;
#ifdef STREAM_TRACE
    int streamable;
#endif
};

typedef struct _Drawable _Drawable;
struct _Drawable {
    union {
        Drawable drawable;
        _Drawable *next;
    } u;
};

typedef struct UpgradeItem {
    PipeItem base;
    int refs;
    Drawable *drawable;
    QRegion region;
} UpgradeItem;

typedef void (*set_access_params_t)(void *canvas, ADDRESS delta);
typedef void (*draw_fill_t)(void *canvas, Rect *bbox, Clip *clip, Fill *fill);
typedef void (*draw_copy_t)(void *canvas, Rect *bbox, Clip *clip, Copy *copy);
typedef void (*draw_opaque_t)(void *canvas, Rect *bbox, Clip *clip, Opaque *opaque);
typedef void (*copy_bits_t)(void *canvas, Rect *bbox, Clip *clip, Point *src_pos);
typedef void (*draw_text_t)(void *canvas, Rect *bbox, Clip *clip, Text *text);
typedef void (*draw_stroke_t)(void *canvas, Rect *bbox, Clip *clip, Stroke *stroke);
typedef void (*draw_rop3_t)(void *canvas, Rect *bbox, Clip *clip, Rop3 *rop3);
typedef void (*draw_blend_t)(void *canvas, Rect *bbox, Clip *clip, Blend *blend);
typedef void (*draw_blackness_t)(void *canvas, Rect *bbox, Clip *clip, Blackness *blackness);
typedef void (*draw_whiteness_t)(void *canvas, Rect *bbox, Clip *clip, Whiteness *whiteness);
typedef void (*draw_invers_t)(void *canvas, Rect *bbox, Clip *clip, Invers *invers);
typedef void (*draw_transparent_t)(void *canvas, Rect *bbox, Clip *clip, Transparent* transparent);
typedef void (*draw_alpha_blend_t)(void *canvas, Rect *bbox, Clip *clip, AlphaBlnd* alpha_blend);
typedef void (*read_pixels_t)(void *canvas, uint8_t *dest, int dest_stride, const Rect *area);
typedef void (*set_top_mask_t)(void *canvas, int num_rect, const Rect *rects);
typedef void (*clear_top_mask_t)(void *canvas);
typedef void (*validate_area_t)(void *canvas, const DrawArea *draw_area, const Rect *area);
typedef void (*destroy_t)(void *canvas);


typedef struct DrawContext {
    void *canvas;
    int top_down;
    set_access_params_t set_access_params;
    draw_fill_t draw_fill;
    draw_copy_t draw_copy;
    draw_opaque_t draw_opaque;
    copy_bits_t copy_bits;
    draw_text_t draw_text;
    draw_stroke_t draw_stroke;
    draw_rop3_t draw_rop3;
    draw_blend_t draw_blend;
    draw_blackness_t draw_blackness;
    draw_whiteness_t draw_whiteness;
    draw_invers_t draw_invers;
    draw_transparent_t draw_transparent;
    draw_alpha_blend_t draw_alpha_blend;
    read_pixels_t read_pixels;
    set_top_mask_t set_top_mask;
    clear_top_mask_t clear_top_mask;
    validate_area_t validate_area;
    destroy_t destroy;
} DrawContext;


#ifdef STREAM_TRACE
typedef struct ItemTrace {
    red_time_t time;
    int frames_count;
    int width;
    int height;
    Rect dest_area;
} ItemTrace;

#define TRACE_ITEMS_SHIFT 3
#define NUM_TRACE_ITEMS (1 << TRACE_ITEMS_SHIFT)
#define ITEMS_TRACE_MASK (NUM_TRACE_ITEMS - 1)

#endif

#define NUM_DRAWABLES 1000

typedef struct RedWorker {
    EventListener dev_listener;
    DisplayChannel *display_channel;
    CursorChannel *cursor_channel;
    QXLInterface *qxl;
    int id;
    int channel;
    int attached;
    int running;
    uint32_t *pending;
    QXLDevInfo dev_info;
    int epoll;
    unsigned int epoll_timeout;
    uint32_t repoll_cmd_ring;
    uint32_t repoll_cursor_ring;
    DrawContext draw_context;
    uint32_t num_renderers;
    uint32_t renderers[RED_MAX_RENDERERS];

    Ring current_list;
    Ring current;
    uint32_t current_size;
    uint32_t drawable_count;
    uint32_t transparent_count;

    uint32_t shadows_count;
    uint32_t containers_count;

    uint32_t bits_unique;

    CursorItem *cursor;
    int cursor_visible;
    Point16 cursor_position;
    uint16_t cursor_trail_length;
    uint16_t cursor_trail_frequency;

    _Drawable drawables[NUM_DRAWABLES];
    _Drawable *free_drawables;

    uint32_t local_images_pos;
    LocalImage local_images[MAX_BITMAPS];

    ImageCache image_cache;

    image_compression_t image_compression;

    uint32_t mouse_mode;

    uint32_t streaming_video;
    Stream streams_buf[NUM_STREAMS];
    Stream *free_streams;
    Ring streams;
#ifdef STREAM_TRACE
    ItemTrace items_trace[NUM_TRACE_ITEMS];
    uint32_t next_item_trace;
#endif

    QuicData quic_data;
    QuicContext *quic;

    LzData lz_data;
    LzContext  *lz;

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

pthread_mutex_t avcodec_lock = PTHREAD_MUTEX_INITIALIZER;

static void red_draw_qxl_drawable(RedWorker *worker, QXLDrawable *drawable);
static void red_current_flush(RedWorker *worker);
static void display_channel_push(RedWorker *worker);
#ifdef DRAW_ALL
#define red_update_area(worker, rect)
#define red_draw_drawable(worker, item)
#else
static void red_draw_drawable(RedWorker *worker, Drawable *item);
static void red_update_area(RedWorker *worker, const Rect *area);
#endif
static void red_release_cursor(RedWorker *worker, CursorItem *cursor);
static inline void release_drawable(RedWorker *worker, Drawable *item);
static void red_display_release_stream(DisplayChannel *display, StreamAgent *agent);
#ifdef STREAM_TRACE
static inline void red_detach_stream(RedWorker *worker, Stream *stream);
#endif
static void red_stop_stream(RedWorker *worker, Stream *stream);
static inline void red_stream_maintenance(RedWorker *worker, Drawable *candidate, Drawable *sect);
static inline void red_begin_send_massage(RedChannel *channel, void *item);
static inline void display_begin_send_massage(DisplayChannel *channel, void *item);
static void red_receive(RedChannel *channel);
static void red_release_pixmap_cache(DisplayChannel *channel);
static void red_release_glz(DisplayChannel *channel);
static void red_freeze_glz(DisplayChannel *channel);
static void display_channel_push_release(DisplayChannel *channel, uint8_t type, uint64_t id,
                                         uint64_t* sync_data);
static void red_display_release_stream_clip(DisplayChannel* channel, StreamClipItem *item);
static int red_display_free_some_independent_glz_drawables(DisplayChannel *channel);
static void red_display_free_glz_drawable(DisplayChannel *channel, RedGlzDrawable *drawable);
static void reset_rate(StreamAgent *stream_agent);

#ifdef DUMP_BITMAP
static void dump_bitmap(RedWorker *worker, Bitmap *bitmap);
#endif

#ifdef COMPRESS_STAT
static void print_compress_stats(DisplayChannel *display_channel)
{
    if (!display_channel) {
        return;
    }
    red_printf("==> Compression stats for display %u", display_channel->base.id);
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
    red_printf("LZ       \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total)
               );
    red_printf("-------------------------------------------------------------------");
    red_printf("Total    \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count + display_channel->glz_stat.count +
                                                display_channel->quic_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size +
                                 display_channel->glz_stat.orig_size +
                                 display_channel->quic_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size +
                                 display_channel->glz_stat.comp_size +
                                 display_channel->quic_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total +
                                    display_channel->glz_stat.total +
                                    display_channel->quic_stat.total)
               );
}

#endif

char *draw_type_to_str(UINT8 type)
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

static void show_qxl_drawable(RedWorker *worker, QXLDrawable *drawable, const char *prefix)
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
        red_error("bad rawable type");
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
           draw_item->base.rgn.bbox.top,
           draw_item->base.rgn.bbox.left,
           draw_item->base.rgn.bbox.bottom,
           draw_item->base.rgn.bbox.right);
}

static inline void red_pipe_item_init(PipeItem *item, int type)
{
    ring_item_init(&item->link);
    item->type = type;
}

static inline void red_pipe_add(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);
    channel->pipe_size++;
    ring_add(&channel->pipe, &item->link);
}

static inline void red_pipe_add_after(RedChannel *channel, PipeItem *item, PipeItem *pos)
{
    ASSERT(channel && pos);
    channel->pipe_size++;
    ring_add_after(&item->link, &pos->link);
}

static inline int pipe_item_is_linked(PipeItem *item)
{
    return ring_item_is_linked(&item->link);
}

static inline void pipe_item_remove(PipeItem *item)
{
    ring_remove(&item->link);
}

static inline void red_pipe_add_tail(RedChannel *channel, PipeItem *item)
{
    ASSERT(channel);
    channel->pipe_size++;
    ring_add_before(&item->link, &channel->pipe);
}

static void red_pipe_add_verb(RedChannel* channel, uint16_t verb)
{
    VerbItem *item = malloc(sizeof(*item));
    if (!item) {
        PANIC("malloc failed");
    }
    red_pipe_item_init(&item->base, PIPE_ITEM_TYPE_VERB);
    item->verb = verb;
    red_pipe_add(channel, &item->base);
}

static void red_pipe_add_type(RedChannel* channel, int pipe_item_type)
{
    PipeItem *item = malloc(sizeof(*item));
    if (!item) {
        PANIC("malloc failed");
    }
    red_pipe_item_init(item, pipe_item_type);
    red_pipe_add(channel, item);
}

static inline void red_pipe_add_drawable(RedWorker *worker, Drawable *drawable)
{
    if (!worker->display_channel) {
        return;
    }
    drawable->refs++;
    red_pipe_add(&worker->display_channel->base, &drawable->pipe_item);
}

static inline void red_pipe_add_drawable_after(RedWorker *worker, Drawable *drawable,
                                               Drawable *pos_after)
{
    if (!worker->display_channel) {
        return;
    }

    if (!pos_after || !pipe_item_is_linked(&pos_after->pipe_item)) {
        red_pipe_add_drawable(worker, drawable);
        return;
    }
    drawable->refs++;
    red_pipe_add_after(&worker->display_channel->base, &drawable->pipe_item, &pos_after->pipe_item);
}

static inline void red_pipe_remove_drawable(RedWorker *worker, Drawable *drawable)
{
    if (ring_item_is_linked(&drawable->pipe_item.link)) {
        worker->display_channel->base.pipe_size--;
        ring_remove(&drawable->pipe_item.link);
        release_drawable(worker, drawable);
    }
}

static inline void red_pipe_add_image_item(RedWorker *worker, ImageItem *item)
{
    if (!worker->display_channel) {
        return;
    }
    item->refs++;
    red_pipe_add(&worker->display_channel->base, &item->link);
}

static inline uint64_t channel_message_serial(RedChannel *channel)
{
    return channel->send_data.header.serial;
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
        region_destroy(&item->region);
        free(item);
    }
}

static void red_pipe_clear(RedChannel *channel)
{
    PipeItem *item;

    ASSERT(channel);
    while ((item = (PipeItem *)ring_get_head(&channel->pipe))) {
        ring_remove(&item->link);
        switch (item->type) {
        case PIPE_ITEM_TYPE_DRAW:
            release_drawable(channel->worker, CONTAINEROF(item, Drawable, pipe_item));
            break;
        case PIPE_ITEM_TYPE_CURSOR:
            red_release_cursor(channel->worker, (CursorItem *)item);
            break;
        case PIPE_ITEM_TYPE_UPGRADE:
            release_upgrade_item(channel->worker, (UpgradeItem *)item);
            break;
        case PIPE_ITEM_TYPE_PIXMAP_RESET:
        case PIPE_ITEM_TYPE_PIXMAP_SYNC:
        case PIPE_ITEM_TYPE_INVAL_ONE:
        case PIPE_ITEM_TYPE_MODE:
        case PIPE_ITEM_TYPE_MIGRATE:
        case PIPE_ITEM_TYPE_SET_ACK:
        case PIPE_ITEM_TYPE_CURSOR_INIT:
        case PIPE_ITEM_TYPE_VERB:
        case PIPE_ITEM_TYPE_MIGRATE_DATA:
        case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
        case PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE:
            free(item);
            break;
        case PIPE_ITEM_TYPE_IMAGE:
            release_image_item((ImageItem *)item);
            break;
        case PIPE_ITEM_TYPE_STREAM_CREATE:
            red_display_release_stream((DisplayChannel *)channel,
                                       CONTAINEROF(item, StreamAgent, create_item));
            break;
        case PIPE_ITEM_TYPE_STREAM_CLIP:
            red_display_release_stream_clip((DisplayChannel *)channel, (StreamClipItem*)item);
            break;
        case PIPE_ITEM_TYPE_STREAM_DESTROY:
            red_display_release_stream((DisplayChannel *)channel,
                                       CONTAINEROF(item, StreamAgent, destroy_item));
            break;
        }
    }
    channel->pipe_size = 0;
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

static void red_reset_palette_cache(DisplayChannel *display_channel)
{
    red_palette_cache_reset(display_channel, CLIENT_PALETTE_CACHE_SIZE);
}

static void red_reset_cursor_cache(CursorChannel *channel)
{
    red_cursor_cache_reset(channel, CLIENT_CURSOR_CACHE_SIZE);
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

static inline void free_qxl_drawable(RedWorker *worker, QXLDrawable *drawable)
{
    if (drawable->bitmap_offset) {
        PHYSICAL *addr = (PHYSICAL *)((uint8_t *)drawable + drawable->bitmap_offset);
        if (*addr) {
            free((uint8_t *)*addr + worker->dev_info.phys_delta);
        }
    }
    worker->qxl->release_resource(worker->qxl, &drawable->release_info);
}

static inline void release_drawable(RedWorker *worker, Drawable *item)
{
    if (!--item->refs) {
#ifdef STREAM_TRACE
        ASSERT(!item->stream);
#else
        if (item->stream) {
            red_stop_stream(worker, item->stream);
        }
#endif
        ASSERT(!item->tree_item.shadow);
        region_destroy(&item->tree_item.base.rgn);

        if (item->red_glz_drawable) {
            item->red_glz_drawable->drawable = NULL;
        } else { // no refernce to the qxl drawable left
            free_qxl_drawable(worker, item->qxl_drawable);
        }
        free_drawable(worker, item);
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

#ifdef STREAM_TRACE
static inline void red_add_item_trace(RedWorker *worker, Drawable *item)
{
    ItemTrace *trace;
    if (!item->streamable) {
        return;
    }

    trace = &worker->items_trace[worker->next_item_trace++ & ITEMS_TRACE_MASK];
    trace->time = item->creation_time;
    trace->frames_count = item->frames_count;
    Rect* src_area = &item->qxl_drawable->u.copy.src_area;
    trace->width = src_area->right - src_area->left;
    trace->height = src_area->bottom - src_area->top;
    trace->dest_area = item->qxl_drawable->bbox;
}

#endif

static inline void current_remove_drawable(RedWorker *worker, Drawable *item)
{
    worker->drawable_count--;
    if (item->tree_item.effect != QXL_EFFECT_OPAQUE) {
        worker->transparent_count--;
    }
#ifdef STREAM_TRACE
    if (item->stream) {
        red_detach_stream(worker, item->stream);
    } else {
        red_add_item_trace(worker, item);
    }
#endif
    remove_shadow(worker, &item->tree_item);
    ring_remove(&item->tree_item.base.siblings_link);
    ring_remove(&item->list_link);
    release_drawable(worker, item);
    worker->current_size--;
}

static void remove_drawable(RedWorker *worker, Drawable *item)
{
    red_pipe_remove_drawable(worker, item);
    current_remove_drawable(worker, item);
}

static inline void current_remove(RedWorker *worker, TreeItem *item)
{
    TreeItem *now = item;

    for (;;) {
        Container *container = now->container;
        RingItem *ring_item;

        if (now->type == TREE_ITEM_TYPE_DRAWABLE) {
            ring_item = now->siblings_link.prev;
            remove_drawable(worker, CONTAINEROF(now, Drawable, tree_item));
        } else {
            Container *container = (Container *)now;

            ASSERT(now->type == TREE_ITEM_TYPE_CONTAINER);

            if ((ring_item = ring_get_head(&container->items))) {
                now = CONTAINEROF(ring_item, TreeItem, siblings_link);
                continue;
            }
            ring_item = now->siblings_link.prev;
            current_remove_container(worker, container);
        }
        if (now == item) {
            return;
        }

        if ((ring_item = ring_next(&container->items, ring_item))) {
            now = CONTAINEROF(ring_item, TreeItem, siblings_link);
        } else {
            now = (TreeItem *)container;
        }
    }
}

static void current_tree_for_each(RedWorker *worker, void (*f)(TreeItem *, void *), void * data)
{
    Ring *ring = &worker->current;
    RingItem *ring_item;
    Ring *top_ring;

    if (!(ring_item = ring_get_head(ring))) {
        return;
    }
    top_ring = ring;

    for (;;) {
        TreeItem *now = CONTAINEROF(ring_item, TreeItem, siblings_link);

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

static void red_current_clear(RedWorker *worker)
{
    RingItem *ring_item;

    while ((ring_item = ring_get_head(&worker->current))) {
        TreeItem *now = CONTAINEROF(ring_item, TreeItem, siblings_link);
        current_remove(worker, now);
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
    const Drawable *drawable = CONTAINEROF(draw_item, Drawable, tree_item);
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

static void show_current(RedWorker *worker)
{
    if (ring_is_empty(&worker->current)) {
        red_printf("TEST: TREE: EMPTY");
        return;
    }
    current_tree_for_each(worker, __show_current, NULL);
}

#else
#define print_rgn(a, b)
#define print_draw_private(a, b)
#define show_current(a)
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

static inline Ring *ring_of(RedWorker *worker, TreeItem *item)
{
    return (item->container) ? &item->container->items : &worker->current;
}

static inline int __contained_by(RedWorker *worker, TreeItem *item, Ring *ring)
{
    ASSERT(item && ring);
    do {
        Ring *now = ring_of(worker, item);
        if (now == ring) {
            return TRUE;
        }
    } while ((item = (TreeItem *)item->container));

    return FALSE;
}

static inline void __exclude_region(RedWorker *worker, TreeItem *item, QRegion *rgn,
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
                int32_t x = item->rgn.bbox.left;
                int32_t y = item->rgn.bbox.top;

                region_exclude(&draw->base.rgn, &and_rgn);
                shadow = draw->shadow;
                region_offset(&and_rgn, shadow->base.rgn.bbox.left - x,
                              shadow->base.rgn.bbox.top - y);
                region_exclude(&shadow->base.rgn, &and_rgn);
                region_and(&and_rgn, &shadow->on_hold);
                if (!region_is_empty(&and_rgn)) {
                    region_exclude(&shadow->on_hold, &and_rgn);
                    region_or(rgn, &and_rgn);
                    // in flat representation of current, shadow is always his owner next
                    if (!__contained_by(worker, (TreeItem*)shadow, *top_ring)) {
                        *top_ring = ring_of(worker, (TreeItem*)shadow);
                    }
                }
            } else {
                if (frame_candidate) {
                    Drawable *drawable = CONTAINEROF(draw, Drawable, tree_item);
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
                        *top_ring = ring_of(worker, (TreeItem*)shadow);
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

#ifdef USE_EXCLUDE_RGN

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
        TreeItem *now = CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;

        ASSERT(!region_is_empty(&now->rgn));

        if (region_intersects(rgn, &now->rgn)) {
            print_base_item("EXCLUDE2", now);
            __exclude_region(worker, now, rgn, &top_ring, frame_candidate);
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

#else

static void exclude_region(RedWorker *worker, Ring *ring, RingItem *ring_item, QRegion *rgn)
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
        TreeItem *now = CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;

        ASSERT(!region_is_empty(&now->rgn));

        if (region_test(rgn, &now->rgn, REGION_TEST_SHARED)) {
            print_base_item("EXCLUDE2", now);
            __exclude_region(worker, now, rgn, &top_ring);
            print_base_item("EXCLUDE3", now);

            if (region_is_empty(&now->rgn)) {
                ASSERT(now->type != TREE_ITEM_TYPE_SHADOW);
                ring_item = now->siblings_link.prev;
                print_base_item("EXCLUDE_REMOVE", now);
                current_remove(worker, now);
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

        while (!(ring_item = ring_next(ring, ring_item))) {
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

#endif

static inline Container *__new_container(RedWorker *worker, DrawItem *item)
{
    Container *container = malloc(sizeof(Container));
    if (!container) {
        return NULL;
    }
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
    ring_add_after(&drawable->tree_item.base.siblings_link, pos);
    ring_add(&worker->current_list, &drawable->list_link);
    drawable->refs++;
}

#ifdef USE_EXCLUDE_RGN

static int is_equal_path(ADDRESS p1, ADDRESS p2, long phys_delta)
{
    QXLPath *path1;
    QXLPath *path2;
    QXLDataChunk *chunk1;
    QXLDataChunk *chunk2;
    uint8_t *data1;
    uint8_t *data2;
    int size;
    int size1;
    int size2;

    ASSERT(p1 && p2);

    path1 = (QXLPath *)(p1 + phys_delta);
    path2 = (QXLPath *)(p2 + phys_delta);

    if ((size = path1->data_size) != path2->data_size) {
        return FALSE;
    }
    ASSERT(size);

    chunk1 = &path1->chunk;
    size1 = chunk1->data_size;
    data1 = chunk1->data;

    chunk2 = &path2->chunk;
    size2 = chunk2->data_size;
    data2 = chunk2->data;

    for (;;) {
        int now = MIN(size1, size2);
        ASSERT(now);
        if (memcmp(data1, data2, now)) {
            return FALSE;
        }
        if (!(size -= now)) {
            return TRUE;
        }
        if ((size1 -= now) == 0) {
            ASSERT(chunk1->next_chunk)
            chunk1 = (QXLDataChunk *)(chunk1->next_chunk + phys_delta);
            size1 = chunk1->data_size;
            data1 = chunk1->data;
        } else {
            data1 += now;
        }

        if ((size2 -= now) == 0) {
            ASSERT(chunk2->next_chunk)
            chunk2 = (QXLDataChunk *)(chunk2->next_chunk + phys_delta);
            size2 = chunk2->data_size;
            data2 = chunk2->data;
        } else {
            data2 += now;
        }
    }
}

// partial imp
static int is_equal_brush(Brush *b1, Brush *b2)
{
    return b1->type == b2->type && b1->type == BRUSH_TYPE_SOLID && b1->u.color == b1->u.color;
}

// partial imp
static int is_equal_line_attr(LineAttr *a1, LineAttr *a2)
{
    return a1->flags == a2->flags && a1->join_style == a2->join_style &&
           a1->end_style == a2->end_style && a1->style_nseg == a2->style_nseg &&
           a1->width == a2->width && a1->miter_limit == a2->miter_limit &&
           a1->style_nseg == 0;
}

static inline int rect_is_equal(const Rect *r1, const Rect *r2)
{
    return r1->top == r2->top && r1->left == r2->left &&
           r1->bottom == r2->bottom && r1->right == r2->right;
}

// partial imp
static int is_same_geometry(QXLDrawable *d1, QXLDrawable *d2, long phys_delta)
{
    if (d1->type != d2->type) {
        return FALSE;
    }

    switch (d1->type) {
    case QXL_DRAW_STROKE:
        return is_equal_line_attr(&d1->u.stroke.attr, &d2->u.stroke.attr) &&
               is_equal_path(d1->u.stroke.path, d2->u.stroke.path, phys_delta);
    case QXL_DRAW_FILL:
        return rect_is_equal(&d1->bbox, &d2->bbox);
    default:
        return FALSE;
    }
}

static int is_same_drawable(QXLDrawable *d1, QXLDrawable *d2, long phys_delta)
{
    if (!is_same_geometry(d1, d2, phys_delta)) {
        return FALSE;
    }

    switch (d1->type) {
    case QXL_DRAW_STROKE:
        return is_equal_brush(&d1->u.stroke.brush, &d2->u.stroke.brush);
    case QXL_DRAW_FILL:
        return is_equal_brush(&d1->u.fill.brush, &d2->u.fill.brush);
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
#ifdef STREAM_TRACE
        ASSERT(!ring_item_is_linked(&stream->link));
#else
        ring_remove(&stream->link);
#endif
        pthread_mutex_lock(&avcodec_lock);
        avcodec_close(stream->av_ctx);
        pthread_mutex_unlock(&avcodec_lock);
        av_free(stream->av_ctx);
        av_free(stream->av_frame);
        free(stream->frame_buf);
        red_free_stream(worker, stream);
    }
}

#ifdef STREAM_TRACE
static inline void red_detach_stream(RedWorker *worker, Stream *stream)
{
    ASSERT(stream->current && stream->current->stream);
    ASSERT(stream->current->stream == stream);
    stream->current->stream = NULL;
    stream->current = NULL;
}

static StreamClipItem *__new_stream_clip(DisplayChannel* channel, StreamAgent *agent)
{
    StreamClipItem *item = (StreamClipItem *)malloc(sizeof(*item));
    if (!item) {
        PANIC("alloc failed");
    }
    red_pipe_item_init((PipeItem *)item, PIPE_ITEM_TYPE_STREAM_CLIP);

    item->stream_agent = agent;
    agent->stream->refs++;
    item->refs = 1;
    return item;
}

static void push_stream_clip_by_drawable(DisplayChannel* channel, StreamAgent *agent,
                                         Drawable *drawable)
{
    StreamClipItem *item = __new_stream_clip(channel, agent);
    if (!item) {
        PANIC("alloc failed");
    }

    if (drawable->qxl_drawable->clip.type == CLIP_TYPE_NONE) {
        region_init(&item->region);
        item->clip_type = CLIP_TYPE_NONE;
    } else {
        item->clip_type = CLIP_TYPE_RECTS;
        region_clone(&item->region, &drawable->tree_item.base.rgn);
    }
    red_pipe_add((RedChannel*)channel, (PipeItem *)item);
}

static void push_stream_clip(DisplayChannel* channel, StreamAgent *agent)
{
    StreamClipItem *item = __new_stream_clip(channel, agent);
    if (!item) {
        PANIC("alloc failed");
    }
    item->clip_type = CLIP_TYPE_RECTS;
    region_clone(&item->region, &agent->vis_region);
    red_pipe_add((RedChannel*)channel, (PipeItem *)item);
}

static void red_display_release_stream_clip(DisplayChannel* channel, StreamClipItem *item)
{
    if (!--item->refs) {
        red_display_release_stream(channel, item->stream_agent);
        region_destroy(&item->region);
        free(item);
    }
}

static void red_attach_stream(RedWorker *worker, Drawable *drawable, Stream *stream)
{
    DisplayChannel *channel;
    ASSERT(!drawable->stream && !stream->current);
    ASSERT(drawable && stream);
    stream->current = drawable;
    drawable->stream = stream;
    stream->last_time = drawable->creation_time;

    if ((channel = worker->display_channel)) {
        StreamAgent *agent = &channel->stream_agents[stream - worker->streams_buf];
        if (!region_is_equal(&agent->vis_region, &drawable->tree_item.base.rgn)) {
            region_destroy(&agent->vis_region);
            region_clone(&agent->vis_region, &drawable->tree_item.base.rgn);
            push_stream_clip_by_drawable(channel, agent, drawable);
        }
    }
}

#endif

static void red_stop_stream(RedWorker *worker, Stream *stream)
{
    DisplayChannel *channel;
#ifdef STREAM_TRACE
    ASSERT(ring_item_is_linked(&stream->link));
    ASSERT(!stream->current);
#else
    ASSERT(stream->current && stream->current->stream);
    stream->current->stream = NULL;
    stream->current = NULL;
#endif

    if ((channel = worker->display_channel)) {
        StreamAgent *stream_agent;
        stream_agent = &channel->stream_agents[stream - worker->streams_buf];
        region_clear(&stream_agent->vis_region);
        ASSERT(!pipe_item_is_linked(&stream_agent->destroy_item));
        stream->refs++;
        red_pipe_add(&channel->base, &stream_agent->destroy_item);
    }
#ifdef STREAM_TRACE
    ring_remove(&stream->link);
#endif
    red_release_stream(worker, stream);
}

#ifdef STREAM_TRACE
static inline void red_detach_stream_gracefully(RedWorker *worker, Stream *stream)
{
    DisplayChannel *channel;
    ASSERT(stream->current);

    if ((channel = worker->display_channel) && !pipe_item_is_linked(&stream->current->pipe_item)) {
        UpgradeItem *upgrade_item;

        if (!(upgrade_item = (UpgradeItem *)malloc(sizeof(*upgrade_item)))) {
            PANIC("malloc failed");
        }
        upgrade_item->refs = 1;
        red_pipe_item_init(&upgrade_item->base, PIPE_ITEM_TYPE_UPGRADE);
        upgrade_item->drawable = stream->current;
        upgrade_item->drawable->refs++;
        region_clone(&upgrade_item->region, &upgrade_item->drawable->tree_item.base.rgn);
        red_pipe_add((RedChannel *)channel, &upgrade_item->base);
    }
    red_detach_stream(worker, stream);
}

#else
static inline void red_stop_stream_gracefully(RedWorker *worker, Stream *stream)
{
    ASSERT(stream->current);
    if (worker->display_channel && !pipe_item_is_linked(&stream->current->pipe_item)) {
        UpgradeItem *item;
        if ((item = (UpgradeItem *)malloc(sizeof(*item)))) {
            item->refs = 1;
            red_pipe_item_init(&item->base, PIPE_ITEM_TYPE_UPGRADE);
            item->drawable = stream->current;
            item->drawable->refs++;
            region_clone(&item->region, &item->drawable->tree_item.base.rgn);
            red_pipe_add((RedChannel *)worker->display_channel, &item->base);
        }
    }
    red_stop_stream(worker, stream);
}

#endif

#ifdef STREAM_TRACE
static void red_detach_streams_behind(RedWorker *worker, QRegion *region)
{
    Ring *ring = &worker->streams;
    RingItem *item = ring_get_head(ring);
    DisplayChannel *channel = worker->display_channel;

    while (item) {
        Stream *stream = CONTAINEROF(item, Stream, link);
        item = ring_next(ring, item);

        if (channel) {
            StreamAgent *agent = &channel->stream_agents[stream - worker->streams_buf];
            if (region_intersects(&agent->vis_region, region)) {
                region_clear(&agent->vis_region);
                push_stream_clip(channel, agent);
                if (stream->current) {
                    red_detach_stream_gracefully(worker, stream);
                }
            }
        } else if (stream->current && region_intersects(&stream->current->tree_item.base.rgn,
                                                        region)) {
            red_detach_stream(worker, stream);
        }
    }
}

#else
static void red_stop_streams_behind(RedWorker *worker, QRegion *region)
{
    Ring *ring = &worker->streams;
    RingItem *item = ring_get_head(ring);

    while (item) {
        Stream *stream = CONTAINEROF(item, Stream, link);
        stream->refs++;
        if (stream->current && region_intersects(region, &stream->current->tree_item.base.rgn)) {
            red_stop_stream_gracefully(worker, stream);
        }
        item = ring_next(ring, item);
        red_release_stream(worker, stream);
    }
}

#endif

static void red_streams_update_clip(RedWorker *worker, Drawable *drawable)
{
    DisplayChannel *channel;
    Ring *ring;
    RingItem *item;

    if (!(channel = worker->display_channel)) {
        return;
    }

    ring = &worker->streams;
    item = ring_get_head(ring);

    while (item) {
        Stream *stream = CONTAINEROF(item, Stream, link);
        StreamAgent *agent;

        item = ring_next(ring, item);

        agent = &channel->stream_agents[stream - worker->streams_buf];

        if (stream->current == drawable) {
            continue;
        }

        if (region_intersects(&agent->vis_region, &drawable->tree_item.base.rgn)) {
            region_exclude(&agent->vis_region, &drawable->tree_item.base.rgn);
            push_stream_clip(channel, agent);
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

        stream = CONTAINEROF(item, Stream, link);
#ifdef STREAM_TRACE
        red_time_t delta = (stream->last_time + RED_STREAM_TIMOUT) - now;

        if (delta < 1000 * 1000) {
            return 0;
        }
        timout = MIN(timout, (unsigned int)(delta / (1000 * 1000)));
#else
        if (stream->current) {
            red_time_t delta = (stream->current->creation_time + RED_STREAM_TIMOUT) - now;
            if (delta < 1000 * 1000) {
                return 0;
            }
            timout = MIN(timout, (unsigned int)(delta / (1000 * 1000)));
        }
#endif
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
        Stream *stream = CONTAINEROF(item, Stream, link);
#ifdef STREAM_TRACE
        item = ring_next(ring, item);
        if (now >= (stream->last_time + RED_STREAM_TIMOUT)) {
            if (stream->current) {
                red_detach_stream_gracefully(worker, stream);
            }
            red_stop_stream(worker, stream);
        }
#else
        stream->refs++;
        if (stream->current && now >= (stream->current->creation_time + RED_STREAM_TIMOUT)) {
            red_stop_stream_gracefully(worker, stream);
        }
        item = ring_next(ring, item);
        red_release_stream(worker, stream);
#endif
    }
}

static void red_display_release_stream(DisplayChannel *display, StreamAgent *agent)
{
    ASSERT(agent->stream);
    red_release_stream(display->base.worker, agent->stream);
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

static int get_bit_rate(int width, int height)
{
    uint64_t bit_rate = width * height * BEST_BIT_RATE_PER_PIXEL;
    if (IS_LOW_BANDWIDTH()) {
        bit_rate = MIN(bitrate_per_sec * 70 / 100, bit_rate);
        bit_rate = MAX(bit_rate, width * height * WARST_BIT_RATE_PER_PIXEL);
    }
    return bit_rate;
}

static void red_dispaly_create_stream(DisplayChannel *display, Stream *stream)
{
    StreamAgent *agent = &display->stream_agents[stream - display->base.worker->streams_buf];
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
    reset_rate(agent);
    red_pipe_add(&display->base, &agent->create_item);
}

static AVCodecContext *red_init_video_encoder(int width, int height)
{
    AVCodec *codec;
    AVCodecContext *ctx;
    int r;

    codec = avcodec_find_encoder(CODEC_ID_MJPEG);

    if (!codec) {
        red_printf("codec not found");
        return NULL;
    }

    if (!(ctx = avcodec_alloc_context())) {
        red_printf("alloc ctx failed");
        return NULL;
    }
    ctx->bit_rate = get_bit_rate(width, height);
    ASSERT(width % 2 == 0);
    ASSERT(height % 2 == 0);
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = (AVRational){1, MAX_FPS};
    ctx->gop_size = 10;
    ctx->max_b_frames = 0;
    ctx->pix_fmt = PIX_FMT_YUVJ420P;

    pthread_mutex_lock(&avcodec_lock);
    r = avcodec_open(ctx, codec);
    pthread_mutex_unlock(&avcodec_lock);
    if (r < 0) {
        red_printf("avcodec open failed");
        av_free(ctx);
        return NULL;
    }
    return ctx;
}

static void red_create_stream(RedWorker *worker, Drawable *drawable)
{
    AVCodecContext *av_ctx;
    Stream *stream;
    AVFrame *frame;
    uint8_t* frame_buf;
    Rect* src_rect;
    int stream_width;
    int stream_height;
    int pict_size;

    ASSERT(!drawable->stream);

    if (!(stream = red_alloc_stream(worker))) {
        return;
    }

    ASSERT(drawable->qxl_drawable->type == QXL_DRAW_COPY);
    src_rect = &drawable->qxl_drawable->u.copy.src_area;
    stream_width = ALIGN(src_rect->right - src_rect->left, 2);
    stream_height = ALIGN(src_rect->bottom - src_rect->top, 2);

    if (!(av_ctx = red_init_video_encoder(stream_width, stream_height))) {
        goto error_1;
    }

    if (!(frame = avcodec_alloc_frame())) {
        goto error_2;
    }

    if ((pict_size = avpicture_get_size(av_ctx->pix_fmt, stream_width, stream_height)) < 0) {
        goto error_3;
    }

    if (!(frame_buf = malloc(pict_size))) {
        goto error_3;
    }

    if (avpicture_fill((AVPicture *)frame, frame_buf, av_ctx->pix_fmt, stream_width,
                       stream_height) < 0) {
        goto error_4;
    }

    ring_add(&worker->streams, &stream->link);
    stream->current = drawable;
#ifdef STREAM_TRACE
    stream->last_time = drawable->creation_time;
    stream->width = src_rect->right - src_rect->left;
    stream->height = src_rect->bottom - src_rect->top;
    stream->dest_area = drawable->qxl_drawable->bbox;
#endif
    stream->refs = 1;
    stream->av_ctx = av_ctx;
    stream->bit_rate = av_ctx->bit_rate;
    stream->av_frame = frame;
    stream->frame_buf = frame_buf;
    stream->frame_buf_end = frame_buf + pict_size;
    QXLImage *qxl_image = (QXLImage *)(drawable->qxl_drawable->u.copy.src_bitmap +
                                       worker->dev_info.phys_delta);
    stream->top_down = !!(qxl_image->bitmap.flags & BITMAP_TOP_DOWN);
    drawable->stream = stream;

    if (worker->display_channel) {
        red_dispaly_create_stream(worker->display_channel, stream);
    }

    return;

error_4:
    free(frame_buf);

error_3:
    av_free(frame);

error_2:
    pthread_mutex_lock(&avcodec_lock);
    avcodec_close(av_ctx);
    pthread_mutex_unlock(&avcodec_lock);
    av_free(av_ctx);

error_1:
    red_free_stream(worker, stream);
}

static void red_disply_start_streams(DisplayChannel *display_channel)
{
    Ring *ring = &display_channel->base.worker->streams;
    RingItem *item = ring;

    while ((item = ring_next(ring, item))) {
        Stream *stream = CONTAINEROF(item, Stream, link);
        red_dispaly_create_stream(display_channel, stream);
    }
}

static void red_display_init_streams(DisplayChannel *display)
{
    int i;
    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &display->stream_agents[i];
        agent->stream = &display->base.worker->streams_buf[i];
        region_init(&agent->vis_region);
        red_pipe_item_init(&agent->create_item, PIPE_ITEM_TYPE_STREAM_CREATE);
        red_pipe_item_init(&agent->destroy_item, PIPE_ITEM_TYPE_STREAM_DESTROY);
    }
}

static void red_display_destroy_streams(DisplayChannel *display)
{
    int i;
    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &display->stream_agents[i];
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

#ifdef STREAM_TRACE

static inline int __red_is_next_stream_frame(const Drawable *candidate,
                                             const int other_src_width,
                                             const int other_src_height,
                                             const Rect *other_dest,
                                             const red_time_t other_time,
                                             const Stream *stream,
                                             unsigned long phys_delta)
{
    QXLDrawable *qxl_drawable;

    if (candidate->creation_time - other_time >
            (stream ? RED_STREAM_CONTINUS_MAX_DELTA : RED_STREAM_DETACTION_MAX_DELTA)) {
        return FALSE;
    }

    qxl_drawable = candidate->qxl_drawable;

    if (!rect_is_equal(&qxl_drawable->bbox, other_dest)) {
        return FALSE;
    }

    Rect* candidate_src = &qxl_drawable->u.copy.src_area;
    if (candidate_src->right - candidate_src->left != other_src_width ||
        candidate_src->bottom - candidate_src->top != other_src_height) {
        return FALSE;
    }

    if (stream) {
        QXLImage *qxl_image = (QXLImage *)(qxl_drawable->u.copy.src_bitmap + phys_delta);

        if (stream->top_down != !!(qxl_image->bitmap.flags & BITMAP_TOP_DOWN)) {
            return FALSE;
        }
    }
    return TRUE;
}

static inline int red_is_next_stream_frame(const Drawable *candidate, const Drawable *prev,
                                           const unsigned long phys_delta)
{
    if (!candidate->streamable) {
        return FALSE;
    }

    Rect* prev_src = &prev->qxl_drawable->u.copy.src_area;
    return __red_is_next_stream_frame(candidate, prev_src->right - prev_src->left,
                                      prev_src->bottom - prev_src->top,
                                      &prev->qxl_drawable->bbox, prev->creation_time,
                                      prev->stream, phys_delta);
}

#else

static inline int red_is_next_stream_frame(Drawable *candidate, Drawable *prev,
                                           unsigned long phys_delta)
{
    QXLImage *qxl_image;
    QXLDrawable *qxl_drawable;
    QXLDrawable *prev_qxl_drawable;

    if (candidate->creation_time - prev->creation_time >
            ((prev->stream) ? RED_STREAM_CONTINUS_MAX_DELTA : RED_STREAM_DETACTION_MAX_DELTA)) {
        return FALSE;
    }

    qxl_drawable = candidate->qxl_drawable;
    prev_qxl_drawable = prev->qxl_drawable;
    if (qxl_drawable->type != QXL_DRAW_COPY || prev_qxl_drawable->type != QXL_DRAW_COPY) {
        return FALSE;
    }

    if (!rect_is_equal(&qxl_drawable->bbox, &prev_qxl_drawable->bbox)) {
        return FALSE;
    }

    if (!rect_is_same_size(&qxl_drawable->u.copy.src_area, &prev_qxl_drawable->u.copy.src_area)) {
        return FALSE;
    }

    if (qxl_drawable->u.copy.rop_decriptor != ROPD_OP_PUT ||
                                    prev_qxl_drawable->u.copy.rop_decriptor != ROPD_OP_PUT) {
        return FALSE;
    }

    qxl_image = (QXLImage *)(qxl_drawable->u.copy.src_bitmap + phys_delta);

    if (qxl_image->descriptor.type != IMAGE_TYPE_BITMAP) {
        return FALSE;
    }

    if (prev->stream && prev->stream->top_down != !!(qxl_image->bitmap.flags & BITMAP_TOP_DOWN)) {
        return FALSE;
    }

    return TRUE;
}

#endif

static void reset_rate(StreamAgent *stream_agent)
{
    Stream *stream = stream_agent->stream;
    AVCodecContext *new_ctx;
    int rate;

    rate = get_bit_rate(stream->width, stream->height);
    if (rate == stream->bit_rate) {
        return;
    }

    int stream_width = ALIGN(stream->width, 2);
    int stream_height = ALIGN(stream->height, 2);

    new_ctx = red_init_video_encoder(stream_width, stream_height);
    if (!new_ctx) {
        red_printf("craete ctx failed");
        return;
    }

    avcodec_close(stream->av_ctx);
    av_free(stream->av_ctx);
    stream->av_ctx = new_ctx;
    stream->bit_rate = rate;
}

static inline void pre_stream_item_swap(RedWorker *worker, Stream *stream)
{
    ASSERT(stream->current);

    if (!worker->display_channel || !IS_LOW_BANDWIDTH()) {
        return;
    }

    int index = stream - worker->streams_buf;
    StreamAgent *agent = &worker->display_channel->stream_agents[index];

    if (pipe_item_is_linked(&stream->current->pipe_item)) {
        ++agent->drops;
    }

    if (agent->frames / agent->fps < FPS_TEST_INTERVAL) {
        agent->frames++;
        return;
    }

    double drop_factor = ((double)agent->frames - (double)agent->drops) / (double)agent->frames;

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

static inline void red_stream_maintenance(RedWorker *worker, Drawable *candidate, Drawable *prev)
{
    Stream *stream;

    if (candidate->stream) {
        return;
    }

#ifdef STREAM_TRACE
    if (!red_is_next_stream_frame(candidate, prev, worker->dev_info.phys_delta)) {
        return;
    }
#else
    if (!worker->streaming_video ||
                        !red_is_next_stream_frame(candidate, prev, worker->dev_info.phys_delta)) {
        return;
    }
#endif

    if ((stream = prev->stream)) {
#ifdef STREAM_TRACE
        pre_stream_item_swap(worker, stream);
        red_detach_stream(worker, stream);
        prev->streamable = FALSE; //prevent item trace
        red_attach_stream(worker, candidate, stream);
#else
        prev->stream = NULL;
        candidate->stream = stream;
        stream->current = candidate;

        if (!region_is_equal(&stream->region, &candidate->tree_item.base.rgn)) {
            region_destroy(&stream->region);
            region_clone(&stream->region, &candidate->tree_item.base.rgn);
            if (worker->display_channel) {
                int index = stream - worker->streams_buf;
                StreamAgent *stream_agent = &worker->display_channel->stream_agents[index];
                if (!pipe_item_is_linked(&stream_agent->clip_item)) {
                    stream->refs++;
                    red_pipe_add((RedChannel*)worker->display_channel, &stream_agent->clip_item);
                }
            }
        }
#endif
    } else if ((candidate->frames_count = prev->frames_count + 1) ==
                                                                RED_STREAM_START_CONDITION) {
        red_create_stream(worker, candidate);
    }
}

static inline int red_current_add_equal(RedWorker *worker, DrawItem *item, TreeItem *other)
{
    DrawItem *other_draw_item;
    Drawable *drawable;
    Drawable *other_drawable;
    QXLDrawable *qxl_drawable;
    QXLDrawable *other_qxl_drawable;

    if (other->type != TREE_ITEM_TYPE_DRAWABLE) {
        return FALSE;
    }
    other_draw_item = (DrawItem *)other;

    if (item->shadow || other_draw_item->shadow || item->effect != other_draw_item->effect) {
        return FALSE;
    }

    drawable = CONTAINEROF(item, Drawable, tree_item);
    other_drawable = CONTAINEROF(other_draw_item, Drawable, tree_item);

    qxl_drawable = drawable->qxl_drawable;
    other_qxl_drawable = other_drawable->qxl_drawable;

    if (item->effect == QXL_EFFECT_OPAQUE) {
        int add_after = !!other_drawable->stream;
        red_stream_maintenance(worker, drawable, other_drawable);
        __current_add_drawable(worker, drawable, &other->siblings_link);
        if (add_after) {
            red_pipe_add_drawable_after(worker, drawable, other_drawable);
        } else {
            red_pipe_add_drawable(worker, drawable);
        }
        remove_drawable(worker, other_drawable);
        return TRUE;
    }

    switch (item->effect) {
    case QXL_EFFECT_REVERT_ON_DUP:
        if (is_same_drawable(qxl_drawable, other_qxl_drawable, worker->dev_info.phys_delta)) {
            if (!ring_item_is_linked(&other_drawable->pipe_item.link)) {
                red_pipe_add_drawable(worker, drawable);
            }
            remove_drawable(worker, other_drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_OPAQUE_BRUSH:
        if (is_same_geometry(qxl_drawable, other_qxl_drawable, worker->dev_info.phys_delta)) {
            __current_add_drawable(worker, drawable, &other->siblings_link);
            remove_drawable(worker, other_drawable);
            red_pipe_add_drawable(worker, drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_NOP_ON_DUP:
        if (is_same_drawable(qxl_drawable, other_qxl_drawable, worker->dev_info.phys_delta)) {
            return TRUE;
        }
        break;
    }
    return FALSE;
}

#ifdef STREAM_TRACE

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
        Stream *stream = CONTAINEROF(item, Stream, link);
        if (!stream->current && __red_is_next_stream_frame(drawable,
                                                           stream->width,
                                                           stream->height,
                                                           &stream->dest_area,
                                                           stream->last_time,
                                                           stream,
                                                           worker->dev_info.phys_delta)) {
            red_attach_stream(worker, drawable, stream);
            return;
        }
        item = ring_next(ring, item);
    }

    trace = worker->items_trace;
    trace_end = trace + NUM_TRACE_ITEMS;
    for (; trace < trace_end; trace++) {
        if (__red_is_next_stream_frame(drawable, trace->width, trace->height, &trace->dest_area,
                                       trace->time, NULL, worker->dev_info.phys_delta)) {
            if ((drawable->frames_count = trace->frames_count + 1) == RED_STREAM_START_CONDITION) {
                red_create_stream(worker, drawable);
            }
            return;
        }
    }
}

static void red_reset_stream_trace(RedWorker *worker)
{
    Ring *ring = &worker->streams;
    RingItem *item = ring_get_head(ring);

    while (item) {
        Stream *stream = CONTAINEROF(item, Stream, link);
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

#endif

static inline int red_current_add(RedWorker *worker, Drawable *drawable)
{
    DrawItem *item = &drawable->tree_item;
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif
    RingItem *now;
    Ring *ring = &worker->current;
    QRegion exclude_rgn;
    RingItem *exclude_base = NULL;

    print_base_item("ADD", &item->base);
    ASSERT(!region_is_empty(&item->base.rgn));
    worker->current_size++;
    region_init(&exclude_rgn);
    now = ring_next(ring, ring);

    while (now) {
        TreeItem *sibling = CONTAINEROF(now, TreeItem, siblings_link);
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
#ifdef STREAM_TRACE
        red_use_stream_trace(worker, drawable);
#endif
        red_streams_update_clip(worker, drawable);
    } else {
#ifdef STREAM_TRACE
        red_detach_streams_behind(worker, &drawable->tree_item.base.rgn);
#else
        red_stop_streams_behind(worker, &drawable->tree_item.base.rgn);
#endif
    }
    region_destroy(&exclude_rgn);
    __current_add_drawable(worker, drawable, ring);
    stat_add(&worker->add_stat, start_time);
    return TRUE;
}

#else

static inline void __handle_remove_shadow(RedWorker *worker, TreeItem *item)
{
    Shadow *shadow;
    Ring *ring;

    while (item->type == TREE_ITEM_TYPE_CONTAINER) {
        if (!(item = (TreeItem *)ring_get_tail(&((Container *)item)->items))) {
            return;
        }
    }

    if (item->type != TREE_ITEM_TYPE_DRAWABLE || !(shadow = ((DrawItem *)item)->shadow)) {
        return;
    }
    print_base_item("SHADW", &shadow->base);
    ring = (shadow->base.container) ? &shadow->base.container->items : &worker->current;
    exclude_region(worker, ring, ring_next(ring, &shadow->base.siblings_link), &shadow->on_hold);
    region_clear(&shadow->on_hold);
}

static inline int red_current_add(RedWorker *worker, Drawable *drawable)
{
    DrawItem *item = &drawable->tree_item;
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif
    RingItem *now;
    Ring *ring = &worker->current;

    print_base_item("ADD", &item->base);
    ASSERT(!region_is_empty(&item->base.rgn));
    worker->current_size++;
    now = ring_next(ring, ring);

    while (now) {
        TreeItem *sibling = CONTAINEROF(now, TreeItem, siblings_link);
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
            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) && item->effect == QXL_EFFECT_OPAQUE) {
                print_base_item("CONTAIN", sibling);
                __handle_remove_shadow(worker, sibling);
                now = now->prev;
                current_remove(worker, sibling);
                now = ring_next(ring, now);
                continue;
            }

            if (!(test_res & REGION_TEST_LEFT_EXCLUSIVE) && is_opaque_item(sibling)) {
                Container *container;

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
                        return FALSE;
                    }
                    item->base.container = container;
                    ring = &container->items;
                }
            }
        }
        if (item->effect == QXL_EFFECT_OPAQUE) {
            QRegion exclude_rgn;
            region_clone(&exclude_rgn, &item->base.rgn);
            exclude_region(worker, ring, now, &exclude_rgn);
            region_destroy(&exclude_rgn);
        }
        break;
    }
    __current_add_drawable(worker, drawable, ring);
    stat_add(&worker->add_stat, start_time);
    return TRUE;
}

#endif

static void add_clip_rects(QRegion *rgn, PHYSICAL data, unsigned long phys_delta)
{
    while (data) {
        QXLDataChunk *chunk;
        Rect *now;
        Rect *end;
        chunk = (QXLDataChunk *)(data + phys_delta);
        now = (Rect *)chunk->data;
        end = now + chunk->data_size / sizeof(Rect);

        for (; now < end; now++) {
            Rect* r = (Rect *)now;

            ASSERT(now->top == r->top && now->left == r->left &&
                   now->bottom == r->bottom && now->right == r->right);
#ifdef PIPE_DEBUG
            printf("TEST: DRAWABLE: RECT: %u %u %u %u\n", r->top, r->left, r->bottom, r->right);
#endif
            region_add(rgn, r);
        }
        data = chunk->next_chunk;
    }
}

static inline Shadow *__new_shadow(RedWorker *worker, Drawable *item, Point *delta)
{
    ASSERT(delta->x || delta->y);

    Shadow *shadow = malloc(sizeof(Shadow));
    if (!shadow) {
        return NULL;
    }
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

static inline int red_current_add_with_shadow(RedWorker *worker, Drawable *item, Point *delta)
{
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif
    Ring *ring;

    Shadow *shadow = __new_shadow(worker, item, delta);
    if (!shadow) {
        stat_add(&worker->add_stat, start_time);
        return FALSE;
    }
    print_base_item("ADDSHADOW", &item->tree_item.base);
    worker->current_size++;
    ring = &worker->current;
    // item and his shadow must initially be placed in the same container.
    // for now putting them on root.
#ifdef STREAM_TRACE
    red_detach_streams_behind(worker, &shadow->base.rgn);
#else
    red_stop_streams_behind(worker, &shadow->base.rgn);
#endif
    ring_add(ring, &shadow->base.siblings_link);
    __current_add_drawable(worker, item, ring);
    if (item->tree_item.effect == QXL_EFFECT_OPAQUE) {
        QRegion exclude_rgn;
        region_clone(&exclude_rgn, &item->tree_item.base.rgn);
#ifdef USE_EXCLUDE_RGN
        exclude_region(worker, ring, &shadow->base.siblings_link, &exclude_rgn, NULL, NULL);
#else
        exclude_region(worker, ring, &shadow->base.siblings_link, &exclude_rgn);
#endif
        region_destroy(&exclude_rgn);
        red_streams_update_clip(worker, item);
    } else {
#ifdef STREAM_TRACE
        red_detach_streams_behind(worker, &item->tree_item.base.rgn);
#else
        red_stop_streams_behind(worker, &item->tree_item.base.rgn);
#endif
    }
    stat_add(&worker->add_stat, start_time);
    return TRUE;
}

static inline int has_shadow(QXLDrawable *drawable)
{
    return drawable->type == QXL_COPY_BITS;
}

#ifdef STREAM_TRACE
static inline void red_update_streamable(RedWorker *worker, Drawable *drawable,
                                         QXLDrawable *qxl_drawable)
{
    QXLImage *qxl_image;

    if (!worker->streaming_video) {
        return;
    }

    if (drawable->tree_item.effect != QXL_EFFECT_OPAQUE ||
                                        qxl_drawable->type != QXL_DRAW_COPY ||
                                        qxl_drawable->u.copy.rop_decriptor != ROPD_OP_PUT) {
        return;
    }

    qxl_image = (QXLImage *)(qxl_drawable->u.copy.src_bitmap + worker->dev_info.phys_delta);
    if (qxl_image->descriptor.type != IMAGE_TYPE_BITMAP) {
        return;
    }

    drawable->streamable = TRUE;
}

#endif

static inline int red_current_add_qxl(RedWorker *worker, Drawable *drawable,
                                      QXLDrawable *qxl_drawable)
{
    int ret;

    if (has_shadow(qxl_drawable)) {
        Point delta;

#ifdef RED_WORKER_STAT
        ++worker->add_with_shadow_count;
#endif
        delta.x = qxl_drawable->u.copy_bits.src_pos.x - qxl_drawable->bbox.left;
        delta.y = qxl_drawable->u.copy_bits.src_pos.y - qxl_drawable->bbox.top;
        ret = red_current_add_with_shadow(worker, drawable, &delta);
    } else {
#ifdef STREAM_TRACE
        red_update_streamable(worker, drawable, qxl_drawable);
#endif
        ret = red_current_add(worker, drawable);
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

static void red_get_area(RedWorker *worker, const Rect *area, uint8_t *dest, int dest_stride,
                         int update)
{
    if (update) {
        red_update_area(worker, area);
    }

    worker->draw_context.read_pixels(worker->draw_context.canvas, dest, dest_stride, area);
}

static inline int red_handle_self_bitmap(RedWorker *worker, QXLDrawable *drawable)
{
    QXLImage *image;
    int32_t width;
    int32_t height;
    uint8_t *dest;
    int dest_stride;
    PHYSICAL *addr;

    if (!drawable->bitmap_offset) {
        return TRUE;
    }

    width = drawable->bbox.right - drawable->bbox.left;
    height = drawable->bbox.bottom - drawable->bbox.top;
    dest_stride = width * sizeof(uint32_t);

    if (!(image = malloc(sizeof(QXLImage) + height * dest_stride))) {
        red_printf("alloc failed");
        return FALSE;
    }
    dest = (uint8_t *)(image + 1);

    image->descriptor.type = IMAGE_TYPE_BITMAP;
    image->descriptor.flags = 0;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    image->bitmap.flags = QXL_BITMAP_DIRECT | (worker->draw_context.top_down ?
                                               QXL_BITMAP_TOP_DOWN : 0);
    image->bitmap.format = BITMAP_FMT_32BIT;
    image->bitmap.stride = dest_stride;
    image->descriptor.width = image->bitmap.x = width;
    image->descriptor.height = image->bitmap.y = height;
    image->bitmap.data = (PHYSICAL)(dest - worker->dev_info.phys_delta);
    image->bitmap.palette = 0;

    red_get_area(worker, &drawable->bitmap_area, dest, dest_stride, TRUE);

    addr = (PHYSICAL *)((uint8_t *)drawable + drawable->bitmap_offset);
    ASSERT(*addr == 0);
    *addr = (PHYSICAL)((uint8_t *)image - worker->dev_info.phys_delta);
    return TRUE;
}

static void free_one_drawable(RedWorker *worker, int force_glz_free)
{
    RingItem *ring_item = ring_get_tail(&worker->current_list);
    Drawable *drawable;
    Container *container;

    ASSERT(ring_item);
    drawable = CONTAINEROF(ring_item, Drawable, list_link);
    if (drawable->red_glz_drawable && force_glz_free) {
        ASSERT(worker->display_channel);
        red_display_free_glz_drawable(worker->display_channel, drawable->red_glz_drawable);
    }
    red_draw_drawable(worker, drawable);
    container = drawable->tree_item.base.container;
    current_remove_drawable(worker, drawable);
    container_cleanup(worker, container);
}

static Drawable *get_drawable(RedWorker *worker, uint8_t effect, QXLDrawable *qxl_drawable)
{
    Drawable *drawable;
    struct timespec time;

    while (!(drawable = alloc_drawable(worker))) {
        free_one_drawable(worker, FALSE);
    }
    memset(drawable, 0, sizeof(Drawable));
    drawable->refs = 1;
    clock_gettime(CLOCK_MONOTONIC, &time);
    drawable->creation_time = timespec_to_red_time(&time);
#ifdef PIPE_DEBUG
    drawable->tree_item.base.id = ++worker->last_id;
#endif
    ring_item_init(&drawable->list_link);
#ifdef UPDATE_AREA_BY_TREE
    ring_item_init(&drawable->collect_link);
#endif
    ring_item_init(&drawable->tree_item.base.siblings_link);
    drawable->tree_item.base.type = TREE_ITEM_TYPE_DRAWABLE;
    region_init(&drawable->tree_item.base.rgn);
    drawable->tree_item.effect = effect;
    red_pipe_item_init(&drawable->pipe_item, PIPE_ITEM_TYPE_DRAW);
    drawable->qxl_drawable = qxl_drawable;

    return drawable;
}

static inline void red_process_drawable(RedWorker *worker, QXLDrawable *drawable)
{
    Drawable *item = get_drawable(worker, drawable->effect, drawable);

    ASSERT(item);
    region_add(&item->tree_item.base.rgn, &drawable->bbox);
#ifdef PIPE_DEBUG
    printf("TEST: DRAWABLE: id %u type %s effect %u bbox %u %u %u %u\n",
           item->tree_item.base.id,
           draw_type_to_str(drawable->type),
           item->tree_item.effect,
           drawable->bbox.top, drawable->bbox.left, drawable->bbox.bottom, drawable->bbox.right);
#endif

    if (drawable->clip.type == CLIP_TYPE_RECTS) {
        QRegion rgn;

        region_init(&rgn);
        add_clip_rects(&rgn, drawable->clip.data + OFFSETOF(QXLClipRects, chunk),
                       worker->dev_info.phys_delta);
        region_and(&item->tree_item.base.rgn, &rgn);
        region_destroy(&rgn);
    } else if (drawable->clip.type == CLIP_TYPE_PATH) {
        item->tree_item.effect = QXL_EFFECT_BLEND;
#ifdef PIPE_DEBUG
        printf("TEST: DRAWABLE: QXL_CLIP_TYPE_PATH\n");
#endif
    }

    if (region_is_empty(&item->tree_item.base.rgn)) {
        release_drawable(worker, item);
        return;
    }

    if (!red_handle_self_bitmap(worker, drawable)) {
        release_drawable(worker, item);
        return;
    }

    if (red_current_add_qxl(worker, item, drawable)) {
        worker->drawable_count++;
        if (item->tree_item.effect != QXL_EFFECT_OPAQUE) {
            worker->transparent_count++;
        }
        red_pipe_add_drawable(worker, item);
#ifdef DRAW_ALL
        red_draw_qxl_drawable(worker, drawable);
#endif
    }
    release_drawable(worker, item);
}

static void localize_path(PHYSICAL *in_path, long phys_delta)
{
    QXLPath *path;
    uint8_t *data;
    QXLDataChunk *chunk;

    ASSERT(in_path && *in_path);
    path = (QXLPath *)(*in_path + phys_delta);
    data = malloc(sizeof(UINT32) + path->data_size);
    ASSERT(data);
    *in_path = (PHYSICAL)data;
    *(UINT32 *)data = path->data_size;
    data += sizeof(UINT32);
    chunk = &path->chunk;
    do {
        memcpy(data, chunk->data, chunk->data_size);
        data += chunk->data_size;
        chunk = chunk->next_chunk ? (QXLDataChunk *)(chunk->next_chunk + phys_delta) : NULL;
    } while (chunk);
}

static void unlocalize_path(PHYSICAL *path)
{
    ASSERT(path && *path);
    free((void *)*path);
    *path = 0;
}

static void localize_str(PHYSICAL *in_str, long phys_delta)
{
    QXLString *qxl_str = (QXLString *)(*in_str + phys_delta);
    QXLDataChunk *chunk;
    String *str;
    uint8_t *dest;

    ASSERT(in_str);
    str = malloc(sizeof(UINT32) + qxl_str->data_size);
    ASSERT(str);
    *in_str = (PHYSICAL)str;
    str->length = qxl_str->length;
    str->flags = qxl_str->flags;
    dest = str->data;
    chunk = &qxl_str->chunk;
    for (;;) {
        memcpy(dest, chunk->data, chunk->data_size);
        if (!chunk->next_chunk) {
            return;
        }
        dest += chunk->data_size;
        chunk = (QXLDataChunk *)(chunk->next_chunk + phys_delta);
    }
}

static void unlocalize_str(PHYSICAL *str)
{
    ASSERT(str && *str);
    free((void *)*str);
    *str = 0;
}

static void localize_clip(Clip *clip, long phys_delta)
{
    switch (clip->type) {
    case CLIP_TYPE_NONE:
        return;
    case CLIP_TYPE_RECTS: {
        QXLClipRects *clip_rects;
        QXLDataChunk *chunk;
        uint8_t *data;
        clip_rects = (QXLClipRects *)(clip->data + phys_delta);
        chunk = &clip_rects->chunk;
        ASSERT(clip->data);
        data = malloc(sizeof(UINT32) + clip_rects->num_rects * sizeof(Rect));
        ASSERT(data);
        clip->data = (PHYSICAL)data;
        *(UINT32 *)(data) = clip_rects->num_rects;
        data += sizeof(UINT32);
        do {
            memcpy(data, chunk->data, chunk->data_size);
            data += chunk->data_size;
            chunk = chunk->next_chunk ? (QXLDataChunk *)(chunk->next_chunk + phys_delta) : NULL;
        } while (chunk);
        break;
    }
    case CLIP_TYPE_PATH:
        localize_path(&clip->data, phys_delta);
        break;
    default:
        red_printf("invalid clip type");
    }
}

static void unlocalize_clip(Clip *clip)
{
    switch (clip->type) {
    case CLIP_TYPE_NONE:
        return;
    case CLIP_TYPE_RECTS:
        free((void *)clip->data);
        clip->data = 0;
        break;
    case CLIP_TYPE_PATH:
        unlocalize_path(&clip->data);
        break;
    default:
        red_printf("invalid clip type");
    }
}

static LocalImage *alloc_local_image(RedWorker *worker)
{
    ASSERT(worker->local_images_pos < MAX_BITMAPS);
    return &worker->local_images[worker->local_images_pos++];
}

static ImageCacheItem *image_cache_find(ImageCache *cache, UINT64 id)
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

static int image_cache_hit(ImageCache *cache, UINT64 id)
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
    cairo_surface_destroy(item->surf);
    free(item);
#ifndef IMAGE_CACHE_AGE
    cache->num_items--;
#endif
}

#define IMAGE_CACHE_MAX_ITEMS 2

static void image_cache_put(void *opaque, uint64_t id, cairo_surface_t *surface)
{
    ImageCache *cache = (ImageCache *)opaque;
    ImageCacheItem *item;

#ifndef IMAGE_CACHE_AGE
    if (cache->num_items == IMAGE_CACHE_MAX_ITEMS) {
        ImageCacheItem *tail = (ImageCacheItem *)ring_get_tail(&cache->lru);
        ASSERT(tail);
        image_cache_remove(cache, tail);
    }
#endif

    if (!(item = (ImageCacheItem *)malloc(sizeof(ImageCacheItem)))) {
        red_error("alloc failed");
    }
    item->id = id;
#ifdef IMAGE_CACHE_AGE
    item->age = cache->age;
#else
    cache->num_items++;
#endif
    item->surf = cairo_surface_reference(surface);
    ring_item_init(&item->lru_link);

    item->next = cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE];
    cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE] = item;

    ring_add(&cache->lru, &item->lru_link);
}

static cairo_surface_t *image_cache_get(void *opaque, uint64_t id)
{
    ImageCache *cache = (ImageCache *)opaque;

    ImageCacheItem *item = image_cache_find(cache, id);
    if (!item) {
        red_error("not found");
    }
    return cairo_surface_reference(item->surf);
}

static void image_cache_init(ImageCache *cache)
{
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

static void localize_bitmap(RedWorker *worker, PHYSICAL *in_bitmap, long phys_delta)
{
    QXLImage *image;
    QXLImage *local_image;

    ASSERT(in_bitmap && *in_bitmap);
    image = (QXLImage *)(*in_bitmap + phys_delta);
    local_image = (QXLImage *)alloc_local_image(worker);
    *local_image = *image;
    *in_bitmap = (PHYSICAL)local_image;
    local_image->descriptor.flags = 0;

    if (image_cache_hit(&worker->image_cache, local_image->descriptor.id)) {
        local_image->descriptor.type = IMAGE_TYPE_FROM_CACHE;
        return;
    }

    switch (local_image->descriptor.type) {
    case IMAGE_TYPE_QUIC: {
        QXLDataChunk **chanks_head;
#ifdef IMAGE_CACHE_AGE
        local_image->descriptor.flags |= IMAGE_CACHE_ME;
#else
        if (local_image->descriptor.width * local_image->descriptor.height >= 640 * 480) {
            local_image->descriptor.flags |= IMAGE_CACHE_ME;
        }
#endif
        chanks_head = (QXLDataChunk **)local_image->quic.data;
        *chanks_head = (QXLDataChunk *)image->quic.data;
        break;
    }
    case IMAGE_TYPE_BITMAP:
        if (image->bitmap.flags & QXL_BITMAP_DIRECT) {
            local_image->bitmap.data = (PHYSICAL)(image->bitmap.data + phys_delta);
        } else {
            PHYSICAL src_data;
            int size = image->bitmap.y * image->bitmap.stride;
            uint8_t *data = malloc(size);
            ASSERT(data);
            local_image->bitmap.data = (PHYSICAL)data;
            src_data = image->bitmap.data;

            while (size) {
                QXLDataChunk *chunk;
                int cp_size;

                ASSERT(src_data);
                chunk = (QXLDataChunk *)(src_data + phys_delta);
                cp_size = MIN(chunk->data_size, size);
                memcpy(data, chunk->data, cp_size);
                data += cp_size;
                size -= cp_size;
                src_data = chunk->next_chunk;
            }
        }

        if (local_image->bitmap.palette) {
            local_image->bitmap.palette = (local_image->bitmap.palette + phys_delta);
        }
        break;
    default:
        red_error("invalid image type");
    }
}

static void unlocalize_bitmap(PHYSICAL *bitmap)
{
    QXLImage *image;

    ASSERT(bitmap && *bitmap);
    image = (QXLImage *)*bitmap;
    *bitmap = 0;

    switch (image->descriptor.type) {
    case IMAGE_TYPE_BITMAP:
        if (!(image->bitmap.flags & QXL_BITMAP_DIRECT)) {
            free((void *)image->bitmap.data);
        }
        break;
    case IMAGE_TYPE_QUIC:
    case IMAGE_TYPE_FROM_CACHE:
        *bitmap = 0;
        break;
    default:
        red_error("invalid image type %u", image->descriptor.type);
    }
}

static void localize_brush(RedWorker *worker, Brush *brush, long phys_delta)
{
    if (brush->type == BRUSH_TYPE_PATTERN) {
        localize_bitmap(worker, &brush->u.pattern.pat, phys_delta);
    }
}

static void unlocalize_brush(Brush *brush)
{
    if (brush->type == BRUSH_TYPE_PATTERN) {
        unlocalize_bitmap(&brush->u.pattern.pat);
    }
}

static void localize_mask(RedWorker *worker, QMask *mask, long phys_delta)
{
    if (mask->bitmap) {
        localize_bitmap(worker, &mask->bitmap, phys_delta);
    }
}

static void unlocalize_mask(QMask *mask)
{
    if (mask->bitmap) {
        unlocalize_bitmap(&mask->bitmap);
    }
}

static void localize_attr(LineAttr *attr, long phys_delta)
{
    if (attr->style_nseg) {
        uint8_t *buf;
        uint8_t *data;

        ASSERT(attr->style);
        buf = (uint8_t *)(attr->style + phys_delta);
        data = malloc(attr->style_nseg * sizeof(uint32_t));
        ASSERT(data);
        memcpy(data, buf, attr->style_nseg * sizeof(uint32_t));
        attr->style = (PHYSICAL)data;
    }
}

static void unlocalize_attr(LineAttr *attr)
{
    if (attr->style_nseg) {
        free((void *)attr->style);
        attr->style = 0;
    }
}

static void red_draw_qxl_drawable(RedWorker *worker, QXLDrawable *drawable)
{
    Clip clip = drawable->clip;

    worker->local_images_pos = 0;
    image_cache_eaging(&worker->image_cache);

    worker->draw_context.set_access_params(worker->draw_context.canvas,
                                           worker->dev_info.phys_delta);    // todo: use delta
                                                                            //       instead of
                                                                            //       localize_x
    localize_clip(&clip, worker->dev_info.phys_delta);
    switch (drawable->type) {
    case QXL_DRAW_FILL: {
        Fill fill = drawable->u.fill;
        localize_brush(worker, &fill.brush, worker->dev_info.phys_delta);
        localize_mask(worker, &fill.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_fill(worker->draw_context.canvas, &drawable->bbox, &clip, &fill);
        unlocalize_mask(&fill.mask);
        unlocalize_brush(&fill.brush);
        break;
    }
    case QXL_DRAW_OPAQUE: {
        Opaque opaque = drawable->u.opaque;
        localize_brush(worker, &opaque.brush, worker->dev_info.phys_delta);
        localize_bitmap(worker, &opaque.src_bitmap, worker->dev_info.phys_delta);
        localize_mask(worker, &opaque.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_opaque(worker->draw_context.canvas, &drawable->bbox, &clip,
                                         &opaque);
        unlocalize_mask(&opaque.mask);
        unlocalize_bitmap(&opaque.src_bitmap);
        unlocalize_brush(&opaque.brush);
        break;
    }
    case QXL_DRAW_COPY: {
        Copy copy = drawable->u.copy;
        localize_bitmap(worker, &copy.src_bitmap, worker->dev_info.phys_delta);
        localize_mask(worker, &copy.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_copy(worker->draw_context.canvas, &drawable->bbox, &clip, &copy);
        unlocalize_mask(&copy.mask);
        unlocalize_bitmap(&copy.src_bitmap);
        break;
    }
    case QXL_DRAW_TRANSPARENT: {
        Transparent transparent = drawable->u.transparent;
        localize_bitmap(worker, &transparent.src_bitmap, worker->dev_info.phys_delta);
        worker->draw_context.draw_transparent(worker->draw_context.canvas, &drawable->bbox, &clip,
                                              &transparent);
        unlocalize_bitmap(&transparent.src_bitmap);
        break;
    }
    case QXL_DRAW_ALPHA_BLEND: {
        AlphaBlnd alpha_blend = drawable->u.alpha_blend;
        localize_bitmap(worker, &alpha_blend.src_bitmap, worker->dev_info.phys_delta);
        worker->draw_context.draw_alpha_blend(worker->draw_context.canvas, &drawable->bbox, &clip,
                                              &alpha_blend);
        unlocalize_bitmap(&alpha_blend.src_bitmap);
        break;
    }
    case QXL_COPY_BITS: {
        worker->draw_context.copy_bits(worker->draw_context.canvas, &drawable->bbox, &clip,
                                       &drawable->u.copy_bits.src_pos);
        break;
    }
    case QXL_DRAW_BLEND: {
        Blend blend = drawable->u.blend;
        localize_bitmap(worker, &blend.src_bitmap, worker->dev_info.phys_delta);
        localize_mask(worker, &blend.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_blend(worker->draw_context.canvas, &drawable->bbox, &clip,
                                        &blend);
        unlocalize_mask(&blend.mask);
        unlocalize_bitmap(&blend.src_bitmap);
        break;
    }
    case QXL_DRAW_BLACKNESS: {
        Blackness blackness = drawable->u.blackness;
        localize_mask(worker, &blackness.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_blackness(worker->draw_context.canvas, &drawable->bbox, &clip,
                                            &blackness);
        unlocalize_mask(&blackness.mask);
        break;
    }
    case QXL_DRAW_WHITENESS: {
        Whiteness whiteness = drawable->u.whiteness;
        localize_mask(worker, &whiteness.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_whiteness(worker->draw_context.canvas, &drawable->bbox, &clip,
                                            &whiteness);
        unlocalize_mask(&whiteness.mask);
        break;
    }
    case QXL_DRAW_INVERS: {
        Invers invers = drawable->u.invers;
        localize_mask(worker, &invers.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_invers(worker->draw_context.canvas, &drawable->bbox, &clip,
                                         &invers);
        unlocalize_mask(&invers.mask);
        break;
    }
    case QXL_DRAW_ROP3: {
        Rop3 rop3 = drawable->u.rop3;
        localize_brush(worker, &rop3.brush, worker->dev_info.phys_delta);
        localize_bitmap(worker, &rop3.src_bitmap, worker->dev_info.phys_delta);
        localize_mask(worker, &rop3.mask, worker->dev_info.phys_delta);
        worker->draw_context.draw_rop3(worker->draw_context.canvas, &drawable->bbox, &clip, &rop3);
        unlocalize_mask(&rop3.mask);
        unlocalize_bitmap(&rop3.src_bitmap);
        unlocalize_brush(&rop3.brush);
        break;
    }
    case QXL_DRAW_STROKE: {
        Stroke stroke = drawable->u.stroke;
        localize_brush(worker, &stroke.brush, worker->dev_info.phys_delta);
        localize_path(&stroke.path, worker->dev_info.phys_delta);
        localize_attr(&stroke.attr, worker->dev_info.phys_delta);
        worker->draw_context.draw_stroke(worker->draw_context.canvas, &drawable->bbox, &clip,
                                         &stroke);
        unlocalize_attr(&stroke.attr);
        unlocalize_path(&stroke.path);
        unlocalize_brush(&stroke.brush);
        break;
    }
    case QXL_DRAW_TEXT: {
        Text text = drawable->u.text;
        localize_brush(worker, &text.fore_brush, worker->dev_info.phys_delta);
        localize_brush(worker, &text.back_brush, worker->dev_info.phys_delta);
        localize_str(&text.str, worker->dev_info.phys_delta);
        worker->draw_context.draw_text(worker->draw_context.canvas, &drawable->bbox, &clip, &text);
        unlocalize_str(&text.str);
        unlocalize_brush(&text.back_brush);
        unlocalize_brush(&text.fore_brush);
        break;
    }
    default:
        red_printf("invlaid type");
    }
    unlocalize_clip(&clip);
}

#ifndef DRAW_ALL

static void red_draw_drawable(RedWorker *worker, Drawable *drawable)
{
#ifdef UPDATE_AREA_BY_TREE
    //todo: add need top mask flag
    worker->draw_context.set_top_mask(worker->draw_context.canvas,
                                      drawable->tree_item.base.rgn.num_rects,
                                      drawable->tree_item.base.rgn.rects);
#endif
    red_draw_qxl_drawable(worker, drawable->qxl_drawable);
#ifdef UPDATE_AREA_BY_TREE
    worker->draw_context.clear_top_mask(worker->draw_context.canvas);
#endif
}

#ifdef UPDATE_AREA_BY_TREE

static inline void __red_collect_for_update(RedWorker *worker, Ring *ring, RingItem *ring_item,
                                            QRegion *rgn, Ring *items)
{
    Ring *top_ring = ring;

    for (;;) {
        TreeItem *now = CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;
        if (region_intersects(rgn, &now->rgn)) {
            if (IS_DRAW_ITEM(now)) {
                Drawable *drawable = CONTAINEROF(now, Drawable, tree_item);

                ring_add(items, &drawable->collect_link);
                region_or(rgn, &now->rgn);
                if (drawable->tree_item.shadow) {
                    region_or(rgn, &drawable->tree_item.shadow->base.rgn);
                }
            } else if (now->type == TREE_ITEM_TYPE_SHADOW) {
                Drawable *owner = CONTAINEROF(((Shadow *)now)->owner, Drawable, tree_item);
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

static void red_update_area(RedWorker *worker, const Rect *area)
{
    Ring *ring = &worker->current;
    RingItem *ring_item;
    Ring items;
    QRegion rgn;

    if (!(ring_item = ring_get_head(ring))) {
        return;
    }

    region_init(&rgn);
    region_add(&rgn, area);
    ring_init(&items);
    __red_collect_for_update(worker, ring, ring_item, &rgn, &items);
    region_destroy(&rgn);

    while ((ring_item = ring_get_head(&items))) {
        Drawable *drawable = CONTAINEROF(ring_item, Drawable, collect_link);
        Container *container;

        ring_remove(ring_item);
        red_draw_drawable(worker, drawable);
        container = drawable->tree_item.base.container;
        current_remove_drawable(worker, drawable);
        container_cleanup(worker, container);
    }
    worker->draw_context.validate_area(worker->draw_context.canvas, &worker->dev_info.draw_area,
                                       area);
}

#else

static void red_update_area(RedWorker *worker, const Rect *area)
{
    Ring *ring = &worker->current_list;
    RingItem *ring_item = ring;
    QRegion rgn;
    Drawable *last = NULL;
    Drawable *now;

    region_init(&rgn);
    region_add(&rgn, area);
    while ((ring_item = ring_next(ring, ring_item))) {
        now = CONTAINEROF(ring_item, Drawable, list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            last = now;
            break;
        }
    }
    region_destroy(&rgn);

    if (!last) {
        return;
    }

    do {
        Container *container;

        ring_item = ring_get_tail(&worker->current_list);
        now = CONTAINEROF(ring_item, Drawable, list_link);
        red_draw_drawable(worker, now);
        container = now->tree_item.base.container;
        current_remove_drawable(worker, now);
        container_cleanup(worker, container);
    } while (now != last);
    worker->draw_context.validate_area(worker->draw_context.canvas, &worker->dev_info.draw_area,
                                       area);
}

#endif

#endif

static void red_release_cursor(RedWorker *worker, CursorItem *cursor)
{
    if (!--cursor->refs) {
        QXLCursorCmd *cursor_cmd;

        if (cursor->type == CURSOR_TYPE_LOCAL) {
            free(cursor);
            return;
        }
        cursor_cmd = CONTAINEROF(cursor, QXLCursorCmd, device_data);
        worker->qxl->release_resource(worker->qxl, &cursor_cmd->release_info);
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

void qxl_process_cursor(RedWorker *worker, QXLCursorCmd *cursor_cmd)
{
    CursorItem *item = (CursorItem *)cursor_cmd->device_data;
    int cursor_show = FALSE;

    red_pipe_item_init(&item->pipe_data, PIPE_ITEM_TYPE_CURSOR);
    item->refs = 1;
    item->type = CURSOR_TYPE_INVALID;

    switch (cursor_cmd->type) {
    case QXL_CURSOR_SET:
        worker->cursor_visible = cursor_cmd->u.set.visible;
        item->type = CURSOR_TYPE_DEV;
        red_set_cursor(worker, item);
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

    if (worker->cursor_channel && (worker->mouse_mode == RED_MOUSE_MODE_SERVER ||
                                   cursor_cmd->type != QXL_CURSOR_MOVE || cursor_show)) {
        red_pipe_add(&worker->cursor_channel->base, &item->pipe_data);
    } else {
        red_release_cursor(worker, item);
    }
}

static inline uint64_t red_now()
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);

    return time.tv_sec * 1000000000 + time.tv_nsec;
}

static int red_process_cursor(RedWorker *worker, uint32_t max_pipe_size)
{
    QXLCommand cmd;
    int n = 0;

    while (!worker->cursor_channel || worker->cursor_channel->base.pipe_size <= max_pipe_size) {
        if (!worker->qxl->get_cursor_command(worker->qxl, &cmd)) {
            if (worker->repoll_cursor_ring < CMD_RING_POLL_RETRIES) {
                worker->repoll_cursor_ring++;
                worker->epoll_timeout = MIN(worker->epoll_timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->repoll_cursor_ring > CMD_RING_POLL_RETRIES ||
                worker->qxl->req_cursor_notification(worker->qxl)) {
                worker->repoll_cursor_ring++;
                break;
            }
            continue;
        }
        worker->repoll_cursor_ring = 0;
        switch (cmd.type) {
        case QXL_CMD_CURSOR: {
            QXLCursorCmd *cursor_cmd = (QXLCursorCmd *)(cmd.data + worker->dev_info.phys_delta);
            ASSERT((uint64_t)cursor_cmd >= worker->dev_info.phys_start);
            ASSERT((uint64_t)cursor_cmd + sizeof(QXLCursorCmd) <= worker->dev_info.phys_end);
            qxl_process_cursor(worker, cursor_cmd);
            break;
        }
        default:
            red_error("bad command type");
        }
        n++;
    }
    return n;
}

static int red_process_commands(RedWorker *worker, uint32_t max_pipe_size)
{
    QXLCommand cmd;
    int n = 0;
    uint64_t start = red_now();

    while (!worker->display_channel || worker->display_channel->base.pipe_size <= max_pipe_size) {
        if (!worker->qxl->get_command(worker->qxl, &cmd)) {
            if (worker->repoll_cmd_ring < CMD_RING_POLL_RETRIES) {
                worker->repoll_cmd_ring++;
                worker->epoll_timeout = MIN(worker->epoll_timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->repoll_cmd_ring > CMD_RING_POLL_RETRIES ||
                         worker->qxl->req_cmd_notification(worker->qxl)) {
                worker->repoll_cmd_ring++;
                break;
            }
            continue;
        }
        stat_inc_counter(worker->command_counter, 1);
        worker->repoll_cmd_ring = 0;
        switch (cmd.type) {
        case QXL_CMD_DRAW: {
            QXLDrawable *drawable = (QXLDrawable *)(cmd.data + worker->dev_info.phys_delta);
            ASSERT((uint64_t)drawable >= worker->dev_info.phys_start);
            ASSERT((uint64_t)drawable + sizeof(QXLDrawable) <= worker->dev_info.phys_end);
            red_process_drawable(worker, drawable);
            break;
        }
        case QXL_CMD_UPDATE: {
            QXLUpdateCmd *draw_cmd = (QXLUpdateCmd *)(cmd.data + worker->dev_info.phys_delta);
            ASSERT((uint64_t)draw_cmd >= worker->dev_info.phys_start);
            ASSERT((uint64_t)draw_cmd + sizeof(QXLUpdateCmd) <= worker->dev_info.phys_end);
            red_update_area(worker, &draw_cmd->area);
            worker->qxl->notify_update(worker->qxl, draw_cmd->update_id);
            worker->qxl->release_resource(worker->qxl, &draw_cmd->release_info);
            break;
        }
        case QXL_CMD_CURSOR: {
            QXLCursorCmd *cursor_cmd = (QXLCursorCmd *)(cmd.data + worker->dev_info.phys_delta);
            ASSERT((uint64_t)cursor_cmd >= worker->dev_info.phys_start);
            ASSERT((uint64_t)cursor_cmd + sizeof(QXLCursorCmd) <= worker->dev_info.phys_end);
            qxl_process_cursor(worker, cursor_cmd);
            break;
        }
        case QXL_CMD_MESSAGE: {
            QXLMessage *message = (QXLMessage *)(cmd.data + worker->dev_info.phys_delta);
            ASSERT((uint64_t)message >= worker->dev_info.phys_start);
            ASSERT((uint64_t)message + sizeof(QXLDrawable) <= worker->dev_info.phys_end);
            red_printf("MESSAGE: %s", message->data);
            worker->qxl->release_resource(worker->qxl, &message->release_info);
            break;
        }
        default:
            red_error("bad command type");
        }
        n++;
        if ((worker->display_channel && worker->display_channel->base.send_data.blocked) ||
                                                            red_now() - start > 10 * 1000 * 1000) {
            worker->epoll_timeout = 0;
            return n;
        }
    }
    return n;
}

#define RED_RELEASE_BUNCH_SIZE 5

static void red_free_some(RedWorker *worker)
{
    int n = 0;

    if (worker->display_channel && worker->display_channel->glz_dict) {
        // encoding using the dictionary is prevented since the following operations might
        // change the dictionary
        pthread_rwlock_wrlock(&worker->display_channel->glz_dict->encode_lock);
        n = red_display_free_some_independent_glz_drawables(worker->display_channel);
    }

    while (!ring_is_empty(&worker->current_list) && n++ < RED_RELEASE_BUNCH_SIZE) {
        free_one_drawable(worker, TRUE);
    }

    if (worker->display_channel && worker->display_channel->glz_dict) {
        pthread_rwlock_unlock(&worker->display_channel->glz_dict->encode_lock);
    }
}

static void red_current_flush(RedWorker *worker)
{
    while (!ring_is_empty(&worker->current_list)) {
        free_one_drawable(worker, FALSE);
    }
    red_current_clear(worker);
}

static void red_add_screen_image(RedWorker *worker)
{
    ImageItem *item;
    int stride;
    Rect area;

    if (!worker->display_channel) {
        return;
    }
    stride = worker->dev_info.x_res << 2;
    if (!(item = (ImageItem *)malloc(sizeof(ImageItem) + worker->dev_info.y_res * stride))) {
        //warn
        return;
    }

    red_pipe_item_init(&item->link, PIPE_ITEM_TYPE_IMAGE);

    item->refs = 1;
    item->pos.x = item->pos.y = 0;
    item->width = worker->dev_info.x_res;
    item->height = worker->dev_info.y_res;
    item->stride = stride;
    item->top_down = worker->draw_context.top_down;

    area.top = area.left = 0;
    area.right = worker->dev_info.x_res;
    area.bottom = worker->dev_info.y_res;
    worker->draw_context.read_pixels(worker->draw_context.canvas, item->data, stride, &area);
    red_pipe_add_image_item(worker, item);
    release_image_item(item);
    display_channel_push(worker);
}

static void inline __add_buf(RedChannel *channel, uint32_t type, void *data, uint32_t size)
{
    int pos = channel->send_data.n_bufs++;
    ASSERT(pos < MAX_SEND_BUFS);
    channel->send_data.bufs[pos].type = type;
    channel->send_data.bufs[pos].size = size;
    channel->send_data.bufs[pos].data = data;
}

static void add_buf(RedChannel *channel, uint32_t type, void *data, uint32_t size)
{
    __add_buf(channel, type, data, size);
    channel->send_data.header.size += size;
}

static void fill_path(DisplayChannel *display_channel, PHYSICAL *in_path)
{
    RedChannel *channel = &display_channel->base;
    ASSERT(in_path && *in_path);
    QXLPath *path = (QXLPath *)(*in_path + channel->worker->dev_info.phys_delta);
    *in_path = channel->send_data.header.size;
    add_buf(channel, BUF_TYPE_RAW, &path->data_size, sizeof(UINT32));
    add_buf(channel, BUF_TYPE_CHUNK, &path->chunk, path->data_size);
}

static void fill_str(DisplayChannel *display_channel, PHYSICAL *in_str)
{
    RedChannel *channel = &display_channel->base;
    ASSERT(in_str && *in_str);
    QXLString *str = (QXLString *)(*in_str + channel->worker->dev_info.phys_delta);
    *in_str = channel->send_data.header.size;
    add_buf(channel, BUF_TYPE_RAW, &str->length, sizeof(UINT32));
    add_buf(channel, BUF_TYPE_CHUNK, &str->chunk, str->data_size);
}

static inline void fill_rects_clip(RedChannel *channel, PHYSICAL *in_clip)
{
    QXLClipRects *clip;

    ASSERT(in_clip && *in_clip);
    clip = (QXLClipRects *)(*in_clip + channel->worker->dev_info.phys_delta);
    *in_clip = channel->send_data.header.size;
    add_buf(channel, BUF_TYPE_RAW, &clip->num_rects, sizeof(UINT32));
    add_buf(channel, BUF_TYPE_CHUNK, &clip->chunk, clip->num_rects * sizeof(Rect));
}

static void fill_base(DisplayChannel *display_channel, RedDrawBase *base, QXLDrawable *drawable,
                      uint32_t size)
{
    RedChannel *channel = &display_channel->base;
    add_buf(channel, BUF_TYPE_RAW, base, size);
    base->box = drawable->bbox;
    base->clip = drawable->clip;

    if (base->clip.type == CLIP_TYPE_RECTS) {
        fill_rects_clip(channel, &base->clip.data);
    } else if (base->clip.type == CLIP_TYPE_PATH) {
        fill_path(display_channel, &base->clip.data);
    }
}

static inline RedImage *alloc_image(DisplayChannel *display_channel)
{
    ASSERT(display_channel->send_data.bitmap_pos < MAX_BITMAPS);
    return &display_channel->send_data.images[display_channel->send_data.bitmap_pos++];
}

/* io_palette is relative address of the palette*/
static inline void fill_palette(DisplayChannel *display_channel, ADDRESS *io_palette, UINT8 *flags)
{
    RedChannel *channel = &display_channel->base;
    Palette *palette;

    if (!(*io_palette)) {
        return;
    }
    palette = (Palette *)(*io_palette + channel->worker->dev_info.phys_delta);
    if (palette->unique) {
        if (red_palette_cache_find(display_channel, palette->unique)) {
            *flags |= BITMAP_PAL_FROM_CACHE;
            *io_palette = palette->unique;
            return;
        }
        if (red_palette_cache_add(display_channel, palette->unique, 1)) {
            *flags |= BITMAP_PAL_CACHE_ME;
        }
    }
    *io_palette = channel->send_data.header.size;
    add_buf(channel, BUF_TYPE_RAW, palette,
            sizeof(Palette) + palette->num_ents * sizeof(UINT32));
}

static inline RedCompressBuf *red_display_alloc_compress_buf(DisplayChannel *display_channel)
{
    RedCompressBuf *ret;

    if (display_channel->send_data.free_compress_bufs) {
        ret = display_channel->send_data.free_compress_bufs;
        display_channel->send_data.free_compress_bufs = ret->next;
    } else {
        if (!(ret = malloc(sizeof(*ret)))) {
            return NULL;
        }
    }

    ret->next = display_channel->send_data.used_compress_bufs;
    display_channel->send_data.used_compress_bufs = ret;
    return ret;
}

static inline void __red_display_free_compress_buf(DisplayChannel *display_channel,
                                                   RedCompressBuf *buf)
{
    buf->next = display_channel->send_data.free_compress_bufs;
    display_channel->send_data.free_compress_bufs = buf;
}

static void red_display_free_compress_buf(DisplayChannel *display_channel,
                                          RedCompressBuf *buf)
{
    RedCompressBuf **curr_used = &display_channel->send_data.used_compress_bufs;

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

static void red_display_reset_compress_buf(DisplayChannel *display_channel)
{
    while (display_channel->send_data.used_compress_bufs) {
        RedCompressBuf *buf = display_channel->send_data.used_compress_bufs;
        display_channel->send_data.used_compress_bufs = buf->next;
        __red_display_free_compress_buf(display_channel, buf);
    }
}

/******************************************************
 *      Global lz red drawables routines
*******************************************************/

/* if already exists, returns it. Otherwise allocates and adds it (1) to the ring tail
   in the channel (2) to the Drawable*/
static RedGlzDrawable *red_display_get_glz_drawable(DisplayChannel *channel, Drawable *drawable)
{
    RedGlzDrawable *ret;

    if (drawable->red_glz_drawable) {
        return drawable->red_glz_drawable;
    }

    if (!(ret = malloc(sizeof(*ret)))) {
        PANIC("malloc failed");
    }

    ret->display_channel = channel;
    ret->qxl_drawable = drawable->qxl_drawable;
    ret->drawable = drawable;
    ret->instances_count = 0;
    ring_init(&ret->instances);

    ring_item_init(&ret->link);

    ring_add_before(&ret->link, &channel->glz_drawables);
    drawable->red_glz_drawable = ret;

    return ret;
}

/* allocates new instance and adds it to instances in the given drawable.
   NOTE - the caller should set the glz_instance returned by the encoder by itself.*/
static GlzDrawableInstanceItem *red_display_add_glz_drawable_instance(RedGlzDrawable *glz_drawable)
{
    ASSERT(glz_drawable->instances_count < MAX_GLZ_DRAWABLE_INSTANCES);
    // NOTE: We assume the addtions are performed consecutively, without removals in the middle
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
   When no instance is left - the RedGlzDrawable is released too. (and the qxl drawblae too, if
   it is not used by Drawable).
   NOTE - 1) can be called only by the display channel that created the drawable
          2) it is assumed that the instance was already removed from the dicitonary*/
static void red_display_free_glz_drawable_instance(DisplayChannel *channel,
                                                   GlzDrawableInstanceItem *glz_drawable_instance)
{
    RedGlzDrawable *glz_drawable;

    ASSERT(glz_drawable_instance);
    ASSERT(glz_drawable_instance->red_glz_drawable);

    glz_drawable = glz_drawable_instance->red_glz_drawable;

    ASSERT(glz_drawable->display_channel == channel);
    ASSERT(glz_drawable->instances_count);

    ring_remove(&glz_drawable_instance->glz_link);
    glz_drawable->instances_count--;
    // whan the remove callback is performed from the channel that the
    // drawable belongs to, the instance is not added to the 'to_free' list
    if (ring_item_is_linked(&glz_drawable_instance->free_link)) {
        ring_remove(&glz_drawable_instance->free_link);
    }

    if (ring_is_empty(&glz_drawable->instances)) {
        ASSERT(!glz_drawable->instances_count);

        Drawable *drawable = glz_drawable->drawable;

        if (drawable) {
            drawable->red_glz_drawable = NULL;
        } else { // no reference to the qxl drawable left
            free_qxl_drawable(channel->base.worker, glz_drawable->qxl_drawable);
        }

        if (ring_item_is_linked(&glz_drawable->link)) {
            ring_remove(&glz_drawable->link);
        }
        free(glz_drawable);
    }
}

static void red_display_handle_glz_drawables_to_free(DisplayChannel* channel)
{
    RingItem *ring_link;
    pthread_mutex_lock(&channel->glz_drawables_inst_to_free_lock);

    while ((ring_link = ring_get_head(&channel->glz_drawables_inst_to_free))) {
        GlzDrawableInstanceItem *drawable_instance = CONTAINEROF(ring_link,
                                                                 GlzDrawableInstanceItem,
                                                                 free_link);
        red_display_free_glz_drawable_instance(channel, drawable_instance);
    }

    pthread_mutex_unlock(&channel->glz_drawables_inst_to_free_lock);
}

/* releases all the instances of the drawable from the dictionary and the display channel.
   The release of the last instance will also release the drawable itself and the qxl drawable
   if possible.
   NOTE - the caller should prevent encoding using the dicitonary during this operation*/
static void red_display_free_glz_drawable(DisplayChannel *channel, RedGlzDrawable *drawable)
{
    RingItem *head_instance = ring_get_head(&drawable->instances);
    int cont = (head_instance != NULL);

    while (cont) {
        if (drawable->instances_count == 1) {
            /* Last instance: red_display_free_glz_drawable_instance will free the drawable */
            cont = FALSE;
        }
        GlzDrawableInstanceItem *instance = CONTAINEROF(head_instance,
                                                        GlzDrawableInstanceItem,
                                                        glz_link);
        if (!ring_item_is_linked(&instance->free_link)) {
            // the instance didn't get out from window yet
            glz_enc_dictionary_remove_image(channel->glz_dict->dict,
                                            instance->glz_instance,
                                            &channel->glz_data.usr);
        }
        red_display_free_glz_drawable_instance(channel, instance);

        if (cont) {
            head_instance = ring_get_head(&drawable->instances);
        }
    }
}

/* Clear all lz drawables - enforce their removal from the global dictionary.
   NOTE - prevents encoding using the dicitonary during the operation*/
static void red_display_clear_glz_drawables(DisplayChannel *channel)
{
    RingItem *ring_link;

    if (!channel || !channel->glz_dict) {
        return;
    }

    // assure no display channel is during global lz encoding
    pthread_rwlock_wrlock(&channel->glz_dict->encode_lock);

    while ((ring_link = ring_get_head(&channel->glz_drawables))) {
        RedGlzDrawable *drawable = CONTAINEROF(ring_link, RedGlzDrawable, link);
        // no need to lock the to_free list, since we assured no other thread is encoding and
        // thus not other thread access the to_free list of the channel
        red_display_free_glz_drawable(channel, drawable);
    }

    pthread_rwlock_unlock(&channel->glz_dict->encode_lock);
}

/* Remove from the global lz dictionary some glz_drawables that have no reference to
   Drawable (their qxl drawables are released too).
   NOTE - the caller should prevent encoding using the dicitonary during the operation*/
static int red_display_free_some_independent_glz_drawables(DisplayChannel *channel)
{
    int n = 0;

    if (!channel) {
        return 0;
    }

    RingItem *ring_link = ring_get_head(&channel->glz_drawables);

    while ((n < RED_RELEASE_BUNCH_SIZE) && (ring_link != NULL)) {
        RedGlzDrawable *glz_drawable = CONTAINEROF(ring_link, RedGlzDrawable, link);
        ring_link = ring_next(&channel->glz_drawables, ring_link);
        if (!glz_drawable->drawable) {
            red_display_free_glz_drawable(channel, glz_drawable);
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

    PANIC(usr_data->message_buf); // if global lz fails in the middle
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
    return malloc(size);
}

static void *lz_usr_malloc(LzUsrContext *usr, int size)
{
    return malloc(size);
}

static void *glz_usr_malloc(GlzEncoderUsrContext *usr, int size)
{
    return malloc(size);
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

    if (!(buf = red_display_alloc_compress_buf(enc_data->display_channel))) {
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

static inline int encoder_usr_more_lines(EncoderData *enc_data, uint8_t **lines)
{
    if (!enc_data->u.lines_data.next) {
        return 0;
    }

    QXLDataChunk *chunk = (QXLDataChunk *)(enc_data->u.lines_data.next +
                                           enc_data->u.lines_data.address_delta);
    if (chunk->data_size % enc_data->u.lines_data.stride) {
        return 0;
    }
    enc_data->u.lines_data.next = chunk->next_chunk;
    *lines = chunk->data;
    return chunk->data_size / enc_data->u.lines_data.stride;
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

static int quic_usr_more_lines_revers(QuicUsrContext *usr, uint8_t **lines)
{
    EncoderData *quic_data = &(((QuicData *)usr)->data);

    if (!quic_data->u.lines_data.next) {
        return 0;
    }

    QXLDataChunk *chunk = (QXLDataChunk *)(quic_data->u.lines_data.next +
                                           quic_data->u.lines_data.address_delta);
    if (chunk->data_size % quic_data->u.lines_data.stride) {
        return 0;
    }
    quic_data->u.lines_data.next = chunk->prev_chunk;
    *lines = chunk->data + chunk->data_size - quic_data->u.lines_data.stride;
    return chunk->data_size / quic_data->u.lines_data.stride;
}

static int quic_usr_more_lines_unstable(QuicUsrContext *usr, uint8_t **out_lines)
{
    EncoderData *quic_data = &(((QuicData *)usr)->data);

    if (!quic_data->u.unstable_lines_data.lines) {
        return 0;
    }
    uint8_t *src = quic_data->u.unstable_lines_data.next;
    int lines = MIN(quic_data->u.unstable_lines_data.lines,
                    quic_data->u.unstable_lines_data.max_lines_bunch);
    quic_data->u.unstable_lines_data.lines -= lines;
    uint8_t *end = src + lines * quic_data->u.unstable_lines_data.src_stride;
    quic_data->u.unstable_lines_data.next = end;

    uint8_t *out = (uint8_t *)quic_data->u.unstable_lines_data.input_bufs[
            quic_data->u.unstable_lines_data.input_bufs_pos++ & 1]->buf;
    uint8_t *dest = out;
    for (; src != end; src += quic_data->u.unstable_lines_data.src_stride,
                       dest += quic_data->u.unstable_lines_data.dest_stride) {
        memcpy(dest, src, quic_data->u.unstable_lines_data.dest_stride);
    }
    *out_lines = out;
    return lines;
}

static int quic_usr_no_more_lines(QuicUsrContext *usr, uint8_t **lines)
{
    return 0;
}

static int lz_usr_no_more_lines(LzUsrContext *usr, uint8_t **lines)
{
    return 0;
}

static int glz_usr_no_more_lines(GlzEncoderUsrContext *usr, uint8_t **lines)
{
    return 0;
}

static void glz_usr_free_image(GlzEncoderUsrContext *usr, GlzUsrImageContext *image)
{
    GlzData *lz_data = (GlzData *)usr;
    GlzDrawableInstanceItem *glz_drawable_instance = (GlzDrawableInstanceItem *)image;
    DisplayChannel *drawable_channel = glz_drawable_instance->red_glz_drawable->display_channel;
    DisplayChannel *this_channel = CONTAINEROF(lz_data, DisplayChannel, glz_data);
    if (this_channel == drawable_channel) {
        red_display_free_glz_drawable_instance(drawable_channel, glz_drawable_instance);
    } else {
        pthread_mutex_lock(&drawable_channel->glz_drawables_inst_to_free_lock);
        ring_add_before(&glz_drawable_instance->free_link,
                        &drawable_channel->glz_drawables_inst_to_free);
        pthread_mutex_unlock(&drawable_channel->glz_drawables_inst_to_free_lock);
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

static inline void red_display_init_glz_data(DisplayChannel *display)
{
    display->glz_data.usr.error = glz_usr_error;
    display->glz_data.usr.warn = glz_usr_warn;
    display->glz_data.usr.info = glz_usr_warn;
    display->glz_data.usr.malloc = glz_usr_malloc;
    display->glz_data.usr.free = glz_usr_free;
    display->glz_data.usr.more_space = glz_usr_more_space;
    display->glz_data.usr.more_lines = glz_usr_more_lines;
    display->glz_data.usr.free_image = glz_usr_free_image;
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

#define GRADUAL_SCORE_RGB24_TH -0.03
#define GRADUAL_SCORE_RGB16_TH 0

// assumes that stride doesn't overflow
static int _bitmap_is_gradual(RedWorker *worker, Bitmap *bitmap)
{
    long address_delta = worker->dev_info.phys_delta;
    double score = 0.0;
    int num_samples = 0;

    if ((bitmap->flags & QXL_BITMAP_DIRECT)) {
        uint8_t *lines = (uint8_t*)(bitmap->data + address_delta);
        switch (bitmap->format) {
        case BITMAP_FMT_16BIT:
            compute_lines_gradual_score_rgb16((rgb16_pixel_t*)lines, bitmap->x, bitmap->y,
                                              &score, &num_samples);
            break;
        case BITMAP_FMT_24BIT:
            compute_lines_gradual_score_rgb24((rgb24_pixel_t*)lines, bitmap->x, bitmap->y,
                                              &score, &num_samples);
            break;
        case BITMAP_FMT_32BIT:
        case BITMAP_FMT_RGBA:
            compute_lines_gradual_score_rgb32((rgb32_pixel_t*)lines, bitmap->x, bitmap->y,
                                              &score, &num_samples);
            break;
        default:
            red_error("invalid bitmap format (not RGB) %u", bitmap->format);
        }
    } else {
        QXLDataChunk *chunk = NULL;
        int num_lines;
        double chunk_score = 0.0;
        int chunk_num_samples = 0;
        ADDRESS relative_address = bitmap->data;

        while (relative_address) {
            chunk = (QXLDataChunk *)(relative_address + address_delta);
            num_lines = chunk->data_size / bitmap->stride;
            switch (bitmap->format) {
            case BITMAP_FMT_16BIT:
                compute_lines_gradual_score_rgb16((rgb16_pixel_t*)chunk->data, bitmap->x, num_lines,
                                                  &chunk_score, &chunk_num_samples);
                break;
            case BITMAP_FMT_24BIT:
                compute_lines_gradual_score_rgb24((rgb24_pixel_t*)chunk->data, bitmap->x, num_lines,
                                                  &chunk_score, &chunk_num_samples);
                break;
            case BITMAP_FMT_32BIT:
            case BITMAP_FMT_RGBA:
                compute_lines_gradual_score_rgb32((rgb32_pixel_t*)chunk->data, bitmap->x, num_lines,
                                                  &chunk_score, &chunk_num_samples);
                break;
            default:
                red_error("invalid bitmap format (not RGB) %u", bitmap->format);
            }

            score += chunk_score;
            num_samples += chunk_num_samples;

            relative_address = chunk->next_chunk;
        }
    }

    ASSERT(num_samples);
    score /= num_samples;

    if (bitmap->format == BITMAP_FMT_16BIT) {
        return (score < GRADUAL_SCORE_RGB16_TH);
    } else {
        return (score < GRADUAL_SCORE_RGB24_TH);
    }
}

static inline int _stride_is_extra(Bitmap *bitmap)
{
    ASSERT(bitmap);
    if (BITMAP_FMT_IS_RGB[bitmap->format]) {
        return ((bitmap->x * BITMAP_FMP_BYTES_PER_PIXEL[bitmap->format]) < bitmap->stride);
    } else {
        switch (bitmap->format) {
        case BITMAP_FMT_8BIT:
            return (bitmap->x < bitmap->stride);
        case BITMAP_FMT_4BIT_BE:
        case BITMAP_FMT_4BIT_LE: {
            int bytes_width = ALIGN(bitmap->x, 2) >> 1;
            return bytes_width < bitmap->stride;
        }
        case BITMAP_FMT_1BIT_BE:
        case BITMAP_FMT_1BIT_LE: {
            int bytes_width = ALIGN(bitmap->x, 8) >> 3;
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
    uint32_t raw_size;
    void*    comp_buf;
    uint32_t comp_buf_size;
    ADDRESS  *plt_ptr;
    UINT8    *flags_ptr;
} compress_send_data_t;


static inline int red_glz_compress_image(DisplayChannel *display_channel,
                                         RedImage *dest, Bitmap *src, Drawable *drawable,
                                         compress_send_data_t* o_comp_data)
{
#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif
    ASSERT(BITMAP_FMT_IS_RGB[src->format]);
    GlzData *glz_data = &display_channel->glz_data;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    RedGlzDrawable *glz_drawable;
    GlzDrawableInstanceItem *glz_drawable_instance;
    uint8_t *lines;
    unsigned int num_lines;
    int size;
    long address_delta;

    glz_data->data.bufs_tail = red_display_alloc_compress_buf(display_channel);
    glz_data->data.bufs_head = glz_data->data.bufs_tail;

    if (!glz_data->data.bufs_head) {
        return FALSE;
    }

    glz_data->data.bufs_head->send_next = NULL;
    glz_data->data.display_channel = display_channel;
    address_delta = display_channel->base.worker->dev_info.phys_delta;

    glz_drawable = red_display_get_glz_drawable(display_channel, drawable);
    glz_drawable_instance = red_display_add_glz_drawable_instance(glz_drawable);

    if ((src->flags & QXL_BITMAP_DIRECT)) {
        glz_data->usr.more_lines = glz_usr_no_more_lines;
        lines = (uint8_t*)(src->data + address_delta);
        num_lines = src->y;
    } else {
        glz_data->data.u.lines_data.address_delta = address_delta;
        glz_data->data.u.lines_data.stride = src->stride;
        glz_data->data.u.lines_data.next = src->data;
        glz_data->usr.more_lines = glz_usr_more_lines;
        lines = NULL;
        num_lines = 0;
    }

    size = glz_encode(display_channel->glz, type, src->x, src->y,
                      (src->flags & QXL_BITMAP_TOP_DOWN), lines, num_lines,
                      src->stride, (uint8_t*)glz_data->data.bufs_head->buf,
                      sizeof(glz_data->data.bufs_head->buf),
                      glz_drawable_instance,
                      &glz_drawable_instance->glz_instance);

    dest->descriptor.type = IMAGE_TYPE_GLZ_RGB;
    dest->lz_rgb.data_size = size;

    o_comp_data->raw_size = sizeof(LZ_RGBImage);
    o_comp_data->comp_buf = glz_data->data.bufs_head;
    o_comp_data->comp_buf_size = size;
    o_comp_data->plt_ptr = NULL;
    o_comp_data->flags_ptr = NULL;

    stat_compress_add(&display_channel->glz_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

static inline int red_lz_compress_image(DisplayChannel *display_channel,
                                        RedImage *dest, Bitmap *src,
                                        compress_send_data_t* o_comp_data)
{
    RedWorker *worker = display_channel->base.worker;
    LzData *lz_data = &worker->lz_data;
    LzContext *lz = worker->lz;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    long address_delta;
    int size;            // size of the compressed data

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif

    lz_data->data.bufs_tail = red_display_alloc_compress_buf(display_channel);
    lz_data->data.bufs_head = lz_data->data.bufs_tail;

    if (!lz_data->data.bufs_head) {
        return FALSE;
    }

    lz_data->data.bufs_head->send_next = NULL;
    lz_data->data.display_channel = display_channel;
    address_delta = worker->dev_info.phys_delta;

    if (setjmp(lz_data->data.jmp_env)) {
        while (lz_data->data.bufs_head) {
            RedCompressBuf *buf = lz_data->data.bufs_head;
            lz_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(display_channel, buf);
        }
        return FALSE;
    }

    if ((src->flags & QXL_BITMAP_DIRECT)) {
        lz_data->usr.more_lines = lz_usr_no_more_lines;
        size = lz_encode(lz, type, src->x, src->y, (src->flags & QXL_BITMAP_TOP_DOWN),
                         (uint8_t*)(src->data + address_delta), src->y, src->stride,
                         (uint8_t*)lz_data->data.bufs_head->buf,
                         sizeof(lz_data->data.bufs_head->buf));
    } else {
        lz_data->data.u.lines_data.address_delta = address_delta;
        lz_data->data.u.lines_data.stride = src->stride;
        lz_data->data.u.lines_data.next = src->data;
        lz_data->usr.more_lines = lz_usr_more_lines;

        size = lz_encode(lz, type, src->x, src->y, (src->flags & QXL_BITMAP_TOP_DOWN),
                         NULL, 0, src->stride,
                         (uint8_t*)lz_data->data.bufs_head->buf,
                         sizeof(lz_data->data.bufs_head->buf));
    }

    // the compressed buffer is bigger than the original data
    if (size > (src->y * src->stride)) {
        longjmp(lz_data->data.jmp_env, 1);
    }

    if (BITMAP_FMT_IS_RGB[src->format]) {
        dest->descriptor.type = IMAGE_TYPE_LZ_RGB;
        dest->lz_rgb.data_size = size;

        o_comp_data->raw_size = sizeof(LZ_RGBImage);
        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;
        o_comp_data->plt_ptr = NULL;
        o_comp_data->flags_ptr = NULL;
    } else {
        dest->descriptor.type = IMAGE_TYPE_LZ_PLT;
        dest->lz_plt.data_size = size;
        dest->lz_plt.flags = src->flags & BITMAP_TOP_DOWN;
        dest->lz_plt.palette = src->palette;

        o_comp_data->raw_size = sizeof(LZ_PLTImage);
        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;
        o_comp_data->plt_ptr = &(dest->lz_plt.palette);
        o_comp_data->flags_ptr = &(dest->lz_plt.flags);
    }
    stat_compress_add(&display_channel->lz_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

static inline int red_quic_compress_image(DisplayChannel *display_channel, RedImage *dest,
                                          Bitmap *src, compress_send_data_t* o_comp_data)
{
    RedWorker *worker = display_channel->base.worker;
    QuicData *quic_data = &worker->quic_data;
    QuicContext *quic = worker->quic;
    QuicImageType type;
    long address_delta;
    int size;

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif

    switch (src->format) {
    case BITMAP_FMT_32BIT:
        type = QUIC_IMAGE_TYPE_RGB32;
        break;
    case BITMAP_FMT_RGBA:
        type = QUIC_IMAGE_TYPE_RGBA;
        break;
    case BITMAP_FMT_16BIT:
        type = QUIC_IMAGE_TYPE_RGB16;
        break;
    case BITMAP_FMT_24BIT:
        type = QUIC_IMAGE_TYPE_RGB24;
        break;
    default:
        return FALSE;
    }

    quic_data->data.bufs_tail = red_display_alloc_compress_buf(display_channel);
    quic_data->data.bufs_head = quic_data->data.bufs_tail;

    if (!quic_data->data.bufs_head) {
        return FALSE;
    }

    quic_data->data.bufs_head->send_next = NULL;
    quic_data->data.display_channel = display_channel;
    address_delta = worker->dev_info.phys_delta;

    if (setjmp(quic_data->data.jmp_env)) {
        while (quic_data->data.bufs_head) {
            RedCompressBuf *buf = quic_data->data.bufs_head;
            quic_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(display_channel, buf);
        }
        return FALSE;
    }

    if ((src->flags & QXL_BITMAP_DIRECT)) {
        int stride;
        uint8_t *data;

        if (!(src->flags & QXL_BITMAP_TOP_DOWN)) {
            data = (uint8_t*)(src->data + address_delta) + src->stride * (src->y - 1);
            stride = -src->stride;
        } else {
            data = (uint8_t*)(src->data + address_delta);
            stride = src->stride;
        }

        if ((src->flags & QXL_BITMAP_UNSTABLE)) {
            quic_data->data.u.unstable_lines_data.next = data;
            quic_data->data.u.unstable_lines_data.src_stride = stride;
            quic_data->data.u.unstable_lines_data.dest_stride = src->stride;
            quic_data->data.u.unstable_lines_data.lines = src->y;
            quic_data->data.u.unstable_lines_data.input_bufs_pos = 0;
            if (!(quic_data->data.u.unstable_lines_data.input_bufs[0] =
                                            red_display_alloc_compress_buf(display_channel)) ||
                !(quic_data->data.u.unstable_lines_data.input_bufs[1] =
                                            red_display_alloc_compress_buf(display_channel))) {
                return FALSE;
            }
            quic_data->data.u.unstable_lines_data.max_lines_bunch =
                                 sizeof(quic_data->data.u.unstable_lines_data.input_bufs[0]->buf) /
                                 quic_data->data.u.unstable_lines_data.dest_stride;
            quic_data->usr.more_lines = quic_usr_more_lines_unstable;
            size = quic_encode(quic, type, src->x, src->y, NULL, 0, src->stride,
                               quic_data->data.bufs_head->buf,
                               sizeof(quic_data->data.bufs_head->buf) >> 2);
        } else {
            quic_data->usr.more_lines = quic_usr_no_more_lines;
            size = quic_encode(quic, type, src->x, src->y, data, src->y, stride,
                               quic_data->data.bufs_head->buf,
                               sizeof(quic_data->data.bufs_head->buf) >> 2);
        }
    } else {
        int stride;

        if ((src->flags & QXL_BITMAP_UNSTABLE)) {
            red_printf_once("unexpected unstable bitmap");
            return FALSE;
        }
        quic_data->data.u.lines_data.address_delta = address_delta;
        quic_data->data.u.lines_data.stride = src->stride;

        if ((src->flags & QXL_BITMAP_TOP_DOWN)) {
            quic_data->data.u.lines_data.next = src->data;
            quic_data->usr.more_lines = quic_usr_more_lines;
            stride = src->stride;
        } else {
            QXLDataChunk *chunk = (QXLDataChunk *)(src->data + address_delta);
            while (chunk->next_chunk) {
                chunk = (QXLDataChunk *)(chunk->next_chunk + address_delta);
                ASSERT(chunk->prev_chunk);
            }
            quic_data->data.u.lines_data.next = (ADDRESS)chunk - address_delta;
            quic_data->usr.more_lines = quic_usr_more_lines_revers;
            stride = -src->stride;
        }
        size = quic_encode(quic, type, src->x, src->y, NULL, 0, stride,
                           quic_data->data.bufs_head->buf,
                           sizeof(quic_data->data.bufs_head->buf) >> 2);
    }

    // the compressed buffer is bigger than the original data
    if ((size << 2) > (src->y * src->stride)) {
        longjmp(quic_data->data.jmp_env, 1);
    }

    dest->descriptor.type = IMAGE_TYPE_QUIC;
    dest->quic.data_size = size << 2;

    o_comp_data->raw_size = sizeof(QUICImage);
    o_comp_data->comp_buf = quic_data->data.bufs_head;
    o_comp_data->comp_buf_size = size << 2;
    o_comp_data->plt_ptr = NULL;
    o_comp_data->flags_ptr = NULL;

    stat_compress_add(&display_channel->quic_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

#define MIN_SIZE_TO_COMPRESS 54
#define MIN_DIMENSION_TO_QUIC 3
static inline int red_compress_image(DisplayChannel *display_channel,
                                     RedImage *dest, Bitmap *src, Drawable *drawable,
                                     compress_send_data_t* o_comp_data)
{
    image_compression_t image_compression = display_channel->base.worker->image_compression;
    int quic_compress = FALSE;

    if ((image_compression == IMAGE_COMPRESS_OFF) ||
        ((src->y * src->stride) < MIN_SIZE_TO_COMPRESS)) { // TODO: change the size cond
        return FALSE;
    } else if (image_compression == IMAGE_COMPRESS_QUIC) {
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
        if (_stride_is_extra(src) || (src->flags & QXL_BITMAP_UNSTABLE)) {
            if ((image_compression == IMAGE_COMPRESS_LZ) ||
                (image_compression == IMAGE_COMPRESS_GLZ) ||
                BITMAP_FMT_IS_PLT[src->format]) {
                return FALSE;
            } else {
                quic_compress = TRUE;
            }
        } else {
            if ((image_compression == IMAGE_COMPRESS_AUTO_LZ) ||
                (image_compression == IMAGE_COMPRESS_AUTO_GLZ)) {
                if ((src->x < MIN_DIMENSION_TO_QUIC) || (src->y < MIN_DIMENSION_TO_QUIC)) {
                    quic_compress = FALSE;
                } else {
                    quic_compress = BITMAP_FMT_IS_RGB[src->format] &&
                        _bitmap_is_gradual(display_channel->base.worker, src);
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
        return red_quic_compress_image(display_channel, dest, src, o_comp_data);
    } else {
        int glz;
        int ret;
        if ((image_compression == IMAGE_COMPRESS_AUTO_GLZ) ||
            (image_compression == IMAGE_COMPRESS_GLZ)) {
            glz = BITMAP_FMT_IS_RGB[src->format] && (
                    (src->x * src->y) < glz_enc_dictionary_get_size(
                        display_channel->glz_dict->dict));
        } else if ((image_compression == IMAGE_COMPRESS_AUTO_LZ) ||
                   (image_compression == IMAGE_COMPRESS_LZ)) {
            glz = FALSE;
        } else {
            red_error("invalid image compression type %u", image_compression);
        }

        if (glz) {
            /* using the global dictionary only if it is not freezed */
            pthread_rwlock_rdlock(&display_channel->glz_dict->encode_lock);
            if (!display_channel->glz_dict->migrate_freeze) {
                ret = red_glz_compress_image(
                        display_channel, dest, src, drawable, o_comp_data);
            } else {
                glz = FALSE;
            }
            pthread_rwlock_unlock(&display_channel->glz_dict->encode_lock);
        }

        if (!glz) {
            ret = red_lz_compress_image(display_channel, dest, src, o_comp_data);
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

static inline void red_display_add_image_to_pixmap_cache(DisplayChannel *display_channel,
                                                         QXLImage *qxl_image, RedImage *io_image)
{
    if ((qxl_image->descriptor.flags & QXL_IMAGE_CACHE)) {
        ASSERT(qxl_image->descriptor.width * qxl_image->descriptor.height > 0);
        if (pixmap_cache_add(display_channel->pixmap_cache, qxl_image->descriptor.id,
                             qxl_image->descriptor.width * qxl_image->descriptor.height,
                             display_channel)) {
            io_image->descriptor.flags |= IMAGE_CACHE_ME;
            stat_inc_counter(display_channel->add_to_cache_counter, 1);
        }
    }

    if (!(io_image->descriptor.flags & IMAGE_CACHE_ME)) {
        stat_inc_counter(display_channel->non_cache_counter, 1);
    }
}

/* if the number of times fill_bits can be called per one qxl_drawable increases -
   MAX_LZ_DRAWABLE_INSTANCES must be increased as well */
static void fill_bits(DisplayChannel *display_channel, PHYSICAL *in_bitmap, Drawable *drawable)
{
    RedChannel *channel = &display_channel->base;
    RedImage *image;
    QXLImage *qxl_image;
    uint8_t *data;
    compress_send_data_t comp_send_data;

    ASSERT(*in_bitmap);

    image = alloc_image(display_channel);
    qxl_image = (QXLImage *)(*in_bitmap + channel->worker->dev_info.phys_delta);

    image->descriptor.id = qxl_image->descriptor.id;
    image->descriptor.type = qxl_image->descriptor.type;
    image->descriptor.flags = 0;
    image->descriptor.width = qxl_image->descriptor.width;
    image->descriptor.height = qxl_image->descriptor.height;

    *in_bitmap = channel->send_data.header.size;
    if ((qxl_image->descriptor.flags & QXL_IMAGE_CACHE)) {
        if (pixmap_cache_hit(display_channel->pixmap_cache, image->descriptor.id,
                             display_channel)) {
            image->descriptor.type = IMAGE_TYPE_FROM_CACHE;
            add_buf(channel, BUF_TYPE_RAW, image, sizeof(ImageDescriptor));
            stat_inc_counter(display_channel->cache_hits_counter, 1);
            return;
        }
    }

    switch (qxl_image->descriptor.type) {
    case IMAGE_TYPE_BITMAP:
#ifdef DUMP_BITMAP
        dump_bitmap(display_channel->base.worker, &qxl_image->bitmap);
#endif
        /* Images must be added to the cache only after they are compressed
           in order to prevent starvation in the client between pixmap_cache and
           global dictionary (in cases of multiple monitors) */
        if (!red_compress_image(display_channel, image, &qxl_image->bitmap,
                                drawable, &comp_send_data)) {
            red_display_add_image_to_pixmap_cache(display_channel, qxl_image, image);

            image->bitmap = qxl_image->bitmap;
            image->bitmap.flags = image->bitmap.flags & BITMAP_TOP_DOWN;
            add_buf(channel, BUF_TYPE_RAW, image, sizeof(BitmapImage));
            fill_palette(display_channel, &(image->bitmap.palette), &(image->bitmap.flags));
            data = (uint8_t *)(image->bitmap.data + channel->worker->dev_info.phys_delta);
            image->bitmap.data = channel->send_data.header.size;
            add_buf(channel,
                    (qxl_image->bitmap.flags & QXL_BITMAP_DIRECT) ? BUF_TYPE_RAW : BUF_TYPE_CHUNK,
                    data, image->bitmap.y * image->bitmap.stride);
        } else {
            red_display_add_image_to_pixmap_cache(display_channel, qxl_image, image);

            add_buf((RedChannel *)display_channel, BUF_TYPE_RAW, image, comp_send_data.raw_size);
            add_buf((RedChannel *)display_channel, BUF_TYPE_COMPRESS_BUF,
                    comp_send_data.comp_buf, comp_send_data.comp_buf_size);

            if (comp_send_data.plt_ptr != NULL) {
                fill_palette(display_channel, comp_send_data.plt_ptr, comp_send_data.flags_ptr);
            }
        }
        break;
    case IMAGE_TYPE_QUIC:
        red_display_add_image_to_pixmap_cache(display_channel, qxl_image, image);
        image->quic = qxl_image->quic;
        add_buf(channel, BUF_TYPE_RAW, image, sizeof(QUICImage));
        add_buf(channel, BUF_TYPE_CHUNK, qxl_image->quic.data, qxl_image->quic.data_size);
        break;
    default:
        red_error("invalid image type %u", image->descriptor.type);
    }
}

static void fill_brush(DisplayChannel *display_channel, Brush *brush, Drawable *drawable)
{
    if (brush->type == BRUSH_TYPE_PATTERN) {
        fill_bits(display_channel, &brush->u.pattern.pat, drawable);
    }
}

static void fill_mask(DisplayChannel *display_channel, QMask *mask, Drawable *drawable)
{
    if (mask->bitmap) {
        if (display_channel->base.worker->image_compression != IMAGE_COMPRESS_OFF) {
            image_compression_t save_img_comp = display_channel->base.worker->image_compression;
            display_channel->base.worker->image_compression = IMAGE_COMPRESS_OFF;
            fill_bits(display_channel, &mask->bitmap, drawable);
            display_channel->base.worker->image_compression = save_img_comp;
        } else {
            fill_bits(display_channel, &mask->bitmap, drawable);
        }
    }
}

static void fill_attr(DisplayChannel *display_channel, LineAttr *attr)
{
    if (attr->style_nseg) {
        RedChannel *channel = &display_channel->base;
        uint8_t *buf = (uint8_t *)(attr->style + channel->worker->dev_info.phys_delta);
        ASSERT(attr->style);
        attr->style = channel->send_data.header.size;
        add_buf(channel, BUF_TYPE_RAW, buf, attr->style_nseg * sizeof(uint32_t));
    }
}

static void fill_cursor(CursorChannel *cursor_channel, RedCursor *red_cursor, CursorItem *cursor)
{
    RedChannel *channel = &cursor_channel->base;

    if (!cursor) {
        red_cursor->flags = RED_CURSOR_NONE;
        return;
    }

    if (cursor->type == CURSOR_TYPE_DEV) {
        QXLCursorCmd *cursor_cmd;
        QXLCursor *qxl_cursor;

        cursor_cmd = CONTAINEROF(cursor, QXLCursorCmd, device_data);
        qxl_cursor = (QXLCursor *)(cursor_cmd->u.set.shape + channel->worker->dev_info.phys_delta);
        red_cursor->flags = 0;
        red_cursor->header = qxl_cursor->header;

        if (red_cursor->header.unique) {
            if (red_cursor_cache_find(cursor_channel, red_cursor->header.unique)) {
                red_cursor->flags |= RED_CURSOR_FROM_CACHE;
                return;
            }
            if (red_cursor_cache_add(cursor_channel, red_cursor->header.unique, 1)) {
                red_cursor->flags |= RED_CURSOR_CACHE_ME;
            }
        }

        if (qxl_cursor->data_size) {
            add_buf(channel, BUF_TYPE_CHUNK, &qxl_cursor->chunk, qxl_cursor->data_size);
        }
    } else {
        LocalCursor *local_cursor;
        ASSERT(cursor->type == CURSOR_TYPE_LOCAL);
        local_cursor = (LocalCursor *)cursor;
        *red_cursor = local_cursor->red_cursor;
        add_buf(channel, BUF_TYPE_RAW, local_cursor->red_cursor.data, local_cursor->data_size);
    }
}

static inline void red_channel_reset_send_data(RedChannel *channel)
{
    channel->send_data.pos = 0;
    channel->send_data.n_bufs = 1;
    channel->send_data.header.size = 0;
    channel->send_data.header.sub_list = 0;
    ++channel->send_data.header.serial;
    channel->send_data.bufs[0].type = BUF_TYPE_RAW;
    channel->send_data.bufs[0].size = sizeof(RedDataHeader);
    channel->send_data.bufs[0].data = (void *)&channel->send_data.header;
}

static inline void red_display_reset_send_data(DisplayChannel *channel)
{
    red_channel_reset_send_data((RedChannel *)channel);
    channel->send_data.bitmap_pos = 0;
    red_display_reset_compress_buf(channel);
    channel->send_data.free_list.res->count = 0;
    memset(channel->send_data.free_list.sync, 0, sizeof(channel->send_data.free_list.sync));
}

static inline void red_send_qxl_drawable(RedWorker *worker, DisplayChannel *display_channel,
                                         Drawable *item)
{
    QXLDrawable *drawable = item->qxl_drawable;

    RedChannel *channel = &display_channel->base;
    switch (drawable->type) {
    case QXL_DRAW_FILL:
        channel->send_data.header.type = RED_DISPLAY_DRAW_FILL;
        fill_base(display_channel, &display_channel->send_data.u.fill.base, drawable,
                  sizeof(RedFill));
        display_channel->send_data.u.fill.data = drawable->u.fill;
        fill_brush(display_channel, &display_channel->send_data.u.fill.data.brush, item);
        fill_mask(display_channel, &display_channel->send_data.u.fill.data.mask, item);
        break;
    case QXL_DRAW_OPAQUE:
        channel->send_data.header.type = RED_DISPLAY_DRAW_OPAQUE;
        fill_base(display_channel, &display_channel->send_data.u.opaque.base, drawable,
                  sizeof(RedOpaque));
        display_channel->send_data.u.opaque.data = drawable->u.opaque;
        fill_bits(display_channel, &display_channel->send_data.u.opaque.data.src_bitmap, item);
        fill_brush(display_channel, &display_channel->send_data.u.opaque.data.brush, item);
        fill_mask(display_channel, &display_channel->send_data.u.opaque.data.mask, item);
        break;
    case QXL_DRAW_COPY:
        channel->send_data.header.type = RED_DISPLAY_DRAW_COPY;
        fill_base(display_channel, &display_channel->send_data.u.copy.base, drawable,
                  sizeof(RedCopy));
        display_channel->send_data.u.copy.data = drawable->u.copy;
        fill_bits(display_channel, &display_channel->send_data.u.copy.data.src_bitmap, item);
        fill_mask(display_channel, &display_channel->send_data.u.copy.data.mask, item);
        break;
    case QXL_DRAW_TRANSPARENT:
        channel->send_data.header.type = RED_DISPLAY_DRAW_TRANSPARENT;
        fill_base(display_channel, &display_channel->send_data.u.transparent.base, drawable,
                  sizeof(RedTransparent));
        display_channel->send_data.u.transparent.data = drawable->u.transparent;
        fill_bits(display_channel, &display_channel->send_data.u.transparent.data.src_bitmap, item);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        channel->send_data.header.type = RED_DISPLAY_DRAW_ALPHA_BLEND;
        fill_base(display_channel, &display_channel->send_data.u.alpha_blend.base, drawable,
                  sizeof(RedAlphaBlend));
        display_channel->send_data.u.alpha_blend.data = drawable->u.alpha_blend;
        fill_bits(display_channel, &display_channel->send_data.u.alpha_blend.data.src_bitmap, item);
        break;
    case QXL_COPY_BITS:
        channel->send_data.header.type = RED_DISPLAY_COPY_BITS;
        fill_base(display_channel, &display_channel->send_data.u.copy_bits.base, drawable,
                  sizeof(RedCopyBits));
        display_channel->send_data.u.copy_bits.src_pos = drawable->u.copy_bits.src_pos;
        break;
    case QXL_DRAW_BLEND:
        channel->send_data.header.type = RED_DISPLAY_DRAW_BLEND;
        fill_base(display_channel, &display_channel->send_data.u.blend.base, drawable,
                  sizeof(RedBlend));
        display_channel->send_data.u.blend.data = drawable->u.blend;
        fill_bits(display_channel, &display_channel->send_data.u.blend.data.src_bitmap, item);
        fill_mask(display_channel, &display_channel->send_data.u.blend.data.mask, item);
        break;
    case QXL_DRAW_BLACKNESS:
        channel->send_data.header.type = RED_DISPLAY_DRAW_BLACKNESS;
        fill_base(display_channel, &display_channel->send_data.u.blackness.base, drawable,
                  sizeof(RedBlackness));
        display_channel->send_data.u.blackness.data = drawable->u.blackness;
        fill_mask(display_channel, &display_channel->send_data.u.blackness.data.mask, item);
        break;
    case QXL_DRAW_WHITENESS:
        channel->send_data.header.type = RED_DISPLAY_DRAW_WHITENESS;
        fill_base(display_channel, &display_channel->send_data.u.whiteness.base, drawable,
                  sizeof(RedWhiteness));
        display_channel->send_data.u.whiteness.data = drawable->u.whiteness;
        fill_mask(display_channel, &display_channel->send_data.u.whiteness.data.mask, item);
        break;
    case QXL_DRAW_INVERS:
        channel->send_data.header.type = RED_DISPLAY_DRAW_INVERS;
        fill_base(display_channel, &display_channel->send_data.u.invers.base, drawable,
                  sizeof(RedInvers));
        display_channel->send_data.u.invers.data = drawable->u.invers;
        fill_mask(display_channel, &display_channel->send_data.u.invers.data.mask, item);
        break;
    case QXL_DRAW_ROP3:
        channel->send_data.header.type = RED_DISPLAY_DRAW_ROP3;
        fill_base(display_channel, &display_channel->send_data.u.rop3.base, drawable,
                  sizeof(RedRop3));
        display_channel->send_data.u.rop3.data = drawable->u.rop3;
        fill_bits(display_channel, &display_channel->send_data.u.rop3.data.src_bitmap, item);
        fill_brush(display_channel, &display_channel->send_data.u.rop3.data.brush, item);
        fill_mask(display_channel, &display_channel->send_data.u.rop3.data.mask, item);
        break;
    case QXL_DRAW_STROKE:
        channel->send_data.header.type = RED_DISPLAY_DRAW_STROKE;
        fill_base(display_channel, &display_channel->send_data.u.stroke.base, drawable,
                  sizeof(RedStroke));
        display_channel->send_data.u.stroke.data = drawable->u.stroke;
        fill_path(display_channel, &display_channel->send_data.u.stroke.data.path);
        fill_attr(display_channel, &display_channel->send_data.u.stroke.data.attr);
        fill_brush(display_channel, &display_channel->send_data.u.stroke.data.brush, item);
        break;
    case QXL_DRAW_TEXT:
        channel->send_data.header.type = RED_DISPLAY_DRAW_TEXT;
        fill_base(display_channel, &display_channel->send_data.u.text.base, drawable,
                  sizeof(RedText));
        display_channel->send_data.u.text.data = drawable->u.text;
        fill_brush(display_channel, &display_channel->send_data.u.text.data.fore_brush, item);
        fill_brush(display_channel, &display_channel->send_data.u.text.data.back_brush, item);
        fill_str(display_channel, &display_channel->send_data.u.text.data.str);
        break;
    default:
        red_error("invalid type");
    }
    display_begin_send_massage(display_channel, &item->pipe_item);
}

#define MAX_SEND_VEC 100

static inline BufDescriptor *find_buf(RedChannel *channel, int buf_pos, int *buf_offset)
{
    BufDescriptor *buf;
    int pos = 0;

    for (buf = channel->send_data.bufs; buf_pos >= pos + buf->size; buf++) {
        pos += buf->size;
        ASSERT(buf != &channel->send_data.bufs[channel->send_data.n_bufs - 1]);
    }
    *buf_offset = buf_pos - pos;
    return buf;
}

static inline uint32_t __fill_iovec(BufDescriptor *buf, int skip, struct iovec *vec, int *vec_index,
                                    long phys_delta)
{
    uint32_t size = 0;

    switch (buf->type) {
    case BUF_TYPE_RAW:
        vec[*vec_index].iov_base = buf->data + skip;
        vec[*vec_index].iov_len = size = buf->size - skip;
        (*vec_index)++;
        break;
    case BUF_TYPE_COMPRESS_BUF: {
        RedCompressBuf *comp_buf = (RedCompressBuf *)buf->data;
        int max = buf->size - skip;
        int now;
        do {
            ASSERT(comp_buf);
            if (skip >= sizeof(comp_buf->buf)) {
                skip -= sizeof(comp_buf->buf);
                comp_buf = comp_buf->send_next;
                continue;
            }
            now = MIN(sizeof(comp_buf->buf) - skip, max);
            max -= now;
            size += now;
            vec[*vec_index].iov_base = (uint8_t*)comp_buf->buf + skip;
            vec[*vec_index].iov_len = now;
            skip = 0;
            comp_buf = comp_buf->send_next;
            (*vec_index)++;
        } while (max && *vec_index < MAX_SEND_VEC);
        break;
    }
    case BUF_TYPE_CHUNK: {
        QXLDataChunk *chunk = (QXLDataChunk *)buf->data;
        do {
            int data_size = chunk->data_size;
            int skip_now = MIN(data_size, skip);
            skip -= skip_now;
            data_size -= skip_now;

            if (data_size) {
                size += data_size;
                vec[*vec_index].iov_base = chunk->data + skip_now;
                vec[*vec_index].iov_len = data_size;
                (*vec_index)++;
            }
            chunk = chunk->next_chunk ? (QXLDataChunk *)(chunk->next_chunk + phys_delta) : NULL;
        } while (chunk && *vec_index < MAX_SEND_VEC);
        break;
    }
    default:
        red_error("invalid type");
    }
    return size;
}

static inline void fill_iovec(RedChannel *channel, struct iovec *vec, int *vec_size)
{
    int vec_index = 0;
    uint32_t pos = channel->send_data.pos;

    ASSERT(channel->send_data.size != pos && channel->send_data.size > pos);

    do {
        BufDescriptor *buf;
        int buf_offset;

        buf = find_buf(channel, pos, &buf_offset);
        ASSERT(buf);
        pos += __fill_iovec(buf, buf_offset, vec, &vec_index, channel->worker->dev_info.phys_delta);
    } while (vec_index < MAX_SEND_VEC && pos != channel->send_data.size);
    *vec_size = vec_index;
}

static void inline channel_release_res(RedChannel *channel)
{
    if (!channel->send_data.item) {
        return;
    }
    channel->release_item(channel, channel->send_data.item);
    channel->send_data.item = NULL;
}

static void red_send_data(RedChannel *channel, void *item)
{
    for (;;) {
        uint32_t n = channel->send_data.size - channel->send_data.pos;
        struct iovec vec[MAX_SEND_VEC];
        int vec_size;

        if (!n) {
            channel->send_data.blocked = FALSE;
            if (channel->send_data.item) {
                channel->release_item(channel, channel->send_data.item);
                channel->send_data.item = NULL;
            }
            break;
        }
        fill_iovec(channel, vec, &vec_size);
        ASSERT(channel->peer);
        if ((n = channel->peer->cb_writev(channel->peer->ctx, vec, vec_size)) == -1) {
            switch (errno) {
            case EAGAIN:
                channel->send_data.blocked = TRUE;
                if (item) {
                    channel->hold_item(item);
                    channel->send_data.item = item;
                }
                return;
            case EINTR:
                break;
            case EPIPE:
                channel->disconnect(channel);
                return;
            default:
                red_printf("%s", strerror(errno));
                channel->disconnect(channel);
                return;
            }
        } else {
            channel->send_data.pos += n;
            stat_inc_counter(channel->out_bytes_counter, n);
        }
    }
}

static void display_channel_push_release(DisplayChannel *channel, uint8_t type, uint64_t id,
                                         uint64_t* sync_data)
{
    FreeList *free_list = &channel->send_data.free_list;
    int i;

    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        free_list->sync[i] = MAX(free_list->sync[i], sync_data[i]);
    }

    if (free_list->res->count == free_list->res_size) {
        RedResorceList *new_list;
        new_list = malloc(sizeof(*new_list) + free_list->res_size * sizeof(RedResorceID) * 2);
        if (!new_list) {
            PANIC("malloc failed");
        }
        new_list->count = free_list->res->count;
        memcpy(new_list->resorces, free_list->res->resorces,
               new_list->count * sizeof(RedResorceID));
        free(free_list->res);
        free_list->res = new_list;
        free_list->res_size *= 2;
    }
    free_list->res->resorces[free_list->res->count].type = type;
    free_list->res->resorces[free_list->res->count++].id = id;
}

static inline void red_begin_send_massage(RedChannel *channel, void *item)
{
    channel->send_data.size = channel->send_data.header.size + sizeof(RedDataHeader);
    channel->messages_window++;
    red_send_data(channel, item);
}

static inline void display_begin_send_massage(DisplayChannel *channel, void *item)
{
    FreeList *free_list = &channel->send_data.free_list;

    if (free_list->res->count) {
        int sync_count = 0;
        int sub_index;
        int i;

        channel->base.send_data.header.sub_list = channel->base.send_data.header.size;
        for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
            if (i != channel->base.id && free_list->sync[i] != 0) {
                free_list->wait.header.wait_list[sync_count].channel_type = RED_CHANNEL_DISPLAY;
                free_list->wait.header.wait_list[sync_count].channel_id = i;
                free_list->wait.header.wait_list[sync_count++].message_serial = free_list->sync[i];
            }
        }
        RedSubMessageList *sub_list = &channel->send_data.sub_list.sub_list;
        RedSubMessage *sub_header = channel->send_data.sub_header;
        if (sync_count) {
            sub_list->size = 2;
            add_buf((RedChannel*)channel, BUF_TYPE_RAW, sub_list,
                    sizeof(*sub_list) + 2 * sizeof(sub_list->sub_messages[0]));
            sub_list->sub_messages[0] = channel->base.send_data.header.size;
            sub_header[0].type = RED_WAIT_FOR_CHANNELS;
            sub_header[0].size = sizeof(free_list->wait.header) +
                                 sync_count * sizeof(free_list->wait.buf[0]);
            add_buf((RedChannel*)channel, BUF_TYPE_RAW, sub_header, sizeof(*sub_header));
            free_list->wait.header.wait_count = sync_count;
            add_buf((RedChannel*)channel, BUF_TYPE_RAW, &free_list->wait.header,
                    sub_header[0].size);
            sub_list->sub_messages[1] = channel->base.send_data.header.size;
            sub_index = 1;
        } else {
            sub_list->size = 1;
            add_buf((RedChannel*)channel, BUF_TYPE_RAW, sub_list,
                    sizeof(*sub_list) + sizeof(sub_list->sub_messages[0]));
            sub_list->sub_messages[0] = channel->base.send_data.header.size;
            sub_index = 0;
        }
        sub_header[sub_index].type = RED_DISPLAY_INVAL_LIST;
        sub_header[sub_index].size = sizeof(*free_list->res) + free_list->res->count *
                                     sizeof(free_list->res->resorces[0]);
        add_buf((RedChannel*)channel, BUF_TYPE_RAW, &sub_header[sub_index], sizeof(*sub_header));
        add_buf((RedChannel*)channel, BUF_TYPE_RAW, free_list->res, sub_header[sub_index].size);
    }
    red_begin_send_massage((RedChannel *)channel, item);
}

static inline RedChannel *red_ref_channel(RedChannel *channel)
{
    if (!channel) {
        return NULL;
    }
    ++channel->listener.refs;
    return channel;
}

static inline void red_unref_channel(RedChannel *channel)
{
    ASSERT(channel);
    if (!--channel->listener.refs) {
        free(channel);
    }
}

static inline uint8_t *red_get_image_line(QXLDataChunk **chunk, int *offset, int stride,
                                          long phys_delta)
{
    uint8_t *ret;
    if ((*chunk)->data_size == *offset) {
        if ((*chunk)->next_chunk == 0) {
            return NULL;
        }
        *offset = 0;
        *chunk = (QXLDataChunk *)((*chunk)->next_chunk + phys_delta);
    }

    if ((*chunk)->data_size - *offset < stride) {
        red_printf("bad chunk aligment");
        return NULL;
    }
    ret = (*chunk)->data + *offset;
    *offset += stride;
    return ret;
}

static void red_display_share_stream_buf(DisplayChannel *display_channel)
{
}

static void red_display_unshare_stream_buf(DisplayChannel *display_channel)
{
}

#define YUV32
#include "red_yuv.h"
#undef YUV32

#define YUV24
#include "red_yuv.h"
#undef YUV24

#define YUV16
#include "red_yuv.h"
#undef YUV16

static inline int red_send_stream_data(DisplayChannel *display_channel, Drawable *drawable)
{
    Stream *stream = drawable->stream;
    QXLImage *qxl_image;
    RedChannel *channel;
    RedWorker* worker;
    int n;

    ASSERT(stream);
    ASSERT(drawable->qxl_drawable->type == QXL_DRAW_COPY);

    channel = &display_channel->base;
    worker = channel->worker;
    qxl_image = (QXLImage *)(drawable->qxl_drawable->u.copy.src_bitmap +
                             worker->dev_info.phys_delta);

    if (qxl_image->descriptor.type != IMAGE_TYPE_BITMAP ||
                                            (qxl_image->bitmap.flags & QXL_BITMAP_DIRECT)) {
        return FALSE;
    }

    StreamAgent *agent = &display_channel->stream_agents[stream - worker->streams_buf];
    uint64_t time_now = red_now();
    if (time_now - agent->lats_send_time < (1000 * 1000 * 1000) / agent->fps) {
        agent->frames--;
        return TRUE;
    }

    switch (qxl_image->bitmap.format) {
    case BITMAP_FMT_32BIT:
        if (!red_rgb_to_yuv420_32bpp(&drawable->qxl_drawable->u.copy.src_area,
                                     &qxl_image->bitmap, stream->av_frame,
                                     worker->dev_info.phys_delta,
                                     stream - worker->streams_buf, stream)) {
            return FALSE;
        }
        break;
    case BITMAP_FMT_16BIT:
        if (!red_rgb_to_yuv420_16bpp(&drawable->qxl_drawable->u.copy.src_area,
                                     &qxl_image->bitmap, stream->av_frame,
                                     worker->dev_info.phys_delta,
                                     stream - worker->streams_buf, stream)) {
            return FALSE;
        }
        break;
    case BITMAP_FMT_24BIT:
        if (!red_rgb_to_yuv420_24bpp(&drawable->qxl_drawable->u.copy.src_area,
                                     &qxl_image->bitmap, stream->av_frame,
                                     worker->dev_info.phys_delta,
                                     stream - worker->streams_buf, stream)) {
            return FALSE;
        }
        break;
    default:
        red_printf_some(1000, "unsupported format %d", qxl_image->bitmap.format);
        return FALSE;
    }
#if 1
    uint32_t min_buf_size = stream->av_ctx->width * stream->av_ctx->height * sizeof(uint32_t) / 2;
    min_buf_size += FF_MIN_BUFFER_SIZE;
    if (display_channel->send_data.stream_outbuf_size < min_buf_size) {
        uint8_t *new_buf;

        if (!(new_buf = malloc(min_buf_size + FF_INPUT_BUFFER_PADDING_SIZE))) {
            return FALSE;
        }

        red_display_unshare_stream_buf(display_channel);
        free(display_channel->send_data.stream_outbuf);
        display_channel->send_data.stream_outbuf = new_buf;
        display_channel->send_data.stream_outbuf_size = min_buf_size;
        red_display_share_stream_buf(display_channel);
    }
    n = avcodec_encode_video(stream->av_ctx, display_channel->send_data.stream_outbuf,
                             display_channel->send_data.stream_outbuf_size,
                             stream->av_frame);

    if (n <= 0) {
        red_printf("avcodec_encode_video failed");
        return FALSE;
    }

    if (n > display_channel->send_data.stream_outbuf_size) {
        PANIC("buf error");
    }
#else
    for (;;) {
        n = avcodec_encode_video(stream->av_ctx, display_channel->send_data.stream_outbuf,
                                 display_channel->send_data.stream_outbuf_size,
                                 stream->av_frame);
        if (n < 0) {
            uint8_t *new_buf;
            size_t max_size;
            size_t new_size;

            max_size = stream->av_ctx->width * stream->av_ctx->height * sizeof(uint32_t);
            max_size += MAX(1024, max_size / 10);

            if (display_channel->send_data.stream_outbuf_size == max_size) {
                return FALSE;
            }

            new_size = display_channel->send_data.stream_outbuf_size * 2;
            new_size = MIN(new_size, max_size);

            if (!(new_buf = malloc(new_size + FF_INPUT_BUFFER_PADDING_SIZE))) {
                return FALSE;
            }
            red_printf("new streaming video buf size %u", new_size);
            red_display_unshare_stream_buf(display_channel);
            free(display_channel->send_data.stream_outbuf);
            display_channel->send_data.stream_outbuf = new_buf;
            display_channel->send_data.stream_outbuf_size = new_size;
            red_display_share_stream_buf(display_channel);
            continue;
        }
        ASSERT(n <= display_channel->send_data.stream_outbuf_size);
        break;
    }
 #endif
    channel->send_data.header.type = RED_DISPLAY_STREAM_DATA;
    RedStreamData* stream_data = &display_channel->send_data.u.stream_data;
    add_buf(channel, BUF_TYPE_RAW, stream_data, sizeof(RedStreamData));
    add_buf(channel, BUF_TYPE_RAW, display_channel->send_data.stream_outbuf,
            n + FF_INPUT_BUFFER_PADDING_SIZE);

    stream_data->id = stream - worker->streams_buf;
    stream_data->multi_media_time = drawable->qxl_drawable->mm_time;
    stream_data->data_size = n;
    stream_data->ped_size = FF_INPUT_BUFFER_PADDING_SIZE;
    display_begin_send_massage(display_channel, NULL);
    agent->lats_send_time = time_now;
    return TRUE;
}

static inline void send_qxl_drawable(DisplayChannel *display_channel, Drawable *item)
{
    RedChannel *channel;

    ASSERT(display_channel);
    channel = &display_channel->base;

    if (item->stream && red_send_stream_data(display_channel, item)) {
        return;
    }
    red_send_qxl_drawable(display_channel->base.worker, display_channel, item);
}

static void red_send_mode(DisplayChannel *display_channel)
{
    RedChannel *channel;
    RedWorker *worker;

    ASSERT(display_channel);
    worker = display_channel->base.worker;
    channel = &display_channel->base;
    channel->send_data.header.type = RED_DISPLAY_MODE;
    display_channel->send_data.u.mode.x_res = worker->dev_info.x_res;
    display_channel->send_data.u.mode.y_res = worker->dev_info.y_res;
    display_channel->send_data.u.mode.bits = worker->dev_info.bits;

    add_buf(channel, BUF_TYPE_RAW, &display_channel->send_data.u.mode, sizeof(RedMode));

    display_begin_send_massage(display_channel, NULL);
}

static void red_send_set_ack(RedChannel *channel)
{
    ASSERT(channel);
    channel->send_data.header.type = RED_SET_ACK;
    channel->send_data.u.ack.generation = ++channel->ack_generation;
    channel->send_data.u.ack.window = channel->client_ack_window;
    channel->messages_window = 0;

    add_buf(channel, BUF_TYPE_RAW, &channel->send_data.u.ack, sizeof(RedSetAck));
    red_begin_send_massage(channel, NULL);
}

static inline void red_send_verb(RedChannel *channel, uint16_t verb)
{
    ASSERT(channel);
    channel->send_data.header.type = verb;
    red_begin_send_massage(channel, NULL);
}

static inline void display_send_verb(DisplayChannel *channel, uint16_t verb)
{
    ASSERT(channel);
    channel->base.send_data.header.type = verb;
    display_begin_send_massage(channel, NULL);
}

static inline void __red_send_inval(RedChannel *channel, CacheItem *cach_item,
                                    RedInvalOne *inval_one)
{
    channel->send_data.header.type = cach_item->inval_type;
    inval_one->id = *(uint64_t *)&cach_item->id;
    add_buf(channel, BUF_TYPE_RAW, inval_one, sizeof(*inval_one));
}

static void red_send_inval(RedChannel *channel, CacheItem *cach_item, RedInvalOne *inval_one)
{
    __red_send_inval(channel, cach_item, inval_one);
    red_begin_send_massage(channel, NULL);
}

static void red_display_send_inval(DisplayChannel *display_channel, CacheItem *cach_item)
{
    __red_send_inval((RedChannel *)display_channel, cach_item,
                     &display_channel->send_data.u.inval_one);
    display_begin_send_massage(display_channel, NULL);
}

static void display_channel_send_migrate(DisplayChannel *display_channel)
{
    display_channel->base.send_data.header.type = RED_MIGRATE;
    display_channel->send_data.u.migrate.flags = RED_MIGRATE_NEED_FLUSH |
                                                 RED_MIGRATE_NEED_DATA_TRANSFER;
    add_buf((RedChannel*)display_channel, BUF_TYPE_RAW, &display_channel->send_data.u.migrate,
            sizeof(display_channel->send_data.u.migrate));
    display_channel->expect_migrate_mark = TRUE;
    display_begin_send_massage(display_channel, NULL);
}

static void display_channel_send_migrate_data(DisplayChannel *display_channel)
{
    DisplayChannelMigrateData* display_data;

    display_channel->base.send_data.header.type = RED_MIGRATE_DATA;
    ASSERT(display_channel->pixmap_cache);
    display_data = &display_channel->send_data.u.migrate_data;
    display_data->magic = DISPLAY_MIGRATE_DATA_MAGIC;
    ASSERT(MAX_CACHE_CLIENTS == 4); //MIGRATE_DATA_VERSION dependent
    display_data->version = DISPLAY_MIGRATE_DATA_VERSION;

    display_data->message_serial = channel_message_serial((RedChannel *)display_channel);

    display_data->pixmap_cache_freezer = pixmap_cache_freeze(display_channel->pixmap_cache);
    display_data->pixmap_cache_id = display_channel->pixmap_cache->id;
    display_data->pixmap_cache_size = display_channel->pixmap_cache->size;
    memcpy(display_data->pixmap_cache_clients, display_channel->pixmap_cache->sync,
           sizeof(display_data->pixmap_cache_clients));

    ASSERT(display_channel->glz_dict);
    red_freeze_glz(display_channel);
    display_data->glz_dict_id = display_channel->glz_dict->id;
    glz_enc_dictionary_get_restore_data(display_channel->glz_dict->dict,
                                        &display_data->glz_dict_restore_data,
                                        &display_channel->glz_data.usr);

    add_buf((RedChannel *)display_channel, BUF_TYPE_RAW, &display_channel->send_data.u.migrate_data,
            sizeof(display_channel->send_data.u.migrate_data));
    display_begin_send_massage(display_channel, NULL);
}

static void display_channel_pixmap_sync(DisplayChannel *display_channel)
{
    RedWaitForChannels *wait;
    PixmapCache *pixmap_cache;


    display_channel->base.send_data.header.type = RED_WAIT_FOR_CHANNELS;
    wait = &display_channel->send_data.u.wait.header;
    pixmap_cache = display_channel->pixmap_cache;


    pthread_mutex_lock(&pixmap_cache->lock);

    wait->wait_count = 1;
    wait->wait_list[0].channel_type = RED_CHANNEL_DISPLAY;
    wait->wait_list[0].channel_id = pixmap_cache->generation_initiator.client;
    wait->wait_list[0].message_serial = pixmap_cache->generation_initiator.message;
    display_channel->pixmap_cache_generation = pixmap_cache->generation;
    display_channel->pending_pixmaps_sync = FALSE;

    pthread_mutex_unlock(&pixmap_cache->lock);

    add_buf((RedChannel *)display_channel, BUF_TYPE_RAW, wait,
            sizeof(*wait) + sizeof(wait->wait_list[0]));
    display_begin_send_massage(display_channel, NULL);
}

static void display_channel_reset_cache(DisplayChannel *display_channel)
{
    RedWaitForChannels *wait = &display_channel->send_data.u.wait.header;

    display_channel->base.send_data.header.type = RED_DISPLAY_INVAL_ALL_PIXMAPS;
    pixmap_cache_reset(display_channel->pixmap_cache, display_channel, wait);

    add_buf((RedChannel *)display_channel, BUF_TYPE_RAW, wait,
            sizeof(*wait) + wait->wait_count * sizeof(wait->wait_list[0]));
    display_begin_send_massage(display_channel, NULL);
}

static void red_send_image(DisplayChannel *display_channel, ImageItem *item)
{
    RedChannel *channel;
    RedImage *red_image;
    RedWorker *worker;
    Bitmap bitmap;

    ASSERT(display_channel && item);
    channel = &display_channel->base;
    worker = channel->worker;

    red_image = alloc_image(display_channel);
    ASSERT(red_image);

    QXL_SET_IMAGE_ID(red_image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    red_image->descriptor.type = IMAGE_TYPE_BITMAP;
    red_image->descriptor.flags = 0;
    red_image->descriptor.width = item->width;
    red_image->descriptor.height = item->height;

    bitmap.format = BITMAP_FMT_32BIT;
    bitmap.flags = QXL_BITMAP_DIRECT;
    bitmap.flags |= item->top_down ? QXL_BITMAP_TOP_DOWN : 0;
    bitmap.x = item->width;
    bitmap.y = item->height;
    bitmap.stride = item->stride;
    bitmap.palette = 0;
    bitmap.data = (ADDRESS)(item->data) - worker->dev_info.phys_delta;

    channel->send_data.header.type = RED_DISPLAY_DRAW_COPY;

    add_buf(channel, BUF_TYPE_RAW, &display_channel->send_data.u.copy, sizeof(RedCopy));
    display_channel->send_data.u.copy.base.box.left = item->pos.x;
    display_channel->send_data.u.copy.base.box.top = item->pos.y;
    display_channel->send_data.u.copy.base.box.right = item->pos.x + bitmap.x;
    display_channel->send_data.u.copy.base.box.bottom = item->pos.y + bitmap.y;
    display_channel->send_data.u.copy.base.clip.type = CLIP_TYPE_NONE;
    display_channel->send_data.u.copy.base.clip.data = 0;
    display_channel->send_data.u.copy.data.rop_decriptor = ROPD_OP_PUT;
    display_channel->send_data.u.copy.data.src_area.left = 0;
    display_channel->send_data.u.copy.data.src_area.top = 0;
    display_channel->send_data.u.copy.data.src_area.right = bitmap.x;
    display_channel->send_data.u.copy.data.src_area.bottom = bitmap.y;
    display_channel->send_data.u.copy.data.scale_mode = 0;
    display_channel->send_data.u.copy.data.src_bitmap = channel->send_data.header.size;
    display_channel->send_data.u.copy.data.mask.bitmap = 0;

    compress_send_data_t comp_send_data;

    if (red_quic_compress_image(display_channel, red_image, &bitmap, &comp_send_data)) {
        add_buf(channel, BUF_TYPE_RAW, red_image, comp_send_data.raw_size);
        add_buf(channel, BUF_TYPE_COMPRESS_BUF, comp_send_data.comp_buf,
                comp_send_data.comp_buf_size);
    } else {
        red_image->descriptor.type = IMAGE_TYPE_BITMAP;
        red_image->bitmap = bitmap;
        red_image->bitmap.flags &= ~QXL_BITMAP_DIRECT;
        add_buf(channel, BUF_TYPE_RAW, red_image, sizeof(BitmapImage));
        red_image->bitmap.data = channel->send_data.header.size;
        add_buf(channel, BUF_TYPE_RAW, item->data, bitmap.y * bitmap.stride);
    }
    display_begin_send_massage(display_channel, &item->link);
}

static void red_display_send_upgrade(DisplayChannel *display_channel, UpgradeItem *item)
{
    RedChannel *channel;
    QXLDrawable *qxl_drawable;
    RedCopy *copy = &display_channel->send_data.u.copy;

    ASSERT(display_channel && item && item->drawable);
    channel = &display_channel->base;

    channel->send_data.header.type = RED_DISPLAY_DRAW_COPY;

    qxl_drawable = item->drawable->qxl_drawable;
    ASSERT(qxl_drawable->type == QXL_DRAW_COPY);
    ASSERT(qxl_drawable->u.copy.rop_decriptor == ROPD_OP_PUT);
    ASSERT(qxl_drawable->u.copy.mask.bitmap == 0);

    add_buf(channel, BUF_TYPE_RAW, copy, sizeof(RedCopy));
    copy->base.box = qxl_drawable->bbox;
    copy->base.clip.type = CLIP_TYPE_RECTS;
    copy->base.clip.data = channel->send_data.header.size;
    add_buf(channel, BUF_TYPE_RAW, &item->region.num_rects, sizeof(uint32_t));
    add_buf(channel, BUF_TYPE_RAW, item->region.rects, sizeof(Rect) * item->region.num_rects);
    copy->data = qxl_drawable->u.copy;
    fill_bits(display_channel, &copy->data.src_bitmap, item->drawable);

    display_begin_send_massage(display_channel, &item->base);
}

static void red_display_send_stream_start(DisplayChannel *display_channel, StreamAgent *agent)
{
    RedChannel *channel = &display_channel->base;
    Stream *stream = agent->stream;

    agent->lats_send_time = 0;
    ASSERT(stream);
    channel->send_data.header.type = RED_DISPLAY_STREAM_CREATE;
    RedStreamCreate *stream_create = &display_channel->send_data.u.stream_create.message;
    stream_create->id = agent - display_channel->stream_agents;
    stream_create->flags = stream->top_down ? STREAM_TOP_DOWN : 0;
    stream_create->codec_type = RED_VIDEO_CODEC_TYPE_MJPEG;

    stream_create->src_width = stream->width;
    stream_create->src_height = stream->height;
    stream_create->stream_width = ALIGN(stream_create->src_width, 2);
    stream_create->stream_height = ALIGN(stream_create->src_height, 2);
    stream_create->dest = stream->dest_area;

    add_buf(channel, BUF_TYPE_RAW, stream_create, sizeof(*stream_create));
    if (stream->current) {
        QXLDrawable *qxl_drawable = stream->current->qxl_drawable;
        stream_create->clip = qxl_drawable->clip;
        if (qxl_drawable->clip.type == CLIP_TYPE_RECTS) {
            fill_rects_clip(channel, &stream_create->clip.data);
        } else {
            ASSERT(qxl_drawable->clip.type == CLIP_TYPE_NONE);
        }
        display_begin_send_massage(display_channel, &stream->current->pipe_item);
    } else {
        stream_create->clip.type = CLIP_TYPE_RECTS;
        stream_create->clip.data = channel->send_data.header.size;
        display_channel->send_data.u.stream_create.num_rects = 0;
        add_buf(channel, BUF_TYPE_RAW, &display_channel->send_data.u.stream_create.num_rects,
                sizeof(uint32_t));
        display_begin_send_massage(display_channel, NULL);
    }
}

static void red_display_send_stream_clip(DisplayChannel *display_channel,
                                         StreamClipItem *item)
{
    RedChannel *channel = &display_channel->base;

    StreamAgent *agent = item->stream_agent;
    Stream *stream = agent->stream;

    ASSERT(stream);

    channel->send_data.header.type = RED_DISPLAY_STREAM_CLIP;
    RedStreamClip *stream_clip = &display_channel->send_data.u.stream_clip;
    add_buf(channel, BUF_TYPE_RAW, stream_clip, sizeof(*stream_clip));
    stream_clip->id = agent - display_channel->stream_agents;
    if ((stream_clip->clip.type = item->clip_type) == CLIP_TYPE_NONE) {
        stream_clip->clip.data = 0;
    } else {
        ASSERT(stream_clip->clip.type == CLIP_TYPE_RECTS);
        stream_clip->clip.data = channel->send_data.header.size;
        add_buf(channel, BUF_TYPE_RAW, &item->region.num_rects, sizeof(uint32_t));
        add_buf(channel, BUF_TYPE_RAW, item->region.rects,
                item->region.num_rects * sizeof(Rect));
    }
    display_begin_send_massage(display_channel, item);
}

static void red_display_send_stream_end(DisplayChannel *display_channel, StreamAgent* agent)
{
    RedChannel *channel = &display_channel->base;
    channel->send_data.header.type = RED_DISPLAY_STREAM_DESTROY;
    display_channel->send_data.u.stream_destroy.id = agent - display_channel->stream_agents;
    add_buf(channel, BUF_TYPE_RAW, &display_channel->send_data.u.stream_destroy,
            sizeof(RedStreamDestroy));
    display_begin_send_massage(display_channel, NULL);
}

static void red_cursor_send_inval(CursorChannel *channel, CacheItem *cach_item)
{
    ASSERT(channel);
    red_send_inval((RedChannel *)channel, cach_item, &channel->send_data.u.inval_one);
}

static void red_send_cursor_init(CursorChannel *channel)
{
    RedWorker *worker;
    ASSERT(channel);

    worker = channel->base.worker;

    channel->base.send_data.header.type = RED_CURSOR_INIT;
    channel->send_data.u.cursor_init.visible = worker->cursor_visible;
    channel->send_data.u.cursor_init.position = worker->cursor_position;
    channel->send_data.u.cursor_init.trail_length = worker->cursor_trail_length;
    channel->send_data.u.cursor_init.trail_frequency = worker->cursor_trail_frequency;
    add_buf(&channel->base, BUF_TYPE_RAW, &channel->send_data.u.cursor_init, sizeof(RedCursorInit));

    fill_cursor(channel, &channel->send_data.u.cursor_init.cursor, worker->cursor);

    red_begin_send_massage(&channel->base, worker->cursor);
}

static void red_send_local_cursor(CursorChannel *cursor_channel, LocalCursor *cursor)
{
    RedChannel *channel;

    ASSERT(cursor_channel);

    channel = &cursor_channel->base;
    channel->send_data.header.type = RED_CURSOR_SET;
    cursor_channel->send_data.u.cursor_set.postition = cursor->position;
    cursor_channel->send_data.u.cursor_set.visible = channel->worker->cursor_visible;
    add_buf(channel, BUF_TYPE_RAW, &cursor_channel->send_data.u.cursor_set,
            sizeof(RedCursorSet));
    fill_cursor(cursor_channel, &cursor_channel->send_data.u.cursor_set.cursor, &cursor->base);

    red_begin_send_massage(channel, cursor);

    red_release_cursor(channel->worker, (CursorItem *)cursor);
}

static void cursor_channel_send_migrate(CursorChannel *cursor_channel)
{
    cursor_channel->base.send_data.header.type = RED_MIGRATE;
    cursor_channel->send_data.u.migrate.flags = 0;
    add_buf((RedChannel*)cursor_channel, BUF_TYPE_RAW, &cursor_channel->send_data.u.migrate,
            sizeof(cursor_channel->send_data.u.migrate));
    red_begin_send_massage((RedChannel*)cursor_channel, NULL);
}

static void red_send_cursor(CursorChannel *cursor_channel, CursorItem *cursor)
{
    RedChannel *channel;
    QXLCursorCmd *cmd;

    ASSERT(cursor_channel);

    channel = &cursor_channel->base;

    cmd = CONTAINEROF(cursor, QXLCursorCmd, device_data);
    switch (cmd->type) {
    case QXL_CURSOR_MOVE:
        channel->send_data.header.type = RED_CURSOR_MOVE;
        cursor_channel->send_data.u.cursor_move.postition = cmd->u.position;
        add_buf(channel, BUF_TYPE_RAW, &cursor_channel->send_data.u.cursor_move,
                sizeof(RedCursorMove));
        break;
    case QXL_CURSOR_SET:
        channel->send_data.header.type = RED_CURSOR_SET;
        cursor_channel->send_data.u.cursor_set.postition = cmd->u.set.position;
        cursor_channel->send_data.u.cursor_set.visible = channel->worker->cursor_visible;
        add_buf(channel, BUF_TYPE_RAW, &cursor_channel->send_data.u.cursor_set,
                sizeof(RedCursorSet));
        fill_cursor(cursor_channel, &cursor_channel->send_data.u.cursor_set.cursor, cursor);
        break;
    case QXL_CURSOR_HIDE:
        channel->send_data.header.type = RED_CURSOR_HIDE;
        break;
    case QXL_CURSOR_TRAIL:
        channel->send_data.header.type = RED_CURSOR_TRAIL;
        cursor_channel->send_data.u.cursor_trail.length = cmd->u.trail.length;
        cursor_channel->send_data.u.cursor_trail.frequency = cmd->u.trail.frequency;
        add_buf(channel, BUF_TYPE_RAW, &cursor_channel->send_data.u.cursor_trail,
                sizeof(RedCursorTrail));
        break;
    default:
        red_error("bad cursor command %d", cmd->type);
    }

    red_begin_send_massage(channel, cursor);

    red_release_cursor(channel->worker, cursor);
}

static inline PipeItem *red_pipe_get(RedChannel *channel)
{
    PipeItem *item;
    if (!channel || channel->send_data.blocked ||
                                            !(item = (PipeItem *)ring_get_tail(&channel->pipe))) {
        return NULL;
    }

    if (channel->messages_window > channel->client_ack_window * 2) {
        channel->send_data.blocked = TRUE;
        return NULL;
    }

    --channel->pipe_size;
    ring_remove(&item->link);
    return item;
}

static void display_channel_push(RedWorker *worker)
{
    PipeItem *pipe_item;

    while ((pipe_item = red_pipe_get((RedChannel *)worker->display_channel))) {
        DisplayChannel *display_channel;
        display_channel = (DisplayChannel *)red_ref_channel((RedChannel *)worker->display_channel);
        red_display_reset_send_data(display_channel);
        switch (pipe_item->type) {
        case PIPE_ITEM_TYPE_DRAW: {
            Drawable *drawable = CONTAINEROF(pipe_item, Drawable, pipe_item);
            send_qxl_drawable(display_channel, drawable);
            release_drawable(worker, drawable);
            break;
        }
        case PIPE_ITEM_TYPE_INVAL_ONE:
            red_display_send_inval(display_channel, (CacheItem *)pipe_item);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_STREAM_CREATE: {
            StreamAgent *agent = CONTAINEROF(pipe_item, StreamAgent, create_item);
            red_display_send_stream_start(display_channel, agent);
            red_display_release_stream(display_channel, agent);
            break;
        }
        case PIPE_ITEM_TYPE_STREAM_CLIP: {
            StreamClipItem* clip_item = (StreamClipItem *)pipe_item;
            red_display_send_stream_clip(display_channel, clip_item);
            red_display_release_stream_clip(display_channel, clip_item);
            break;
        }
        case PIPE_ITEM_TYPE_STREAM_DESTROY: {
            StreamAgent *agent = CONTAINEROF(pipe_item, StreamAgent, destroy_item);
            red_display_send_stream_end(display_channel, agent);
            red_display_release_stream(display_channel, agent);
            break;
        }
        case PIPE_ITEM_TYPE_UPGRADE:
            red_display_send_upgrade(display_channel, (UpgradeItem *)pipe_item);
            release_upgrade_item(worker, (UpgradeItem *)pipe_item);
            break;
        case PIPE_ITEM_TYPE_VERB:
            display_send_verb(display_channel, ((VerbItem*)pipe_item)->verb);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_MODE:
            red_send_mode(display_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_MIGRATE:
            red_printf("PIPE_ITEM_TYPE_MIGRATE");
            display_channel_send_migrate(display_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_MIGRATE_DATA:
            display_channel_send_migrate_data(display_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_SET_ACK:
            red_send_set_ack((RedChannel *)display_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_IMAGE:
            red_send_image(display_channel, (ImageItem *)pipe_item);
            release_image_item((ImageItem *)pipe_item);
            break;
        case PIPE_ITEM_TYPE_PIXMAP_SYNC:
            display_channel_pixmap_sync(display_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_PIXMAP_RESET:
            display_channel_reset_cache(display_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
            red_reset_palette_cache(display_channel);
            red_send_verb((RedChannel *)display_channel, RED_DISPLAY_INVAL_ALL_PALETTES);
            free(pipe_item);
            break;
        default:
            red_error("invalid pipe item type");
        }
        red_unref_channel((RedChannel *)display_channel);
    }
}

static void cursor_channel_push(RedWorker *worker)
{
    PipeItem *pipe_item;

    while ((pipe_item = red_pipe_get((RedChannel *)worker->cursor_channel))) {
        CursorChannel *cursor_channel;

        cursor_channel = (CursorChannel *)red_ref_channel((RedChannel *)worker->cursor_channel);
        red_channel_reset_send_data((RedChannel*)cursor_channel);
        switch (pipe_item->type) {
        case PIPE_ITEM_TYPE_CURSOR:
            red_send_cursor(cursor_channel, (CursorItem *)pipe_item);
            break;
        case PIPE_ITEM_TYPE_LOCAL_CURSOR:
            red_send_local_cursor(cursor_channel, (LocalCursor *)pipe_item);
            break;
        case PIPE_ITEM_TYPE_INVAL_ONE:
            red_cursor_send_inval(cursor_channel, (CacheItem *)pipe_item);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_VERB:
            red_send_verb((RedChannel *)cursor_channel, ((VerbItem*)pipe_item)->verb);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_MIGRATE:
            red_printf("PIPE_ITEM_TYPE_MIGRATE");
            cursor_channel_send_migrate(cursor_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_SET_ACK:
            red_send_set_ack((RedChannel *)cursor_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_CURSOR_INIT:
            red_send_cursor_init(cursor_channel);
            free(pipe_item);
            break;
        case PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE:
            red_reset_cursor_cache(cursor_channel);
            red_send_verb((RedChannel *)cursor_channel, RED_CURSOR_INVAL_ALL);
            free(pipe_item);
            break;
        default:
            red_error("invalid pipe item type");
        }
        red_unref_channel((RedChannel *)cursor_channel);
    }
}

static inline void red_push(RedWorker *worker)
{
    cursor_channel_push(worker);
    display_channel_push(worker);
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
        Drawable *drawable = CONTAINEROF(item, Drawable, tree_item);
        const int max_indent = 200;
        char indent_str[max_indent + 1];
        int indent_str_len;

        for (i = 0; i < tree_data->level; i++) {
            printf("  ");
        }
        printf(item_prefix, 0);
        show_qxl_drawable(tree_data->worker, drawable->qxl_drawable, NULL);
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
    ShowTreeData show_tree_data;
    show_tree_data.worker = worker;
    show_tree_data.level = 0;
    show_tree_data.container = NULL;
    current_tree_for_each(worker, __show_tree_call, &show_tree_data);
}

static inline int channel_is_connected(RedChannel *channel)
{
    return !!channel->peer;
}

static void red_disconnect_channel(RedChannel *channel)
{
    channel_release_res(channel);
    red_pipe_clear(channel);

    channel->peer->cb_free(channel->peer);

    channel->peer = NULL;
    channel->send_data.blocked = FALSE;
    channel->send_data.size = channel->send_data.pos = 0;
    red_unref_channel(channel);
}

static void red_disconnect_display(RedChannel *channel)
{
    DisplayChannel *display_channel;

    if (!channel || !channel->peer) {
        return;
    }

    display_channel = (DisplayChannel *)channel;
    ASSERT(display_channel == channel->worker->display_channel);
#ifdef COMPRESS_STAT
    print_compress_stats(display_channel);
#endif
    channel->worker->display_channel = NULL;
    red_display_unshare_stream_buf(display_channel);
    red_release_pixmap_cache(display_channel);
    red_release_glz(display_channel);
    red_reset_palette_cache(display_channel);
    free(display_channel->send_data.stream_outbuf);
    red_display_reset_compress_buf(display_channel);
    while (display_channel->send_data.free_compress_bufs) {
        RedCompressBuf *buf = display_channel->send_data.free_compress_bufs;
        display_channel->send_data.free_compress_bufs = buf->next;
        free(buf);
    }
    free(display_channel->send_data.free_list.res);
    red_display_destroy_streams(display_channel);
    red_disconnect_channel(channel);
}

static void red_migrate_display(RedWorker *worker)
{
    if (worker->display_channel) {
        red_pipe_add_verb(&worker->display_channel->base, RED_DISPLAY_STREAM_DESTROY_ALL);
        red_pipe_add_type(&worker->display_channel->base, PIPE_ITEM_TYPE_MIGRATE);
    }
}

static void destroy_cairo_canvas(CairoCanvas *canvas)
{
    cairo_t *cairo;

    if (!canvas) {
        return;
    }

    cairo = canvas_get_cairo(canvas);
    canvas_destroy(canvas);
    cairo_destroy(cairo);
}

static void validate_area_nop(void *canvas, const DrawArea *draw_area, const Rect *area)
{
}

static void init_cairo_draw_context(RedWorker *worker, CairoCanvas *canvas)
{
    worker->draw_context.canvas = canvas;
    worker->draw_context.top_down = TRUE;
    worker->draw_context.set_access_params = (set_access_params_t)canvas_set_access_params;
    worker->draw_context.draw_fill = (draw_fill_t)canvas_draw_fill;
    worker->draw_context.draw_copy = (draw_copy_t)canvas_draw_copy;
    worker->draw_context.draw_opaque = (draw_opaque_t)canvas_draw_opaque;
    worker->draw_context.copy_bits = (copy_bits_t)canvas_copy_bits;
    worker->draw_context.draw_text = (draw_text_t)canvas_draw_text;
    worker->draw_context.draw_stroke = (draw_stroke_t)canvas_draw_stroke;
    worker->draw_context.draw_rop3 = (draw_rop3_t)canvas_draw_rop3;
    worker->draw_context.draw_blend = (draw_blend_t)canvas_draw_blend;
    worker->draw_context.draw_blackness = (draw_blackness_t)canvas_draw_blackness;
    worker->draw_context.draw_whiteness = (draw_whiteness_t)canvas_draw_whiteness;
    worker->draw_context.draw_invers = (draw_invers_t)canvas_draw_invers;
    worker->draw_context.draw_transparent = (draw_transparent_t)canvas_draw_transparent;
    worker->draw_context.draw_alpha_blend = (draw_alpha_blend_t)canvas_draw_alpha_blend;
    worker->draw_context.read_pixels = (read_pixels_t)canvas_read_bits;

    worker->draw_context.set_top_mask = (set_top_mask_t)canvas_group_start;
    worker->draw_context.clear_top_mask = (clear_top_mask_t)canvas_group_end;
    worker->draw_context.validate_area = validate_area_nop;
    worker->draw_context.destroy = (destroy_t)destroy_cairo_canvas;
}

static int create_cairo_context(RedWorker *worker)
{
    cairo_surface_t *cairo_surface;
    CairoCanvas *canvas;
    cairo_t *cairo;

    cairo_surface = cairo_image_surface_create_for_data(worker->dev_info.draw_area.line_0,
                                                        CAIRO_FORMAT_RGB24,
                                                        worker->dev_info.draw_area.width,
                                                        worker->dev_info.draw_area.heigth,
                                                        worker->dev_info.draw_area.stride);

    if (cairo_surface_status(cairo_surface) != CAIRO_STATUS_SUCCESS) {
        red_error("create cairo surface failed, %s",
                  cairo_status_to_string(cairo_surface_status(cairo_surface)));
    }
    cairo = cairo_create(cairo_surface);
    cairo_surface_destroy(cairo_surface);
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        red_error("create cairo failed, %s",
                  cairo_status_to_string(cairo_status(cairo)));
    }

    if (!(canvas = canvas_create(cairo, worker->dev_info.bits, &worker->image_cache,
                                 image_cache_put, image_cache_get))) {
        red_error("create cairo canvas failed");
    }
    init_cairo_draw_context(worker, canvas);
    red_printf("using cairo canvas");
    return TRUE;
}

static void destroy_gl_canvas(GLCanvas *canvas)
{
    OGLCtx *ctx;

    if (!canvas) {
        return;
    }

    ctx = gl_canvas_get_usr_data(canvas);
    ASSERT(ctx);
    gl_canvas_destroy(canvas, 0);
    oglctx_destroy(ctx);
}

static void gl_validate_area(GLCanvas *canvas, DrawArea *draw_area, const Rect *area)
{
    int h;

    if (!(h = area->bottom - area->top)) {
        return;
    }

    ASSERT(draw_area->stride < 0);
    uint8_t *dest = draw_area->line_0 + (area->top * draw_area->stride) +
                    area->left * sizeof(uint32_t);
    dest += (h - 1) * draw_area->stride;
    gl_canvas_read_pixels(canvas, dest, -draw_area->stride, area);
}

static void init_ogl_draw_context(RedWorker *worker, GLCanvas *canvas)
{
    worker->draw_context.canvas = canvas;
    worker->draw_context.top_down = FALSE;
    worker->draw_context.set_access_params = (set_access_params_t)gl_canvas_set_access_params;
    worker->draw_context.draw_fill = (draw_fill_t)gl_canvas_draw_fill;
    worker->draw_context.draw_copy = (draw_copy_t)gl_canvas_draw_copy;
    worker->draw_context.draw_opaque = (draw_opaque_t)gl_canvas_draw_opaque;
    worker->draw_context.copy_bits = (copy_bits_t)gl_canvas_copy_pixels;
    worker->draw_context.draw_text = (draw_text_t)gl_canvas_draw_text;
    worker->draw_context.draw_stroke = (draw_stroke_t)gl_canvas_draw_stroke;
    worker->draw_context.draw_rop3 = (draw_rop3_t)gl_canvas_draw_rop3;
    worker->draw_context.draw_blend = (draw_blend_t)gl_canvas_draw_blend;
    worker->draw_context.draw_blackness = (draw_blackness_t)gl_canvas_draw_blackness;
    worker->draw_context.draw_whiteness = (draw_whiteness_t)gl_canvas_draw_whiteness;
    worker->draw_context.draw_invers = (draw_invers_t)gl_canvas_draw_invers;
    worker->draw_context.draw_transparent = (draw_transparent_t)gl_canvas_draw_transparent;
    worker->draw_context.draw_alpha_blend = (draw_alpha_blend_t)gl_canvas_draw_alpha_blend;
    worker->draw_context.read_pixels = (read_pixels_t)gl_canvas_read_pixels;

    worker->draw_context.set_top_mask = (set_top_mask_t)gl_canvas_set_top_mask;
    worker->draw_context.clear_top_mask = (clear_top_mask_t)gl_canvas_clear_top_mask;
    worker->draw_context.validate_area = (validate_area_t)gl_validate_area;
    worker->draw_context.destroy = (destroy_t)destroy_gl_canvas;
}

static int create_ogl_context_common(RedWorker *worker, OGLCtx *ctx)
{
    GLCanvas *canvas;
    int width;
    int height;

    width = worker->dev_info.x_res;
    height = worker->dev_info.y_res;

    oglctx_make_current(ctx);
    if (!(canvas = gl_canvas_create(ctx, width, height, worker->dev_info.bits, &worker->image_cache,
                                    image_cache_put, image_cache_get))) {
        return FALSE;
    }

    init_ogl_draw_context(worker, canvas);
    gl_canvas_clear(canvas);
    red_printf("using ogl %s canvas", oglctx_type_str(ctx));
    return TRUE;
}

static int create_ogl_pbuf_context(RedWorker *worker)
{
    OGLCtx *ctx;

    if (!(ctx = pbuf_create(worker->dev_info.x_res, worker->dev_info.y_res))) {
        return FALSE;
    }

    if (!create_ogl_context_common(worker, ctx)) {
        oglctx_destroy(ctx);
        return FALSE;
    }

    return TRUE;
}

static int create_ogl_pixmap_context(RedWorker *worker)
{
    OGLCtx *ctx;

    if (!(ctx = pixmap_create(worker->dev_info.x_res, worker->dev_info.y_res))) {
        return FALSE;
    }

    if (!create_ogl_context_common(worker, ctx)) {
        oglctx_destroy(ctx);
        return FALSE;
    }

    return TRUE;
}

static void red_create_draw_context(RedWorker *worker)
{
    int i;

    worker->draw_context.destroy(worker->draw_context.canvas);
    worker->draw_context.canvas = NULL;
    for (i = 0; i < worker->num_renderers; i++) {
        switch (worker->renderers[i]) {
        case RED_RENDERER_CAIRO:
            if (create_cairo_context(worker)) {
                return;
            }
            break;
        case RED_RENDERER_OGL_PBUF:
            if (create_ogl_pbuf_context(worker)) {
                return;
            }
            break;
        case RED_RENDERER_OGL_PIXMAP:
            if (create_ogl_pixmap_context(worker)) {
                return;
            }
            break;
        default:
            red_error("invalid renderer type");
        }
    }
    red_error("create renderer failed");
    memset(worker->dev_info.draw_area.buf, 0, worker->dev_info.draw_area.size);
}

static void push_new_mode(RedWorker *worker)
{
    DisplayChannel *display_channel;

    if ((display_channel = worker->display_channel)) {
        red_pipe_add_type(&display_channel->base, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
        if (!display_channel->base.migrate) {
            red_pipe_add_type(&display_channel->base, PIPE_ITEM_TYPE_MODE);
        }
        display_channel_push(worker);
    }
}

static int display_channel_wait_for_init(DisplayChannel *display_channel)
{
    display_channel->expect_init = TRUE;
    uint64_t end_time = red_now() + DISPLAY_CLIENT_TIMEOUT;
    for (;;) {
        red_receive((RedChannel *)display_channel);
        if (!display_channel->base.peer) {
            break;
        }
        if (display_channel->pixmap_cache && display_channel->glz_dict) {
            display_channel->pixmap_cache_generation = display_channel->pixmap_cache->generation;
            display_channel->glz = glz_encoder_create(display_channel->base.id,
                                                      display_channel->glz_dict->dict,
                                                      &display_channel->glz_data.usr);
            if (!display_channel->glz) {
                PANIC("create global lz failed");
            }
            return TRUE;
        }
        if (red_now() > end_time) {
            red_printf("timeout");
            red_disconnect_display((RedChannel *)display_channel);
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    return FALSE;
}

static void on_new_display_channel(RedWorker *worker)
{
    DisplayChannel *display_channel = worker->display_channel;
    ASSERT(display_channel);

    red_pipe_add_type(&display_channel->base, PIPE_ITEM_TYPE_SET_ACK);

    if (display_channel->base.migrate) {
        display_channel->expect_migrate_data = TRUE;
        return;
    }

    if (!display_channel_wait_for_init(display_channel)) {
        return;
    }
    display_channel->base.messages_window = 0;
    if (worker->attached) {
        red_current_flush(worker);
        push_new_mode(worker);
        red_add_screen_image(worker);
        if (channel_is_connected(&display_channel->base)) {
            red_pipe_add_verb(&display_channel->base, RED_DISPLAY_MARK);
            red_disply_start_streams(display_channel);
        }
    }
}

static int channel_handle_message(RedChannel *channel, RedDataHeader *message)
{
    switch (message->type) {
    case REDC_ACK_SYNC:
        if (message->size != sizeof(uint32_t)) {
            red_printf("bad message size");
            return FALSE;
        }
        channel->client_ack_generation = *(uint32_t *)(message + 1);
        break;
    case REDC_ACK:
        if (channel->client_ack_generation == channel->ack_generation) {
            channel->messages_window -= channel->client_ack_window;
        }
        break;
    case REDC_DISCONNECTING:
        break;
    default:
        red_printf("invalid message type %u", message->type);
        return FALSE;
    }
    return TRUE;
}

static GlzSharedDictionary *_red_find_glz_dictionary(uint8_t dict_id)
{
    RingItem *now;
    GlzSharedDictionary *ret = NULL;

    now = &glz_dictionary_list;

    while ((now = ring_next(&glz_dictionary_list, now))) {
        if (((GlzSharedDictionary *)now)->id == dict_id) {
            ret = (GlzSharedDictionary *)now;
            break;
        }
    }

    return ret;
}

static GlzSharedDictionary *_red_create_glz_dictionary(DisplayChannel *display,
                                                       uint8_t id,
                                                       GlzEncDictContext *opaque_dict)
{
    GlzSharedDictionary *shared_dict = malloc(sizeof(*shared_dict));
    memset(shared_dict, 0, sizeof(*shared_dict)); // nullify the ring base

    if (!shared_dict) {
        return NULL;
    }
    shared_dict->dict = opaque_dict;
    shared_dict->id = id;
    shared_dict->refs = 1;
    shared_dict->migrate_freeze = FALSE;
    ring_item_init(&shared_dict->base);
    pthread_rwlock_init(&shared_dict->encode_lock, NULL);
    return shared_dict;
}

static GlzSharedDictionary *red_create_glz_dictionary(DisplayChannel *display,
                                                      uint8_t id, int window_size)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_create(window_size,
                                                            MAX_LZ_ENCODERS,
                                                            &display->glz_data.usr);
#ifdef COMPRESS_DEBUG
    red_printf("Lz Window %d Size=%d", id, window_size);
#endif
    if (!glz_dict) {
        PANIC("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(display, id, glz_dict);
}

static GlzSharedDictionary *red_create_restored_glz_dictionary(DisplayChannel *display,
                                                               uint8_t id,
                                                               GlzEncDictRestoreData *restore_data)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_restore(restore_data,
                                                             &display->glz_data.usr);
    if (!glz_dict) {
        PANIC("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(display, id, glz_dict);
}

static GlzSharedDictionary *red_get_glz_dictionary(DisplayChannel *display,
                                                   uint8_t id, int window_size)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(id);

    if (!shared_dict) {
        shared_dict = red_create_glz_dictionary(display, id, window_size);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static GlzSharedDictionary *red_restore_glz_dictionary(DisplayChannel *display,
                                                       uint8_t id,
                                                       GlzEncDictRestoreData *restore_data)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(id);

    if (!shared_dict) {
        shared_dict = red_create_restored_glz_dictionary(display, id, restore_data);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static void red_freeze_glz(DisplayChannel *channel)
{
    pthread_rwlock_wrlock(&channel->glz_dict->encode_lock);
    if (!channel->glz_dict->migrate_freeze) {
        channel->glz_dict->migrate_freeze = TRUE;
    }
    pthread_rwlock_unlock(&channel->glz_dict->encode_lock);
}

/* destroy encoder, and dictionary if no one uses it*/
static void red_release_glz(DisplayChannel *channel)
{
    GlzSharedDictionary *shared_dict;

    red_display_clear_glz_drawables(channel);

    glz_encoder_destroy(channel->glz);
    channel->glz = NULL;

    if (!(shared_dict = channel->glz_dict)) {
        return;
    }

    channel->glz_dict = NULL;
    pthread_mutex_lock(&cache_lock);
    if (--shared_dict->refs) {
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    ring_remove(&shared_dict->base);
    pthread_mutex_unlock(&cache_lock);
    glz_enc_dictionary_destroy(shared_dict->dict, &channel->glz_data.usr);
    free(shared_dict);
}

static PixmapCache *red_create_pixmap_cache(uint8_t id, int64_t size)
{
    PixmapCache *cache = malloc(sizeof(*cache));
    if (!cache) {
        return NULL;
    }
    memset(cache, 0, sizeof(*cache));
    ring_item_init(&cache->base);
    pthread_mutex_init(&cache->lock, NULL);
    cache->id = id;
    cache->refs = 1;
    ring_init(&cache->lru);
    cache->available = size;
    cache->size = size;
    return cache;
}

static PixmapCache *red_get_pixmap_cache(uint8_t id, int64_t size)
{
    PixmapCache *cache = NULL;
    RingItem *now;
    pthread_mutex_lock(&cache_lock);

    now = &pixmap_cache_list;
    while ((now = ring_next(&pixmap_cache_list, now))) {
        if (((PixmapCache *)now)->id == id) {
            cache = (PixmapCache *)now;
            cache->refs++;
            break;
        }
    }
    if (!cache) {
        cache = red_create_pixmap_cache(id, size);
        ring_add(&pixmap_cache_list, &cache->base);
    }
    pthread_mutex_unlock(&cache_lock);
    return cache;
}

static void red_release_pixmap_cache(DisplayChannel *channel)
{
    PixmapCache *cache;
    if (!(cache = channel->pixmap_cache)) {
        return;
    }
    channel->pixmap_cache = NULL;
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

static int display_channel_init_cache(DisplayChannel *channel, RedcDisplayInit *init_info)
{
    ASSERT(!channel->pixmap_cache);
    return !!(channel->pixmap_cache = red_get_pixmap_cache(init_info->pixmap_cache_id,
                                                           init_info->pixmap_cache_size));
}

static int display_channel_init_glz_dictionary(DisplayChannel *channel, RedcDisplayInit *init_info)
{
    ASSERT(!channel->glz_dict);
    ring_init(&channel->glz_drawables);
    ring_init(&channel->glz_drawables_inst_to_free);
    pthread_mutex_init(&channel->glz_drawables_inst_to_free_lock, NULL);
    return !!(channel->glz_dict = red_get_glz_dictionary(channel,
                                                         init_info->glz_dictionary_id,
                                                         init_info->glz_dictionary_window_size));
}

static int display_channel_init(DisplayChannel *channel, RedcDisplayInit *init_info)
{
    return (display_channel_init_cache(channel, init_info) &&
            display_channel_init_glz_dictionary(channel, init_info));
}

static int display_channel_handle_migrate_glz_dictionary(DisplayChannel *channel,
                                                         DisplayChannelMigrateData *migrate_info)
{
    ASSERT(!channel->glz_dict);
    ring_init(&channel->glz_drawables);
    ring_init(&channel->glz_drawables_inst_to_free);
    pthread_mutex_init(&channel->glz_drawables_inst_to_free_lock, NULL);
    return !!(channel->glz_dict = red_restore_glz_dictionary(channel,
                                                             migrate_info->glz_dict_id,
                                                             &migrate_info->glz_dict_restore_data));
}

static int display_channel_handle_migrate_mark(DisplayChannel *channel)
{
    if (!channel->expect_migrate_mark) {
        red_printf("unexpected");
        return FALSE;
    }
    channel->expect_migrate_mark = FALSE;
    red_pipe_add_type((RedChannel *)channel, PIPE_ITEM_TYPE_MIGRATE_DATA);
    return TRUE;
}

static int display_channel_handle_migrate_data(DisplayChannel *channel, RedDataHeader *message)
{
    DisplayChannelMigrateData *migrate_data;
    int i;

    if (!channel->expect_migrate_data) {
        red_printf("unexpected");
        return FALSE;
    }
    channel->expect_migrate_data = FALSE;
    if (message->size < sizeof(*migrate_data)) {
        red_printf("bad message size");
        return FALSE;
    }
    migrate_data = (DisplayChannelMigrateData *)(message + 1);
    if (migrate_data->magic != DISPLAY_MIGRATE_DATA_MAGIC ||
                                            migrate_data->version != DISPLAY_MIGRATE_DATA_VERSION) {
        red_printf("invalid content");
        return FALSE;
    }
    ASSERT(channel->base.send_data.header.serial == 0);
    channel->base.send_data.header.serial = migrate_data->message_serial;
    if (!(channel->pixmap_cache = red_get_pixmap_cache(migrate_data->pixmap_cache_id, -1))) {
        return FALSE;
    }
    pthread_mutex_lock(&channel->pixmap_cache->lock);
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        channel->pixmap_cache->sync[i] = MAX(channel->pixmap_cache->sync[i],
                                             migrate_data->pixmap_cache_clients[i]);
    }
    pthread_mutex_unlock(&channel->pixmap_cache->lock);

    if (migrate_data->pixmap_cache_freezer) {
        channel->pixmap_cache->size = migrate_data->pixmap_cache_size;
        red_pipe_add_type((RedChannel *)channel, PIPE_ITEM_TYPE_PIXMAP_RESET);
    }

    if (display_channel_handle_migrate_glz_dictionary(channel, migrate_data)) {
        channel->glz = glz_encoder_create(channel->base.id,
                                          channel->glz_dict->dict, &channel->glz_data.usr);
        if (!channel->glz) {
            PANIC("create global lz failed");
        }
    } else {
        PANIC("restoring global lz dictionary failed");
    }

    channel->base.messages_window = 0;
    return TRUE;
}

static int display_channel_handle_message(RedChannel *channel, RedDataHeader *message)
{
    switch (message->type) {
    case REDC_DISPLAY_INIT:
        if (message->size != sizeof(RedcDisplayInit)) {
            red_printf("bad message size");
            return FALSE;
        }
        if (!((DisplayChannel *)channel)->expect_init) {
            red_printf("unexpected REDC_DISPLAY_INIT");
            return FALSE;
        }
        ((DisplayChannel *)channel)->expect_init = FALSE;
        return display_channel_init((DisplayChannel *)channel, (RedcDisplayInit *)(message + 1));
    case REDC_MIGRATE_FLUSH_MARK:
        return display_channel_handle_migrate_mark((DisplayChannel *)channel);
    case REDC_MIGRATE_DATA:
        return display_channel_handle_migrate_data((DisplayChannel *)channel, message);
    default:
        return channel_handle_message(channel, message);
    }
}

static void red_receive(RedChannel *channel)
{
    for (;;) {
        ssize_t n;
        n = channel->recive_data.end - channel->recive_data.now;
        ASSERT(n);
        ASSERT(channel->peer);
        if ((n = channel->peer->cb_read(channel->peer->ctx, channel->recive_data.now, n)) <= 0) {
            if (n == 0) {
                channel->disconnect(channel);
                return;
            }
            ASSERT(n == -1);
            switch (errno) {
            case EAGAIN:
                return;
            case EINTR:
                break;
            case EPIPE:
                channel->disconnect(channel);
                return;
            default:
                red_printf("%s", strerror(errno));
                channel->disconnect(channel);
                return;
            }
        } else {
            channel->recive_data.now += n;
            for (;;) {
                RedDataHeader *message = channel->recive_data.message;
                n = channel->recive_data.now - (uint8_t *)message;
                if (n < sizeof(RedDataHeader) ||
                    n < sizeof(RedDataHeader) + message->size) {
                    break;
                }
                if (!channel->handle_message(channel, message)) {
                    channel->disconnect(channel);
                    return;
                }
                channel->recive_data.message = (RedDataHeader *)((uint8_t *)message +
                                                                 sizeof(RedDataHeader) +
                                                                 message->size);
            }

            if (channel->recive_data.now == (uint8_t *)channel->recive_data.message) {
                channel->recive_data.now = channel->recive_data.buf;
                channel->recive_data.message = (RedDataHeader *)channel->recive_data.buf;
            } else if (channel->recive_data.now == channel->recive_data.end) {
                memcpy(channel->recive_data.buf, channel->recive_data.message, n);
                channel->recive_data.now = channel->recive_data.buf + n;
                channel->recive_data.message = (RedDataHeader *)channel->recive_data.buf;
            }
        }
    }
}

static RedChannel *__new_channel(RedWorker *worker, int size, RedsStreamContext *peer, int migrate,
                                 event_listener_action_proc handler,
                                 disconnect_channel_proc disconnect,
                                 hold_item_proc hold_item,
                                 release_item_proc release_item,
                                 handle_message_proc handle_message)
{
    struct epoll_event event;
    RedChannel *channel;
    int flags;
    int delay_val;

    if ((flags = fcntl(peer->socket, F_GETFL)) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error1;
    }

    if (fcntl(peer->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error1;
    }

    delay_val = IS_LOW_BANDWIDTH() ? 0 : 1;
    if (setsockopt(peer->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    ASSERT(size >= sizeof(*channel));
    if (!(channel = malloc(size))) {
        red_printf("malloc failed");
        goto error1;
    }
    memset(channel, 0, size);
    channel->id = worker->id;
    channel->listener.refs = 1;
    channel->listener.action = handler;
    channel->disconnect = disconnect;
    channel->hold_item = hold_item;
    channel->release_item = release_item;
    channel->handle_message = handle_message;
    channel->peer = peer;
    channel->worker = worker;
    channel->messages_window = ~0;  // blocks send message (maybe use send_data.blocked +
                                    // block flags)
    channel->client_ack_window = IS_LOW_BANDWIDTH() ? WIDE_CLIENT_ACK_WINDOW :
                                                      NARROW_CLIENT_ACK_WINDOW;
    channel->client_ack_generation = ~0;
    channel->recive_data.message = (RedDataHeader *)channel->recive_data.buf;
    channel->recive_data.now = channel->recive_data.buf;
    channel->recive_data.end = channel->recive_data.buf + sizeof(channel->recive_data.buf);
    ring_init(&channel->pipe);

    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    event.data.ptr = channel;
    if (epoll_ctl(worker->epoll, EPOLL_CTL_ADD, peer->socket, &event) == -1) {
        red_printf("epoll_ctl failed, %s", strerror(errno));
        goto error2;
    }

    channel->migrate = migrate;
    return channel;

error2:
    free(channel);
error1:
    peer->cb_free(peer);

    return NULL;
}

static void handle_channel_events(EventListener *in_listener, uint32_t events)
{
    RedChannel *channel = (RedChannel *)in_listener;

    if ((events & EPOLLIN)) {
        red_receive(channel);
    }

    if (channel->send_data.blocked) {
        red_send_data(channel, NULL);
    }
}

static void display_channel_hold_item(void *item)
{
    ASSERT(item);
    switch (((PipeItem *)item)->type) {
    case PIPE_ITEM_TYPE_DRAW:
    case PIPE_ITEM_TYPE_STREAM_CREATE:
        CONTAINEROF(item, Drawable, pipe_item)->refs++;
        break;
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

static void display_channel_release_item(RedChannel *channel, void *item)
{
    ASSERT(item);
    switch (((PipeItem *)item)->type) {
    case PIPE_ITEM_TYPE_DRAW:
    case PIPE_ITEM_TYPE_STREAM_CREATE:
        release_drawable(channel->worker, CONTAINEROF(item, Drawable, pipe_item));
        break;
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        red_display_release_stream_clip((DisplayChannel *)channel, (StreamClipItem *)item);
        break;
    case PIPE_ITEM_TYPE_UPGRADE:
        release_upgrade_item(channel->worker, (UpgradeItem *)item);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        release_image_item((ImageItem *)item);
        break;
    default:
        PANIC("invalid item type");
    }
}

static void handle_new_display_channel(RedWorker *worker, RedsStreamContext *peer, int migrate)
{
    DisplayChannel *display_channel;
    size_t stream_buf_size;

    red_disconnect_display((RedChannel *)worker->display_channel);

    if (!(display_channel = (DisplayChannel *)__new_channel(worker, sizeof(*display_channel), peer,
                                                            migrate, handle_channel_events,
                                                            red_disconnect_display,
                                                            display_channel_hold_item,
                                                            display_channel_release_item,
                                                            display_channel_handle_message))) {
        return;
    }
#ifdef RED_STATISTICS
    display_channel->stat = stat_add_node(worker->stat, "display_channel", TRUE);
    display_channel->base.out_bytes_counter = stat_add_counter(display_channel->stat,
                                                               "out_bytes", TRUE);
    display_channel->cache_hits_counter = stat_add_counter(display_channel->stat,
                                                           "cache_hits", TRUE);
    display_channel->add_to_cache_counter = stat_add_counter(display_channel->stat,
                                                             "add_to_cache", TRUE);
    display_channel->non_cache_counter = stat_add_counter(display_channel->stat,
                                                          "non_cache", TRUE);
#endif
    ring_init(&display_channel->palette_cache_lru);
    display_channel->palette_cache_available = CLIENT_PALETTE_CACHE_SIZE;
    red_display_init_streams(display_channel);

    stream_buf_size = FF_MIN_BUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE;
    if (!(display_channel->send_data.stream_outbuf = malloc(stream_buf_size))) {
        PANIC("alloc failed");
    }
    display_channel->send_data.stream_outbuf_size = FF_MIN_BUFFER_SIZE;
    red_display_share_stream_buf(display_channel);
    red_display_init_glz_data(display_channel);
    worker->display_channel = display_channel;


    if (!(display_channel->send_data.free_list.res = malloc(sizeof(RedResorceList) +
                                                            DISPLAY_FREE_LIST_DEFAULT_SIZE *
                                                            sizeof(RedResorceID)))) {
        PANIC("free list alloc failed");
    }
    display_channel->send_data.free_list.res_size = DISPLAY_FREE_LIST_DEFAULT_SIZE;
    red_ref_channel((RedChannel*)display_channel);
    on_new_display_channel(worker);
    red_unref_channel((RedChannel*)display_channel);

    stat_compress_init(&display_channel->lz_stat, lz_stat_name);
    stat_compress_init(&display_channel->glz_stat, glz_stat_name);
    stat_compress_init(&display_channel->quic_stat, quic_stat_name);
}

static void red_disconnect_cursor(RedChannel *channel)
{
    if (!channel || !channel->peer) {
        return;
    }

    ASSERT(channel == (RedChannel *)channel->worker->cursor_channel);
    channel->worker->cursor_channel = NULL;
    red_reset_cursor_cache((CursorChannel *)channel);
    red_disconnect_channel(channel);
}

static void red_migrate_cursor(RedWorker *worker)
{
    if (worker->cursor_channel) {
        red_pipe_add_type(&worker->cursor_channel->base, PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
        red_pipe_add_type(&worker->cursor_channel->base, PIPE_ITEM_TYPE_MIGRATE);
    }
}

static void on_new_cursor_channel(RedWorker *worker)
{
    CursorChannel *channel = worker->cursor_channel;

    ASSERT(channel);

    channel->base.messages_window = 0;
    red_pipe_add_type(&channel->base, PIPE_ITEM_TYPE_SET_ACK);
    if (worker->attached && !channel->base.migrate) {
        red_pipe_add_type(&worker->cursor_channel->base, PIPE_ITEM_TYPE_CURSOR_INIT);
    }
}

static void cursor_channel_hold_item(void *item)
{
    ASSERT(item);
    ((CursorItem *)item)->refs++;
}

static void cursor_channel_release_item(RedChannel *channel, void *item)
{
    ASSERT(item);
    red_release_cursor(channel->worker, item);
}

static void red_connect_cursor(RedWorker *worker, RedsStreamContext *peer, int migrate)
{
    CursorChannel *channel;

    red_disconnect_cursor((RedChannel *)worker->cursor_channel);

    if (!(channel = (CursorChannel *)__new_channel(worker, sizeof(*channel), peer, migrate,
                                                   handle_channel_events,
                                                   red_disconnect_cursor,
                                                   cursor_channel_hold_item,
                                                   cursor_channel_release_item,
                                                   channel_handle_message))) {
        return;
    }
#ifdef RED_STATISTICS
    channel->stat = stat_add_node(worker->stat, "cursor_channel", TRUE);
    channel->base.out_bytes_counter = stat_add_counter(channel->stat, "out_bytes", TRUE);
#endif
    ring_init(&channel->cursor_cache_lru);
    channel->cursor_cache_available = CLIENT_CURSOR_CACHE_SIZE;
    worker->cursor_channel = channel;
    on_new_cursor_channel(worker);
}

typedef struct __attribute__ ((__packed__)) CursorData {
    uint32_t visible;
    Point16 position;
    uint16_t trail_length;
    uint16_t trail_frequency;
    uint32_t data_size;
    RedCursor _cursor;
} CursorData;

static void red_save_cursor(RedWorker *worker)
{
    CursorData *cursor_data;
    LocalCursor *local;
    int size;

    ASSERT(worker->cursor);
    ASSERT(worker->cursor->type == CURSOR_TYPE_LOCAL);

    local = (LocalCursor *)worker->cursor;
    size = sizeof(CursorData) + sizeof(RedCursor) + local->data_size;
    cursor_data = malloc(size);
    ASSERT(cursor_data);

    cursor_data->position = worker->cursor_position;
    cursor_data->visible = worker->cursor_visible;
    cursor_data->trail_frequency = worker->cursor_trail_frequency;
    cursor_data->trail_length = worker->cursor_trail_length;
    cursor_data->data_size = local->data_size;
    cursor_data->_cursor.header = local->red_cursor.header;
    memcpy(cursor_data->_cursor.data, local->red_cursor.data, local->data_size);
    worker->qxl->set_save_data(worker->qxl, cursor_data, size);
}

static LocalCursor *_new_local_cursor(CursorHeader *header, int data_size, Point16 position)
{
    LocalCursor *local;

    local = (LocalCursor *)malloc(sizeof(LocalCursor) + data_size);
    if (!local) {
        return NULL;
    }

    red_pipe_item_init(&local->base.pipe_data, PIPE_ITEM_TYPE_LOCAL_CURSOR);
    local->base.refs = 1;
    local->base.type = CURSOR_TYPE_LOCAL;

    local->red_cursor.header = *header;
    local->red_cursor.header.unique = 0;
    local->red_cursor.flags = 0;
    local->position = position;
    local->data_size = data_size;
    return local;
}

static void red_cursor_flush(RedWorker *worker)
{
    QXLCursorCmd *cursor_cmd;
    QXLCursor *qxl_cursor;
    LocalCursor *local;
    uint32_t data_size;
    QXLDataChunk *chunk;
    uint8_t *dest;

    if (!worker->cursor || worker->cursor->type == CURSOR_TYPE_LOCAL) {
        return;
    }

    ASSERT(worker->cursor->type == CURSOR_TYPE_DEV);

    cursor_cmd = CONTAINEROF(worker->cursor, QXLCursorCmd, device_data);
    ASSERT(cursor_cmd->type == QXL_CURSOR_SET);
    qxl_cursor = (QXLCursor *)(cursor_cmd->u.set.shape + worker->dev_info.phys_delta);

    local = _new_local_cursor(&qxl_cursor->header, qxl_cursor->data_size,
                              worker->cursor_position);
    ASSERT(local);
    data_size = local->data_size;
    dest = local->red_cursor.data;
    chunk = &qxl_cursor->chunk;

    while (data_size) {
        ASSERT(chunk);
        ASSERT(chunk->data_size <= data_size);
        memcpy(dest, chunk->data, chunk->data_size);
        data_size -= chunk->data_size;
        dest += chunk->data_size;
        chunk = chunk->next_chunk ? (QXLDataChunk *)(chunk->next_chunk +
                                                     worker->dev_info.phys_delta) : NULL;
    }
    red_set_cursor(worker, &local->base);
    red_release_cursor(worker, &local->base);
}

static void red_save(RedWorker *worker)
{
    if (!worker->cursor) {
        worker->qxl->set_save_data(worker->qxl, NULL, 0);
        return;
    }
    red_save_cursor(worker);
}

static void red_cursor_load(RedWorker *worker)
{
    CursorData *cursor_data = worker->qxl->get_save_data(worker->qxl);
    LocalCursor *local;

    if (!cursor_data) {
        return;
    }

    worker->cursor_position = cursor_data->position;
    worker->cursor_visible = cursor_data->visible;
    worker->cursor_trail_frequency = cursor_data->trail_frequency;
    worker->cursor_trail_length = cursor_data->trail_length;

    local = _new_local_cursor(&cursor_data->_cursor.header, cursor_data->data_size,
                              cursor_data->position);
    ASSERT(local);
    memcpy(local->red_cursor.data, cursor_data->_cursor.data, cursor_data->data_size);
    red_set_cursor(worker, &local->base);
    red_release_cursor(worker, &local->base);
}

static void red_wait_outgoiong_item(RedChannel *channel)
{
    uint64_t end_time;
    int blocked;

    if (!channel || !channel->send_data.blocked) {
        return;
    }
    red_ref_channel(channel);

    end_time = red_now() + DETACH_TIMEOUT;
    red_printf("blocked");

    do {
        usleep(DETACH_SLEEP_DURATION);
        red_receive(channel);
        red_send_data(channel, NULL);
    } while ((blocked = channel->send_data.blocked) && red_now() < end_time);

    if (blocked) {
        red_printf("timeout");
        channel->disconnect(channel);
    }
    red_unref_channel(channel);
}

static inline void handle_dev_update(RedWorker *worker)
{
    RedWorkeMessage message;

    ASSERT(worker->attached && worker->running);
    for (;;) {
        uint64_t end_time;

        while (red_process_commands(worker, MAX_PIPE_SIZE)) {
            display_channel_push(worker);
        }

        if (!worker->qxl->has_command(worker->qxl)) {
            break;
        }
        end_time = red_now() + DISPLAY_CLIENT_TIMEOUT;
        int sleep_count = 0;
        for (;;) {
            display_channel_push(worker);
            if (!worker->display_channel ||
                                         worker->display_channel->base.pipe_size <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->display_channel;
            red_ref_channel(channel);
            red_receive(channel);
            red_send_data(channel, NULL);
            if (red_now() >= end_time) {
                red_printf("update timout");
                red_disconnect_display((RedChannel *)worker->display_channel);
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
            red_unref_channel(channel);
        }
    }
    red_update_area(worker, worker->qxl->get_update_area(worker->qxl));
    message = RED_WORKER_MESSAGE_READY;
    write_message(worker->channel, &message);
}

static void handle_dev_input(EventListener *listener, uint32_t events)
{
    RedWorker *worker = CONTAINEROF(listener, RedWorker, dev_listener);
    RedWorkeMessage message;

    read_message(worker->channel, &message);

    switch (message) {
    case RED_WORKER_MESSAGE_UPDATE:
        handle_dev_update(worker);
        break;
    case RED_WORKER_MESSAGE_WAKEUP:
        clear_bit(RED_WORKER_PENDING_WAKEUP, worker->pending);
        stat_inc_counter(worker->wakeup_counter, 1);
        break;
    case RED_WORKER_MESSAGE_OOM:
        ASSERT(worker->attached && worker->running);
        while (red_process_commands(worker, MAX_PIPE_SIZE)) {
            display_channel_push(worker);
        }
        if (worker->qxl->flush_resources(worker->qxl) == 0) {
            red_printf("oom current %u pipe %u", worker->current_size, worker->display_channel ?
                       worker->display_channel->base.pipe_size : 0);
            red_free_some(worker);
        }
        clear_bit(RED_WORKER_PENDING_OOM, worker->pending);
        break;
    case RED_WORKER_MESSAGE_ATTACH:
        ASSERT(!worker->attached);
        red_printf("attach");
        receive_data(worker->channel, &worker->dev_info, sizeof(QXLDevInfo));
        worker->attached = TRUE;
        worker->repoll_cmd_ring = worker->repoll_cursor_ring = 0;
        red_create_draw_context(worker);
        ASSERT(ring_is_empty(&worker->current));
        if (worker->display_channel && !worker->display_channel->base.migrate) {
            red_pipe_add_type(&worker->display_channel->base, PIPE_ITEM_TYPE_MODE);
            red_pipe_add_verb(&worker->display_channel->base, RED_DISPLAY_MARK);
            display_channel_push(worker);
        }
        if (worker->cursor_channel && !worker->cursor_channel->base.migrate) {
            red_pipe_add_type(&worker->cursor_channel->base, PIPE_ITEM_TYPE_CURSOR_INIT);
        }
        message = RED_WORKER_MESSAGE_READY;
        write_message(worker->channel, &message);
        break;
    case RED_WORKER_MESSAGE_DETACH:
        red_printf("detach");
        red_display_clear_glz_drawables(worker->display_channel);
        red_current_clear(worker);
#ifdef STREAM_TRACE
        red_reset_stream_trace(worker);
#endif
        if (worker->cursor) {
            red_release_cursor(worker, worker->cursor);
            worker->cursor = NULL;
        }

        red_wait_outgoiong_item((RedChannel *)worker->cursor_channel);
        if (worker->cursor_channel) {
            red_pipe_add_type(&worker->cursor_channel->base, PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
            if (!worker->cursor_channel->base.migrate) {
                red_pipe_add_verb(&worker->cursor_channel->base, RED_CURSOR_RESET);
            }
            ASSERT(!worker->cursor_channel->base.send_data.item);
        }

        red_wait_outgoiong_item((RedChannel *)worker->display_channel);
        ASSERT(ring_is_empty(&worker->streams));
        if (worker->display_channel) {
            red_pipe_add_type(&worker->display_channel->base, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
            red_pipe_add_verb(&worker->display_channel->base, RED_DISPLAY_STREAM_DESTROY_ALL);
            if (!worker->display_channel->base.migrate) {
                red_pipe_add_verb(&worker->display_channel->base, RED_DISPLAY_RESET);
            }
            ASSERT(!worker->display_channel->base.send_data.item);
        }

        worker->attached = FALSE;
        worker->cursor_visible = TRUE;
        worker->cursor_position.x = worker->cursor_position.y = 0;
        worker->cursor_trail_length = worker->cursor_trail_frequency = 0;

        image_cache_reset(&worker->image_cache);
        message = RED_WORKER_MESSAGE_READY;
        write_message(worker->channel, &message);
        break;
    case RED_WORKER_MESSAGE_DISPLAY_CONNECT: {
        RedsStreamContext *peer;
        int migrate;
        red_printf("connect");

        receive_data(worker->channel, &peer, sizeof(RedsStreamContext *));
        receive_data(worker->channel, &migrate, sizeof(int));
        handle_new_display_channel(worker, peer, migrate);
        break;
    }
    case RED_WORKER_MESSAGE_DISPLAY_DISCONNECT:
        red_printf("disconnect");
        red_disconnect_display((RedChannel *)worker->display_channel);
        break;
    case RED_WORKER_MESSAGE_SAVE:
        red_printf("save");
        ASSERT(!worker->running);
        red_save(worker);
        message = RED_WORKER_MESSAGE_READY;
        write_message(worker->channel, &message);
        break;
    case RED_WORKER_MESSAGE_LOAD:
        red_printf("load");
        ASSERT(!worker->running);
        red_add_screen_image(worker);
        red_cursor_load(worker);
        message = RED_WORKER_MESSAGE_READY;
        write_message(worker->channel, &message);
        break;
    case RED_WORKER_MESSAGE_STOP: {
        red_printf("stop");
        ASSERT(worker->running);
        worker->running = FALSE;
        red_display_clear_glz_drawables(worker->display_channel);
        red_current_flush(worker);
        red_cursor_flush(worker);
        red_wait_outgoiong_item((RedChannel *)worker->display_channel);
        red_wait_outgoiong_item((RedChannel *)worker->cursor_channel);
        message = RED_WORKER_MESSAGE_READY;
        write_message(worker->channel, &message);
        break;
    }
    case RED_WORKER_MESSAGE_START:
        red_printf("start");
        ASSERT(!worker->running);
        if (worker->cursor_channel) {
            worker->cursor_channel->base.migrate = FALSE;
        }
        if (worker->display_channel) {
            worker->display_channel->base.migrate = FALSE;
        }
        worker->running = TRUE;
        break;
    case RED_WORKER_MESSAGE_DISPLAY_MIGRATE:
        red_printf("migrate");
        red_migrate_display(worker);
        break;
    case RED_WORKER_MESSAGE_CURSOR_CONNECT: {
        RedsStreamContext *peer;
        int migrate;

        red_printf("cursor connect");
        receive_data(worker->channel, &peer, sizeof(RedsStreamContext *));
        receive_data(worker->channel, &migrate, sizeof(int));
        red_connect_cursor(worker, peer, migrate);
        break;
    }
    case RED_WORKER_MESSAGE_CURSOR_DISCONNECT:
        red_printf("cursor disconnect");
        red_disconnect_cursor((RedChannel *)worker->cursor_channel);
        break;
    case RED_WORKER_MESSAGE_CURSOR_MIGRATE:
        red_printf("cursor migrate");
        red_migrate_cursor(worker);
        break;
    case RED_WORKER_MESSAGE_SET_COMPRESSION:
        receive_data(worker->channel, &worker->image_compression, sizeof(image_compression_t));
        switch (worker->image_compression) {
        case IMAGE_COMPRESS_AUTO_LZ:
            red_printf("ic auto_lz");
            break;
        case IMAGE_COMPRESS_AUTO_GLZ:
            red_printf("ic auto_glz");
            break;
        case IMAGE_COMPRESS_QUIC:
            red_printf("ic quic");
            break;
        case IMAGE_COMPRESS_LZ:
            red_printf("ic lz");
            break;
        case IMAGE_COMPRESS_GLZ:
            red_printf("ic glz");
            break;
        case IMAGE_COMPRESS_OFF:
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
        }
#endif
        break;
    case RED_WORKER_MESSAGE_SET_STREAMING_VIDEO:
        receive_data(worker->channel, &worker->streaming_video, sizeof(uint32_t));
        red_printf("sv %u", worker->streaming_video);
        break;
    case RED_WORKER_MESSAGE_SET_MOUSE_MODE:
        receive_data(worker->channel, &worker->mouse_mode, sizeof(uint32_t));
        red_printf("mouse mode %u", worker->mouse_mode);
        break;
    default:
        red_error("message error");
    }
}

static void default_draw_context_destroy(void *canvas)
{
}

static void red_init(RedWorker *worker, WorkerInitData *init_data)
{
    struct epoll_event event;
    RedWorkeMessage message;
    int epoll;

    ASSERT(sizeof(CursorItem) <= QXL_CURSUR_DEVICE_DATA_SIZE);

    memset(worker, 0, sizeof(RedWorker));
    worker->qxl = init_data->qxl_interface;
    worker->id = init_data->id;
    worker->channel = init_data->channel;
    worker->pending = init_data->pending;
    worker->dev_listener.refs = 1;
    worker->dev_listener.action = handle_dev_input;
    worker->cursor_visible = TRUE;
    worker->draw_context.destroy = default_draw_context_destroy;
    ASSERT(init_data->num_renderers > 0);
    worker->num_renderers = init_data->num_renderers;
    memcpy(worker->renderers, init_data->renderers, sizeof(worker->renderers));
    worker->mouse_mode = RED_MOUSE_MODE_SERVER;
    worker->image_compression = init_data->image_compression;
    worker->streaming_video = init_data->streaming_video;
    ring_init(&worker->current_list);
    ring_init(&worker->current);
    image_cache_init(&worker->image_cache);
    drawables_init(worker);
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

    message = RED_WORKER_MESSAGE_READY;
    write_message(worker->channel, &message);
}

void *red_worker_main(void *arg)
{
    RedWorker worker;

    red_printf("begin");
    ASSERT(MAX_PIPE_SIZE > WIDE_CLIENT_ACK_WINDOW &&
           MAX_PIPE_SIZE > NARROW_CLIENT_ACK_WINDOW); //ensure wakeup by ack message
    ASSERT(QXL_BITMAP_TOP_DOWN == BITMAP_TOP_DOWN);

#if  defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    if (pthread_getcpuclockid(pthread_self(), &clock_id)) {
        red_error("pthread_getcpuclockid failed");
    }
#endif

    avcodec_init();
    avcodec_register_all();
    red_init(&worker, (WorkerInitData *)arg);
    red_init_quic(&worker);
    red_init_lz(&worker);
    worker.epoll_timeout = INF_EPOLL_WAIT;
    for (;;) {
        struct epoll_event events[MAX_EPOLL_SOURCES];
        int num_events;
        struct epoll_event *event;
        struct epoll_event *end;

        worker.epoll_timeout = MIN(red_get_streams_timout(&worker), worker.epoll_timeout);
        num_events = epoll_wait(worker.epoll, events, MAX_EPOLL_SOURCES, worker.epoll_timeout);
        red_handle_streams_timout(&worker);
        if (worker.display_channel && worker.display_channel->glz_dict) {
            /* during migration, in the dest, the display channel can be initialized
               while the global lz data not since migrate data msg hasn't been
               recieved yet */
            red_display_handle_glz_drawables_to_free(worker.display_channel);
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
            free(evt_listener);
        }

        if (worker.attached && worker.running) {
            red_process_cursor(&worker, MAX_PIPE_SIZE);
            red_process_commands(&worker, MAX_PIPE_SIZE);
        }
        red_push(&worker);
    }
    red_printf("exit");
    return 0;
}

#ifdef DUMP_BITMAP
#include <stdio.h>
static void dump_palette(FILE *f, Palette* plt)
{
    int i;
    for (i = 0; i < plt->num_ents; i++) {
        fwrite(plt->ents + i, sizeof(uint32_t), 1, f);
    }
}

static void dump_line(FILE *f, uint8_t* line, uint16_t n_pixel_bits, int width, int row_size)
{
    int i;
    int copy_bytes_size = ALIGN(n_pixel_bits * width, 8) / 8;

    fwrite(line, 1, copy_bytes_size, f);
    if (row_size > copy_bytes_size) {
        // each line should be 4 bytes aligned
        for (i = copy_bytes_size; i < row_size; i++) {
            fprintf(f, "%c", 0);
        }
    }
}

#define RAM_PATH "/tmp/tmpfs"

static void dump_bitmap(RedWorker *worker, Bitmap *bitmap)
{
    static uint32_t file_id = 0;

    char file_str[200];
    int rgb = TRUE;
    uint16_t n_pixel_bits;
    Palette *plt = NULL;
    uint32_t id;
    int row_size;
    uint32_t file_size;
    long address_delta = worker->dev_info.phys_delta;
    int alpha = 0;
    uint32_t header_size = 14 + 40;
    uint32_t bitmap_data_offset;
    uint32_t tmp_u32;
    int32_t tmp_32;
    uint16_t tmp_u16;
    FILE *f;

    switch (bitmap->format) {
    case BITMAP_FMT_1BIT_BE:
    case BITMAP_FMT_1BIT_LE:
        rgb = FALSE;
        n_pixel_bits = 1;
        break;
    case BITMAP_FMT_4BIT_BE:
    case BITMAP_FMT_4BIT_LE:
        rgb = FALSE;
        n_pixel_bits = 4;
        break;
    case BITMAP_FMT_8BIT:
        rgb = FALSE;
        n_pixel_bits = 8;
        break;
    case BITMAP_FMT_16BIT:
        n_pixel_bits = 16;
        break;
    case BITMAP_FMT_24BIT:
        n_pixel_bits = 24;
        break;
    case BITMAP_FMT_32BIT:
        n_pixel_bits = 32;
        break;
    case BITMAP_FMT_RGBA:
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
        plt = (Palette *)(bitmap->palette + address_delta);
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
    if ((bitmap->flags & QXL_BITMAP_DIRECT)) {
        uint8_t *lines = (uint8_t*)(bitmap->data + address_delta);
        int i;
        for (i = 0; i < bitmap->y; i++) {
            dump_line(f, lines + (i * bitmap->stride), n_pixel_bits, bitmap->x, row_size);
        }
    } else {
        QXLDataChunk *chunk = NULL;
        int num_lines;
        ADDRESS relative_address = bitmap->data;

        while (relative_address) {
            int i;
            chunk = (QXLDataChunk *)(relative_address + address_delta);
            num_lines = chunk->data_size / bitmap->stride;
            for (i = 0; i < num_lines; i++) {
                dump_line(f, chunk->data + (i * bitmap->stride), n_pixel_bits, bitmap->x, row_size);
            }
            relative_address = chunk->next_chunk;
        }
    }
    fclose(f);
}

#endif


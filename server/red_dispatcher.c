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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <signal.h>

#include <spice/qxl_dev.h>
#include "spice.h"
#include "red_worker.h"
#include "quic.h"
#include "sw_canvas.h"
#ifdef USE_OGL
#include "gl_canvas.h"
#endif // USE_OGL
#include "reds.h"
#include "red_dispatcher.h"
#include "red_parse_qxl.h"

static int num_active_workers = 0;

//volatile

typedef struct RedDispatcher RedDispatcher;
struct RedDispatcher {
    QXLWorker base;
    QXLInstance *qxl;
    int channel;
    pthread_t worker_thread;
    uint32_t pending;
    int primary_active;
    int x_res;
    int y_res;
    int use_hardware_cursor;
    RedDispatcher *next;
};

typedef struct RedWorkeState {
    uint8_t *io_base;
    unsigned long phys_delta;

    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
    uint32_t stride;
} RedWorkeState;

extern uint32_t streaming_video;
extern spice_image_compression_t image_compression;
extern spice_wan_compression_t jpeg_state;
extern spice_wan_compression_t zlib_glz_state;

static RedDispatcher *dispatchers = NULL;

static void red_dispatcher_set_peer(Channel *channel, RedsStreamContext *peer, int migration,
                                    int num_common_caps, uint32_t *common_caps, int num_caps,
                                    uint32_t *caps)
{
    RedDispatcher *dispatcher;

    red_printf("");
    dispatcher = (RedDispatcher *)channel->data;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DISPLAY_CONNECT;
    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &peer, sizeof(RedsStreamContext *));
    send_data(dispatcher->channel, &migration, sizeof(int));
}

static void red_dispatcher_shutdown_peer(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("");
    RedWorkerMessage message = RED_WORKER_MESSAGE_DISPLAY_DISCONNECT;
    write_message(dispatcher->channel, &message);
}

static void red_dispatcher_migrate(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("channel type %u id %u", channel->type, channel->id);
    RedWorkerMessage message = RED_WORKER_MESSAGE_DISPLAY_MIGRATE;
    write_message(dispatcher->channel, &message);
}

static void red_dispatcher_set_cursor_peer(Channel *channel, RedsStreamContext *peer,
                                           int migration, int num_common_caps,
                                           uint32_t *common_caps, int num_caps,
                                           uint32_t *caps)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("");
    RedWorkerMessage message = RED_WORKER_MESSAGE_CURSOR_CONNECT;
    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &peer, sizeof(RedsStreamContext *));
    send_data(dispatcher->channel, &migration, sizeof(int));
}

static void red_dispatcher_shutdown_cursor_peer(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("");
    RedWorkerMessage message = RED_WORKER_MESSAGE_CURSOR_DISCONNECT;
    write_message(dispatcher->channel, &message);
}

static void red_dispatcher_cursor_migrate(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("channel type %u id %u", channel->type, channel->id);
    RedWorkerMessage message = RED_WORKER_MESSAGE_CURSOR_MIGRATE;
    write_message(dispatcher->channel, &message);
}

typedef struct RendererInfo {
    int id;
    const char *name;
} RendererInfo;

static RendererInfo renderers_info[] = {
    {RED_RENDERER_SW, "sw"},
#ifdef USE_OGL
    {RED_RENDERER_OGL_PBUF, "oglpbuf"},
    {RED_RENDERER_OGL_PIXMAP, "oglpixmap"},
#endif
    {RED_RENDERER_INVALID, NULL},
};

static uint32_t renderers[RED_MAX_RENDERERS];
static uint32_t num_renderers = 0;

static RendererInfo *find_renderer(const char *name)
{
    RendererInfo *inf = renderers_info;
    while (inf->name) {
        if (strcmp(name, inf->name) == 0) {
            return inf;
        }
        inf++;
    }
    return NULL;
}

int red_dispatcher_add_renderer(const char *name)
{
    RendererInfo *inf;

    if (num_renderers == RED_MAX_RENDERERS || !(inf = find_renderer(name))) {
        return FALSE;
    }
    renderers[num_renderers++] = inf->id;
    return TRUE;
}

int red_dispatcher_qxl_count()
{
    return num_active_workers;
}

static void update_client_mouse_allowed()
{
    static int allowed = FALSE;
    int allow_now = FALSE;
    int x_res = 0;
    int y_res = 0;

    if (num_active_workers > 0) {
        allow_now = TRUE;
        RedDispatcher *now = dispatchers;
        while (now && allow_now) {
            if (now->primary_active) {
                allow_now = now->use_hardware_cursor;
                if (num_active_workers == 1) {
                    if (allow_now) {
                        x_res = now->x_res;
                        y_res = now->y_res;
                    }
                    break;
                }
            }
            now = now->next;
        }
    }

    if (allow_now || allow_now != allowed) {
        allowed = allow_now;
        reds_set_client_mouse_allowed(allowed, x_res, y_res);
    }
}

static void qxl_worker_update_area(QXLWorker *qxl_worker, uint32_t surface_id,
                                   QXLRect *qxl_area, QXLRect *qxl_dirty_rects,
                                   uint32_t num_dirty_rects, uint32_t clear_dirty_region)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_UPDATE;
    SpiceRect *dirty_rects = spice_new0(SpiceRect, num_dirty_rects);
    SpiceRect *area = spice_new0(SpiceRect, 1);
    int i;

    red_get_rect_ptr(area, qxl_area);

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &surface_id, sizeof(uint32_t));
    send_data(dispatcher->channel, &area, sizeof(SpiceRect *));
    send_data(dispatcher->channel, &dirty_rects, sizeof(SpiceRect *));
    send_data(dispatcher->channel, &num_dirty_rects, sizeof(uint32_t));
    send_data(dispatcher->channel, &clear_dirty_region, sizeof(uint32_t));
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);

    for (i = 0; i < num_dirty_rects; i++) {
        qxl_dirty_rects[i].top    = dirty_rects[i].top;
        qxl_dirty_rects[i].left   = dirty_rects[i].left;
        qxl_dirty_rects[i].bottom = dirty_rects[i].bottom;
        qxl_dirty_rects[i].right  = dirty_rects[i].right;
    }

    free(dirty_rects);
    free(area);
}

static void qxl_worker_add_memslot(QXLWorker *qxl_worker, QXLDevMemSlot *mem_slot)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_ADD_MEMSLOT;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, mem_slot, sizeof(QXLDevMemSlot));
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_del_memslot(QXLWorker *qxl_worker, uint32_t slot_group_id, uint32_t slot_id)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DEL_MEMSLOT;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &slot_group_id, sizeof(uint32_t));
    send_data(dispatcher->channel, &slot_id, sizeof(uint32_t));
}

static void qxl_worker_destroy_surfaces(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_SURFACES;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_destroy_primary(QXLWorker *qxl_worker, uint32_t surface_id)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &surface_id, sizeof(uint32_t));
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);

    dispatcher->x_res = 0;
    dispatcher->y_res = 0;
    dispatcher->use_hardware_cursor = FALSE;
    dispatcher->primary_active = FALSE;

    update_client_mouse_allowed();
}

static void qxl_worker_create_primary(QXLWorker *qxl_worker, uint32_t surface_id,
                                      QXLDevSurfaceCreate *surface)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE;

    dispatcher->x_res = surface->width;
    dispatcher->y_res = surface->height;
    dispatcher->use_hardware_cursor = surface->mouse_mode;
    dispatcher->primary_active = TRUE;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &surface_id, sizeof(uint32_t));
    send_data(dispatcher->channel, surface, sizeof(QXLDevSurfaceCreate));
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);

    update_client_mouse_allowed();
}

static void qxl_worker_reset_image_cache(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_RESET_IMAGE_CACHE;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_reset_cursor(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_RESET_CURSOR;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_destroy_surface_wait(QXLWorker *qxl_worker, uint32_t surface_id)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &surface_id, sizeof(uint32_t));
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_reset_memslots(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_RESET_MEMSLOTS;

    write_message(dispatcher->channel, &message);
}

static void qxl_worker_wakeup(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;

    if (!test_bit(RED_WORKER_PENDING_WAKEUP, dispatcher->pending)) {
        RedWorkerMessage message = RED_WORKER_MESSAGE_WAKEUP;
        set_bit(RED_WORKER_PENDING_WAKEUP, &dispatcher->pending);
        write_message(dispatcher->channel, &message);
    }
}

static void qxl_worker_oom(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    if (!test_bit(RED_WORKER_PENDING_OOM, dispatcher->pending)) {
        RedWorkerMessage message = RED_WORKER_MESSAGE_OOM;
        set_bit(RED_WORKER_PENDING_OOM, &dispatcher->pending);
        write_message(dispatcher->channel, &message);
    }
}

static void qxl_worker_start(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_START;

    write_message(dispatcher->channel, &message);
}

static void qxl_worker_stop(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_STOP;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

void qxl_worker_loadvm_commands(QXLWorker *qxl_worker,
                                struct QXLCommandExt *ext, uint32_t count)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkerMessage message = RED_WORKER_MESSAGE_LOADVM_COMMANDS;

    red_printf("");
    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &count, sizeof(uint32_t));
    send_data(dispatcher->channel, ext, sizeof(QXLCommandExt) * count);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

void red_dispatcher_set_mm_time(uint32_t mm_time)
{
    RedDispatcher *now = dispatchers;
    while (now) {
        now->qxl->st->qif->set_mm_time(now->qxl, mm_time);
        now = now->next;
    }
}

static inline int calc_compression_level()
{
    ASSERT(streaming_video != STREAM_VIDEO_INVALID);
    if ((streaming_video != STREAM_VIDEO_OFF) ||
        (image_compression != SPICE_IMAGE_COMPRESS_QUIC)) {
        return 0;
    } else {
        return 1;
    }
}

void red_dispatcher_on_ic_change()
{
    int compression_level = calc_compression_level();
    RedDispatcher *now = dispatchers;
    while (now) {
        RedWorkerMessage message = RED_WORKER_MESSAGE_SET_COMPRESSION;
        now->qxl->st->qif->set_compression_level(now->qxl, compression_level);
        write_message(now->channel, &message);
        send_data(now->channel, &image_compression, sizeof(spice_image_compression_t));
        now = now->next;
    }
}

void red_dispatcher_on_sv_change()
{
    int compression_level = calc_compression_level();
    RedDispatcher *now = dispatchers;
    while (now) {
        RedWorkerMessage message = RED_WORKER_MESSAGE_SET_STREAMING_VIDEO;
        now->qxl->st->qif->set_compression_level(now->qxl, compression_level);
        write_message(now->channel, &message);
        send_data(now->channel, &streaming_video, sizeof(uint32_t));
        now = now->next;
    }
}

void red_dispatcher_set_mouse_mode(uint32_t mode)
{
    RedDispatcher *now = dispatchers;
    while (now) {
        RedWorkerMessage message = RED_WORKER_MESSAGE_SET_MOUSE_MODE;
        write_message(now->channel, &message);
        send_data(now->channel, &mode, sizeof(uint32_t));
        now = now->next;
    }
}

int red_dispatcher_count()
{
    RedDispatcher *now = dispatchers;
    int ret = 0;

    while (now) {
        ret++;
        now = now->next;
    }
    return ret;
}

uint32_t red_dispatcher_qxl_ram_size()
{
    QXLDevInitInfo qxl_info;
    if (!dispatchers) {
        return 0;
    }
    dispatchers->qxl->st->qif->get_init_info(dispatchers->qxl, &qxl_info);
    return qxl_info.qxl_ram_size;
}

RedDispatcher *red_dispatcher_init(QXLInstance *qxl)
{
    RedDispatcher *dispatcher;
    int channels[2];
    RedWorkerMessage message;
    WorkerInitData init_data;
    QXLDevInitInfo init_info;
    int r;
    Channel *reds_channel;
    Channel *cursor_channel;
    sigset_t thread_sig_mask;
    sigset_t curr_sig_mask;

    quic_init();
    sw_canvas_init();
#ifdef USE_OGL
    gl_canvas_init();
#endif // USE_OGL

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, channels) == -1) {
        red_error("socketpair failed %s", strerror(errno));
    }

    dispatcher = spice_new0(RedDispatcher, 1);
    dispatcher->channel = channels[0];
    init_data.qxl = dispatcher->qxl = qxl;
    init_data.id = qxl->id;
    init_data.channel = channels[1];
    init_data.pending = &dispatcher->pending;
    init_data.num_renderers = num_renderers;
    memcpy(init_data.renderers, renderers, sizeof(init_data.renderers));

    init_data.image_compression = image_compression;
    init_data.jpeg_state = jpeg_state;
    init_data.zlib_glz_state = zlib_glz_state;
    init_data.streaming_video = streaming_video;

    dispatcher->base.major_version = SPICE_INTERFACE_QXL_MAJOR;
    dispatcher->base.minor_version = SPICE_INTERFACE_QXL_MINOR;
    dispatcher->base.wakeup = qxl_worker_wakeup;
    dispatcher->base.oom = qxl_worker_oom;
    dispatcher->base.start = qxl_worker_start;
    dispatcher->base.stop = qxl_worker_stop;
    dispatcher->base.update_area = qxl_worker_update_area;
    dispatcher->base.add_memslot = qxl_worker_add_memslot;
    dispatcher->base.del_memslot = qxl_worker_del_memslot;
    dispatcher->base.reset_memslots = qxl_worker_reset_memslots;
    dispatcher->base.destroy_surfaces = qxl_worker_destroy_surfaces;
    dispatcher->base.create_primary_surface = qxl_worker_create_primary;
    dispatcher->base.destroy_primary_surface = qxl_worker_destroy_primary;

    dispatcher->base.reset_image_cache = qxl_worker_reset_image_cache;
    dispatcher->base.reset_cursor = qxl_worker_reset_cursor;
    dispatcher->base.destroy_surface_wait = qxl_worker_destroy_surface_wait;
    dispatcher->base.loadvm_commands = qxl_worker_loadvm_commands;

    qxl->st->qif->get_init_info(qxl, &init_info);

    init_data.memslot_id_bits = init_info.memslot_id_bits;
    init_data.memslot_gen_bits = init_info.memslot_gen_bits;
    init_data.num_memslots = init_info.num_memslots;
    init_data.num_memslots_groups = init_info.num_memslots_groups;
    init_data.internal_groupslot_id = init_info.internal_groupslot_id;
    init_data.n_surfaces = init_info.n_surfaces;

    num_active_workers = 1;

    sigfillset(&thread_sig_mask);
    sigdelset(&thread_sig_mask, SIGILL);
    sigdelset(&thread_sig_mask, SIGFPE);
    sigdelset(&thread_sig_mask, SIGSEGV);
    pthread_sigmask(SIG_SETMASK, &thread_sig_mask, &curr_sig_mask);
    if ((r = pthread_create(&dispatcher->worker_thread, NULL, red_worker_main, &init_data))) {
        red_error("create thread failed %d", r);
    }
    pthread_sigmask(SIG_SETMASK, &curr_sig_mask, NULL);

    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);

    reds_channel = spice_new0(Channel, 1);
    reds_channel->type = SPICE_CHANNEL_DISPLAY;
    reds_channel->id = qxl->id;
    reds_channel->link = red_dispatcher_set_peer;
    reds_channel->shutdown = red_dispatcher_shutdown_peer;
    reds_channel->migrate = red_dispatcher_migrate;
    reds_channel->data = dispatcher;
    reds_register_channel(reds_channel);

    cursor_channel = spice_new0(Channel, 1);
    cursor_channel->type = SPICE_CHANNEL_CURSOR;
    cursor_channel->id = qxl->id;
    cursor_channel->link = red_dispatcher_set_cursor_peer;
    cursor_channel->shutdown = red_dispatcher_shutdown_cursor_peer;
    cursor_channel->migrate = red_dispatcher_cursor_migrate;
    cursor_channel->data = dispatcher;
    reds_register_channel(cursor_channel);
    qxl->st->qif->attache_worker(qxl, &dispatcher->base);
    qxl->st->qif->set_compression_level(qxl, calc_compression_level());

    dispatcher->next = dispatchers;
    dispatchers = dispatcher;
    return dispatcher;
}


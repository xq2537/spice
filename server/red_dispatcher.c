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
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <signal.h>

#include "qxl_dev.h"
#include "vd_interface.h"
#include "red_worker.h"
#include "quic.h"
#include "cairo_canvas.h"
#include "gl_canvas.h"
#include "reds.h"
#include "red_dispatcher.h"

static int num_active_workers = 0;

//volatile

typedef struct RedDispatcher RedDispatcher;
struct RedDispatcher {
    QXLWorker base;
    QXLInterface *qxl_interface;
    int channel;
    pthread_t worker_thread;
    uint32_t pending;
    int active;
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
extern image_compression_t image_compression;

static RedDispatcher *dispatchers = NULL;

static void red_dispatcher_set_peer(Channel *channel, RedsStreamContext *peer, int migration,
                                    int num_common_caps, uint32_t *common_caps, int num_caps,
                                    uint32_t *caps)
{
    RedDispatcher *dispatcher;

    red_printf("");
    dispatcher = (RedDispatcher *)channel->data;
    RedWorkeMessage message = RED_WORKER_MESSAGE_DISPLAY_CONNECT;
    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &peer, sizeof(RedsStreamContext *));
    send_data(dispatcher->channel, &migration, sizeof(int));
}

static void red_dispatcher_shutdown_peer(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("");
    RedWorkeMessage message = RED_WORKER_MESSAGE_DISPLAY_DISCONNECT;
    write_message(dispatcher->channel, &message);
}

static void red_dispatcher_migrate(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("channel type %u id %u", channel->type, channel->id);
    RedWorkeMessage message = RED_WORKER_MESSAGE_DISPLAY_MIGRATE;
    write_message(dispatcher->channel, &message);
}

static void red_dispatcher_set_cursor_peer(Channel *channel, RedsStreamContext *peer,
                                           int migration, int num_common_caps,
                                           uint32_t *common_caps, int num_caps,
                                           uint32_t *caps)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("");
    RedWorkeMessage message = RED_WORKER_MESSAGE_CURSOR_CONNECT;
    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &peer, sizeof(RedsStreamContext *));
    send_data(dispatcher->channel, &migration, sizeof(int));
}

static void red_dispatcher_shutdown_cursor_peer(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("");
    RedWorkeMessage message = RED_WORKER_MESSAGE_CURSOR_DISCONNECT;
    write_message(dispatcher->channel, &message);
}

static void red_dispatcher_cursor_migrate(Channel *channel)
{
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    red_printf("channel type %u id %u", channel->type, channel->id);
    RedWorkeMessage message = RED_WORKER_MESSAGE_CURSOR_MIGRATE;
    write_message(dispatcher->channel, &message);
}

typedef struct RendererInfo {
    int id;
    const char *name;
} RendererInfo;

static RendererInfo renderers_info[] = {
    {RED_RENDERER_CAIRO, "cairo"},
    {RED_RENDERER_OGL_PBUF, "oglpbuf"},
    {RED_RENDERER_OGL_PIXMAP, "oglpixmap"},
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
            if (now->active) {
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

static void qxl_worker_attach(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_ATTACH;
    QXLDevInfo info;

    dispatcher->qxl_interface->get_info(dispatcher->qxl_interface, &info);
    dispatcher->x_res = info.x_res;
    dispatcher->y_res = info.y_res;
    dispatcher->use_hardware_cursor = info.use_hardware_cursor;
    dispatcher->active = TRUE;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &info, sizeof(QXLDevInfo));
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);

    num_active_workers++;
    update_client_mouse_allowed();
}

static void qxl_worker_detach(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_DETACH;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);

    dispatcher->x_res = 0;
    dispatcher->y_res = 0;
    dispatcher->use_hardware_cursor = FALSE;
    dispatcher->active = FALSE;
    num_active_workers--;
    update_client_mouse_allowed();
}

static void qxl_worker_update_area(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_UPDATE;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_add_memslot(QXLWorker *qxl_worker, QXLDevMemSlot *mem_slot)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_ADD_MEMSLOT;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, mem_slot, sizeof(QXLDevMemSlot));
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_del_memslot(QXLWorker *qxl_worker, uint32_t slot_id)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_DEL_MEMSLOT;

    write_message(dispatcher->channel, &message);
    send_data(dispatcher->channel, &slot_id, sizeof(uint32_t));
}

static void qxl_worker_reset_memslots(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_RESET_MEMSLOTS;

    write_message(dispatcher->channel, &message);
}

static void qxl_worker_wakeup(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;

    if (!test_bit(RED_WORKER_PENDING_WAKEUP, dispatcher->pending)) {
        RedWorkeMessage message = RED_WORKER_MESSAGE_WAKEUP;
        set_bit(RED_WORKER_PENDING_WAKEUP, &dispatcher->pending);
        write_message(dispatcher->channel, &message);
    }
}

static void qxl_worker_oom(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    if (!test_bit(RED_WORKER_PENDING_OOM, dispatcher->pending)) {
        RedWorkeMessage message = RED_WORKER_MESSAGE_OOM;
        set_bit(RED_WORKER_PENDING_OOM, &dispatcher->pending);
        write_message(dispatcher->channel, &message);
    }
}

static void qxl_worker_save(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_SAVE;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_load(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_LOAD;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

static void qxl_worker_start(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_START;

    write_message(dispatcher->channel, &message);
}

static void qxl_worker_stop(QXLWorker *qxl_worker)
{
    RedDispatcher *dispatcher = (RedDispatcher *)qxl_worker;
    RedWorkeMessage message = RED_WORKER_MESSAGE_STOP;

    write_message(dispatcher->channel, &message);
    read_message(dispatcher->channel, &message);
    ASSERT(message == RED_WORKER_MESSAGE_READY);
}

void red_dispatcher_set_mm_time(uint32_t mm_time)
{
    RedDispatcher *now = dispatchers;
    while (now) {
        now->qxl_interface->set_mm_time(now->qxl_interface, mm_time);
        now = now->next;
    }
}

static inline int calc_compression_level()
{
    ASSERT(streaming_video != STREAM_VIDEO_INVALID);
    if ((streaming_video != STREAM_VIDEO_OFF) || (image_compression != IMAGE_COMPRESS_QUIC)) {
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
        RedWorkeMessage message = RED_WORKER_MESSAGE_SET_COMPRESSION;
        now->qxl_interface->set_compression_level(now->qxl_interface, compression_level);
        write_message(now->channel, &message);
        send_data(now->channel, &image_compression, sizeof(image_compression_t));
        now = now->next;
    }
}

void red_dispatcher_on_sv_change()
{
    int compression_level = calc_compression_level();
    RedDispatcher *now = dispatchers;
    while (now) {
        RedWorkeMessage message = RED_WORKER_MESSAGE_SET_STREAMING_VIDEO;
        now->qxl_interface->set_compression_level(now->qxl_interface, compression_level);
        write_message(now->channel, &message);
        send_data(now->channel, &streaming_video, sizeof(uint32_t));
        now = now->next;
    }
}

void red_dispatcher_set_mouse_mode(uint32_t mode)
{
    RedDispatcher *now = dispatchers;
    while (now) {
        RedWorkeMessage message = RED_WORKER_MESSAGE_SET_MOUSE_MODE;
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
    QXLDevInfo qxl_info;
    dispatchers->qxl_interface->get_info(dispatchers->qxl_interface, &qxl_info);
    return qxl_info.ram_size;
}

RedDispatcher *red_dispatcher_init(QXLInterface *qxl_interface)
{
    RedDispatcher *dispatcher;
    int channels[2];
    RedWorkeMessage message;
    WorkerInitData init_data;
    QXLDevInitInfo init_info;
    int r;
    Channel *reds_channel;
    Channel *cursor_channel;
    sigset_t thread_sig_mask;
    sigset_t curr_sig_mask;

    if (qxl_interface->pci_vendor != REDHAT_PCI_VENDOR_ID ||
        qxl_interface->pci_id != QXL_DEVICE_ID ||
        qxl_interface->pci_revision != QXL_REVISION) {
        red_printf("pci mismatch");
        return NULL;
    }

    quic_init();
    cairo_canvas_init();
    gl_canvas_init();

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, channels) == -1) {
        red_error("socketpair failed %s", strerror(errno));
    }

    if (!(dispatcher = malloc(sizeof(RedDispatcher)))) {
        red_error("malloc failed");
    }
    memset(dispatcher, 0, sizeof(RedDispatcher));
    dispatcher->channel = channels[0];
    init_data.qxl_interface = dispatcher->qxl_interface = qxl_interface;
    init_data.id = qxl_interface->base.id;
    init_data.channel = channels[1];
    init_data.pending = &dispatcher->pending;
    init_data.num_renderers = num_renderers;
    memcpy(init_data.renderers, renderers, sizeof(init_data.renderers));

    init_data.image_compression = image_compression;
    init_data.streaming_video = streaming_video;

    dispatcher->base.major_version = VD_INTERFACE_QXL_MAJOR;
    dispatcher->base.major_version = VD_INTERFACE_QXL_MINOR;
    dispatcher->base.attach = qxl_worker_attach;
    dispatcher->base.detach = qxl_worker_detach;
    dispatcher->base.wakeup = qxl_worker_wakeup;
    dispatcher->base.oom = qxl_worker_oom;
    dispatcher->base.save = qxl_worker_save;
    dispatcher->base.load = qxl_worker_load;
    dispatcher->base.start = qxl_worker_start;
    dispatcher->base.stop = qxl_worker_stop;
    dispatcher->base.update_area = qxl_worker_update_area;
    dispatcher->base.add_memslot = qxl_worker_add_memslot;
    dispatcher->base.del_memslot = qxl_worker_del_memslot;
    dispatcher->base.reset_memslots = qxl_worker_reset_memslots;

    qxl_interface->get_init_info(qxl_interface, &init_info);

    init_data.memslot_id_bits = init_info.memslot_id_bits;
    init_data.memslot_gen_bits = init_info.memslot_gen_bits;
    init_data.num_memslots = init_info.num_memslots;

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
    if (!(reds_channel = malloc(sizeof(Channel)))) {
        red_error("reds channel malloc failed");
    }
    memset(reds_channel, 0, sizeof(Channel));
    reds_channel->type = RED_CHANNEL_DISPLAY;
    reds_channel->id = qxl_interface->base.id;
    reds_channel->link = red_dispatcher_set_peer;
    reds_channel->shutdown = red_dispatcher_shutdown_peer;
    reds_channel->migrate = red_dispatcher_migrate;
    reds_channel->data = dispatcher;
    reds_register_channel(reds_channel);

    if (!(cursor_channel = malloc(sizeof(Channel)))) {
        red_error("reds channel malloc failed");
    }
    memset(cursor_channel, 0, sizeof(Channel));
    cursor_channel->type = RED_CHANNEL_CURSOR;
    cursor_channel->id = qxl_interface->base.id;
    cursor_channel->link = red_dispatcher_set_cursor_peer;
    cursor_channel->shutdown = red_dispatcher_shutdown_cursor_peer;
    cursor_channel->migrate = red_dispatcher_cursor_migrate;
    cursor_channel->data = dispatcher;
    reds_register_channel(cursor_channel);
    qxl_interface->attache_worker(qxl_interface, &dispatcher->base);
    qxl_interface->set_compression_level(qxl_interface, calc_compression_level());

    dispatcher->next = dispatchers;
    dispatchers = dispatcher;
    return dispatcher;
}


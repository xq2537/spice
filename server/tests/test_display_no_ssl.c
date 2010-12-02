#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>
#include <spice.h>
#include <spice/qxl_dev.h>
#include "red_channel.h"
#include "test_util.h"
#include "basic_event_loop.h"

/* Parts cribbed from spice-display.h/.c/qxl.c */

typedef struct SimpleSpiceUpdate {
    QXLDrawable drawable;
    QXLImage image;
    QXLCommandExt ext;
    uint8_t *bitmap;
} SimpleSpiceUpdate;

void test_spice_destroy_update(SimpleSpiceUpdate *update)
{
    if (!update) {
        return;
    }
    free(update->bitmap);
    free(update);
}

#define WIDTH 320
#define HEIGHT 320

static float angle = 0;
static int unique = 1;
static int color = 0;

SimpleSpiceUpdate *test_spice_create_update()
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLImage *image;
    QXLCommand *cmd;
    QXLRect bbox = {
        .top = HEIGHT/2 + (HEIGHT/3)*cos(angle),
        .left = WIDTH/2 + (WIDTH/3)*sin(angle),
    };
    uint8_t *dst;
    int bw, bh;
    int i;

    angle += 0.2;
    color = (color + 1) % 2;
    unique++;

    update   = calloc(sizeof(*update), 1);
    drawable = &update->drawable;
    image    = &update->image;
    cmd      = &update->ext.cmd;

    bw       = 64;
    bh       = 48;
    bbox.right = bbox.left + bw;
    bbox.bottom = bbox.top + bh;
    update->bitmap = malloc(bw * bh * 4);
    //printf("allocated %p, %p\n", update, update->bitmap);

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    drawable->release_info.id = (intptr_t)update;
    drawable->type            = QXL_DRAW_COPY;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy.rop_descriptor  = SPICE_ROPD_OP_PUT;
    drawable->u.copy.src_bitmap      = (intptr_t)image;
    drawable->u.copy.src_area.right  = bw;
    drawable->u.copy.src_area.bottom = bh;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_DEVICE, unique);
    image->descriptor.type   = SPICE_IMAGE_TYPE_BITMAP;
    image->bitmap.flags      = QXL_BITMAP_DIRECT | QXL_BITMAP_TOP_DOWN;
    image->bitmap.stride     = bw * 4;
    image->descriptor.width  = image->bitmap.x = bw;
    image->descriptor.height = image->bitmap.y = bh;
    image->bitmap.data = (intptr_t)(update->bitmap);
    image->bitmap.palette = 0;
    image->bitmap.format = SPICE_BITMAP_FMT_32BIT;

    dst = update->bitmap;
    for (i = 0 ; i < bh * bw ; ++i, dst+=4) {
        *dst = (color+i % 255);
        *(dst+1) = 255 - color;
        *(dst+2) = (color * (color + i)) & 0xff;
        *(dst+3) = 0;
    }

    cmd->type = QXL_CMD_DRAW;
    cmd->data = (intptr_t)drawable;

    return update;
}

#define MEM_SLOT_GROUP_ID 0

static QXLWorker *qxl_worker = NULL;
static uint8_t primary_surface[HEIGHT * WIDTH * 4];

void create_test_primary_surface(QXLWorker *worker)
{
    QXLDevSurfaceCreate surface;

    surface.format     = SPICE_SURFACE_FMT_32_xRGB;
    surface.width      = WIDTH;
    surface.height     = HEIGHT;
    surface.stride     = -WIDTH * 4;
    surface.mouse_mode = TRUE;
    surface.flags      = 0;
    surface.type       = 0;
    surface.mem        = (intptr_t)&primary_surface;
    surface.group_id   = MEM_SLOT_GROUP_ID;

    qxl_worker->create_primary_surface(qxl_worker, 0, &surface);
}

QXLDevMemSlot slot = {
.slot_group_id = MEM_SLOT_GROUP_ID,
.slot_id = 0,
.generation = 0,
.virt_start = 0,
.virt_end = ~0,
.addr_delta = 0,
.qxl_ram_size = ~0,
};

void attache_worker(QXLInstance *qin, QXLWorker *_qxl_worker)
{
    static int count = 0;
    if (++count > 1) {
        printf("%s ignored\n", __func__);
        return;
    }
    printf("%s\n", __func__);
    qxl_worker = _qxl_worker;
    qxl_worker->add_memslot(qxl_worker, &slot);
    create_test_primary_surface(qxl_worker);
    qxl_worker->start(qxl_worker);
}

void set_compression_level(QXLInstance *qin, int level)
{
    printf("%s\n", __func__);
}

void set_mm_time(QXLInstance *qin, uint32_t mm_time)
{
}

void get_init_info(QXLInstance *qin, QXLDevInitInfo *info)
{
    bzero(info, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = 1;
}

#define NOTIFY_DISPLAY_BATCH 0
#define NOTIFY_CURSOR_BATCH 1

int notify = NOTIFY_DISPLAY_BATCH;
int cursor_notify = NOTIFY_CURSOR_BATCH;

int get_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    SimpleSpiceUpdate *update;

    if (!notify) {
        return FALSE;
    }
    notify--;
    update = test_spice_create_update();
    *ext = update->ext;
    notify = FALSE;
    return TRUE;
}


SpiceTimer *wakeup_timer;
int wakeup_ms = 500;

int req_cmd_notification(QXLInstance *qin)
{
    core->timer_start(wakeup_timer, wakeup_ms);
    return TRUE;
}

void do_wakeup()
{
    notify = NOTIFY_DISPLAY_BATCH;
    cursor_notify = NOTIFY_CURSOR_BATCH;
    qxl_worker->wakeup(qxl_worker);
}

void release_resource(QXLInstance *qin, struct QXLReleaseInfoExt release_info)
{
    //printf("%s\n", __func__);
    ASSERT(release_info.group_id == MEM_SLOT_GROUP_ID);
    test_spice_destroy_update((void*)release_info.info->id);
}

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32

static struct {
    QXLCursor cursor;
    uint8_t data[CURSOR_WIDTH * CURSOR_HEIGHT * 4]; // 32bit per pixel
} cursor;

void init_cursor()
{
    cursor.cursor.header.unique = 0; // TODO ??
    cursor.cursor.header.type = SPICE_CURSOR_TYPE_COLOR32;
    cursor.cursor.header.width = CURSOR_WIDTH;
    cursor.cursor.header.height = CURSOR_HEIGHT;
    cursor.cursor.header.hot_spot_x = 0;
    cursor.cursor.header.hot_spot_y = 0;
    cursor.cursor.data_size = CURSOR_WIDTH * CURSOR_HEIGHT;
    cursor.cursor.chunk.data_size = cursor.cursor.data_size;
    cursor.cursor.chunk.prev_chunk = cursor.cursor.chunk.next_chunk = 0;
}

int get_cursor_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    static int color = 0;
    QXLCursorCmd *cursor_cmd;
    int i, x, y, p;
    QXLCommandExt cmd;

    if (!cursor_notify) {
        return FALSE;
    }
    cursor_notify--;
    cursor_cmd = calloc(sizeof(QXLCursorCmd), 1);
    color = 100 + ((color + 1) % 100);
    cursor_cmd->type = QXL_CURSOR_SET;
    cursor_cmd->release_info.id = 0; // TODO: we leak the QXLCommandExt's
    cursor_cmd->u.set.position.x = 0;
    cursor_cmd->u.set.position.y = 0;
    cursor_cmd->u.set.visible = TRUE;
    cursor_cmd->u.set.shape = (uint64_t)&cursor;
    for (x = 0 ; x < CURSOR_WIDTH; ++x) {
        for (y = 0 ; y < CURSOR_HEIGHT; ++y) {
            p = 0;
            cursor.data[p] = (y*10 > color) ? color : 0;
            cursor.data[p+1] = cursor.data[p+2] = cursor.data[p+3] = 0;
        }
    }
    // TODO - shape has the, well, shape. device_data has?
    for (i = 0 ; i < QXL_CURSUR_DEVICE_DATA_SIZE ; ++i) {
        cursor_cmd->device_data[i] = color;
    }
    cmd.cmd.data = (uint64_t)cursor_cmd;
    cmd.cmd.type = QXL_CMD_CURSOR;
    cmd.group_id = MEM_SLOT_GROUP_ID;
    cmd.flags    = 0; // TODO - cursor flags (qxl->cmdflags in qxl/pointer.c)?
    *ext = cmd;
    //printf("%s\n", __func__);
    return TRUE;
}

int req_cursor_notification(QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

void notify_update(QXLInstance *qin, uint32_t update_id)
{
    printf("%s\n", __func__);
}

int flush_resources(QXLInstance *qin)
{
    printf("%s\n", __func__);
}

QXLInterface display_sif = {
    .base = {
        .type = SPICE_INTERFACE_QXL,
        .description = "test",
        .major_version = SPICE_INTERFACE_QXL_MAJOR,
        .minor_version = SPICE_INTERFACE_QXL_MINOR
    },
    .attache_worker = attache_worker,
    .set_compression_level = set_compression_level,
    .set_mm_time = set_mm_time,
    .get_init_info = get_init_info,
    .get_command = get_command,
    .req_cmd_notification = req_cmd_notification,
    .release_resource = release_resource,
    .get_cursor_command = get_cursor_command,
    .req_cursor_notification = req_cursor_notification,
    .notify_update = notify_update,
    .flush_resources = flush_resources,
};

QXLInstance display_sin = {
    .base = {
        .sif = &display_sif.base,
    },
    .id = 0,
};

SpiceServer *server;
SpiceCoreInterface *core;
SpiceTimer *ping_timer;

void show_channels(SpiceServer *server);

int ping_ms = 100;

void pinger(void *opaque)
{
    // show_channels is not thread safe - fails if disconnections / connections occur
    //show_channels(server);

    core->timer_start(ping_timer, ping_ms);
}

int main()
{
    core = basic_event_loop_init();
    server = spice_server_new();
    bzero(primary_surface, sizeof(primary_surface));
    spice_server_set_port(server, 5912);
    spice_server_set_noauth(server);
    spice_server_init(server, core);
    //spice_server_set_image_compression(server, SPICE_IMAGE_COMPRESS_OFF);
    spice_server_add_interface(server, &display_sin.base);

    ping_timer = core->timer_add(pinger, NULL);
    wakeup_timer = core->timer_add(do_wakeup, NULL);
    core->timer_start(ping_timer, ping_ms);

    basic_event_loop_mainloop();

    return 0;
}


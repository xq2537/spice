#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>
#include <spice/qxl_dev.h>
#include "test_display_base.h"
#include "red_channel.h"
#include "test_util.h"

#define MEM_SLOT_GROUP_ID 0

/* Parts cribbed from spice-display.h/.c/qxl.c */

typedef struct SimpleSpiceUpdate {
    QXLCommandExt ext; // first
    QXLDrawable drawable;
    QXLImage image;
    uint8_t *bitmap;
} SimpleSpiceUpdate;

typedef struct SimpleSurfaceCmd {
    QXLCommandExt ext; // first
    QXLSurfaceCmd surface_cmd;
} SimpleSurfaceCmd;

static void test_spice_destroy_update(SimpleSpiceUpdate *update)
{
    if (!update) {
        return;
    }
    if (update->bitmap) {
        free(update->bitmap);
    }
    free(update);
}

#define WIDTH 320
#define HEIGHT 320

#define SINGLE_PART 8
static const int angle_parts = 64 / SINGLE_PART;
static int unique = 1;
static int color = -1;
static int c_i = 0;

static void set_cmd(QXLCommandExt *ext, uint32_t type, QXLPHYSICAL data)
{
    ext->cmd.type = type;
    ext->cmd.data = data;
    ext->cmd.padding = 0;
    ext->group_id = MEM_SLOT_GROUP_ID;
    ext->flags = 0;
}

static void simple_set_release_info(QXLReleaseInfo *info, intptr_t ptr)
{
    info->id = ptr;
    //info->group_id = MEM_SLOT_GROUP_ID;
}

typedef struct Path {
    int t;
    int min_t;
    int max_t;
} Path;

static void path_init(Path *path, int min, int max)
{
    path->t = min;
    path->min_t = min;
    path->max_t = max;
}

static void path_progress(Path *path)
{
    path->t = (path->t+1)% (path->max_t - path->min_t) + path->min_t;
}

Path path;

static void draw_pos(int t, int *x, int *y)
{
#ifdef CIRCLE
    *y = HEIGHT/2 + (HEIGHT/3)*cos(t*2*M_PI/angle_parts);
    *x = WIDTH/2 + (WIDTH/3)*sin(t*2*M_PI/angle_parts);
#else
    *y = HEIGHT*(t % SINGLE_PART)/SINGLE_PART;
    *x = ((WIDTH/SINGLE_PART)*(t / SINGLE_PART)) % WIDTH;
#endif
}

static SimpleSpiceUpdate *test_spice_create_update_draw(uint32_t surface_id, int t)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLImage *image;
    int top, left;
    draw_pos(t, &left, &top);
    QXLRect bbox = {
        .top = top,
        .left = left,
    };
    uint8_t *dst;
    int bw, bh;
    int i;

    if ((t % angle_parts) == 0) {
        c_i++;
    }
    color = (color + 1) % 2;
    unique++;

    update   = calloc(sizeof(*update), 1);
    drawable = &update->drawable;
    image    = &update->image;

    bw       = WIDTH/SINGLE_PART;
    bh       = 48;
    bbox.right = bbox.left + bw;
    bbox.bottom = bbox.top + bh;
    update->bitmap = malloc(bw * bh * 4);
    //printf("allocated %p, %p\n", update, update->bitmap);

    drawable->surface_id      = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
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
        *(dst+((1+c_i)%3)) = 255 - color;
        *(dst+((2+c_i)%3)) = (color * (color + i)) & 0xff;
        *(dst+((3+c_i)%3)) = 0;
    }

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    return update;
}

static SimpleSpiceUpdate *test_spice_create_update_copy_bits(uint32_t surface_id)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    int bw, bh;
    QXLRect bbox = {
        .left = 10,
        .top = 0,
    };

    update   = calloc(sizeof(*update), 1);
    drawable = &update->drawable;

    bw       = WIDTH/SINGLE_PART;
    bh       = 48;
    bbox.right = bbox.left + bw;
    bbox.bottom = bbox.top + bh;
    //printf("allocated %p, %p\n", update, update->bitmap);

    drawable->surface_id      = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_COPY_BITS;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy_bits.src_pos.x = 0;
    drawable->u.copy_bits.src_pos.y = 0;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    return update;
}

static SimpleSurfaceCmd *create_surface(int surface_id, int width, int height, uint8_t *data)
{
    SimpleSurfaceCmd *simple_cmd = calloc(sizeof(SimpleSurfaceCmd), 1);
    QXLSurfaceCmd *surface_cmd = &simple_cmd->surface_cmd;

    set_cmd(&simple_cmd->ext, QXL_CMD_SURFACE, (intptr_t)surface_cmd);
    simple_set_release_info(&surface_cmd->release_info, (intptr_t)simple_cmd);
    surface_cmd->type = QXL_SURFACE_CMD_CREATE;
    surface_cmd->flags = 0; // ?
    surface_cmd->surface_id = surface_id;
    surface_cmd->u.surface_create.format = SPICE_SURFACE_FMT_32_xRGB;
    surface_cmd->u.surface_create.width = width;
    surface_cmd->u.surface_create.height = height;
    surface_cmd->u.surface_create.stride = -width * 4;
    surface_cmd->u.surface_create.data = (intptr_t)data;
    return simple_cmd;
}

static SimpleSurfaceCmd *destroy_surface(int surface_id)
{
    SimpleSurfaceCmd *simple_cmd = calloc(sizeof(SimpleSurfaceCmd), 1);
    QXLSurfaceCmd *surface_cmd = &simple_cmd->surface_cmd;

    set_cmd(&simple_cmd->ext, QXL_CMD_SURFACE, (intptr_t)surface_cmd);
    simple_set_release_info(&surface_cmd->release_info, (intptr_t)simple_cmd);
    surface_cmd->type = QXL_SURFACE_CMD_DESTROY;
    surface_cmd->flags = 0; // ?
    surface_cmd->surface_id = surface_id;
    return simple_cmd;
}

static QXLWorker *qxl_worker = NULL;
static uint8_t primary_surface[HEIGHT * WIDTH * 4];

static void create_test_primary_surface(QXLWorker *worker)
{
    QXLDevSurfaceCreate surface = { 0, };

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

static void attache_worker(QXLInstance *qin, QXLWorker *_qxl_worker)
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

static void set_compression_level(QXLInstance *qin, int level)
{
    printf("%s\n", __func__);
}

static void set_mm_time(QXLInstance *qin, uint32_t mm_time)
{
}

// we now have a secondary surface
#define MAX_SURFACE_NUM 2

static void get_init_info(QXLInstance *qin, QXLDevInitInfo *info)
{
    bzero(info, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = MAX_SURFACE_NUM;
}

#define NOTIFY_DISPLAY_BATCH (SINGLE_PART/2)
#define NOTIFY_CURSOR_BATCH 0

int cursor_notify = NOTIFY_CURSOR_BATCH;

#define SURF_WIDTH 320
#define SURF_HEIGHT 240
uint8_t secondary_surface[SURF_WIDTH * SURF_HEIGHT * 4];

// We shall now have a ring of commands, so that we can update
// it from a separate thread - since get_command is called from
// the worker thread, and we need to sometimes do an update_area,
// which cannot be done from red_worker context (not via dispatcher,
// since you get a deadlock, and it isn't designed to be done
// any other way, so no point testing that).
int commands_end = 0;
int commands_start = 0;
struct QXLCommandExt* commands[1024];

#define COMMANDS_SIZE COUNT(commands)

static void push_command(QXLCommandExt *ext)
{
    ASSERT(commands_end - commands_start < COMMANDS_SIZE);
    commands[commands_end%COMMANDS_SIZE] = ext;
    commands_end++;
}

static struct QXLCommandExt *get_simple_command(void)
{
    struct QXLCommandExt *ret = commands[commands_start%COMMANDS_SIZE];
    ASSERT(commands_start < commands_end);
    commands_start++;
    return ret;
}

static int num_commands(void)
{
    return commands_end - commands_start;
}

// called from spice_server thread (i.e. red_worker thread)
static int get_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    if (num_commands() == 0) {
        return FALSE;
    }
    *ext = *get_simple_command();
    return TRUE;
}

static int *simple_commands = NULL;
static int num_simple_commands = 0;

static void produce_command(void)
{
    static int target_surface = 0;
    static int cmd_index = 0;

    ASSERT(num_simple_commands);

    switch (simple_commands[cmd_index]) {
        case PATH_PROGRESS:
            path_progress(&path);
            break;
        case SIMPLE_UPDATE: {
            QXLRect rect = {.left = 0, .right = WIDTH,
                            .top = 0, .bottom = HEIGHT};
            qxl_worker->update_area(qxl_worker, 0, &rect, NULL, 0, 1);
            break;
        }
        case SIMPLE_COPY_BITS:
        case SIMPLE_DRAW: {
            SimpleSpiceUpdate *update;
            switch (simple_commands[cmd_index]) {
                case SIMPLE_COPY_BITS:
                    update = test_spice_create_update_copy_bits(target_surface);
                    break;
                case SIMPLE_DRAW:
                    update = test_spice_create_update_draw(target_surface, path.t);
                    break;
            }
            push_command(&update->ext);
            break;
        }
        case SIMPLE_CREATE_SURFACE: {
            SimpleSurfaceCmd *update;
            target_surface = MAX_SURFACE_NUM - 1;
            update = create_surface(target_surface, SURF_WIDTH, SURF_HEIGHT,
                                    secondary_surface);
            push_command(&update->ext);
            break;
        }
        case SIMPLE_DESTROY_SURFACE: {
            SimpleSurfaceCmd *update;
            update = destroy_surface(target_surface);
            target_surface = 0;
            push_command(&update->ext);
            break;
        }
    }
    cmd_index = (cmd_index + 1) % num_simple_commands;
}

SpiceTimer *wakeup_timer;
int wakeup_ms = 50;
SpiceCoreInterface *g_core;

static int req_cmd_notification(QXLInstance *qin)
{
    g_core->timer_start(wakeup_timer, wakeup_ms);
    return TRUE;
}

static void do_wakeup(void *opaque)
{
    int notify;
    cursor_notify = NOTIFY_CURSOR_BATCH;

    for (notify = NOTIFY_DISPLAY_BATCH; notify > 0;--notify) {
        produce_command();
    }

    g_core->timer_start(wakeup_timer, wakeup_ms);
    qxl_worker->wakeup(qxl_worker);
}

static void release_resource(QXLInstance *qin, struct QXLReleaseInfoExt release_info)
{
    QXLCommandExt *ext = (void*)release_info.info->id;
    //printf("%s\n", __func__);
    ASSERT(release_info.group_id == MEM_SLOT_GROUP_ID);
    switch (ext->cmd.type) {
        case QXL_CMD_DRAW:
            test_spice_destroy_update((void*)ext);
            break;
        case QXL_CMD_SURFACE:
            free(ext);
            break;
        default:
            abort();
    }
}

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32

static struct {
    QXLCursor cursor;
    uint8_t data[CURSOR_WIDTH * CURSOR_HEIGHT * 4]; // 32bit per pixel
} cursor;

#if 0
static void init_cursor()
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
#endif

static int get_cursor_command(QXLInstance *qin, struct QXLCommandExt *ext)
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

static int req_cursor_notification(QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

static void notify_update(QXLInstance *qin, uint32_t update_id)
{
    printf("%s\n", __func__);
}

static int flush_resources(QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
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

/* interface for tests */
void test_add_display_interface(SpiceServer *server)
{
    spice_server_add_interface(server, &display_sin.base);
}

void test_set_simple_command_list(int* commands, int num_commands)
{
    simple_commands = commands;
    num_simple_commands = num_commands;
}

SpiceServer* test_init(SpiceCoreInterface *core)
{
    int port = 5912;
    SpiceServer* server = spice_server_new();
    g_core = core;

    // some common initialization for all display tests
    printf("TESTER: listening on port %d (unsecure)\n", port);
    spice_server_set_port(server, port);
    spice_server_set_noauth(server);
    spice_server_init(server, core);

    path_init(&path, 0, angle_parts);
    bzero(primary_surface, sizeof(primary_surface));
    bzero(secondary_surface, sizeof(secondary_surface));
    wakeup_timer = core->timer_add(do_wakeup, NULL);
    return server;
}


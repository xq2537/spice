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

#ifndef _H_REDWORKER
#define _H_REDWORKER

#include "red_common.h"


static inline void set_bit(int index, uint32_t *addr)
{
    __asm__ __volatile__ ("lock btsl %1, %0": : "m" (*addr), "r" (index));
}

static inline void clear_bit(int index, uint32_t *addr)
{
    __asm__ __volatile__ ("lock btrl %1, %0": : "m" (*addr), "r" (index));
}

static inline int test_bit(int index, uint32_t val)
{
    return val & (1u << index);
}

enum {
    RED_WORKER_PENDING_WAKEUP,
    RED_WORKER_PENDING_OOM,
};

enum {
    RED_WORKER_MESSAGE_NOP,
    RED_WORKER_MESSAGE_UPDATE,
    RED_WORKER_MESSAGE_WAKEUP,
    RED_WORKER_MESSAGE_OOM,
    RED_WORKER_MESSAGE_READY,
    RED_WORKER_MESSAGE_DISPLAY_CONNECT,
    RED_WORKER_MESSAGE_DISPLAY_DISCONNECT,
    RED_WORKER_MESSAGE_DISPLAY_MIGRATE,
    RED_WORKER_MESSAGE_SAVE,
    RED_WORKER_MESSAGE_LOAD,
    RED_WORKER_MESSAGE_START,
    RED_WORKER_MESSAGE_STOP,
    RED_WORKER_MESSAGE_CURSOR_CONNECT,
    RED_WORKER_MESSAGE_CURSOR_DISCONNECT,
    RED_WORKER_MESSAGE_CURSOR_MIGRATE,
    RED_WORKER_MESSAGE_SET_COMPRESSION,
    RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
    RED_WORKER_MESSAGE_SET_MOUSE_MODE,
    RED_WORKER_MESSAGE_ADD_MEMSLOT,
    RED_WORKER_MESSAGE_DEL_MEMSLOT,
    RED_WORKER_MESSAGE_RESET_MEMSLOTS,
    RED_WORKER_MESSAGE_DESTROY_SURFACES,
    RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
    RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
    RED_WORKER_MESSAGE_RESET_CURSOR,
    RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
    RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
};

typedef uint32_t RedWorkeMessage;

#define RED_MAX_RENDERERS 4

enum {
    RED_RENDERER_INVALID,
    RED_RENDERER_CAIRO,
    RED_RENDERER_OGL_PBUF,
    RED_RENDERER_OGL_PIXMAP,
};

typedef struct WorkerInitData {
    struct QXLInterface *qxl_interface;
    int id;
    int channel;
    uint32_t *pending;
    uint32_t num_renderers;
    uint32_t renderers[RED_MAX_RENDERERS];
    spice_image_compression_t image_compression;
    int streaming_video;
    uint32_t num_memslots;
    uint32_t num_memslots_groups;
    uint8_t memslot_gen_bits;
    uint8_t memslot_id_bits;
    uint8_t internal_groupslot_id;
} WorkerInitData;

void *red_worker_main(void *arg);

static inline void send_data(int fd, void *in_buf, int n)
{
    uint8_t *buf = in_buf;
    do {
        int now;
        if ((now = write(fd, buf, n)) == -1) {
            if (errno == EINTR) {
                continue;
            }
            red_error("%s", strerror(errno));
        }
        buf += now;
        n -= now;
    } while (n);
}

static inline void write_message(int fd, RedWorkeMessage *message)
{
    send_data(fd, message, sizeof(RedWorkeMessage));
}

static inline void receive_data(int fd, void *in_buf, int n)
{
    uint8_t *buf = in_buf;
    do {
        int now;
        if ((now = read(fd, buf, n)) == -1) {
            if (errno == EINTR) {
                continue;
            }
            red_error("%s", strerror(errno));
        }
        buf += now;
        n -= now;
    } while (n);
}

static inline void read_message(int fd, RedWorkeMessage *message)
{
    receive_data(fd, message, sizeof(RedWorkeMessage));
}

#endif


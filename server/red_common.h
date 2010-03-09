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

#ifndef _H_RED_COMMON
#define _H_RED_COMMON

#include <sys/uio.h>
#include <openssl/ssl.h>

#include <spice/protocol.h>
#include <spice/macros.h>

#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    abort();                                                \
}

#define PANIC(format, ...) {                              \
    printf("%s: panic: " format "\n", __FUNCTION__, ## __VA_ARGS__ );   \
    abort();                                        \
}

#define PANIC_ON(x) if ((x)) {                             \
    printf("%s: panic %s\n", __FUNCTION__, #x);             \
    abort();                                                \
}

#define red_error(format, ...) {                                 \
    printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ );   \
    abort();                                                     \
}

#define red_printf(format, ...) \
    printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ )

#define red_printf_once(format, ...) {                              \
    static int do_print = TRUE;                                     \
    if (do_print) {                                                 \
        do_print = FALSE;                                           \
        printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ );  \
    }                                                               \
}

#define red_printf_some(every, format, ...) {                       \
    static int count = 0;                                           \
    if (count++ % (every) == 0) {                                   \
        printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ );  \
    }                                                               \
}

typedef enum {
    IMAGE_COMPRESS_INVALID,
    IMAGE_COMPRESS_AUTO_GLZ,
    IMAGE_COMPRESS_AUTO_LZ,
    IMAGE_COMPRESS_QUIC,
    IMAGE_COMPRESS_GLZ,
    IMAGE_COMPRESS_LZ,
    IMAGE_COMPRESS_OFF,
} image_compression_t;

enum {
    STREAM_VIDEO_INVALID,
    STREAM_VIDEO_OFF,
    STREAM_VIDEO_ALL,
    STREAM_VIDEO_FILTER
};

static inline uint64_t get_time_stamp()
{
    struct timespec time_space;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    return time_space.tv_sec * 1000 * 1000 * 1000 + time_space.tv_nsec;
}

#endif


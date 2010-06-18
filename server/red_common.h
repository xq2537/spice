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

#ifndef _H_RED_COMMON
#define _H_RED_COMMON

#include <sys/uio.h>
#include <openssl/ssl.h>

#include "spice.h"
#include "mem.h"
#include <messages.h>
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


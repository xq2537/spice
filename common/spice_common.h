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

#ifndef H_SPICE_COMMON
#define H_SPICE_COMMON

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include "backtrace.h"

#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    spice_backtrace();                                      \
    abort();                                                \
}

#define PANIC(format, ...) do {                                         \
    printf("%s: panic: " format "\n", __FUNCTION__, ## __VA_ARGS__ );   \
    abort();                                                            \
} while (0)

#define PANIC_ON(x) if ((x)) {                              \
    printf("%s: panic %s\n", __FUNCTION__, #x);             \
    abort();                                                \
}

#define red_error(format, ...) do {                              \
    printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ );   \
    abort();                                                     \
} while (0)
#define red_printf(format, ...) \
    printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ )

#define red_printf_once(format, ...) do {                           \
    static int do_print = TRUE;                                     \
    if (do_print) {                                                 \
        do_print = FALSE;                                           \
        printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ );  \
    }                                                               \
} while (0)

#define WARN(format, ...) red_printf("warning: "format"\n", ##__VA_ARGS__ );
#define WARN_ONCE red_printf_once

#define red_printf_some(every, format, ...) do {                    \
    static int count = 0;                                           \
    if (count++ % (every) == 0) {                                   \
        printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__ );  \
    }                                                               \
} while (0)

#define red_printf_debug(debug, prefix, format, ...) do {                   \
    static int debug_level = -1;                                    \
    if (debug_level == -1) {                                        \
        debug_level = getenv("SPICE_DEBUG_LEVEL") != NULL ? atoi(getenv("SPICE_DEBUG_LEVEL")) : 0;  \
    }                                                               \
    if (debug <= debug_level) {                                     \
        printf("%s: %s: " format "\n", prefix, __FUNCTION__, ## __VA_ARGS__ );  \
    }                                                               \
} while(0)

#endif

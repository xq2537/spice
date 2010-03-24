/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2010 Red Hat, Inc.

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

#ifndef _H_MEM
#define _H_MEM

#include <stdlib.h>
#include <spice/macros.h>

char *spice_strdup(const char *str) SPICE_GNUC_MALLOC;
char *spice_strndup(const char *str, size_t n_bytes) SPICE_GNUC_MALLOC;
void *spice_memdup(const void *mem, size_t n_bytes) SPICE_GNUC_MALLOC;
void *spice_malloc(size_t n_bytes) SPICE_GNUC_MALLOC SPICE_GNUC_ALLOC_SIZE(1);
void *spice_malloc0(size_t n_bytes) SPICE_GNUC_MALLOC SPICE_GNUC_ALLOC_SIZE(1);
void *spice_realloc(void *mem, size_t n_bytes) SPICE_GNUC_WARN_UNUSED_RESULT;
void *spice_malloc_n(size_t n_blocks, size_t n_block_bytes) SPICE_GNUC_MALLOC SPICE_GNUC_ALLOC_SIZE2(1,2);
void *spice_malloc_n_m(size_t n_blocks, size_t n_block_bytes, size_t extra_size) SPICE_GNUC_MALLOC;
void *spice_malloc0_n(size_t n_blocks, size_t n_block_bytes) SPICE_GNUC_MALLOC SPICE_GNUC_ALLOC_SIZE2(1,2);
void *spice_realloc_n(void *mem, size_t n_blocks, size_t n_block_bytes) SPICE_GNUC_WARN_UNUSED_RESULT;

/* Optimise: avoid the call to the (slower) _n function if we can
 * determine at compile-time that no overflow happens.
 */
#if defined (__GNUC__) && (__GNUC__ >= 2) && defined (__OPTIMIZE__)
#  define _SPICE_NEW(struct_type, n_structs, func)        \
    (struct_type *) (__extension__ ({                     \
        size_t __n = (size_t) (n_structs);                \
        size_t __s = sizeof (struct_type);                \
        void *__p;                                        \
        if (__s == 1)                                     \
            __p = spice_##func (__n);                     \
        else if (__builtin_constant_p (__n) &&            \
                 __n <= SIZE_MAX / __s)                   \
            __p = spice_##func (__n * __s);               \
        else                                              \
            __p = spice_##func##_n (__n, __s);            \
        __p;                                              \
    }))
#  define _SPICE_RENEW(struct_type, mem, n_structs, func) \
    (struct_type *) (__extension__ ({                             \
        size_t __n = (size_t) (n_structs);                \
        size_t __s = sizeof (struct_type);                \
        void *__p = (void *) (mem);                       \
        if (__s == 1)                                     \
            __p = spice_##func (__p, __n);                \
        else if (__builtin_constant_p (__n) &&            \
                 __n <= SIZE_MAX / __s)                   \
            __p = spice_##func (__p, __n * __s);          \
        else                                              \
            __p = spice_##func##_n (__p, __n, __s);       \
        __p;                                              \
    }))
#else

/* Unoptimised version: always call the _n() function. */

#define _SPICE_NEW(struct_type, n_structs, func)                        \
    ((struct_type *) spice_##func##_n ((n_structs), sizeof (struct_type)))
#define _SPICE_RENEW(struct_type, mem, n_structs, func)                 \
    ((struct_type *) spice_##func##_n (mem, (n_structs), sizeof (struct_type)))

#endif

#define spice_new(struct_type, n_structs) _SPICE_NEW(struct_type, n_structs, malloc)
#define spice_new0(struct_type, n_structs) _SPICE_NEW(struct_type, n_structs, malloc0)
#define spice_renew(struct_type, mem, n_structs) _SPICE_RENEW(struct_type, mem, n_structs, realloc)

#endif

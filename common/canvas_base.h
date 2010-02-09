/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#ifndef _H_CANVAS_BASE
#define _H_CANVAS_BASE


#include "pixman_utils.h"
#include "lz.h"
#include <spice/draw.h>

typedef struct _SpiceImageCache SpiceImageCache;
typedef struct _SpicePaletteCache SpicePaletteCache;

typedef struct {
    void (*put)(SpiceImageCache *cache,
                uint64_t id,
                pixman_image_t *surface);
    pixman_image_t *(*get)(SpiceImageCache *cache,
                           uint64_t id);
} SpiceImageCacheOps;

struct _SpiceImageCache {
  SpiceImageCacheOps *ops;
};

typedef struct {
    void (*put)(SpicePaletteCache *cache,
                SpicePalette *palette);
    SpicePalette *(*get)(SpicePaletteCache *cache,
                         uint64_t id);
    void (*release)(SpicePaletteCache *cache,
                    SpicePalette *palette);
} SpicePaletteCacheOps;

struct _SpicePaletteCache {
  SpicePaletteCacheOps *ops;
};

typedef void (*glz_decode_fn_t)(void *glz_decoder_opaque, uint8_t *data,
                                SpicePalette *plt, void *usr_data);
#ifndef CAIRO_CANVAS_NO_CHUNKS
typedef void *(*get_virt_fn_t)(void *get_virt_opaque, unsigned long addr, uint32_t add_size);
typedef void (*validate_virt_fn_t)(void *validate_virt_opaque, unsigned long virt,
                                   unsigned long from_addr, uint32_t add_size);
#endif

#endif


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
#include "region.h"
#include <spice/draw.h>

typedef void (*spice_destroy_fn_t)(void *data);

typedef struct _SpiceImageCache SpiceImageCache;
typedef struct _SpicePaletteCache SpicePaletteCache;
typedef struct _SpiceGlzDecoder SpiceGlzDecoder;
typedef struct _SpiceVirtMapping SpiceVirtMapping;
typedef struct _SpiceCanvas SpiceCanvas;

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

typedef struct {
    void (*decode)(SpiceGlzDecoder *decoder,
                   uint8_t *data,
                   SpicePalette *plt,
                   void *usr_data);
} SpiceGlzDecoderOps;

struct _SpiceGlzDecoder {
  SpiceGlzDecoderOps *ops;
};

typedef struct {
    void *(*get_virt)(SpiceVirtMapping *mapping, unsigned long addr, uint32_t add_size);
    void (*validate_virt)(SpiceVirtMapping *mapping, unsigned long virt,
                          unsigned long from_addr, uint32_t add_size);
} SpiceVirtMappingOps;

struct _SpiceVirtMapping {
  SpiceVirtMappingOps *ops;
};

typedef struct {
    void (*draw_fill)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill);
    void (*draw_copy)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy);
    void (*draw_opaque)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque);
    void (*copy_bits)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos);
    void (*draw_text)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceText *text);
    void (*draw_stroke)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke);
    void (*draw_rop3)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3);
    void (*draw_blend)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend);
    void (*draw_blackness)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness);
    void (*draw_whiteness)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness);
    void (*draw_invers)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers);
    void (*draw_transparent)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent);
    void (*draw_alpha_blend)(SpiceCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend);
    void (*put_image)(SpiceCanvas *canvas,
#ifdef WIN32
                      HDC dc,
#endif
                      const SpiceRect *dest, const uint8_t *src_data,
                      uint32_t src_width, uint32_t src_height, int src_stride,
                      const QRegion *clip);
    void (*clear)(SpiceCanvas *canvas);
    void (*read_bits)(SpiceCanvas *canvas, uint8_t *dest, int dest_stride, const SpiceRect *area);
    void (*group_start)(SpiceCanvas *canvas, QRegion *region);
    void (*group_end)(SpiceCanvas *canvas);
    void (*set_access_params)(SpiceCanvas *canvas, unsigned long base, unsigned long max);
    void (*destroy)(SpiceCanvas *canvas);
} SpiceCanvasOps;

void spice_canvas_set_usr_data(SpiceCanvas *canvas, void *data, spice_destroy_fn_t destroy_fn);
void *spice_canvas_get_usr_data(SpiceCanvas *canvas);

struct _SpiceCanvas {
  SpiceCanvasOps *ops;
};

#endif

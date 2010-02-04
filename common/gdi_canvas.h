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

#ifndef _H__GDI_CANVAS
#define _H__GDI_CANVAS

#include <stdint.h>

#include "draw.h"
#include "cairo.h"
#include "canvas_base.h"
#include "region.h"

typedef struct GdiCanvas GdiCanvas;

typedef struct {
    int width;
    int height;
    int stride;
    uint8_t *pixels;
} GdiImage;

void gdi_canvas_draw_fill(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill);
void gdi_canvas_draw_copy(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy);
void gdi_canvas_draw_opaque(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque);
void gdi_canvas_copy_bits(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos);
void gdi_canvas_draw_text(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceText *text);
void gdi_canvas_draw_stroke(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke);
void gdi_canvas_draw_rop3(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3);
void gdi_canvas_draw_blend(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend);
void gdi_canvas_draw_blackness(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness);
void gdi_canvas_draw_whiteness(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness);
void gdi_canvas_draw_invers(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers);
void gdi_canvas_draw_transparent(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip,
                                 SpiceTransparent* transparent);
void gdi_canvas_draw_alpha_blend(GdiCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend);
void gdi_canvas_put_image(GdiCanvas *canvas, HDC dc, const SpiceRect *dest, const uint8_t *src_data,
                          uint32_t src_width, uint32_t src_height, int src_stride,
                          const QRegion *clip);
void gdi_canvas_clear(GdiCanvas *canvas);

#ifdef CAIRO_CANVAS_ACCESS_TEST
void gdi_canvas_set_access_params(GdiCanvas *canvas, unsigned long base, unsigned long max);
#endif


GdiCanvas *gdi_canvas_create(HDC dc, class Mutex *lock, int bits, void *bits_cache_opaque,
                             bits_cache_put_fn_t bits_cache_put, bits_cache_get_fn_t bits_cache_get,
                             void *palette_cache_opaque, palette_cache_put_fn_t palette_cache_put,
                             palette_cache_get_fn_t palette_cache_get,
                             palette_cache_release_fn_t palette_cache_release,
                             void *glz_decoder_opaque,
                             glz_decode_fn_t glz_decode);

void gdi_canvas_destroy(GdiCanvas *canvas);

void gdi_canvas_init();

#endif

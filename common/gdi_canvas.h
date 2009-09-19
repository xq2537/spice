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

void gdi_canvas_draw_fill(GdiCanvas *canvas, Rect *bbox, Clip *clip, Fill *fill);
void gdi_canvas_draw_copy(GdiCanvas *canvas, Rect *bbox, Clip *clip, Copy *copy);
void gdi_canvas_draw_opaque(GdiCanvas *canvas, Rect *bbox, Clip *clip, Opaque *opaque);
void gdi_canvas_copy_bits(GdiCanvas *canvas, Rect *bbox, Clip *clip, Point *src_pos);
void gdi_canvas_draw_text(GdiCanvas *canvas, Rect *bbox, Clip *clip, Text *text);
void gdi_canvas_draw_stroke(GdiCanvas *canvas, Rect *bbox, Clip *clip, Stroke *stroke);
void gdi_canvas_draw_rop3(GdiCanvas *canvas, Rect *bbox, Clip *clip, Rop3 *rop3);
void gdi_canvas_draw_blend(GdiCanvas *canvas, Rect *bbox, Clip *clip, Blend *blend);
void gdi_canvas_draw_blackness(GdiCanvas *canvas, Rect *bbox, Clip *clip, Blackness *blackness);
void gdi_canvas_draw_whiteness(GdiCanvas *canvas, Rect *bbox, Clip *clip, Whiteness *whiteness);
void gdi_canvas_draw_invers(GdiCanvas *canvas, Rect *bbox, Clip *clip, Invers *invers);
void gdi_canvas_draw_transparent(GdiCanvas *canvas, Rect *bbox, Clip *clip,
                                 Transparent* transparent);
void gdi_canvas_draw_alpha_blend(GdiCanvas *canvas, Rect *bbox, Clip *clip, AlphaBlnd* alpha_blend);
void gdi_canvas_put_image(GdiCanvas *canvas, HDC dc, const Rect *dest, const uint8_t *src_data,
                          uint32_t src_width, uint32_t src_height, int src_stride,
                          const QRegion *clip);
void gdi_canvas_clear(GdiCanvas *canvas);

#ifdef CAIRO_CANVAS_ACCESS_TEST
void gdi_canvas_set_access_params(GdiCanvas *canvas, ADDRESS delta, unsigned long base,
                                  unsigned long max);
#else
void gdi_canvas_set_access_params(GdiCanvas *canvas, ADDRESS delta);
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

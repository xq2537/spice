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

#ifndef _H__CANVAS
#define _H__CANVAS

#include <stdint.h>

#include "draw.h"
#include "cairo.h"
#include "canvas_base.h"
#include "region.h"

typedef struct CairoCanvas CairoCanvas;

void canvas_draw_fill(CairoCanvas *canvas, Rect *bbox, Clip *clip, Fill *fill);
void canvas_draw_copy(CairoCanvas *canvas, Rect *bbox, Clip *clip, Copy *copy);
void canvas_draw_opaque(CairoCanvas *canvas, Rect *bbox, Clip *clip, Opaque *opaque);
void canvas_copy_bits(CairoCanvas *canvas, Rect *bbox, Clip *clip, Point *src_pos);
void canvas_draw_text(CairoCanvas *canvas, Rect *bbox, Clip *clip, Text *text);
void canvas_draw_stroke(CairoCanvas *canvas, Rect *bbox, Clip *clip, Stroke *stroke);
void canvas_draw_rop3(CairoCanvas *canvas, Rect *bbox, Clip *clip, Rop3 *rop3);
void canvas_draw_blend(CairoCanvas *canvas, Rect *bbox, Clip *clip, Blend *blend);
void canvas_draw_blackness(CairoCanvas *canvas, Rect *bbox, Clip *clip, Blackness *blackness);
void canvas_draw_whiteness(CairoCanvas *canvas, Rect *bbox, Clip *clip, Whiteness *whiteness);
void canvas_draw_invers(CairoCanvas *canvas, Rect *bbox, Clip *clip, Invers *invers);
void canvas_draw_transparent(CairoCanvas *canvas, Rect *bbox, Clip *clip, Transparent* transparent);
void canvas_draw_alpha_blend(CairoCanvas *canvas, Rect *bbox, Clip *clip, AlphaBlnd* alpha_blend);
#ifdef WIN32
void canvas_put_image(CairoCanvas *canvas, HDC dc, const Rect *dest, const uint8_t *src_data,
                      uint32_t src_width, uint32_t src_height, int src_stride,
                      const QRegion *clip);
#else
void canvas_put_image(CairoCanvas *canvas, const Rect *dest, const uint8_t *src_data,
                      uint32_t src_width, uint32_t src_height, int src_stride,
                      const QRegion *clip);
#endif
void canvas_clear(CairoCanvas *canvas);
void canvas_read_bits(CairoCanvas *canvas, uint8_t *dest, int dest_stride, const Rect *area);
void canvas_group_start(CairoCanvas *canvas, int n_clip_rects, Rect *clip_rects);
void canvas_group_end(CairoCanvas *canvas);
void canvas_set_addr_delta(CairoCanvas *canvas, ADDRESS delta);
#ifdef CAIRO_CANVAS_ACCESS_TEST
void canvas_set_access_params(CairoCanvas *canvas, ADDRESS delta, unsigned long base,
                              unsigned long max);
#else
void canvas_set_access_params(CairoCanvas *canvas, ADDRESS delta);
#endif

cairo_t *canvas_get_cairo(CairoCanvas *canvas);

#ifdef CAIRO_CANVAS_CACHE
CairoCanvas *canvas_create(cairo_t *cairo, int bits, void *bits_cache_opaque,
                           bits_cache_put_fn_t bits_cache_put, bits_cache_get_fn_t bits_cache_get,
                           void *palette_cache_opaque, palette_cache_put_fn_t palette_cache_put,
                           palette_cache_get_fn_t palette_cache_get,
                           palette_cache_release_fn_t palette_cache_release
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
CairoCanvas *canvas_create(cairo_t *cairo, int bits, void *bits_cache_opaque,
                           bits_cache_put_fn_t bits_cache_put, bits_cache_get_fn_t bits_cache_get
#else
CairoCanvas *canvas_create(cairo_t *cairo, int bits
#endif
#ifdef USE_GLZ
                           , void *glz_decoder_opaque, glz_decode_fn_t glz_decode
#endif
                           );
void canvas_destroy(CairoCanvas *canvas);

void cairo_canvas_init();

#endif

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

void canvas_draw_fill(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill);
void canvas_draw_copy(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy);
void canvas_draw_opaque(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque);
void canvas_copy_bits(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos);
void canvas_draw_text(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceText *text);
void canvas_draw_stroke(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke);
void canvas_draw_rop3(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3);
void canvas_draw_blend(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend);
void canvas_draw_blackness(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness);
void canvas_draw_whiteness(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness);
void canvas_draw_invers(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers);
void canvas_draw_transparent(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent);
void canvas_draw_alpha_blend(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend);
#ifdef WIN32
void canvas_put_image(CairoCanvas *canvas, HDC dc, const SpiceRect *dest, const uint8_t *src_data,
                      uint32_t src_width, uint32_t src_height, int src_stride,
                      const QRegion *clip);
#else
void canvas_put_image(CairoCanvas *canvas, const SpiceRect *dest, const uint8_t *src_data,
                      uint32_t src_width, uint32_t src_height, int src_stride,
                      const QRegion *clip);
#endif
void canvas_clear(CairoCanvas *canvas);
void canvas_read_bits(CairoCanvas *canvas, uint8_t *dest, int dest_stride, const SpiceRect *area);
void canvas_group_start(CairoCanvas *canvas, int n_clip_rects, SpiceRect *clip_rects);
void canvas_group_end(CairoCanvas *canvas);
void canvas_set_addr_delta(CairoCanvas *canvas, SPICE_ADDRESS delta);
#ifdef CAIRO_CANVAS_ACCESS_TEST
void canvas_set_access_params(CairoCanvas *canvas, unsigned long base, unsigned long max);
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
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , void *get_virt_opaque, get_virt_fn_t get_virt,
                           void *validate_virt_opaque, validate_virt_fn_t validate_virt
#endif
                           );
void canvas_destroy(CairoCanvas *canvas);

void cairo_canvas_init();

#endif

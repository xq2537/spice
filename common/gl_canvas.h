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

#include "glc.h"
#include "draw.h"
#include "canvas_base.h"
#include "region.h"

typedef struct GLCanvas GLCanvas;

void gl_canvas_draw_fill(GLCanvas *canvas, Rect *bbox, Clip *clip, Fill *fill);
void gl_canvas_draw_copy(GLCanvas *canvas, Rect *bbox, Clip *clip, Copy *copy);
void gl_canvas_draw_opaque(GLCanvas *canvas, Rect *bbox, Clip *clip, Opaque *opaque);
void gl_canvas_draw_blend(GLCanvas *canvas, Rect *bbox, Clip *clip, Blend *blend);
void gl_canvas_draw_alpha_blend(GLCanvas *canvas, Rect *bbox, Clip *clip, AlphaBlnd *alpha_blend);
void gl_canvas_draw_transparent(GLCanvas *canvas, Rect *bbox, Clip *clip, Transparent *transparent);
void gl_canvas_draw_whiteness(GLCanvas *canvas, Rect *bbox, Clip *clip, Whiteness *whiteness);
void gl_canvas_draw_blackness(GLCanvas *canvas, Rect *bbox, Clip *clip, Blackness *blackness);
void gl_canvas_draw_invers(GLCanvas *canvas, Rect *bbox, Clip *clip, Invers *invers);
void gl_canvas_draw_rop3(GLCanvas *canvas, Rect *bbox, Clip *clip, Rop3 *rop3);
void gl_canvas_draw_stroke(GLCanvas *canvas, Rect *bbox, Clip *clip, Stroke *stroke);
void gl_canvas_draw_text(GLCanvas *canvas, Rect *bbox, Clip *clip, Text *text);

void gl_canvas_copy_pixels(GLCanvas *canvas, Rect *bbox, Clip *clip, Point *src_pos);
void gl_canvas_read_pixels(GLCanvas *canvas, uint8_t *dest, int dest_stride, const Rect *area);

void gl_canvas_put_image(GLCanvas *canvas, const Rect *dest, const uint8_t *src_data,
                         uint32_t src_width, uint32_t src_height, int src_stride,
                         const QRegion *clip);

void gl_canvas_clear(GLCanvas *canvas);

void gl_canvas_set_top_mask(GLCanvas *canvas, int num_rect, const Rect *rects);
void gl_canvas_clear_top_mask(GLCanvas *canvas);

#ifdef CAIRO_CANVAS_ACCESS_TEST
void gl_canvas_set_access_params(GLCanvas *canvas, unsigned long base, unsigned long max);
#endif

void *gl_canvas_get_usr_data(GLCanvas *canvas);

#ifdef CAIRO_CANVAS_CACHE
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth,
                           void *bits_cache_opaque,
                           bits_cache_put_fn_t bits_cache_put,
                           bits_cache_get_fn_t bits_cache_get,
                           void *palette_cache_opaque,
                           palette_cache_put_fn_t palette_cache_put,
                           palette_cache_get_fn_t palette_cache_get,
                           palette_cache_release_fn_t palette_cache_release
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth,
                           void *bits_cache_opaque,
                           bits_cache_put_fn_t bits_cache_put,
                           bits_cache_get_fn_t bits_cache_get
#else
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth
#endif
#ifdef USE_GLZ
                           , void *glz_decoder_opaque, glz_decode_fn_t glz_decode
#endif
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , void *get_virt_opaque, get_virt_fn_t get_virt,
                           void *validate_virt_opaque, validate_virt_fn_t validate_virt
#endif
                           );
void gl_canvas_destroy(GLCanvas *, int);

void gl_canvas_init();


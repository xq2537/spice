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

#include "glc.h"
#include <spice/draw.h>
#include "canvas_base.h"
#include "region.h"

typedef struct GLCanvas GLCanvas;

void gl_canvas_draw_fill(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill);
void gl_canvas_draw_copy(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy);
void gl_canvas_draw_opaque(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque);
void gl_canvas_draw_blend(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend);
void gl_canvas_draw_alpha_blend(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd *alpha_blend);
void gl_canvas_draw_transparent(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceTransparent *transparent);
void gl_canvas_draw_whiteness(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness);
void gl_canvas_draw_blackness(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness);
void gl_canvas_draw_invers(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers);
void gl_canvas_draw_rop3(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3);
void gl_canvas_draw_stroke(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke);
void gl_canvas_draw_text(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceText *text);

void gl_canvas_copy_pixels(GLCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos);
void gl_canvas_read_pixels(GLCanvas *canvas, uint8_t *dest, int dest_stride, const SpiceRect *area);

void gl_canvas_put_image(GLCanvas *canvas, const SpiceRect *dest, const uint8_t *src_data,
                         uint32_t src_width, uint32_t src_height, int src_stride,
                         const QRegion *clip);

void gl_canvas_clear(GLCanvas *canvas);

void gl_canvas_set_top_mask(GLCanvas *canvas, QRegion *region);
void gl_canvas_clear_top_mask(GLCanvas *canvas);

#ifdef CAIRO_CANVAS_ACCESS_TEST
void gl_canvas_set_access_params(GLCanvas *canvas, unsigned long base, unsigned long max);
#endif

void *gl_canvas_get_usr_data(GLCanvas *canvas);

#ifdef CAIRO_CANVAS_CACHE
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth,
                           SpiceImageCache *bits_cache,
                           SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth,
                           SpiceImageCache *bits_cache
#else
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth
#endif
                           , SpiceGlzDecoder *glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                            , SpiceVirtMapping *virt_mapping
#endif
                           );
void gl_canvas_destroy(GLCanvas *, int);

void gl_canvas_init();


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

#include "common.h"
#include <stdint.h>
#include "red_window.h"
#include "red_cairo_canvas.h"
#include "utils.h"
#include "debug.h"
#include "region.h"
#include "red_pixmap_cairo.h"

CCanvas::CCanvas(PixmapCache& pixmap_cache, PaletteCache& palette_cache,
                 GlzDecoderWindow &glz_decoder_window)
    : Canvas (pixmap_cache, palette_cache, glz_decoder_window)
    , _canvas (NULL)
    , _pixmap (0)
{
}

CCanvas::~CCanvas()
{
    destroy();
}

void CCanvas::destroy()
{
    if (_canvas) {
        cairo_t *cairo = canvas_get_cairo(_canvas);
        canvas_destroy(_canvas);
        cairo_destroy(cairo);
        _canvas = NULL;
    }
    destroy_pixmap();
}

void CCanvas::clear()
{
    if (_canvas) {
        canvas_clear(_canvas);
    }
}

void CCanvas::destroy_pixmap()
{
    delete _pixmap;
    _pixmap = NULL;
}

void CCanvas::create_pixmap(int width, int height, RedWindow *win)
{
    _pixmap = new RedPixmapCairo(width, height, RedPixmap::RGB32, true, NULL, win);
}

void CCanvas::copy_pixels(const QRegion& region, RedDrawable& dest_dc)
{
    for (int i = 0; i < (int)region.num_rects; i++) {
        Rect* r = &region.rects[i];
        dest_dc.copy_pixels(*_pixmap, r->left, r->top, *r);
    }
}

void CCanvas::copy_pixels(const QRegion& region, RedDrawable* dest_dc, const PixmapHeader* pixmap)
{
    copy_pixels(region, *dest_dc);
}

void CCanvas::set_mode(int width, int height, int depth, RedWindow *win)
{
    cairo_surface_t *cairo_surface;
    cairo_t *cairo;

    destroy();
    create_pixmap(width, height, win);
    cairo_surface = cairo_image_surface_create_for_data(_pixmap->get_data(), CAIRO_FORMAT_RGB24,
                                                        width, height, _pixmap->get_stride());
    if (cairo_surface_status(cairo_surface) != CAIRO_STATUS_SUCCESS) {
        THROW("create surface failed, %s",
              cairo_status_to_string(cairo_surface_status(cairo_surface)));
    }

    cairo = cairo_create(cairo_surface);
    cairo_surface_destroy(cairo_surface);
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        THROW("create cairo failed, %s", cairo_status_to_string(cairo_status(cairo)));
    }
    if (!(_canvas = canvas_create(cairo, depth,
                                  &pixmap_cache(), bits_cache_put, bits_cache_get,
                                  &palette_cache(), palette_cache_put, palette_cache_get,
                                  palette_cache_release,
                                  &glz_decoder(),
                                  glz_decode))) {
        THROW("create canvas failed");
    }
}

void CCanvas::set_access_params(ADDRESS delta, unsigned long base, unsigned long max)
{
    canvas_set_access_params(_canvas, delta, base, max);
}

void CCanvas::draw_fill(Rect *bbox, Clip *clip, Fill *fill)
{
    canvas_draw_fill(_canvas, bbox, clip, fill);
}

void CCanvas::draw_text(Rect *bbox, Clip *clip, Text *text)
{
    canvas_draw_text(_canvas, bbox, clip, text);
}

void CCanvas::draw_opaque(Rect *bbox, Clip *clip, Opaque *opaque)
{
    canvas_draw_opaque(_canvas, bbox, clip, opaque);
}

void CCanvas::draw_copy(Rect *bbox, Clip *clip, Copy *copy)
{
    canvas_draw_copy(_canvas, bbox, clip, copy);
}

void CCanvas::draw_transparent(Rect *bbox, Clip *clip, Transparent* transparent)
{
    canvas_draw_transparent(_canvas, bbox, clip, transparent);
}

void CCanvas::draw_alpha_blend(Rect *bbox, Clip *clip, AlphaBlnd* alpha_blend)
{
    canvas_draw_alpha_blend(_canvas, bbox, clip, alpha_blend);
}

void CCanvas::copy_bits(Rect *bbox, Clip *clip, Point *src_pos)
{
    canvas_copy_bits(_canvas, bbox, clip, src_pos);
}

void CCanvas::draw_blend(Rect *bbox, Clip *clip, Blend *blend)
{
    canvas_draw_blend(_canvas, bbox, clip, blend);
}

void CCanvas::draw_blackness(Rect *bbox, Clip *clip, Blackness *blackness)
{
    canvas_draw_blackness(_canvas, bbox, clip, blackness);
}

void CCanvas::draw_whiteness(Rect *bbox, Clip *clip, Whiteness *whiteness)
{
    canvas_draw_whiteness(_canvas, bbox, clip, whiteness);
}

void CCanvas::draw_invers(Rect *bbox, Clip *clip, Invers *invers)
{
    canvas_draw_invers(_canvas, bbox, clip, invers);
}

void CCanvas::draw_rop3(Rect *bbox, Clip *clip, Rop3 *rop3)
{
    canvas_draw_rop3(_canvas, bbox, clip, rop3);
}

void CCanvas::draw_stroke(Rect *bbox, Clip *clip, Stroke *stroke)
{
    canvas_draw_stroke(_canvas, bbox, clip, stroke);
}

#ifdef WIN32
void CCanvas::put_image(HDC dc, const PixmapHeader& image, const Rect& dest, const QRegion* clip)
{
    canvas_put_image(_canvas, dc, &dest, image.data, image.width, image.height, image.stride,
                     clip);
}

#else
void CCanvas::put_image(const PixmapHeader& image, const Rect& dest, const QRegion* clip)
{
    canvas_put_image(_canvas, &dest, image.data, image.width, image.height, image.stride,
                     clip);
}

#endif

CanvasType CCanvas::get_pixmap_type()
{
    return CANVAS_TYPE_CAIRO;
}


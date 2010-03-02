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
#include "red_gdi_canvas.h"
#include "utils.h"
#include "debug.h"
#include "region.h"
#include "red_pixmap_gdi.h"

GDICanvas::GDICanvas(PixmapCache& pixmap_cache, PaletteCache& palette_cache,
                     GlzDecoderWindow &glz_decoder_window)
    : Canvas (pixmap_cache, palette_cache, glz_decoder_window)
    , _canvas (NULL)
    , _pixmap (0)
{
}

GDICanvas::~GDICanvas()
{
    destroy();
}

void GDICanvas::destroy()
{
    if (_canvas) {
        _canvas = NULL;
    }
    destroy_pixmap();
}

void GDICanvas::clear()
{
    if (_canvas) {
        gdi_canvas_clear(_canvas);
    }
}

void GDICanvas::destroy_pixmap()
{
    delete _pixmap;
    _pixmap = NULL;
}

void GDICanvas::create_pixmap(int width, int height)
{
    _pixmap = new RedPixmapGdi(width, height, RedPixmap::RGB32, true, NULL);
}

void GDICanvas::copy_pixels(const QRegion& region, RedDrawable& dest_dc)
{
    pixman_box32_t *rects;
    int num_rects;

    rects = pixman_region32_rectangles((pixman_region32_t *)&region, &num_rects);
	for (int i = 0; i < num_rects; i++) {
        SpiceRect r;

        r.left = rects[i].x1;
        r.top = rects[i].y1;
        r.right = rects[i].x2;
        r.bottom = rects[i].y2;
        dest_dc.copy_pixels(*_pixmap, r.left, r.top, r);
    }
}

void GDICanvas::copy_pixels(const QRegion& region, RedDrawable* dest_dc, const PixmapHeader* pixmap)
{
    copy_pixels(region, *dest_dc);
}

void GDICanvas::set_mode(int width, int height, int depth)
{
    destroy();
    create_pixmap(width, height);
    if (!(_canvas = gdi_canvas_create(_pixmap->get_dc(),
                                      &_pixmap->get_mutex(),
                                      depth, &pixmap_cache().base,
                                      &palette_cache().base,
                                      &glz_decoder()))) {
        THROW("create canvas failed");
    }
}

void GDICanvas::set_access_params(unsigned long base, unsigned long max)
{
    gdi_canvas_set_access_params(_canvas, base, max);
}

void GDICanvas::draw_fill(SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill)
{
    gdi_canvas_draw_fill(_canvas, bbox, clip, fill);
}

void GDICanvas::draw_text(SpiceRect *bbox, SpiceClip *clip, SpiceText *text)
{
    gdi_canvas_draw_text(_canvas, bbox, clip, text);
}

void GDICanvas::draw_opaque(SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque)
{
    gdi_canvas_draw_opaque(_canvas, bbox, clip, opaque);
}

void GDICanvas::draw_copy(SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy)
{
    gdi_canvas_draw_copy(_canvas, bbox, clip, copy);
}

void GDICanvas::draw_transparent(SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent)
{
    gdi_canvas_draw_transparent(_canvas, bbox, clip, transparent);
}

void GDICanvas::draw_alpha_blend(SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend)
{
    gdi_canvas_draw_alpha_blend(_canvas, bbox, clip, alpha_blend);
}

void GDICanvas::copy_bits(SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos)
{
    gdi_canvas_copy_bits(_canvas, bbox, clip, src_pos);
}

void GDICanvas::draw_blend(SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend)
{
    gdi_canvas_draw_blend(_canvas, bbox, clip, blend);
}

void GDICanvas::draw_blackness(SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness)
{
    gdi_canvas_draw_blackness(_canvas, bbox, clip, blackness);
}

void GDICanvas::draw_whiteness(SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness)
{
    gdi_canvas_draw_whiteness(_canvas, bbox, clip, whiteness);
}

void GDICanvas::draw_invers(SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers)
{
    gdi_canvas_draw_invers(_canvas, bbox, clip, invers);
}

void GDICanvas::draw_rop3(SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3)
{
    gdi_canvas_draw_rop3(_canvas, bbox, clip, rop3);
}

void GDICanvas::draw_stroke(SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke)
{
    gdi_canvas_draw_stroke(_canvas, bbox, clip, stroke);
}

void GDICanvas::put_image(HDC dc, const PixmapHeader& image, const SpiceRect& dest, const QRegion* clip)
{
    gdi_canvas_put_image(_canvas, dc, &dest, image.data, image.width, image.height, image.stride,
                         clip);
}

CanvasType GDICanvas::get_pixmap_type()
{
    return CANVAS_TYPE_GDI;
}


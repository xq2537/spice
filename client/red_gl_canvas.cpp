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
#include "red_gl_canvas.h"
#include "utils.h"
#include "debug.h"
#include "region.h"
#include "red_pixmap_gl.h"
#include <GL/glx.h>

GCanvas::GCanvas(PixmapCache& pixmap_cache, PaletteCache& palette_cache,
                 GlzDecoderWindow &glz_decoder_window)
    : Canvas(pixmap_cache, palette_cache, glz_decoder_window)
    , _canvas (NULL)
    , _pixmap (0)
    , _textures_lost (false)
{
}

GCanvas::~GCanvas()
{
    destroy();
}

void GCanvas::destroy()
{
    if (_canvas) {
        gl_canvas_destroy(_canvas, _textures_lost);
        _canvas = NULL;
    }
    destroy_pixmap();
}

void GCanvas::clear()
{
    if (_canvas) {
        gl_canvas_clear(_canvas);
    }
}

void GCanvas::destroy_pixmap()
{
    delete _pixmap;
    _pixmap = NULL;
}

void GCanvas::create_pixmap(int width, int height, RedWindow *win,
                            RenderType rendertype)
{
    _pixmap = new RedPixmapGL(width, height, RedPixmap::RGB32, true, NULL,
                              win, rendertype);
}

void GCanvas::copy_pixels(const QRegion& region, RedDrawable& dest_dc)
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

void GCanvas::copy_pixels(const QRegion& region, RedDrawable* dest_dc, const PixmapHeader* pixmap)
{
    copy_pixels(region, *dest_dc);
}

void GCanvas::set_mode(int width, int height, int depth, RedWindow *win,
                       RenderType rendertype)
{
    destroy();

    create_pixmap(width, height, win, rendertype);
    if (!(_canvas = gl_canvas_create(NULL, width, height, depth,
                                     &pixmap_cache().base,
                                     &palette_cache().base,
                                     &glz_decoder()))) {
        THROW("create canvas failed");
    }
}

void GCanvas::set_access_params(unsigned long base, unsigned long max)
{
    gl_canvas_set_access_params(_canvas, base, max);
}

void GCanvas::draw_fill(SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill)
{
    gl_canvas_draw_fill(_canvas, bbox, clip, fill);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_text(SpiceRect *bbox, SpiceClip *clip, SpiceText *text)
{
    gl_canvas_draw_text(_canvas, bbox, clip, text);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_opaque(SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque)
{
    gl_canvas_draw_opaque(_canvas, bbox, clip, opaque);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_copy(SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy)
{
    gl_canvas_draw_copy(_canvas, bbox, clip, copy);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_transparent(SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent)
{
    gl_canvas_draw_transparent(_canvas, bbox, clip, transparent);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_alpha_blend(SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend)
{
    gl_canvas_draw_alpha_blend(_canvas, bbox, clip, alpha_blend);
    _pixmap->update_texture(bbox);
}

void GCanvas::copy_bits(SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos)
{
    gl_canvas_copy_pixels(_canvas, bbox, clip, src_pos);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_blend(SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend)
{
    gl_canvas_draw_blend(_canvas, bbox, clip, blend);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_blackness(SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness)
{
    gl_canvas_draw_blackness(_canvas, bbox, clip, blackness);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_whiteness(SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness)
{
    gl_canvas_draw_whiteness(_canvas, bbox, clip, whiteness);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_invers(SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers)
{
    gl_canvas_draw_invers(_canvas, bbox, clip, invers);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_rop3(SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3)
{
    gl_canvas_draw_rop3(_canvas, bbox, clip, rop3);
    _pixmap->update_texture(bbox);
}

void GCanvas::draw_stroke(SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke)
{
    gl_canvas_draw_stroke(_canvas, bbox, clip, stroke);
    _pixmap->update_texture(bbox);
}

void GCanvas::put_image(const PixmapHeader& image, const SpiceRect& dest,
                        const QRegion* clip)
{
    gl_canvas_put_image(_canvas, &dest, image.data, image.width, image.height,
                        image.stride, clip);
    _pixmap->update_texture(&dest);
}

CanvasType GCanvas::get_pixmap_type()
{
    return CANVAS_TYPE_GL;
}

void GCanvas::textures_lost()
{
    _textures_lost = true;
    _pixmap->textures_lost();
}

void GCanvas::touch_context()
{
    _pixmap->touch_context();
}

void GCanvas::pre_gl_copy()
{
    _pixmap->pre_copy();
}

void GCanvas::post_gl_copy()
{
    _pixmap->past_copy();
}


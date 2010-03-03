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
        gl_canvas_set_textures_lost (_canvas, (int)_textures_lost);
        _canvas->ops->destroy(_canvas);
        _canvas = NULL;
    }
    destroy_pixmap();
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
    if (!(_canvas = gl_canvas_create(width, height, depth,
                                     &pixmap_cache().base,
                                     &palette_cache().base,
                                     &glz_decoder()))) {
        THROW("create canvas failed");
    }
}

void GCanvas::touched_bbox(const SpiceRect *bbox)
{
    _pixmap->update_texture(bbox);
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


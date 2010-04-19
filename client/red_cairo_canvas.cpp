/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
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
                 GlzDecoderWindow &glz_decoder_window, CSurfaces& csurfaces)
    : Canvas (pixmap_cache, palette_cache, glz_decoder_window, csurfaces)
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
        _canvas->ops->destroy(_canvas);
        _canvas = NULL;
    }
    destroy_pixmap();
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

void CCanvas::copy_pixels(const QRegion& region, RedDrawable* dest_dc, const PixmapHeader* pixmap)
{
    copy_pixels(region, *dest_dc);
}

void CCanvas::set_mode(int width, int height, int depth, RedWindow *win)
{
    pixman_image_t *surface;

    destroy();
    create_pixmap(width, height, win);
    surface = pixman_image_create_bits(PIXMAN_x8r8g8b8, width, height,
                                       (uint32_t *)_pixmap->get_data(),
                                       _pixmap->get_stride());
    if (surface == NULL) {
        THROW("create surface failed, out of memory");
    }

    if (!(_canvas = canvas_create(surface, depth,
                                  &pixmap_cache().base,
                                  &palette_cache().base,
                                  &csurfaces().base,
                                  &glz_decoder()))) {
        THROW("create canvas failed");
    }
    pixman_image_unref (surface);
}

CanvasType CCanvas::get_pixmap_type()
{
    return CANVAS_TYPE_CAIRO;
}


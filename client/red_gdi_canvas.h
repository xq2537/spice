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

#ifndef _H_GDICANVAS
#define _H_GDICANVAS

#include "canvas.h"
#include "gdi_canvas.h"
#include "red_pixmap_gdi.h"

class RedPixmap;


class GDICanvas: public Canvas {
public:
    GDICanvas(PixmapCache& pixmap_cache, PaletteCache& palette_cache,
              GlzDecoderWindow &glz_decoder_window, CSurfaces &csurfaces);
    virtual ~GDICanvas();

    virtual void set_mode(int x, int y, int bits);
    virtual void thread_touch() {}
    virtual void copy_pixels(const QRegion& region, RedDrawable* dc,
                             const PixmapHeader* pixmap);
    virtual void copy_pixels(const QRegion& region, RedDrawable& dc);

    virtual CanvasType get_pixmap_type();

private:
    void create_pixmap(int width, int height);
    void destroy_pixmap();
    void destroy();

private:
    RedPixmapGdi *_pixmap;
    RedPixmapGdi *_helper_pixmap;
    HDC _dc;
    HBITMAP _prev_bitmap;
    unsigned long _base;
    unsigned long _max;
};

#endif


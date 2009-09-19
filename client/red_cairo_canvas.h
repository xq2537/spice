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

#ifndef _H_CCANVAS
#define _H_CCANVAS

#include "canvas.h"
#include "cairo_canvas.h"

class RedPixmap;

class CCanvas: public Canvas {
public:
    CCanvas(PixmapCache& pixmap_cache, PaletteCache& palette_cache,
            GlzDecoderWindow &glz_decoder_window);
    virtual ~CCanvas();

    virtual void set_mode(int x, int y, int bits, RedWindow *win);
    virtual void clear();
    virtual void thread_touch() {}
    virtual void copy_pixels(const QRegion& region, RedDrawable* dc,
                             const PixmapHeader* pixmap);
    virtual void copy_pixels(const QRegion& region, RedDrawable& dc);
#ifdef WIN32
    virtual void put_image(HDC dc, const PixmapHeader& image,
                           const Rect& dest, const QRegion* clip);
#else
    virtual void put_image(const PixmapHeader& image, const Rect& dest,
                           const QRegion* clip);
#endif

    virtual CanvasType get_pixmap_type();

protected:
    virtual void set_access_params(ADDRESS delta, unsigned long base, unsigned long max);
    virtual void draw_fill(Rect *bbox, Clip *clip, Fill *fill);
    virtual void draw_copy(Rect *bbox, Clip *clip, Copy *copy);
    virtual void draw_opaque(Rect *bbox, Clip *clip, Opaque *opaque);
    virtual void copy_bits(Rect *bbox, Clip *clip, Point *src_pos);
    virtual void draw_text(Rect *bbox, Clip *clip, Text *text);
    virtual void draw_stroke(Rect *bbox, Clip *clip, Stroke *stroke);
    virtual void draw_rop3(Rect *bbox, Clip *clip, Rop3 *rop3);
    virtual void draw_blend(Rect *bbox, Clip *clip, Blend *blend);
    virtual void draw_blackness(Rect *bbox, Clip *clip, Blackness *blackness);
    virtual void draw_whiteness(Rect *bbox, Clip *clip, Whiteness *whiteness);
    virtual void draw_invers(Rect *bbox, Clip *clip, Invers *invers);
    virtual void draw_transparent(Rect *bbox, Clip *clip, Transparent* transparent);
    virtual void draw_alpha_blend(Rect *bbox, Clip *clip, AlphaBlnd* alpha_blend);

private:
    void create_pixmap(int width, int height, RedWindow *win);
    void destroy_pixmap();
    void destroy();

private:
    CairoCanvas* _canvas;
    RedPixmap *_pixmap;
    unsigned long _base;
    unsigned long _max;
};

#endif


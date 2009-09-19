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

#ifndef _H_GCANVAS
#define _H_GCANVAS

#include "canvas.h"
#include "cairo_canvas.h"
#include "gl_canvas.h"
#include "red_pixmap_gl.h"
#include "red_window.h"

class RedPixmapGL;

class GCanvas: public Canvas {
public:
    GCanvas(PixmapCache& pixmap_cache, PaletteCache& palette_cache,
            GlzDecoderWindow &glz_decoder_window);
    virtual ~GCanvas();

    void set_mode(int width, int height, int depth, RedWindow *win,
                  RenderType rendertype);
    void clear();
    void thread_touch() {}
    void copy_pixels(const QRegion& region, RedDrawable* dc,
                     const PixmapHeader* pixmap);
    void copy_pixels(const QRegion& region, RedDrawable& dc);
    void put_image(const PixmapHeader& image, const Rect& dest,
                   const QRegion* clip);

    void set_access_params(ADDRESS delta, unsigned long base, unsigned long max);
    void draw_fill(Rect *bbox, Clip *clip, Fill *fill);
    void draw_copy(Rect *bbox, Clip *clip, Copy *copy);
    void draw_opaque(Rect *bbox, Clip *clip, Opaque *opaque);
    void copy_bits(Rect *bbox, Clip *clip, Point *src_pos);
    void draw_text(Rect *bbox, Clip *clip, Text *text);
    void draw_stroke(Rect *bbox, Clip *clip, Stroke *stroke);
    void draw_rop3(Rect *bbox, Clip *clip, Rop3 *rop3);
    void draw_blend(Rect *bbox, Clip *clip, Blend *blend);
    void draw_blackness(Rect *bbox, Clip *clip, Blackness *blackness);
    void draw_whiteness(Rect *bbox, Clip *clip, Whiteness *whiteness);
    void draw_invers(Rect *bbox, Clip *clip, Invers *invers);
    void draw_transparent(Rect *bbox, Clip *clip, Transparent* transparent);
    void draw_alpha_blend(Rect *bbox, Clip *clip, AlphaBlnd* alpha_blend);

    virtual void textures_lost();
    virtual CanvasType get_pixmap_type();
    virtual void touch_context();
    virtual void pre_gl_copy();
    virtual void post_gl_copy();

private:
    void create_pixmap(int width, int height, RedWindow *win,
                       RenderType rendertype);
    void destroy_pixmap();
    void destroy();

private:
    GLCanvas* _canvas;
    RedPixmapGL *_pixmap;
    bool _textures_lost;
};

#endif


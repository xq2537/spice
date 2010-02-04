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
    void put_image(const PixmapHeader& image, const SpiceRect& dest,
                   const QRegion* clip);

    void set_access_params(unsigned long base, unsigned long max);
    void draw_fill(SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill);
    void draw_copy(SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy);
    void draw_opaque(SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque);
    void copy_bits(SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos);
    void draw_text(SpiceRect *bbox, SpiceClip *clip, SpiceText *text);
    void draw_stroke(SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke);
    void draw_rop3(SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3);
    void draw_blend(SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend);
    void draw_blackness(SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness);
    void draw_whiteness(SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness);
    void draw_invers(SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers);
    void draw_transparent(SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent);
    void draw_alpha_blend(SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend);

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


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

#define GL_GLEXT_PROTOTYPES
#include "common.h"
#include "red_drawable.h"
#include "pixels_source_p.h"
#include "debug.h"
#include "x_platform.h"
#include "utils.h"
#include "gl_utils.h"
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>

static inline void copy_to_gldrawable_from_gltexture(const RedDrawable_p* dest,
                                                     const Rect& area,
                                                     const Point& offset,
                                                     const PixelsSource_p* source,
                                                     int src_x, int src_y)
{
    float text_x1, text_x2;
    float text_y1, text_y2;
    int vertex_x1, vertex_x2;
    int vertex_y1, vertex_y2;
    GLXPbuffer pbuffer;
    GLXContext context;
    RenderType rendertype;
    Window win;

    text_x1 = (float)src_x / source->gl.width_powed;
    text_x2 = text_x1 + (float)(area.right - area.left) / source->gl.width_powed;

    text_y1 = ((float)source->gl.height - (area.bottom - area.top) - src_y) /
              source->gl.height_powed;
    text_y2 = text_y1 + (float)(area.bottom - area.top) / source->gl.height_powed;

    vertex_x1 = area.left + offset.x;
    vertex_y1 = dest->source.x_drawable.height - (area.top + offset.y) - (area.bottom - area.top);
    vertex_x2 = vertex_x1 + (area.right - area.left);
    vertex_y2 = vertex_y1 + (area.bottom - area.top);

    glEnable(GL_TEXTURE_2D);

    rendertype = source->gl.rendertype;
    if (rendertype == RENDER_TYPE_FBO) {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    } else {
        win = source->gl.win;
        context = source->gl.context;
        glXMakeCurrent(XPlatform::get_display(), win, context);
    }

    glBindTexture(GL_TEXTURE_2D, source->gl.tex);

    glBegin(GL_QUADS);
    glTexCoord2f(text_x1, text_y1);
    glVertex2i(vertex_x1, vertex_y1);
    glTexCoord2f(text_x1, text_y2);
    glVertex2i(vertex_x1, vertex_y2);
    glTexCoord2f(text_x2, text_y2);
    glVertex2i(vertex_x2, vertex_y2);
    glTexCoord2f(text_x2, text_y1);
    glVertex2i(vertex_x2, vertex_y1);
    glEnd();

    if (rendertype == RENDER_TYPE_FBO) {
        GLuint fbo;

        fbo = source->gl.fbo;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    } else {
        pbuffer = source->gl.pbuff;
        glXMakeCurrent(XPlatform::get_display(), pbuffer, context);
    }
}

static inline void copy_to_gldrawable_from_pixmap(const RedDrawable_p* dest,
                                                  const Rect& area,
                                                  const Point& offset,
                                                  const PixelsSource_p* source,
                                                  int src_x, int src_y)
{
    uint8_t *addr;
    GLXContext context = NULL;
    GLXPbuffer pbuffer;
    RenderType rendertype;
    Window win;

    rendertype = dest->source.x_drawable.rendertype;
    if (rendertype == RENDER_TYPE_FBO) {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        context = dest->source.x_drawable.context;
        win = dest->source.x_drawable.drawable;
        glXMakeCurrent(XPlatform::get_display(), win, context);
        glDisable(GL_TEXTURE_2D);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, source->pixmap.x_image->bytes_per_line / 4);

    glPixelZoom(1, -1);
    addr = (uint8_t *)source->pixmap.x_image->data;
    addr += (src_x * 4 + src_y * source->pixmap.x_image->bytes_per_line);
    glWindowPos2i(area.left + offset.x, dest->source.x_drawable.height -
                  (area.top + offset.y));  //+ (area.bottom - area.top)));
    glDrawPixels(area.right - area.left, area.bottom - area.top,
                 GL_BGRA, GL_UNSIGNED_BYTE, addr);

    if (rendertype == RENDER_TYPE_FBO) {
        GLuint fbo;

        fbo = dest->source.x_drawable.fbo;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    } else {
        pbuffer = dest->source.x_drawable.pbuff;
        glXMakeCurrent(XPlatform::get_display(), pbuffer, context);
    }
}

static inline void copy_to_drawable_from_drawable(const RedDrawable_p* dest,
                                                  const Rect& area,
                                                  const Point& offset,
                                                  const PixelsSource_p* source,
                                                  int src_x, int src_y)
{
    XGCValues gc_vals;
    gc_vals.function = GXcopy;

    XChangeGC(XPlatform::get_display(), dest->source.x_drawable.gc, GCFunction, &gc_vals);
    XCopyArea(XPlatform::get_display(), source->x_drawable.drawable,
              dest->source.x_drawable.drawable, dest->source.x_drawable.gc,
              src_x, src_y,
              area.right - area.left, area.bottom - area.top,
              area.left + offset.x, area.top + offset.y);
}

static inline void copy_to_drawable_from_pixmap(const RedDrawable_p* dest,
                                                const Rect& area,
                                                const Point& offset,
                                                const PixelsSource_p* source,
                                                int src_x, int src_y)
{
    XGCValues gc_vals;
    gc_vals.function = GXcopy;

    XChangeGC(XPlatform::get_display(), dest->source.x_drawable.gc, GCFunction, &gc_vals);
    XPutImage(XPlatform::get_display(), dest->source.x_drawable.drawable,
              dest->source.x_drawable.gc, source->pixmap.x_image, src_x,
              src_y, area.left + offset.x, area.top + offset.y,
              area.right - area.left, area.bottom - area.top);
}

static inline void copy_to_drawable_from_shmdrawable(const RedDrawable_p* dest,
                                                     const Rect& area,
                                                     const Point& offset,
                                                     const PixelsSource_p* source,
                                                     int src_x, int src_y)
{
    XGCValues gc_vals;
    gc_vals.function = GXcopy;

    XChangeGC(XPlatform::get_display(), dest->source.x_drawable.gc, GCFunction, &gc_vals);
    XShmPutImage(XPlatform::get_display(), dest->source.x_drawable.drawable,
                 dest->source.x_drawable.gc, source->x_shm_drawable.x_image,
                 src_x, src_y, area.left + offset.x, area.top + offset.y,
                 area.right - area.left, area.bottom - area.top, false);
    XSync(XPlatform::get_display(), 0);
}

static inline void copy_to_x_drawable(const RedDrawable_p* dest,
                                      const Rect& area,
                                      const Point& offset,
                                      const PixelsSource_p* source,
                                      int src_x, int src_y)
{
    switch (source->type) {
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        copy_to_drawable_from_drawable(dest, area, offset, source, src_x, src_y);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        copy_to_drawable_from_pixmap(dest, area, offset, source, src_x, src_y);
        break;
    case PIXELS_SOURCE_TYPE_XSHM_DRAWABLE:
        copy_to_drawable_from_shmdrawable(dest, area, offset, source, src_x, src_y);
        break;
    default:
        THROW("invalid source type %d", source->type);
    }
}

static inline void copy_to_gl_drawable(const RedDrawable_p* dest,
                                       const Rect& area,
                                       const Point& offset,
                                       const PixelsSource_p* source,
                                       int src_x, int src_y)
{
    switch (source->type) {
    case PIXELS_SOURCE_TYPE_GL_TEXTURE:
        copy_to_gldrawable_from_gltexture(dest, area, offset, source, src_x, src_y);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        copy_to_gldrawable_from_pixmap(dest, area, offset, source, src_x, src_y);
        break;
    default:
        THROW("invalid source type %d", source->type);
    }
}

static inline void copy_to_pixmap_from_drawable(const RedDrawable_p* dest,
                                                const Rect& area,
                                                const Point& offset,
                                                const PixelsSource_p* source,
                                                int src_x, int src_y)
{
    LOG_WARN("not implemented");
}

static inline void copy_to_pixmap_from_shmdrawable(const RedDrawable_p* dest,
                                                   const Rect& area,
                                                   const Point& offset,
                                                   const PixelsSource_p* source,
                                                   int src_x, int src_y)
{
    cairo_t* cairo = dest->cairo;
    cairo_surface_t* surf = source->x_shm_drawable.cairo_surf;

    ASSERT(cairo);
    ASSERT(surf);

    cairo_set_source_surface(cairo, surf, area.left + offset.x - src_x,
                             area.top + offset.y - src_y);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_COPY);
    cairo_rectangle(cairo, area.left + offset.x, area.top + offset.y, area.right - area.left,
                    area.bottom - area.top);
    cairo_fill(cairo);
}

static inline void copy_to_pixmap_from_pixmap(const RedDrawable_p* dest,
                                              const Rect& area,
                                              const Point& offset,
                                              const PixelsSource_p* source,
                                              int src_x, int src_y)
{
    cairo_t* cairo = dest->cairo;
    cairo_surface_t* surf = source->pixmap.cairo_surf;

    ASSERT(cairo);
    ASSERT(surf);

    cairo_set_source_surface(cairo, surf, area.left + offset.x - src_x,
                             area.top + offset.y - src_y);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_COPY);
    cairo_rectangle(cairo, area.left + offset.x, area.top + offset.y, area.right - area.left,
                    area.bottom - area.top);
    cairo_fill(cairo);
}

static inline void copy_to_pixmap_from_gltexture(const RedDrawable_p* dest,
                                                 const Rect& area,
                                                 const Point& offset,
                                                 const PixelsSource_p* source,
                                                 int src_x, int src_y)
{
    int y, height;
    GLXContext context = NULL;
    GLXPbuffer pbuffer;
    Window win;
    RenderType rendertype;

    y = source->gl.height - src_y;
    height = area.bottom - area.top;

    rendertype = source->gl.rendertype;
    if (rendertype == RENDER_TYPE_FBO) {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, source->gl.fbo);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        context = source->gl.context;
        pbuffer = source->gl.pbuff;
        glXMakeCurrent(XPlatform::get_display(), pbuffer, context);
        glDisable(GL_TEXTURE_2D);
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glPixelStorei(GL_PACK_ROW_LENGTH,
                  dest->source.pixmap.x_image->bytes_per_line / 4);

    while (height > 0) {
        glReadPixels(src_x, y - height, area.right - area.left, 1,
                     GL_BGRA, GL_UNSIGNED_BYTE,
                     dest->source.pixmap.x_image->data +
                     (area.left + offset.x) * 4 +
                     (area.top + offset.y + height - 1) *
                     dest->source.pixmap.x_image->bytes_per_line);
        height--;
    }
    if (rendertype != RENDER_TYPE_FBO) {
        win = source->gl.win;
        glXMakeCurrent(XPlatform::get_display(), win, context);
    }
}

static inline void copy_to_pixmap(const RedDrawable_p* dest,
                                  const Rect& area,
                                  const Point& offset,
                                  const PixelsSource_p* source,
                                  int src_x, int src_y)
{
    switch (source->type) {
    case PIXELS_SOURCE_TYPE_GL_TEXTURE:
        copy_to_pixmap_from_gltexture(dest, area, offset, source, src_x, src_y);
        break;
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        copy_to_pixmap_from_drawable(dest, area, offset, source, src_x, src_y);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        copy_to_pixmap_from_pixmap(dest, area, offset, source, src_x, src_y);
        break;
    case PIXELS_SOURCE_TYPE_XSHM_DRAWABLE:
        copy_to_pixmap_from_shmdrawable(dest, area, offset, source, src_x, src_y);
        break;
    default:
        THROW("invalid source type %d", source->type);
    }
}

void RedDrawable::copy_pixels(const PixelsSource& src, int src_x, int src_y, const Rect& area)
{
    PixelsSource_p* source = (PixelsSource_p*)src.get_opaque();
    RedDrawable_p* dest = (RedDrawable_p*)get_opaque();
    switch (dest->source.type) {
    case PIXELS_SOURCE_TYPE_GL_DRAWABLE:
        copy_to_gl_drawable(dest, area, _origin, source, src_x + src._origin.x,
                            src_y + src._origin.y);
        break;
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        copy_to_x_drawable(dest, area, _origin, source, src_x + src._origin.x,
                           src_y + src._origin.y);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        copy_to_pixmap(dest, area, _origin, source, src_x + src._origin.x, src_y + src._origin.y);
        break;
    default:
        THROW("invalid dest type %d", dest->source.type);
    }
}

static inline void blend_to_drawable(const RedDrawable_p* dest,
                                     const Rect& area,
                                     const Point& offset,
                                     const PixelsSource_p* source,
                                     int src_x, int src_y)
{
    LOG_WARN("not implemented");
}

static inline void blend_to_pixmap_from_drawable(const RedDrawable_p* dest,
                                                 const Rect& area,
                                                 const Point& offset,
                                                 const PixelsSource_p* source,
                                                 int src_x, int src_y)
{
    LOG_WARN("not implemented");
}

static inline void blend_to_pixmap_from_pixmap(const RedDrawable_p* dest,
                                               const Rect& area,
                                               const Point& offset,
                                               const PixelsSource_p* source,
                                               int src_x, int src_y)
{
    cairo_t* cairo = dest->cairo;
    cairo_surface_t* surf = source->pixmap.cairo_surf;

    ASSERT(cairo);
    ASSERT(surf);

    cairo_set_source_surface(cairo, surf, (area.left + offset.x - src_x),
                             (area.top + offset.y - src_y));
    cairo_set_operator(cairo, CAIRO_OPERATOR_ATOP);
    cairo_rectangle(cairo, area.left + offset.x, area.top + offset.y, area.right - area.left,
                    area.bottom - area.top);
    cairo_fill(cairo);
}

static inline void blend_to_pixmap(const RedDrawable_p* dest,
                                   const Rect& area,
                                   const Point& offset,
                                   const PixelsSource_p* source,
                                   int src_x, int src_y)
{
    switch (source->type) {
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        blend_to_pixmap_from_drawable(dest, area, offset, source, src_x, src_y);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        blend_to_pixmap_from_pixmap(dest, area, offset, source, src_x, src_y);
        break;
    default:
        THROW("invalid source type %d", source->type);
    }
}

void RedDrawable::blend_pixels(const PixelsSource& src, int src_x, int src_y, const Rect& area)
{
    PixelsSource_p* source = (PixelsSource_p*)src.get_opaque();
    RedDrawable_p* dest = (RedDrawable_p*)get_opaque();
    switch (dest->source.type) {
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        blend_to_drawable(dest, area, _origin, source, src_x + src._origin.x,
                          src_y + src._origin.y);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        blend_to_pixmap(dest, area, _origin, source, src_x + src._origin.x, src_y + src._origin.y);
        break;
    default:
        THROW("invalid dest type %d", dest->source.type);
    }
}

static inline void combine_to_drawable(const RedDrawable_p* dest,
                                       const Rect& area,
                                       const Point& offset,
                                       const PixelsSource_p* source,
                                       int src_x, int src_y,
                                       RedDrawable::CombineOP op)
{
    LOG_WARN("not implemented");
}

static inline void combine_to_pixmap_from_drawable(const RedDrawable_p* dest,
                                                   const Rect& area,
                                                   const Point& offset,
                                                   const PixelsSource_p* source,
                                                   int src_x, int src_y,
                                                   RedDrawable::CombineOP op)
{
    LOG_WARN("not implemented");
}

static inline void combine_to_pixmap_from_pixmap(const RedDrawable_p* dest,
                                                 const Rect& area,
                                                 const Point& offset,
                                                 const PixelsSource_p* source,
                                                 int src_x, int src_y,
                                                 RedDrawable::CombineOP op)
{
    cairo_t* cairo = dest->cairo;
    cairo_surface_t* surf = source->pixmap.cairo_surf;

    ASSERT(cairo);
    ASSERT(surf);
    cairo_operator_t cairo_op;
    switch (op) {
    case RedDrawable::OP_COPY:
        cairo_op = CAIRO_OPERATOR_RASTER_COPY;
        break;
    case RedDrawable::OP_AND:
        cairo_op = CAIRO_OPERATOR_RASTER_AND;
        break;
    case RedDrawable::OP_XOR:
        cairo_op = CAIRO_OPERATOR_RASTER_XOR;
        break;
    default:
        THROW("invalid op %d", op);
    }


    cairo_set_operator(cairo, cairo_op);
    if (cairo_image_surface_get_format(surf) == CAIRO_FORMAT_A1) {
        cairo_rectangle(cairo, area.left + offset.x, area.top + offset.y, area.right - area.left,
                        area.bottom - area.top);
        cairo_clip(cairo);
        cairo_set_source_rgb(cairo, 1, 1, 1);
        cairo_mask_surface(cairo, surf, area.left + offset.x - src_x, area.top + offset.y - src_y);
        cairo_reset_clip(cairo);
    } else {
        cairo_set_source_surface(cairo, surf, area.left + offset.x - src_x,
                                 area.top + offset.y - src_y);
        cairo_rectangle(cairo, area.left + offset.x, area.top + offset.y, area.right - area.left,
                        area.bottom - area.top);
        cairo_fill(cairo);
    }
}

static inline void combine_to_pixmap(const RedDrawable_p* dest,
                                     const Rect& area,
                                     const Point& offset,
                                     const PixelsSource_p* source,
                                     int src_x, int src_y,
                                     RedDrawable::CombineOP op)
{
    switch (source->type) {
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        combine_to_pixmap_from_drawable(dest, area, offset, source, src_x, src_y, op);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        combine_to_pixmap_from_pixmap(dest, area, offset, source, src_x, src_y, op);
        break;
    default:
        THROW("invalid source type %d", source->type);
    }
}

void RedDrawable::combine_pixels(const PixelsSource& src, int src_x, int src_y, const Rect& area,
                                 CombineOP op)
{
    PixelsSource_p* source = (PixelsSource_p*)src.get_opaque();
    RedDrawable_p* dest = (RedDrawable_p*)get_opaque();
    switch (dest->source.type) {
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        combine_to_drawable(dest, area, _origin, source, src_x + src._origin.x,
                            src_y + src._origin.y, op);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        combine_to_pixmap(dest, area, _origin, source, src_x + src._origin.x,
                          src_y + src._origin.y, op);
        break;
    default:
        THROW("invalid dest type %d", dest->source.type);
    }
}

void RedDrawable::erase_rect(const Rect& area, rgb32_t color)
{
    LOG_WARN("not implemented");
}

static inline void fill_drawable(RedDrawable_p* dest, const Rect& area, rgb32_t color,
                                 const Point& offset)
{
    Drawable drawable = dest->source.x_drawable.drawable;
    GC gc = dest->source.x_drawable.gc;

    Colormap color_map = DefaultColormap(XPlatform::get_display(),
                                         DefaultScreen(XPlatform::get_display()));
    XColor x_color;
    x_color.red = (uint16_t)rgb32_get_red(color) << 8;
    x_color.green = (uint16_t)rgb32_get_green(color) << 8;
    x_color.blue = (uint16_t)rgb32_get_blue(color) << 8;
    x_color.flags = DoRed | DoGreen | DoBlue;
    //todo: optimize color map
    if (!XAllocColor(XPlatform::get_display(), color_map, &x_color)) {
        LOG_WARN("color map failed");
    }

    XGCValues gc_vals;
    gc_vals.foreground = x_color.pixel;
    gc_vals.function = GXcopy;
    gc_vals.fill_style = FillSolid;
    XChangeGC(XPlatform::get_display(), gc, GCFunction | GCForeground | GCFillStyle, &gc_vals);
    XFillRectangle(XPlatform::get_display(), drawable,
                   gc, area.left + offset.x, area.top + offset.y,
                   area.right - area.left, area.bottom - area.top);
}

static inline void fill_gl_drawable(RedDrawable_p* dest, const Rect& area, rgb32_t color,
                                    const Point& offset)
{
    int vertex_x1, vertex_x2;
    int vertex_y1, vertex_y2;
    GLXContext context;

    context = glXGetCurrentContext();
    if (!context) {
        return;
    }

    vertex_x1 = area.left + offset.x;
    vertex_y1 = dest->source.x_drawable.height - (area.top + offset.y) - (area.bottom - area.top);

    vertex_x2 = vertex_x1 + (area.right - area.left);
    vertex_y2 = vertex_y1 + (area.bottom - area.top);

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

    glColor3f(rgb32_get_red(color), rgb32_get_green(color),
              rgb32_get_blue(color));

    glBegin(GL_QUADS);
    glVertex2i(vertex_x1, vertex_y1);
    glVertex2i(vertex_x1, vertex_y2);
    glVertex2i(vertex_x2, vertex_y2);
    glVertex2i(vertex_x2, vertex_y1);
    glEnd();
    glFlush();

    glColor3f(1, 1, 1);
}

static inline void fill_pixmap(RedDrawable_p* dest, const Rect& area, rgb32_t color,
                               const Point& offset)
{
    cairo_t* cairo = dest->cairo;

    ASSERT(cairo);
    cairo_set_source_rgb(cairo,
                         (double)rgb32_get_red(color) / 0xff,
                         (double)rgb32_get_green(color) / 0xff,
                         (double)rgb32_get_blue(color) / 0xff);
    cairo_rectangle(cairo, area.left + offset.x, area.top + offset.y, area.right - area.left,
                    area.bottom - area.top);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_COPY);
    cairo_fill(cairo);
}

void RedDrawable::fill_rect(const Rect& area, rgb32_t color)
{
    RedDrawable_p* dest = (RedDrawable_p*)get_opaque();
    switch (dest->source.type) {
    case PIXELS_SOURCE_TYPE_GL_DRAWABLE:
        fill_gl_drawable(dest, area, color, _origin);
        break;
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        fill_drawable(dest, area, color, _origin);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        fill_pixmap(dest, area, color, _origin);
        break;
    default:
        THROW("invalid dest type %d", dest->source.type);
    }
}

static inline void frame_drawable(RedDrawable_p* dest, const Rect& area, rgb32_t color,
                                  const Point& offset)
{
    Drawable drawable = dest->source.x_drawable.drawable;
    GC gc = dest->source.x_drawable.gc;

    Colormap color_map = DefaultColormap(XPlatform::get_display(),
                                         DefaultScreen(XPlatform::get_display()));
    XColor x_color;
    x_color.red = (uint16_t)rgb32_get_red(color) << 8;
    x_color.green = (uint16_t)rgb32_get_green(color) << 8;
    x_color.blue = (uint16_t)rgb32_get_blue(color) << 8;
    x_color.flags = DoRed | DoGreen | DoBlue;
    //todo: optimize color map
    if (!XAllocColor(XPlatform::get_display(), color_map, &x_color)) {
        LOG_WARN("color map failed");
    }

    XGCValues gc_vals;
    gc_vals.foreground = x_color.pixel;
    gc_vals.function = GXcopy;
    gc_vals.fill_style = FillSolid;
    XChangeGC(XPlatform::get_display(), gc, GCFunction | GCForeground | GCFillStyle, &gc_vals);
    XFillRectangle(XPlatform::get_display(), drawable,
                   gc, area.left + offset.x, area.top + offset.y,
                   area.right - area.left, area.bottom - area.top);
}

static inline void frame_pixmap(RedDrawable_p* dest, const Rect& area, rgb32_t color,
                                const Point& offset)
{
    cairo_t* cairo = dest->cairo;

    ASSERT(cairo);
    cairo_set_source_rgb(cairo,
                         (double)rgb32_get_red(color) / 0xff,
                         (double)rgb32_get_green(color) / 0xff,
                         (double)rgb32_get_blue(color) / 0xff);
    cairo_rectangle(cairo, area.left + offset.x, area.top + offset.y, area.right - area.left,
                    area.bottom - area.top);
    cairo_set_line_width(cairo, 1);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_COPY);
    cairo_stroke(cairo);
}

void RedDrawable::frame_rect(const Rect& area, rgb32_t color)
{
    RedDrawable_p* dest = (RedDrawable_p*)get_opaque();
    switch (dest->source.type) {
    case PIXELS_SOURCE_TYPE_X_DRAWABLE:
        frame_drawable(dest, area, color, _origin);
        break;
    case PIXELS_SOURCE_TYPE_PIXMAP:
        frame_pixmap(dest, area, color, _origin);
        break;
    default:
        THROW("invalid dest type %d", dest->source.type);
    }
}


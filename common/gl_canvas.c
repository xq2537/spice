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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_canvas.h"
#include "quic.h"
#include "rop3.h"
#include "region.h"

#define GL_CANVAS
#include "canvas_base.c"

struct GLCanvas {
    CanvasBase base;
    GLCCtx glc;
    void *usr_data;
    void *private_data;
    int private_data_size;
};

static inline uint8_t *copy_opposite_image(GLCanvas *canvas, void *data, int stride, int height)
{
    uint8_t *ret_data = (uint8_t *)data;
    uint8_t *dest;
    uint8_t *src;
    int i;

    if (!canvas->private_data) {
        canvas->private_data = malloc(stride * height);
        if (!canvas->private_data) {
            return ret_data;
        }
        canvas->private_data_size = stride * height;
    }

    if (canvas->private_data_size < (stride * height)) {
        free(canvas->private_data);
        canvas->private_data = malloc(stride * height);
        if (!canvas->private_data) {
            return ret_data;
        }
        canvas->private_data_size = stride * height;
    }

    dest = (uint8_t *)canvas->private_data;
    src = (uint8_t *)data + (height - 1) * stride;

    for (i = 0; i < height; ++i) {
        memcpy(dest, src, stride);
        dest += stride;
        src -= stride;
    }
    return (uint8_t *)canvas->private_data;
}

static cairo_surface_t *canvas_surf_to_trans_surf(GLCImage *image,
                                                  uint32_t trans_color)
{
    int width = image->width;
    int height = image->height;
    uint8_t *src_line;
    uint8_t *end_src_line;
    int src_stride;
    uint8_t *dest_line;
    int dest_stride;
    cairo_surface_t *ret;
    int i;

    ret = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(ret) != CAIRO_STATUS_SUCCESS) {
        CANVAS_ERROR("create surface failed, %s",
                     cairo_status_to_string(cairo_surface_status(ret)));
    }

    src_line = image->pixels;
    src_stride = image->stride;
    end_src_line = src_line + src_stride * height;

    dest_line = cairo_image_surface_get_data(ret);
    dest_stride = cairo_image_surface_get_stride(ret);

    for (; src_line < end_src_line; src_line += src_stride, dest_line += dest_stride) {
        for (i = 0; i < width; i++) {
            if ((((uint32_t*)src_line)[i] & 0x00ffffff) == trans_color) {
                ((uint32_t*)dest_line)[i] = 0;
            } else {
                ((uint32_t*)dest_line)[i] = (((uint32_t*)src_line)[i]) | 0xff000000;
            }
        }
    }

    return ret;
}

static GLCPath get_path(GLCanvas *canvas, void *addr)
{
    GLCPath path = glc_path_create(canvas->glc);
    uint32_t* data_size = (uint32_t*)addr;
    access_test(&canvas->base, data_size, sizeof(uint32_t));
    uint32_t more = *data_size;

    PathSeg* seg = (PathSeg*)(data_size + 1);

    do {
        access_test(&canvas->base, seg, sizeof(PathSeg));

        uint32_t flags = seg->flags;
        PointFix* point = (PointFix*)seg->data;
        PointFix* end_point = point + seg->count;
        access_test(&canvas->base, point, (unsigned long)end_point - (unsigned long)point);
        ASSERT(point < end_point);
        more -= ((unsigned long)end_point - (unsigned long)seg);
        seg = (PathSeg*)end_point;

        if (flags & PATH_BEGIN) {
            glc_path_move_to(path, fix_to_double(point->x), fix_to_double(point->y));
            point++;
        }

        if (flags & PATH_BEZIER) {
            ASSERT((point - end_point) % 3 == 0);
            for (; point + 2 < end_point; point += 3) {
                glc_path_curve_to(path,
                                  fix_to_double(point[0].x), fix_to_double(point[0].y),
                                  fix_to_double(point[1].x), fix_to_double(point[1].y),
                                  fix_to_double(point[2].x), fix_to_double(point[2].y));
            }
        } else {
            for (; point < end_point; point++) {
                glc_path_line_to(path, fix_to_double(point->x), fix_to_double(point->y));
            }
        }
        if (flags & PATH_END) {
            if (flags & PATH_CLOSE) {
                glc_path_close(path);
            }
        }
    } while (more);

    return path;
}

#define SET_GLC_RECT(dest, src) {                   \
    (dest)->x = (src)->left;                        \
    (dest)->y = (src)->top;                         \
    (dest)->width = (src)->right - (src)->left;     \
    (dest)->height = (src)->bottom - (src)->top;    \
}

static void set_clip(GLCanvas *canvas, Rect *bbox, Clip *clip)
{
    GLCRect rect;
    glc_clip_reset(canvas->glc);

    switch (clip->type) {
    case CLIP_TYPE_NONE:
        break;
    case CLIP_TYPE_RECTS: {
        uint32_t *n = (uint32_t *)GET_ADDRESS(clip->data);
        access_test(&canvas->base, n, sizeof(uint32_t));
        Rect *now = (Rect *)(n + 1);
        Rect *end = now + *n;
        access_test(&canvas->base, now, (unsigned long)end - (unsigned long)now);

        if (*n == 0) {
            rect.x = rect.y = 0;
            rect.width = rect.height = 0;
            glc_clip_rect(canvas->glc, &rect, GLC_CLIP_OP_SET);
            break;
        } else {
            SET_GLC_RECT(&rect, now);
            glc_clip_rect(canvas->glc, &rect, GLC_CLIP_OP_SET);
        }

        for (now++; now < end; now++) {
            SET_GLC_RECT(&rect, now);
            glc_clip_rect(canvas->glc, &rect, GLC_CLIP_OP_OR);
        }
        break;
    }
    case CLIP_TYPE_PATH: {
        GLCPath path = get_path(canvas, GET_ADDRESS(clip->data));
        glc_clip_path(canvas->glc, path, GLC_CLIP_OP_SET);
        glc_path_destroy(path);
        break;
    }
    default:
        CANVAS_ERROR("invalid clip type");
    }
}

static void set_mask(GLCanvas *canvas, QMask *mask, int x, int y)
{
    cairo_surface_t *surface;

    if (!(surface = canvas_get_mask(&canvas->base, mask))) {
        glc_clear_mask(canvas->glc, GLC_MASK_A);
        return;
    }


    glc_set_mask(canvas->glc, x - mask->pos.x, y - mask->pos.y,
                 cairo_image_surface_get_width(surface),
                 cairo_image_surface_get_height(surface),
                 cairo_image_surface_get_stride(surface),
                 cairo_image_surface_get_data(surface), GLC_MASK_A);
}

static inline void surface_to_image(GLCanvas *canvas, cairo_surface_t *surface, GLCImage *image,
                                    int ignore_stride)
{
    cairo_format_t format = cairo_image_surface_get_format(surface);

    ASSERT(format == CAIRO_FORMAT_ARGB32 || format == CAIRO_FORMAT_RGB24);
    image->format = (format == CAIRO_FORMAT_RGB24) ? GLC_IMAGE_RGB32 : GLC_IMAGE_ARGB32;
    image->width = cairo_image_surface_get_width(surface);
    image->height = cairo_image_surface_get_height(surface);
    image->stride = cairo_image_surface_get_stride(surface);
    image->pixels = cairo_image_surface_get_data(surface);
    image->pallet = NULL;
    if (ignore_stride) {
        return;
    }
    if (image->stride < 0) {
        image->stride = -image->stride;
        image->pixels = image->pixels - (image->height - 1) * image->stride;
    } else {
        image->pixels = copy_opposite_image(canvas, image->pixels, image->stride, image->height);
    }
}

static void set_brush(GLCanvas *canvas, Brush *brush)
{
    switch (brush->type) {
    case BRUSH_TYPE_SOLID: {
        uint32_t color = brush->u.color;
        double r, g, b;

        b = (double)(color & canvas->base.color_mask) / canvas->base.color_mask;
        color >>= canvas->base.color_shift;
        g = (double)(color & canvas->base.color_mask) / canvas->base.color_mask;
        color >>= canvas->base.color_shift;
        r = (double)(color & canvas->base.color_mask) / canvas->base.color_mask;
        glc_set_rgb(canvas->glc, r, g, b);
        break;
    }
    case BRUSH_TYPE_PATTERN: {
        GLCImage image;
        GLCPattern pattern;
        cairo_surface_t *surface;

        surface = canvas_get_image(&canvas->base, brush->u.pattern.pat);
        surface_to_image(canvas, surface, &image, 0);

        pattern = glc_pattern_create(canvas->glc, -brush->u.pattern.pos.x,
                                     -brush->u.pattern.pos.y, &image);

        glc_set_pattern(canvas->glc, pattern);
        glc_pattern_destroy(pattern);
    }
    case BRUSH_TYPE_NONE:
        return;
    default:
        CANVAS_ERROR("invalid brush type");
    }
}

static void set_op(GLCanvas *canvas, UINT16 rop_decriptor)
{
    GLCOp op;

    switch (rop_decriptor) {
    case ROPD_OP_PUT:
        op = GLC_OP_COPY;
        break;
    case ROPD_OP_XOR:
        op = GLC_OP_XOR;
        break;
    case ROPD_OP_BLACKNESS:
        op = GLC_OP_CLEAR;
        break;
    case ROPD_OP_WHITENESS:
        op = GLC_OP_SET;
        break;
    case ROPD_OP_PUT | ROPD_INVERS_BRUSH:
    case ROPD_OP_PUT | ROPD_INVERS_SRC:
        op = GLC_OP_COPY_INVERTED;
        break;
    case ROPD_OP_INVERS:
        op = GLC_OP_INVERT;
        break;
    case ROPD_OP_AND:
        op = GLC_OP_AND;
        break;
    case ROPD_OP_AND | ROPD_INVERS_RES:
        op = GLC_OP_NAND;
        break;
    case ROPD_OP_OR:
        op = GLC_OP_OR;
        break;
    case ROPD_OP_OR | ROPD_INVERS_RES:
        op = GLC_OP_NOR;
        break;
    case ROPD_OP_XOR | ROPD_INVERS_RES:
        op = GLC_OP_EQUIV;
        break;
    case ROPD_OP_AND | ROPD_INVERS_DEST:
        op = GLC_OP_AND_REVERSE;
        break;
    case ROPD_OP_AND | ROPD_INVERS_BRUSH:
    case ROPD_OP_AND | ROPD_INVERS_SRC:
        op = GLC_OP_AND_INVERTED;
        break;
    case ROPD_OP_OR | ROPD_INVERS_DEST:
        op = GLC_OP_OR_REVERSE;
        break;
    case ROPD_OP_OR | ROPD_INVERS_BRUSH:
    case ROPD_OP_OR | ROPD_INVERS_SRC:
        op = GLC_OP_OR_INVERTED;
        break;
    default:
        WARN("GLC_OP_NOOP");
        op = GLC_OP_NOOP;
    }
    glc_set_op(canvas->glc, op);
}

void gl_canvas_draw_fill(GLCanvas *canvas, Rect *bbox, Clip *clip, Fill *fill)
{
    GLCRect rect;
    set_clip(canvas, bbox, clip);
    set_mask(canvas, &fill->mask, bbox->left, bbox->top);
    set_brush(canvas, &fill->brush);
    set_op(canvas, fill->rop_decriptor);
    SET_GLC_RECT(&rect, bbox);

    glc_fill_rect(canvas->glc, &rect);
    glc_flush(canvas->glc);
}

void gl_canvas_draw_copy(GLCanvas *canvas, Rect *bbox, Clip *clip, Copy *copy)
{
    cairo_surface_t *surface;
    GLCRecti src;
    GLCRecti dest;
    GLCImage image;

    set_clip(canvas, bbox, clip);
    set_mask(canvas, &copy->mask, bbox->left, bbox->top);
    set_op(canvas, copy->rop_decriptor);

    //todo: optimize get_imag (use ogl conversion + remove unnecessary copy of 32bpp)
    surface = canvas_get_image(&canvas->base, copy->src_bitmap);
    surface_to_image(canvas, surface, &image, 0);
    SET_GLC_RECT(&dest, bbox);
    SET_GLC_RECT(&src, &copy->src_area);
    glc_draw_image(canvas->glc, &dest, &src, &image, 0, 1);

    cairo_surface_destroy(surface);
    glc_flush(canvas->glc);
}

void gl_canvas_draw_opaque(GLCanvas *canvas, Rect *bbox, Clip *clip, Opaque *opaque)
{
    cairo_surface_t *surface;
    GLCRecti src;
    GLCRecti dest;
    GLCRect fill_rect;
    GLCImage image;

    set_clip(canvas, bbox, clip);
    set_mask(canvas, &opaque->mask, bbox->left, bbox->top);

    glc_set_op(canvas->glc, (opaque->rop_decriptor & ROPD_INVERS_SRC) ? GLC_OP_COPY_INVERTED :
               GLC_OP_COPY);
    surface = canvas_get_image(&canvas->base, opaque->src_bitmap);
    surface_to_image(canvas, surface, &image, 0);
    SET_GLC_RECT(&dest, bbox);
    SET_GLC_RECT(&src, &opaque->src_area);
    glc_draw_image(canvas->glc, &dest, &src, &image, 0, 1);
    cairo_surface_destroy(surface);

    set_brush(canvas, &opaque->brush);
    set_op(canvas, opaque->rop_decriptor & ~ROPD_INVERS_SRC);
    SET_GLC_RECT(&fill_rect, bbox);
    glc_fill_rect(canvas->glc, &fill_rect);

    glc_flush(canvas->glc);
}

void gl_canvas_draw_alpha_blend(GLCanvas *canvas, Rect *bbox, Clip *clip, AlphaBlnd *alpha_blend)
{
    cairo_surface_t *surface;
    GLCRecti src;
    GLCRecti dest;
    GLCImage image;

    set_clip(canvas, bbox, clip);
    glc_clear_mask(canvas->glc, GLC_MASK_A);
    glc_set_op(canvas->glc, GLC_OP_COPY);

    surface = canvas_get_image(&canvas->base, alpha_blend->src_bitmap);
    surface_to_image(canvas, surface, &image, 0);
    SET_GLC_RECT(&dest, bbox);
    SET_GLC_RECT(&src, &alpha_blend->src_area);
    glc_draw_image(canvas->glc, &dest, &src, &image, 0, (double)alpha_blend->alpha / 0xff);

    cairo_surface_destroy(surface);
    glc_flush(canvas->glc);
}

void gl_canvas_draw_blend(GLCanvas *canvas, Rect *bbox, Clip *clip, Blend *blend)
{
    cairo_surface_t *surface;
    GLCRecti src;
    GLCRecti dest;
    GLCImage image;

    set_clip(canvas, bbox, clip);
    set_mask(canvas, &blend->mask, bbox->left, bbox->top);
    set_op(canvas, blend->rop_decriptor);

    surface = canvas_get_image(&canvas->base, blend->src_bitmap);
    SET_GLC_RECT(&dest, bbox);
    SET_GLC_RECT(&src, &blend->src_area);
    surface_to_image(canvas, surface, &image, 0);
    glc_draw_image(canvas->glc, &dest, &src, &image, 0, 1);

    cairo_surface_destroy(surface);
    glc_flush(canvas->glc);
}

void gl_canvas_draw_transparent(GLCanvas *canvas, Rect *bbox, Clip *clip, Transparent *transparent)
{
    cairo_surface_t *surface;
    cairo_surface_t *trans_surf;
    GLCImage image;
    GLCRecti src;
    GLCRecti dest;

    set_clip(canvas, bbox, clip);
    glc_clear_mask(canvas->glc, GLC_MASK_A);
    glc_set_op(canvas->glc, GLC_OP_COPY);

    surface = canvas_get_image(&canvas->base, transparent->src_bitmap);
    surface_to_image(canvas, surface, &image, 0);

    trans_surf = canvas_surf_to_trans_surf(&image, transparent->true_color);
    cairo_surface_destroy(surface);

    surface_to_image(canvas, trans_surf, &image, 1);
    SET_GLC_RECT(&dest, bbox);
    SET_GLC_RECT(&src, &transparent->src_area);
    glc_draw_image(canvas->glc, &dest, &src, &image, 0, 1);

    cairo_surface_destroy(trans_surf);
    glc_flush(canvas->glc);
}

static inline void fill_common(GLCanvas *canvas, Rect *bbox, Clip *clip, QMask * mask, GLCOp op)
{
    GLCRect rect;

    set_clip(canvas, bbox, clip);
    set_mask(canvas, mask, bbox->left, bbox->top);
    glc_set_op(canvas->glc, op);
    SET_GLC_RECT(&rect, bbox);
    glc_fill_rect(canvas->glc, &rect);
}

void gl_canvas_draw_whiteness(GLCanvas *canvas, Rect *bbox, Clip *clip, Whiteness *whiteness)
{
    fill_common(canvas, bbox, clip, &whiteness->mask, GLC_OP_SET);
}

void gl_canvas_draw_blackness(GLCanvas *canvas, Rect *bbox, Clip *clip, Blackness *blackness)
{
    fill_common(canvas, bbox, clip, &blackness->mask, GLC_OP_CLEAR);
}

void gl_canvas_draw_invers(GLCanvas *canvas, Rect *bbox, Clip *clip, Invers *invers)
{
    fill_common(canvas, bbox, clip, &invers->mask, GLC_OP_INVERT);
}

void gl_canvas_draw_rop3(GLCanvas *canvas, Rect *bbox, Clip *clip, Rop3 *rop3)
{
    cairo_surface_t *d;
    cairo_surface_t *s;
    GLCImage image;
    Point src_pos;
    uint8_t *data_opp;
    int src_stride;

    set_clip(canvas, bbox, clip);
    set_mask(canvas, &rop3->mask, bbox->left, bbox->top);

    glc_set_op(canvas->glc, GLC_OP_COPY);

    image.format = GLC_IMAGE_RGB32;
    image.width = bbox->right - bbox->left;
    image.height = bbox->bottom - bbox->top;

    image.pallet = NULL;

    d = cairo_image_surface_create(CAIRO_FORMAT_RGB24, image.width, image.height);
    if (cairo_surface_status(d) != CAIRO_STATUS_SUCCESS) {
        CANVAS_ERROR("create surface failed, %s",
                     cairo_status_to_string(cairo_surface_status(d)));
    }
    image.pixels = cairo_image_surface_get_data(d);
    image.stride = cairo_image_surface_get_stride(d);

    glc_read_pixels(canvas->glc, bbox->left, bbox->top, &image);
    data_opp = copy_opposite_image(canvas, image.pixels,
                                   cairo_image_surface_get_stride(d),
                                   cairo_image_surface_get_height(d));
    memcpy(image.pixels, data_opp,
           cairo_image_surface_get_stride(d) * cairo_image_surface_get_height(d));

    s = canvas_get_image(&canvas->base, rop3->src_bitmap);
    src_stride = cairo_image_surface_get_stride(s);
    if (src_stride > 0) {
        data_opp = copy_opposite_image(canvas, cairo_image_surface_get_data(s),
                                       src_stride, cairo_image_surface_get_height(s));
        memcpy(cairo_image_surface_get_data(s), data_opp,
               src_stride * cairo_image_surface_get_height(s));
    }

    if (!rect_is_same_size(bbox, &rop3->src_area)) {
        cairo_surface_t *scaled_s = canvas_scale_surface(s, &rop3->src_area, image.width,
                                                         image.height, rop3->scale_mode);
        cairo_surface_destroy(s);
        s = scaled_s;
        src_pos.x = 0;
        src_pos.y = 0;
    } else {
        src_pos.x = rop3->src_area.left;
        src_pos.y = rop3->src_area.top;
    }

    if (cairo_image_surface_get_width(s) - src_pos.x < image.width ||
                                  cairo_image_surface_get_height(s) - src_pos.y < image.height) {
        CANVAS_ERROR("bad src bitmap size");
    }

    if (rop3->brush.type == BRUSH_TYPE_PATTERN) {
        cairo_surface_t *p = canvas_get_image(&canvas->base, rop3->brush.u.pattern.pat);
        Point pat_pos;

        pat_pos.x = (bbox->left - rop3->brush.u.pattern.pos.x) % cairo_image_surface_get_width(p);

        pat_pos.y = (bbox->top - rop3->brush.u.pattern.pos.y) % cairo_image_surface_get_height(p);

        //for now (bottom-top)
        if (pat_pos.y < 0) {
            pat_pos.y = cairo_image_surface_get_height(p) + pat_pos.y;
        }
        pat_pos.y = (image.height + pat_pos.y) % cairo_image_surface_get_height(p);
        pat_pos.y = cairo_image_surface_get_height(p) - pat_pos.y;

        do_rop3_with_pattern(rop3->rop3, d, s, &src_pos, p, &pat_pos);
        cairo_surface_destroy(p);
    } else {
        uint32_t color = (canvas->base.color_shift) == 8 ? rop3->brush.u.color :
                                                         canvas_16bpp_to_32bpp(rop3->brush.u.color);
        do_rop3_with_color(rop3->rop3, d, s, &src_pos, color);
    }

    cairo_surface_destroy(s);

    GLCRecti dest;
    GLCRecti src;
    dest.x = bbox->left;
    dest.y = bbox->top;

    image.pixels = copy_opposite_image(canvas, image.pixels, cairo_image_surface_get_stride(d),
                                       cairo_image_surface_get_height(d));

    src.x = src.y = 0;
    dest.width = src.width = image.width;
    dest.height = src.height = image.height;
    glc_draw_image(canvas->glc, &dest, &src, &image, 0, 1);
    cairo_surface_destroy(d);
}

void gl_canvas_draw_stroke(GLCanvas *canvas, Rect *bbox, Clip *clip, Stroke *stroke)
{
    GLCPath path;

    set_clip(canvas, bbox, clip);
    glc_clear_mask(canvas->glc, GLC_MASK_A);
    set_op(canvas, stroke->fore_mode);
    set_brush(canvas, &stroke->brush);

    if (stroke->attr.flags & LINE_ATTR_STYLED) {
        WARN("LINE_ATTR_STYLED");
    }
    glc_set_line_width(canvas->glc, fix_to_double(stroke->attr.width));

    path = get_path(canvas, GET_ADDRESS(stroke->path));
    glc_stroke_path(canvas->glc, path);
    glc_path_destroy(path);
}

void gl_canvas_draw_text(GLCanvas *canvas, Rect *bbox, Clip *clip, Text *text)
{
    GLCRect rect;
    String *str;

    set_clip(canvas, bbox, clip);
    glc_clear_mask(canvas->glc, GLC_MASK_A);

    if (!rect_is_empty(&text->back_area)) {
        set_brush(canvas, &text->back_brush);
        set_op(canvas, text->back_mode);
        SET_GLC_RECT(&rect, bbox);
        glc_fill_rect(canvas->glc, &rect);
    }

    str = (String *)GET_ADDRESS(text->str);
    set_brush(canvas, &text->fore_brush);
    set_op(canvas, text->fore_mode);
    if (str->flags & STRING_RASTER_A1) {
        Point pos;
        cairo_surface_t *mask = canvas_get_str_mask(&canvas->base, str, 1, &pos);
        _glc_fill_mask(canvas->glc, pos.x, pos.y,
                       cairo_image_surface_get_width(mask),
                       cairo_image_surface_get_height(mask),
                       cairo_image_surface_get_stride(mask),
                       cairo_image_surface_get_data(mask));
        cairo_surface_destroy(mask);
    } else if (str->flags & STRING_RASTER_A4) {
        Point pos;
        cairo_surface_t *mask = canvas_get_str_mask(&canvas->base, str, 4, &pos);
        glc_fill_alpha(canvas->glc, pos.x, pos.y,
                       cairo_image_surface_get_width(mask),
                       cairo_image_surface_get_height(mask),
                       cairo_image_surface_get_stride(mask),
                       cairo_image_surface_get_data(mask));

        cairo_surface_destroy(mask);
    } else if (str->flags & STRING_RASTER_A8) {
        WARN("untested path A8 glyphs, doing nothing");
        if (0) {
            Point pos;
            cairo_surface_t *mask = canvas_get_str_mask(&canvas->base, str, 8, &pos);
            glc_fill_alpha(canvas->glc, pos.x, pos.y,
                           cairo_image_surface_get_width(mask),
                           cairo_image_surface_get_height(mask),
                           cairo_image_surface_get_stride(mask),
                           cairo_image_surface_get_data(mask));
            cairo_surface_destroy(mask);
        }
    } else {
        WARN("untested path vector glyphs, doing nothing");
        if (0) {
            //draw_vector_str(canvas, str, &text->fore_brush, text->fore_mode);
        }
    }
    glc_flush(canvas->glc);
}

void gl_canvas_clear(GLCanvas *canvas)
{
    glc_clear(canvas->glc);
    glc_flush(canvas->glc);
}

void gl_canvas_copy_pixels(GLCanvas *canvas, Rect *bbox, Clip *clip, Point *src_pos)
{
    set_clip(canvas, bbox, clip);
    glc_clear_mask(canvas->glc, GLC_MASK_A);
    glc_set_op(canvas->glc, GLC_OP_COPY);
    glc_copy_pixels(canvas->glc, bbox->left, bbox->top, src_pos->x, src_pos->y,
                    bbox->right - bbox->left, bbox->bottom - bbox->top);
}

void gl_canvas_read_pixels(GLCanvas *canvas, uint8_t *dest, int dest_stride, const Rect *area)
{
    GLCImage image;

    ASSERT(dest_stride > 0);
    image.format = GLC_IMAGE_RGB32;
    image.height = area->bottom - area->top;
    image.width = area->right - area->left;
    image.pixels = dest;
    image.stride = dest_stride;
    glc_read_pixels(canvas->glc, area->left, area->top, &image);
}

void gl_canvas_set_top_mask(GLCanvas *canvas, int num_rect, const Rect *rects)
{
    GLCRect *glc_rects = (GLCRect *)malloc(num_rect * sizeof(GLCRect));
    GLCRect *now = glc_rects;
    GLCRect *end = glc_rects + num_rect;

    for (; now < end; now++, rects++) {
        SET_GLC_RECT(now, rects);
    }
    glc_mask_rects(canvas->glc, num_rect, glc_rects, GLC_MASK_B);

    free(glc_rects);
}

void gl_canvas_put_image(GLCanvas *canvas, const Rect *dest, const uint8_t *src_data,
                         uint32_t src_width, uint32_t src_height, int src_stride,
                         const QRegion *clip)
{
    GLCRecti src;
    GLCRecti gldest;
    GLCImage image;
    uint32_t i;

    ASSERT(src_stride <= 0)
    glc_clip_reset(canvas->glc);

    if (clip) {
        GLCRect rect;
        if (clip->num_rects == 0) {
            rect.x = rect.y = rect.width = rect.height = 0;
            glc_clip_rect(canvas->glc, &rect, GLC_CLIP_OP_SET);
        } else {
            SET_GLC_RECT(&rect, clip->rects);
            glc_clip_rect(canvas->glc, &rect, GLC_CLIP_OP_SET);
            for (i = 1; i < clip->num_rects; i++) {
                SET_GLC_RECT(&rect, clip->rects + i);
                glc_clip_rect(canvas->glc, &rect, GLC_CLIP_OP_OR);
            }
        }
    }

    SET_GLC_RECT(&gldest, dest);
    src.x = src.y = 0;
    src.width = src_width;
    src.height = src_height;

    image.format = GLC_IMAGE_RGB32;
    image.width = src_width;
    image.height = src_height;
    src_stride = -src_stride;
    image.stride = src_stride;
    image.pixels = (uint8_t *)src_data - (src_height - 1) * src_stride;
    image.pallet = NULL;
    glc_draw_image(canvas->glc, &gldest, &src, &image, 0, 1);

    glc_flush(canvas->glc);
}

void gl_canvas_clear_top_mask(GLCanvas *canvas)
{
    glc_clear_mask(canvas->glc, GLC_MASK_B);
}

#ifdef CAIRO_CANVAS_ACCESS_TEST
void gl_canvas_set_access_params(GLCanvas *canvas, unsigned long base, unsigned long max)
{
    __canvas_set_access_params(&canvas->base, base, max);
}

#endif

void *gl_canvas_get_usr_data(GLCanvas *canvas)
{
    return canvas->usr_data;
}

static int need_init = 1;

#ifdef CAIRO_CANVAS_CACHE
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth,
                           void *bits_cache_opaque,
                           bits_cache_put_fn_t bits_cache_put,
                           bits_cache_get_fn_t bits_cache_get,
                           void *palette_cache_opaque,
                           palette_cache_put_fn_t palette_cache_put,
                           palette_cache_get_fn_t palette_cache_get,
                           palette_cache_release_fn_t palette_cache_release
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth,
                           void *bits_cache_opaque,
                           bits_cache_put_fn_t bits_cache_put,
                           bits_cache_get_fn_t bits_cache_get
#else
GLCanvas *gl_canvas_create(void *usr_data, int width, int height, int depth
#endif
#ifdef USE_GLZ
                           , void *glz_decoder_opaque, glz_decode_fn_t glz_decode
#endif
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , void *get_virt_opaque, get_virt_fn_t get_virt,
                           void *validate_virt_opaque, validate_virt_fn_t validate_virt
#endif
                           )
{
    GLCanvas *canvas;
    int init_ok;

    if (need_init || !(canvas = (GLCanvas *)malloc(sizeof(GLCanvas)))) {
        return NULL;
    }
    memset(canvas, 0, sizeof(GLCanvas));

    if (!(canvas->glc = glc_create(width, height))) {
        goto error_1;
    }
    canvas->usr_data = usr_data;
    canvas->private_data = NULL;
#ifdef CAIRO_CANVAS_CACHE
    init_ok = canvas_base_init(&canvas->base, depth,
                               bits_cache_opaque,
                               bits_cache_put,
                               bits_cache_get,
                               palette_cache_opaque,
                               palette_cache_put,
                               palette_cache_get,
                               palette_cache_release
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
    init_ok = canvas_base_init(&canvas->base, depth,
                               bits_cache_opaque,
                               bits_cache_put,
                               bits_cache_get
#else
    init_ok = canvas_base_init(&canvas->base, depth
#endif
#ifdef USE_GLZ
                               ,
                               glz_decoder_opaque,
                               glz_decode
#endif
#ifndef CAIRO_CANVAS_NO_CHUNKS
                               ,
                               get_virt_opaque,
                               get_virt,
                               validate_virt_opaque,
                               validate_virt
#endif
                               );
    if (!init_ok) {
        goto error_2;
    }

    return canvas;

error_2:
    glc_destroy(canvas->glc, 0);
error_1:
    free(canvas);

    return NULL;
}

void gl_canvas_destroy(GLCanvas *canvas, int textures_lost)
{
    if (!canvas) {
        return;
    }
    canvas_base_destroy(&canvas->base);
    glc_destroy(canvas->glc, textures_lost);
    if (canvas->private_data) {
        free(canvas->private_data);
    }
    free(canvas);
}

void gl_canvas_init() //unsafe global function
{
    if (!need_init) {
        return;
    }
    need_init = 0;
    rop3_init();
}

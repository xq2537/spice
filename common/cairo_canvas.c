/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#include <math.h>
#include "cairo_canvas.h"
#define CANVAS_USE_PIXMAN
#include "canvas_base.c"
#include "rop3.h"
#include "rect.h"
#include "region.h"
#include "pixman_utils.h"

#define ROUND(_x) floor((_x) + 0.5)

struct CairoCanvas {
    CanvasBase base;
    cairo_t *cairo;
    uint32_t *private_data;
    int private_data_size;
    pixman_image_t *image;
    pixman_region32_t canvas_region;
};

typedef enum {
    ROP_INPUT_SRC,
    ROP_INPUT_BRUSH,
    ROP_INPUT_DEST
} ROPInput;

SpiceROP ropd_descriptor_to_rop(int desc,
                                ROPInput src_input,
                                ROPInput dest_input)
{
    int old;
    int invert_masks[] = {
        SPICE_ROPD_INVERS_SRC,
        SPICE_ROPD_INVERS_BRUSH,
        SPICE_ROPD_INVERS_DEST
    };

    old = desc;

    desc &= ~(SPICE_ROPD_INVERS_SRC | SPICE_ROPD_INVERS_DEST);
    if (old & invert_masks[src_input]) {
        desc |= SPICE_ROPD_INVERS_SRC;
    }

    if (old & invert_masks[dest_input]) {
        desc |= SPICE_ROPD_INVERS_DEST;
    }

    if (desc & SPICE_ROPD_OP_PUT) {
        if (desc & SPICE_ROPD_INVERS_SRC) {
            if (desc & SPICE_ROPD_INVERS_RES) {
                return SPICE_ROP_COPY;
            }
            return SPICE_ROP_COPY_INVERTED;
        } else {
            if (desc & SPICE_ROPD_INVERS_RES) {
                return SPICE_ROP_COPY_INVERTED;
            }
            return SPICE_ROP_COPY;
        }
    } else if (desc & SPICE_ROPD_OP_OR) {

        if (desc & SPICE_ROPD_INVERS_RES) {
            if (desc & SPICE_ROPD_INVERS_SRC) {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !(!src or !dest) == src and dest*/
                    return SPICE_ROP_AND;
                } else {
                    /* ! (!src or dest) = src and !dest*/
                    return SPICE_ROP_AND_REVERSE;
                }
            } else {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !(src or !dest) == !src and dest */
                    return SPICE_ROP_AND_INVERTED;
                } else {
                    /* !(src or dest) */
                    return SPICE_ROP_NOR;
                }
            }
        } else {
            if (desc & SPICE_ROPD_INVERS_SRC) {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !src or !dest == !(src and dest)*/
                    return SPICE_ROP_NAND;
                } else {
                    /* !src or dest */
                    return SPICE_ROP_OR_INVERTED;
                }
            } else {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* src or !dest */
                    return SPICE_ROP_OR_REVERSE;
                } else {
                    /* src or dest */
                    return SPICE_ROP_OR;
                }
            }
        }

    } else if (desc & SPICE_ROPD_OP_AND) {

        if (desc & SPICE_ROPD_INVERS_RES) {
            if (desc & SPICE_ROPD_INVERS_SRC) {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !(!src and !dest) == src or dest*/
                    return SPICE_ROP_OR;
                } else {
                    /* ! (!src and dest) = src or !dest*/
                    return SPICE_ROP_OR_REVERSE;
                }
            } else {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !(src and !dest) == !src or dest */
                    return SPICE_ROP_OR_INVERTED;
                } else {
                    /* !(src and dest) */
                    return SPICE_ROP_NAND;
                }
            }
        } else {
            if (desc & SPICE_ROPD_INVERS_SRC) {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !src and !dest == !(src or dest)*/
                    return SPICE_ROP_NOR;
                } else {
                    /* !src and dest */
                    return SPICE_ROP_AND_INVERTED;
                }
            } else {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* src and !dest */
                    return SPICE_ROP_AND_REVERSE;
                } else {
                    /* src and dest */
                    return SPICE_ROP_AND;
                }
            }
        }

    } else if (desc & SPICE_ROPD_OP_XOR) {

        if (desc & SPICE_ROPD_INVERS_RES) {
            if (desc & SPICE_ROPD_INVERS_SRC) {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !(!src xor !dest) == !src xor dest */
                    return SPICE_ROP_EQUIV;
                } else {
                    /* ! (!src xor dest) = src xor dest*/
                    return SPICE_ROP_XOR;
                }
            } else {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !(src xor !dest) == src xor dest */
                    return SPICE_ROP_XOR;
                } else {
                    /* !(src xor dest) */
                    return SPICE_ROP_EQUIV;
                }
            }
        } else {
            if (desc & SPICE_ROPD_INVERS_SRC) {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* !src xor !dest == src xor dest */
                    return SPICE_ROP_XOR;
                } else {
                    /* !src xor dest */
                    return SPICE_ROP_EQUIV;
                }
            } else {
                if (desc & SPICE_ROPD_INVERS_DEST) {
                    /* src xor !dest */
                    return SPICE_ROP_EQUIV;
                } else {
                    /* src xor dest */
                    return SPICE_ROP_XOR;
                }
            }
        }

    } else if (desc & SPICE_ROPD_OP_BLACKNESS) {
        return SPICE_ROP_CLEAR;
    } else if (desc & SPICE_ROPD_OP_WHITENESS) {
        return SPICE_ROP_SET;
    } else if (desc & SPICE_ROPD_OP_INVERS) {
        return SPICE_ROP_INVERT;
    }
    return SPICE_ROP_COPY;
}

static void canvas_clip_pixman(CairoCanvas *canvas,
                               pixman_region32_t *dest_region,
                               SpiceClip *clip)
{
    pixman_region32_intersect(dest_region, dest_region, &canvas->canvas_region);

    switch (clip->type) {
    case SPICE_CLIP_TYPE_NONE:
        break;
    case SPICE_CLIP_TYPE_RECTS: {
        uint32_t *n = (uint32_t *)SPICE_GET_ADDRESS(clip->data);
        access_test(&canvas->base, n, sizeof(uint32_t));

        SpiceRect *now = (SpiceRect *)(n + 1);
        access_test(&canvas->base, now, (unsigned long)(now + *n) - (unsigned long)now);

        pixman_region32_t clip;

        if (spice_pixman_region32_init_rects(&clip, now, *n)) {
            pixman_region32_intersect(dest_region, dest_region, &clip);
            pixman_region32_fini(&clip);
        }

        break;
    }
    case SPICE_CLIP_TYPE_PATH:
        CANVAS_ERROR("clip paths not supported anymore");
        break;
    default:
        CANVAS_ERROR("invalid clip type");
    }
}

static void canvas_mask_pixman(CairoCanvas *canvas,
                               pixman_region32_t *dest_region,
                               SpiceQMask *mask, int x, int y)
{
    pixman_image_t *image, *subimage;
    int needs_invert;
    pixman_region32_t mask_region;
    uint32_t *mask_data;
    int mask_x, mask_y;
    int mask_width, mask_height, mask_stride;
    pixman_box32_t extents;

    needs_invert = FALSE;
    image = canvas_get_mask(&canvas->base,
                            mask,
                            &needs_invert);

    if (image == NULL) {
        return; /* no mask */
    }

    mask_data = pixman_image_get_data(image);
    mask_width = pixman_image_get_width(image);
    mask_height = pixman_image_get_height(image);
    mask_stride = pixman_image_get_stride(image);

    mask_x = mask->pos.x;
    mask_y = mask->pos.y;

    /* We need to subset the area of the mask that we turn into a region,
       because a cached mask may be much larger than what is used for
       the clip operation. */
    extents = *pixman_region32_extents(dest_region);

    /* convert from destination pixels to mask pixels */
    extents.x1 -= x - mask_x;
    extents.y1 -= y - mask_y;
    extents.x2 -= x - mask_x;
    extents.y2 -= y - mask_y;

    /* clip to mask size */
    if (extents.x1 < 0) {
        extents.x1 = 0;
    }
    if (extents.x2 >= mask_width) {
        extents.x2 = mask_width;
    }
    if (extents.x2 < extents.x1) {
        extents.x2 = extents.x1;
    }
    if (extents.y1 < 0) {
        extents.y1 = 0;
    }
    if (extents.y2 >= mask_height) {
        extents.y2 = mask_height;
    }
    if (extents.y2 < extents.y1) {
        extents.y2 = extents.y1;
    }

    /* round down X to even 32 pixels (i.e. uint32_t) */
    extents.x1 = extents.x1 & ~(0x1f);

    mask_data = (uint32_t *)((uint8_t *)mask_data + mask_stride * extents.y1 + extents.x1 / 32);
    mask_x -= extents.x1;
    mask_y -= extents.y1;
    mask_width = extents.x2 - extents.x1;
    mask_height = extents.y2 - extents.y1;

    subimage = pixman_image_create_bits(PIXMAN_a1, mask_width, mask_height,
                                        mask_data, mask_stride);
    pixman_region32_init_from_image(&mask_region,
                                    subimage);
    pixman_image_unref(subimage);

    if (needs_invert) {
        pixman_box32_t rect;

        rect.x1 = rect.y1 = 0;
        rect.x2 = mask_width;
        rect.y2 = mask_height;

        pixman_region32_inverse(&mask_region, &mask_region, &rect);
    }

    pixman_region32_translate(&mask_region,
                              -mask_x + x, -mask_y + y);

    pixman_region32_intersect(dest_region, dest_region, &mask_region);
    pixman_region32_fini(&mask_region);

    pixman_image_unref(image);
}

static void canvas_set_path(CairoCanvas *canvas, void *addr)
{
    cairo_t *cairo = canvas->cairo;
    uint32_t* data_size = (uint32_t*)addr;
    access_test(&canvas->base, data_size, sizeof(uint32_t));
    uint32_t more = *data_size;

    SpicePathSeg* seg = (SpicePathSeg*)(data_size + 1);

    do {
        access_test(&canvas->base, seg, sizeof(SpicePathSeg));

        uint32_t flags = seg->flags;
        SpicePointFix* point = (SpicePointFix*)seg->data;
        SpicePointFix* end_point = point + seg->count;
        access_test(&canvas->base, point, (unsigned long)end_point - (unsigned long)point);
        ASSERT(point < end_point);
        more -= ((unsigned long)end_point - (unsigned long)seg);
        seg = (SpicePathSeg*)end_point;

        if (flags & SPICE_PATH_BEGIN) {
            cairo_new_sub_path(cairo);
            cairo_move_to(cairo, fix_to_double(point->x), fix_to_double(point->y));
            point++;
        }

        if (flags & SPICE_PATH_BEZIER) {
            ASSERT((point - end_point) % 3 == 0);
            for (; point + 2 < end_point; point += 3) {
                cairo_curve_to(cairo,
                               fix_to_double(point[0].x), fix_to_double(point[0].y),
                               fix_to_double(point[1].x), fix_to_double(point[1].y),
                               fix_to_double(point[2].x), fix_to_double(point[2].y));
            }
        } else {
            for (; point < end_point; point++) {
                cairo_line_to(cairo, fix_to_double(point->x), fix_to_double(point->y));
            }
        }
        if (flags & SPICE_PATH_END) {
            if (flags & SPICE_PATH_CLOSE) {
                cairo_close_path(cairo);
            }
            cairo_new_sub_path(cairo);
        }
    } while (more);
}

static void canvas_clip(CairoCanvas *canvas, SpiceClip *clip)
{
    cairo_t *cairo = canvas->cairo;

    switch (clip->type) {
    case SPICE_CLIP_TYPE_NONE:
        break;
    case SPICE_CLIP_TYPE_RECTS: {
        uint32_t *n = (uint32_t *)SPICE_GET_ADDRESS(clip->data);
        access_test(&canvas->base, n, sizeof(uint32_t));

        SpiceRect *now = (SpiceRect *)(n + 1);
        SpiceRect *end = now + *n;
        access_test(&canvas->base, now, (unsigned long)end - (unsigned long)now);

        for (; now < end; now++) {
            cairo_rectangle(cairo, now->left, now->top, now->right - now->left,
                            now->bottom - now->top);
        }
        cairo_clip(cairo);
        break;
    }
    case SPICE_CLIP_TYPE_PATH:
        canvas_set_path(canvas, SPICE_GET_ADDRESS(clip->data));
        cairo_clip(cairo);
        break;
    default:
        CANVAS_ERROR("invalid clip type");
    }
}

static inline cairo_line_cap_t canvas_line_cap_to_cairo(int end_style)
{
    switch (end_style) {
    case SPICE_LINE_CAP_ROUND:
        return CAIRO_LINE_CAP_ROUND;
    case SPICE_LINE_CAP_SQUARE:
        return CAIRO_LINE_CAP_SQUARE;
    case SPICE_LINE_CAP_BUTT:
        return CAIRO_LINE_CAP_BUTT;
    default:
        CANVAS_ERROR("bad end style %d", end_style);
    }
}

static inline cairo_line_join_t canvas_line_join_to_cairo(int join_style)
{
    switch (join_style) {
    case SPICE_LINE_JOIN_ROUND:
        return CAIRO_LINE_JOIN_ROUND;
    case SPICE_LINE_JOIN_BEVEL:
        return CAIRO_LINE_JOIN_BEVEL;
    case SPICE_LINE_JOIN_MITER:
        return CAIRO_LINE_JOIN_MITER;
    default:
        CANVAS_ERROR("bad join style %d", join_style);
    }
}

static void canvas_set_line_attr_no_dash(CairoCanvas *canvas, SpiceLineAttr *attr)
{
    cairo_t *cairo = canvas->cairo;

    cairo_set_line_width(cairo, fix_to_double(attr->width));
    cairo_set_line_cap(cairo, canvas_line_cap_to_cairo(attr->end_style));
    cairo_set_line_join(cairo, canvas_line_join_to_cairo(attr->join_style));
    cairo_set_miter_limit(cairo, fix_to_double(attr->miter_limit));
    cairo_set_dash(cairo, NULL, 0, 0);
}

static void canvas_set_dash(CairoCanvas *canvas, uint8_t nseg, SPICE_ADDRESS addr, int start_is_gap)
{
    SPICE_FIXED28_4* style = (SPICE_FIXED28_4*)SPICE_GET_ADDRESS(addr);
    double offset = 0;
    double *local_style;
    int i;

    access_test(&canvas->base, style, nseg * sizeof(*style));

    if (nseg == 0) {
        CANVAS_ERROR("bad nseg");
    }
    local_style = (double *)malloc(nseg * sizeof(*local_style));

    if (start_is_gap) {
        offset = fix_to_double(*style);
        local_style[nseg - 1] = fix_to_double(*style);
        style++;

        for (i = 0; i < nseg - 1; i++, style++) {
            local_style[i] = fix_to_double(*style);
        }
    } else {
        for (i = 0; i < nseg; i++, style++) {
            local_style[i] = fix_to_double(*style);
        }
    }
    cairo_set_dash(canvas->cairo, local_style, nseg, offset);
    free(local_style);
}

static inline void canvas_invers_32bpp(uint8_t *dest, int dest_stride, uint8_t *src, int src_stride,
                                       int width, uint8_t *end)
{
    for (; src != end; src += src_stride, dest += dest_stride) {
        uint32_t *src_line = (uint32_t *)src;
        uint32_t *src_line_end = src_line + width;
        uint32_t *dest_line = (uint32_t *)dest;

        while (src_line < src_line_end) {
            *(dest_line++) = ~*(src_line++) & 0x00ffffff;
        }
    }
}

static inline void canvas_invers_24bpp(uint8_t *dest, int dest_stride, uint8_t *src, int src_stride,
                                       int width, uint8_t *end)
{
    for (; src != end; src += src_stride, dest += dest_stride) {
        uint8_t *src_line = src;
        uint8_t *src_line_end = src_line + width * 3;
        uint8_t *dest_line = dest;

        for (; src_line < src_line_end; ++dest_line) {
            *(dest_line++) = ~*(src_line++);
            *(dest_line++) = ~*(src_line++);
            *(dest_line++) = ~*(src_line++);
        }
    }
}

static inline void canvas_invers_16bpp(uint8_t *dest, int dest_stride, uint8_t *src, int src_stride,
                                       int width, uint8_t *end)
{
    for (; src != end; src += src_stride, dest += dest_stride) {
        uint16_t *src_line = (uint16_t*)src;
        uint16_t *src_line_end = src_line + width;
        uint32_t *dest_line = (uint32_t*)dest;

        for (; src_line < src_line_end; ++dest_line, src_line++) {
            *dest_line = ~canvas_16bpp_to_32bpp(*src_line) & 0x00ffffff;
        }
    }
}

static inline void canvas_invers_8bpp(uint8_t *dest, int dest_stride, uint8_t *src, int src_stride,
                                      int width, uint8_t *end, SpicePalette *palette)
{
    if (!palette) {
        CANVAS_ERROR("no palette");
    }

    for (; src != end; src += src_stride, dest += dest_stride) {
        uint32_t *dest_line = (uint32_t*)dest;
        uint8_t *src_line = src;
        uint8_t *src_line_end = src_line + width;

        while (src_line < src_line_end) {
            ASSERT(*src_line < palette->num_ents);
            *(dest_line++) = ~palette->ents[*(src_line++)] & 0x00ffffff;
        }
    }
}

static inline void canvas_invers_4bpp_be(uint8_t* dest, int dest_stride, uint8_t* src,
                                         int src_stride, int width, uint8_t* end,
                                         SpicePalette *palette)
{
    if (!palette) {
        CANVAS_ERROR("no palette");
    }

    for (; src != end; src += src_stride, dest += dest_stride) {
        uint32_t *dest_line = (uint32_t *)dest;
        uint8_t *now = src;
        int i;

        for (i = 0; i < (width >> 1); i++) {
            ASSERT((*now & 0x0f) < palette->num_ents);
            ASSERT(((*now >> 4) & 0x0f) < palette->num_ents);
            *(dest_line++) = ~palette->ents[(*now >> 4) & 0x0f] & 0x00ffffff;
            *(dest_line++) = ~palette->ents[*(now++) & 0x0f] & 0x00ffffff;
        }
        if (width & 1) {
            *(dest_line) = ~palette->ents[(*src >> 4) & 0x0f] & 0x00ffffff;
        }
    }
}

static inline void canvas_invers_1bpp_be(uint8_t* dest, int dest_stride, uint8_t* src,
                                         int src_stride, int width, uint8_t* end,
                                         SpicePalette *palette)
{
    uint32_t fore_color;
    uint32_t back_color;

    if (!palette) {
        CANVAS_ERROR("no palette");
    }

    fore_color = palette->ents[1];
    back_color = palette->ents[0];

    for (; src != end; src += src_stride, dest += dest_stride) {
        uint32_t* dest_line = (uint32_t*)dest;
        int i;

        for (i = 0; i < width; i++) {
            if (test_bit_be(src, i)) {
                *(dest_line++) = ~fore_color & 0x00ffffff;
            } else {
                *(dest_line++) = ~back_color & 0x00ffffff;
            }
        }
    }
}

static pixman_image_t *canvas_bitmap_to_invers_surface(CairoCanvas *canvas, SpiceBitmap* bitmap,
                                                        SpicePalette *palette)
{
    uint8_t* src = (uint8_t *)SPICE_GET_ADDRESS(bitmap->data);
    int src_stride;
    uint8_t* end;
    uint8_t* dest;
    int dest_stride;
    pixman_image_t* surface;

    src_stride = bitmap->stride;
    end = src + (bitmap->y * src_stride);
    access_test(&canvas->base, src, bitmap->y * src_stride);

    surface = pixman_image_create_bits((bitmap->format == SPICE_BITMAP_FMT_RGBA) ?
                                       PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8,
                                       bitmap->x, bitmap->y, NULL, 0);
    if (surface == NULL) {
        CANVAS_ERROR("create surface failed");
    }
    dest = (uint8_t *)pixman_image_get_data(surface);
    dest_stride = pixman_image_get_stride(surface);

    if (!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        ASSERT(bitmap->y > 0);
        dest += dest_stride * (bitmap->y - 1);
        dest_stride = -dest_stride;
    }
    switch (bitmap->format) {
    case SPICE_BITMAP_FMT_32BIT:
    case SPICE_BITMAP_FMT_RGBA:
        canvas_invers_32bpp(dest, dest_stride, src, src_stride, bitmap->x, end);
        break;
    case SPICE_BITMAP_FMT_24BIT:
        canvas_invers_24bpp(dest, dest_stride, src, src_stride, bitmap->x, end);
        break;
    case SPICE_BITMAP_FMT_16BIT:
        canvas_invers_16bpp(dest, dest_stride, src, src_stride, bitmap->x, end);
        break;
    case SPICE_BITMAP_FMT_8BIT:
        canvas_invers_8bpp(dest, dest_stride, src, src_stride, bitmap->x, end, palette);
        break;
    case SPICE_BITMAP_FMT_4BIT_BE:
        canvas_invers_4bpp_be(dest, dest_stride, src, src_stride, bitmap->x, end, palette);
        break;
    case SPICE_BITMAP_FMT_1BIT_BE:
        canvas_invers_1bpp_be(dest, dest_stride, src, src_stride, bitmap->x, end, palette);
        break;
    }
    return surface;
}

#if defined(CAIRO_CANVAS_CACHE) || defined(CAIRO_CANVAS_IMAGE_CACHE)

#ifndef CAIRO_CANVAS_CACHE
static SpicePalette *canvas_get_palette(CairoCanvas *canvas, SpiceBitmap *bitmap)
{
    SpicePalette *local_palette;
    SpicePalette *palette;
    int size;

    if (!bitmap->palette) {
        return NULL;
    }

    palette = (SpicePalette *)SPICE_GET_ADDRESS(bitmap->palette);
    if (canvas->base.color_shift != 5) {
        return palette;
    }

    size = sizeof(SpicePalette) + (palette->num_ents << 2);
    local_palette = malloc(size);
    memcpy(local_palette, palette, size);
    canvas_localize_palette(&canvas->base, palette);

    return local_palette;
}

static void free_palette(SpiceBitmap *bitmap, SpicePalette *palette)
{
    if (!palette || palette == SPICE_GET_ADDRESS(bitmap->palette)) {
        return;
    }
    free(palette);
}

#endif

static pixman_image_t *canvas_get_invers_image(CairoCanvas *canvas, SPICE_ADDRESS addr)
{
    SpiceImageDescriptor *descriptor = (SpiceImageDescriptor *)SPICE_GET_ADDRESS(addr);
    pixman_image_t *surface;
    pixman_image_t *invers = NULL;

    access_test(&canvas->base, descriptor, sizeof(SpiceImageDescriptor));

    int cache_me = descriptor->flags & SPICE_IMAGE_FLAGS_CACHE_ME;

    switch (descriptor->type) {
    case SPICE_IMAGE_TYPE_QUIC: {
        SpiceQUICImage *image = (SpiceQUICImage *)descriptor;
        access_test(&canvas->base, descriptor, sizeof(SpiceQUICImage));
        if (cache_me) {
            surface = canvas_get_quic(&canvas->base, image, 0);
        } else {
            return canvas_get_quic(&canvas->base, image, 1);
        }
        break;
    }
#ifdef CAIRO_CANVAS_NO_CHUNKS
    case SPICE_IMAGE_TYPE_LZ_PLT: {
        access_test(&canvas->base, descriptor, sizeof(SpiceLZPLTImage));
        LZImage *image = (LZImage *)descriptor;
        if (cache_me) {
            surface = canvas_get_lz(&canvas->base, image, 0);
        } else {
            return canvas_get_lz(&canvas->base, image, 1);
        }
        break;
    }
    case SPICE_IMAGE_TYPE_LZ_RGB: {
        access_test(&canvas->base, descriptor, sizeof(SpiceLZRGBImage));
        LZImage *image = (LZImage *)descriptor;
        if (cache_me) {
            surface = canvas_get_lz(&canvas->base, image, 0);
        } else {
            return canvas_get_lz(&canvas->base, image, 1);
        }
        break;
    }
#endif
#ifdef USE_GLZ
    case SPICE_IMAGE_TYPE_GLZ_RGB: {
        access_test(&canvas->base, descriptor, sizeof(SpiceLZRGBImage));
        LZImage *image = (LZImage *)descriptor;
        surface = canvas_get_glz(&canvas->base, image);
        break;
    }
#endif
    case SPICE_IMAGE_TYPE_FROM_CACHE:
        surface = canvas->base.bits_cache->ops->get(canvas->base.bits_cache, descriptor->id);
        break;
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmapImage *bitmap = (SpiceBitmapImage *)descriptor;
        access_test(&canvas->base, descriptor, sizeof(SpiceBitmapImage));
        if (cache_me) {
            surface = canvas_get_bits(&canvas->base, &bitmap->bitmap);
        } else {
#ifdef CAIRO_CANVAS_CACHE
            return canvas_bitmap_to_invers_surface(canvas, &bitmap->bitmap,
                                                   canvas_get_palett(&canvas->base,
                                                                     bitmap->bitmap.palette,
                                                                     bitmap->bitmap.flags));
#else
            SpicePalette *palette = canvas_get_palette(canvas, &bitmap->bitmap);
            surface = canvas_bitmap_to_invers_surface(canvas, &bitmap->bitmap, palette);
            free_palette(&bitmap->bitmap, palette);
            return surface;
#endif
        }
        break;
    }
    default:
        CANVAS_ERROR("invalid image type");
    }

    if (cache_me) {
        canvas->base.bits_cache->ops->put(canvas->base.bits_cache, descriptor->id, surface);
    }

    invers = canvas_handle_inverse_user_data(surface);
    pixman_image_unref(surface);
    return invers;
}

#else

static pixman_image_t *canvas_get_invers(CairoCanvas *canvas, SpiceBitmap *bitmap)
{
    SpicePalette *palette;

    if (!bitmap->palette) {
        return canvas_bitmap_to_invers_surface(canvas, bitmap, NULL);
    }
    palette = (SpicePalette *)SPICE_GET_ADDRESS(bitmap->palette);
    if (canvas->color_shift == 5) {
        int size = sizeof(SpicePalette) + (palette->num_ents << 2);
        SpicePalette *local_palette = malloc(size);
        pixman_image_t* surface;

        memcpy(local_palette, palette, size);
        canvas_localize_palette(canvas, palette);
        surface = canvas_bitmap_to_invers_surface(canvas, bitmap, local_palette);
        free(local_palette);
        return surface;
    } else {
        return canvas_bitmap_to_invers_surface(canvas, bitmap, palette);
    }
}

static pixman_image_t *canvas_get_invers_image(CairoCanvas *canvas, SPICE_ADDRESS addr)
{
    SpiceImageDescriptor *descriptor = (SpiceImageDescriptor *)SPICE_GET_ADDRESS(addr);

    access_test(canvas, descriptor, sizeof(SpiceImageDescriptor));

    switch (descriptor->type) {
    case SPICE_IMAGE_TYPE_QUIC: {
        SpiceQUICImage *image = (SpiceQUICImage *)descriptor;
        access_test(canvas, descriptor, sizeof(SpiceQUICImage));
        return canvas_get_quic(canvas, image, 1);
    }
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmapImage *bitmap = (SpiceBitmapImage *)descriptor;
        access_test(canvas, descriptor, sizeof(SpiceBitmapImage));
        return canvas_get_invers(canvas, &bitmap->bitmap);
    }
    default:
        CANVAS_ERROR("invalid image type");
    }
}

#endif

static pixman_image_t* canvas_surface_from_self(CairoCanvas *canvas, SpicePoint *pos,
                                                int32_t width, int32_t heigth)
{
    pixman_image_t *surface;
    pixman_image_t *src_surface;
    uint8_t *dest;
    int dest_stride;
    uint8_t *src;
    int src_stride;
    int i;

    surface = pixman_image_create_bits(PIXMAN_x8r8g8b8, width, heigth, NULL, 0);
    if (surface == NULL) {
        CANVAS_ERROR("create surface failed");
    }

    dest = (uint8_t *)pixman_image_get_data(surface);
    dest_stride = pixman_image_get_stride(surface);

    src_surface = canvas->image;
    src = (uint8_t *)pixman_image_get_data(src_surface);
    src_stride = pixman_image_get_stride(src_surface);
    src += pos->y * src_stride + (pos->x << 2);
    for (i = 0; i < heigth; i++, dest += dest_stride, src += src_stride) {
        memcpy(dest, src, width << 2);
    }
    return surface;
}

static cairo_pattern_t *canvas_get_brush(CairoCanvas *canvas, SpiceBrush *brush, uint32_t invers)
{
    switch (brush->type) {
    case SPICE_BRUSH_TYPE_SOLID: {
        uint32_t color = (invers) ? ~brush->u.color : brush->u.color;
        double r, g, b;

        b = (double)(color & canvas->base.color_mask) / canvas->base.color_mask;
        color >>= canvas->base.color_shift;
        g = (double)(color & canvas->base.color_mask) / canvas->base.color_mask;
        color >>= canvas->base.color_shift;
        r = (double)(color & canvas->base.color_mask) / canvas->base.color_mask;
        return cairo_pattern_create_rgb(r, g, b);
    }
    case SPICE_BRUSH_TYPE_PATTERN: {
        pixman_image_t* surface;
        cairo_surface_t* cairo_surface;
        cairo_pattern_t *pattern;
        cairo_matrix_t matrix;

        if (invers) {
            surface = canvas_get_invers_image(canvas, brush->u.pattern.pat);
        } else {
            surface = canvas_get_image(&canvas->base, brush->u.pattern.pat);
        }
        cairo_surface = surface_from_pixman_image (surface);
        pixman_image_unref (surface);
        pattern = cairo_pattern_create_for_surface(cairo_surface);
        if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(cairo_surface);
            CANVAS_ERROR("create pattern failed, %s",
                         cairo_status_to_string(cairo_pattern_status(pattern)));
        }
        cairo_surface_destroy(cairo_surface);
        cairo_matrix_init_translate(&matrix, -brush->u.pattern.pos.x, -brush->u.pattern.pos.y);
        cairo_pattern_set_matrix(pattern, &matrix);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
        return pattern;
    }
    case SPICE_BRUSH_TYPE_NONE:
        return NULL;
    default:
        CANVAS_ERROR("invalid brush type");
    }
}

typedef void (*DrawMethod)(void *);

static inline void __canvas_draw_invers(CairoCanvas *canvas, DrawMethod draw_method, void *data)
{
    cairo_t *cairo = canvas->cairo;
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_XOR);
    draw_method(data);
}

static inline int canvas_set_ropd_operator(cairo_t *cairo, uint16_t rop_decriptor)
{
    cairo_operator_t cairo_op;

    if (rop_decriptor & SPICE_ROPD_OP_PUT) {
        cairo_op = CAIRO_OPERATOR_RASTER_COPY;
    } else if (rop_decriptor & SPICE_ROPD_OP_XOR) {
        cairo_op = CAIRO_OPERATOR_RASTER_XOR;
    } else if (rop_decriptor & SPICE_ROPD_OP_OR) {
        cairo_op = CAIRO_OPERATOR_RASTER_OR;
    } else if (rop_decriptor & SPICE_ROPD_OP_AND) {
        cairo_op = CAIRO_OPERATOR_RASTER_AND;
    } else {
        return 0;
    }
    cairo_set_operator(cairo, cairo_op);
    return 1;
}

static void canvas_draw_with_pattern(CairoCanvas *canvas, cairo_pattern_t *pattern,
                                     uint16_t rop_decriptor,
                                     DrawMethod draw_method, void *data)
{
    cairo_t *cairo = canvas->cairo;

    if (rop_decriptor & SPICE_ROPD_OP_BLACKNESS) {
        cairo_set_source_rgb(cairo, 0, 0, 0);
        draw_method(data);
    } else if (rop_decriptor & SPICE_ROPD_OP_WHITENESS) {
        cairo_set_source_rgb(cairo, 1, 1, 1);
        draw_method(data);
    } else if (rop_decriptor & SPICE_ROPD_OP_INVERS) {
        __canvas_draw_invers(canvas, draw_method, data);
    } else {
        if (rop_decriptor & SPICE_ROPD_INVERS_DEST) {
            __canvas_draw_invers(canvas, draw_method, data);
        }

        if (canvas_set_ropd_operator(cairo, rop_decriptor)) {
            ASSERT(pattern);
            cairo_set_source(cairo, pattern);
            draw_method(data);
        }

        if (rop_decriptor & SPICE_ROPD_INVERS_RES) {
            __canvas_draw_invers(canvas, draw_method, data);
        }
    }
}

static inline void canvas_draw(CairoCanvas *canvas, SpiceBrush *brush, uint16_t rop_decriptor,
                               DrawMethod draw_method, void *data)
{
    cairo_pattern_t *pattern = canvas_get_brush(canvas, brush, rop_decriptor & SPICE_ROPD_INVERS_BRUSH);
    canvas_draw_with_pattern(canvas, pattern, rop_decriptor, draw_method, data);
    if (pattern) {
        cairo_pattern_destroy(pattern);
    }
}

static cairo_pattern_t *canvas_get_mask_pattern(CairoCanvas *canvas, SpiceQMask *mask, int x, int y)
{
    pixman_image_t *surface;
    cairo_surface_t *cairo_surface;
    cairo_pattern_t *pattern;
    cairo_matrix_t matrix;

    if (!(surface = canvas_get_mask(&canvas->base, mask, NULL))) {
        return NULL;
    }
    cairo_surface = surface_from_pixman_image (surface);
    pixman_image_unref (surface);
    pattern = cairo_pattern_create_for_surface(cairo_surface);
    if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cairo_surface);
        CANVAS_ERROR("create pattern failed, %s",
                     cairo_status_to_string(cairo_pattern_status(pattern)));
    }
    cairo_surface_destroy(cairo_surface);

    cairo_matrix_init_translate(&matrix, mask->pos.x - x, mask->pos.y - y);
    cairo_pattern_set_matrix(pattern, &matrix);
    return pattern;
}

typedef struct DrawMaskData {
    cairo_t *cairo;
    cairo_pattern_t *mask;
} DrawMaskData;

static void __draw_mask(void *data)
{
    cairo_mask(((DrawMaskData *)data)->cairo, ((DrawMaskData *)data)->mask);
}

static void fill_solid_rects(CairoCanvas *canvas,
                             pixman_region32_t *region,
                             uint32_t color)
{
    pixman_box32_t *rects;
    int n_rects;
    int i;

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        spice_pixman_fill_rect(canvas->image,
                               rects[i].x1, rects[i].y1,
                               rects[i].x2 - rects[i].x1,
                               rects[i].y2 - rects[i].y1,
                               color);
    }
}

static void fill_solid_rects_rop(CairoCanvas *canvas,
                                 pixman_region32_t *region,
                                 uint32_t color,
                                 SpiceROP rop)
{
    pixman_box32_t *rects;
    int n_rects;
    int i;

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        spice_pixman_fill_rect_rop(canvas->image,
                                   rects[i].x1, rects[i].y1,
                                   rects[i].x2 - rects[i].x1,
                                   rects[i].y2 - rects[i].y1,
                                   color, rop);
    }
}

static void fill_tiled_rects(CairoCanvas *canvas,
                             pixman_region32_t *region,
                             SpicePattern *pattern)
{
    pixman_image_t *tile;
    int offset_x, offset_y;
    pixman_box32_t *rects;
    int n_rects;
    int i;

    rects = pixman_region32_rectangles(region, &n_rects);

    tile = canvas_get_image(&canvas->base, pattern->pat);
    offset_x = pattern->pos.x;
    offset_y = pattern->pos.y;

    for (i = 0; i < n_rects; i++) {
        spice_pixman_tile_rect(canvas->image,
                               rects[i].x1, rects[i].y1,
                               rects[i].x2 - rects[i].x1,
                               rects[i].y2 - rects[i].y1,
                               tile, offset_x, offset_y);
    }

    pixman_image_unref(tile);
}

static void fill_tiled_rects_rop(CairoCanvas *canvas,
                                 pixman_region32_t *region,
                                 SpicePattern *pattern,
                                 SpiceROP rop)
{
    pixman_image_t *tile;
    int offset_x, offset_y;
    pixman_box32_t *rects;
    int n_rects;
    int i;

    rects = pixman_region32_rectangles(region, &n_rects);

    tile = canvas_get_image(&canvas->base, pattern->pat);
    offset_x = pattern->pos.x;
    offset_y = pattern->pos.y;

    for (i = 0; i < n_rects; i++) {
        spice_pixman_tile_rect_rop(canvas->image,
                                   rects[i].x1, rects[i].y1,
                                   rects[i].x2 - rects[i].x1,
                                   rects[i].y2 - rects[i].y1,
                                   tile, offset_x, offset_y,
                                   rop);
    }

    pixman_image_unref(tile);
}


static void blit_with_region(CairoCanvas *canvas,
                             pixman_region32_t *region,
                             pixman_image_t *src_image,
                             int offset_x, int offset_y)
{
    pixman_box32_t *rects;
    int n_rects, i;

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        int src_x, src_y, dest_x, dest_y, width, height;

        dest_x = rects[i].x1;
        dest_y = rects[i].y1;
        width = rects[i].x2 - rects[i].x1;
        height = rects[i].y2 - rects[i].y1;

        src_x = rects[i].x1 - offset_x;
        src_y = rects[i].y1 - offset_y;

        spice_pixman_blit(canvas->image,
                          src_image,
                          src_x, src_y,
                          dest_x, dest_y,
                          width, height);
    }
}

static void blit_image(CairoCanvas *canvas,
                       pixman_region32_t *region,
                       SPICE_ADDRESS src_bitmap,
                       int offset_x, int offset_y)
{
    pixman_image_t *src_image;

    src_image = canvas_get_image(&canvas->base, src_bitmap);
    blit_with_region(canvas, region, src_image,
                      offset_x, offset_y);
    pixman_image_unref(src_image);
}

static void blit_image_rop(CairoCanvas *canvas,
                           pixman_region32_t *region,
                           SPICE_ADDRESS src_bitmap,
                           int offset_x, int offset_y,
                           SpiceROP rop)
{
    pixman_image_t *src_image;
    pixman_box32_t *rects;
    int n_rects, i;

    rects = pixman_region32_rectangles(region, &n_rects);

    src_image = canvas_get_image(&canvas->base, src_bitmap);
    for (i = 0; i < n_rects; i++) {
        int src_x, src_y, dest_x, dest_y, width, height;

        dest_x = rects[i].x1;
        dest_y = rects[i].y1;
        width = rects[i].x2 - rects[i].x1;
        height = rects[i].y2 - rects[i].y1;

        src_x = rects[i].x1 - offset_x;
        src_y = rects[i].y1 - offset_y;

        spice_pixman_blit_rop(canvas->image,
                              src_image,
                              src_x, src_y,
                              dest_x, dest_y,
                              width, height, rop);
    }
    pixman_image_unref(src_image);
}

static void scale_image(CairoCanvas *canvas,
                        pixman_region32_t *region,
                        SPICE_ADDRESS src_bitmap,
                        int src_x, int src_y,
                        int src_width, int src_height,
                        int dest_x, int dest_y,
                        int dest_width, int dest_height,
                        int scale_mode)
{
    pixman_transform_t transform;
    pixman_image_t *src;
    double sx, sy;

    sx = (double)(src_width) / (dest_width);
    sy = (double)(src_height) / (dest_height);

    src = canvas_get_image(&canvas->base, src_bitmap);

    pixman_image_set_clip_region32(canvas->image, region);

    pixman_transform_init_scale(&transform,
                                pixman_double_to_fixed(sx),
                                pixman_double_to_fixed(sy));

    pixman_image_set_transform(src, &transform);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    ASSERT(scale_mode == SPICE_IMAGE_SCALE_MODE_INTERPOLATE ||
           scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST);
    pixman_image_set_filter(src,
                            (scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST) ?
                            PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_GOOD,
                            NULL, 0);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src, NULL, canvas->image,
                             ROUND(src_x / sx), ROUND(src_y / sy), /* src */
                             0, 0, /* mask */
                             dest_x, dest_y, /* dst */
                             dest_width, dest_height);

    pixman_transform_init_identity(&transform);
    pixman_image_set_transform(src, &transform);

    pixman_image_set_clip_region32(canvas->image, NULL);
    pixman_image_unref(src);
}

static void scale_image_rop(CairoCanvas *canvas,
                            pixman_region32_t *region,
                            SPICE_ADDRESS src_bitmap,
                            int src_x, int src_y,
                            int src_width, int src_height,
                            int dest_x, int dest_y,
                            int dest_width, int dest_height,
                            int scale_mode, SpiceROP rop)
{
    pixman_transform_t transform;
    pixman_image_t *src;
    pixman_image_t *scaled;
    pixman_box32_t *rects;
    int n_rects, i;
    double sx, sy;

    sx = (double)(src_width) / (dest_width);
    sy = (double)(src_height) / (dest_height);

    src = canvas_get_image(&canvas->base, src_bitmap);

    scaled = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                      dest_width,
                                      dest_height,
                                      NULL, 0);

    pixman_region32_translate(region, -dest_x, -dest_y);
    pixman_image_set_clip_region32(scaled, region);

    pixman_transform_init_scale(&transform,
                                pixman_double_to_fixed(sx),
                                pixman_double_to_fixed(sy));

    pixman_image_set_transform(src, &transform);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    ASSERT(scale_mode == SPICE_IMAGE_SCALE_MODE_INTERPOLATE ||
           scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST);
    pixman_image_set_filter(src,
                            (scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST) ?
                            PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_GOOD,
                            NULL, 0);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src, NULL, scaled,
                             ROUND(src_x / sx), ROUND(src_y / sy), /* src */
                             0, 0, /* mask */
                             0, 0, /* dst */
                             dest_width,
                             dest_height);

    pixman_transform_init_identity(&transform);
    pixman_image_set_transform(src, &transform);

    /* Translate back */
    pixman_region32_translate(region, dest_x, dest_y);

    rects = pixman_region32_rectangles(region, &n_rects);

    for (i = 0; i < n_rects; i++) {
        spice_pixman_blit_rop(canvas->image,
                              scaled,
                              rects[i].x1 - dest_x,
                              rects[i].y1 - dest_y,
                              rects[i].x1, rects[i].y1,
                              rects[i].x2 - rects[i].x1,
                              rects[i].y2 - rects[i].y1,
                              rop);
    }

    pixman_image_unref(scaled);
    pixman_image_unref(src);
}

static void draw_brush(CairoCanvas *canvas,
                       pixman_region32_t *region,
                       SpiceBrush *brush,
                       SpiceROP rop)
{
    uint32_t color;
    SpicePattern *pattern;

    switch (brush->type) {
    case SPICE_BRUSH_TYPE_SOLID:
        color = brush->u.color;
        if (rop == SPICE_ROP_COPY) {
            fill_solid_rects(canvas, region, color);
        } else {
            fill_solid_rects_rop(canvas, region, color, rop);
        }
        break;
    case SPICE_BRUSH_TYPE_PATTERN:
        pattern = &brush->u.pattern;

        if (rop == SPICE_ROP_COPY) {
            fill_tiled_rects(canvas, region, pattern);
        } else {
            fill_tiled_rects_rop(canvas, region, pattern, rop);
        }
        break;
    case SPICE_BRUSH_TYPE_NONE:
        /* Still need to do *something* here, because rop could be e.g invert dest */
        fill_solid_rects_rop(canvas, region, 0, rop);
        break;
    default:
        CANVAS_ERROR("invalid brush type");
    }
}

/* If we're exiting early we may still have to load an image in case
   it has to be cached or something */
static void touch_brush(CairoCanvas *canvas, SpiceBrush *brush)
{
    SpicePattern *pattern;
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        pattern = &brush->u.pattern;
        canvas_touch_image(&canvas->base, pattern->pat);
    }
}

void canvas_draw_fill(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill)
{
    pixman_region32_t dest_region;
    SpiceROP rop;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);


    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &fill->mask,
                       bbox->left, bbox->top);

    rop = ropd_descriptor_to_rop(fill->rop_decriptor,
                                 ROP_INPUT_BRUSH,
                                 ROP_INPUT_DEST);

    if (rop == SPICE_ROP_NOOP || pixman_region32_n_rects(&dest_region) == 0) {
        touch_brush(canvas, &fill->brush);
        pixman_region32_fini(&dest_region);
        return;
    }

    draw_brush(canvas, &dest_region, &fill->brush, rop);

    pixman_region32_fini(&dest_region);
}

static cairo_pattern_t *canvas_src_image_to_pat(CairoCanvas *canvas, SPICE_ADDRESS src_bitmap,
                                                const SpiceRect *src, const SpiceRect *dest, int invers,
                                                int scale_mode)
{
    cairo_pattern_t *pattern;
    pixman_image_t *surface;
    cairo_surface_t *cairo_surface;
    cairo_matrix_t matrix;

    ASSERT(src_bitmap);
    ASSERT(scale_mode == SPICE_IMAGE_SCALE_MODE_INTERPOLATE || scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST);
    if (invers) {
        surface = canvas_get_invers_image(canvas, src_bitmap);
    } else {
        surface = canvas_get_image(&canvas->base, src_bitmap);
    }

    cairo_surface = surface_from_pixman_image (surface);
    pixman_image_unref (surface);
    pattern = cairo_pattern_create_for_surface(cairo_surface);
    cairo_surface_destroy(cairo_surface);
    if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS) {
        CANVAS_ERROR("create pattern failed, %s",
                     cairo_status_to_string(cairo_pattern_status(pattern)));
    }

    if (!rect_is_same_size(src, dest)) {
        double sx, sy;

        sx = (double)(src->right - src->left) / (dest->right - dest->left);
        sy = (double)(src->bottom - src->top) / (dest->bottom - dest->top);

        cairo_matrix_init_scale(&matrix, sx, sy);
        cairo_pattern_set_filter(pattern, (scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST) ?
                                 CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);

        cairo_matrix_translate(&matrix, src->left / sx - dest->left, src->top / sy - dest->top);
    } else {
        cairo_matrix_init_translate(&matrix, src->left - dest->left, src->top - dest->top);
    }

    cairo_pattern_set_matrix(pattern, &matrix);
    return pattern;
}

void canvas_draw_copy(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy)
{
    cairo_t *cairo = canvas->cairo;
    cairo_pattern_t *pattern;
    cairo_pattern_t *mask;

    cairo_save(cairo);
    canvas_clip(canvas, clip);

    pattern = canvas_src_image_to_pat(canvas, copy->src_bitmap, &copy->src_area, bbox,
                                      copy->rop_decriptor & SPICE_ROPD_INVERS_SRC, copy->scale_mode);
    cairo_set_source(cairo, pattern);
    cairo_pattern_destroy(pattern);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_COPY);
    if ((mask = canvas_get_mask_pattern(canvas, &copy->mask, bbox->left, bbox->top))) {
        cairo_rectangle(cairo, bbox->left, bbox->top, bbox->right - bbox->left,
                        bbox->bottom - bbox->top);
        cairo_clip(cairo);
        cairo_mask(cairo, mask);
        cairo_pattern_destroy(mask);
    } else {
        cairo_rectangle(cairo, bbox->left, bbox->top, bbox->right - bbox->left,
                        bbox->bottom - bbox->top);
        cairo_fill(cairo);
    }

    cairo_restore(cairo);
}

#ifdef WIN32
void canvas_put_image(CairoCanvas *canvas, HDC dc, const SpiceRect *dest, const uint8_t *_src_data,
                      uint32_t src_width, uint32_t src_height, int src_stride,
                      const QRegion *clip)
#else
void canvas_put_image(CairoCanvas *canvas, const SpiceRect *dest, const uint8_t *_src_data,
                      uint32_t src_width, uint32_t src_height, int src_stride,
                      const QRegion *clip)
#endif
{
    cairo_t *cairo = canvas->cairo;
    cairo_surface_t* surf = NULL;
    int dest_width;
    int dest_height;
    uint8_t *src_data = (uint8_t *)_src_data;
    uint32_t *data;
    int nstride;

    cairo_save(cairo);

    if (clip) {
        const SpiceRect *now = clip->rects;
        const SpiceRect *end = clip->rects + clip->num_rects;
        for (; now < end; now++) {
            cairo_rectangle(cairo, now->left, now->top, now->right - now->left,
                            now->bottom - now->top);
        }
        cairo_clip(cairo);
    }


    dest_width = dest->right - dest->left;
    dest_height = dest->bottom - dest->top;

    if (dest_width != src_width || dest_height != src_height) {
        int x, y;
        int x_mul = (uint32_t)((src_width << 16) / dest_width);
        int y_mul = (uint32_t)((src_height << 16) / dest_height);
        int new_y;
        int set_y;
        int nsrc_stride;

        if (src_stride < 0) {
            nsrc_stride = -src_stride;
            src_data = src_data - (src_height - 1) * nsrc_stride;
            nsrc_stride = nsrc_stride / 4;
        } else {
            nsrc_stride = src_stride / 4;
        }
        if ((dest_width * dest_height) > canvas->private_data_size) {
            if (canvas->private_data) {
                free(canvas->private_data);
                canvas->private_data = NULL;
                canvas->private_data_size = 0;
            }
            canvas->private_data = (uint32_t *)malloc(4 * dest_width * dest_height);
            if (!canvas->private_data) {
                return;
            }
            canvas->private_data_size = dest_width * dest_height;
        }
        if (!clip) {
            surf = cairo_get_target(cairo);
            data = (uint32_t *)cairo_image_surface_get_data(surf);
            nstride = cairo_image_surface_get_stride(surf) / 4;
            data += dest->top * nstride + dest->left + (dest_height - 1) * nstride;
        } else {
            data = (uint32_t *)canvas->private_data;
            nstride = dest_width;
            data += (dest_height - 1) * nstride;
        }

        for (y = 0; y < dest_height; ++y) {
            int y_mul_stride = -y * nstride;
            new_y = ((y * y_mul) >> 16);
            set_y = (new_y * nsrc_stride);
            for (x = 0; x < dest_width; ++x) {
                data[y_mul_stride + x] = ((uint32_t *)src_data)[set_y + ((x * x_mul) >> 16)];
            }
        }
        if (clip) {
            surf = cairo_image_surface_create_for_data((uint8_t *)canvas->private_data,
                                                       CAIRO_FORMAT_RGB24, dest_width,
                                                       dest_height, 4 * dest_width);
        }
    } else {
        surf = cairo_image_surface_create_for_data((uint8_t *)src_data, CAIRO_FORMAT_RGB24,
                                                   src_width, src_height, src_stride);
    }

    if (clip || !(dest_width != src_width || dest_height != src_height)) {
        cairo_set_source_surface(cairo, surf, dest->left, dest->top);
        cairo_surface_destroy(surf);

        cairo_rectangle(cairo, dest->left, dest->top, dest_width, dest_height);
        cairo_fill(cairo);
    }
    cairo_restore(cairo);
}

static cairo_surface_t *canvas_surf_to_color_maks_invers(cairo_surface_t *surface,
                                                         uint32_t trans_color)
{
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    uint8_t *src_line;
    uint8_t *end_src_line;
    int src_stride;
    uint8_t *dest_line;
    int dest_stride;
    cairo_surface_t *mask;
    int i;

    ASSERT(cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32 ||
           cairo_image_surface_get_format(surface) == CAIRO_FORMAT_RGB24);

    mask = cairo_image_surface_create(CAIRO_FORMAT_A1, width, height);
    if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) {
        CANVAS_ERROR("create surface failed, %s",
                     cairo_status_to_string(cairo_surface_status(mask)));
    }

    src_line = cairo_image_surface_get_data(surface);
    src_stride = cairo_image_surface_get_stride(surface);
    end_src_line = src_line + src_stride * height;

    dest_line = cairo_image_surface_get_data(mask);
    dest_stride = cairo_image_surface_get_stride(mask);

    for (; src_line != end_src_line; src_line += src_stride, dest_line += dest_stride) {
        memset(dest_line, 0xff, dest_stride);
        for (i = 0; i < width; i++) {
            if ((((uint32_t*)src_line)[i] & 0x00ffffff) == trans_color) {
                dest_line[i >> 3] &= ~(1 << (i & 0x07));
            }
        }
    }

    return mask;
}

void canvas_draw_transparent(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent)
{
    cairo_t *cairo = canvas->cairo;
    cairo_pattern_t *pattern;
    cairo_pattern_t *mask;
    cairo_surface_t *surface;
    cairo_surface_t *mask_surf;
    cairo_matrix_t matrix;

    cairo_save(cairo);
    cairo_rectangle(cairo,
                    bbox->left,
                    bbox->top,
                    bbox->right - bbox->left,
                    bbox->bottom - bbox->top);
    cairo_clip(cairo);
    canvas_clip(canvas, clip);

    pattern = canvas_src_image_to_pat(canvas, transparent->src_bitmap, &transparent->src_area, bbox,
                                      0, SPICE_IMAGE_SCALE_MODE_NEAREST);
    if (cairo_pattern_get_surface(pattern, &surface) != CAIRO_STATUS_SUCCESS) {
        CANVAS_ERROR("surfase from pattern pattern failed");
    }

    mask_surf = canvas_surf_to_color_maks_invers(surface, transparent->true_color);
    mask = cairo_pattern_create_for_surface(mask_surf);
    cairo_surface_destroy(mask_surf);
    if (cairo_pattern_status(mask) != CAIRO_STATUS_SUCCESS) {
        CANVAS_ERROR("create pattern failed, %s",
                     cairo_status_to_string(cairo_pattern_status(pattern)));
    }
    cairo_pattern_get_matrix(pattern, &matrix);
    cairo_pattern_set_matrix(mask, &matrix);

    cairo_set_source(cairo, pattern);
    cairo_pattern_destroy(pattern);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_COPY);
    cairo_mask(cairo, mask);
    cairo_pattern_destroy(mask);

    cairo_restore(cairo);
}

void canvas_draw_alpha_blend(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend)
{
    cairo_t *cairo = canvas->cairo;
    cairo_pattern_t *pattern;

    cairo_save(cairo);
    cairo_rectangle(cairo,
                    bbox->left,
                    bbox->top,
                    bbox->right - bbox->left,
                    bbox->bottom - bbox->top);
    cairo_clip(cairo);
    canvas_clip(canvas, clip);

    pattern = canvas_src_image_to_pat(canvas, alpha_blend->src_bitmap, &alpha_blend->src_area, bbox,
                                      0, SPICE_IMAGE_SCALE_MODE_INTERPOLATE);
    cairo_set_source(cairo, pattern);
    cairo_pattern_destroy(pattern);
    cairo_set_operator(cairo, CAIRO_OPERATOR_ATOP);
    cairo_paint_with_alpha(cairo, (double)alpha_blend->alpha / 0xff);

    cairo_restore(cairo);
}

void canvas_draw_opaque(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque)
{
    pixman_region32_t dest_region;
    SpiceROP rop;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &opaque->mask,
                       bbox->left, bbox->top);

    rop = ropd_descriptor_to_rop(opaque->rop_decriptor,
                                 ROP_INPUT_BRUSH,
                                 ROP_INPUT_SRC);

    if (rop == SPICE_ROP_NOOP || pixman_region32_n_rects(&dest_region) == 0) {
        canvas_touch_image(&canvas->base, opaque->src_bitmap);
        touch_brush(canvas, &opaque->brush);
        pixman_region32_fini(&dest_region);
        return;
    }

    if (rect_is_same_size(bbox, &opaque->src_area)) {
        blit_image(canvas, &dest_region,
                   opaque->src_bitmap,
                   bbox->left - opaque->src_area.left,
                   bbox->top - opaque->src_area.top);
    } else {
        scale_image(canvas, &dest_region,
                    opaque->src_bitmap,
                    opaque->src_area.left,
                    opaque->src_area.top,
                    opaque->src_area.right - opaque->src_area.left,
                    opaque->src_area.bottom - opaque->src_area.top,
                    bbox->left,
                    bbox->top,
                    bbox->right - bbox->left,
                    bbox->bottom - bbox->top,
                    opaque->scale_mode);
    }

    draw_brush(canvas, &dest_region, &opaque->brush, rop);

    pixman_region32_fini(&dest_region);
}

void canvas_draw_blend(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend)
{
    pixman_region32_t dest_region;
    SpiceROP rop;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &blend->mask,
                       bbox->left, bbox->top);

    rop = ropd_descriptor_to_rop(blend->rop_decriptor,
                                 ROP_INPUT_SRC,
                                 ROP_INPUT_DEST);

    if (rop == SPICE_ROP_NOOP || pixman_region32_n_rects(&dest_region) == 0) {
        canvas_touch_image(&canvas->base, blend->src_bitmap);
        pixman_region32_fini(&dest_region);
        return;
    }

    if (rect_is_same_size(bbox, &blend->src_area)) {
        if (rop == SPICE_ROP_COPY)
            blit_image(canvas, &dest_region,
                       blend->src_bitmap,
                       bbox->left - blend->src_area.left,
                       bbox->top - blend->src_area.top);
        else
            blit_image_rop(canvas, &dest_region,
                           blend->src_bitmap,
                           bbox->left - blend->src_area.left,
                           bbox->top - blend->src_area.top,
                           rop);
    } else {
        double sx, sy;

        sx = (double)(blend->src_area.right - blend->src_area.left) / (bbox->right - bbox->left);
        sy = (double)(blend->src_area.bottom - blend->src_area.top) / (bbox->bottom - bbox->top);

        if (rop == SPICE_ROP_COPY) {
            scale_image(canvas, &dest_region,
                         blend->src_bitmap,
                         blend->src_area.left,
                         blend->src_area.top,
                         blend->src_area.right - blend->src_area.left,
                         blend->src_area.bottom - blend->src_area.top,
                         bbox->left,
                         bbox->top,
                         bbox->right - bbox->left,
                         bbox->bottom - bbox->top,
                         blend->scale_mode);
        } else {
            scale_image_rop(canvas, &dest_region,
                            blend->src_bitmap,
                            blend->src_area.left,
                            blend->src_area.top,
                            blend->src_area.right - blend->src_area.left,
                            blend->src_area.bottom - blend->src_area.top,
                            bbox->left,
                            bbox->top,
                            bbox->right - bbox->left,
                            bbox->bottom - bbox->top,
                            blend->scale_mode, rop);
        }
    }

    pixman_region32_fini(&dest_region);
}

static inline void canvas_fill_common(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip,
                                      SpiceQMask *qxl_mask)
{
    cairo_t *cairo = canvas->cairo;
    cairo_pattern_t *mask;

    canvas_clip(canvas, clip);
    if ((mask = canvas_get_mask_pattern(canvas, qxl_mask, bbox->left, bbox->top))) {
        cairo_rectangle(cairo,
                        bbox->left,
                        bbox->top,
                        bbox->right - bbox->left,
                        bbox->bottom - bbox->top);
        cairo_clip(cairo);
        cairo_mask(cairo, mask);
        cairo_pattern_destroy(mask);
    } else {
        cairo_rectangle(cairo, bbox->left, bbox->top, bbox->right - bbox->left,
                        bbox->bottom - bbox->top);
        cairo_fill(cairo);
    }
}

void canvas_draw_blackness(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness)
{
    cairo_t *cairo = canvas->cairo;
    cairo_save(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    canvas_fill_common(canvas, bbox, clip, &blackness->mask);
    cairo_restore(cairo);
}

void canvas_draw_whiteness(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness)
{
    cairo_t *cairo = canvas->cairo;
    cairo_save(cairo);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    canvas_fill_common(canvas, bbox, clip, &whiteness->mask);
    cairo_restore(cairo);
}

void canvas_draw_invers(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers)
{
    cairo_t *cairo = canvas->cairo;
    cairo_save(cairo);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_XOR);
    canvas_fill_common(canvas, bbox, clip, &invers->mask);
    cairo_restore(cairo);
}

void canvas_draw_rop3(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3)
{
    cairo_t *cairo = canvas->cairo;
    cairo_pattern_t *mask;
    pixman_image_t *dd;
    cairo_surface_t *d;
    pixman_image_t *s;
    SpicePoint pos;
    int width;
    int heigth;
    double x_pos;
    double y_pos;
    SpicePoint src_pos;

    cairo_save(cairo);
    canvas_clip(canvas, clip);
    width = bbox->right - bbox->left;
    heigth = bbox->bottom - bbox->top;
    x_pos = bbox->left;
    y_pos = bbox->top;
    cairo_user_to_device(cairo, &x_pos, &y_pos);
    pos.x = (int32_t)x_pos;
    pos.y = (int32_t)y_pos;
    dd = canvas_surface_from_self(canvas, &pos, width, heigth);
    s = canvas_get_image(&canvas->base, rop3->src_bitmap);

    if (!rect_is_same_size(bbox, &rop3->src_area)) {
        pixman_image_t *scaled_s = canvas_scale_surface(s, &rop3->src_area, width, heigth,
                                                         rop3->scale_mode);
        pixman_image_unref(s);
        s = scaled_s;
        src_pos.x = 0;
        src_pos.y = 0;
    } else {
        src_pos.x = rop3->src_area.left;
        src_pos.y = rop3->src_area.top;
    }
    if (pixman_image_get_width(s) - src_pos.x < width ||
        pixman_image_get_height(s) - src_pos.y < heigth) {
        CANVAS_ERROR("bad src bitmap size");
    }
    if (rop3->brush.type == SPICE_BRUSH_TYPE_PATTERN) {
        pixman_image_t *p = canvas_get_image(&canvas->base, rop3->brush.u.pattern.pat);
        SpicePoint pat_pos;

        pat_pos.x = (bbox->left - rop3->brush.u.pattern.pos.x) % pixman_image_get_width(p);
        pat_pos.y = (bbox->top - rop3->brush.u.pattern.pos.y) % pixman_image_get_height(p);
        do_rop3_with_pattern(rop3->rop3, dd, s, &src_pos, p, &pat_pos);
        pixman_image_unref(p);
    } else {
        uint32_t color = (canvas->base.color_shift) == 8 ? rop3->brush.u.color :
                                                         canvas_16bpp_to_32bpp(rop3->brush.u.color);
        do_rop3_with_color(rop3->rop3, dd, s, &src_pos, color);
    }
    pixman_image_unref(s);
    d = surface_from_pixman_image (dd);
    cairo_set_source_surface(cairo, d, bbox->left, bbox->top);
    cairo_surface_destroy(d);
    pixman_image_unref (dd);
    if ((mask = canvas_get_mask_pattern(canvas, &rop3->mask, bbox->left, bbox->top))) {
        cairo_rectangle(cairo,
                        bbox->left,
                        bbox->top,
                        bbox->right - bbox->left,
                        bbox->bottom - bbox->top);
        cairo_clip(cairo);
        cairo_mask(cairo, mask);
        cairo_pattern_destroy(mask);
    } else {
        cairo_rectangle(cairo, bbox->left, bbox->top, bbox->right - bbox->left,
                        bbox->bottom - bbox->top);
        cairo_fill(cairo);
    }
    cairo_restore(cairo);
}

#define FAST_COPY_BITS

#ifdef FAST_COPY_BITS

static inline void __canvas_copy_bits_up(uint8_t *data, const int stride,
                                         const int src_x, const int src_y,
                                         const int width, const int height,
                                         const int dest_x, const int dest_y)
{
    uint8_t *src = data + src_y * stride + src_x * sizeof(uint32_t);
    uint8_t *dest = data + dest_y * stride + dest_x * sizeof(uint32_t);
    uint8_t *end = dest + height * stride;
    for (; dest != end; dest += stride, src += stride) {
        memcpy(dest, src, width * sizeof(uint32_t));
    }
}

static inline void __canvas_copy_bits_down(uint8_t *data, const int stride,
                                           const int src_x, const int src_y,
                                           const int width, const int height,
                                           const int dest_x, const int dest_y)
{
    uint8_t *src = data + (src_y + height - 1) * stride + src_x * sizeof(uint32_t);
    uint8_t *end = data + (dest_y - 1) * stride + dest_x * sizeof(uint32_t);
    uint8_t *dest = end + height * stride;

    for (; dest != end; dest -= stride, src -= stride) {
        memcpy(dest, src, width * sizeof(uint32_t));
    }
}

static inline void __canvas_copy_bits_right(uint8_t *data, const int stride,
                                            const int src_x, const int src_y,
                                            const int width, const int height,
                                            const int dest_x, const int dest_y)
{
    uint8_t *src = data + src_y * stride + (src_x + width - 1) * sizeof(uint32_t);
    uint8_t *dest = data + dest_y * stride + (dest_x + width - 1) * sizeof(uint32_t);
    uint8_t *end = dest + height * stride;
    for (; dest != end; dest += stride, src += stride) {
        uint32_t *src_pix = (uint32_t *)src;
        uint32_t *end_pix = src_pix - width;
        uint32_t *dest_pix = (uint32_t *)dest;

        for (; src_pix > end_pix; src_pix--, dest_pix--) {
            *dest_pix = *src_pix;
        }
    }
}

static inline void __canvas_copy_rect_bits(uint8_t *data, const int stride, SpiceRect *dest_rect,
                                           SpicePoint *src_pos)
{
    if (dest_rect->top > src_pos->y) {
        __canvas_copy_bits_down(data, stride, src_pos->x, src_pos->y,
                                dest_rect->right - dest_rect->left,
                                dest_rect->bottom - dest_rect->top,
                                dest_rect->left, dest_rect->top);
    } else if (dest_rect->top < src_pos->y || dest_rect->left < src_pos->x) {
        __canvas_copy_bits_up(data, stride, src_pos->x, src_pos->y,
                              dest_rect->right - dest_rect->left,
                              dest_rect->bottom - dest_rect->top,
                              dest_rect->left, dest_rect->top);
    } else {
        __canvas_copy_bits_right(data, stride, src_pos->x, src_pos->y,
                                 dest_rect->right - dest_rect->left,
                                 dest_rect->bottom - dest_rect->top,
                                 dest_rect->left, dest_rect->top);
    }
}

static inline void canvas_copy_fix_clip_area(const SpiceRect *dest,
                                             const SpicePoint *src_pos,
                                             const SpiceRect *now,
                                             SpicePoint *ret_pos,
                                             SpiceRect *ret_dest)
{
    *ret_dest = *now;
    rect_sect(ret_dest, dest);
    ret_pos->x = src_pos->x + (ret_dest->left - dest->left);
    ret_pos->y = src_pos->y + (ret_dest->top - dest->top);
}

static inline void __canvas_copy_region_bits(uint8_t *data, int stride, SpiceRect *dest_rect,
                                             SpicePoint *src_pos, QRegion *region)
{
    SpiceRect curr_area;
    SpicePoint curr_pos;
    SpiceRect *now;
    SpiceRect *end;

    if (dest_rect->top > src_pos->y) {
        end = region->rects - 1;
        now = end + region->num_rects;
        if (dest_rect->left < src_pos->x) {
            for (; now > end; now--) {
                SpiceRect *line_end = now;
                SpiceRect *line_pos;

                while (now - 1 > end && now->top == now[-1].top) {
                    now--;
                }

                for (line_pos = now; line_pos <= line_end; line_pos++) {
                    canvas_copy_fix_clip_area(dest_rect, src_pos, line_pos, &curr_pos, &curr_area);
                    __canvas_copy_bits_down(data, stride, curr_pos.x, curr_pos.y,
                                            curr_area.right - curr_area.left,
                                            curr_area.bottom - curr_area.top,
                                            curr_area.left, curr_area.top);
                }
            }
        } else {
            for (; now > end; now--) {
                canvas_copy_fix_clip_area(dest_rect, src_pos, now, &curr_pos, &curr_area);
                __canvas_copy_bits_down(data, stride, curr_pos.x, curr_pos.y,
                                        curr_area.right - curr_area.left,
                                        curr_area.bottom - curr_area.top,
                                        curr_area.left, curr_area.top);
            }
        }
    } else if (dest_rect->top < src_pos->y || dest_rect->left < src_pos->x) {
        now = region->rects;
        end = now + region->num_rects;
        if (dest_rect->left > src_pos->x) {
            for (; now < end; now++) {
                SpiceRect *line_end = now;
                SpiceRect *line_pos;

                while (now + 1 < end && now->top == now[1].top) {
                    now++;
                }

                for (line_pos = now; line_pos >= line_end; line_pos--) {
                    canvas_copy_fix_clip_area(dest_rect, src_pos, line_pos, &curr_pos, &curr_area);
                    __canvas_copy_bits_up(data, stride, curr_pos.x, curr_pos.y,
                                          curr_area.right - curr_area.left,
                                          curr_area.bottom - curr_area.top,
                                          curr_area.left, curr_area.top);
                }
            }
        } else {
            for (; now < end; now++) {
                canvas_copy_fix_clip_area(dest_rect, src_pos, now, &curr_pos, &curr_area);
                __canvas_copy_bits_up(data, stride, curr_pos.x, curr_pos.y,
                                      curr_area.right - curr_area.left,
                                      curr_area.bottom - curr_area.top,
                                      curr_area.left, curr_area.top);
            }
        }
    } else {
        end = region->rects - 1;
        now = end + region->num_rects;
        for (; now > end; now--) {
            canvas_copy_fix_clip_area(dest_rect, src_pos, now, &curr_pos, &curr_area);
            __canvas_copy_bits_right(data, stride, curr_pos.x, curr_pos.y,
                                     curr_area.right - curr_area.left,
                                     curr_area.bottom - curr_area.top,
                                     curr_area.left, curr_area.top);
        }
    }
}

#endif

void canvas_copy_bits(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos)
{
    cairo_t *cairo = canvas->cairo;
    cairo_surface_t *cairo_surface;
    pixman_image_t *surface;
    int32_t width;
    int32_t heigth;

    cairo_save(cairo);
#ifdef FAST_COPY_BITS
    switch (clip->type) {
    case SPICE_CLIP_TYPE_NONE: {
        __canvas_copy_rect_bits((uint8_t *)pixman_image_get_data(canvas->image),
                                pixman_image_get_stride(canvas->image),
                                bbox, src_pos);
        break;
    }
    case SPICE_CLIP_TYPE_RECTS: {
        uint32_t *n = (uint32_t *)SPICE_GET_ADDRESS(clip->data);
        access_test(&canvas->base, n, sizeof(uint32_t));

        SpiceRect *now = (SpiceRect *)(n + 1);
        SpiceRect *end = now + *n;
        access_test(&canvas->base, now, (unsigned long)end - (unsigned long)now);
        uint8_t *data = (uint8_t *)pixman_image_get_data(canvas->image);
        int stride = pixman_image_get_stride(canvas->image);

        //using QRegion in order to sort and remove intersections
        QRegion region;
        region_init(&region);
        for (; now < end; now++) {
            region_add(&region, now);
        }
        __canvas_copy_region_bits(data, stride, bbox, src_pos, &region);
        region_destroy(&region);
        break;
    }
    default:
#endif
        canvas_clip(canvas, clip);

        width = bbox->right - bbox->left;
        heigth = bbox->bottom - bbox->top;
        surface = canvas_surface_from_self(canvas, src_pos, width, heigth);
        cairo_surface = surface_from_pixman_image (surface);
        cairo_set_source_surface(cairo, cairo_surface, bbox->left, bbox->top);
        cairo_rectangle(cairo, bbox->left, bbox->top, width, heigth);
        cairo_set_operator(cairo, CAIRO_OPERATOR_RASTER_COPY);
        cairo_fill(cairo);
        cairo_surface_destroy(cairo_surface);
        pixman_image_unref (surface);
#ifdef FAST_COPY_BITS
    }

#endif
    cairo_restore(cairo);
}

static void canvas_draw_raster_str(CairoCanvas *canvas, SpiceString *str, int bpp,
                                   SpiceBrush *brush, uint16_t rop_decriptor)
{
    pixman_image_t *str_mask;
    cairo_surface_t *cairo_str_mask;
    DrawMaskData draw_data;
    cairo_matrix_t matrix;
    SpicePoint pos;

    str_mask = canvas_get_str_mask(&canvas->base, str, bpp, &pos);
    cairo_str_mask = surface_from_pixman_image (str_mask);
    draw_data.cairo = canvas->cairo;
    draw_data.mask = cairo_pattern_create_for_surface(cairo_str_mask);
    if (cairo_pattern_status(draw_data.mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cairo_str_mask);
        CANVAS_ERROR("create pattern failed, %s",
                     cairo_status_to_string(cairo_pattern_status(draw_data.mask)));
    }
    cairo_matrix_init_translate(&matrix, -pos.x, -pos.y);
    cairo_pattern_set_matrix(draw_data.mask, &matrix);
    canvas_draw(canvas, brush, rop_decriptor, __draw_mask, &draw_data);
    cairo_pattern_destroy(draw_data.mask);
    pixman_image_unref(str_mask);
    cairo_surface_destroy(cairo_str_mask);
}

static void canvas_draw_vector_str(CairoCanvas *canvas, SpiceString *str, SpiceBrush *brush,
                                   uint16_t rop_decriptor)
{
    SpiceVectorGlyph *glyph = (SpiceVectorGlyph *)str->data;
    int i;

    for (i = 0; i < str->length; i++) {
        SpiceVectorGlyph *next_glyph = canvas_next_vector_glyph(glyph);
        access_test(&canvas->base, glyph, (uint8_t *)next_glyph - (uint8_t *)glyph);
        canvas_set_path(canvas, glyph->data);
        glyph = next_glyph;
    }
    canvas_draw(canvas, brush, rop_decriptor, (DrawMethod)cairo_fill_preserve, canvas->cairo);
    cairo_new_path(canvas->cairo);
}

void canvas_draw_text(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceText *text)
{
    cairo_t *cairo = canvas->cairo;
    SpiceString *str;

    cairo_save(cairo);
    canvas_clip(canvas, clip);
    if (!rect_is_empty(&text->back_area)) {
        cairo_rectangle(cairo,
                        text->back_area.left,
                        text->back_area.top,
                        text->back_area.right - text->back_area.left,
                        text->back_area.bottom - text->back_area.top);
        canvas_draw(canvas, &text->back_brush, text->back_mode,
                    (DrawMethod)cairo_fill_preserve, cairo);
        cairo_new_path(cairo);
    }
    str = (SpiceString *)SPICE_GET_ADDRESS(text->str);

    if (str->flags & SPICE_STRING_FLAGS_RASTER_A1) {
        canvas_draw_raster_str(canvas, str, 1, &text->fore_brush, text->fore_mode);
    } else if (str->flags & SPICE_STRING_FLAGS_RASTER_A4) {
        canvas_draw_raster_str(canvas, str, 4, &text->fore_brush, text->fore_mode);
    } else if (str->flags & SPICE_STRING_FLAGS_RASTER_A8) {
        WARN("untested path A8 glyphs, doing nothing");
        if (0) {
            canvas_draw_raster_str(canvas, str, 8, &text->fore_brush, text->fore_mode);
        }
    } else {
        WARN("untested path vector glyphs, doing nothing");
        if (0) {
            canvas_draw_vector_str(canvas, str, &text->fore_brush, text->fore_mode);
        }
    }
    cairo_restore(cairo);
}

void canvas_draw_stroke(CairoCanvas *canvas, SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke)
{
    cairo_t *cairo = canvas->cairo;

    cairo_save(cairo);
    canvas_clip(canvas, clip);

    canvas_set_line_attr_no_dash(canvas, &stroke->attr);
    canvas_set_path(canvas, SPICE_GET_ADDRESS(stroke->path));
    if (stroke->attr.flags & SPICE_LINE_ATTR_STYLED) {
        canvas_draw(canvas, &stroke->brush, stroke->back_mode,
                    (DrawMethod)cairo_stroke_preserve, cairo);
        canvas_set_dash(canvas, stroke->attr.style_nseg, stroke->attr.style,
                        !!(stroke->attr.flags & SPICE_LINE_ATTR_STARTGAP));
    }
    canvas_draw(canvas, &stroke->brush, stroke->fore_mode, (DrawMethod)cairo_stroke_preserve,
                cairo);
    cairo_new_path(cairo);
    cairo_restore(cairo);
}

void canvas_read_bits(CairoCanvas *canvas, uint8_t *dest, int dest_stride, const SpiceRect *area)
{
    cairo_t *cairo = canvas->cairo;
    cairo_surface_t* surface;
    uint8_t *src;
    int src_stride;
    uint8_t *dest_end;

    ASSERT(canvas && area);

    surface = cairo_get_target(cairo);
    src_stride = cairo_image_surface_get_stride(surface);
    src = cairo_image_surface_get_data(surface) + area->top * src_stride +
                                                  area->left * sizeof(uint32_t);
    dest_end = dest + (area->bottom - area->top) * dest_stride;
    for (; dest != dest_end; dest += dest_stride, src += src_stride) {
        memcpy(dest, src, dest_stride);
    }
}

void canvas_group_start(CairoCanvas *canvas, int n_clip_rects, SpiceRect *clip_rects)
{
    cairo_t *cairo = canvas->cairo;

    cairo_save(cairo);

    if (n_clip_rects) {
        SpiceRect *end = clip_rects + n_clip_rects;
        for (; clip_rects < end; clip_rects++) {
            cairo_rectangle(cairo,
                            clip_rects->left,
                            clip_rects->top,
                            clip_rects->right - clip_rects->left,
                            clip_rects->bottom - clip_rects->top);
        }
        cairo_clip(cairo);
    }
}

void canvas_group_end(CairoCanvas *canvas)
{
    cairo_restore(canvas->cairo);
}

void canvas_clear(CairoCanvas *canvas)
{
    cairo_t *cairo = canvas->cairo;

    ASSERT(cairo);
    cairo_save(cairo);
    cairo_reset_clip(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

cairo_t *canvas_get_cairo(CairoCanvas *canvas)
{
    return canvas->cairo;
}

#ifdef CAIRO_CANVAS_ACCESS_TEST
void canvas_set_access_params(CairoCanvas *canvas, unsigned long base, unsigned long max)
{
    __canvas_set_access_params(&canvas->base, base, max);
}
#endif

void canvas_destroy(CairoCanvas *canvas)
{
    if (!canvas) {
        return;
    }
    pixman_image_unref(canvas->image);
    canvas_base_destroy(&canvas->base);
    if (canvas->private_data) {
        free(canvas->private_data);
    }
    free(canvas);
}

static int need_init = 1;

#ifdef CAIRO_CANVAS_CACHE
CairoCanvas *canvas_create(cairo_t *cairo, int bits,
                           SpiceImageCache *bits_cache,
                           SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
CairoCanvas *canvas_create(cairo_t *cairo, int bits,
                           SpiceImageCache *bits_cache
#else
CairoCanvas *canvas_create(cairo_t *cairo, int bits
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
    CairoCanvas *canvas;
    int init_ok;

    if (need_init || !(canvas = (CairoCanvas *)malloc(sizeof(CairoCanvas)))) {
        return NULL;
    }
    memset(canvas, 0, sizeof(CairoCanvas));
#ifdef CAIRO_CANVAS_CACHE
    init_ok = canvas_base_init(&canvas->base, bits,
                               bits_cache,
                               palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
    init_ok = canvas_base_init(&canvas->base, bits,
                               bits_cache
#else
    init_ok = canvas_base_init(&canvas->base, bits
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
    canvas->cairo = cairo;
    canvas->private_data = NULL;
    canvas->private_data_size = 0;
    cairo_set_antialias(cairo, CAIRO_ANTIALIAS_NONE);

    canvas->image = pixman_image_from_surface (cairo_get_target (cairo));
    pixman_region32_init_rect(&canvas->canvas_region,
                              0, 0,
                              pixman_image_get_width (canvas->image),
                              pixman_image_get_height (canvas->image));

    return canvas;
}

void cairo_canvas_init() //unsafe global function
{
    if (!need_init) {
        return;
    }
    need_init = 0;
    rop3_init();
}


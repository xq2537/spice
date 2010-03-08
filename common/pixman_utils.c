/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
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

#include "pixman_utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef FALSE
#   define FALSE 0
#endif

#ifndef TRUE
#   define TRUE 1
#endif

#ifndef ASSERT
#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    abort();                                                \
}
#endif

#define SOLID_RASTER_OP(_name, _size, _type, _equation)  \
static void                                        \
solid_rop_ ## _name ## _ ## _size (_type *ptr, int len, _type src)  \
{                                                  \
    while (len--) {                                \
        _type dst = *ptr;                          \
        if (dst) /* avoid unused warning */{};       \
        *ptr = (_type)(_equation);                 \
        ptr++;                                     \
    }                                              \
}                                                  \

#define TILED_RASTER_OP(_name, _size, _type, _equation) \
static void                                        \
tiled_rop_ ## _name ## _ ## _size (_type *ptr, int len, _type *tile, _type *tile_end, int tile_width)   \
{                                                  \
    while (len--) {                                \
        _type src = *tile;                         \
        _type dst = *ptr;                          \
        if (src) /* avoid unused warning */{};       \
        if (dst) /* avoid unused warning */{};       \
        *ptr = (_type)(_equation);                 \
        ptr++;                                     \
        tile++;                                    \
        if (tile == tile_end)                      \
            tile -= tile_width;                    \
    }                                              \
}                                                  \

#define COPY_RASTER_OP(_name, _size, _type, _equation) \
static void                                        \
 copy_rop_ ## _name ## _ ## _size (_type *ptr, _type *src_line, int len)        \
{                                                  \
    while (len--) {                                \
        _type src = *src_line;                     \
        _type dst = *ptr;                          \
        if (src) /* avoid unused warning */ {};       \
        if (dst) /* avoid unused warning */{};       \
        *ptr = (_type)(_equation);                 \
        ptr++;                                     \
        src_line++;                                \
    }                                              \
}                                                  \

#define RASTER_OP(name, equation) \
    SOLID_RASTER_OP(name, 8, uint8_t, equation) \
    SOLID_RASTER_OP(name, 16, uint16_t, equation) \
    SOLID_RASTER_OP(name, 32, uint32_t, equation) \
    TILED_RASTER_OP(name, 8, uint8_t, equation) \
    TILED_RASTER_OP(name, 16, uint16_t, equation) \
    TILED_RASTER_OP(name, 32, uint32_t, equation) \
    COPY_RASTER_OP(name, 8, uint8_t, equation) \
    COPY_RASTER_OP(name, 16, uint16_t, equation) \
    COPY_RASTER_OP(name, 32, uint32_t, equation)

RASTER_OP(clear, 0x0)
RASTER_OP(and, src & dst)
RASTER_OP(and_reverse, src & ~dst)
RASTER_OP(copy, src)
RASTER_OP(and_inverted, ~src & dst)
RASTER_OP(noop, dst)
RASTER_OP(xor, src ^ dst)
RASTER_OP(or, src | dst)
RASTER_OP(nor, ~src & ~dst)
RASTER_OP(equiv, ~src ^ dst)
RASTER_OP(invert, ~dst)
RASTER_OP(or_reverse, src | ~dst)
RASTER_OP(copy_inverted, ~src)
RASTER_OP(or_inverted, ~src | dst)
RASTER_OP(nand, ~src | ~dst)
RASTER_OP(set, 0xffffffff)

typedef void (*solid_rop_8_func_t)(uint8_t *ptr, int len, uint8_t src);
typedef void (*solid_rop_16_func_t)(uint16_t *ptr, int len, uint16_t src);
typedef void (*solid_rop_32_func_t)(uint32_t *ptr, int len, uint32_t src);
typedef void (*tiled_rop_8_func_t)(uint8_t *ptr, int len,
                                   uint8_t *tile, uint8_t *tile_end, int tile_width);
typedef void (*tiled_rop_16_func_t)(uint16_t *ptr, int len,
                                    uint16_t *tile, uint16_t *tile_end, int tile_width);
typedef void (*tiled_rop_32_func_t)(uint32_t *ptr, int len,
                                    uint32_t *tile, uint32_t *tile_end, int tile_width);
typedef void (*copy_rop_8_func_t)(uint8_t *ptr, uint8_t *src, int len);
typedef void (*copy_rop_16_func_t)(uint16_t *ptr, uint16_t *src, int len);
typedef void (*copy_rop_32_func_t)(uint32_t *ptr, uint32_t *src, int len);

#define ROP_TABLE(_type, _size)                                                 \
static void (*solid_rops_ ## _size[16]) (_type *ptr, int len, _type src) = { \
    solid_rop_clear_ ## _size,  \
    solid_rop_and_ ## _size,    \
    solid_rop_and_reverse_ ## _size,    \
    solid_rop_copy_ ## _size,    \
    solid_rop_and_inverted_ ## _size,    \
    solid_rop_noop_ ## _size,    \
    solid_rop_xor_ ## _size,    \
    solid_rop_or_ ## _size,    \
    solid_rop_nor_ ## _size,    \
    solid_rop_equiv_ ## _size,    \
    solid_rop_invert_ ## _size,    \
    solid_rop_or_reverse_ ## _size,    \
    solid_rop_copy_inverted_ ## _size,    \
    solid_rop_or_inverted_ ## _size,    \
    solid_rop_nand_ ## _size,    \
    solid_rop_set_ ## _size    \
};                          \
static void (*tiled_rops_ ## _size[16]) (_type *ptr, int len, _type *tile, _type *tile_end, int tile_width) = { \
    tiled_rop_clear_ ## _size,  \
    tiled_rop_and_ ## _size,    \
    tiled_rop_and_reverse_ ## _size,    \
    tiled_rop_copy_ ## _size,    \
    tiled_rop_and_inverted_ ## _size,    \
    tiled_rop_noop_ ## _size,    \
    tiled_rop_xor_ ## _size,    \
    tiled_rop_or_ ## _size,    \
    tiled_rop_nor_ ## _size,    \
    tiled_rop_equiv_ ## _size,    \
    tiled_rop_invert_ ## _size,    \
    tiled_rop_or_reverse_ ## _size,    \
    tiled_rop_copy_inverted_ ## _size,    \
    tiled_rop_or_inverted_ ## _size,    \
    tiled_rop_nand_ ## _size,    \
    tiled_rop_set_ ## _size    \
}; \
static void (*copy_rops_ ## _size[16]) (_type *ptr, _type *tile, int len) = { \
    copy_rop_clear_ ## _size,  \
    copy_rop_and_ ## _size,    \
    copy_rop_and_reverse_ ## _size,    \
    copy_rop_copy_ ## _size,    \
    copy_rop_and_inverted_ ## _size,    \
    copy_rop_noop_ ## _size,    \
    copy_rop_xor_ ## _size,    \
    copy_rop_or_ ## _size,    \
    copy_rop_nor_ ## _size,    \
    copy_rop_equiv_ ## _size,    \
    copy_rop_invert_ ## _size,    \
    copy_rop_or_reverse_ ## _size,    \
    copy_rop_copy_inverted_ ## _size,    \
    copy_rop_or_inverted_ ## _size,    \
    copy_rop_nand_ ## _size,    \
    copy_rop_set_ ## _size    \
};

ROP_TABLE(uint8_t, 8)
ROP_TABLE(uint16_t, 16)
ROP_TABLE(uint32_t, 32)

void spice_pixman_fill_rect(pixman_image_t *dest,
                            int x, int y,
                            int width, int height,
                            uint32_t value)
{
    uint32_t *bits;
    int stride, depth;
    uint32_t byte_width;
    uint8_t *byte_line;

    bits = pixman_image_get_data(dest);
    stride = pixman_image_get_stride(dest);
    depth = pixman_image_get_depth(dest);
    /* stride is in bytes, depth in bits */

    ASSERT(x >= 0);
    ASSERT(y >= 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(x + width <= pixman_image_get_width(dest));
    ASSERT(y + height <= pixman_image_get_height(dest));

    if (depth == 24) {
        depth = 32; /* Needed for pixman_fill */
    }

    if (pixman_fill(bits,
                    stride / 4,
                    depth,
                    x, y,
                    width, height,
                    value)) {
        return;
    }

    if (depth == 8) {
        byte_line = ((uint8_t *)bits) + stride * y + x;
        byte_width = width;
        value = (value & 0xff) * 0x01010101;
    } else if (depth == 16) {
        byte_line = ((uint8_t *)bits) + stride * y + x * 2;
        byte_width = 2 * width;
        value = (value & 0xffff) * 0x00010001;
    } else {
        ASSERT (depth == 32 || depth == 24)
        byte_line = ((uint8_t *)bits) + stride * y + x * 4;
        byte_width = 4 * width;
    }

    while (height--) {
        int w;
        uint8_t *d = byte_line;

        byte_line += stride;
        w = byte_width;

        while (w >= 1 && ((unsigned long)d & 1)) {
            *(uint8_t *)d = (value & 0xff);
            w--;
            d++;
        }

        while (w >= 2 && ((unsigned long)d & 3)) {
            *(uint16_t *)d = value;
            w -= 2;
            d += 2;
        }

        while (w >= 4 && ((unsigned long)d & 7)) {
            *(uint32_t *)d = value;

            w -= 4;
            d += 4;
        }

        while (w >= 4) {
            *(uint32_t *)d = value;

            w -= 4;
            d += 4;
        }

        while (w >= 2) {
            *(uint16_t *)d = value;
            w -= 2;
            d += 2;
        }

        while (w >= 1) {
            *(uint8_t *)d = (value & 0xff);
            w--;
            d++;
        }
    }
}

void spice_pixman_fill_rect_rop(pixman_image_t *dest,
                                int x, int y,
                                int width, int height,
                                uint32_t value,
                                SpiceROP rop)
{
    uint32_t *bits;
    int stride, depth;
    uint8_t *byte_line;

    bits = pixman_image_get_data(dest);
    stride = pixman_image_get_stride(dest);
    depth = pixman_image_get_depth(dest);
    /* stride is in bytes, depth in bits */

    ASSERT(x >= 0);
    ASSERT(y >= 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(x + width <= pixman_image_get_width(dest));
    ASSERT(y + height <= pixman_image_get_height(dest));
    ASSERT(rop >= 0 && rop < 16);

    if (depth == 8) {
        solid_rop_8_func_t rop_func = solid_rops_8[rop];

        byte_line = ((uint8_t *)bits) + stride * y + x;
        while (height--) {
            rop_func((uint8_t *)byte_line, width, (uint8_t)value);
            byte_line += stride;
        }

    } else if (depth == 16) {
        solid_rop_16_func_t rop_func = solid_rops_16[rop];

        byte_line = ((uint8_t *)bits) + stride * y + x * 2;
        while (height--) {
            rop_func((uint16_t *)byte_line, width, (uint16_t)value);
            byte_line += stride;
        }
    }  else {
        solid_rop_32_func_t rop_func = solid_rops_32[rop];

        ASSERT (depth == 32 || depth == 24);

        byte_line = ((uint8_t *)bits) + stride * y + x * 4;
        while (height--) {
            rop_func((uint32_t *)byte_line, width, (uint32_t)value);
            byte_line += stride;
        }
    }
}

void spice_pixman_tile_rect(pixman_image_t *dest,
                            int x, int y,
                            int width, int height,
                            pixman_image_t *tile,
                            int offset_x,
                            int offset_y)
{
    uint32_t *bits, *tile_bits;
    int stride, depth;
    int tile_width, tile_height, tile_stride;
    uint8_t *byte_line;
    uint8_t *tile_line;
    int tile_start_x, tile_start_y, tile_end_dx;

    bits = pixman_image_get_data(dest);
    stride = pixman_image_get_stride(dest);
    depth = pixman_image_get_depth(dest);
    /* stride is in bytes, depth in bits */

    tile_bits = pixman_image_get_data(tile);
    tile_stride = pixman_image_get_stride(tile);
    tile_width = pixman_image_get_width(tile);
    tile_height = pixman_image_get_height(tile);

    ASSERT(x >= 0);
    ASSERT(y >= 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(x + width <= pixman_image_get_width(dest));
    ASSERT(y + height <= pixman_image_get_height(dest));
    ASSERT(depth == pixman_image_get_depth(tile));

    tile_start_x = (x - offset_x) % tile_width;
    if (tile_start_x < 0) {
        tile_start_x += tile_width;
    }
    tile_start_y = (y - offset_y) % tile_height;
    if (tile_start_y < 0) {
        tile_start_y += tile_height;
    }
    tile_end_dx = tile_width - tile_start_x;

    if (depth == 8) {
        byte_line = ((uint8_t *)bits) + stride * y + x;
        tile_line = ((uint8_t *)tile_bits) + tile_stride * tile_start_y + tile_start_x;
        while (height--) {
            tiled_rop_copy_8((uint8_t *)byte_line, width,
                             (uint8_t *)tile_line, (uint8_t *)tile_line + tile_end_dx,
                             tile_width);
            byte_line += stride;
            tile_line += tile_stride;
            if (++tile_start_y == tile_height) {
                tile_line -= tile_height * tile_stride;
                tile_start_y = 0;
            }
        }

    } else if (depth == 16) {
        byte_line = ((uint8_t *)bits) + stride * y + x * 2;
        tile_line = ((uint8_t *)tile_bits) + tile_stride * tile_start_y + tile_start_x * 2;
        while (height--) {
            tiled_rop_copy_16((uint16_t *)byte_line, width,
                              (uint16_t *)tile_line, (uint16_t *)tile_line + tile_end_dx,
                              tile_width);
            byte_line += stride;
            tile_line += tile_stride;
            if (++tile_start_y == tile_height) {
                tile_line -= tile_height * tile_stride;
                tile_start_y = 0;
            }
        }
    }  else {
        ASSERT (depth == 32 || depth == 24);

        byte_line = ((uint8_t *)bits) + stride * y + x * 4;
        tile_line = ((uint8_t *)tile_bits) + tile_stride * tile_start_y + tile_start_x * 4;
        while (height--) {
            tiled_rop_copy_32((uint32_t *)byte_line, width,
                              (uint32_t *)tile_line, (uint32_t *)tile_line + tile_end_dx,
                              tile_width);
            byte_line += stride;
            tile_line += tile_stride;
            if (++tile_start_y == tile_height) {
                tile_line -= tile_height * tile_stride;
                tile_start_y = 0;
            }
        }
    }
}

void spice_pixman_tile_rect_rop(pixman_image_t *dest,
                                int x, int y,
                                int width, int height,
                                pixman_image_t *tile,
                                int offset_x,
                                int offset_y,
                                SpiceROP rop)
{
    uint32_t *bits, *tile_bits;
    int stride, depth;
    int tile_width, tile_height, tile_stride;
    uint8_t *byte_line;
    uint8_t *tile_line;
    int tile_start_x, tile_start_y, tile_end_dx;

    bits = pixman_image_get_data(dest);
    stride = pixman_image_get_stride(dest);
    depth = pixman_image_get_depth(dest);
    /* stride is in bytes, depth in bits */

    tile_bits = pixman_image_get_data(tile);
    tile_stride = pixman_image_get_stride(tile);
    tile_width = pixman_image_get_width(tile);
    tile_height = pixman_image_get_height(tile);

    ASSERT(x >= 0);
    ASSERT(y >= 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(x + width <= pixman_image_get_width(dest));
    ASSERT(y + height <= pixman_image_get_height(dest));
    ASSERT(rop >= 0 && rop < 16);
    ASSERT(depth == pixman_image_get_depth(tile));

    tile_start_x = (x - offset_x) % tile_width;
    if (tile_start_x < 0) {
        tile_start_x += tile_width;
    }
    tile_start_y = (y - offset_y) % tile_height;
    if (tile_start_y < 0) {
        tile_start_y += tile_height;
    }
    tile_end_dx = tile_width - tile_start_x;

    if (depth == 8) {
        tiled_rop_8_func_t rop_func = tiled_rops_8[rop];

        byte_line = ((uint8_t *)bits) + stride * y + x;
        tile_line = ((uint8_t *)tile_bits) + tile_stride * tile_start_y + tile_start_x;
        while (height--) {
            rop_func((uint8_t *)byte_line, width,
                     (uint8_t *)tile_line, (uint8_t *)tile_line + tile_end_dx,
                     tile_width);
            byte_line += stride;
            tile_line += tile_stride;
            if (++tile_start_y == tile_height) {
                tile_line -= tile_height * tile_stride;
                tile_start_y = 0;
            }
        }

    } else if (depth == 16) {
        tiled_rop_16_func_t rop_func = tiled_rops_16[rop];

        byte_line = ((uint8_t *)bits) + stride * y + x * 2;
        tile_line = ((uint8_t *)tile_bits) + tile_stride * tile_start_y + tile_start_x * 2;
        while (height--) {
            rop_func((uint16_t *)byte_line, width,
                     (uint16_t *)tile_line, (uint16_t *)tile_line + tile_end_dx,
                     tile_width);
            byte_line += stride;
            tile_line += tile_stride;
            if (++tile_start_y == tile_height) {
                tile_line -= tile_height * tile_stride;
                tile_start_y = 0;
            }
        }
    }  else {
        tiled_rop_32_func_t rop_func = tiled_rops_32[rop];

        ASSERT (depth == 32 || depth == 24);

        byte_line = ((uint8_t *)bits) + stride * y + x * 4;
        tile_line = ((uint8_t *)tile_bits) + tile_stride * tile_start_y + tile_start_x * 4;
        while (height--) {
            rop_func((uint32_t *)byte_line, width,
                     (uint32_t *)tile_line, (uint32_t *)tile_line + tile_end_dx,
                     tile_width);
            byte_line += stride;
            tile_line += tile_stride;
            if (++tile_start_y == tile_height) {
                tile_line -= tile_height * tile_stride;
                tile_start_y = 0;
            }
        }
    }
}


void spice_pixman_blit(pixman_image_t *dest,
                       pixman_image_t *src,
                       int src_x, int src_y,
                       int dest_x, int dest_y,
                       int width, int height)
{
    uint32_t *bits, *src_bits;
    int stride, depth;
    int src_width, src_height, src_stride;
    uint8_t *byte_line;
    uint8_t *src_line;
    int byte_width;

    bits = pixman_image_get_data(dest);
    stride = pixman_image_get_stride(dest);
    depth = pixman_image_get_depth(dest);
    /* stride is in bytes, depth in bits */

    src_bits = pixman_image_get_data(src);
    src_stride = pixman_image_get_stride(src);
    src_width = pixman_image_get_width(src);
    src_height = pixman_image_get_height(src);

    /* Clip source */
    if (src_x < 0) {
        width += src_x;
        dest_x -= src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        height += src_y;
        dest_y -= src_y;
        src_y = 0;
    }
    if (src_x + width > src_width) {
        width = src_width - src_x;
    }
    if (src_y + height > src_height) {
        height = src_height - src_y;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    ASSERT(src_x >= 0);
    ASSERT(src_y >= 0);
    ASSERT(dest_x >= 0);
    ASSERT(dest_y >= 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(dest_x + width <= pixman_image_get_width(dest));
    ASSERT(dest_y + height <= pixman_image_get_height(dest));
    ASSERT(src_x + width <= pixman_image_get_width(src));
    ASSERT(src_y + height <= pixman_image_get_height(src));
    ASSERT(depth == pixman_image_get_depth(src));

    if (depth == 24) {
        depth = 32; /* Needed for pixman_blt */
    }

    if (pixman_blt(src_bits,
                   bits,
                   src_stride / 4,
                   stride / 4,
                   depth, depth,
                   src_x, src_y,
                   dest_x, dest_y,
                   width, height)) {
        return;
    }

    if (depth == 8) {
        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x;
        byte_width = width;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x;
    } else if (depth == 16) {
        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x * 2;
        byte_width = width * 2;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x * 2;
    }  else {
        ASSERT (depth == 32 || depth == 24);
        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x * 4;
        byte_width = width * 4;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x * 4;
    }

    while (height--) {
        memcpy(byte_line, src_line, byte_width);
        byte_line += stride;
        src_line += src_stride;
    }
}

void spice_pixman_blit_rop (pixman_image_t *dest,
                            pixman_image_t *src,
                            int src_x, int src_y,
                            int dest_x, int dest_y,
                            int width, int height,
                            SpiceROP rop)
{
    uint32_t *bits, *src_bits;
    int stride, depth;
    int src_width, src_height, src_stride;
    uint8_t *byte_line;
    uint8_t *src_line;

    bits = pixman_image_get_data(dest);
    stride = pixman_image_get_stride(dest);
    depth = pixman_image_get_depth(dest);
    /* stride is in bytes, depth in bits */

    src_bits = pixman_image_get_data(src);
    src_stride = pixman_image_get_stride(src);
    src_width = pixman_image_get_width(src);
    src_height = pixman_image_get_height(src);

    /* Clip source */
    if (src_x < 0) {
        width += src_x;
        dest_x -= src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        height += src_y;
        dest_y -= src_y;
        src_y = 0;
    }
    if (src_x + width > src_width) {
        width = src_width - src_x;
    }
    if (src_y + height > src_height) {
        height = src_height - src_y;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    ASSERT(src_x >= 0);
    ASSERT(src_y >= 0);
    ASSERT(dest_x >= 0);
    ASSERT(dest_y >= 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(dest_x + width <= pixman_image_get_width(dest));
    ASSERT(dest_y + height <= pixman_image_get_height(dest));
    ASSERT(src_x + width <= pixman_image_get_width(src));
    ASSERT(src_y + height <= pixman_image_get_height(src));
    ASSERT(depth == pixman_image_get_depth(src));

    if (depth == 8) {
        copy_rop_8_func_t rop_func = copy_rops_8[rop];

        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x;

        while (height--) {
            rop_func((uint8_t *)byte_line, (uint8_t *)src_line, width);
            byte_line += stride;
            src_line += src_stride;
        }
    } else if (depth == 16) {
        copy_rop_16_func_t rop_func = copy_rops_16[rop];

        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x * 2;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x * 2;

        while (height--) {
            rop_func((uint16_t *)byte_line, (uint16_t *)src_line, width);
            byte_line += stride;
            src_line += src_stride;
        }
    }  else {
        copy_rop_32_func_t rop_func = copy_rops_32[rop];

        ASSERT (depth == 32 || depth == 24);
        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x * 4;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x * 4;

        while (height--) {
            rop_func((uint32_t *)byte_line, (uint32_t *)src_line, width);
            byte_line += stride;
            src_line += src_stride;
        }
    }

}

void spice_pixman_blit_colorkey (pixman_image_t *dest,
                                 pixman_image_t *src,
                                 int src_x, int src_y,
                                 int dest_x, int dest_y,
                                 int width, int height,
                                 uint32_t transparent_color)
{
    uint32_t *bits, *src_bits;
    int stride, depth;
    int src_width, src_height, src_stride;
    uint8_t *byte_line;
    uint8_t *src_line;
    int x;

    bits = pixman_image_get_data(dest);
    stride = pixman_image_get_stride(dest);
    depth = pixman_image_get_depth(dest);
    /* stride is in bytes, depth in bits */

    src_bits = pixman_image_get_data(src);
    src_stride = pixman_image_get_stride(src);
    src_width = pixman_image_get_width(src);
    src_height = pixman_image_get_height(src);

    /* Clip source */
    if (src_x < 0) {
        width += src_x;
        dest_x -= src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        height += src_y;
        dest_y -= src_y;
        src_y = 0;
    }
    if (src_x + width > src_width) {
        width = src_width - src_x;
    }
    if (src_y + height > src_height) {
        height = src_height - src_y;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    ASSERT(src_x >= 0);
    ASSERT(src_y >= 0);
    ASSERT(dest_x >= 0);
    ASSERT(dest_y >= 0);
    ASSERT(width > 0);
    ASSERT(height > 0);
    ASSERT(dest_x + width <= pixman_image_get_width(dest));
    ASSERT(dest_y + height <= pixman_image_get_height(dest));
    ASSERT(src_x + width <= pixman_image_get_width(src));
    ASSERT(src_y + height <= pixman_image_get_height(src));
    ASSERT(depth == pixman_image_get_depth(src));

    if (depth == 8) {
        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x;

        while (height--) {
            uint8_t *d = (uint8_t *)byte_line;
            uint8_t *s = (uint8_t *)byte_line;

            s = (uint8_t *)src_line;
            for (x = 0; x < width; x++) {
                uint8_t val = *s;
                if (val != (uint8_t)transparent_color) {
                    *d = val;
                }
                s++; d++;
            }

            byte_line += stride;
            src_line += src_stride;
        }
    } else if (depth == 16) {
        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x * 2;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x * 2;

        while (height--) {
            uint16_t *d = (uint16_t *)byte_line;
            uint16_t *s = (uint16_t *)byte_line;

            s = (uint16_t *)src_line;
            for (x = 0; x < width; x++) {
                uint16_t val = *s;
                if (val != (uint16_t)transparent_color) {
                    *d = val;
                }
                s++; d++;
            }

            byte_line += stride;
            src_line += src_stride;
        }
    }  else {
        ASSERT (depth == 32 || depth == 24);
        byte_line = ((uint8_t *)bits) + stride * dest_y + dest_x * 4;
        src_line = ((uint8_t *)src_bits) + src_stride * src_y + src_x * 4;

        while (height--) {
            uint32_t *d = (uint32_t *)byte_line;
            uint32_t *s = (uint32_t *)byte_line;

            transparent_color &= 0xffffff;
            s = (uint32_t *)src_line;
            for (x = 0; x < width; x++) {
                uint32_t val = *s;
                if ((0xffffff & val) != transparent_color) {
                    *d = val;
                }
                s++; d++;
            }

            byte_line += stride;
            src_line += src_stride;
        }
    }
}

static void copy_bits_up(uint8_t *data, const int stride,
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

static void copy_bits_down(uint8_t *data, const int stride,
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

static void copy_bits_same_line(uint8_t *data, const int stride,
                                const int src_x, const int src_y,
                                const int width, const int height,
                                const int dest_x, const int dest_y)
{
    uint8_t *src = data + src_y * stride + src_x * sizeof(uint32_t);
    uint8_t *dest = data + dest_y * stride + dest_x * sizeof(uint32_t);
    uint8_t *end = dest + height * stride;
    for (; dest != end; dest += stride, src += stride) {
        memmove(dest, src, width * sizeof(uint32_t));
    }
}

void spice_pixman_copy_rect (pixman_image_t *image,
                             int src_x, int src_y,
                             int width, int height,
                             int dest_x, int dest_y)
{
    uint8_t *data;
    int stride;

    data = (uint8_t *)pixman_image_get_data(image);
    stride = pixman_image_get_stride(image);

    ASSERT(pixman_image_get_depth(image) == 24 ||
           pixman_image_get_depth(image) == 32);

    if (dest_y > src_y) {
        copy_bits_down(data, stride,
                       src_x, src_y,
                       width, height,
                       dest_x, dest_y);
    } else if (dest_y < src_y) {
        copy_bits_up(data, stride,
                     src_x, src_y,
                     width, height,
                     dest_x, dest_y);
    } else {
        copy_bits_same_line(data, stride,
                            src_x, src_y,
                            width, height,
                            dest_x, dest_y);
    }
}

pixman_bool_t spice_pixman_region32_init_rects (pixman_region32_t *region,
                                                const SpiceRect   *rects,
                                                int                count)
{
    pixman_box32_t boxes_array[10];
    pixman_box32_t *boxes;
    pixman_bool_t res;
    int i;

    if (count < 10) {
        boxes = boxes_array;
    } else {
        boxes = (pixman_box32_t *)malloc(sizeof(pixman_box32_t) * count);
        if (boxes == NULL) {
            return FALSE;
        }
    }

    for (i = 0; i < count; i++) {
        boxes[i].x1 = rects[i].left;
        boxes[i].y1 = rects[i].top;
        boxes[i].x2 = rects[i].right;
        boxes[i].y2 = rects[i].bottom;
    }

    res = pixman_region32_init_rects(region, boxes, count);

    if (count >= 10) {
        free(boxes);
    }

    return res;
}

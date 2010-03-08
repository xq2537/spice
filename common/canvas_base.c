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

#include <stdarg.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdio.h>
#include <math.h>

#include <spice/draw.h>
#include "quic.h"
#include "lz.h"
#include "canvas_base.h"
#include "pixman_utils.h"
#include "canvas_utils.h"
#include "rect.h"
#include "lines.h"
#include "rop3.h"

#include "mutex.h"

#ifndef CANVAS_ERROR
#define CANVAS_ERROR(format, ...) {                             \
    printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__);   \
    abort();                                                    \
}
#endif

#ifdef CAIRO_CANVAS_ACCESS_TEST
#define access_test(cancas, ptr, size)                                                         \
    if ((unsigned long)(ptr) < (cancas)->base ||                                               \
                                            (unsigned long)(ptr) + (size) > (cancas)->max) {   \
        CANVAS_ERROR("access violation 0x%lx %lu", (unsigned long)ptr, (unsigned long)(size)); \
    }
#else
#define access_test(cancas, base, size)
#endif

#ifndef ASSERT
#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    abort();                                                \
}
#endif

#ifndef WARN
#define WARN(x) printf("warning: %s\n", x)
#endif

#define PANIC(str) {                                \
    printf("%s: panic: %s", __FUNCTION__, str);     \
    abort();                                        \
}

#ifndef DBG
#define DBG(level, format, ...) printf("%s: debug: " format "\n", __FUNCTION__, ## __VA_ARGS__);
#endif

#ifndef ALIGN
#define ALIGN(a, b) (((a) + ((b) - 1)) & ~((b) - 1))
#endif

#ifndef MIN
#define MIN(x, y) (((x) <= (y)) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) (((x) >= (y)) ? (x) : (y))
#endif

#define ROUND(_x) ((int)floor((_x) + 0.5))

#ifdef WIN32
typedef struct  __declspec (align(1)) LZImage {
#else
typedef struct __attribute__ ((__packed__)) LZImage {
#endif
    SpiceImageDescriptor descriptor;
    union {
        SpiceLZRGBData lz_rgb;
        SpiceLZPLTData lz_plt;
    };
} LZImage;

 static inline int fix_to_int(SPICE_FIXED28_4 fixed)
{
    int val, rem;

    rem = fixed & 0x0f;
    val = fixed >> 4;
    if (rem > 8) {
        val++;
    }
    return val;
}

 static inline SPICE_FIXED28_4  int_to_fix(int v)
{
    return v << 4;
}

static inline double fix_to_double(SPICE_FIXED28_4 fixed)
{
    return (double)(fixed & 0x0f) / 0x0f + (fixed >> 4);
}

static inline uint32_t canvas_16bpp_to_32bpp(uint32_t color)
{
    uint32_t ret;

    ret = ((color & 0x001f) << 3) | ((color & 0x001c) >> 2);
    ret |= ((color & 0x03e0) << 6) | ((color & 0x0380) << 1);
    ret |= ((color & 0x7c00) << 9) | ((color & 0x7000) << 4);

    return ret;
}

static inline int test_bit(void* addr, int bit)
{
    return !!(((uint32_t*)addr)[bit >> 5] & (1 << (bit & 0x1f)));
}

static inline int test_bit_be(void* addr, int bit)
{
    return !!(((uint8_t*)addr)[bit >> 3] & (0x80 >> (bit & 0x07)));
}

#ifdef WIN32
static HDC create_compatible_dc()
{
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        CANVAS_ERROR("create compatible DC failed");
    }
    return dc;
}

#endif

typedef struct LzData {
    LzUsrContext usr;
    LzContext *lz;
    LzDecodeUsrData decode_data;
    jmp_buf jmp_env;
    char message_buf[512];
} LzData;

typedef struct GlzData {
    SpiceGlzDecoder *decoder;
    LzDecodeUsrData decode_data;
} GlzData;

typedef struct QuicData {
    QuicUsrContext usr;
    QuicContext *quic;
    jmp_buf jmp_env;
#ifndef CAIRO_CANVAS_NO_CHUNKS
    SPICE_ADDRESS next;
    SpiceVirtMapping *virt_mapping;
#endif
    char message_buf[512];
} QuicData;

typedef struct CanvasBase {
    SpiceCanvas parent;
    uint32_t color_shift;
    uint32_t color_mask;
    QuicData quic_data;
#ifdef CAIRO_CANVAS_ACCESS_TEST
    unsigned long base;
    unsigned long max;
#endif

    int width;
    int height;
    pixman_region32_t canvas_region;

#if defined(CAIRO_CANVAS_CACHE) || defined(CAIRO_CANVAS_IMAGE_CACHE)
    SpiceImageCache *bits_cache;
#endif
#ifdef CAIRO_CANVAS_CACHE
    SpicePaletteCache *palette_cache;
#endif
#ifdef WIN32
    HDC dc;
#endif

    LzData lz_data;
    GlzData glz_data;

    void *usr_data;
    spice_destroy_fn_t usr_data_destroy;
} CanvasBase;


#ifndef CAIRO_CANVAS_NO_CHUNKS

#ifdef __GNUC__
#define ATTR_PACKED __attribute__ ((__packed__))
#else
#pragma pack(push)
#pragma pack(1)
#define ATTR_PACKED
#endif

typedef struct ATTR_PACKED DataChunk {
    uint32_t size;
    SPICE_ADDRESS prev;
    SPICE_ADDRESS next;
    uint8_t data[0];
} DataChunk;

#undef ATTR_PACKED

#ifndef __GNUC__
#pragma pack(pop)
#endif

#endif

typedef enum {
    ROP_INPUT_SRC,
    ROP_INPUT_BRUSH,
    ROP_INPUT_DEST
} ROPInput;

static SpiceROP ropd_descriptor_to_rop(int desc,
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

static inline void canvas_localize_palette(CanvasBase *canvas, SpicePalette *palette)
{
    if (canvas->color_shift == 5) {
        uint32_t *now = palette->ents;
        uint32_t *end = now + palette->num_ents;
        for (; now < end; now++) {
            *now = canvas_16bpp_to_32bpp(*now);
        }
    }
}

//#define DEBUG_DUMP_COMPRESS
#ifdef DEBUG_DUMP_COMPRESS
static void dump_surface(pixman_image_t *surface, int cache);
#endif
static pixman_image_t *canvas_get_quic(CanvasBase *canvas, SpiceQUICImage *image, int invers)
{
    pixman_image_t *surface = NULL;
    QuicData *quic_data = &canvas->quic_data;
    QuicImageType type;
    uint8_t *dest;
    int stride;
    int width;
    int height;
    int alpha;
#ifndef CAIRO_CANVAS_NO_CHUNKS
    DataChunk **tmp;
    DataChunk *chunk;
#endif

    if (setjmp(quic_data->jmp_env)) {
        pixman_image_unref(surface);
        CANVAS_ERROR("quic error, %s", quic_data->message_buf);
    }

#ifdef CAIRO_CANVAS_NO_CHUNKS
    if (quic_decode_begin(quic_data->quic, (uint32_t *)image->quic.data,
                          image->quic.data_size >> 2, &type, &width, &height) == QUIC_ERROR) {
        CANVAS_ERROR("quic decode begin failed");
    }
#else
    tmp = (DataChunk **)image->quic.data;
    chunk = *tmp;
    quic_data->next = chunk->next;
    if (quic_decode_begin(quic_data->quic, (uint32_t *)chunk->data, chunk->size >> 2,
                          &type, &width, &height) == QUIC_ERROR) {
        CANVAS_ERROR("quic decode begin failed");
    }
#endif

    switch (type) {
    case QUIC_IMAGE_TYPE_RGBA:
        alpha = 1;
        break;
    case QUIC_IMAGE_TYPE_RGB32:
    case QUIC_IMAGE_TYPE_RGB24:
    case QUIC_IMAGE_TYPE_RGB16:
        alpha = 0;
        break;
    case QUIC_IMAGE_TYPE_INVALID:
    case QUIC_IMAGE_TYPE_GRAY:
    default:
        CANVAS_ERROR("unexpected image type");
    }

    ASSERT((uint32_t)width == image->descriptor.width);
    ASSERT((uint32_t)height == image->descriptor.height);

    surface = surface_create(
#ifdef WIN32
                             canvas->dc,
#endif
                             alpha ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8,
                             width, height, FALSE);

    if (surface == NULL) {
        CANVAS_ERROR("create surface failed");
    }

    dest = (uint8_t *)pixman_image_get_data(surface);
    stride = pixman_image_get_stride(surface);
    if (quic_decode(quic_data->quic, alpha ? QUIC_IMAGE_TYPE_RGBA : QUIC_IMAGE_TYPE_RGB32,
                    dest, stride) == QUIC_ERROR) {
        pixman_image_unref(surface);
        CANVAS_ERROR("quic decode failed");
    }

    if (invers) {
        uint8_t *end = dest + height * stride;
        for (; dest != end; dest += stride) {
            uint32_t *pix;
            uint32_t *end_pix;

            pix = (uint32_t *)dest;
            end_pix = pix + width;
            for (; pix < end_pix; pix++) {
                *pix ^= 0x00ffffff;
            }
        }
    }

#ifdef DEBUG_DUMP_COMPRESS
    dump_surface(surface, 0);
#endif
    return surface;
}

static inline void canvas_copy_32bpp(uint8_t* dest, int dest_stride, uint8_t* src, int src_stride,
                                     int width, uint8_t* end)
{
    for (; src != end; src += src_stride, dest += dest_stride) {
        memcpy(dest, src, width << 2);
    }
}

static inline void canvas_copy_24bpp(uint8_t* dest, int dest_stride, uint8_t* src, int src_stride,
                                     int width, uint8_t* end)
{
    for (; src != end; src += src_stride, dest += dest_stride) {
        uint8_t* src_line = src;
        uint8_t* src_line_end = src_line + width * 3;
        uint8_t* dest_line = dest;

        for (; src_line < src_line_end; ++dest_line) {
            *(dest_line++) = *(src_line++);
            *(dest_line++) = *(src_line++);
            *(dest_line++) = *(src_line++);
        }
    }
}

static inline void canvas_copy_16bpp(uint8_t* dest, int dest_stride, uint8_t* src, int src_stride,
                                     int width, uint8_t* end)
{
    for (; src != end; src += src_stride, dest += dest_stride) {
        uint16_t* src_line = (uint16_t*)src;
        uint16_t* src_line_end = src_line + width;
        uint32_t* dest_line = (uint32_t*)dest;

        for (; src_line < src_line_end; ++dest_line, src_line++) {
            *dest_line = canvas_16bpp_to_32bpp(*src_line);
        }
    }
}

static inline void canvas_copy_8bpp(uint8_t *dest, int dest_stride, uint8_t *src, int src_stride,
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
            *(dest_line++) = palette->ents[*(src_line++)];
        }
    }
}

static inline void canvas_copy_4bpp_be(uint8_t* dest, int dest_stride, uint8_t* src, int src_stride,
                                       int width, uint8_t* end, SpicePalette *palette)
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
            *(dest_line++) = palette->ents[(*now >> 4) & 0x0f];
            *(dest_line++) = palette->ents[*(now++) & 0x0f];
        }
        if (width & 1) {
            *(dest_line) = palette->ents[(*src >> 4) & 0x0f];
        }
    }
}

static inline void canvas_copy_1bpp_be(uint8_t* dest, int dest_stride, uint8_t* src, int src_stride,
                                       int width, uint8_t* end, SpicePalette *palette)
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
                *(dest_line++) = fore_color;
            } else {
                *(dest_line++) = back_color;
            }
        }
    }
}

static pixman_image_t *canvas_bitmap_to_surface(CanvasBase *canvas, SpiceBitmap* bitmap,
                                                SpicePalette *palette)
{
    uint8_t* src = (uint8_t *)SPICE_GET_ADDRESS(bitmap->data);
    int src_stride;
    uint8_t* end;
    uint8_t* dest;
    int dest_stride;
    pixman_image_t* image;

    src_stride = bitmap->stride;
    end = src + (bitmap->y * src_stride);
    access_test(canvas, src, bitmap->y * src_stride);

    image = surface_create(
#ifdef WIN32
                                   canvas->dc,
#endif
                                   (bitmap->format == SPICE_BITMAP_FMT_RGBA) ? PIXMAN_a8r8g8b8 :
                                                                         PIXMAN_x8r8g8b8,
                                   bitmap->x, bitmap->y, FALSE);
    if (image == NULL) {
        CANVAS_ERROR("create surface failed");
    }
    dest = (uint8_t *)pixman_image_get_data(image);
    dest_stride = pixman_image_get_stride(image);
    if (!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        ASSERT(bitmap->y > 0);
        dest += dest_stride * ((int)bitmap->y - 1);
        dest_stride = -dest_stride;
    }

    switch (bitmap->format) {
    case SPICE_BITMAP_FMT_32BIT:
    case SPICE_BITMAP_FMT_RGBA:
        canvas_copy_32bpp(dest, dest_stride, src, src_stride, bitmap->x, end);
        break;
    case SPICE_BITMAP_FMT_24BIT:
        canvas_copy_24bpp(dest, dest_stride, src, src_stride, bitmap->x, end);
        break;
    case SPICE_BITMAP_FMT_16BIT:
        canvas_copy_16bpp(dest, dest_stride, src, src_stride, bitmap->x, end);
        break;
    case SPICE_BITMAP_FMT_8BIT:
        canvas_copy_8bpp(dest, dest_stride, src, src_stride, bitmap->x, end, palette);
        break;
    case SPICE_BITMAP_FMT_4BIT_BE:
        canvas_copy_4bpp_be(dest, dest_stride, src, src_stride, bitmap->x, end, palette);
        break;
    case SPICE_BITMAP_FMT_1BIT_BE:
        canvas_copy_1bpp_be(dest, dest_stride, src, src_stride, bitmap->x, end, palette);
        break;
    }
    return image;
}

#ifdef CAIRO_CANVAS_CACHE

static inline SpicePalette *canvas_get_palett(CanvasBase *canvas, SPICE_ADDRESS base_palette, uint8_t flags)
{
    SpicePalette *palette;
    if (!base_palette) {
        return NULL;
    }

    if (flags & SPICE_BITMAP_FLAGS_PAL_FROM_CACHE) {
        palette = canvas->palette_cache->ops->get(canvas->palette_cache, base_palette);
    } else if (flags & SPICE_BITMAP_FLAGS_PAL_CACHE_ME) {
        palette = (SpicePalette *)SPICE_GET_ADDRESS(base_palette);
        access_test(canvas, palette, sizeof(SpicePalette));
        access_test(canvas, palette, sizeof(SpicePalette) + palette->num_ents * sizeof(uint32_t));
        canvas_localize_palette(canvas, palette);
        canvas->palette_cache->ops->put(canvas->palette_cache, palette);
    } else {
        palette = (SpicePalette *)SPICE_GET_ADDRESS(base_palette);
        canvas_localize_palette(canvas, palette);
    }
    return palette;
}

static pixman_image_t *canvas_get_lz(CanvasBase *canvas, LZImage *image, int invers)
{
    LzData *lz_data = &canvas->lz_data;
    uint8_t *comp_buf = NULL;
    int comp_size;
    uint8_t    *decomp_buf = NULL;
    uint8_t    *src;
    LzImageType type;
    SpicePalette *palette;
    int alpha;
    int n_comp_pixels;
    int width;
    int height;
    int top_down;
    int stride;

    if (setjmp(lz_data->jmp_env)) {
        if (decomp_buf) {
            free(decomp_buf);
        }
        CANVAS_ERROR("lz error, %s", lz_data->message_buf);
    }

    if (image->descriptor.type == SPICE_IMAGE_TYPE_LZ_RGB) {
        comp_buf = image->lz_rgb.data;
        comp_size = image->lz_rgb.data_size;
        palette = NULL;
    } else if (image->descriptor.type == SPICE_IMAGE_TYPE_LZ_PLT) {
        comp_buf = image->lz_plt.data;
        comp_size = image->lz_plt.data_size;
        palette = canvas_get_palett(canvas, image->lz_plt.palette, image->lz_plt.flags);
    } else {
        CANVAS_ERROR("unexpected image type");
    }

    lz_decode_begin(lz_data->lz, comp_buf, comp_size, &type,
                    &width, &height, &n_comp_pixels, &top_down, palette);

    switch (type) {
    case LZ_IMAGE_TYPE_RGBA:
        alpha = 1;
        break;
    case LZ_IMAGE_TYPE_RGB32:
    case LZ_IMAGE_TYPE_RGB24:
    case LZ_IMAGE_TYPE_RGB16:
    case LZ_IMAGE_TYPE_PLT1_LE:
    case LZ_IMAGE_TYPE_PLT1_BE:
    case LZ_IMAGE_TYPE_PLT4_LE:
    case LZ_IMAGE_TYPE_PLT4_BE:
    case LZ_IMAGE_TYPE_PLT8:
        alpha = 0;
        break;
    default:
        CANVAS_ERROR("unexpected LZ image type");
    }

    ASSERT(width == image->descriptor.width);
    ASSERT(height == image->descriptor.height);

    ASSERT((image->descriptor.type == SPICE_IMAGE_TYPE_LZ_PLT) || (n_comp_pixels == width * height));
#ifdef WIN32
    lz_data->decode_data.dc = canvas->dc;
#endif


    alloc_lz_image_surface(&lz_data->decode_data, alpha ? LZ_IMAGE_TYPE_RGBA : LZ_IMAGE_TYPE_RGB32,
                           width, height, n_comp_pixels, top_down);

    src = (uint8_t *)pixman_image_get_data(lz_data->decode_data.out_surface);

    stride = (n_comp_pixels / height) * 4;
    if (!top_down) {
        stride = -stride;
        decomp_buf = src + stride * (height - 1);
    } else {
        decomp_buf = src;
    }

    lz_decode(lz_data->lz, alpha ? LZ_IMAGE_TYPE_RGBA : LZ_IMAGE_TYPE_RGB32, decomp_buf);

    if (invers) {
        uint8_t *line = src;
        uint8_t *end = src + height * stride;
        for (; line != end; line += stride) {
            uint32_t *pix;
            uint32_t *end_pix;

            pix = (uint32_t *)line;
            end_pix = pix + width;
            for (; pix < end_pix; pix++) {
                *pix ^= 0x00ffffff;
            }
        }
    }

    return lz_data->decode_data.out_surface;
}

// don't handle plts since bitmaps with plt can be decoded globaly to RGB32 (because
// same byte sequence can be transformed to different RGB pixels by different plts)
static pixman_image_t *canvas_get_glz(CanvasBase *canvas, LZImage *image)
{
    ASSERT(image->descriptor.type == SPICE_IMAGE_TYPE_GLZ_RGB);
#ifdef WIN32
    canvas->glz_data.decode_data.dc = canvas->dc;
#endif

    if (canvas->glz_data.decoder == NULL) {
        CANVAS_ERROR("glz not supported");
    }

    canvas->glz_data.decoder->ops->decode(canvas->glz_data.decoder,
                                          image->lz_rgb.data, NULL,
                                          &canvas->glz_data.decode_data);
    /* global_decode calls alloc_lz_image, which sets canvas->glz_data.surface */
    return (canvas->glz_data.decode_data.out_surface);
}

//#define DEBUG_DUMP_BITMAP

#ifdef DEBUG_DUMP_BITMAP
static void dump_bitmap(SpiceBitmap *bitmap, SpicePalette *palette)
{
    uint8_t* data = (uint8_t *)SPICE_GET_ADDRESS(bitmap->data);
    static uint32_t file_id = 0;
    uint32_t i, j;
    char file_str[200];
    uint32_t id = ++file_id;

#ifdef WIN32
    sprintf(file_str, "c:\\tmp\\spice_dump\\%u.%ubpp", id, bitmap->format);
#else
    sprintf(file_str, "/tmp/spice_dump/%u.%ubpp", id, bitmap->format);
#endif
    FILE *f = fopen(file_str, "wb");
    if (!f) {
        return;
    }

    fprintf(f, "%d\n", bitmap->format);                          // 1_LE,1_BE,....
    fprintf(f, "%d %d\n", bitmap->x, bitmap->y);     // width and height
    fprintf(f, "%d\n", palette->num_ents);               // #plt entries
    for (i = 0; i < palette->num_ents; i++) {
        fwrite(&(palette->ents[i]), 4, 1, f);
    }
    fprintf(f, "\n");

    for (i = 0; i < bitmap->y; i++, data += bitmap->stride) {
        uint8_t *now = data;
        for (j = 0; j < bitmap->x; j++) {
            fwrite(now, 1, 1, f);
            now++;
        }
    }
}

#endif

static pixman_image_t *canvas_get_bits(CanvasBase *canvas, SpiceBitmap *bitmap)
{
    pixman_image_t* surface;
    SpicePalette *palette;

    palette = canvas_get_palett(canvas, bitmap->palette, bitmap->flags);
#ifdef DEBUG_DUMP_BITMAP
    if (palette) {
        dump_bitmap(bitmap, palette);
    }
#endif

    surface = canvas_bitmap_to_surface(canvas, bitmap, palette);

    if (palette && (bitmap->flags & SPICE_BITMAP_FLAGS_PAL_FROM_CACHE)) {
        canvas->palette_cache->ops->release(canvas->palette_cache, palette);
    }

    return surface;
}

#else


static pixman_image_t *canvas_get_bits(CanvasBase *canvas, SpiceBitmap *bitmap)
{
    SpicePalette *palette;

    if (!bitmap->palette) {
        return canvas_bitmap_to_surface(canvas, bitmap, NULL);
    }
    palette = (SpicePalette *)SPICE_GET_ADDRESS(bitmap->palette);
    if (canvas->color_shift == 5) {
        int size = sizeof(SpicePalette) + (palette->num_ents << 2);
        SpicePalette *local_palette = malloc(size);
        pixman_image_t* surface;

        memcpy(local_palette, palette, size);
        canvas_localize_palette(canvas, local_palette);
        surface = canvas_bitmap_to_surface(canvas, bitmap, local_palette);
        free(local_palette);
        return surface;
    } else {
        return canvas_bitmap_to_surface(canvas, bitmap, palette);
    }
}

#endif



// caution: defining DEBUG_DUMP_SURFACE will dump both cached & non-cached
//          images to disk. it will reduce performance dramatically & eat
//          disk space rapidly. use it only for debugging.
//#define DEBUG_DUMP_SURFACE

#if defined(DEBUG_DUMP_SURFACE) || defined(DEBUG_DUMP_COMPRESS)

static void dump_surface(pixman_image_t *surface, int cache)
{
    static uint32_t file_id = 0;
    int i, j;
    char file_str[200];
    int depth = pixman_image_get_depth(surface);

    if (depth != 24 && depth != 32) {
        return;
    }

    uint8_t *data = (uint8_t *)pixman_image_get_data(surface);
    int width = pixman_image_get_width(surface);
    int height = pixman_image_get_height(surface);
    int stride = pixman_image_surface_get_stride(surface);

    uint32_t id = ++file_id;
#ifdef WIN32
    sprintf(file_str, "c:\\tmp\\spice_dump\\%d\\%u.ppm", cache, id);
#else
    sprintf(file_str, "/tmp/spice_dump/%u.ppm", id);
#endif
    FILE *f = fopen(file_str, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n");
    fprintf(f, "%d %d\n", width, height);
    fprintf(f, "#spicec dump\n");
    fprintf(f, "255\n");
    for (i = 0; i < height; i++, data += stride) {
        uint8_t *now = data;
        for (j = 0; j < width; j++) {
            fwrite(&now[2], 1, 1, f);
            fwrite(&now[1], 1, 1, f);
            fwrite(&now[0], 1, 1, f);
            now += 4;
        }
    }
    fclose(f);
}

#endif

#if defined(CAIRO_CANVAS_CACHE) || defined(CAIRO_CANVAS_IMAGE_CACHE)

//#define DEBUG_LZ

/* If real get is FALSE, then only do whatever is needed but don't return an image. For instance,
   if we need to read it to cache it we do. */
static pixman_image_t *canvas_get_image_internal(CanvasBase *canvas, SPICE_ADDRESS addr, int real_get)
{
    SpiceImageDescriptor *descriptor = (SpiceImageDescriptor *)SPICE_GET_ADDRESS(addr);
    pixman_image_t *surface;
    access_test(canvas, descriptor, sizeof(SpiceImageDescriptor));
#ifdef DEBUG_LZ
    LOG_DEBUG("canvas_get_image image type: " << (int)descriptor->type);
#endif

    /* When touching, only really allocate if we need to cache, or
     * if we're loading a GLZ stream (since those need inter-thread communication
     * to happen which breaks if we don't. */
    if (!real_get &&
        !(descriptor->flags & SPICE_IMAGE_FLAGS_CACHE_ME) &&
        (descriptor->type != SPICE_IMAGE_TYPE_GLZ_RGB)) {
        return NULL;
    }

    switch (descriptor->type) {
    case SPICE_IMAGE_TYPE_QUIC: {
        SpiceQUICImage *image = (SpiceQUICImage *)descriptor;
        access_test(canvas, descriptor, sizeof(SpiceQUICImage));
        surface = canvas_get_quic(canvas, image, 0);
        break;
    }
#ifdef CAIRO_CANVAS_NO_CHUNKS
    case SPICE_IMAGE_TYPE_LZ_PLT: {
        access_test(canvas, descriptor, sizeof(SpiceLZPLTImage));
        LZImage *image = (LZImage *)descriptor;
        surface = canvas_get_lz(canvas, image, 0);
        break;
    }
    case SPICE_IMAGE_TYPE_LZ_RGB: {
        access_test(canvas, descriptor, sizeof(SpiceLZRGBImage));
        LZImage *image = (LZImage *)descriptor;
        surface = canvas_get_lz(canvas, image, 0);
        break;
    }
#endif

#if defined(CAIRO_CANVAS_CACHE)
    case SPICE_IMAGE_TYPE_GLZ_RGB: {
        access_test(canvas, descriptor, sizeof(SpiceLZRGBImage));
        LZImage *image = (LZImage *)descriptor;
        surface = canvas_get_glz(canvas, image);
        break;
    }
#endif

    case SPICE_IMAGE_TYPE_FROM_CACHE:
        return canvas->bits_cache->ops->get(canvas->bits_cache, descriptor->id);
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmapImage *bitmap = (SpiceBitmapImage *)descriptor;
        access_test(canvas, descriptor, sizeof(SpiceBitmapImage));
        surface = canvas_get_bits(canvas, &bitmap->bitmap);
        break;
    }
    default:
        CANVAS_ERROR("invalid image type");
    }

    if (descriptor->flags & SPICE_IMAGE_FLAGS_CACHE_ME) {
        canvas->bits_cache->ops->put(canvas->bits_cache, descriptor->id, surface);
#ifdef DEBUG_DUMP_SURFACE
        dump_surface(surface, 1);
#endif
    } else if (descriptor->type != SPICE_IMAGE_TYPE_FROM_CACHE) {
#ifdef DEBUG_DUMP_SURFACE
        dump_surface(surface, 0);
#endif
    }

    if (!real_get) {
        pixman_image_unref(surface);
        return NULL;
    }

    return surface;
}

#else

static pixman_image_t *canvas_get_image_internal(CanvasBase *canvas, SPICE_ADDRESS addr, int real_get)
{
    SpiceImageDescriptor *descriptor = (SpiceImageDescriptor *)SPICE_GET_ADDRESS(addr);

    access_test(canvas, descriptor, sizeof(SpiceImageDescriptor));

    /* When touching, never load image. */
    if (!real_get) {
        return NULL;
    }

    switch (descriptor->type) {
    case SPICE_IMAGE_TYPE_QUIC: {
        SpiceQUICImage *image = (SpiceQUICImage *)descriptor;
        access_test(canvas, descriptor, sizeof(SpiceQUICImage));
        return canvas_get_quic(canvas, image, 0);
    }
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmapImage *bitmap = (SpiceBitmapImage *)descriptor;
        access_test(canvas, descriptor, sizeof(SpiceBitmapImage));
        return canvas_get_bits(canvas, &bitmap->bitmap);
    }
    default:
        CANVAS_ERROR("invalid image type");
    }
}

#endif

static pixman_image_t *canvas_get_image(CanvasBase *canvas, SPICE_ADDRESS addr)
{
    return canvas_get_image_internal(canvas, addr, TRUE);
}

static void canvas_touch_image(CanvasBase *canvas, SPICE_ADDRESS addr)
{
    canvas_get_image_internal(canvas, addr, FALSE);
}

static pixman_image_t* canvas_get_image_from_self(SpiceCanvas *canvas,
                                                  int x, int y,
                                                  int32_t width, int32_t height)
{
    pixman_image_t *surface;
    uint8_t *dest;
    int dest_stride;
    SpiceRect area;

    surface = pixman_image_create_bits(PIXMAN_x8r8g8b8, width, height, NULL, 0);
    if (surface == NULL) {
        CANVAS_ERROR("create surface failed");
    }

    dest = (uint8_t *)pixman_image_get_data(surface);
    dest_stride = pixman_image_get_stride(surface);

    area.left = x;
    area.top = y;
    area.right = x + width;
    area.bottom = y + height;

    canvas->ops->read_bits(canvas, dest, dest_stride, &area);

    return surface;
}

static inline uint8_t revers_bits(uint8_t byte)
{
    uint8_t ret = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int shift = 7 - i * 2;
        ret |= (byte & (1 << i)) << shift;
        ret |= (byte & (0x80 >> i)) >> shift;
    }
    return ret;
}

static pixman_image_t *canvas_get_bitmap_mask(CanvasBase *canvas, SpiceBitmap* bitmap, int invers)
{
    pixman_image_t *surface;
    uint8_t *src_line;
    uint8_t *end_line;
    uint8_t *dest_line;
    int src_stride;
    int line_size;
    int dest_stride;

    surface = surface_create(
#ifdef WIN32
            canvas->dc,
#endif
            PIXMAN_a1, bitmap->x, bitmap->y, TRUE);
    if (surface == NULL) {
        CANVAS_ERROR("create surface failed");
    }

    src_line = (uint8_t *)SPICE_GET_ADDRESS(bitmap->data);
    src_stride = bitmap->stride;
    end_line = src_line + (bitmap->y * src_stride);
    access_test(canvas, src_line, end_line - src_line);
    line_size = ALIGN(bitmap->x, 8) >> 3;

    dest_stride = pixman_image_get_stride(surface);
    dest_line = (uint8_t *)pixman_image_get_data(surface);
#if defined(GL_CANVAS)
    if ((bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
#else
    if (!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
#endif
        ASSERT(bitmap->y > 0);
        dest_line += dest_stride * ((int)bitmap->y - 1);
        dest_stride = -dest_stride;
    }

    if (invers) {
        switch (bitmap->format) {
#if defined(GL_CANVAS) || defined(GDI_CANVAS)
        case SPICE_BITMAP_FMT_1BIT_BE:
#else
        case SPICE_BITMAP_FMT_1BIT_LE:
#endif
            for (; src_line != end_line; src_line += src_stride, dest_line += dest_stride) {
                uint8_t *dest = dest_line;
                uint8_t *now = src_line;
                uint8_t *end = now + line_size;
                while (now < end) {
                    *(dest++) = ~*(now++);
                }
            }
            break;
#if defined(GL_CANVAS) || defined(GDI_CANVAS)
        case SPICE_BITMAP_FMT_1BIT_LE:
#else
        case SPICE_BITMAP_FMT_1BIT_BE:
#endif
            for (; src_line != end_line; src_line += src_stride, dest_line += dest_stride) {
                uint8_t *dest = dest_line;
                uint8_t *now = src_line;
                uint8_t *end = now + line_size;

                while (now < end) {
                    *(dest++) = ~revers_bits(*(now++));
                }
            }
            break;
        default:
            pixman_image_unref(surface);
            surface = NULL;
            CANVAS_ERROR("invalid bitmap format");
        }
    } else {
        switch (bitmap->format) {
#if defined(GL_CANVAS) || defined(GDI_CANVAS)
        case SPICE_BITMAP_FMT_1BIT_BE:
#else
        case SPICE_BITMAP_FMT_1BIT_LE:
#endif
            for (; src_line != end_line; src_line += src_stride, dest_line += dest_stride) {
                memcpy(dest_line, src_line, line_size);
            }
            break;
#if defined(GL_CANVAS) || defined(GDI_CANVAS)
        case SPICE_BITMAP_FMT_1BIT_LE:
#else
        case SPICE_BITMAP_FMT_1BIT_BE:
#endif
            for (; src_line != end_line; src_line += src_stride, dest_line += dest_stride) {
                uint8_t *dest = dest_line;
                uint8_t *now = src_line;
                uint8_t *end = now + line_size;

                while (now < end) {
                    *(dest++) = revers_bits(*(now++));
                }
            }
            break;
        default:
            pixman_image_unref(surface);
            surface = NULL;
            CANVAS_ERROR("invalid bitmap format");
        }
    }
    return surface;
}

static inline pixman_image_t *canvas_A1_invers(pixman_image_t *src_surf)
{
    int width = pixman_image_get_width(src_surf);
    int height = pixman_image_get_height(src_surf);

    pixman_image_t * invers = pixman_image_create_bits(PIXMAN_a1, width, height, NULL, 0);
    if (invers != NULL) {
        uint8_t *src_line = (uint8_t *)pixman_image_get_data(src_surf);
        int src_stride = pixman_image_get_stride(src_surf);
        uint8_t *end_line = src_line + (height * src_stride);
        int line_size = ALIGN(width, 8) >> 3;
        uint8_t *dest_line = (uint8_t *)pixman_image_get_data(invers);
        int dest_stride = pixman_image_get_stride(invers);

        for (; src_line != end_line; src_line += src_stride, dest_line += dest_stride) {
            uint8_t *dest = dest_line;
            uint8_t *now = src_line;
            uint8_t *end = now + line_size;
            while (now < end) {
                *(dest++) = ~*(now++);
            }
        }
    }
    return invers;
}

static pixman_image_t *canvas_surf_to_invers(pixman_image_t *surf)
{
    int width = pixman_image_get_width(surf);
    int height = pixman_image_get_height(surf);
    uint8_t *dest_line;
    uint8_t *dest_line_end;
    uint8_t *src_line;
    int dest_stride;
    int src_stride;

    ASSERT(pixman_image_get_depth(surf) == 24);
    pixman_image_t *invers = pixman_image_create_bits (PIXMAN_x8r8g8b8, width, height, NULL, 0);

    if (invers == NULL) {
        CANVAS_ERROR("create surface failed");
    }

    dest_line = (uint8_t *)pixman_image_get_data(invers);
    dest_stride = pixman_image_get_stride(invers);
    dest_line_end = dest_line + dest_stride * height;
    src_line = (uint8_t *)pixman_image_get_data(surf);
    src_stride = pixman_image_get_stride(surf);

    for (; dest_line != dest_line_end; dest_line += dest_stride, src_line += src_stride) {
        uint32_t *src = (uint32_t *)src_line;
        uint32_t *dest = (uint32_t *)dest_line;
        uint32_t *end = dest + width;
        while (dest < end) {
            *(dest++) = ~*(src++) & 0x00ffffff;
        }
    }
    return invers;
}

/*
* Return the inversed surface and assigns it to the user data of the given surface.
* The routine also handles the reference count of the inversed surface. It you don't use
* the returned reference, you must call cairo_surface_destroy.
* Thread safe with respect to the user data.
*/
static inline pixman_image_t* canvas_handle_inverse_user_data(pixman_image_t* surface)
{
    pixman_image_t *inv_surf = NULL;

    if (pixman_image_get_depth(surface) == 1) {
        inv_surf = canvas_A1_invers(surface);
    } else {
        inv_surf = canvas_surf_to_invers(surface);
    }

    if (inv_surf == NULL) {
        CANVAS_ERROR("create surface failed");
    }

    return inv_surf;
}

static pixman_image_t *canvas_get_mask(CanvasBase *canvas, SpiceQMask *mask, int *needs_invert_out)
{
    SpiceImageDescriptor *descriptor;
    pixman_image_t *surface;
    int need_invers;
    int is_invers;
    int cache_me;

    if (needs_invert_out) {
        *needs_invert_out = 0;
    }

    if (!mask->bitmap) {
        return NULL;
    }

    descriptor = (SpiceImageDescriptor *)SPICE_GET_ADDRESS(mask->bitmap);
    access_test(canvas, descriptor, sizeof(SpiceImageDescriptor));
    need_invers = mask->flags & SPICE_MASK_FLAGS_INVERS;

#ifdef CAIRO_CANVAS_CACHE
    cache_me = descriptor->flags & SPICE_IMAGE_FLAGS_CACHE_ME;
#else
    cache_me = 0;
#endif

    switch (descriptor->type) {
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmapImage *bitmap = (SpiceBitmapImage *)descriptor;
        access_test(canvas, descriptor, sizeof(SpiceBitmapImage));
        is_invers = need_invers && !cache_me;
        surface = canvas_get_bitmap_mask(canvas, &bitmap->bitmap, is_invers);
        break;
    }
#if defined(CAIRO_CANVAS_CACHE) || defined(CAIRO_CANVAS_IMAGE_CACHE)
    case SPICE_IMAGE_TYPE_FROM_CACHE:
        surface = canvas->bits_cache->ops->get(canvas->bits_cache, descriptor->id);
        is_invers = 0;
        break;
#endif
    default:
        CANVAS_ERROR("invalid image type");
    }

#if defined(CAIRO_CANVAS_CACHE) || defined(CAIRO_CANVAS_IMAGE_CACHE)
    if (cache_me) {
        canvas->bits_cache->ops->put(canvas->bits_cache, descriptor->id, surface);
    }

    if (need_invers && !is_invers) { // surface is in cache
        if (needs_invert_out != NULL) {
            *needs_invert_out = TRUE;
        } else {
            pixman_image_t *inv_surf;
            inv_surf = canvas_handle_inverse_user_data(surface);
            pixman_image_unref(surface);
            surface = inv_surf;
        }
    }
#endif
    return surface;
}

static inline SpiceRasterGlyph *canvas_next_raster_glyph(const SpiceRasterGlyph *glyph, int bpp)
{
    return (SpiceRasterGlyph *)((uint8_t *)(glyph + 1) +
                                          (ALIGN(glyph->width * bpp, 8) * glyph->height >> 3));
}

static inline void canvas_raster_glyph_box(const SpiceRasterGlyph *glyph, SpiceRect *r)
{
    ASSERT(r);
    r->top = glyph->render_pos.y + glyph->glyph_origin.y;
    r->bottom = r->top + glyph->height;
    r->left = glyph->render_pos.x + glyph->glyph_origin.x;
    r->right = r->left + glyph->width;
}

#ifdef GL_CANVAS
static inline void __canvas_put_bits(uint8_t *dest, int offset, uint8_t val, int n)
{
    uint8_t mask;
    int now;

    dest = dest + (offset >> 3);
    offset &= 0x07;
    now = MIN(8 - offset, n);

    mask = ~((1 << (8 - now)) - 1);
    mask >>= offset;
    *dest = ((val >> offset) & mask) | *dest;

    if ((n = n - now)) {
        mask = ~((1 << (8 - n)) - 1);
        dest++;
        *dest = ((val << now) & mask) | *dest;
    }
}

#else
static inline void __canvas_put_bits(uint8_t *dest, int offset, uint8_t val, int n)
{
    uint8_t mask;
    int now;

    dest = dest + (offset >> 3);
    offset &= 0x07;

    now = MIN(8 - offset, n);

    mask = (1 << now) - 1;
    mask <<= offset;
    val = revers_bits(val);
    *dest = ((val << offset) & mask) | *dest;

    if ((n = n - now)) {
        mask = (1 << n) - 1;
        dest++;
        *dest = ((val >> now) & mask) | *dest;
    }
}

#endif

static inline void canvas_put_bits(uint8_t *dest, int dest_offset, uint8_t *src, int n)
{
    while (n) {
        int now = MIN(n, 8);

        n -= now;
        __canvas_put_bits(dest, dest_offset, *src, now);
        dest_offset += now;
        src++;
    }
}

static void canvas_put_glyph_bits(SpiceRasterGlyph *glyph, int bpp, uint8_t *dest, int dest_stride,
                                  SpiceRect *bounds)
{
    SpiceRect glyph_box;
    uint8_t *src;
    int lines;
    int width;

    //todo: support SPICE_STRING_FLAGS_RASTER_TOP_DOWN
    canvas_raster_glyph_box(glyph, &glyph_box);
    ASSERT(glyph_box.top >= bounds->top && glyph_box.bottom <= bounds->bottom);
    ASSERT(glyph_box.left >= bounds->left && glyph_box.right <= bounds->right);
    rect_offset(&glyph_box, -bounds->left, -bounds->top);

    dest += glyph_box.top * dest_stride;
    src = glyph->data;
    lines = glyph_box.bottom - glyph_box.top;
    width = glyph_box.right - glyph_box.left;
    switch (bpp) {
    case 1: {
        int src_stride = ALIGN(width, 8) >> 3;
        int i;

        src += src_stride * (lines);
        for (i = 0; i < lines; i++) {
            src -= src_stride;
            canvas_put_bits(dest, glyph_box.left, src, width);
            dest += dest_stride;
        }
        break;
    }
    case 4: {
        uint8_t *end;
        int src_stride = ALIGN(width * 4, 8) >> 3;

        src += src_stride * lines;
        dest += glyph_box.left;
        end = dest + dest_stride * lines;
        for (; dest != end; dest += dest_stride) {
            int i = 0;
            uint8_t *now;

            src -= src_stride;
            now = src;
            while (i < (width & ~1)) {
                dest[i] = MAX(dest[i], *now & 0xf0);
                dest[i + 1] = MAX(dest[i + 1], *now << 4);
                i += 2;
                now++;
            }
            if (i < width) {
                dest[i] = MAX(dest[i], *now & 0xf0);
                now++;
            }
        }
        break;
    }
    case 8: {
        uint8_t *end;
        src += width * lines;
        dest += glyph_box.left;
        end = dest + dest_stride * lines;
        for (; dest != end; dest += dest_stride, src -= width) {
            int i;

            for (i = 0; i < width; i++) {
                dest[i] = MAX(dest[i], src[i]);
            }
        }
        break;
    }
    default:
        CANVAS_ERROR("invalid bpp");
    }
}

static pixman_image_t *canvas_get_str_mask(CanvasBase *canvas, SpiceString *str, int bpp, SpicePoint *pos)
{
    SpiceRasterGlyph *glyph = (SpiceRasterGlyph *)str->data;
    SpiceRasterGlyph *next_glyph;
    SpiceRect bounds;
    pixman_image_t *str_mask;
    uint8_t *dest;
    int dest_stride;
    int i;

    ASSERT(str->length > 0);

    access_test(canvas, glyph, sizeof(SpiceRasterGlyph));
    next_glyph = canvas_next_raster_glyph(glyph, bpp);
    access_test(canvas, glyph, (uint8_t*)next_glyph - (uint8_t*)glyph);
    canvas_raster_glyph_box(glyph, &bounds);

    for (i = 1; i < str->length; i++) {
        SpiceRect glyph_box;

        glyph = next_glyph;
        access_test(canvas, glyph, sizeof(SpiceRasterGlyph));
        next_glyph = canvas_next_raster_glyph(glyph, bpp);
        access_test(canvas, glyph, (uint8_t*)next_glyph - (uint8_t*)glyph);
        canvas_raster_glyph_box(glyph, &glyph_box);
        rect_union(&bounds, &glyph_box);
    }

    str_mask = pixman_image_create_bits((bpp == 1) ? PIXMAN_a1 : PIXMAN_a8,
                                        bounds.right - bounds.left,
                                        bounds.bottom - bounds.top, NULL, 0);
    if (str_mask == NULL) {
        CANVAS_ERROR("create surface failed");
    }
    dest = (uint8_t *)pixman_image_get_data(str_mask);
    dest_stride = pixman_image_get_stride(str_mask);
    glyph = (SpiceRasterGlyph *)str->data;
    for (i = 0; i < str->length; i++) {
#if defined(GL_CANVAS)
        canvas_put_glyph_bits(glyph, bpp, dest + (bounds.bottom - bounds.top - 1) * dest_stride,
                              -dest_stride, &bounds);
#else
        canvas_put_glyph_bits(glyph, bpp, dest, dest_stride, &bounds);
#endif
        glyph = canvas_next_raster_glyph(glyph, bpp);
    }

    pos->x = bounds.left;
    pos->y = bounds.top;
    return str_mask;
}

static inline SpiceVectorGlyph *canvas_next_vector_glyph(const SpiceVectorGlyph *glyph)
{
    return (SpiceVectorGlyph *)((uint8_t *)(glyph + 1) + glyph->data_size);
}

static pixman_image_t *canvas_scale_surface(pixman_image_t *src, const SpiceRect *src_area, int width,
                                            int height, int scale_mode)
{
    pixman_image_t *surface;
    pixman_transform_t transform;
    double sx, sy;

    surface = pixman_image_create_bits(PIXMAN_x8r8g8b8, width, height, NULL, 0);
    if (surface == NULL) {
        CANVAS_ERROR("create surface failed");
    }

    sx = (double)(src_area->right - src_area->left) / width;
    sy = (double)(src_area->bottom - src_area->top) / height;

    pixman_transform_init_scale(&transform, pixman_double_to_fixed(sx), pixman_double_to_fixed(sy));

    pixman_image_set_transform (src, &transform);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    ASSERT(scale_mode == SPICE_IMAGE_SCALE_MODE_INTERPOLATE || scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST);
    pixman_image_set_filter(src,
                            (scale_mode == SPICE_IMAGE_SCALE_MODE_NEAREST) ?PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_GOOD,
                            NULL, 0);

    pixman_image_composite32(PIXMAN_OP_SRC,
                             src, NULL, surface,
                             ROUND(src_area->left / sx), ROUND (src_area->top / sy),
                             0, 0, /* mask */
                             0, 0, /* dst */
                             width, height);

    pixman_image_set_transform(src, NULL);

    return surface;
}

static void quic_usr_error(QuicUsrContext *usr, const char *fmt, ...)
{
    QuicData *usr_data = (QuicData *)usr;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);

    longjmp(usr_data->jmp_env, 1);
}

static void quic_usr_warn(QuicUsrContext *usr, const char *fmt, ...)
{
    QuicData *usr_data = (QuicData *)usr;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
}

static void *quic_usr_malloc(QuicUsrContext *usr, int size)
{
    return malloc(size);
}

static void quic_usr_free(QuicUsrContext *usr, void *ptr)
{
    free(ptr);
}

#ifdef CAIRO_CANVAS_NO_CHUNKS

static int quic_usr_more_space(QuicUsrContext *usr, uint32_t **io_ptr, int rows_completed)
{
    return 0;
}

static void lz_usr_warn(LzUsrContext *usr, const char *fmt, ...)
{
    LzData *usr_data = (LzData *)usr;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
}

static void lz_usr_error(LzUsrContext *usr, const char *fmt, ...)
{
    LzData *usr_data = (LzData *)usr;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);

    longjmp(usr_data->jmp_env, 1);
}

static void *lz_usr_malloc(LzUsrContext *usr, int size)
{
    return malloc(size);
}

static void lz_usr_free(LzUsrContext *usr, void *ptr)
{
    free(ptr);
}

static int lz_usr_more_space(LzUsrContext *usr, uint8_t **io_ptr)
{
    return 0;
}

static int lz_usr_more_lines(LzUsrContext *usr, uint8_t **lines)
{
    return 0;
}

#else

static int quic_usr_more_space(QuicUsrContext *usr, uint32_t **io_ptr, int rows_completed)
{
    QuicData *quic_data = (QuicData *)usr;
    DataChunk *chunk;
    uint32_t size;

    if (!quic_data->next) {
        return 0;
    }
    chunk = (DataChunk *)quic_data->virt_mapping->ops->get_virt(quic_data->virt_mapping, quic_data->next,
                                                                sizeof(DataChunk));
    size = chunk->size;
    quic_data->virt_mapping->ops->validate_virt(quic_data->virt_mapping, (unsigned long)chunk->data,
                                                quic_data->next, size);

    quic_data->next = chunk->next;
    *io_ptr = (uint32_t *)chunk->data;
    return size >> 2;
}

#endif

static int quic_usr_more_lines(QuicUsrContext *usr, uint8_t **lines)
{
    return 0;
}

#ifdef CAIRO_CANVAS_ACCESS_TEST
static void __canvas_set_access_params(CanvasBase *canvas, unsigned long base, unsigned long max)
{
    canvas->base = base;
    canvas->max = max;
}
#endif

static void canvas_base_destroy(CanvasBase *canvas)
{
    quic_destroy(canvas->quic_data.quic);
#ifdef CAIRO_CANVAS_NO_CHUNKS
    lz_destroy(canvas->lz_data.lz);
#endif
#ifdef GDI_CANVAS
    DeleteDC(canvas->dc);
#endif

    if (canvas->usr_data && canvas->usr_data_destroy) {
        canvas->usr_data_destroy (canvas->usr_data);
        canvas->usr_data = NULL;
    }
}

/* This is kind of lame, but it protects against muliple
   instances of these functions. We really should stop including
   canvas_base.c and build it separately instead */
#ifdef  CANVAS_SINGLE_INSTANCE

void spice_canvas_set_usr_data(SpiceCanvas *spice_canvas,
                               void *data,
                               spice_destroy_fn_t destroy_fn)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    if (canvas->usr_data && canvas->usr_data_destroy) {
        canvas->usr_data_destroy (canvas->usr_data);
    }
    canvas->usr_data = data;
    canvas->usr_data_destroy = destroy_fn;
}

void *spice_canvas_get_usr_data(SpiceCanvas *spice_canvas)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    return  canvas->usr_data;
}
#endif


static void canvas_clip_pixman(CanvasBase *canvas,
                               pixman_region32_t *dest_region,
                               SpiceClip *clip)
{
    pixman_region32_intersect(dest_region, dest_region, &canvas->canvas_region);

    switch (clip->type) {
    case SPICE_CLIP_TYPE_NONE:
        break;
    case SPICE_CLIP_TYPE_RECTS: {
        uint32_t *n = (uint32_t *)SPICE_GET_ADDRESS(clip->data);
        access_test(canvas, n, sizeof(uint32_t));

        SpiceRect *now = (SpiceRect *)(n + 1);
        access_test(canvas, now, (unsigned long)(now + *n) - (unsigned long)now);

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

static void canvas_mask_pixman(CanvasBase *canvas,
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
    image = canvas_get_mask(canvas,
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

static void draw_brush(SpiceCanvas *canvas,
                       pixman_region32_t *region,
                       SpiceBrush *brush,
                       SpiceROP rop)
{
    CanvasBase *canvas_base = (CanvasBase *)canvas;
    uint32_t color;
    SpicePattern *pattern;
    pixman_image_t *tile;
    int offset_x, offset_y;
    pixman_box32_t *rects;
    int n_rects;

    rects = pixman_region32_rectangles(region, &n_rects);

   switch (brush->type) {
    case SPICE_BRUSH_TYPE_SOLID:
        color = brush->u.color;
        if (rop == SPICE_ROP_COPY) {
            canvas->ops->fill_solid_rects(canvas, rects, n_rects, color);
        } else {
            canvas->ops->fill_solid_rects_rop(canvas, rects, n_rects, color, rop);
        }
        break;
    case SPICE_BRUSH_TYPE_PATTERN:
        pattern = &brush->u.pattern;
        tile = canvas_get_image(canvas_base, pattern->pat);
        offset_x = pattern->pos.x;
        offset_y = pattern->pos.y;

        if (rop == SPICE_ROP_COPY) {
            canvas->ops->fill_tiled_rects(canvas, rects, n_rects, tile, offset_x, offset_y);
        } else {
            canvas->ops->fill_tiled_rects_rop(canvas, rects, n_rects,
                                              tile, offset_x, offset_y, rop);
        }
        pixman_image_unref(tile);
        break;
    case SPICE_BRUSH_TYPE_NONE:
        /* Still need to do *something* here, because rop could be e.g invert dest */
        canvas->ops->fill_solid_rects_rop(canvas, rects, n_rects, 0, rop);
        break;
    default:
        CANVAS_ERROR("invalid brush type");
    }
}

/* If we're exiting early we may still have to load an image in case
   it has to be cached or something */
static void touch_brush(CanvasBase *canvas, SpiceBrush *brush)
{
    SpicePattern *pattern;

    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        pattern = &brush->u.pattern;
        canvas_touch_image(canvas, pattern->pat);
    }
}

static void canvas_draw_fill(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
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

    draw_brush(spice_canvas, &dest_region, &fill->brush, rop);

    pixman_region32_fini(&dest_region);
}

static void canvas_draw_copy(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_t dest_region;
    pixman_image_t *src_image;
    SpiceROP rop;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &copy->mask,
                       bbox->left, bbox->top);

    rop = ropd_descriptor_to_rop(copy->rop_decriptor,
                                 ROP_INPUT_SRC,
                                 ROP_INPUT_DEST);

    if (rop == SPICE_ROP_NOOP || pixman_region32_n_rects(&dest_region) == 0) {
        canvas_touch_image(canvas, copy->src_bitmap);
        pixman_region32_fini(&dest_region);
        return;
    }

    src_image = canvas_get_image(canvas, copy->src_bitmap);

    if (rect_is_same_size(bbox, &copy->src_area)) {
        if (rop == SPICE_ROP_COPY) {
            spice_canvas->ops->blit_image(spice_canvas, &dest_region,
                                          src_image,
                                          bbox->left - copy->src_area.left,
                                          bbox->top - copy->src_area.top);
        } else {
            spice_canvas->ops->blit_image_rop(spice_canvas, &dest_region,
                                              src_image,
                                              bbox->left - copy->src_area.left,
                                              bbox->top - copy->src_area.top,
                                              rop);
        }
    } else {
        if (rop == SPICE_ROP_COPY) {
            spice_canvas->ops->scale_image(spice_canvas, &dest_region,
                                           src_image,
                                           copy->src_area.left,
                                           copy->src_area.top,
                                           copy->src_area.right - copy->src_area.left,
                                           copy->src_area.bottom - copy->src_area.top,
                                           bbox->left,
                                           bbox->top,
                                           bbox->right - bbox->left,
                                           bbox->bottom - bbox->top,
                                           copy->scale_mode);
        } else {
            spice_canvas->ops->scale_image_rop(spice_canvas, &dest_region,
                                               src_image,
                                               copy->src_area.left,
                                               copy->src_area.top,
                                               copy->src_area.right - copy->src_area.left,
                                               copy->src_area.bottom - copy->src_area.top,
                                               bbox->left,
                                               bbox->top,
                                               bbox->right - bbox->left,
                                               bbox->bottom - bbox->top,
                                               copy->scale_mode,
                                               rop);
        }
    }

    pixman_image_unref(src_image);
    pixman_region32_fini(&dest_region);
}

static void canvas_draw_transparent(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_image_t *src_image;
    pixman_region32_t dest_region;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &dest_region, clip);

    if (pixman_region32_n_rects (&dest_region) == 0) {
        canvas_touch_image(canvas, transparent->src_bitmap);
        pixman_region32_fini(&dest_region);
        return;
    }

    src_image = canvas_get_image(canvas, transparent->src_bitmap);

    if (rect_is_same_size(bbox, &transparent->src_area)) {
        spice_canvas->ops->colorkey_image(spice_canvas, &dest_region,
                                          src_image,
                                          bbox->left - transparent->src_area.left,
                                          bbox->top - transparent->src_area.top,
                                          transparent->true_color);
    } else {
        spice_canvas->ops->colorkey_scale_image(spice_canvas, &dest_region,
                                                src_image,
                                                transparent->src_area.left,
                                                transparent->src_area.top,
                                                transparent->src_area.right - transparent->src_area.left,
                                                transparent->src_area.bottom - transparent->src_area.top,
                                                bbox->left,
                                                bbox->top,
                                                bbox->right - bbox->left,
                                                bbox->bottom - bbox->top,
                                                transparent->true_color);
    }

    pixman_image_unref(src_image);
    pixman_region32_fini(&dest_region);
}

static void canvas_draw_alpha_blend(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_t dest_region;
    pixman_image_t *src_image;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &dest_region, clip);

    if (alpha_blend->alpha == 0 ||
        pixman_region32_n_rects(&dest_region) == 0) {
        canvas_touch_image(canvas, alpha_blend->src_bitmap);
        pixman_region32_fini(&dest_region);
        return;
    }

    src_image = canvas_get_image(canvas, alpha_blend->src_bitmap);

    if (rect_is_same_size(bbox, &alpha_blend->src_area)) {
        spice_canvas->ops->blend_image(spice_canvas, &dest_region,
                                       src_image,
                                       alpha_blend->src_area.left,
                                       alpha_blend->src_area.top,
                                       bbox->left,
                                       bbox->top,
                                       bbox->right - bbox->left,
                                       bbox->bottom - bbox->top,
                                       alpha_blend->alpha);
    } else {
        spice_canvas->ops->blend_scale_image(spice_canvas, &dest_region,
                                             src_image,
                                             alpha_blend->src_area.left,
                                             alpha_blend->src_area.top,
                                             alpha_blend->src_area.right - alpha_blend->src_area.left,
                                             alpha_blend->src_area.bottom - alpha_blend->src_area.top,
                                             bbox->left,
                                             bbox->top,
                                             bbox->right - bbox->left,
                                             bbox->bottom - bbox->top,
                                             SPICE_IMAGE_SCALE_MODE_INTERPOLATE,
                                             alpha_blend->alpha);
    }

    pixman_image_unref(src_image);
    pixman_region32_fini(&dest_region);
}

static void canvas_draw_opaque(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_image_t *src_image;
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
        canvas_touch_image(canvas, opaque->src_bitmap);
        touch_brush(canvas, &opaque->brush);
        pixman_region32_fini(&dest_region);
        return;
    }

    src_image = canvas_get_image(canvas, opaque->src_bitmap);

    if (rect_is_same_size(bbox, &opaque->src_area)) {
        spice_canvas->ops->blit_image(spice_canvas, &dest_region,
                                      src_image,
                                      bbox->left - opaque->src_area.left,
                                      bbox->top - opaque->src_area.top);
    } else {
        spice_canvas->ops->scale_image(spice_canvas, &dest_region,
                                       src_image,
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

    draw_brush(spice_canvas, &dest_region, &opaque->brush, rop);

    pixman_image_unref(src_image);
    pixman_region32_fini(&dest_region);
}

static void canvas_draw_blend(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_image_t *src_image;
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
        canvas_touch_image(canvas, blend->src_bitmap);
        pixman_region32_fini(&dest_region);
        return;
    }

    src_image = canvas_get_image(canvas, blend->src_bitmap);

    if (rect_is_same_size(bbox, &blend->src_area)) {
        if (rop == SPICE_ROP_COPY)
            spice_canvas->ops->blit_image(spice_canvas, &dest_region,
                                          src_image,
                                          bbox->left - blend->src_area.left,
                                          bbox->top - blend->src_area.top);
        else
            spice_canvas->ops->blit_image_rop(spice_canvas, &dest_region,
                                              src_image,
                                              bbox->left - blend->src_area.left,
                                              bbox->top - blend->src_area.top,
                                              rop);
    } else {
        double sx, sy;

        sx = (double)(blend->src_area.right - blend->src_area.left) / (bbox->right - bbox->left);
        sy = (double)(blend->src_area.bottom - blend->src_area.top) / (bbox->bottom - bbox->top);

        if (rop == SPICE_ROP_COPY) {
            spice_canvas->ops->scale_image(spice_canvas, &dest_region,
                                           src_image,
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
            spice_canvas->ops->scale_image_rop(spice_canvas, &dest_region,
                                               src_image,
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

    pixman_image_unref(src_image);
    pixman_region32_fini(&dest_region);
}

static void canvas_draw_blackness(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_t dest_region;
    pixman_box32_t *rects;
    int n_rects;

   pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);


    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &blackness->mask,
                       bbox->left, bbox->top);

    if (pixman_region32_n_rects(&dest_region) == 0) {
        pixman_region32_fini (&dest_region);
        return;
    }

    rects = pixman_region32_rectangles(&dest_region, &n_rects);

    spice_canvas->ops->fill_solid_rects(spice_canvas, rects, n_rects, 0x000000);

    pixman_region32_fini(&dest_region);
}

static void canvas_draw_whiteness(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_t dest_region;
    pixman_box32_t *rects;
    int n_rects;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);


    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &whiteness->mask,
                       bbox->left, bbox->top);

    if (pixman_region32_n_rects(&dest_region) == 0) {
        pixman_region32_fini(&dest_region);
        return;
    }

    rects = pixman_region32_rectangles(&dest_region, &n_rects);
    spice_canvas->ops->fill_solid_rects(spice_canvas, rects, n_rects, 0xffffffff);

    pixman_region32_fini(&dest_region);
}

static void canvas_draw_invers(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_t dest_region;
    pixman_box32_t *rects;
    int n_rects;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);


    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &invers->mask,
                       bbox->left, bbox->top);

    if (pixman_region32_n_rects(&dest_region) == 0) {
        pixman_region32_fini(&dest_region);
        return;
    }

    rects = pixman_region32_rectangles(&dest_region, &n_rects);
    spice_canvas->ops->fill_solid_rects_rop(spice_canvas, rects, n_rects, 0x00000000,
                                            SPICE_ROP_INVERT);

    pixman_region32_fini(&dest_region);
}

typedef struct {
    lineGC base;
    SpiceCanvas *canvas;
    pixman_region32_t dest_region;
    SpiceROP fore_rop;
    SpiceROP back_rop;
    int solid;
    uint32_t color;
    pixman_image_t *tile;
    int tile_offset_x;
    int tile_offset_y;
} StrokeGC;

static void stroke_fill_spans(lineGC * pGC,
                              int num_spans,
                              SpicePoint *points,
                              int *widths,
                              int sorted,
                              int foreground)
{
    SpiceCanvas *canvas;
    StrokeGC *strokeGC;
    int i;
    SpiceROP rop;

    strokeGC = (StrokeGC *)pGC;
    canvas = strokeGC->canvas;

    num_spans = spice_canvas_clip_spans(&strokeGC->dest_region,
                                        points, widths, num_spans,
                                        points, widths, sorted);

    if (foreground) {
        rop = strokeGC->fore_rop;
    } else {
        rop = strokeGC->back_rop;
    }

    if (strokeGC->solid) {
        if (rop == SPICE_ROP_COPY) {
            canvas->ops->fill_solid_spans(canvas, points, widths, num_spans,
                                          strokeGC->color);
        } else {
            for (i = 0; i < num_spans; i++) {
                pixman_box32_t r;
                r.x1 = points[i].x;
                r.y1 = points[i].y;
                r.x2 = points[i].x + widths[i];
                canvas->ops->fill_solid_rects_rop(canvas, &r, 1,
                                                  strokeGC->color, rop);
            }
        }
    } else {
        if (rop == SPICE_ROP_COPY) {
            for (i = 0; i < num_spans; i++) {
                pixman_box32_t r;
                r.x1 = points[i].x;
                r.y1 = points[i].y;
                r.x2 = points[i].x + widths[i];
                canvas->ops->fill_tiled_rects(canvas, &r, 1,
                                              strokeGC->tile,
                                              strokeGC->tile_offset_x,
                                              strokeGC->tile_offset_y);
            }
        } else {
            for (i = 0; i < num_spans; i++) {
                pixman_box32_t r;
                r.x1 = points[i].x;
                r.y1 = points[i].y;
                r.x2 = points[i].x + widths[i];
                canvas->ops->fill_tiled_rects_rop(canvas, &r, 1,
                                                  strokeGC->tile,
                                                  strokeGC->tile_offset_x,
                                                  strokeGC->tile_offset_y, rop);
            }
        }
    }
}

static void stroke_fill_rects(lineGC * pGC,
                              int num_rects,
                              pixman_rectangle32_t *rects,
                              int foreground)
{
    SpiceCanvas *canvas;
    pixman_region32_t area;
    pixman_box32_t *boxes;
    StrokeGC *strokeGC;
    SpiceROP rop;
    int i;
    pixman_box32_t *area_rects;
    int n_area_rects;

    strokeGC = (StrokeGC *)pGC;
    canvas = strokeGC->canvas;

    if (foreground) {
        rop = strokeGC->fore_rop;
    } else {
        rop = strokeGC->back_rop;
    }

    /* TODO: We can optimize this for more common cases where
       dest is one rect */

    boxes = (pixman_box32_t *)malloc(num_rects * sizeof(pixman_box32_t));
    for (i = 0; i < num_rects; i++) {
        boxes[i].x1 = rects[i].x;
        boxes[i].y1 = rects[i].y;
        boxes[i].x2 = rects[i].x + rects[i].width;
        boxes[i].y2 = rects[i].y + rects[i].height;
    }
    pixman_region32_init_rects(&area, boxes, num_rects);
    pixman_region32_intersect(&area, &area, &strokeGC->dest_region);
    free(boxes);

    area_rects = pixman_region32_rectangles(&area, &n_area_rects);

    if (strokeGC->solid) {
        if (rop == SPICE_ROP_COPY) {
            canvas->ops->fill_solid_rects(canvas, area_rects, n_area_rects,
                                          strokeGC->color);
        } else {
            canvas->ops->fill_solid_rects_rop(canvas, area_rects, n_area_rects,
                                              strokeGC->color, rop);
        }
    } else {
        if (rop == SPICE_ROP_COPY) {
            canvas->ops->fill_tiled_rects(canvas, area_rects, n_area_rects,
                                          strokeGC->tile,
                                          strokeGC->tile_offset_x,
                                          strokeGC->tile_offset_y);
        } else {
            canvas->ops->fill_tiled_rects_rop(canvas, area_rects, n_area_rects,
                                              strokeGC->tile,
                                              strokeGC->tile_offset_x,
                                              strokeGC->tile_offset_y,
                                              rop);
        }
    }

   pixman_region32_fini(&area);
}

typedef struct {
    SpicePoint *points;
    int num_points;
    int size;
} StrokeLines;

static void stroke_lines_init(StrokeLines *lines)
{
    lines->points = (SpicePoint *)malloc(10*sizeof(SpicePoint));
    lines->size = 10;
    lines->num_points = 0;
}

static void stroke_lines_free(StrokeLines *lines)
{
    free(lines->points);
}

static void stroke_lines_append(StrokeLines *lines,
                                int x, int y)
{
    if (lines->num_points == lines->size) {
        lines->size *= 2;
        lines->points = (SpicePoint *)realloc(lines->points,
                                              lines->size * sizeof(SpicePoint));
    }
    lines->points[lines->num_points].x = x;
    lines->points[lines->num_points].y = y;
    lines->num_points++;
}

static void stroke_lines_append_fix(StrokeLines *lines,
                                    SpicePointFix *point)
{
    stroke_lines_append(lines,
                        fix_to_int(point->x),
                        fix_to_int(point->y));
}

static inline int64_t dot(SPICE_FIXED28_4 x1,
                          SPICE_FIXED28_4 y1,
                          SPICE_FIXED28_4 x2,
                          SPICE_FIXED28_4 y2)
{
    return (((int64_t)x1) *((int64_t)x2) +
            ((int64_t)y1) *((int64_t)y2)) >> 4;
}

static inline int64_t dot2(SPICE_FIXED28_4 x,
                           SPICE_FIXED28_4 y)
{
    return (((int64_t)x) *((int64_t)x) +
            ((int64_t)y) *((int64_t)y)) >> 4;
}

static void subdivide_bezier(StrokeLines *lines,
                             SpicePointFix point0,
                             SpicePointFix point1,
                             SpicePointFix point2,
                             SpicePointFix point3)
{
    int64_t A2, B2, C2, AB, CB, h1, h2;

    A2 = dot2(point1.x - point0.x,
              point1.y - point0.y);
    B2 = dot2(point3.x - point0.x,
              point3.y - point0.y);
    C2 = dot2(point2.x - point3.x,
              point2.y - point3.y);

    AB = dot(point1.x - point0.x,
             point1.y - point0.y,
             point3.x - point0.x,
             point3.y - point0.y);

    CB = dot(point2.x - point3.x,
             point2.y - point3.y,
             point0.x - point3.x,
             point0.y - point3.y);

    h1 = (A2*B2 - AB*AB) >> 3;
    h2 = (C2*B2 - CB*CB) >> 3;

    if (h1 < B2 && h2 < B2) {
        /* deviation squared less than half a pixel, use straight line */
        stroke_lines_append_fix(lines, &point3);
    } else {
        SpicePointFix point01, point23, point12, point012, point123, point0123;

        point01.x = (point0.x + point1.x) / 2;
        point01.y = (point0.y + point1.y) / 2;
        point12.x = (point1.x + point2.x) / 2;
        point12.y = (point1.y + point2.y) / 2;
        point23.x = (point2.x + point3.x) / 2;
        point23.y = (point2.y + point3.y) / 2;
        point012.x = (point01.x + point12.x) / 2;
        point012.y = (point01.y + point12.y) / 2;
        point123.x = (point12.x + point23.x) / 2;
        point123.y = (point12.y + point23.y) / 2;
        point0123.x = (point012.x + point123.x) / 2;
        point0123.y = (point012.y + point123.y) / 2;

        subdivide_bezier(lines, point0, point01, point012, point0123);
        subdivide_bezier(lines, point0123, point123, point23, point3);
    }
}

static void stroke_lines_append_bezier(StrokeLines *lines,
                                       SpicePointFix *point1,
                                       SpicePointFix *point2,
                                       SpicePointFix *point3)
{
    SpicePointFix point0;

    point0.x = int_to_fix(lines->points[lines->num_points-1].x);
    point0.y = int_to_fix(lines->points[lines->num_points-1].y);

    subdivide_bezier(lines, point0, *point1, *point2, *point3);
}

static void stroke_lines_draw(StrokeLines *lines,
                              lineGC *gc,
                              int dashed)
{
    if (lines->num_points != 0) {
        if (dashed) {
            spice_canvas_zero_dash_line(gc, CoordModeOrigin,
                                        lines->num_points, lines->points);
        } else {
            spice_canvas_zero_line(gc, CoordModeOrigin,
                                   lines->num_points, lines->points);
        }
        lines->num_points = 0;
    }
}


static void canvas_draw_stroke(SpiceCanvas *spice_canvas, SpiceRect *bbox,
                               SpiceClip *clip, SpiceStroke *stroke)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    StrokeGC gc = { { 0 } };
    lineGCOps ops = {
        stroke_fill_spans,
        stroke_fill_rects
    };
    uint32_t *data_size;
    uint32_t more;
    SpicePathSeg *seg;
    StrokeLines lines;
    int i;
    int dashed;

    pixman_region32_init_rect(&gc.dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &gc.dest_region, clip);

    if (pixman_region32_n_rects(&gc.dest_region) == 0) {
        touch_brush(canvas, &stroke->brush);
        pixman_region32_fini(&gc.dest_region);
        return;
    }

    gc.canvas = spice_canvas;
    gc.fore_rop = ropd_descriptor_to_rop(stroke->fore_mode,
                                         ROP_INPUT_BRUSH,
                                         ROP_INPUT_DEST);
    gc.back_rop = ropd_descriptor_to_rop(stroke->back_mode,
                                         ROP_INPUT_BRUSH,
                                         ROP_INPUT_DEST);

    gc.base.width = canvas->width;
    gc.base.height = canvas->height;
    gc.base.alu = gc.fore_rop;
    gc.base.lineWidth = 0;

    /* dash */
    gc.base.dashOffset = 0;
    gc.base.dash = NULL;
    gc.base.numInDashList = 0;
    gc.base.lineStyle = LineSolid;
    /* win32 cosmetic lines are endpoint-exclusive, so use CapNotLast */
    gc.base.capStyle = CapNotLast;
    gc.base.joinStyle = JoinMiter;
    gc.base.ops = &ops;

    dashed = 0;
    if (stroke->attr.flags & SPICE_LINE_FLAGS_STYLED) {
        SPICE_FIXED28_4 *style = (SPICE_FIXED28_4*)SPICE_GET_ADDRESS(stroke->attr.style);
        int nseg;

        dashed = 1;

        nseg = stroke->attr.style_nseg;

        /* To truly handle back_mode we should use LineDoubleDash here
           and treat !foreground as back_rop using the same brush.
           However, using the same brush for that seems wrong.
           The old cairo backend was stroking the non-dashed line with
           rop_mode before enabling dashes for the foreground which is
           not right either. The gl an gdi backend don't use back_mode
           at all */
        gc.base.lineStyle = LineOnOffDash;
        gc.base.dash = (unsigned char *)malloc(nseg);
        gc.base.numInDashList = nseg;
        access_test(canvas, style, nseg * sizeof(*style));

        if (stroke->attr.flags & SPICE_LINE_FLAGS_START_WITH_GAP) {
            gc.base.dash[stroke->attr.style_nseg - 1] = fix_to_int(style[0]);
            for (i = 0; i < stroke->attr.style_nseg - 1; i++) {
                gc.base.dash[i] = fix_to_int(style[i+1]);
            }
            gc.base.dashOffset = gc.base.dash[0];
        } else {
            for (i = 0; i < stroke->attr.style_nseg; i++) {
                gc.base.dash[i] = fix_to_int(style[i]);
            }
        }
    }

    switch (stroke->brush.type) {
    case SPICE_BRUSH_TYPE_NONE:
        gc.solid = TRUE;
        gc.color = 0;
        break;
    case SPICE_BRUSH_TYPE_SOLID:
        gc.solid = TRUE;
        gc.color = stroke->brush.u.color;
        break;
    case SPICE_BRUSH_TYPE_PATTERN:
        gc.solid = FALSE;
        gc.tile = canvas_get_image(canvas,
                                   stroke->brush.u.pattern.pat);
        gc.tile_offset_x = stroke->brush.u.pattern.pos.x;
        gc.tile_offset_y = stroke->brush.u.pattern.pos.y;
        break;
    default:
        CANVAS_ERROR("invalid brush type");
    }

    data_size = (uint32_t*)SPICE_GET_ADDRESS(stroke->path);
    access_test(canvas, data_size, sizeof(uint32_t));
    more = *data_size;
    seg = (SpicePathSeg*)(data_size + 1);

    stroke_lines_init(&lines);

    do {
        access_test(canvas, seg, sizeof(SpicePathSeg));

        uint32_t flags = seg->flags;
        SpicePointFix* point = (SpicePointFix*)seg->data;
        SpicePointFix* end_point = point + seg->count;
        access_test(canvas, point, (unsigned long)end_point - (unsigned long)point);
        ASSERT(point < end_point);
        more -= ((unsigned long)end_point - (unsigned long)seg);
        seg = (SpicePathSeg*)end_point;

        if (flags & SPICE_PATH_BEGIN) {
            stroke_lines_draw(&lines, (lineGC *)&gc, dashed);
            stroke_lines_append_fix(&lines, point);
            point++;
        }

        if (flags & SPICE_PATH_BEZIER) {
            ASSERT((point - end_point) % 3 == 0);
            for (; point + 2 < end_point; point += 3) {
                stroke_lines_append_bezier(&lines,
                                           &point[0],
                                           &point[1],
                                           &point[2]);
            }
        } else
            {
            for (; point < end_point; point++) {
                stroke_lines_append_fix(&lines, point);
            }
        }
        if (flags & SPICE_PATH_END) {
            if (flags & SPICE_PATH_CLOSE) {
                stroke_lines_append(&lines,
                                    lines.points[0].x, lines.points[0].y);
            }
            stroke_lines_draw(&lines, (lineGC *)&gc, dashed);
        }
    } while (more);

    stroke_lines_draw(&lines, (lineGC *)&gc, dashed);

    if (gc.base.dash) {
        free(gc.base.dash);
    }
    stroke_lines_free(&lines);

    if (!gc.solid && gc.tile) {
        pixman_image_unref(gc.tile);
    }

    pixman_region32_fini(&gc.dest_region);
}

static void canvas_draw_rop3(SpiceCanvas *spice_canvas, SpiceRect *bbox,
                             SpiceClip *clip, SpiceRop3 *rop3)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_t dest_region;
    pixman_image_t *d;
    pixman_image_t *s;
    SpicePoint src_pos;
    int width;
    int heigth;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &dest_region, clip);
    canvas_mask_pixman(canvas, &dest_region, &rop3->mask,
                       bbox->left, bbox->top);

    width = bbox->right - bbox->left;
    heigth = bbox->bottom - bbox->top;

    d = canvas_get_image_from_self(spice_canvas, bbox->left, bbox->top, width, heigth);
    s = canvas_get_image(canvas, rop3->src_bitmap);

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
        pixman_image_t *p = canvas_get_image(canvas, rop3->brush.u.pattern.pat);
        SpicePoint pat_pos;

        pat_pos.x = (bbox->left - rop3->brush.u.pattern.pos.x) % pixman_image_get_width(p);
        pat_pos.y = (bbox->top - rop3->brush.u.pattern.pos.y) % pixman_image_get_height(p);
        do_rop3_with_pattern(rop3->rop3, d, s, &src_pos, p, &pat_pos);
        pixman_image_unref(p);
    } else {
        uint32_t color = (canvas->color_shift) == 8 ? rop3->brush.u.color :
                                                         canvas_16bpp_to_32bpp(rop3->brush.u.color);
        do_rop3_with_color(rop3->rop3, d, s, &src_pos, color);
    }
    pixman_image_unref(s);

    spice_canvas->ops->blit_image(spice_canvas, &dest_region, d,
                                  bbox->left,
                                  bbox->top);

    pixman_image_unref(d);

    pixman_region32_fini(&dest_region);
}

static void canvas_copy_bits(SpiceCanvas *spice_canvas, SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_t dest_region;
    int dx, dy;

    pixman_region32_init_rect(&dest_region,
                              bbox->left, bbox->top,
                              bbox->right - bbox->left,
                              bbox->bottom - bbox->top);

    canvas_clip_pixman(canvas, &dest_region, clip);

    dx = bbox->left - src_pos->x;
    dy = bbox->top - src_pos->y;

    if (dx != 0 || dy != 0) {
        pixman_region32_t src_region;

        /* Clip so we don't read outside canvas */
        pixman_region32_init_rect(&src_region,
                                  dx, dy,
                                  canvas->width,
                                  canvas->height);
        pixman_region32_intersect(&dest_region, &dest_region, &src_region);
        pixman_region32_fini(&src_region);

        spice_canvas->ops->copy_region(spice_canvas, &dest_region, dx, dy);
    }

    pixman_region32_fini(&dest_region);
}



static void canvas_base_group_start(SpiceCanvas *spice_canvas, QRegion *region)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_fini(&canvas->canvas_region);

    /* Make sure we always clip to canvas size */
    pixman_region32_init_rect(&canvas->canvas_region,
                              0, 0,
                              canvas->width,
                              canvas->height);

    pixman_region32_intersect(&canvas->canvas_region, &canvas->canvas_region, region);
}

static void canvas_base_group_end(SpiceCanvas *spice_canvas)
{
    CanvasBase *canvas = (CanvasBase *)spice_canvas;
    pixman_region32_fini(&canvas->canvas_region);
    pixman_region32_init_rect(&canvas->canvas_region,
                              0, 0,
                              canvas->width,
                              canvas->height);
}


static void unimplemented_op(SpiceCanvas *canvas)
{
    PANIC("unimplemented canvas operation");
}

inline static void canvas_base_init_ops(SpiceCanvasOps *ops)
{
    void **ops_cast;
    int i;

    ops_cast = (void **)ops;
    for (i = 0; i < sizeof(SpiceCanvasOps) / sizeof(void *); i++) {
        ops_cast[i] = (void *) unimplemented_op;
    }

    ops->draw_fill = canvas_draw_fill;
    ops->draw_copy = canvas_draw_copy;
    ops->draw_opaque = canvas_draw_opaque;
    ops->copy_bits = canvas_copy_bits;
    ops->draw_blend = canvas_draw_blend;
    ops->draw_blackness = canvas_draw_blackness;
    ops->draw_whiteness = canvas_draw_whiteness;
    ops->draw_invers = canvas_draw_invers;
    ops->draw_transparent = canvas_draw_transparent;
    ops->draw_alpha_blend = canvas_draw_alpha_blend;
    ops->draw_stroke = canvas_draw_stroke;
    ops->draw_rop3 = canvas_draw_rop3;
    ops->group_start = canvas_base_group_start;
    ops->group_end = canvas_base_group_end;
}

static int canvas_base_init(CanvasBase *canvas, SpiceCanvasOps *ops,
                            int width, int height, int depth
#ifdef CAIRO_CANVAS_CACHE
                            , SpiceImageCache *bits_cache
                            , SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                            , SpiceImageCache *bits_cache
#endif
                            , SpiceGlzDecoder *glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                            , SpiceVirtMapping *virt_mapping
#endif
                            )
{
    canvas->parent.ops = ops;
    canvas->quic_data.usr.error = quic_usr_error;
    canvas->quic_data.usr.warn = quic_usr_warn;
    canvas->quic_data.usr.info = quic_usr_warn;
    canvas->quic_data.usr.malloc = quic_usr_malloc;
    canvas->quic_data.usr.free = quic_usr_free;
    canvas->quic_data.usr.more_space = quic_usr_more_space;
    canvas->quic_data.usr.more_lines = quic_usr_more_lines;
#ifndef CAIRO_CANVAS_NO_CHUNKS
    canvas->quic_data.virt_mapping = virt_mapping;
#endif
    if (!(canvas->quic_data.quic = quic_create(&canvas->quic_data.usr))) {
            return 0;
    }
#ifdef CAIRO_CANVAS_NO_CHUNKS
    canvas->lz_data.usr.error = lz_usr_error;
    canvas->lz_data.usr.warn = lz_usr_warn;
    canvas->lz_data.usr.info = lz_usr_warn;
    canvas->lz_data.usr.malloc = lz_usr_malloc;
    canvas->lz_data.usr.free = lz_usr_free;
    canvas->lz_data.usr.more_space = lz_usr_more_space;
    canvas->lz_data.usr.more_lines = lz_usr_more_lines;
    if (!(canvas->lz_data.lz = lz_create(&canvas->lz_data.usr))) {
            return 0;
    }
#endif
    canvas->glz_data.decoder = glz_decoder;

    if (depth == 16) {
        canvas->color_shift = 5;
        canvas->color_mask = 0x1f;
    } else {
        canvas->color_shift = 8;
        canvas->color_mask = 0xff;
    }

    canvas->width = width;
    canvas->height = height;
    pixman_region32_init_rect(&canvas->canvas_region,
                              0, 0,
                              canvas->width,
                              canvas->height);

#if defined(CAIRO_CANVAS_CACHE) || defined(CAIRO_CANVAS_IMAGE_CACHE)
    canvas->bits_cache = bits_cache;
#endif
#ifdef CAIRO_CANVAS_CACHE
    canvas->palette_cache = palette_cache;
#endif

#ifdef WIN32
    canvas->dc = NULL;
#endif

#ifdef GDI_CANVAS
    canvas->dc = create_compatible_dc();
    if (!canvas->dc) {
        lz_destroy(canvas->lz_data.lz);
        return 0;
    }
#endif
    return 1;
}

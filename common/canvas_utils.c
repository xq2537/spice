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

#include "canvas_utils.h"

#ifdef __GNUC__
#include <stdlib.h>
#include <stdio.h>
#endif

#ifdef WIN32
extern int gdi_handlers;
#endif

#ifndef ASSERT
#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    abort();                                                \
}
#endif


#ifndef CANVAS_ERROR
#define CANVAS_ERROR(format, ...) {                             \
    printf("%s: " format "\n", __FUNCTION__, ## __VA_ARGS__);   \
    abort();                                                    \
}
#endif

#ifndef ALIGN
#define ALIGN(a, b) (((a) + ((b) - 1)) & ~((b) - 1))
#endif

const cairo_user_data_key_t bitmap_data_type = {0};
const cairo_user_data_key_t bitmap_withstride_data_type = {0};

#ifdef WIN32
static void release_bitmap(void *bitmap_cache)
{
    DeleteObject((HBITMAP)((BitmapCache *)bitmap_cache)->bitmap);
    CloseHandle(((BitmapCache *)bitmap_cache)->mutex);
    free(bitmap_cache);
    gdi_handlers--;
}

#endif

static void release_withstride_bitmap(void *data)
{
    free(data);
}

static inline cairo_surface_t *__surface_create_stride(cairo_format_t format, int width, int height,
                                                       int stride)
{
    uint8_t *data;
    uint8_t *stride_data;
    cairo_surface_t *surface;

    data = (uint8_t *)malloc(abs(stride) * height);
    if (stride < 0) {
        stride_data = data + (-stride) * (height - 1);
    } else {
        stride_data = data;
    }

    surface = cairo_image_surface_create_for_data(stride_data, format, width, height, stride);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        free(data);
        CANVAS_ERROR("create surface failed, %s",
                     cairo_status_to_string(cairo_surface_status(surface)));
    }

    if (cairo_surface_set_user_data(surface, &bitmap_withstride_data_type, data,
                                    release_withstride_bitmap) != CAIRO_STATUS_SUCCESS) {
        free(data);
        cairo_surface_destroy(surface);
        CANVAS_ERROR("set_user_data surface failed, %s",
                     cairo_status_to_string(cairo_surface_status(surface)));
    }

    return surface;
}

#ifdef WIN32
cairo_surface_t *surface_create(HDC dc, cairo_format_t format,
                                int width, int height, int top_down)
#else
cairo_surface_t * surface_create(cairo_format_t format, int width, int height, int top_down)
#endif
{
#ifdef WIN32
    /*
     * Windows xp allow only 10,000 of gdi handlers, considering the fact that
     * we limit here the number to 5000, we dont use atomic operations to sync
     * this calculation against the other canvases (in case of multiple
     * monitors), in worst case there will be little more than 5000 gdi
     * handlers.
     */
    if (dc && gdi_handlers < 5000) {
        uint8_t *data;
        uint8_t *src;
        struct {
            BITMAPINFO inf;
            RGBQUAD palette[255];
        } bitmap_info;
        int nstride;
        cairo_surface_t *surface;
        BitmapCache *bitmap_cache;

        memset(&bitmap_info, 0, sizeof(bitmap_info));
        bitmap_info.inf.bmiHeader.biSize = sizeof(bitmap_info.inf.bmiHeader);
        bitmap_info.inf.bmiHeader.biWidth = width;

        bitmap_info.inf.bmiHeader.biHeight = (!top_down) ? height : -height;

        bitmap_info.inf.bmiHeader.biPlanes = 1;
        switch (format) {
        case CAIRO_FORMAT_ARGB32:
        case CAIRO_FORMAT_RGB24:
            bitmap_info.inf.bmiHeader.biBitCount = 32;
            nstride = width * 4;
            break;
        case CAIRO_FORMAT_A8:
            bitmap_info.inf.bmiHeader.biBitCount = 8;
            nstride = ALIGN(width, 4);
            break;
        case CAIRO_FORMAT_A1:
            bitmap_info.inf.bmiHeader.biBitCount = 1;
            nstride = ALIGN(width, 32) / 8;
            break;
        default:
            CANVAS_ERROR("invalid format");
        }

        bitmap_info.inf.bmiHeader.biCompression = BI_RGB;

        bitmap_cache = (BitmapCache *)malloc(sizeof(*bitmap_cache));
        if (!bitmap_cache) {
            CANVAS_ERROR("malloc failed");
            return NULL;
        }

        bitmap_cache->mutex = CreateMutex(NULL, 0, NULL);
        if (!bitmap_cache->mutex) {
            free(bitmap_cache);
            CANVAS_ERROR("Unable to CreateMutex");
            return NULL;
        }

        bitmap_cache->bitmap = CreateDIBSection(dc, &bitmap_info.inf, 0, (VOID **)&data, NULL, 0);
        if (!bitmap_cache->bitmap) {
            CloseHandle(bitmap_cache->mutex);
            free(bitmap_cache);
            CANVAS_ERROR("Unable to CreateDIBSection");
            return NULL;
        }

        if (top_down) {
            src = data;
        } else {
            src = data + nstride * (height - 1);
            nstride = -nstride;
        }

        surface = cairo_image_surface_create_for_data(src, format, width, height, nstride);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            CloseHandle(bitmap_cache->mutex);
            DeleteObject((HBITMAP)bitmap_cache->bitmap);
            free(bitmap_cache);
            CANVAS_ERROR("create surface failed, %s",
                         cairo_status_to_string(cairo_surface_status(surface)));
        }
        if (cairo_surface_set_user_data(surface, &bitmap_data_type, bitmap_cache,
                                        release_bitmap) != CAIRO_STATUS_SUCCESS) {
            CloseHandle(bitmap_cache->mutex);
            cairo_surface_destroy(surface);
            DeleteObject((HBITMAP)bitmap_cache->bitmap);
            free(bitmap_cache);
            CANVAS_ERROR("set_user_data surface failed, %s",
                         cairo_status_to_string(cairo_surface_status(surface)));
        }
        gdi_handlers++;
        return surface;
    } else {
#endif
    if (top_down) {
        return cairo_image_surface_create(format, width, height);
    } else {
        // NOTE: we assume here that the lz decoders always decode to RGB32.
        int stride = 0;
        switch (format) {
        case CAIRO_FORMAT_ARGB32:
        case CAIRO_FORMAT_RGB24:
            stride = width * 4;
            break;
        case CAIRO_FORMAT_A8:
            stride = ALIGN(width, 4);
            break;
        case CAIRO_FORMAT_A1:
            stride = ALIGN(width, 32) / 8;
            break;
        default:
            CANVAS_ERROR("invalid format");
        }
        stride = -stride;
        return __surface_create_stride(format, width, height, stride);
    }
#ifdef WIN32
}

#endif
}

#ifdef WIN32
cairo_surface_t *surface_create_stride(HDC dc, cairo_format_t format, int width, int height,
                                       int stride)
#else
cairo_surface_t *surface_create_stride(cairo_format_t format, int width, int height,
                                        int stride)
#endif
{
#ifdef WIN32
    if (dc) {
        if (abs(stride) == (width * 4)) {
            return surface_create(dc, format, width, height, (stride > 0));
        }
    }
#endif

    return __surface_create_stride(format, width, height, stride);
}

cairo_surface_t *alloc_lz_image_surface(LzDecodeUsrData *canvas_data, LzImageType type, int width,
                                        int height, int gross_pixels, int top_down)
{
    int stride;
    int alpha;
    cairo_surface_t *surface = NULL;

    stride = (gross_pixels / height) * 4;

    if (!top_down) {
        stride = -stride;
    }

    if (type == LZ_IMAGE_TYPE_RGB32) {
        alpha = 0;
    } else if (type == LZ_IMAGE_TYPE_RGBA) {
        alpha = 1;
    } else {
        CANVAS_ERROR("unexpected image type");
    }
    surface = surface_create_stride(
#ifdef WIN32
            canvas_data->dc,
#endif
            alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, width, height, stride);
    canvas_data->out_surface = surface;
    return surface;
}


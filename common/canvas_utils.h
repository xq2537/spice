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

#ifndef _H_CANVAS_UTILS
#define _H_CANVAS_UTILS

#ifdef __GNUC__
#include <stdint.h>
#else
#include <stddef.h>
#include <basetsd.h>
typedef UINT8 uint8_t;
#endif  //__GNUC__

#include "cairo.h"
#include "lz.h"

#ifdef WIN32
typedef struct BitmapCache {
    HBITMAP bitmap;
    HANDLE mutex;
} BitmapCache;
#endif

extern const cairo_user_data_key_t bitmap_data_type;

#ifdef WIN32
cairo_surface_t *surface_create(HDC dc, cairo_format_t format,
                                int width, int height, int top_down);
#else
cairo_surface_t *surface_create(cairo_format_t format, int width, int height, int top_down);
#endif

#ifdef WIN32
cairo_surface_t *surface_create_stride(HDC dc, cairo_format_t format, int width, int height,
                                       int stride);
#else
cairo_surface_t *surface_create_stride(cairo_format_t format, int width, int height,
                                       int stride);
#endif


typedef struct LzDecodeUsrData {
#ifdef WIN32
    HDC dc;
#endif
    cairo_surface_t       *out_surface;
} LzDecodeUsrData;


cairo_surface_t *alloc_lz_image_surface(LzDecodeUsrData *canvas_data, LzImageType type, int width,
                                        int height, int gross_pixels, int top_down);
#endif

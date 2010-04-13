/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "red_pixmap_gdi.h"
#include "red_pixmap.h"
#include "debug.h"
#include "utils.h"
#include "pixels_source_p.h"
#include "platform_utils.h"

struct RedPixmap_p {
    PixelsSource_p pixels_source_p;
    HBITMAP prev_bitmap;
};

static inline int format_to_bpp(RedPixmap::Format format)
{
    return ((format == RedPixmap::A1) ? 1 : 32);
}

RedPixmapGdi::RedPixmapGdi(int width, int height, RedPixmap::Format format, bool top_bottom,
                           rgb32_t* pallet)
    : RedPixmap(width, height, format, top_bottom, pallet)
{
    ASSERT(format == RedPixmap::ARGB32 || format == RedPixmap::RGB32 || format == RedPixmap::A1);
    ASSERT(sizeof(RedPixmap_p) <= PIXELES_SOURCE_OPAQUE_SIZE);

    struct {
        BITMAPINFO inf;
        RGBQUAD palette[255];
    } bitmap_info;

    memset(&bitmap_info, 0, sizeof(bitmap_info));
    bitmap_info.inf.bmiHeader.biSize = sizeof(bitmap_info.inf.bmiHeader);
    bitmap_info.inf.bmiHeader.biWidth = _width;
    bitmap_info.inf.bmiHeader.biHeight = top_bottom ? -_height : _height;

    bitmap_info.inf.bmiHeader.biPlanes = 1;
    bitmap_info.inf.bmiHeader.biBitCount = format_to_bpp(format);
    bitmap_info.inf.bmiHeader.biCompression = BI_RGB;
    switch (format) {
    case RedPixmap::A1:
        for (int i = 0; i < (1 << format_to_bpp(format)); i++) {
            bitmap_info.inf.bmiColors[i].rgbRed = rgb32_get_red(pallet[i]);
            bitmap_info.inf.bmiColors[i].rgbGreen = rgb32_get_green(pallet[i]);
            bitmap_info.inf.bmiColors[i].rgbBlue = rgb32_get_blue(pallet[i]);
        }
        break;
    }
    AutoDC dc(create_compatible_dc());
    AutoGDIObject bitmap(CreateDIBSection(dc.get(), &bitmap_info.inf, 0,
                                          (VOID **)&_data, NULL, 0));
    if (!bitmap.valid()) {
        THROW("create compatible bitmap failed");
    }
    memset(_data, 1, 1);
    ((RedPixmap_p*)get_opaque())->prev_bitmap = (HBITMAP)SelectObject(dc.get(), bitmap.release());
    ((RedPixmap_p*)get_opaque())->pixels_source_p.dc = dc.release();
}

HDC RedPixmapGdi::get_dc()
{
    return ((RedPixmap_p*)get_opaque())->pixels_source_p.dc;
}

void *RedPixmapGdi::get_memptr()
{
    return _data;
}

RedPixmapGdi::~RedPixmapGdi()
{
    HDC dc = ((RedPixmap_p*)get_opaque())->pixels_source_p.dc;
    if (dc) {
        HBITMAP prev_bitmap = ((RedPixmap_p*)get_opaque())->prev_bitmap;
        HBITMAP bitmap = (HBITMAP)SelectObject(dc, prev_bitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
}

RecurciveMutex& RedPixmapGdi::get_mutex()
{
    RedPixmap_p* p_data = (RedPixmap_p*)get_opaque();
    return *p_data->pixels_source_p._mutex;
}


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
#include "x_platform.h"
#include "pixels_source.h"
#include "pixels_source_p.h"
#include "utils.h"
#include "debug.h"
#include "res.h"


static void create_image(const PixmapHeader* pixmap, PixelsSource_p& pixels_source,
                         pixman_format_code_t format)
{
    pixman_image_t *pixman_image;
    XImage *image = new XImage;

    memset(image, 0, sizeof(*image));
    image->width = pixmap->width;
    image->height = pixmap->height;

    image->data = (char*)pixmap->data;
    image->byte_order = LSBFirst;
    image->bitmap_unit = 32;
    image->bitmap_bit_order = LSBFirst;
    image->bitmap_pad = 32;
    image->bytes_per_line = pixmap->stride;

    image->depth = XPlatform::get_vinfo()[0]->depth;
    image->format = ZPixmap;
    image->bits_per_pixel = 32;
    image->red_mask = 0x00ff0000;
    image->green_mask = 0x0000ff00;
    image->blue_mask = 0x000000ff;

    try {
        if (!XInitImage(image)) {
            THROW("init image failed");
        }

        pixman_image = pixman_image_create_bits(format,
                                                pixmap->width, pixmap->height,
                                                (uint32_t *)pixmap->data,
                                                pixmap->stride);
        if (pixman_image == NULL) {
            THROW("surf create failed");
        }
    } catch (...) {
        delete image;
        throw;
    }

    pixels_source.type = PIXELS_SOURCE_TYPE_PIXMAP;
    pixels_source.pixmap.x_image = image;
    pixels_source.pixmap.pixman_image = pixman_image;
}

PixelsSource::PixelsSource()
{
    _origin.x = _origin.y = 0;
    memset(_opaque, 0, sizeof(_opaque));
}

PixelsSource::~PixelsSource()
{
}

ImageFromRes::ImageFromRes(int res_id)
{
    const PixmapHeader* pixmap = res_get_image(res_id);
    if (!pixmap) {
        THROW("no image %d", res_id);
    }
    create_image(pixmap, *(PixelsSource_p*)get_opaque(), PIXMAN_x8r8g8b8);
}

ImageFromRes::~ImageFromRes()
{
    pixman_image_unref(((PixelsSource_p*)get_opaque())->pixmap.pixman_image);
    delete ((PixelsSource_p*)get_opaque())->pixmap.x_image;
}

SpicePoint ImageFromRes::get_size()
{
    XImage *image = ((PixelsSource_p*)get_opaque())->pixmap.x_image;
    SpicePoint pt;
    pt.x = image->width;
    pt.y = image->height;
    return pt;
}

AlphaImageFromRes::AlphaImageFromRes(int res_id)
{
    const PixmapHeader* pixmap = res_get_image(res_id);
    if (!pixmap) {
        THROW("no image %d", res_id);
    }
    create_image(pixmap, *(PixelsSource_p*)get_opaque(), PIXMAN_a8r8g8b8);
}

AlphaImageFromRes::~AlphaImageFromRes()
{
    pixman_image_unref(((PixelsSource_p*)get_opaque())->pixmap.pixman_image);
    delete ((PixelsSource_p*)get_opaque())->pixmap.x_image;
}

SpicePoint AlphaImageFromRes::get_size()
{
    XImage *image = ((PixelsSource_p*)get_opaque())->pixmap.x_image;
    SpicePoint pt;
    pt.x = image->width;
    pt.y = image->height;
    return pt;
}


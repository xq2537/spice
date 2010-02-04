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

#include "common.h"
#include "x_platform.h"
#include "pixels_source.h"
#include "pixels_source_p.h"
#include "utils.h"
#include "debug.h"
#include "res.h"


static void create_image(const PixmapHeader* pixmap, PixelsSource_p& pixels_source,
                         cairo_format_t cairo_format)
{
    cairo_surface_t* cairo_surf;
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

        cairo_surf = cairo_image_surface_create_for_data(pixmap->data, cairo_format,
                                                         pixmap->width, pixmap->height,
                                                         pixmap->stride);
        if (cairo_surface_status(cairo_surf) != CAIRO_STATUS_SUCCESS) {
            THROW("surf create failed");
        }
    } catch (...) {
        delete image;
        throw;
    }

    pixels_source.type = PIXELS_SOURCE_TYPE_PIXMAP;
    pixels_source.pixmap.x_image = image;
    pixels_source.pixmap.cairo_surf = cairo_surf;
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
    create_image(pixmap, *(PixelsSource_p*)get_opaque(), CAIRO_FORMAT_RGB24);
}

ImageFromRes::~ImageFromRes()
{
    cairo_surface_destroy(((PixelsSource_p*)get_opaque())->pixmap.cairo_surf);
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
    create_image(pixmap, *(PixelsSource_p*)get_opaque(), CAIRO_FORMAT_ARGB32);
}

AlphaImageFromRes::~AlphaImageFromRes()
{
    cairo_surface_destroy(((PixelsSource_p*)get_opaque())->pixmap.cairo_surf);
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


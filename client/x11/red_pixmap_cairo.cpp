/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
#include "red_pixmap_cairo.h"
#include "debug.h"
#include "utils.h"
#include "pixels_source_p.h"
#include "x_platform.h"
#include <sys/shm.h>


RedPixmapCairo::RedPixmapCairo(int width, int height, RedPixmap::Format format,
                               bool top_bottom, rgb32_t* pallet, RedWindow *win)
    : RedPixmap(width, height, format, top_bottom, pallet)
{
    ASSERT(format == RedPixmap::ARGB32 || format == RedPixmap::RGB32 || format == RedPixmap::A1);
    ASSERT(sizeof(RedDrawable_p) <= PIXELES_SOURCE_OPAQUE_SIZE);
    pixman_image_t *pixman_image;
    XImage *image = NULL;
    XShmSegmentInfo *shminfo = NULL;
    _data = NULL;
    XVisualInfo *vinfo = NULL;
    bool using_shm = false;


    try {
        pixman_format_code_t pixman_format;

        if (win) {
            vinfo = XPlatform::get_vinfo()[win->get_screen_num()];
        }

        using_shm = vinfo && XPlatform::is_x_shm_avail();

        if (using_shm) {
            int depth;

            switch (format) {
            case RedPixmap::ARGB32:
            case RedPixmap::RGB32:
                depth = XPlatform::get_vinfo()[0]->depth;
                pixman_format = format == RedPixmap::ARGB32 ? PIXMAN_a8r8g8b8 :
                                                             PIXMAN_x8r8g8b8;
                break;
            case RedPixmap::A1:
                depth = 1;
                pixman_format = PIXMAN_a1;
                break;
            default:
                THROW("unsupported format %d", format);
            }

            shminfo = new XShmSegmentInfo;
            shminfo->shmid = -1;
            shminfo->shmaddr = 0;
            ((PixelsSource_p*)get_opaque())->type = PIXELS_SOURCE_TYPE_XSHM_DRAWABLE;
            memset(shminfo, 0, sizeof(XShmSegmentInfo));
            image = XShmCreateImage(XPlatform::get_display(), vinfo->visual,
                                    depth, ZPixmap, NULL, shminfo, width, height);
            if (!image) {
                THROW("XShmCreateImage failed");
            }

            shminfo->shmid = shmget(IPC_PRIVATE, height * _stride, IPC_CREAT | 0777);
            if (shminfo->shmid < 0) {
                THROW("shmget failed");
            }
            shminfo->shmaddr = (char *)shmat(shminfo->shmid, 0, 0);
            if (shmctl(shminfo->shmid, IPC_RMID, NULL) == -1) {
                LOG_WARN("shmctl IPC_RMID failed %s (%d)", strerror(errno), errno);
            }
            if (!shminfo->shmaddr) {
                THROW("shmat failed");
            }
            shminfo->readOnly = False;
            if (!XShmAttach(XPlatform::get_display(), shminfo)) {
                THROW("XShmAttach failed");
            }

            ((PixelsSource_p*)get_opaque())->x_shm_drawable.x_image = image;
            ((PixelsSource_p*)get_opaque())->x_shm_drawable.shminfo = shminfo;
            _data = (uint8_t *)shminfo->shmaddr;
            image->data = (char *)_data;
        } else {
            image = new XImage;
            _data = new uint8_t[height * _stride];
            ((PixelsSource_p*)get_opaque())->type = PIXELS_SOURCE_TYPE_PIXMAP;
            ((PixelsSource_p*)get_opaque())->pixmap.x_image = image;
            memset(image, 0, sizeof(*image));
            image->width = _width;
            image->height = _height;

            image->data = (char*)_data;
            image->byte_order = LSBFirst;
            image->bitmap_unit = 32;
            image->bitmap_bit_order = LSBFirst;
            image->bitmap_pad = 32;

            image->bytes_per_line = _stride;
            switch (format) {
            case RedPixmap::ARGB32:
            case RedPixmap::RGB32:
                image->depth = XPlatform::get_vinfo()[0]->depth;
                image->format = ZPixmap;
                image->bits_per_pixel = 32;
                image->red_mask = 0x00ff0000;
                image->green_mask = 0x0000ff00;
                image->blue_mask = 0x000000ff;
                pixman_format = format == RedPixmap::ARGB32 ? PIXMAN_a8r8g8b8 :
                                                             PIXMAN_x8r8g8b8;
                break;
            case RedPixmap::A1:
                image->depth = 1;
                image->format = XYBitmap;
                pixman_format = PIXMAN_a1;
                break;
            default:
                THROW("unsupported format %d", format);
            }

            if (!XInitImage(image)) {
                THROW("init image failed");
            }
        }
        pixman_image = pixman_image_create_bits(pixman_format, _width, _height,
                                                (uint32_t *)_data, _stride);
        if (pixman_image == NULL) {
            THROW("surf create failed");
        }

        if (!using_shm) {
            ((PixelsSource_p*)get_opaque())->pixmap.pixman_image = pixman_image;
        } else {
            ((PixelsSource_p*)get_opaque())->x_shm_drawable.pixman_image = pixman_image;
        }
    } catch (...) {
        if (using_shm) {
            if (image) {
                XDestroyImage(image);
            }
            if (shminfo) {
                if (shminfo->shmid >= 0) {
                    shmctl(shminfo->shmid, IPC_RMID, NULL);
                }
                if (shminfo->shmaddr) {
                    shmdt(shminfo->shmaddr);
                }
            }
        } else {
            delete image;
            delete _data;
        }
        throw;
    }
}

RedPixmapCairo::~RedPixmapCairo()
{
    if (((PixelsSource_p*)get_opaque())->type == PIXELS_SOURCE_TYPE_PIXMAP) {
        pixman_image_unref(((PixelsSource_p*)get_opaque())->pixmap.pixman_image);
        delete ((PixelsSource_p*)get_opaque())->pixmap.x_image;
        delete[] _data;
    } else {
        pixman_image_unref(((PixelsSource_p*)get_opaque())->x_shm_drawable.pixman_image);
        XShmSegmentInfo *shminfo = ((PixelsSource_p*)get_opaque())->x_shm_drawable.shminfo;
        XShmDetach(XPlatform::get_display(), shminfo);
        XDestroyImage(((PixelsSource_p*)get_opaque())->x_shm_drawable.x_image);
        XSync(XPlatform::get_display(), False);
        shmdt(shminfo->shmaddr);
        delete shminfo;
    }
}


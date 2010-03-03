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

#ifndef _H__GDI_CANVAS
#define _H__GDI_CANVAS

#include <stdint.h>

#include <spice/draw.h>
#include "pixman_utils.h"
#include "canvas_base.h"
#include "region.h"

typedef struct {
    int width;
    int height;
    int stride;
    uint8_t *pixels;
} GdiImage;

SpiceCanvas *gdi_canvas_create(HDC dc, class Mutex *lock, int bits,
                               SpiceImageCache *bits_cache,
                               SpicePaletteCache *palette_cache,
                               SpiceGlzDecoder *glz_decoder);

void gdi_canvas_init();

#endif

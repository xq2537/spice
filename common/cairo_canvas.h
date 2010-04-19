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

#ifndef _H__CANVAS
#define _H__CANVAS

#include <stdint.h>

#include <spice/draw.h>
#include "pixman_utils.h"
#include "canvas_base.h"
#include "region.h"

SpiceCanvas *canvas_create(int width, int height, uint32_t format
#ifdef CAIRO_CANVAS_CACHE
                           , SpiceImageCache *bits_cache
                           , SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                           , SpiceImageCache *bits_cache
#endif
			   , SpiceImageSurfaces *surfaces
                           , SpiceGlzDecoder *glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , SpiceVirtMapping *virt_mapping
#endif
                           );

SpiceCanvas *canvas_create_for_data(int width, int height, uint32_t format, uint8_t *data, size_t stride
#ifdef CAIRO_CANVAS_CACHE
                           , SpiceImageCache *bits_cache
                           , SpicePaletteCache *palette_cache
#elif defined(CAIRO_CANVAS_IMAGE_CACHE)
                           , SpiceImageCache *bits_cache
#endif
			   , SpiceImageSurfaces *surfaces
                           , SpiceGlzDecoder *glz_decoder
#ifndef CAIRO_CANVAS_NO_CHUNKS
                           , SpiceVirtMapping *virt_mapping
#endif
                           );


void cairo_canvas_init();

#endif

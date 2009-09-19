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

#ifndef _H_CANVAS_BASE
#define _H_CANVAS_BASE


#include "cairo.h"
#include "lz.h"
#include "draw.h"

#if defined(CAIRO_CANVAS_CACHE) || defined(CAIRO_CANVAS_IMAGE_CACHE)
typedef void (*bits_cache_put_fn_t)(void *bits_cache_opaque, uint64_t id, cairo_surface_t *surface);
typedef cairo_surface_t *(*bits_cache_get_fn_t)(void *bits_cache_opaque, uint64_t id);
#endif
#ifdef CAIRO_CANVAS_CACHE
typedef void (*palette_cache_put_fn_t)(void *palette_cache_opaque, Palette *palette);
typedef Palette *(*palette_cache_get_fn_t)(void *palette_cache_opaque, uint64_t id);
typedef void (*palette_cache_release_fn_t)(Palette *palette);
#endif

typedef void (*glz_decode_fn_t)(void *glz_decoder_opaque, uint8_t *data,
                                Palette *plt, void *usr_data);

#endif


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

#ifndef _H_ROP3
#define _H_ROP3

#include <stdint.h>

#include <spice/draw.h>
#include "cairo.h"

void do_rop3_with_pattern(uint8_t rop3, cairo_surface_t *d, cairo_surface_t *s, SpicePoint *src_pos,
                          cairo_surface_t *p, SpicePoint *pat_pos);
void do_rop3_with_color(uint8_t rop3, cairo_surface_t *d, cairo_surface_t *s, SpicePoint *src_pos,
                        uint32_t rgb);

void rop3_init();
#endif


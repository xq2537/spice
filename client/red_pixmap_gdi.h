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

#ifndef _H_RED_PIXMAP_CAIRO
#define _H_RED_PIXMAP_CAIRO

#include "red_pixmap.h"

class Mutex;

class RedPixmapGdi: public RedPixmap {
public:
    RedPixmapGdi(int width, int height, Format format, bool top_bottom,
                 rgb32_t *pallete);
    HDC get_dc();
    void *get_memptr();
    ~RedPixmapGdi();
    Mutex& get_mutex();
};

#endif


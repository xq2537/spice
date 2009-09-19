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
#include "red_pixmap.h"
#include "debug.h"
#include "utils.h"

RedPixmap::RedPixmap(int width, int height, RedPixmap::Format format,
                     bool top_bottom, rgb32_t* pallet)
    : _format (format)
    , _width (width)
    , _height (height)
    , _stride (ALIGN(width * (_format == RedPixmap::A1 ? 1: 32), 32) / 8)
    , _top_bottom (top_bottom)
    , _data (NULL)
{
}

RedPixmap::~RedPixmap()
{
}

bool RedPixmap::is_big_endian_bits()
{
    return _format == RedPixmap::A1;
}


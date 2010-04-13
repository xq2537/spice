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

#ifndef _H_RED_DRAWABLE
#define _H_RED_DRAWABLE

#include "pixels_source.h"

typedef uint32_t rgb32_t;

static inline rgb32_t rgb32_make(uint8_t r, uint8_t g, uint8_t b)
{
    return (rgb32_t(r) << 16) | (rgb32_t(g) << 8) | b;
}

static inline uint8_t rgb32_get_red(rgb32_t color)
{
    return color >> 16;
}

static inline uint8_t rgb32_get_green(rgb32_t color)
{
    return color >> 8;
}

static inline uint8_t rgb32_get_blue(rgb32_t color)
{
    return color;
}

class RedDrawable: public PixelsSource {
public:
    RedDrawable() {}
    virtual ~RedDrawable() {}

    enum CombineOP {
        OP_COPY,
        OP_AND,
        OP_XOR,
    };

    void copy_pixels(const PixelsSource& src, int src_x, int src_y, const SpiceRect& dest);
    void blend_pixels(const PixelsSource& src, int src_x, int src_y, const SpiceRect& dest);
    void combine_pixels(const PixelsSource& src, int src_x, int src_y, const SpiceRect& dest,
                        CombineOP op);
    void fill_rect(const SpiceRect& rect, rgb32_t color);
    void frame_rect(const SpiceRect& rect, rgb32_t color);
    void erase_rect(const SpiceRect& rect, rgb32_t color);
};

#endif


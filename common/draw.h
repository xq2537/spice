/*
   Copyright (C) 2009 Red Hat, Inc.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in
         the documentation and/or other materials provided with the
         distribution.
       * Neither the name of the copyright holder nor the names of its
         contributors may be used to endorse or promote products derived
         from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _H_DRAW
#define _H_DRAW

#ifndef _WIN32
#include <stdint.h>
#endif

#ifdef __GNUC__
#define ATTR_PACKED __attribute__ ((__packed__))
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;

typedef int16_t INT16;
typedef int32_t INT32;
#else
#include <basetsd.h>
#pragma pack(push)
#pragma pack(1)
#define ATTR_PACKED
#pragma warning(disable:4200)
#endif

#ifdef _WIN32_WCE
#include <stdint.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;

typedef int16_t INT16;
typedef int32_t INT32;
#endif

#define GET_ADDRESS(addr) ((void *)(unsigned long)(addr))
#define SET_ADDRESS(addr, val) ((addr) = (unsigned long)(val))

typedef INT32 FIXED28_4;
typedef UINT64 ADDRESS;

enum {
    PATH_BEGIN = (1 << 0),
    PATH_END = (1 << 1),
    PATH_CLOSE = (1 << 3),
    PATH_BEZIER = (1 << 4),
};

enum {
    LINE_ATTR_STARTGAP = (1 << 2),
    LINE_ATTR_STYLED = (1 << 3),
};

typedef struct ATTR_PACKED PointFix {
    FIXED28_4 x;
    FIXED28_4 y;
} PointFix;

typedef struct ATTR_PACKED Point {
    INT32 x;
    INT32 y;
} Point;

typedef struct ATTR_PACKED Point16 {
    INT16 x;
    INT16 y;
} Point16;

typedef struct ATTR_PACKED Rect {
    INT32 top;
    INT32 left;
    INT32 bottom;
    INT32 right;
} Rect;

typedef struct ATTR_PACKED PathSeg {
    UINT32 flags;
    UINT32 count;
    UINT8 data[0];
} PathSeg;

enum ClipType {
    CLIP_TYPE_NONE,
    CLIP_TYPE_RECTS,
    CLIP_TYPE_PATH,
};

typedef struct ATTR_PACKED Clip {
    UINT32 type;
    ADDRESS data;
} Clip;

enum ROPDescriptor {
    ROPD_INVERS_SRC = (1 << 0),
    ROPD_INVERS_BRUSH = (1 << 1),
    ROPD_INVERS_DEST = (1 << 2),
    ROPD_OP_PUT = (1 << 3),
    ROPD_OP_OR = (1 << 4),
    ROPD_OP_AND = (1 << 5),
    ROPD_OP_XOR = (1 << 6),
    ROPD_OP_BLACKNESS = (1 << 7),
    ROPD_OP_WHITENESS = (1 << 8),
    ROPD_OP_INVERS = (1 << 9),
    ROPD_INVERS_RES = (1 << 10),
};

typedef struct ATTR_PACKED Pattern {
    ADDRESS pat;
    Point pos;
} Pattern;

enum {
    BRUSH_TYPE_NONE,
    BRUSH_TYPE_SOLID,
    BRUSH_TYPE_PATTERN,
};

typedef struct ATTR_PACKED Brush {
    UINT32 type;
    union {
        UINT32 color;
        Pattern pattern;
    } u;
} Brush;

enum {
    MASK_INVERS = (1 << 0),
};

typedef struct ATTR_PACKED QMask {
    UINT8 flags;
    Point pos;
    ADDRESS bitmap;
} QMask;

typedef struct ATTR_PACKED Fill {
    Brush brush;
    UINT16 rop_decriptor;
    QMask mask;
} Fill;

typedef struct ATTR_PACKED Palette {
    UINT64 unique;
    UINT16 num_ents;
    UINT32 ents[0];
} Palette;

enum {
    IMAGE_TYPE_BITMAP,
    IMAGE_TYPE_QUIC,
    IMAGE_TYPE_RESERVED,
    IMAGE_TYPE_LZ_PLT = 100,
    IMAGE_TYPE_LZ_RGB,
    IMAGE_TYPE_GLZ_RGB,
    IMAGE_TYPE_FROM_CACHE,
};

enum {
    IMAGE_CACHE_ME = (1 << 0),
};

typedef struct ATTR_PACKED ImageDescriptor {
    UINT64 id;
    UINT8 type;
    UINT8 flags;
    UINT32 width;
    UINT32 height;
} ImageDescriptor;

enum {
    BITMAP_FMT_INVALID,
    BITMAP_FMT_1BIT_LE,
    BITMAP_FMT_1BIT_BE,
    BITMAP_FMT_4BIT_LE,
    BITMAP_FMT_4BIT_BE,
    BITMAP_FMT_8BIT,
    BITMAP_FMT_16BIT,
    BITMAP_FMT_24BIT,
    BITMAP_FMT_32BIT,
    BITMAP_FMT_RGBA,
};

enum {
    BITMAP_PAL_CACHE_ME = (1 << 0),
    BITMAP_PAL_FROM_CACHE = (1 << 1),
    BITMAP_TOP_DOWN = (1 << 2),
};

typedef struct ATTR_PACKED Bitmap {
    UINT8 format;
    UINT8 flags;
    UINT32 x;
    UINT32 y;
    UINT32 stride;
    ADDRESS palette;
    ADDRESS data; //data[0] ?
} Bitmap;

typedef struct ATTR_PACKED BitmapImage {
    ImageDescriptor descriptor;
    Bitmap bitmap;
} BitmapImage;

typedef struct ATTR_PACKED QUICData {
    UINT32 data_size;
    UINT8 data[0];
} QUICData, LZ_RGBData;

typedef struct ATTR_PACKED QUICImage {
    ImageDescriptor descriptor;
    QUICData quic;
} QUICImage;

typedef struct ATTR_PACKED LZ_RGBImage {
    ImageDescriptor descriptor;
    LZ_RGBData lz_rgb;
} LZ_RGBImage;

typedef struct ATTR_PACKED LZ_PLTData {
    UINT8 flags;
    UINT32 data_size;
    ADDRESS palette;
    UINT8 data[0];
} LZ_PLTData;

typedef struct ATTR_PACKED LZ_PLTImage {
    ImageDescriptor descriptor;
    LZ_PLTData lz_plt;
} LZ_PLTImage;

enum {
    IMAGE_SCALE_INTERPOLATE,
    IMAGE_SCALE_NEAREST,
};

typedef struct ATTR_PACKED Opaque {
    ADDRESS src_bitmap;
    Rect src_area;
    Brush brush;
    UINT16 rop_decriptor;
    UINT8 scale_mode;
    QMask mask;
} Opaque;

typedef struct ATTR_PACKED Copy {
    ADDRESS src_bitmap;
    Rect src_area;
    UINT16 rop_decriptor;
    UINT8 scale_mode;
    QMask mask;
} Copy, Blend;

typedef struct ATTR_PACKED Transparent {
    ADDRESS src_bitmap;
    Rect src_area;
    UINT32 src_color;
    UINT32 true_color;
} Transparent;

typedef struct ATTR_PACKED AlphaBlnd {
    UINT8 alpha;
    ADDRESS src_bitmap;
    Rect src_area;
} AlphaBlnd;

typedef struct ATTR_PACKED Rop3 {
    ADDRESS src_bitmap;
    Rect src_area;
    Brush brush;
    UINT8 rop3;
    UINT8 scale_mode;
    QMask mask;
} Rop3;

typedef struct ATTR_PACKED Blackness {
    QMask mask;
} Blackness, Invers, Whiteness;

enum {
    LINE_STYLED = (1 << 3),
    LINE_START_WITH_GAP = (1 << 2),
};

enum {
    LINE_CAP_ROUND,
    LINE_CAP_SQUARE,
    LINE_CAP_BUTT,
};

enum {
    LINE_JOIN_ROUND,
    LINE_JOIN_BEVEL,
    LINE_JOIN_MITER,
};

typedef struct ATTR_PACKED LineAttr {
    UINT8 flags;
    UINT8 join_style;
    UINT8 end_style;
    UINT8 style_nseg;
    FIXED28_4 width;
    FIXED28_4 miter_limit;
    ADDRESS style; //data[0] ?
} LineAttr;

typedef struct ATTR_PACKED Stroke {
    ADDRESS path;
    LineAttr attr;
    Brush brush;
    UINT16 fore_mode;
    UINT16 back_mode;
} Stroke;

typedef struct ATTR_PACKED RasterGlyph {
    Point render_pos;
    Point glyph_origin;
    UINT16 width;
    UINT16 height;
    UINT8 data[0];
} RasterGlyph;

typedef struct ATTR_PACKED VectotGlyph {
    Point render_pos;
    UINT32 data_size;
    UINT8 data[0]; //PathSeg[]
} VectotGlyph;

enum {
    STRING_RASTER_A1 = 1 << 0,
    STRING_RASTER_A4 = 1 << 1,
    STRING_RASTER_A8 = 1 << 2,
    STRING_RASTER_TOP_DOWN = 1 << 3,
};

typedef struct ATTR_PACKED String {
    UINT16 length;
    UINT16 flags;
    UINT8 data[0];
} String;

typedef struct ATTR_PACKED Text {
    ADDRESS str;
    Rect back_area;
    Brush fore_brush;
    Brush back_brush;
    UINT16 fore_mode;
    UINT16 back_mode;
} Text;

enum {
    CURSOR_TYPE_ALPHA,
    CURSOR_TYPE_MONO,
    CURSOR_TYPE_COLOR4,
    CURSOR_TYPE_COLOR8,
    CURSOR_TYPE_COLOR16,
    CURSOR_TYPE_COLOR24,
    CURSOR_TYPE_COLOR32,
};

typedef struct ATTR_PACKED CursorHeader {
    UINT64 unique;
    UINT16 type;
    UINT16 width;
    UINT16 height;
    UINT16 hot_spot_x;
    UINT16 hot_spot_y;
} CursorHeader;


#ifndef __GNUC__
#pragma pack(pop)
#endif

#undef ATTR_PACKED

#endif

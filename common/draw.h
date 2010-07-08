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

#ifndef _H_SPICE_DRAW
#define _H_SPICE_DRAW

#include <spice/types.h>
#include <spice/enums.h>
#include <common/mem.h>

#include <spice/start-packed.h>

#define SPICE_GET_ADDRESS(addr) ((void *)(unsigned long)(addr))
#define SPICE_SET_ADDRESS(addr, val) ((addr) = (unsigned long)(val))

typedef int32_t SPICE_FIXED28_4;
typedef uint64_t SPICE_ADDRESS;

typedef struct SPICE_ATTR_PACKED SpicePointFix {
    SPICE_FIXED28_4 x;
    SPICE_FIXED28_4 y;
} SpicePointFix;

typedef struct SPICE_ATTR_PACKED SpicePoint {
    int32_t x;
    int32_t y;
} SpicePoint;

typedef struct SPICE_ATTR_PACKED SpicePoint16 {
    int16_t x;
    int16_t y;
} SpicePoint16;

typedef struct SPICE_ATTR_PACKED SpiceRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} SpiceRect;

typedef struct SPICE_ATTR_PACKED SpicePathSeg {
    uint32_t flags;
    uint32_t count;
    SpicePointFix points[0];
} SpicePathSeg;

typedef struct SPICE_ATTR_PACKED SpicePath {
  uint32_t num_segments;
  SpicePathSeg *segments[0];
} SpicePath;

typedef struct SPICE_ATTR_PACKED SpiceClipRects {
  uint32_t num_rects;
  SpiceRect rects[0];
} SpiceClipRects;

typedef struct SPICE_ATTR_PACKED SpiceClip {
    uint32_t type;
    SpiceClipRects *rects;
} SpiceClip;

typedef struct SPICE_ATTR_PACKED SpicePalette {
    uint64_t unique;
    uint16_t num_ents;
    uint32_t ents[0];
} SpicePalette;

#define SPICE_SURFACE_FMT_DEPTH(_d) ((_d) & 0x3f)

typedef struct SPICE_ATTR_PACKED SpiceImageDescriptor {
    uint64_t id;
    uint8_t type;
    uint8_t flags;
    uint32_t width;
    uint32_t height;
} SpiceImageDescriptor;

typedef struct SPICE_ATTR_PACKED SpiceBitmap {
    uint8_t format;
    uint8_t flags;
    uint32_t x;
    uint32_t y;
    uint32_t stride;
    SpicePalette *palette;
    uint64_t palette_id;
    SpiceChunks *data;
} SpiceBitmap;

typedef struct SPICE_ATTR_PACKED SpiceSurface {
    uint32_t surface_id;
} SpiceSurface;

typedef struct SPICE_ATTR_PACKED SpiceQUICData {
    uint32_t data_size;
    SpiceChunks *data;
} SpiceQUICData, SpiceLZRGBData, SpiceJPEGData;

typedef struct SPICE_ATTR_PACKED SpiceLZPLTData {
    uint8_t flags;
    uint32_t data_size;
    SpicePalette *palette;
    uint64_t palette_id;
    SpiceChunks *data;
} SpiceLZPLTData;

typedef struct SPICE_ATTR_PACKED SpiceZlibGlzRGBData {
    uint32_t glz_data_size;
    uint32_t data_size;
    SpiceChunks *data;
} SpiceZlibGlzRGBData;

typedef struct SPICE_ATTR_PACKED SpiceJPEGAlphaData {
    uint8_t flags;
    uint32_t jpeg_size;
    uint32_t data_size;
    SpiceChunks *data;
} SpiceJPEGAlphaData;


typedef struct SPICE_ATTR_PACKED SpiceImage {
    SpiceImageDescriptor descriptor;
    union {
        SpiceBitmap         bitmap;
        SpiceQUICData       quic;
        SpiceSurface        surface;
        SpiceLZRGBData      lz_rgb;
        SpiceLZPLTData      lz_plt;
        SpiceJPEGData       jpeg;
        SpiceZlibGlzRGBData zlib_glz;
        SpiceJPEGAlphaData  jpeg_alpha;
    } u;
} SpiceImage;

typedef struct SPICE_ATTR_PACKED SpicePattern {
    SpiceImage *pat;
    SpicePoint pos;
} SpicePattern;

typedef struct SPICE_ATTR_PACKED SpiceBrush {
    uint32_t type;
    union {
        uint32_t color;
        SpicePattern pattern;
    } u;
} SpiceBrush;

typedef struct SPICE_ATTR_PACKED SpiceQMask {
    uint8_t flags;
    SpicePoint pos;
    SpiceImage *bitmap;
} SpiceQMask;

typedef struct SPICE_ATTR_PACKED SpiceFill {
    SpiceBrush brush;
    uint16_t rop_descriptor;
    SpiceQMask mask;
} SpiceFill;

typedef struct SPICE_ATTR_PACKED SpiceOpaque {
    SpiceImage *src_bitmap;
    SpiceRect src_area;
    SpiceBrush brush;
    uint16_t rop_descriptor;
    uint8_t scale_mode;
    SpiceQMask mask;
} SpiceOpaque;

typedef struct SPICE_ATTR_PACKED SpiceCopy {
    SpiceImage *src_bitmap;
    SpiceRect src_area;
    uint16_t rop_descriptor;
    uint8_t scale_mode;
    SpiceQMask mask;
} SpiceCopy, SpiceBlend;

typedef struct SPICE_ATTR_PACKED SpiceTransparent {
    SpiceImage *src_bitmap;
    SpiceRect src_area;
    uint32_t src_color;
    uint32_t true_color;
} SpiceTransparent;

typedef struct SPICE_ATTR_PACKED SpiceAlphaBlnd {
    uint16_t alpha_flags;
    uint8_t alpha;
    SpiceImage *src_bitmap;
    SpiceRect src_area;
} SpiceAlphaBlnd;

typedef struct SPICE_ATTR_PACKED SpiceRop3 {
    SpiceImage *src_bitmap;
    SpiceRect src_area;
    SpiceBrush brush;
    uint8_t rop3;
    uint8_t scale_mode;
    SpiceQMask mask;
} SpiceRop3;

typedef struct SPICE_ATTR_PACKED SpiceBlackness {
    SpiceQMask mask;
} SpiceBlackness, SpiceInvers, SpiceWhiteness;

typedef struct SPICE_ATTR_PACKED SpiceLineAttr {
    uint8_t flags;
    uint8_t style_nseg;
    SPICE_FIXED28_4 *style;
} SpiceLineAttr;

typedef struct SPICE_ATTR_PACKED SpiceStroke {
    SpicePath *path;
    SpiceLineAttr attr;
    SpiceBrush brush;
    uint16_t fore_mode;
    uint16_t back_mode;
} SpiceStroke;

typedef struct SPICE_ATTR_PACKED SpiceRasterGlyph {
    SpicePoint render_pos;
    SpicePoint glyph_origin;
    uint16_t width;
    uint16_t height;
    uint8_t data[0];
} SpiceRasterGlyph;

typedef struct SPICE_ATTR_PACKED SpiceString {
    uint16_t length;
    uint16_t flags;
    SpiceRasterGlyph *glyphs[0];
} SpiceString;

typedef struct SPICE_ATTR_PACKED SpiceText {
    SpiceString *str;
    SpiceRect back_area;
    SpiceBrush fore_brush;
    SpiceBrush back_brush;
    uint16_t fore_mode;
    uint16_t back_mode;
} SpiceText;

typedef struct SPICE_ATTR_PACKED SpiceCursorHeader {
    uint64_t unique;
    uint16_t type;
    uint16_t width;
    uint16_t height;
    uint16_t hot_spot_x;
    uint16_t hot_spot_y;
} SpiceCursorHeader;

#include <spice/end-packed.h>

#endif /* _H_SPICE_DRAW */

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

#ifndef _H_CANVAS
#define _H_CANVAS

#include "common.h"
#include "debug.h"
#include "cairo.h"
#include "red.h"
#include "cache.hpp"
#include "shared_cache.hpp"
#include "canvas_utils.h"
#include "glz_decoded_image.h"
#include "glz_decoder.h"

enum CanvasType {
    CANVAS_TYPE_INVALID,
    CANVAS_TYPE_CAIRO,
    CANVAS_TYPE_GL,
    CANVAS_TYPE_GDI,
};

struct QRegion;

class PixmapCacheTreat {
public:
    static inline cairo_surface_t *get(cairo_surface_t *surf)
    {
        return cairo_surface_reference(surf);
    }

    static inline void release(cairo_surface_t *surf)
    {
        cairo_surface_destroy(surf);
    }

    static const char* name() { return "pixmap";}
};

typedef SharedCache<cairo_surface_t, PixmapCacheTreat, 1024> PixmapCache;

class CachedPalette {
public:
    CachedPalette(SpicePalette* palette)
        : _refs(1)
    {
        int size = sizeof(SpicePalette) + palette->num_ents * sizeof(uint32_t);
        CachedPalette **ptr = (CachedPalette **)new uint8_t[size + sizeof(CachedPalette *)];
        *ptr = this;
        _palette = (SpicePalette*)(ptr + 1);
        memcpy(_palette, palette, size);
    }

    CachedPalette* ref()
    {
        _refs++;
        return this;
    }

    void unref()
    {
        if (--_refs == 0) {
            delete this;
        }
    }

    static void unref(SpicePalette *pal)
    {
        CachedPalette **ptr = (CachedPalette **)pal;
        (*(ptr - 1))->unref();
    }

    SpicePalette* palette() { return _palette;}

private:
    ~CachedPalette()
    {
        delete[] (uint8_t *)((CachedPalette **)_palette - 1);
    }

private:
    int _refs;
    SpicePalette* _palette;
};

class PaletteCacheTreat {
public:
    static inline CachedPalette* get(CachedPalette* palette)
    {
        return palette->ref();
    }

    static inline void release(CachedPalette* palette)
    {
        palette->unref();
    }

    static const char* name() { return "palette";}
};

typedef Cache<CachedPalette, PaletteCacheTreat, 1024> PaletteCache;

/* Lz decoder related classes */

class GlzDecodedSurface: public GlzDecodedImage {
public:
    GlzDecodedSurface(uint64_t id, uint64_t win_head_id, uint8_t *data, int size,
                      int bytes_per_pixel, cairo_surface_t *surface)
        : GlzDecodedImage(id, win_head_id, data, size, bytes_per_pixel)
        , _surface (surface)
    {
        cairo_surface_reference(_surface);
    }

    virtual ~GlzDecodedSurface()
    {
        cairo_surface_destroy(_surface);
    }

private:
    cairo_surface_t *_surface;
};

class GlzDecodeSurfaceHandler: public GlzDecodeHandler {
public:
    virtual GlzDecodedImage *alloc_image(void *opaque_usr_info, uint64_t image_id,
                                         uint64_t image_win_head_id, LzImageType type,
                                         int width, int height, int gross_pixels,
                                         int n_bytes_per_pixel, bool top_down)
    {
        cairo_surface_t *surface = alloc_lz_image_surface((LzDecodeUsrData *)opaque_usr_info,
                                                          type, width, height, gross_pixels,
                                                          top_down);
        uint8_t *data = cairo_image_surface_get_data(surface);
        if (!top_down) {
            data = data - (gross_pixels / height) * n_bytes_per_pixel * (height - 1);
        }

        return (new GlzDecodedSurface(image_id, image_win_head_id, data,
                                      gross_pixels, n_bytes_per_pixel, surface));
    }
};

/* TODO: unite with the window debug callbacks? */
class GlzDecoderCanvasDebug: public GlzDecoderDebug {
public:
    virtual void error(const std::string& str)
    {
        throw Exception(str);
    }

    virtual void warn(const std::string& str)
    {
        LOG_WARN("%s", str.c_str());
    }

    virtual void info(const std::string& str)
    {
        LOG_INFO("%s", str.c_str());
    }
};

class Canvas {
public:
    Canvas(PixmapCache& bits_cache, PaletteCache& palette_cache,
           GlzDecoderWindow &glz_decoder_window);
    virtual ~Canvas();

    virtual void copy_pixels(const QRegion& region, RedDrawable* dc,
                             const PixmapHeader* pixmap) = 0;
    virtual void copy_pixels(const QRegion& region, RedDrawable& dc) = 0;
    virtual void thread_touch() = 0;

    virtual void clear() = 0;

    void draw_fill(SpiceMsgDisplayDrawFill& fill, int size);
    void draw_text(SpiceMsgDisplayDrawText& text, int size);
    void draw_opaque(SpiceMsgDisplayDrawOpaque& opaque, int size);
    void draw_copy(SpiceMsgDisplayDrawCopy& copy, int size);
    void draw_transparent(SpiceMsgDisplayDrawTransparent& transparent, int size);
    void draw_alpha_blend(SpiceMsgDisplayDrawAlphaBlend& alpha_blend, int size);
    void copy_bits(SpiceMsgDisplayCopyBits& copy_bits, int size);
    void draw_blend(SpiceMsgDisplayDrawBlend& blend, int size);
    void draw_blackness(SpiceMsgDisplayDrawBlackness& blackness, int size);
    void draw_whiteness(SpiceMsgDisplayDrawWhiteness& whiteness, int size);
    void draw_invers(SpiceMsgDisplayDrawInvers& invers, int size);
    void draw_rop3(SpiceMsgDisplayDrawRop3& rop3, int size);
    void draw_stroke(SpiceMsgDisplayDrawStroke& stroke, int size);

#ifdef WIN32
    virtual void put_image(HDC dc, const PixmapHeader& image,
                           const SpiceRect& dest, const QRegion* clip) = 0;
#else
    virtual void put_image(const PixmapHeader& image, const SpiceRect& dest,
                           const QRegion* clip) = 0;
#endif

    virtual CanvasType get_pixmap_type() { return CANVAS_TYPE_INVALID; }

protected:
    virtual void set_access_params(unsigned long base, unsigned long max) = 0;
    virtual void draw_fill(SpiceRect *bbox, SpiceClip *clip, SpiceFill *fill) = 0;
    virtual void draw_copy(SpiceRect *bbox, SpiceClip *clip, SpiceCopy *copy) = 0;
    virtual void draw_opaque(SpiceRect *bbox, SpiceClip *clip, SpiceOpaque *opaque) = 0;
    virtual void copy_bits(SpiceRect *bbox, SpiceClip *clip, SpicePoint *src_pos) = 0;
    virtual void draw_text(SpiceRect *bbox, SpiceClip *clip, SpiceText *text) = 0;
    virtual void draw_stroke(SpiceRect *bbox, SpiceClip *clip, SpiceStroke *stroke) = 0;
    virtual void draw_rop3(SpiceRect *bbox, SpiceClip *clip, SpiceRop3 *rop3) = 0;
    virtual void draw_blend(SpiceRect *bbox, SpiceClip *clip, SpiceBlend *blend) = 0;
    virtual void draw_blackness(SpiceRect *bbox, SpiceClip *clip, SpiceBlackness *blackness) = 0;
    virtual void draw_whiteness(SpiceRect *bbox, SpiceClip *clip, SpiceWhiteness *whiteness) = 0;
    virtual void draw_invers(SpiceRect *bbox, SpiceClip *clip, SpiceInvers *invers) = 0;
    virtual void draw_transparent(SpiceRect *bbox, SpiceClip *clip, SpiceTransparent* transparent) = 0;
    virtual void draw_alpha_blend(SpiceRect *bbox, SpiceClip *clip, SpiceAlphaBlnd* alpha_blend) = 0;

    PixmapCache& pixmap_cache() { return _pixmap_cache;}
    PaletteCache& palette_cache() { return _palette_cache;}
    static void bits_cache_put(void *opaque, uint64_t id, cairo_surface_t *surface);
    static cairo_surface_t* bits_cache_get(void *opaque, uint64_t id);
    static void palette_cache_put(void *opaque, SpicePalette *palette);
    static SpicePalette* palette_cache_get(void *opaque, uint64_t id);
    static void palette_cache_release(SpicePalette* palette);

    GlzDecoder& glz_decoder() {return _glz_decoder;}
    static void glz_decode(void *opaque, uint8_t *data, SpicePalette *plt, void *usr_data);

private:
    void access_test(void* ptr, size_t size);
    void localalize_ptr(SPICE_ADDRESS* data);
    void localalize_image(SPICE_ADDRESS* in_bitmap);
    void localalize_brush(SpiceBrush& brush);
    void localalize_attr(SpiceLineAttr& attr);
    void localalize_mask(SpiceQMask& mask);
    void begin_draw(SpiceMsgDisplayBase& base, int size, size_t min_size);

private:
    PixmapCache& _pixmap_cache;
    PaletteCache& _palette_cache;

    GlzDecodeSurfaceHandler _glz_handler;
    GlzDecoderCanvasDebug _glz_debug;
    GlzDecoder _glz_decoder;

    unsigned long _base;
    unsigned long _max;
};


#endif


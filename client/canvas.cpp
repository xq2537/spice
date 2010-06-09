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

#include "common.h"
#include "canvas.h"
#include "utils.h"
#include "debug.h"


Canvas::Canvas(PixmapCache& pixmap_cache, PaletteCache& palette_cache,
               GlzDecoderWindow &glz_decoder_window, CSurfaces &csurfaces)
    : _canvas (NULL)
    , _pixmap_cache (pixmap_cache)
    , _palette_cache (palette_cache)
    , _glz_decoder(glz_decoder_window, _glz_handler, _glz_debug)
    , _csurfaces(csurfaces)
{
}

Canvas::~Canvas()
{
    /* _canvas is both set and destroyed by derived class */
}

void Canvas::clear()
{
    if (_canvas) {
        _canvas->ops->clear(_canvas);
    }
}


inline void Canvas::access_test(void *ptr, size_t size)
{
    if ((unsigned long)ptr < _base || (unsigned long)ptr + size > _max) {
        THROW("access violation %p %lu", ptr, size);
    }
}

void Canvas::localalize_ptr(SPICE_ADDRESS* data)
{
    if (*data) {
        *data += _base;
    }
}

void Canvas::localalize_image(SPICE_ADDRESS* in_bitmap)
{
    SpiceImageDescriptor* image;

    ASSERT(*in_bitmap);
    localalize_ptr(in_bitmap);
    image = (SpiceImageDescriptor*)SPICE_GET_ADDRESS(*in_bitmap);
    switch (image->type) {
    case SPICE_IMAGE_TYPE_SURFACE:
        break;
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmapImage *bitmap = (SpiceBitmapImage *)image;
        localalize_ptr(&bitmap->bitmap.data);
        if (bitmap->bitmap.palette && !(bitmap->bitmap.flags & SPICE_BITMAP_FLAGS_PAL_FROM_CACHE)) {
            localalize_ptr(&bitmap->bitmap.palette);
        }
        break;
    }
    case SPICE_IMAGE_TYPE_LZ_PLT: {
        SpiceLZPLTImage *lzImage = (SpiceLZPLTImage *)image;
        ASSERT(lzImage->lz_plt.palette);
        if (!(lzImage->lz_plt.flags & SPICE_BITMAP_FLAGS_PAL_FROM_CACHE)) {
            localalize_ptr(&lzImage->lz_plt.palette);
        }
        break;
    }
    case SPICE_IMAGE_TYPE_LZ_RGB:
    case SPICE_IMAGE_TYPE_GLZ_RGB:
    case SPICE_IMAGE_TYPE_QUIC:
    case SPICE_IMAGE_TYPE_JPEG:
        break;
    case SPICE_IMAGE_TYPE_FROM_CACHE:
        break;
    default:
        THROW("invalid image type %u", image->type);
    }
}

void Canvas::localalize_brush(SpiceBrush& brush)
{
    if (brush.type == SPICE_BRUSH_TYPE_PATTERN) {
        localalize_image(&brush.u.pattern.pat);
    }
}

void Canvas::localalize_attr(SpiceLineAttr& attr)
{
    if (attr.style_nseg) {
        localalize_ptr(&attr.style);
    }
}

void Canvas::localalize_mask(SpiceQMask& mask)
{
    if (mask.bitmap) {
        localalize_image(&mask.bitmap);
    }
}

void Canvas::begin_draw(SpiceMsgDisplayBase& base, int size, size_t min_size)
{
    _base = (unsigned long)&base;
    _max = _base + size;
    _canvas->ops->set_access_params(_canvas, _base, _max);
    access_test(&base, min_size);
    localalize_ptr(&base.clip.data);
}

void Canvas::draw_fill(SpiceMsgDisplayDrawFill& fill, int size)
{
    begin_draw(fill.base, size, sizeof(SpiceMsgDisplayDrawFill));
    localalize_brush(fill.data.brush);
    localalize_mask(fill.data.mask);
    _canvas->ops->draw_fill(_canvas, &fill.base.box, &fill.base.clip, &fill.data);
    touched_bbox(&fill.base.box);
}

void Canvas::draw_text(SpiceMsgDisplayDrawText& text, int size)
{
    begin_draw(text.base, size, sizeof(SpiceMsgDisplayDrawText));
    localalize_brush(text.data.fore_brush);
    localalize_brush(text.data.back_brush);
    localalize_ptr(&text.data.str);
    _canvas->ops->draw_text(_canvas, &text.base.box, &text.base.clip, &text.data);
    touched_bbox(&text.base.box);
}

void Canvas::draw_opaque(SpiceMsgDisplayDrawOpaque& opaque, int size)
{
    begin_draw(opaque.base, size, sizeof(SpiceMsgDisplayDrawOpaque));
    localalize_brush(opaque.data.brush);
    localalize_image(&opaque.data.src_bitmap);
    localalize_mask(opaque.data.mask);
    _canvas->ops->draw_opaque(_canvas, &opaque.base.box, &opaque.base.clip, &opaque.data);
    touched_bbox(&opaque.base.box);
}

void Canvas::draw_copy(SpiceMsgDisplayDrawCopy& copy, int size)
{
    begin_draw(copy.base, size, sizeof(SpiceMsgDisplayDrawCopy));
    localalize_image(&copy.data.src_bitmap);
    localalize_mask(copy.data.mask);
    _canvas->ops->draw_copy(_canvas, &copy.base.box, &copy.base.clip, &copy.data);
    touched_bbox(&copy.base.box);
}

void Canvas::draw_transparent(SpiceMsgDisplayDrawTransparent& transparent, int size)
{
    begin_draw(transparent.base, size, sizeof(SpiceMsgDisplayDrawTransparent));
    localalize_image(&transparent.data.src_bitmap);
    _canvas->ops->draw_transparent(_canvas, &transparent.base.box, &transparent.base.clip, &transparent.data);
    touched_bbox(&transparent.base.box);
}

void Canvas::draw_alpha_blend(SpiceMsgDisplayDrawAlphaBlend& alpha_blend, int size)
{
    begin_draw(alpha_blend.base, size, sizeof(SpiceMsgDisplayDrawAlphaBlend));
    localalize_image(&alpha_blend.data.src_bitmap);
    _canvas->ops->draw_alpha_blend(_canvas, &alpha_blend.base.box, &alpha_blend.base.clip, &alpha_blend.data);
    touched_bbox(&alpha_blend.base.box);
}

void Canvas::copy_bits(SpiceMsgDisplayCopyBits& copy, int size)
{
    begin_draw(copy.base, size, sizeof(SpiceMsgDisplayCopyBits));
    _canvas->ops->copy_bits(_canvas, &copy.base.box, &copy.base.clip, &copy.src_pos);
    touched_bbox(&copy.base.box);
}

void Canvas::draw_blend(SpiceMsgDisplayDrawBlend& blend, int size)
{
    begin_draw(blend.base, size, sizeof(SpiceMsgDisplayDrawBlend));
    localalize_image(&blend.data.src_bitmap);
    localalize_mask(blend.data.mask);
    _canvas->ops->draw_blend(_canvas, &blend.base.box, &blend.base.clip, &blend.data);
    touched_bbox(&blend.base.box);
}

void Canvas::draw_blackness(SpiceMsgDisplayDrawBlackness& blackness, int size)
{
    begin_draw(blackness.base, size, sizeof(SpiceMsgDisplayDrawBlackness));
    localalize_mask(blackness.data.mask);
    _canvas->ops->draw_blackness(_canvas, &blackness.base.box, &blackness.base.clip, &blackness.data);
    touched_bbox(&blackness.base.box);
}

void Canvas::draw_whiteness(SpiceMsgDisplayDrawWhiteness& whiteness, int size)
{
    begin_draw(whiteness.base, size, sizeof(SpiceMsgDisplayDrawWhiteness));
    localalize_mask(whiteness.data.mask);
    _canvas->ops->draw_whiteness(_canvas, &whiteness.base.box, &whiteness.base.clip, &whiteness.data);
    touched_bbox(&whiteness.base.box);
}

void Canvas::draw_invers(SpiceMsgDisplayDrawInvers& invers, int size)
{
    begin_draw(invers.base, size, sizeof(SpiceMsgDisplayDrawInvers));
    localalize_mask(invers.data.mask);
    _canvas->ops->draw_invers(_canvas, &invers.base.box, &invers.base.clip, &invers.data);
    touched_bbox(&invers.base.box);
}

void Canvas::draw_rop3(SpiceMsgDisplayDrawRop3& rop3, int size)
{
    begin_draw(rop3.base, size, sizeof(SpiceMsgDisplayDrawRop3));
    localalize_brush(rop3.data.brush);
    localalize_image(&rop3.data.src_bitmap);
    localalize_mask(rop3.data.mask);
    _canvas->ops->draw_rop3(_canvas, &rop3.base.box, &rop3.base.clip, &rop3.data);
    touched_bbox(&rop3.base.box);
}

void Canvas::draw_stroke(SpiceMsgDisplayDrawStroke& stroke, int size)
{
    begin_draw(stroke.base, size, sizeof(SpiceMsgDisplayDrawStroke));
    localalize_brush(stroke.data.brush);
    localalize_ptr(&stroke.data.path);
    localalize_attr(stroke.data.attr);
    _canvas->ops->draw_stroke(_canvas, &stroke.base.box, &stroke.base.clip, &stroke.data);
    touched_bbox(&stroke.base.box);
}

void Canvas::put_image(
#ifdef WIN32
                        HDC dc,
#endif
                        const PixmapHeader& image, const SpiceRect& dest, const QRegion* clip)
{
    _canvas->ops->put_image(_canvas,
#ifdef WIN32
                            dc,
#endif
                            &dest, image.data, image.width, image.height, image.stride,
                            clip);
    touched_bbox(&dest);
}

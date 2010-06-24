/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

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

#include "red_common.h"
#include "red_memslots.h"
#include "red_parse_qxl.h"

static void red_get_brush_ptr(RedMemSlotInfo *slots, int group_id,
                              SpiceBrush *red, QXLBrush *qxl)
{
    red->type = qxl->type;
    switch (red->type) {
    case SPICE_BRUSH_TYPE_SOLID:
        red->u.color = qxl->u.color;
        break;
    case SPICE_BRUSH_TYPE_PATTERN:
        red->u.pattern.pat = qxl->u.pattern.pat;
        red->u.pattern.pos = qxl->u.pattern.pos;
        break;
    }
}

static void red_get_qmask_ptr(RedMemSlotInfo *slots, int group_id,
                              SpiceQMask *red, QXLQMask *qxl)
{
    red->flags  = qxl->flags;
    red->pos    = qxl->pos;
    red->bitmap = qxl->bitmap;
}

static void red_get_fill_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceFill *red, QXLFill *qxl)
{
    red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush);
    red->rop_descriptor = qxl->rop_descriptor;
    red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_opaque_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceOpaque *red, QXLOpaque *qxl)
{
   red->src_bitmap     = qxl->src_bitmap;
   red->src_area       = qxl->src_area;
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush);
   red->rop_descriptor = qxl->rop_descriptor;
   red->scale_mode     = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_copy_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceCopy *red, QXLCopy *qxl)
{
   red->src_bitmap      = qxl->src_bitmap;
   red->src_area        = qxl->src_area;
   red->rop_descriptor  = qxl->rop_descriptor;
   red->scale_mode      = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_blend_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceBlend *red, QXLBlend *qxl)
{
   red->src_bitmap      = qxl->src_bitmap;
   red->src_area        = qxl->src_area;
   red->rop_descriptor  = qxl->rop_descriptor;
   red->scale_mode      = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_transparent_ptr(RedMemSlotInfo *slots, int group_id,
                                    SpiceTransparent *red, QXLTransparent *qxl)
{
   red->src_bitmap      = qxl->src_bitmap;
   red->src_area        = qxl->src_area;
   red->src_color       = qxl->src_color;
   red->true_color      = qxl->true_color;
}

static void red_get_alpha_blend_ptr(RedMemSlotInfo *slots, int group_id,
                                    SpiceAlphaBlnd *red, QXLAlphaBlnd *qxl)
{
    red->alpha_flags = qxl->alpha_flags;
    red->alpha       = qxl->alpha;
    red->src_bitmap  = qxl->src_bitmap;
    red->src_area    = qxl->src_area;
}

static void red_get_alpha_blend_ptr_compat(RedMemSlotInfo *slots, int group_id,
                                           SpiceAlphaBlnd *red, QXLCompatAlphaBlnd *qxl)
{
    red->alpha       = qxl->alpha;
    red->src_bitmap  = qxl->src_bitmap;
    red->src_area    = qxl->src_area;
}

static void red_get_rop3_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceRop3 *red, QXLRop3 *qxl)
{
   red->src_bitmap = qxl->src_bitmap;
   red->src_area   = qxl->src_area;
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush);
   red->rop3       = qxl->rop3;
   red->scale_mode = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_stroke_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceStroke *red, QXLStroke *qxl)
{
   red->path             = qxl->path;
   red->attr.flags       = qxl->attr.flags;
   red->attr.join_style  = qxl->attr.join_style;
   red->attr.end_style   = qxl->attr.end_style;
   red->attr.style_nseg  = qxl->attr.style_nseg;
   red->attr.width       = qxl->attr.width;
   red->attr.miter_limit = qxl->attr.miter_limit;
   red->attr.style       = qxl->attr.style;
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush);
   red->fore_mode        = qxl->fore_mode;
   red->back_mode        = qxl->back_mode;
}

static void red_get_text_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceText *red, QXLText *qxl)
{
   red->str        = qxl->str;
   red->back_area  = qxl->back_area;
   red_get_brush_ptr(slots, group_id, &red->fore_brush, &qxl->fore_brush);
   red_get_brush_ptr(slots, group_id, &red->back_brush, &qxl->back_brush);
   red->fore_mode  = qxl->fore_mode;
   red->back_mode  = qxl->back_mode;
}

static void red_get_whiteness_ptr(RedMemSlotInfo *slots, int group_id,
                                  SpiceWhiteness *red, QXLWhiteness *qxl)
{
    red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_blackness_ptr(RedMemSlotInfo *slots, int group_id,
                                  SpiceBlackness *red, QXLBlackness *qxl)
{
    red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_invers_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceInvers *red, QXLInvers *qxl)
{
    red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_clip_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceClip *red, QXLClip *qxl)
{
    red->type = qxl->type;
    red->data = qxl->data;
}

void red_get_drawable(RedMemSlotInfo *slots, int group_id,
                      RedDrawable *red, SPICE_ADDRESS addr)
{
    QXLDrawable *qxl;
    int i;

    qxl = (QXLDrawable *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info     = &qxl->release_info;

    red->bbox             = qxl->bbox;
    red_get_clip_ptr(slots, group_id, &red->clip, &qxl->clip);
    red->effect           = qxl->effect;
    red->mm_time          = qxl->mm_time;
    red->self_bitmap      = qxl->self_bitmap;
    red->self_bitmap_area = qxl->self_bitmap_area;
    red->surface_id       = qxl->surface_id;

    for (i = 0; i < 3; i++) {
        red->surfaces_dest[i] = qxl->surfaces_dest[i];
        red->surfaces_rects[i] = qxl->surfaces_rects[i];
    }

    red->type = qxl->type;
    switch (red->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red_get_alpha_blend_ptr(slots, group_id,
                                &red->u.alpha_blend, &qxl->u.alpha_blend);
        break;
    case QXL_DRAW_BLACKNESS:
        red_get_blackness_ptr(slots, group_id, &red->u.blackness, &qxl->u.blackness);
        break;
    case QXL_DRAW_BLEND:
        red_get_blend_ptr(slots, group_id, &red->u.blend, &qxl->u.blend);
        break;
    case QXL_DRAW_COPY:
        red_get_copy_ptr(slots, group_id, &red->u.copy, &qxl->u.copy);
        break;
    case QXL_COPY_BITS:
        red->u.copy_bits = qxl->u.copy_bits;
        break;
    case QXL_DRAW_FILL:
        red_get_fill_ptr(slots, group_id, &red->u.fill, &qxl->u.fill);
        break;
    case QXL_DRAW_OPAQUE:
        red_get_opaque_ptr(slots, group_id, &red->u.opaque, &qxl->u.opaque);
        break;
    case QXL_DRAW_INVERS:
        red_get_invers_ptr(slots, group_id, &red->u.invers, &qxl->u.invers);
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red_get_rop3_ptr(slots, group_id, &red->u.rop3, &qxl->u.rop3);
        break;
    case QXL_DRAW_STROKE:
        red_get_stroke_ptr(slots, group_id, &red->u.stroke, &qxl->u.stroke);
        break;
    case QXL_DRAW_TEXT:
        red_get_text_ptr(slots, group_id, &red->u.text, &qxl->u.text);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_get_transparent_ptr(slots, group_id,
                                &red->u.transparent, &qxl->u.transparent);
        break;
    case QXL_DRAW_WHITENESS:
        red_get_whiteness_ptr(slots, group_id, &red->u.whiteness, &qxl->u.whiteness);
        break;
    default:
        red_error("unknown type");
        break;
    };
}

void red_get_compat_drawable(RedMemSlotInfo *slots, int group_id,
                             RedDrawable *red, SPICE_ADDRESS addr)
{
    QXLCompatDrawable *qxl;

    qxl = (QXLCompatDrawable *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info     = &qxl->release_info;

    red->bbox             = qxl->bbox;
    red_get_clip_ptr(slots, group_id, &red->clip, &qxl->clip);
    red->effect           = qxl->effect;
    red->mm_time          = qxl->mm_time;

    red->type = qxl->type;
    switch (red->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red_get_alpha_blend_ptr_compat(slots, group_id,
                                       &red->u.alpha_blend, &qxl->u.alpha_blend);
        break;
    case QXL_DRAW_BLACKNESS:
        red_get_blackness_ptr(slots, group_id, &red->u.blackness, &qxl->u.blackness);
        break;
    case QXL_DRAW_BLEND:
        red_get_blend_ptr(slots, group_id, &red->u.blend, &qxl->u.blend);
        break;
    case QXL_DRAW_COPY:
        red_get_copy_ptr(slots, group_id, &red->u.copy, &qxl->u.copy);
        break;
    case QXL_COPY_BITS:
        red->u.copy_bits = qxl->u.copy_bits;
        break;
    case QXL_DRAW_FILL:
        red_get_fill_ptr(slots, group_id, &red->u.fill, &qxl->u.fill);
        break;
    case QXL_DRAW_OPAQUE:
        red_get_opaque_ptr(slots, group_id, &red->u.opaque, &qxl->u.opaque);
        break;
    case QXL_DRAW_INVERS:
        red_get_invers_ptr(slots, group_id, &red->u.invers, &qxl->u.invers);
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red_get_rop3_ptr(slots, group_id, &red->u.rop3, &qxl->u.rop3);
        break;
    case QXL_DRAW_STROKE:
        red_get_stroke_ptr(slots, group_id, &red->u.stroke, &qxl->u.stroke);
        break;
    case QXL_DRAW_TEXT:
        red_get_text_ptr(slots, group_id, &red->u.text, &qxl->u.text);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_get_transparent_ptr(slots, group_id,
                                &red->u.transparent, &qxl->u.transparent);
        break;
    case QXL_DRAW_WHITENESS:
        red_get_whiteness_ptr(slots, group_id, &red->u.whiteness, &qxl->u.whiteness);
        break;
    default:
        red_error("unknown type");
        break;
    };
}

void red_put_drawable(RedDrawable *red)
{
    /* nothing yet */
}

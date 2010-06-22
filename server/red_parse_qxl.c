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

void red_get_drawable(RedMemSlotInfo *slots, int group_id,
                      RedDrawable *red, SPICE_ADDRESS addr)
{
    QXLDrawable *qxl;
    int i;

    qxl = (QXLDrawable *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info     = &qxl->release_info;

    red->bbox             = qxl->bbox;
    red->clip             = qxl->clip;
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
        red->u.alpha_blend = qxl->u.alpha_blend;
        break;
    case QXL_DRAW_BLACKNESS:
        red->u.blackness = qxl->u.blackness;
        break;
    case QXL_DRAW_BLEND:
        red->u.blend = qxl->u.blend;
        break;
    case QXL_DRAW_COPY:
        red->u.copy = qxl->u.copy;
        break;
    case QXL_DRAW_FILL:
        red->u.fill = qxl->u.fill;
        break;
    case QXL_DRAW_INVERS:
        red->u.invers = qxl->u.invers;
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red->u.rop3 = qxl->u.rop3;
        break;
    case QXL_DRAW_STROKE:
        red->u.stroke = qxl->u.stroke;
        break;
    case QXL_DRAW_TEXT:
        red->u.text = qxl->u.text;
        break;
    case QXL_DRAW_TRANSPARENT:
        red->u.transparent = qxl->u.transparent;
        break;
    case QXL_DRAW_WHITENESS:
        red->u.whiteness = qxl->u.whiteness;
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
    red->clip             = qxl->clip;
    red->effect           = qxl->effect;
    red->mm_time          = qxl->mm_time;

    red->type = qxl->type;
    switch (red->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red->u.alpha_blend = qxl->u.alpha_blend;
        break;
    case QXL_DRAW_BLACKNESS:
        red->u.blackness = qxl->u.blackness;
        break;
    case QXL_DRAW_BLEND:
        red->u.blend = qxl->u.blend;
        break;
    case QXL_DRAW_COPY:
        red->u.copy = qxl->u.copy;
        break;
    case QXL_DRAW_FILL:
        red->u.fill = qxl->u.fill;
        break;
    case QXL_DRAW_INVERS:
        red->u.invers = qxl->u.invers;
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red->u.rop3 = qxl->u.rop3;
        break;
    case QXL_DRAW_STROKE:
        red->u.stroke = qxl->u.stroke;
        break;
    case QXL_DRAW_TEXT:
        red->u.text = qxl->u.text;
        break;
    case QXL_DRAW_TRANSPARENT:
        red->u.transparent = qxl->u.transparent;
        break;
    case QXL_DRAW_WHITENESS:
        red->u.whiteness = qxl->u.whiteness;
        break;
    };
}

void red_put_drawable(RedDrawable *red)
{
    /* nothing yet */
}

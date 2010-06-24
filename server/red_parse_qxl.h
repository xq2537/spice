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

#ifndef RED_ABI_TRANSLATE_H
#define RED_ABI_TRANSLATE_H

#include <spice/qxl_dev.h>
#include <spice/start-packed.h>
#include "red_common.h"
#include "red_memslots.h"

typedef struct SPICE_ATTR_PACKED RedDrawable {
    QXLReleaseInfo *release_info;
    uint32_t surface_id;
    uint8_t effect;
    uint8_t type;
    uint8_t self_bitmap;
    SpiceRect self_bitmap_area;
    SpiceRect bbox;
    SpiceClip clip;
    uint32_t mm_time;
    int32_t surfaces_dest[3];
    SpiceRect surfaces_rects[3];
    union {
        SpiceFill fill;
        SpiceOpaque opaque;
        SpiceCopy copy;
        SpiceTransparent transparent;
        SpiceAlphaBlnd alpha_blend;
        QXLCopyBits copy_bits;
        SpiceBlend blend;
        SpiceRop3 rop3;
        SpiceStroke stroke;
        SpiceText text;
        SpiceBlackness blackness;
        SpiceInvers invers;
        SpiceWhiteness whiteness;
    } u;
} RedDrawable;

typedef struct SPICE_ATTR_PACKED RedUpdateCmd {
    QXLReleaseInfo *release_info;
    SpiceRect area;
    uint32_t update_id;
    uint32_t surface_id;
} RedUpdateCmd;

typedef struct SPICE_ATTR_PACKED RedMessage {
    QXLReleaseInfo *release_info;
    uint8_t *data;
} RedMessage;

void red_get_drawable(RedMemSlotInfo *slots, int group_id,
                      RedDrawable *red, SPICE_ADDRESS addr);
void red_get_compat_drawable(RedMemSlotInfo *slots, int group_id,
                             RedDrawable *red, SPICE_ADDRESS addr);
void red_put_drawable(RedDrawable *red);

void red_get_update_cmd(RedMemSlotInfo *slots, int group_id,
                        RedUpdateCmd *red, SPICE_ADDRESS addr);
void red_put_update_cmd(RedUpdateCmd *red);

void red_get_message(RedMemSlotInfo *slots, int group_id,
                     RedMessage *red, SPICE_ADDRESS addr);
void red_put_message(RedMessage *red);

#endif

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

#include <stdbool.h>
#include "red_common.h"
#include "red_memslots.h"
#include "red_parse_qxl.h"

#if 0
static void hexdump_qxl(RedMemSlotInfo *slots, int group_id,
                        SPICE_ADDRESS addr, uint8_t bytes)
{
    uint8_t *hex;
    int i;

    hex = (uint8_t*)get_virt(slots, addr, bytes, group_id);
    for (i = 0; i < bytes; i++) {
        if (0 == i % 16) {
            fprintf(stderr, "%lx: ", addr+i);
        }
        if (0 == i % 4) {
            fprintf(stderr, " ");
        }
        fprintf(stderr, " %02x", hex[i]);
        if (15 == i % 16) {
            fprintf(stderr, "\n");
        }
    }
}
#endif

static uint8_t *red_linearize_chunk(RedDataChunk *head, size_t size, bool *free_chunk)
{
    uint8_t *data, *ptr;
    RedDataChunk *chunk;
    uint32_t copy;

    if (head->next_chunk == NULL) {
        ASSERT(size <= head->data_size);
        *free_chunk = false;
        return head->data;
    }

    ptr = data = spice_malloc(size);
    *free_chunk = true;
    for (chunk = head; chunk != NULL && size > 0; chunk = chunk->next_chunk) {
        copy = MIN(chunk->data_size, size);
        memcpy(ptr, chunk->data, copy);
        ptr += copy;
        size -= copy;
    }
    ASSERT(size == 0);
    return data;
}

static size_t red_get_data_chunks_ptr(RedMemSlotInfo *slots, int group_id,
                                      int memslot_id,
                                      RedDataChunk *red, QXLDataChunk *qxl)
{
    RedDataChunk *red_prev;
    size_t data_size = 0;

    red->data_size = qxl->data_size;
    data_size += red->data_size;
    validate_virt(slots, (intptr_t)qxl->data, memslot_id, red->data_size, group_id);
    red->data = qxl->data;
    red->prev_chunk = NULL;

    while (qxl->next_chunk) {
        red_prev = red;
        red = spice_new(RedDataChunk, 1);
        memslot_id = get_memslot_id(slots, qxl->next_chunk);
        qxl = (QXLDataChunk*)get_virt(slots, qxl->next_chunk, sizeof(*qxl), group_id);

        red->data_size = qxl->data_size;
        data_size += red->data_size;
        validate_virt(slots, (intptr_t)qxl->data, memslot_id, red->data_size, group_id);
        red->data = qxl->data;
        red->prev_chunk = red_prev;
        red_prev->next_chunk = red;
    }

    red->next_chunk = NULL;
    return data_size;
}

#if 0
static size_t red_get_data_chunks(RedMemSlotInfo *slots, int group_id,
                                  RedDataChunk *red, SPICE_ADDRESS addr)
{
    QXLDataChunk *qxl;
    int memslot_id = get_memslot_id(slots, addr);

    qxl = (QXLDataChunk*)get_virt(slots, addr, sizeof(*qxl), group_id);
    return red_get_data_chunks_ptr(slots, group_id, memslot_id, red, qxl);
}
#endif

static void red_put_data_chunks(RedDataChunk *red)
{
    RedDataChunk *tmp;

    red = red->next_chunk;
    while (red) {
        tmp = red;
        red = red->next_chunk;
        free(tmp);
    }
}

static void red_get_point_ptr(SpicePoint *red, QXLPoint *qxl)
{
    red->x = qxl->x;
    red->y = qxl->y;
}

static void red_get_point16_ptr(SpicePoint16 *red, QXLPoint16 *qxl)
{
    red->x = qxl->x;
    red->y = qxl->y;
}

void red_get_rect_ptr(SpiceRect *red, QXLRect *qxl)
{
    red->top    = qxl->top;
    red->left   = qxl->left;
    red->bottom = qxl->bottom;
    red->right  = qxl->right;
}

static SpicePath *red_get_path(RedMemSlotInfo *slots, int group_id,
                               SPICE_ADDRESS addr)
{
    RedDataChunk chunks;
    QXLPathSeg *start, *end;
    SpicePathSeg *seg;
    uint8_t *data;
    bool free_data;
    QXLPath *qxl;
    SpicePath *red;
    size_t size, mem_size, mem_size2, dsize;
    int n_segments;
    int i;
    uint32_t count;

    qxl = (QXLPath *)get_virt(slots, addr, sizeof(*qxl), group_id);
    size = red_get_data_chunks_ptr(slots, group_id,
                                   get_memslot_id(slots, addr),
                                   &chunks, &qxl->chunk);
    data = red_linearize_chunk(&chunks, size, &free_data);
    red_put_data_chunks(&chunks);

    n_segments = 0;
    mem_size = sizeof(*red);

    start = (QXLPathSeg*)data;
    end = (QXLPathSeg*)(data + size);
    while (start < end) {
        n_segments++;
        count = start->count;
        mem_size += sizeof(SpicePathSeg) + count * sizeof(SpicePointFix);
        start = (QXLPathSeg*)(&start->points[count]);
    }

    red = spice_malloc(mem_size);
    red->num_segments = n_segments;

    start = (QXLPathSeg*)data;
    end = (QXLPathSeg*)(data + size);
    seg = (SpicePathSeg*)red->segments;
    n_segments = 0;
    mem_size2 = sizeof(*red);
    while (start < end) {
        n_segments++;
        count = start->count;

        /* Protect against overflow in size calculations before
           writing to memory */
        ASSERT(mem_size2 + sizeof(SpicePathSeg) > mem_size2);
        mem_size2  += sizeof(SpicePathSeg);
        ASSERT(count < UINT32_MAX / sizeof(SpicePointFix));
        dsize = count * sizeof(SpicePointFix);
        ASSERT(mem_size2 + dsize > mem_size2);
        mem_size2  += dsize;

        /* Verify that we didn't overflow due to guest changing data */
        ASSERT(mem_size2 <= mem_size);

        seg->flags = start->flags;
        seg->count = count;
        for (i = 0; i < seg->count; i++) {
            seg->points[i].x = start->points[i].x;
            seg->points[i].y = start->points[i].y;
        }
        start = (QXLPathSeg*)(&start->points[i]);
        seg = (SpicePathSeg*)(&seg->points[i]);
    }
    /* Ensure guest didn't tamper with segment count */
    ASSERT(n_segments == red->num_segments);

    if (free_data) {
        free(data);
    }
    return red;
}

static SpiceClipRects *red_get_clip_rects(RedMemSlotInfo *slots, int group_id,
                                          SPICE_ADDRESS addr)
{
    RedDataChunk chunks;
    QXLClipRects *qxl;
    SpiceClipRects *red;
    QXLRect *start, *end;
    uint8_t *data;
    bool free_data;
    size_t size;
    int i;

    qxl = (QXLClipRects *)get_virt(slots, addr, sizeof(*qxl), group_id);
    size = red_get_data_chunks_ptr(slots, group_id,
                                   get_memslot_id(slots, addr),
                                   &chunks, &qxl->chunk);
    data = red_linearize_chunk(&chunks, size, &free_data);
    red_put_data_chunks(&chunks);

    ASSERT(qxl->num_rects * sizeof(QXLRect) == size);
    red = spice_malloc(sizeof(*red) + qxl->num_rects * sizeof(SpiceRect));
    red->num_rects = qxl->num_rects;

    start = (QXLRect*)data;
    end = (QXLRect*)(data + size);
    for (i = 0; i < red->num_rects; i++) {
        red_get_rect_ptr(red->rects + i, start++);
    }

    if (free_data) {
        free(data);
    }
    return red;
}

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
        red_get_point_ptr(&red->u.pattern.pos, &qxl->u.pattern.pos);
        break;
    }
}

static void red_get_qmask_ptr(RedMemSlotInfo *slots, int group_id,
                              SpiceQMask *red, QXLQMask *qxl)
{
    red->flags  = qxl->flags;
    red_get_point_ptr(&red->pos, &qxl->pos);
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
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush);
   red->rop_descriptor = qxl->rop_descriptor;
   red->scale_mode     = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_copy_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceCopy *red, QXLCopy *qxl)
{
   red->src_bitmap      = qxl->src_bitmap;
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red->rop_descriptor  = qxl->rop_descriptor;
   red->scale_mode      = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_blend_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceBlend *red, QXLBlend *qxl)
{
   red->src_bitmap      = qxl->src_bitmap;
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red->rop_descriptor  = qxl->rop_descriptor;
   red->scale_mode      = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_transparent_ptr(RedMemSlotInfo *slots, int group_id,
                                    SpiceTransparent *red, QXLTransparent *qxl)
{
   red->src_bitmap      = qxl->src_bitmap;
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red->src_color       = qxl->src_color;
   red->true_color      = qxl->true_color;
}

static void red_get_alpha_blend_ptr(RedMemSlotInfo *slots, int group_id,
                                    SpiceAlphaBlnd *red, QXLAlphaBlnd *qxl)
{
    red->alpha_flags = qxl->alpha_flags;
    red->alpha       = qxl->alpha;
    red->src_bitmap  = qxl->src_bitmap;
    red_get_rect_ptr(&red->src_area, &qxl->src_area);
}

static void red_get_alpha_blend_ptr_compat(RedMemSlotInfo *slots, int group_id,
                                           SpiceAlphaBlnd *red, QXLCompatAlphaBlnd *qxl)
{
    red->alpha       = qxl->alpha;
    red->src_bitmap  = qxl->src_bitmap;
    red_get_rect_ptr(&red->src_area, &qxl->src_area);
}

static void red_get_rop3_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceRop3 *red, QXLRop3 *qxl)
{
   red->src_bitmap = qxl->src_bitmap;
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush);
   red->rop3       = qxl->rop3;
   red->scale_mode = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask);
}

static void red_get_stroke_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceStroke *red, QXLStroke *qxl)
{
   red->path = red_get_path(slots, group_id, qxl->path);
   red->attr.flags       = qxl->attr.flags;
   if (red->attr.flags & SPICE_LINE_FLAGS_STYLED) {
       red->attr.style_nseg  = qxl->attr.style_nseg;
       red->attr.style       = qxl->attr.style;
   } else {
       red->attr.style_nseg  = 0;
       red->attr.style       = 0;
   }
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush);
   red->fore_mode        = qxl->fore_mode;
   red->back_mode        = qxl->back_mode;
}

static void red_put_stroke_ptr(SpiceStroke *red)
{
    free(red->path);
}

static void red_get_text_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceText *red, QXLText *qxl)
{
   red->str        = qxl->str;
   red_get_rect_ptr(&red->back_area, &qxl->back_area);
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
    switch (red->type) {
    case SPICE_CLIP_TYPE_RECTS:
        red->rects = red_get_clip_rects(slots, group_id, qxl->data);
        break;
    }
}

static void red_put_clip(SpiceClip *red)
{
    switch (red->type) {
    case SPICE_CLIP_TYPE_RECTS:
        free(red->rects);
        break;
    }
}

void red_get_drawable(RedMemSlotInfo *slots, int group_id,
                      RedDrawable *red, SPICE_ADDRESS addr)
{
    QXLDrawable *qxl;
    int i;

    qxl = (QXLDrawable *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info     = &qxl->release_info;

    red_get_rect_ptr(&red->bbox, &qxl->bbox);
    red_get_clip_ptr(slots, group_id, &red->clip, &qxl->clip);
    red->effect           = qxl->effect;
    red->mm_time          = qxl->mm_time;
    red->self_bitmap      = qxl->self_bitmap;
    red_get_rect_ptr(&red->self_bitmap_area, &qxl->self_bitmap_area);
    red->surface_id       = qxl->surface_id;

    for (i = 0; i < 3; i++) {
        red->surfaces_dest[i] = qxl->surfaces_dest[i];
        red_get_rect_ptr(&red->surfaces_rects[i], &qxl->surfaces_rects[i]);
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
        red_get_point_ptr(&red->u.copy_bits.src_pos, &qxl->u.copy_bits.src_pos);
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

    red_get_rect_ptr(&red->bbox, &qxl->bbox);
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
        red_get_point_ptr(&red->u.copy_bits.src_pos, &qxl->u.copy_bits.src_pos);
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
    red_put_clip(&red->clip);
    switch (red->type) {
    case QXL_DRAW_STROKE:
        red_put_stroke_ptr(&red->u.stroke);
        break;
    }
}

void red_get_update_cmd(RedMemSlotInfo *slots, int group_id,
                        RedUpdateCmd *red, SPICE_ADDRESS addr)
{
    QXLUpdateCmd *qxl;

    qxl = (QXLUpdateCmd *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info     = &qxl->release_info;

    red_get_rect_ptr(&red->area, &qxl->area);
    red->update_id  = qxl->update_id;
    red->surface_id = qxl->surface_id;
}

void red_put_update_cmd(RedUpdateCmd *red)
{
    /* nothing yet */
}

void red_get_message(RedMemSlotInfo *slots, int group_id,
                     RedMessage *red, SPICE_ADDRESS addr)
{
    QXLMessage *qxl;

    /*
     * security alert:
     *   qxl->data[0] size isn't specified anywhere -> can't verify
     *   luckily this is for debug logging only,
     *   so we can just ignore it by default.
     */
    qxl = (QXLMessage *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info  = &qxl->release_info;
    red->data          = qxl->data;
}

void red_put_message(RedMessage *red)
{
    /* nothing yet */
}

void red_get_surface_cmd(RedMemSlotInfo *slots, int group_id,
                         RedSurfaceCmd *red, SPICE_ADDRESS addr)
{
    QXLSurfaceCmd *qxl;

    qxl = (QXLSurfaceCmd *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info     = &qxl->release_info;

    red->surface_id = qxl->surface_id;
    red->type       = qxl->type;
    red->flags      = qxl->flags;

    switch (red->type) {
    case QXL_SURFACE_CMD_CREATE:
        red->u.surface_create.format = qxl->u.surface_create.format;
        red->u.surface_create.width  = qxl->u.surface_create.width;
        red->u.surface_create.height = qxl->u.surface_create.height;
        red->u.surface_create.stride = qxl->u.surface_create.stride;
        red->u.surface_create.data   = qxl->u.surface_create.data;
        break;
    }
}

void red_put_surface_cmd(RedSurfaceCmd *red)
{
    /* nothing yet */
}

void red_get_cursor_cmd(RedMemSlotInfo *slots, int group_id,
                        RedCursorCmd *red, SPICE_ADDRESS addr)
{
    QXLCursorCmd *qxl;

    qxl = (QXLCursorCmd *)get_virt(slots, addr, sizeof(*qxl), group_id);
    red->release_info     = &qxl->release_info;

    red->type = qxl->type;
    switch (red->type) {
    case QXL_CURSOR_SET:
        red_get_point16_ptr(&red->u.set.position, &qxl->u.set.position);
        red->u.set.visible  = qxl->u.set.visible;
        red->u.set.shape    = qxl->u.set.shape;
        break;
    case QXL_CURSOR_MOVE:
        red_get_point16_ptr(&red->u.position, &qxl->u.position);
        break;
    case QXL_CURSOR_TRAIL:
        red->u.trail.length    = qxl->u.trail.length;
        red->u.trail.frequency = qxl->u.trail.frequency;
        break;
    }
}

void red_put_cursor_cmd(RedCursorCmd *red)
{
    /* nothing yet */
}


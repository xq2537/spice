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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "region.h"
#include "rect.h"

//#define ALLOC_ON_STEAL
//#define REGION_DEBUG

#define FALSE 0
#define TRUE 1

#define MIN(x, y) (((x) <= (y)) ? (x) : (y))
#define MAX(x, y) (((x) >= (y)) ? (x) : (y))

#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    abort();                                                \
}

#ifdef REGION_DEBUG
#define REGION_IS_VALID(region) region_is_valid(region)
#else
#define REGION_IS_VALID(region) TRUE
#endif

static int rect_is_valid(const SpiceRect *r)
{
    if (r->top > r->bottom || r->left > r->right) {
        printf("%s: invalid rect\n", __FUNCTION__);
        return FALSE;
    }
    return TRUE;
}

#ifdef REGION_TEST
static void rect_set(SpiceRect *r, int32_t top, int32_t left, int32_t bottom, int32_t right)
{
    r->top = top;
    r->left = left;
    r->bottom = bottom;
    r->right = right;
    ASSERT(rect_is_valid(r));
}

#endif

static inline void __region_init(QRegion *rgn)
{
    rgn->num_rects = 0;
    rgn->rects = rgn->buf;
    rgn->rects_size = RECTS_BUF_SIZE;
}

void region_init(QRegion *rgn)
{
    __region_init(rgn);
    ASSERT(REGION_IS_VALID(rgn));
}

void region_clear(QRegion *rgn)
{
    rgn->num_rects = 0;
}

void region_destroy(QRegion *rgn)
{
    ASSERT(REGION_IS_VALID(rgn));
    if (rgn->rects != rgn->buf) {
        free(rgn->rects);
    }
}

void region_clone(QRegion *dest, const QRegion *src)
{
    ASSERT(REGION_IS_VALID(src));
    dest->bbox = src->bbox;
    if ((dest->num_rects = src->num_rects) <= RECTS_BUF_SIZE) {
        dest->rects = dest->buf;
        dest->rects_size = RECTS_BUF_SIZE;
    } else {
        dest->rects = (SpiceRect *)malloc(sizeof(SpiceRect) * dest->num_rects);
        dest->rects_size = dest->num_rects;
    }
    memcpy(dest->rects, src->rects, dest->num_rects * sizeof(SpiceRect));
    ASSERT(REGION_IS_VALID(src));
    ASSERT(REGION_IS_VALID(dest));
}

int region_is_valid(const QRegion *rgn)
{
    if (rgn->num_rects) {
        uint32_t i;
        SpiceRect bbox;

        if (!rect_is_valid(&rgn->bbox)) {
            return FALSE;
        }
        bbox = rgn->rects[0];
        if (!rect_is_valid(&bbox) || rect_is_empty(&bbox)) {
            return FALSE;
        }
        for (i = 1; i < rgn->num_rects; i++) {
            SpiceRect *r;

            r = &rgn->rects[i];
            if (!rect_is_valid(r) || rect_is_empty(r)) {
                return FALSE;
            }

            SpiceRect *priv = r - 1;
            if (r->top < priv->top) {
                return FALSE;
            } else if (r->top == priv->top) {
                if (r->bottom != priv->bottom) {
                    return FALSE;
                }
                if (r->left < priv->right) {
                    return FALSE;
                }
            } else if (priv->bottom > r->top) {
                return FALSE;
            }
            bbox.top = MIN(bbox.top, r->top);
            bbox.left = MIN(bbox.left, r->left);
            bbox.bottom = MAX(bbox.bottom, r->bottom);
            bbox.right = MAX(bbox.right, r->right);
        }
        return rect_is_equal(&bbox, &rgn->bbox);
    }
    return TRUE;
}

void region_dump(const QRegion *rgn, const char *prefix)
{
    char *indent;
    int len;
    uint32_t i;

    len = strlen(prefix);
    if (!(indent = (char *)malloc(len + 1))) {
        printf("%s: malloc failed\n", __FUNCTION__);
        return;
    }
    memset(indent, ' ', len);
    indent[len] = 0;


    printf("%sREGION: %p, size %u storage is %s, ",
           prefix,
           rgn,
           rgn->rects_size,
           (rgn->rects == rgn->buf) ? "BUF" : "MALLOC");

    if (rgn->num_rects == 0) {
        printf("EMPTY\n");
        return;
    }

    printf("num %u bounds (%d, %d, %d, %d)\n",
           rgn->num_rects,
           rgn->bbox.top,
           rgn->bbox.left,
           rgn->bbox.bottom,
           rgn->bbox.right);

    for (i = 0; i < rgn->num_rects; i++) {
        printf("%s  %12d %12d %12d %12d\n",
               indent,
               rgn->rects[i].top,
               rgn->rects[i].left,
               rgn->rects[i].bottom,
               rgn->rects[i].right);
    }
    free(indent);
    ASSERT(region_is_valid(rgn));
}

int region_is_empty(const QRegion *rgn)
{
    ASSERT(REGION_IS_VALID(rgn));
    return !rgn->num_rects;
}

#ifdef REGION_USE_IMPROVED

int region_is_equal(const QRegion *rgn1, const QRegion *rgn2)
{
    int test_res;

    ASSERT(REGION_IS_VALID(rgn1));
    ASSERT(REGION_IS_VALID(rgn2));

    if (rgn1->num_rects == 0 || rgn2->num_rects == 0) {
        return rgn1->num_rects == rgn2->num_rects;
    }

    if (!rect_is_equal(&rgn1->bbox, &rgn2->bbox)) {
        return FALSE;
    }

    test_res = region_test(rgn1, rgn2, REGION_TEST_LEFT_EXCLUSIVE | REGION_TEST_RIGHT_EXCLUSIVE);
    return !test_res;
}

#else

int region_is_equal(const QRegion *rgn1, const QRegion *rgn2)
{
    QRegion tmp_rgn;
    int ret;

    ASSERT(REGION_IS_VALID(rgn1));
    ASSERT(REGION_IS_VALID(rgn2));

    if (rgn1->num_rects == 0 || rgn2->num_rects == 0) {
        return rgn1->num_rects == rgn2->num_rects;
    }

    if (!rect_is_equal(&rgn1->bbox, &rgn2->bbox)) {
        return FALSE;
    }

    region_clone(&tmp_rgn, rgn1);
    region_xor(&tmp_rgn, rgn2);
    ret = region_is_empty(&tmp_rgn);
    region_destroy(&tmp_rgn);
    return ret;
}

#endif

typedef struct RgnOpCtx {
    SpiceRect *now;
    SpiceRect *end;
    SpiceRect *scan_line;
    SpiceRect r;
    SpiceRect split;
#ifdef REGION_USE_IMPROVED
    int abort;
#endif
} RgnOpCtx;

static inline int op_ctx_is_valid(RgnOpCtx *ctx)
{
    return ctx->now != ctx->end;
}

static void op_context_next(RgnOpCtx *ctx)
{
    SpiceRect *now;
    SpiceRect *next;

    ASSERT(op_ctx_is_valid(ctx));
    now = ctx->now;
    next = now + 1;

    if (next == ctx->end || now->top != next->top) {
        if (now->bottom != ctx->r.bottom) { //h_split
            ctx->r.top = ctx->r.bottom;
            ctx->r.bottom = now->bottom;
            next = ctx->scan_line;
        } else {
            if (next == ctx->end) {
#ifdef REGION_USE_IMPROVED
                ctx->scan_line = ++ctx->now;
#else
                ++ctx->now;
#endif
                ctx->r.top = ctx->r.left = ctx->r.bottom = ctx->r.right = (1U << 31) - 1;
                return;
            }
            ctx->scan_line = next;
            ctx->r.top = next->top;
            ctx->r.bottom = next->bottom;
        }
    }
    ctx->r.left = next->left;
    ctx->r.right = next->right;
    ctx->now = next;
}

static void op_context_init(RgnOpCtx *ctx, uint32_t num_rects, SpiceRect *rects)
{
    ctx->scan_line = ctx->now = rects;
    ctx->end = ctx->now + num_rects;
#ifdef REGION_USE_IMPROVED
    ctx->abort = FALSE;
#endif
    if (!op_ctx_is_valid(ctx)) {
        ctx->r.top = ctx->r.left = ctx->r.bottom = ctx->r.right = (1U << 31) - 1;
    } else {
        ctx->r = *ctx->now;
    }
}

static inline void op_ctx_h_split(RgnOpCtx *ctx, int32_t h_line)
{
    ctx->r.bottom = h_line;
    ctx->split = ctx->r;
    op_context_next(ctx);
}

static inline void op_ctx_v_split(RgnOpCtx *ctx, int32_t v_line)
{
    ctx->split = ctx->r;
    ctx->r.left = ctx->split.right = v_line;
    if (rect_is_empty(&ctx->r)) {
        op_context_next(ctx);
    }
}

static inline void op_ctx_split(RgnOpCtx *ctx, int32_t h_line)
{
    ASSERT(ctx->now == ctx->scan_line);
    ctx->r.bottom = h_line;
}

static void region_steal_rects(QRegion *rgn, uint32_t *num_rects, SpiceRect **rects)
{
    ASSERT(REGION_IS_VALID(rgn));
    if ((*num_rects = rgn->num_rects)) {
        if (rgn->rects == rgn->buf) {
            *rects = (SpiceRect *)malloc(sizeof(SpiceRect) * rgn->num_rects);
            memcpy(*rects, rgn->rects, sizeof(SpiceRect) * rgn->num_rects);
        } else {
            *rects = rgn->rects;
#ifdef ALLOC_ON_STEAL
            rgn->rects = (SpiceRect *)malloc(sizeof(SpiceRect) * rgn->num_rects);
            rgn->rects_size = rgn->num_rects;
            rgn->num_rects = 0;
            return;
#endif
        }
    } else {
        *rects = NULL;
    }
    __region_init(rgn);
    ASSERT(REGION_IS_VALID(rgn));
}

typedef struct JoinContext {
    QRegion *rgn;
    SpiceRect *line0;
    SpiceRect *line1;
    SpiceRect *end;
} JoinContext;

static inline SpiceRect *__get_line(QRegion *rgn, SpiceRect *pos)
{
    SpiceRect *end = rgn->rects + rgn->num_rects;

    if (pos < end) {
        int32_t line_top = pos->top;
        while (++pos < end && pos->top == line_top) {
            ASSERT((pos - 1)->right < pos->left); //join in region_push_rect
        }
    }
    return pos;
}

static inline int region_join_init(QRegion *rgn, JoinContext *context)
{
    context->rgn = rgn;
    context->end = __get_line(rgn, (context->line1 = rgn->rects));
    return context->end != context->line1;
}

static inline int region_join_next(JoinContext *context)
{
    context->line0 = context->line1;
    context->line1 = context->end;
    context->end = __get_line(context->rgn, context->line1);
    return context->end != context->line1;
}

static inline void region_join_join(JoinContext *context)
{
    SpiceRect *pos_0 = context->line0;
    SpiceRect *pos_1 = context->line1;
    int32_t bottom;
    QRegion *rgn;

    if (pos_0->bottom != pos_1->top) {
        return;
    }

    if (pos_1 - pos_0 != context->end - pos_1) {
        return;
    }

    for (; pos_1 < context->end; pos_0++, pos_1++) {
        if (pos_0->left != pos_1->left || pos_0->right != pos_1->right) {
            return;
        }
    }
    bottom = context->line1->bottom;
    pos_0 = context->line0;
    for (; pos_0 < context->line1; pos_0++) {
        pos_0->bottom = bottom;
    }
    rgn = context->rgn;
    memmove(context->line1, context->end,
            (unsigned long)(rgn->rects + rgn->num_rects) - (unsigned long)context->end);
    rgn->num_rects -= (context->line1 - context->line0);
    context->end = context->line1;
    context->line1 = context->line0;
}

static inline void region_join(QRegion *rgn)
{
    JoinContext context;

    ASSERT(REGION_IS_VALID(rgn));

    if (!region_join_init(rgn, &context)) {
        return;
    }
    while (region_join_next(&context)) {
        region_join_join(&context);
    }

    ASSERT(REGION_IS_VALID(rgn));
}

static void region_push_rect(QRegion *rgn, SpiceRect *r)
{
    ASSERT(REGION_IS_VALID(rgn));
    ASSERT(rect_is_valid(r));
    if (rgn->num_rects == 0) {
        rgn->num_rects++;
        rgn->rects[0] = rgn->bbox = *r;
        return;
    } else {
        SpiceRect *priv = &rgn->rects[rgn->num_rects - 1];

        if (priv->top == r->top && priv->right == r->left) {
            ASSERT(priv->bottom == r->bottom);
            priv->right = r->right;
            rgn->bbox.right = MAX(rgn->bbox.right, priv->right);
            return;
        }
        if (rgn->rects_size == rgn->num_rects) {
            SpiceRect *old = rgn->rects;
            rgn->rects_size = rgn->rects_size * 2;
            rgn->rects = (SpiceRect *)malloc(sizeof(SpiceRect) * rgn->rects_size);
            memcpy(rgn->rects, old, sizeof(SpiceRect) * rgn->num_rects);
            if (old != rgn->buf) {
                free(old);
            }
        }
        rgn->rects[rgn->num_rects++] = *r;
        rect_union(&rgn->bbox, r);
    }
}

#ifdef REGION_USE_IMPROVED

static SpiceRect *op_context_find_area_below(RgnOpCtx *ctx, int32_t val)
{
    SpiceRect *start = ctx->now;
    SpiceRect *end = ctx->end;

    while (start != end) {
        int pos = (end - start) / 2;
        if (start[pos].bottom <= val) {
            start = &start[pos + 1];
        } else {
            end = &start[pos];
        }
    }
    return start;
}

static int op_context_skip_v(RgnOpCtx *ctx, int32_t top)
{
    SpiceRect *end = op_context_find_area_below(ctx, top);
    if (end != ctx->now) {
        ctx->now = ctx->scan_line = end;
        if (ctx->now == ctx->end) {
            ctx->r.top = ctx->r.left = ctx->r.bottom = ctx->r.right = (1U << 31) - 1;
        } else {
            ctx->r = *ctx->now;
        }
        return TRUE;
    }
    return FALSE;
}

typedef void (*op_callback_t)(RgnOpCtx *context, SpiceRect *, SpiceRect *);

static void op_context_skip(RgnOpCtx *self, RgnOpCtx *other, op_callback_t on_self,
                            op_callback_t on_other)
{
    SpiceRect *save1 = self->now;
    SpiceRect *save2 = other->now;
    int more;
    do {
        op_context_skip_v(self, other->r.top);
        if (save1 != self->now) {
            if (on_self) {
                on_self(self, save1, self->now);
            }
            save1 = self->now;
        }
        more = op_context_skip_v(other, self->r.top);
        if (save2 != other->now) {
            if (on_other) {
                on_other(self, save2, other->now);
            }
            save2 = other->now;
        }
    } while (more && !self->abort);
}

static inline int op_context_more_overlap(RgnOpCtx *ctx, int32_t *bottom)
{
    if (!op_ctx_is_valid(ctx)) {
        return FALSE;
    }

    if (ctx->scan_line->bottom > *bottom && ctx->scan_line->top < *bottom) {
        *bottom = ctx->scan_line->bottom;
    }
    return ctx->scan_line->top < *bottom;
}

static inline void op_context_overlap(RgnOpCtx *self, RgnOpCtx *other, op_callback_t on_self,
                                      op_callback_t on_other, op_callback_t on_both)
{
    int32_t bottom = MAX(self->scan_line->bottom, other->scan_line->bottom);

    do {
        if (self->r.top < other->r.top) {
            op_ctx_h_split(self, MIN(other->r.top, self->r.bottom));
            if (on_self) {
                on_self(self, &self->split, &self->split + 1);
            }
        } else if (self->r.top > other->r.top) {
            op_ctx_h_split(other, MIN(self->r.top, other->r.bottom));
            if (on_other) {
                on_other(self, &other->split, &other->split + 1);
            }
        } else {
            if (self->r.bottom > other->r.bottom) {
                op_ctx_split(self, other->r.bottom);
            } else if (other->r.bottom > self->r.bottom) {
                op_ctx_split(other, self->r.bottom);
            }
            if (self->r.left < other->r.left) {
                op_ctx_v_split(self, MIN(other->r.left, self->r.right));
                if (on_self) {
                    on_self(self, &self->split, &self->split + 1);
                }
            } else if (self->r.left > other->r.left) {
                op_ctx_v_split(other, MIN(self->r.left, other->r.right));
                if (on_other) {
                    on_other(self, &other->split, &other->split + 1);
                }
            } else {
                int32_t right = MIN(self->r.right, other->r.right);
                op_ctx_v_split(self, right);
                op_ctx_v_split(other, right);
                if (on_both) {
                    on_both(self, &self->split, &self->split + 1);
                }
            }
        }
    } while (!self->abort && (op_context_more_overlap(self, &bottom) ||
                              op_context_more_overlap(other, &bottom)));
}

static inline void op_context_op(RgnOpCtx *self, RgnOpCtx *other, op_callback_t on_self,
                                 op_callback_t on_other, op_callback_t on_both)
{
    for (;;) {
        op_context_skip(self, other, on_self, on_other);
        if (self->abort || !op_ctx_is_valid(self)) {
            ASSERT(self->abort || !op_ctx_is_valid(other));
            return;
        }
        op_context_overlap(self, other, on_self, on_other, on_both);
    }
}

typedef struct SelfOpCtx {
    RgnOpCtx ctx;
    QRegion *rgn;
} SelfOpCtx;

static void add_rects(RgnOpCtx *ctx, SpiceRect *now, SpiceRect *end)
{
    SelfOpCtx *self_ctx = (SelfOpCtx *)ctx;
    for (; now < end; now++) {
        region_push_rect(self_ctx->rgn, now);
    }
}

static void region_op(QRegion *rgn, const QRegion *other_rgn, op_callback_t on_self,
                      op_callback_t on_other, op_callback_t on_both)
{
    SelfOpCtx self;
    RgnOpCtx other;
    uint32_t num_rects;
    SpiceRect *rects;

    region_steal_rects(rgn, &num_rects, &rects);
    op_context_init(&self.ctx, num_rects, rects);
    self.rgn = rgn;
    op_context_init(&other, other_rgn->num_rects, other_rgn->rects);
    op_context_op(&self.ctx, &other, on_self, on_other, on_both);
    free(rects);
    region_join(rgn);
}

void region_or(QRegion *rgn, const QRegion *other_rgn)
{
    region_op(rgn, other_rgn, add_rects, add_rects, add_rects);
}

void region_and(QRegion *rgn, const QRegion *other_rgn)
{
    if (!region_bounds_intersects(rgn, other_rgn)) {
        region_clear(rgn);
        return;
    }
    region_op(rgn, other_rgn, NULL, NULL, add_rects);
}

void region_xor(QRegion *rgn, const QRegion *other_rgn)
{
    region_op(rgn, other_rgn, add_rects, add_rects, NULL);
}

void region_exclude(QRegion *rgn, const QRegion *other_rgn)
{
    if (!region_bounds_intersects(rgn, other_rgn)) {
        return;
    }
    region_op(rgn, other_rgn, add_rects, NULL, NULL);
}

typedef struct TestOpCtx {
    RgnOpCtx ctx;
    int result;
    int abort_on;
} TestOpCtx;


static void region_test_on_self(RgnOpCtx *ctx, SpiceRect *now, SpiceRect *end)
{
    TestOpCtx *test_ctx = (TestOpCtx *)ctx;
    test_ctx->result |= REGION_TEST_LEFT_EXCLUSIVE;
    test_ctx->result &= test_ctx->abort_on;
    if (test_ctx->result == test_ctx->abort_on) {
        test_ctx->ctx.abort = TRUE;
    }
}

static void region_test_on_other(RgnOpCtx *ctx, SpiceRect *now, SpiceRect *end)
{
    TestOpCtx *test_ctx = (TestOpCtx *)ctx;
    test_ctx->result |= REGION_TEST_RIGHT_EXCLUSIVE;
    test_ctx->result &= test_ctx->abort_on;
    if (test_ctx->result == test_ctx->abort_on) {
        test_ctx->ctx.abort = TRUE;
    }
}

static void region_test_on_both(RgnOpCtx *ctx, SpiceRect *now, SpiceRect *end)
{
    TestOpCtx *test_ctx = (TestOpCtx *)ctx;
    test_ctx->result |= REGION_TEST_SHARED;
    test_ctx->result &= test_ctx->abort_on;
    if (test_ctx->result == test_ctx->abort_on) {
        test_ctx->ctx.abort = TRUE;
    }
}

int region_test(const QRegion *rgn, const QRegion *other_rgn, int query)
{
    TestOpCtx self;
    RgnOpCtx other;

    op_context_init(&self.ctx, rgn->num_rects, rgn->rects);
    self.result = 0;
    self.abort_on = (query) ? query & REGION_TEST_ALL : REGION_TEST_ALL;
    op_context_init(&other, other_rgn->num_rects, other_rgn->rects);
    op_context_op(&self.ctx, &other, region_test_on_self, region_test_on_other,
                  region_test_on_both);
    return self.result;
}

#else

#define RIGION_OP_ADD_SELF (1 << 0)
#define RIGION_OP_ADD_OTHER (1 << 1)
#define RIGION_OP_ADD_COMMON (1 << 2)

static inline void region_on_self(QRegion *rgn, SpiceRect *r, uint32_t op)
{
    ASSERT(REGION_IS_VALID(rgn));
    if (op & RIGION_OP_ADD_SELF) {
        region_push_rect(rgn, r);
    }
}

static inline void region_on_other(QRegion *rgn, SpiceRect *r, uint32_t op)
{
    ASSERT(REGION_IS_VALID(rgn));
    if (op & RIGION_OP_ADD_OTHER) {
        region_push_rect(rgn, r);
    }
}

static inline void region_on_both(QRegion *rgn, SpiceRect *r, uint32_t op)
{
    ASSERT(REGION_IS_VALID(rgn));
    if (op & RIGION_OP_ADD_COMMON) {
        region_push_rect(rgn, r);
    }
}

static void region_op(QRegion *rgn, const QRegion *other_rgn, uint32_t op)
{
    RgnOpCtx self;
    RgnOpCtx other;
    uint32_t num_rects;
    SpiceRect *rects;

    ASSERT(REGION_IS_VALID(rgn));
    ASSERT(REGION_IS_VALID(other_rgn));
    region_steal_rects(rgn, &num_rects, &rects);

    op_context_init(&self, num_rects, rects);
    op_context_init(&other, other_rgn->num_rects, other_rgn->rects);

    while (op_ctx_is_valid(&self) || op_ctx_is_valid(&other)) {
        if (self.r.top < other.r.top) {
            op_ctx_h_split(&self, MIN(other.r.top, self.r.bottom));
            region_on_self(rgn, &self.split, op);
        } else if (self.r.top > other.r.top) {
            op_ctx_h_split(&other, MIN(self.r.top, other.r.bottom));
            region_on_other(rgn, &other.split, op);
        } else {
            if (self.r.bottom > other.r.bottom) {
                op_ctx_split(&self, other.r.bottom);
            } else if (other.r.bottom > self.r.bottom) {
                op_ctx_split(&other, self.r.bottom);
            }
            if (self.r.left < other.r.left) {
                op_ctx_v_split(&self, MIN(other.r.left, self.r.right));
                region_on_self(rgn, &self.split, op);
            } else if (self.r.left > other.r.left) {
                op_ctx_v_split(&other, MIN(self.r.left, other.r.right));
                region_on_other(rgn, &other.split, op);
            } else {
                int32_t right = MIN(self.r.right, other.r.right);
                op_ctx_v_split(&self, right);
                op_ctx_v_split(&other, right);
                region_on_both(rgn, &self.split, op);
            }
        }
    }
    free(rects);
    region_join(rgn);
}

void region_or(QRegion *rgn, const QRegion *other_rgn)
{
    region_op(rgn, other_rgn, RIGION_OP_ADD_SELF | RIGION_OP_ADD_OTHER | RIGION_OP_ADD_COMMON);
}

void region_and(QRegion *rgn, const QRegion *other_rgn)
{
    region_op(rgn, other_rgn, RIGION_OP_ADD_COMMON);
}

void region_xor(QRegion *rgn, const QRegion *other_rgn)
{
    region_op(rgn, other_rgn, RIGION_OP_ADD_SELF | RIGION_OP_ADD_OTHER);
}

void region_exclude(QRegion *rgn, const QRegion *other_rgn)
{
    region_op(rgn, other_rgn, RIGION_OP_ADD_SELF);
}

#endif


void region_offset(QRegion *rgn, int32_t dx, int32_t dy)
{
    SpiceRect *now;
    SpiceRect *end;
    ASSERT(REGION_IS_VALID(rgn));
    if (region_is_empty(rgn)) {
        return;
    }
    rect_offset(&rgn->bbox, dx, dy);
    now = rgn->rects;
    end = now + rgn->num_rects;
    for (; now < end; now++) {
        rect_offset(now, dx, dy);
    }
}

void region_add(QRegion *rgn, const SpiceRect *r)
{
    ASSERT(REGION_IS_VALID(rgn));
    ASSERT(rect_is_valid(r));

    if (!rgn->num_rects) {
        if (rect_is_empty(r)) {
            return;
        }
        rgn->num_rects++;
        rgn->rects[0] = rgn->bbox = *r;
    } else {
        QRegion rect_rgn;
        region_init(&rect_rgn);
        region_add(&rect_rgn, r);
        region_or(rgn, &rect_rgn);
    }
}

void region_remove(QRegion *rgn, const SpiceRect *r)
{
    ASSERT(REGION_IS_VALID(rgn));
    ASSERT(rect_is_valid(r));
    if (rgn->num_rects) {
        QRegion rect_rgn;

        region_init(&rect_rgn);
        region_add(&rect_rgn, r);
        region_exclude(rgn, &rect_rgn);
    }
}

#ifdef REGION_USE_IMPROVED

int region_intersects(const QRegion *rgn1, const QRegion *rgn2)
{
    int test_res;

    ASSERT(REGION_IS_VALID(rgn1));
    ASSERT(REGION_IS_VALID(rgn2));

    if (!region_bounds_intersects(rgn1, rgn2)) {
        return FALSE;
    }

    test_res = region_test(rgn1, rgn2, REGION_TEST_SHARED);
    return !!test_res;
}

#else

int region_intersects(const QRegion *rgn1, const QRegion *rgn2)
{
    QRegion tmp;
    int ret;

    ASSERT(REGION_IS_VALID(rgn1));
    ASSERT(REGION_IS_VALID(rgn2));

    region_clone(&tmp, rgn1);
    region_and(&tmp, rgn2);
    ret = !region_is_empty(&tmp);
    region_destroy(&tmp);
    return ret;
}

#endif

int region_bounds_intersects(const QRegion *rgn1, const QRegion *rgn2)
{
    return !region_is_empty(rgn1) && !region_is_empty(rgn2) &&
           rect_intersects(&rgn1->bbox, &rgn2->bbox);
}

#ifdef REGION_USE_IMPROVED

int region_contains(const QRegion *rgn, const QRegion *other)
{
    int test_res;

    ASSERT(REGION_IS_VALID(rgn));
    ASSERT(REGION_IS_VALID(other));

    test_res = region_test(rgn, other, REGION_TEST_RIGHT_EXCLUSIVE);
    return !test_res;
}

#else

int region_contains(const QRegion *rgn, const QRegion *other)
{
    QRegion tmp;
    int ret;

    ASSERT(REGION_IS_VALID(rgn));
    ASSERT(REGION_IS_VALID(other));

    region_clone(&tmp, rgn);
    region_and(&tmp, other);
    ret = region_is_equal(&tmp, other);
    region_destroy(&tmp);
    return ret;
}

#endif

int region_contains_point(const QRegion *rgn, int32_t x, int32_t y)
{
    if (region_is_empty(rgn)) {
        return FALSE;
    }
    SpiceRect point;
    point.left = x;
    point.right = point.left + 1;
    point.top = y;
    point.bottom = point.top + 1;

    if (!rect_intersects(&rgn->bbox, &point)) {
        return FALSE;
    }

    SpiceRect* now = rgn->rects;
    SpiceRect* end = now + rgn->num_rects;

    for (; now < end; now++) {
        if (rect_intersects(now, &point)) {
            return TRUE;
        }
    }
    return FALSE;
}

#ifdef REGION_TEST

static void test(const QRegion *r1, const QRegion *r2, int *expected)
{
    printf("r1 is_empty %s [%s]\n",
           region_is_empty(r1) ? "TRUE" : "FALSE",
           (region_is_empty(r1) == *(expected++)) ? "OK" : "ERR");
    printf("r2 is_empty %s [%s]\n",
           region_is_empty(r2) ? "TRUE" : "FALSE",
           (region_is_empty(r2) == *(expected++)) ? "OK" : "ERR");
    printf("is_equal %s [%s]\n",
           region_is_equal(r1, r2) ? "TRUE" : "FALSE",
           (region_is_equal(r1, r2) == *(expected++)) ? "OK" : "ERR");
    printf("intersects %s [%s]\n",
           region_intersects(r1, r2) ? "TRUE" : "FALSE",
           (region_intersects(r1, r2) == *(expected++)) ? "OK" : "ERR");
    printf("contains %s [%s]\n",
           region_contains(r1, r2) ? "TRUE" : "FALSE",
           (region_contains(r1, r2) == *(expected++)) ? "OK" : "ERR");
}

enum {
    EXPECT_R1_EMPTY,
    EXPECT_R2_EMPTY,
    EXPECT_EQUAL,
    EXPECT_SECT,
    EXPECT_CONT,
};

int main(void)
{
    QRegion _r1, _r2, _r3;
    QRegion *r1 = &_r1;
    QRegion *r2 = &_r2;
    QRegion *r3 = &_r3;
    SpiceRect _r;
    SpiceRect *r = &_r;
    int expected[5];

    region_init(r1);
    region_init(r2);

    printf("dump r1 empty rgn [%s]\n", region_is_valid(r1) ? "VALID" : "INVALID");
    region_dump(r1, "");
    expected[EXPECT_R1_EMPTY] = TRUE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = TRUE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    region_clone(r3, r1);
    printf("dump r3 clone rgn [%s]\n", region_is_valid(r3) ? "VALID" : "INVALID");
    region_dump(r3, "");
    expected[EXPECT_R1_EMPTY] = TRUE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = TRUE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r3, expected);
    region_destroy(r3);
    printf("\n");

    rect_set(r, 0, 0, 100, 100);
    region_add(r1, r);
    printf("dump r1 [%s]\n", region_is_valid(r1) ? "VALID" : "INVALID");
    region_dump(r1, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r1);
    rect_set(r, 0, 0, 0, 0);
    region_add(r1, r);
    printf("dump r1 [%s]\n", region_is_valid(r1) ? "VALID" : "INVALID");
    region_dump(r1, "");
    expected[EXPECT_R1_EMPTY] = TRUE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = TRUE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    rect_set(r, -100, -100, 0, 0);
    region_add(r1, r);
    printf("dump r1 [%s]\n", region_is_valid(r1) ? "VALID" : "INVALID");
    region_dump(r1, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r1);
    rect_set(r, -100, -100, 100, 100);
    region_add(r1, r);
    printf("dump r1 [%s]\n", region_is_valid(r1) ? "VALID" : "INVALID");
    region_dump(r1, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");


    region_clear(r1);
    region_clear(r2);

    rect_set(r, 100, 100, 200, 200);
    region_add(r1, r);
    printf("dump r1 [%s]\n", region_is_valid(r1) ? "VALID" : "INVALID");
    region_dump(r1, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    rect_set(r, 300, 300, 400, 400);
    region_add(r1, r);
    printf("dump r1 [%s]\n", region_is_valid(r1) ? "VALID" : "INVALID");
    region_dump(r1, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = TRUE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    rect_set(r, 500, 500, 600, 600);
    region_add(r2, r);
    printf("dump r2 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = FALSE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r2);

    rect_set(r, 100, 100, 200, 200);
    region_add(r2, r);
    rect_set(r, 300, 300, 400, 400);
    region_add(r2, r);
    printf("dump r2 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = TRUE;
    expected[EXPECT_SECT] = TRUE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r2);

    rect_set(r, 100, 100, 200, 200);
    region_add(r2, r);
    printf("dump r2 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = TRUE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r2);

    rect_set(r, -2000, -2000, -1000, -1000);
    region_add(r2, r);
    printf("dump r2 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = FALSE;
    expected[EXPECT_CONT] = FALSE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r2);

    rect_set(r, -2000, -2000, 1000, 1000);
    region_add(r2, r);
    printf("dump r2 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = TRUE;
    expected[EXPECT_CONT] = FALSE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r2);

    rect_set(r, 150, 150, 175, 175);
    region_add(r2, r);
    printf("dump r2 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = TRUE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r2, expected);
    printf("\n");

    region_clear(r2);

    rect_set(r, 150, 150, 350, 350);
    region_add(r2, r);
    printf("dump r2 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = TRUE;
    expected[EXPECT_CONT] = FALSE;
    test(r1, r2, expected);
    printf("\n");

    region_and(r2, r1);
    printf("dump r2 and r1 [%s]\n", region_is_valid(r2) ? "VALID" : "INVALID");
    region_dump(r2, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = FALSE;
    expected[EXPECT_SECT] = TRUE;
    expected[EXPECT_CONT] = FALSE;
    test(r2, r1, expected);
    printf("\n");


    region_clone(r3, r1);
    printf("dump r3 clone rgn [%s]\n", region_is_valid(r3) ? "VALID" : "INVALID");
    region_dump(r3, "");
    expected[EXPECT_R1_EMPTY] = FALSE;
    expected[EXPECT_R2_EMPTY] = FALSE;
    expected[EXPECT_EQUAL] = TRUE;
    expected[EXPECT_SECT] = TRUE;
    expected[EXPECT_CONT] = TRUE;
    test(r1, r3, expected);
    printf("\n");


    region_destroy(r3);
    region_destroy(r1);
    region_destroy(r2);

    return 0;
}

#endif


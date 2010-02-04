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

#ifndef _H_REGION
#define _H_REGION

#include <stdint.h>
#include "draw.h"

#define REGION_USE_IMPROVED

#define RECTS_BUF_SIZE 4

typedef struct QRegion {
    uint32_t num_rects;
    SpiceRect bbox;
    SpiceRect *rects;
    uint32_t rects_size;
    SpiceRect buf[RECTS_BUF_SIZE];
} QRegion;

#ifdef REGION_USE_IMPROVED

#define REGION_TEST_LEFT_EXCLUSIVE (1 << 0)
#define REGION_TEST_RIGHT_EXCLUSIVE (1 << 1)
#define REGION_TEST_SHARED (1 << 2)
#define REGION_TEST_ALL \
    (REGION_TEST_LEFT_EXCLUSIVE | REGION_TEST_RIGHT_EXCLUSIVE | REGION_TEST_SHARED)

#endif

void region_init(QRegion *rgn);
void region_clear(QRegion *rgn);
void region_destroy(QRegion *rgn);
void region_clone(QRegion *dest, const QRegion *src);

#ifdef REGION_USE_IMPROVED
int region_test(const QRegion *rgn, const QRegion *other_rgn, int query);
#endif
int region_is_valid(const QRegion *rgn);
int region_is_empty(const QRegion *rgn);
int region_is_equal(const QRegion *rgn1, const QRegion *rgn2);
int region_intersects(const QRegion *rgn1, const QRegion *rgn2);
int region_bounds_intersects(const QRegion *rgn1, const QRegion *rgn2);
int region_contains(const QRegion *rgn, const QRegion *other);
int region_contains_point(const QRegion *rgn, int32_t x, int32_t y);

void region_or(QRegion *rgn, const QRegion *other_rgn);
void region_and(QRegion *rgn, const QRegion *other_rgn);
void region_xor(QRegion *rgn, const QRegion *other_rgn);
void region_exclude(QRegion *rgn, const QRegion *other_rgn);

void region_add(QRegion *rgn, const SpiceRect *r);
void region_remove(QRegion *rgn, const SpiceRect *r);

void region_offset(QRegion *rgn, int32_t dx, int32_t dy);

void region_dump(const QRegion *rgn, const char *prefix);

#endif


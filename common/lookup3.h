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

#ifndef __LOOKUP3_H
#define __LOOKUP3_H

#ifdef __GNUC__

#include <stdint.h>

#else

#ifdef QXLDD
#include <windef.h>
#include "os_dep.h"
#else
#include <stddef.h>
#include <basetsd.h>
#endif

typedef UINT32 uint32_t;
typedef UINT16 uint16_t;
typedef UINT8 uint8_t;

#endif

uint32_t hashlittle(const void *key, size_t length, uint32_t initval);

#endif

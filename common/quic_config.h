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

#ifndef __QUIC_CONFIG_H
#define __QUIC_CONFIG_H

#include <spice/types.h>

#ifdef __GNUC__

#include <string.h>

#define INLINE inline

#define MEMCLEAR(ptr, size) memset(ptr, 0, size)

#else

#ifdef QXLDD
#include <windef.h>
#include "os_dep.h"
#define INLINE _inline
#define MEMCLEAR(ptr, size) RtlZeroMemory(ptr, size)
#else
#include <stddef.h>
#include <string.h>

#define INLINE inline
#define MEMCLEAR(ptr, size) memset(ptr, 0, size)
#endif


#endif

#endif


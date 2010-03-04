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

#ifndef _H_SPICE
#define _H_SPICE

#include "vd_interface.h"

/* old interface */
extern const char *spice_usage_str[];

int spice_parse_args(const char *args);
void spice_init(CoreInterface *core);

/* new interface */
typedef struct RedsState SpiceServer;
SpiceServer *spice_server_new(void);
int spice_server_init(SpiceServer *s, CoreInterface *core);
void spice_server_destroy(SpiceServer *s);

#endif

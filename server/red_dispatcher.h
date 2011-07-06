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

#ifndef _H_RED_DISPATCHER
#define _H_RED_DISPATCHER

struct RedDispatcher *red_dispatcher_init(QXLInstance *qxl);

void red_dispatcher_set_mm_time(uint32_t);
void red_dispatcher_on_ic_change(void);
void red_dispatcher_on_sv_change(void);
void red_dispatcher_set_mouse_mode(uint32_t mode);
int red_dispatcher_count(void);
int red_dispatcher_add_renderer(const char *name);
uint32_t red_dispatcher_qxl_ram_size(void);
int red_dispatcher_qxl_count(void);
void red_dispatcher_async_complete(struct RedDispatcher*, uint64_t);

#endif


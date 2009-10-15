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


    Author:
        yhalperi@redhat.com
*/

#ifndef _H_RED_TUNNEL_WORKER
#define _H_RED_TUNNEL_WORKER

#include "vd_interface.h"

void *red_tunnel_attach(CoreInterface *core_interface, NetWireInterface *vlan_interface);

#endif

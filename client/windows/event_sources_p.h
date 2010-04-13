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

#ifndef _H_EVENT_SOURCES_P
#define _H_EVENT_SOURCES_P

#include "common.h"

#include <vector>

class EventSource;
class Handle_p;

class EventSources_p {
protected:
    /* return true if quit should be performed */
    bool process_system_events();
    void add_event(HANDLE event, EventSource* source);
    void remove_event(EventSource* source);
public:
    std::vector<EventSource*> _events;
    std::vector<HANDLE> _handles;
};

class Handle_p {
public:
    Handle_p();
    virtual ~Handle_p();
    HANDLE get_handle() { return _event;}
protected:
    HANDLE _event;
};

class Trigger_p: public Handle_p {
};

#endif


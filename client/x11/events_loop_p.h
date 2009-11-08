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

#ifndef _H_EVENTS_LOOP_P
#define _H_EVENTS_LOOP_P

#include "common.h"
#include "threads.h"

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 8)
#define USING_EVENT_FD
#endif

#define INFINITE -1

class EventWrapper;

class EventsLoop_p {
public:
    class Trigger_p;
    void remove_wrapper(EventWrapper*);

public:
    int _epoll;
    typedef std::list<EventWrapper*> Events;
    Events _events;

    friend class EventWrapper;
};

class EventsLoop_p::Trigger_p {
public:
    Trigger_p() : _pending_int (false) {}
    int get_fd() { return _event_fd;}
    bool reset_event();

public:
    int _event_fd;
#ifndef USING_EVENT_FD
    int _event_write_fd;
#endif
    bool _pending_int;
    Mutex _lock;
};

#endif


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

#ifndef _H_EVENTS_LOOP
#define _H_EVENTS_LOOP

#include "common.h"
#include "events_loop_p.h"

class EventSource;

class EventsLoop: public EventsLoop_p {
public:
    class Trigger;
    class Socket;
    class File;

    EventsLoop();
    virtual ~EventsLoop();

    void add_trigger(Trigger& trigger);
    void remove_trigger(Trigger& trigger);
    void add_socket(Socket& socket);
    void remove_socket(Socket& socket);
    void add_file(File& file);
    void remove_file(File& file);
    void run();
    // FIXME: temporary - need to adjust the loop for the main thread
    void run_once(int timeout_milli = INFINITE);
};

class EventSource {
public:
    virtual ~EventSource() {}
    virtual void on_event() = 0;

private:
    virtual void action() {on_event();}

    friend class EventsLoop;
};

class EventsLoop::Trigger: public EventSource, private Trigger_p {
public:
    Trigger();
    virtual ~Trigger();
    virtual void trigger();
    virtual void reset();

private:
    virtual void action();

    friend class EventsLoop;
};

class EventsLoop::Socket: public EventSource {
protected:
    virtual int get_socket() = 0;

    friend class EventsLoop;
};


class EventsLoop::File: public EventSource {
protected:
    virtual int get_fd() = 0;

    friend class EventsLoop;
};

#endif


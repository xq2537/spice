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

#include "common.h"
#include "events_loop.h"
#include "debug.h"
#include "utils.h"


EventsLoop::EventsLoop()
{
}

EventsLoop::~EventsLoop()
{
}

void EventsLoop::run()
{
    for (;;) {
        run_once();
    }
}

void EventsLoop::run_once(int timeout_milli)
{
    DWORD wait_res = WaitForMultipleObjects(_handles.size(), &_handles[0], FALSE, timeout_milli);
    if (wait_res == WAIT_FAILED) {
        THROW("wait failed");
    }
    int event_index = wait_res - WAIT_OBJECT_0;
    if (event_index < 0 || event_index >= (int)_events.size()) {
        THROW("invalid event id");
    }
    _events[event_index]->action();
}

void EventsLoop::add_socket(Socket& socket)
{
    HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!event) {
        THROW("create event failed");
    }
    if (WSAEventSelect(socket.get_socket(), event, FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        CloseHandle(event);
        THROW("event select failed");
    }
    int size = _events.size();
    _events.resize(size + 1);
    _handles.resize(size + 1);
    _events[size] = &socket;
    _handles[size] = event;
}

void EventsLoop::remove_socket(Socket& socket)
{
    int size = _events.size();
    for (int i = 0; i < size; i++) {
        if (_events[i] == &socket) {
            if (WSAEventSelect(socket.get_socket(), NULL, 0) == SOCKET_ERROR) {
                THROW("event select failed");
            }
            u_long arg = 0;
            if (ioctlsocket(socket.get_socket(), FIONBIO, &arg) == SOCKET_ERROR) {
                THROW("set blocking mode failed");
            }
            CloseHandle(_handles[i]);
            for (i++; i < size; i++) {
                _events[i - 1] = _events[i];
                _handles[i - 1] = _handles[i];
            }
            _events.resize(size - 1);
            _handles.resize(size - 1);
            return;
        }
    }
    THROW("socket not found");
}

void EventsLoop::add_trigger(Trigger& trigger)
{
    int size = _events.size();
    _events.resize(size + 1);
    _handles.resize(size + 1);
    _events[size] = &trigger;
    _handles[size] = trigger.get_handle();
}

void EventsLoop::remove_trigger(Trigger& trigger)
{
    int size = _events.size();
    for (int i = 0; i < size; i++) {
        if (_events[i] == &trigger) {
            for (i++; i < size; i++) {
                _events[i - 1] = _events[i];
                _handles[i - 1] = _handles[i];
            }
            _events.resize(size - 1);
            _handles.resize(size - 1);
            return;
        }
    }
    THROW("trigger not found");
}

EventsLoop::Trigger::Trigger()
{
    if (!(event = CreateEvent(NULL, FALSE, FALSE, NULL))) {
        THROW("create event failed");
    }
}

EventsLoop::Trigger::~Trigger()
{
    CloseHandle(event);
}

void EventsLoop::Trigger::trigger()
{
    if (!SetEvent(event)) {
        THROW("set event failed");
    }
}

void EventsLoop::Trigger::reset()
{
    if (!ResetEvent(event)) {
        THROW("set event failed");
    }
}

void EventsLoop::Trigger::action()
{
    on_event();
}

void EventsLoop::add_file(File& file)
{
}

void EventsLoop::remove_file(File& file)
{
}


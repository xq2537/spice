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

#include <sys/epoll.h>
#include <sys/fcntl.h>

#include "events_loop.h"
#include "debug.h"
#include "utils.h"

#ifdef USING_EVENT_FD
#include <sys/eventfd.h>
#endif

#define NUM_EPOLL_EVENTS 10

#ifdef USING_EVENT_FD
#define WRITE_FD _event_fd
#define EVENT_DATA_TYPE eventfd_t
#else
#define WRITE_FD _event_write_fd
#define EVENT_DATA_TYPE uint8_t
#endif

class EventWrapper {
public:
    EventWrapper(EventsLoop& owner, EventSource& event)
        : _owner (owner)
        , _event (&event)
        , _refs (1)
    {
    }

    EventWrapper* ref()
    {
        _refs++;
        return this;
    }

    void unref()
    {
        if (!--_refs) {
            _owner.remove_wrapper(this);
            delete this;
        }
    }

    EventSource* get_event()
    {
        return _event;
    }

    void invalidate()
    {
        _event = NULL;
    }

private:
    EventsLoop& _owner;
    EventSource* _event;
    int _refs;
};

EventsLoop::EventsLoop()
{
    _epoll = epoll_create(NUM_EPOLL_EVENTS);
    if (_epoll == -1) {
        THROW("create epool failed");
    }
}

EventsLoop::~EventsLoop()
{
    Events::iterator iter = _events.begin();
    for (; iter != _events.end(); iter++) {
        delete *iter;
    }
    close(_epoll);
}

void EventsLoop::run()
{
    for (;;) {
        run_once();
    }
}

void EventsLoop::run_once(int timeout_milli)
{
    struct epoll_event events[NUM_EPOLL_EVENTS];

    int num_events = epoll_wait(_epoll, events, NUM_EPOLL_EVENTS, timeout_milli);
    if (num_events == -1) {
        if (errno == EINTR) {
            return;
        }
        THROW("wait error eventfd failed");
    }

    for (int i = 0; i < num_events; i++) {
        ((EventWrapper*)events[i].data.ptr)->ref();
    }
    for (int i = 0; i < num_events; i++) {
        EventWrapper* wrapper;
        EventSource* event;

        wrapper = (EventWrapper *)events[i].data.ptr;
        if ((event = wrapper->get_event())) {
            event->action();
        }
        wrapper->unref();
    }
}

void EventsLoop::add_trigger(Trigger& trigger)
{
    int fd = trigger.get_fd();
    EventWrapper* wrapper = new EventWrapper(*this, trigger);
    struct epoll_event event;
    event.data.ptr = wrapper;
    event.events = EPOLLIN;
    if (epoll_ctl(_epoll, EPOLL_CTL_ADD, fd, &event) == -1) {
        THROW("epoll add failed");
    }
    _events.push_back(wrapper);
}

void EventsLoop_p::remove_wrapper(EventWrapper* wrapper)
{
    Events::iterator iter = _events.begin();
    for (;; iter++) {
        if (iter == _events.end()) {
            THROW("wrapper not found");
        }
        if ((*iter) == wrapper) {
            _events.erase(iter);
            return;
        }
    }
}

void EventsLoop::remove_trigger(Trigger& trigger)
{
    Events::iterator iter = _events.begin();
    for (;; iter++) {
        if (iter == _events.end()) {
            THROW("trigger not found");
        }
        if ((*iter)->get_event() == &trigger) {
            (*iter)->invalidate();
            (*iter)->unref();
            break;
        }
    }
    int fd = trigger.get_fd();
    if (epoll_ctl(_epoll, EPOLL_CTL_DEL, fd, NULL) == -1) {
        THROW("epoll remove failed");
    }
}

EventsLoop::Trigger::Trigger()
{
#ifdef USING_EVENT_FD
    _event_fd = eventfd(0, 0);
    if (_event_fd == -1) {
        THROW("create eventfd failed");
    }
#else
    int fd[2];
    if (pipe(fd) == -1) {
        THROW("create pipe failed");
    }
    _event_fd = fd[0];
    _event_write_fd = fd[1];
#endif
    int flags;
    if ((flags = fcntl(_event_fd, F_GETFL)) == -1) {
        THROW("failed to set eventfd non block: %s", strerror(errno));
    }

    if (fcntl(_event_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        THROW("failed to set eventfd non block: %s", strerror(errno));
    }
}

EventsLoop::Trigger::~Trigger()
{
    close(_event_fd);
#ifndef USING_EVENT_FD
    close(_event_write_fd);
#endif
}

void EventsLoop::Trigger::trigger()
{
    Lock lock(_lock);
    if (_pending_int) {
        return;
    }
    _pending_int = true;
    static const EVENT_DATA_TYPE val = 1;
    if (::write(WRITE_FD, &val, sizeof(val)) != sizeof(val)) {
        THROW("write event failed");
    }
}

bool Trigger_p::reset_event()
{
    Lock lock(_lock);
    if (!_pending_int) {
        return false;
    }
    EVENT_DATA_TYPE val;
    if (read(_event_fd, &val, sizeof(val)) != sizeof(val)) {
        THROW("event read error");
    }
    _pending_int = false;
    return true;
}

void EventsLoop::Trigger::reset()
{
    reset_event();
}

void EventsLoop::Trigger::action()
{
    if (reset_event()) {
        on_event();
    }
}

static void set_non_blocking(int fd)
{
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        THROW("failed to set socket non block: %s", strerror(errno));
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        THROW("failed to set socket non block: %s", strerror(errno));
    }
}

static void set_blocking(int fd)
{
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        THROW("failed to clear socket non block: %s", strerror(errno));
    }

    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        THROW("failed to clear socket non block: %s", strerror(errno));
    }
}

static void add_to_poll(int fd, int epoll, EventWrapper* wrapper)
{
    struct epoll_event event;
    event.data.ptr = wrapper;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    if (epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event) == -1) {
        THROW("epoll add failed");
    }
}

void EventsLoop::add_socket(Socket& socket)
{
    int fd = socket.get_socket();
    set_non_blocking(fd);
    EventWrapper* wrapper = new EventWrapper(*this, socket);
    add_to_poll(fd, _epoll, wrapper);
    _events.push_back(wrapper);
}

static bool remove_event(EventsLoop_p::Events& events, EventSource& event)
{
    EventsLoop_p::Events::iterator iter = events.begin();
    for (;; iter++) {
        if (iter == events.end()) {
            return false;
        }
        if ((*iter)->get_event() == &event) {
            (*iter)->invalidate();
            (*iter)->unref();
            return true;
        }
    }
}

void EventsLoop::remove_socket(Socket& socket)
{
    if (!remove_event(_events, socket)) {
        THROW("socket not found");
    }
    int fd = socket.get_socket();
    if (epoll_ctl(_epoll, EPOLL_CTL_DEL, fd, NULL) == -1) {
        THROW("epoll remove failed");
    }
    set_blocking(fd);
}

void EventsLoop::add_file(File& file)
{
    int fd = file.get_fd();
    set_non_blocking(fd);
    EventWrapper* wrapper = new EventWrapper(*this, file);
    add_to_poll(fd, _epoll, wrapper);
    _events.push_back(wrapper);
}

void EventsLoop::remove_file(File& file)
{
    if (!remove_event(_events, file)) {
        THROW("file not found");
    }
    int fd = file.get_fd();
    if (epoll_ctl(_epoll, EPOLL_CTL_DEL, fd, NULL) == -1) {
        THROW("epoll remove failed");
    }
    set_blocking(fd);
}


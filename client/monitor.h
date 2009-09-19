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

#ifndef _H_MONITOR
#define _H_MONITOR

#include "draw.h"

class Monitor {
public:
    Monitor(int id);

    int get_id() { return _id;}
    bool is_free() { return _free;}
    void set_free() {_free = true;}
    void set_used() {_free = false;}

    virtual void set_mode(int width, int height) = 0;
    virtual void restore() = 0;
    virtual int get_depth() = 0;
    virtual Point get_position() = 0;
    virtual Point get_size() const = 0;
    virtual bool is_out_of_sync() = 0;
    virtual int get_screen_id() = 0;

    static bool is_self_change();

protected:
    virtual ~Monitor() {}

private:
    int _id;
    bool _free;

protected:
    static uint32_t self_monitors_change;

    friend class Platform;
};

#endif


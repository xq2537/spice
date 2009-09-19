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

#ifndef _H_INPUTS_HANDLER
#define _H_INPUTS_HANDLER


class InputsHandler {
public:
    virtual ~InputsHandler() {}
    virtual void on_mouse_motion(int dx, int dy, int buttons_state) {}
    virtual void on_mouse_position(int x, int y, int buttons_state, int display_id) {}
    virtual void on_mouse_down(int button, int buttons_state) {}
    virtual void on_mouse_up(int button, int buttons_state) {}
    virtual void on_key_down(uint32_t scan_code) {}
    virtual void on_key_up(uint32_t scan_code) {}
    virtual void on_focus_in() {}
    virtual void on_focus_out() {}
};

#endif


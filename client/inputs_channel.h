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

#ifndef _H_INPUTS_CHANNEL
#define _H_INPUTS_CHANNEL

#include "red_channel.h"
#include "inputs_handler.h"

class ChannelFactory;

class InputsChannel: public RedChannel, public KeyHandler, public MouseHandler {
public:
    InputsChannel(RedClient& client, uint32_t id);
    virtual ~InputsChannel();

    virtual void on_mouse_motion(int dx, int dy, int buttons_state);
    virtual void on_mouse_down(int button, int buttons_state);
    virtual void on_mouse_up(int button, int buttons_state);
    virtual void on_key_down(uint32_t scan_code);
    virtual void on_key_up(uint32_t scan_code);
    virtual void on_focus_in();

    void on_mouse_position(int x, int y, int buttons_state, int display_id);

    static ChannelFactory& Factory();

protected:
    virtual void on_connect();
    virtual void on_disconnect();
    virtual void on_migrate();

private:
    void set_motion_event(RedcMouseMotion& motion_event);
    void set_position_event(RedcMousePosition& position_event);
    void set_local_modifiers();

    void handle_init(RedPeer::InMessage* message);
    void handle_modifaiers(RedPeer::InMessage* message);
    void handle_motion_ack(RedPeer::InMessage* message);

private:
    Mutex _motion_lock;
    int _mouse_buttons_state;
    int _mouse_dx;
    int _mouse_dy;
    unsigned int _mouse_x;
    unsigned int _mouse_y;
    int _display_id;
    bool _active_motion;
    int _motion_count;
    uint32_t _modifiers;
    Mutex _update_modifiers_lock;
    bool _active_modifiers_event;

    friend class MotionMessage;
    friend class PositionMessage;
    friend class KeyModifiersEvent;
    friend class SetInputsHandlerEvent;
    friend class RemoveInputsHandlerEvent;
};


#endif


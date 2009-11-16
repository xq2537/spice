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
#include "inputs_channel.h"
#include "utils.h"
#include "debug.h"
#include "red_client.h"
#include "application.h"
#include "display_channel.h"

#define SYNC_REMOTH_MODIFIRES

class SetInputsHandlerEvent: public Event {
public:
    SetInputsHandlerEvent(InputsChannel& channel) : _channel (channel) {}

    class AttachFunc: public ForEachChannelFunc {
    public:
        AttachFunc(InputsChannel& channel)
            : _channel (channel)
        {
        }

        virtual bool operator() (RedChannel& channel)
        {
            if (channel.get_type() == RED_CHANNEL_DISPLAY) {
                static_cast<DisplayChannel&>(channel).attach_inputs(&_channel);
            }
            return true;
        }

    public:
        InputsChannel& _channel;
    };

    virtual void response(AbstractProcessLoop& events_loop)
    {
        static_cast<Application*>(events_loop.get_owner())->set_key_handler(_channel);
        static_cast<Application*>(events_loop.get_owner())->set_mouse_handler(_channel);
        AttachFunc func(_channel);
        _channel.get_client().for_each_channel(func);
    }

private:
    InputsChannel& _channel;
};

class KeyModifiersEvent: public Event {
public:
    KeyModifiersEvent(InputsChannel& channel) : _channel (channel) {}

    virtual void response(AbstractProcessLoop& events_loop)
    {
        Lock lock(_channel._update_modifiers_lock);
        _channel._active_modifiers_event = false;
        _channel.set_local_modifiers();
    }

private:
    InputsChannel& _channel;
};

class RemoveInputsHandlerEvent: public SyncEvent {
public:
    RemoveInputsHandlerEvent(InputsChannel& channel) : _channel (channel) {}

    class DetachFunc: public ForEachChannelFunc {
    public:
        virtual bool operator() (RedChannel& channel)
        {
            if (channel.get_type() == RED_CHANNEL_DISPLAY) {
                static_cast<DisplayChannel&>(channel).detach_inputs();
            }
            return true;
        }
    };

    virtual void do_response(AbstractProcessLoop& events_loop)
    {
        static_cast<Application*>(events_loop.get_owner())->remove_key_handler(_channel);
        static_cast<Application*>(events_loop.get_owner())->remove_mouse_handler(_channel);
        DetachFunc detach_func;
        _channel.get_client().for_each_channel(detach_func);
    }

private:
    InputsChannel& _channel;
};

class MotionMessage: public RedChannel::OutMessage, public RedPeer::OutMessage {
public:
    MotionMessage(InputsChannel& channel);
    virtual RedPeer::OutMessage& peer_message();
    virtual void release();

private:
    InputsChannel& _channel;
};

MotionMessage::MotionMessage(InputsChannel& channel)
    : RedChannel::OutMessage()
    , RedPeer::OutMessage(REDC_INPUTS_MOUSE_MOTION, sizeof(RedcMouseMotion))
    , _channel (channel)
{
}

void MotionMessage::release()
{
    delete this;
}

RedPeer::OutMessage& MotionMessage::peer_message()
{
    _channel.set_motion_event(*(RedcMouseMotion*)data());
    return *this;
}

class PositionMessage: public RedChannel::OutMessage, public RedPeer::OutMessage {
public:
    PositionMessage(InputsChannel& channel);
    virtual RedPeer::OutMessage& peer_message();
    virtual void release();

private:
    InputsChannel& _channel;
};

PositionMessage::PositionMessage(InputsChannel& channel)
    : RedChannel::OutMessage()
    , RedPeer::OutMessage(REDC_INPUTS_MOUSE_POSITION, sizeof(RedcMousePosition))
    , _channel (channel)
{
}

void PositionMessage::release()
{
    delete this;
}

RedPeer::OutMessage& PositionMessage::peer_message()
{
    _channel.set_position_event(*(RedcMousePosition*)data());
    return *this;
}

class InputsMessHandler: public MessageHandlerImp<InputsChannel, RED_INPUTS_MESSAGES_END> {
public:
    InputsMessHandler(InputsChannel& channel)
        : MessageHandlerImp<InputsChannel, RED_INPUTS_MESSAGES_END>(channel) {}
};

InputsChannel::InputsChannel(RedClient& client, uint32_t id)
    : RedChannel(client, RED_CHANNEL_INPUTS, id, new InputsMessHandler(*this))
    , _mouse_buttons_state (0)
    , _mouse_dx (0)
    , _mouse_dy (0)
    , _mouse_x (~0)
    , _mouse_y (~0)
    , _display_id (-1)
    , _active_motion (false)
    , _motion_count (0)
    , _active_modifiers_event (false)
{
    InputsMessHandler* handler = static_cast<InputsMessHandler*>(get_message_handler());
    handler->set_handler(RED_MIGRATE, &InputsChannel::handle_migrate, 0);
    handler->set_handler(RED_SET_ACK, &InputsChannel::handle_set_ack, sizeof(RedSetAck));
    handler->set_handler(RED_PING, &InputsChannel::handle_ping, sizeof(RedPing));
    handler->set_handler(RED_WAIT_FOR_CHANNELS, &InputsChannel::handle_wait_for_channels,
                         sizeof(RedWaitForChannels));
    handler->set_handler(RED_DISCONNECTING, &InputsChannel::handle_disconnect,
                         sizeof(RedDisconnect));
    handler->set_handler(RED_NOTIFY, &InputsChannel::handle_notify, sizeof(RedNotify));

    handler->set_handler(RED_INPUTS_INIT, &InputsChannel::handle_init, sizeof(RedInputsInit));
    handler->set_handler(RED_INPUTS_KEY_MODIFAIERS, &InputsChannel::handle_modifaiers,
                         sizeof(RedKeyModifiers));
    handler->set_handler(RED_INPUTS_MOUSE_MOTION_ACK, &InputsChannel::handle_motion_ack, 0);
}

InputsChannel::~InputsChannel()
{
}

void InputsChannel::on_connect()
{
    _motion_count = _mouse_dx = _mouse_dy = _mouse_buttons_state = _modifiers = 0;
    _mouse_x = _mouse_y = ~0;
    _display_id = -1;
}

void InputsChannel::on_disconnect()
{
    AutoRef<RemoveInputsHandlerEvent> remove_handler_event(new RemoveInputsHandlerEvent(*this));
    get_client().push_event(*remove_handler_event);
    (*remove_handler_event)->wait();
}

void InputsChannel::handle_init(RedPeer::InMessage* message)
{
    RedInputsInit* init = (RedInputsInit*)message->data();
    _modifiers = init->keyboard_modifiers;
    AutoRef<SetInputsHandlerEvent> set_handler_event(new SetInputsHandlerEvent(*this));
    get_client().push_event(*set_handler_event);
}

void InputsChannel::handle_modifaiers(RedPeer::InMessage* message)
{
    RedKeyModifiers* init = (RedKeyModifiers*)message->data();
    _modifiers = init->modifiers;
    Lock lock(_update_modifiers_lock);
    if (_active_modifiers_event) {
        return;
    }
    _active_modifiers_event = true;
    AutoRef<KeyModifiersEvent> modifiers_event(new KeyModifiersEvent(*this));
    get_client().push_event(*modifiers_event);
}

void InputsChannel::handle_motion_ack(RedPeer::InMessage* message)
{
    Lock lock(_motion_lock);
    if (_motion_count < RED_MOTION_ACK_BUNCH) {
        LOG_WARN("invalid motion count");
        _motion_count = 0;
    } else {
        _motion_count -= RED_MOTION_ACK_BUNCH;
    }
    if (!_active_motion && (_mouse_dx || _mouse_dy || _display_id != -1)) {
        _active_motion = true;
        _motion_count++;
        switch (get_client().get_mouse_mode()) {
        case RED_MOUSE_MODE_CLIENT:
            post_message(new PositionMessage(*this));
            break;
        case RED_MOUSE_MODE_SERVER:
            post_message(new MotionMessage(*this));
            break;
        default:
            THROW("invalid mouse mode");
        }
    }
}

void InputsChannel::set_motion_event(RedcMouseMotion& motion)
{
    Lock lock(_motion_lock);
    motion.buttons_state = _mouse_buttons_state;
    motion.dx = _mouse_dx;
    motion.dy = _mouse_dy;
    _mouse_dx = _mouse_dy = 0;
    _active_motion = false;
}

void InputsChannel::set_position_event(RedcMousePosition& position)
{
    Lock lock(_motion_lock);
    position.buttons_state = _mouse_buttons_state;
    position.x = _mouse_x;
    position.y = _mouse_y;
    position.display_id = _display_id;
    _mouse_x = _mouse_y = ~0;
    _display_id = -1;
    _active_motion = false;
}

void InputsChannel::on_mouse_motion(int dx, int dy, int buttons_state)
{
    Lock lock(_motion_lock);
    _mouse_buttons_state = buttons_state;
    _mouse_dx += dx;
    _mouse_dy += dy;
    if (!_active_motion && _motion_count < RED_MOTION_ACK_BUNCH * 2) {
        _active_motion = true;
        _motion_count++;
        post_message(new MotionMessage(*this));
    }
}

void InputsChannel::on_mouse_position(int x, int y, int buttons_state, int display_id)
{
    Lock lock(_motion_lock);
    _mouse_buttons_state = buttons_state;
    _mouse_x = x;
    _mouse_y = y;
    _display_id = display_id;
    if (!_active_motion && _motion_count < RED_MOTION_ACK_BUNCH * 2) {
        _active_motion = true;
        _motion_count++;
        post_message(new PositionMessage(*this));
    }
}

void InputsChannel::on_migrate()
{
    _motion_count = _active_motion ? 1 : 0;
}

void InputsChannel::on_mouse_down(int button, int buttons_state)
{
    Message* message;

    message = new Message(REDC_INPUTS_MOUSE_PRESS, sizeof(RedcMouseRelease));
    RedcMousePress* event = (RedcMousePress*)message->data();
    event->button = button;
    event->buttons_state = buttons_state;
    post_message(message);
}

void InputsChannel::on_mouse_up(int button, int buttons_state)
{
    Message* message;

    message = new Message(REDC_INPUTS_MOUSE_RELEASE, sizeof(RedcMouseRelease));
    RedcMouseRelease* event = (RedcMouseRelease*)message->data();
    event->button = button;
    event->buttons_state = buttons_state;
    post_message(message);
}

void InputsChannel::on_key_down(uint32_t scan_code)
{
    Message* message = new Message(REDC_INPUTS_KEY_DOWN, sizeof(RedcKeyDown));
    RedcKeyDown* event = (RedcKeyDown*)message->data();
    event->code = scan_code;
    post_message(message);
}

void InputsChannel::on_key_up(uint32_t scan_code)
{
    Message* message = new Message(REDC_INPUTS_KEY_UP, sizeof(RedcKeyUp));
    RedcKeyUp* event = (RedcKeyUp*)message->data();
    event->code = scan_code;
    post_message(message);
}

void InputsChannel::set_local_modifiers()
{
    unsigned int modifiers = 0;

    if (_modifiers & RED_SCROLL_LOCK_MODIFIER) {
        modifiers |= Platform::SCROLL_LOCK_MODIFIER;
    }

    if (_modifiers & RED_NUM_LOCK_MODIFIER) {
        modifiers |= Platform::NUM_LOCK_MODIFIER;
    }

    if (_modifiers & RED_CAPS_LOCK_MODIFIER) {
        modifiers |= Platform::CAPS_LOCK_MODIFIER;
    }

    Platform::set_keyboard_lock_modifiers(_modifiers);
}

void InputsChannel::on_focus_in()
{
#ifdef SYNC_REMOTH_MODIFIRES
    Message* message = new Message(REDC_INPUTS_KEY_MODIFAIERS, sizeof(RedcKeyDown));
    RedcKeyModifiers* modifiers = (RedcKeyModifiers*)message->data();
    modifiers->modifiers = Platform::get_keyboard_lock_modifiers();
    post_message(message);
#else
    set_local_modifiers();
#endif
}

class InputsFactory: public ChannelFactory {
public:
    InputsFactory() : ChannelFactory(RED_CHANNEL_INPUTS) {}
    virtual RedChannel* construct(RedClient& client, uint32_t id)
    {
        return new InputsChannel(client, id);
    }
};

static InputsFactory factory;

ChannelFactory& InputsChannel::Factory()
{
    return factory;
}


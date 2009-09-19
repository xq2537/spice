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

#ifndef _H_REDCHANNEL
#define _H_REDCHANNEL

#include "common.h"
#include "utils.h"
#include "threads.h"
#include "red_peer.h"
#include "platform.h"
#include "events_loop.h"

enum {
    PASSIVE_STATE,
    DISCONNECTED_STATE,
    CONNECTING_STATE,
    CONNECTED_STATE,
    TERMINATED_STATE,
};

enum {
    WAIT_ACTION,
    CONNECT_ACTION,
    DISCONNECT_ACTION,
    QUIT_ACTION,
};

class RedClient;
class RedChannel;

typedef std::vector<uint32_t> ChannelCaps;

class RedChannelBase: public RedPeer {
public:
    RedChannelBase(uint8_t type, uint8_t id, const ChannelCaps& common_caps,
                   const ChannelCaps& caps);

    virtual ~RedChannelBase();

    uint8_t get_type() { return _type;}
    uint8_t get_id() { return _id;}

    void connect(const ConnectionOptions& options, uint32_t connection_id, uint32_t ip,
                 std::string password);
    void connect(const ConnectionOptions& options, uint32_t connection_id, const char *host,
                 std::string password);

    const ChannelCaps& get_common_caps() { return _common_caps;}
    const ChannelCaps& get_caps() {return _caps;}

protected:
    void set_common_capability(uint32_t cap);
    void set_capability(uint32_t cap);
    bool test_common_capability(uint32_t cap);
    bool test_capability(uint32_t cap);

private:
    void set_capability(ChannelCaps& caps, uint32_t cap);
    bool test_capability(const ChannelCaps& caps, uint32_t cap);
    void link(uint32_t connection_id, const std::string& password);

private:
    uint8_t _type;
    uint8_t _id;

    ChannelCaps _common_caps;
    ChannelCaps _caps;

    ChannelCaps _remote_common_caps;
    ChannelCaps _remote_caps;
};

class SendTrigger: public EventsLoop::Trigger {
public:
    SendTrigger(RedChannel& channel);

    virtual void on_event();

private:
    RedChannel& _channel;
};

class AbortTrigger: public EventsLoop::Trigger {
public:
    virtual void on_event();
};

struct SyncInfo {
    Mutex* lock;
    Condition* condition;
    uint64_t* message_serial;
};

class RedChannel: public RedChannelBase {
public:
    friend class RedCannel;
    class MessageHandler;
    class OutMessage;

    RedChannel(RedClient& client, uint8_t type, uint8_t id, MessageHandler* handler,
               Platform::ThreadPriority worker_priority = Platform::PRIORITY_NORMAL);
    virtual ~RedChannel();
    void start();

    virtual void connect();
    virtual void disconnect();
    virtual bool abort();

    virtual CompundInMessage *recive();

    virtual void post_message(RedChannel::OutMessage* message);
    int get_connection_error() { return _error;}
    Platform::ThreadPriority get_worker_priority() { return _worker_priority;}

protected:
    RedClient& get_client() { return _client;}
    EventsLoop& get_events_loop() { return _loop;}
    MessageHandler* get_message_handler() { return _message_handler.get();}
    virtual void on_connecting() {}
    virtual void on_connect() {}
    virtual void on_disconnect() {}
    virtual void on_migrate() {}
    void handle_migrate(RedPeer::InMessage* message);
    void handle_set_ack(RedPeer::InMessage* message);
    void handle_ping(RedPeer::InMessage* message);
    void handle_wait_for_channels(RedPeer::InMessage* message);
    void handle_disconnect(RedPeer::InMessage* message);
    void handle_notify(RedPeer::InMessage* message);

private:
    void set_state(int state);
    void run();
    void send_migrate_flush_mark();
    void send_messages();
    void recive_messages();
    void on_send_trigger();
    virtual void on_event();
    void on_message_recived();
    void on_message_complition(uint64_t serial);

    static void* worker_main(void *);

    RedChannel::OutMessage* get_outgoing_message();
    void clear_outgoing_messages();

private:
    RedClient& _client;
    int _state;
    int _action;
    int _error;
    bool _wait_for_threads;
    bool _socket_in_loop;

    Thread* _worker;
    Platform::ThreadPriority _worker_priority;
    std::auto_ptr<MessageHandler> _message_handler;
    Mutex _state_lock;
    Condition _state_cond;
    Mutex _action_lock;
    Condition _action_cond;
    SyncInfo _sync_info;

    Mutex _outgoing_lock;
    std::list<RedChannel::OutMessage*> _outgoing_messages;
    RedChannel::OutMessage* _outgoing_message;
    uint32_t _outgoing_pos;

    RedDataHeader _incomming_header;
    uint32_t _incomming_header_pos;
    RedPeer::CompundInMessage* _incomming_message;
    uint32_t _incomming_message_pos;

    uint32_t _message_ack_count;
    uint32_t _message_ack_window;

    EventsLoop _loop;
    SendTrigger _send_trigger;
    AbortTrigger _abort_trigger;

    uint64_t _disconnect_stamp;
    uint64_t _disconnect_reason;

    friend class SendTrigger;
};


class RedChannel::OutMessage {
public:
    OutMessage() {}
    virtual ~OutMessage() {}

    virtual RedPeer::OutMessage& peer_message() = 0;
    virtual void release() = 0;
};

class Message: public RedChannel::OutMessage, public RedPeer::OutMessage {
public:
    Message(uint32_t type, uint32_t size)
        : RedChannel::OutMessage()
        , RedPeer::OutMessage(type, size)
    {
    }

    virtual RedPeer::OutMessage& peer_message() { return *this;}
    virtual void release() {delete this;}
};


class RedChannel::MessageHandler {
public:
    MessageHandler() {}
    virtual ~MessageHandler() {}
    virtual void handle_message(RedPeer::CompundInMessage& message) = 0;
};


template <class HandlerClass, unsigned int end_message>
class MessageHandlerImp: public RedChannel::MessageHandler {
public:
    MessageHandlerImp(HandlerClass& obj);
    virtual ~MessageHandlerImp() {}
    virtual void handle_message(RedPeer::CompundInMessage& message);
    typedef void (HandlerClass::*Handler)(RedPeer::InMessage* message);
    void set_handler(unsigned int id, Handler handler, size_t mess_size);

private:
    HandlerClass& _obj;
    struct HandlerInfo {
        Handler handler;
        size_t mess_size;
    };

    HandlerInfo _handlers[end_message];
};

template <class HandlerClass, unsigned int end_message>
MessageHandlerImp<HandlerClass, end_message>::MessageHandlerImp(HandlerClass& obj)
    : _obj (obj)
{
    memset(_handlers, 0, sizeof(_handlers));
}

template <class HandlerClass, unsigned int end_message>
void MessageHandlerImp<HandlerClass, end_message>::handle_message(RedPeer::CompundInMessage&
                                                                                            message)
{
    if (message.type() >= end_message || !_handlers[message.type()].handler) {
        THROW("bad message type %d", message.type());
    }
    if (message.size() < _handlers[message.type()].mess_size) {
        THROW("bad message size, type %d size %d expected %d",
              message.type(),
              message.size(),
              _handlers[message.type()].mess_size);
    }
    if (message.sub_list()) {
        RedSubMessageList *sub_list;
        sub_list = (RedSubMessageList *)(message.data() + message.sub_list());
        for (int i = 0; i < sub_list->size; i++) {
            RedSubMessage *sub = (RedSubMessage *)(message.data() + sub_list->sub_messages[i]);
            //todo: test size
            RedPeer::InMessage sub_message(sub->type, sub->size, (uint8_t *)(sub + 1));
            (_obj.*_handlers[sub_message.type()].handler)(&sub_message);
        }
    }
    (_obj.*_handlers[message.type()].handler)(&message);
}

template <class HandlerClass, unsigned int end_message>
void MessageHandlerImp<HandlerClass, end_message>::set_handler(unsigned int id, Handler handler,
                                                               size_t mess_size)
{
    if (id >= end_message) {
        THROW("bad handler id");
    }
    _handlers[id].handler = handler;
    _handlers[id].mess_size = mess_size;
}

#endif


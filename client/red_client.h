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

#ifndef _H_REDCLIENT
#define _H_REDCLIENT

#include <list>

#include "common.h"
#include "red_peer.h"
#include "red_channel.h"
#include "display_channel.h"
#include "inputs_channel.h"
#include "cursor_channel.h"
#include "audio_channels.h"
#include "red.h"
#include "vd_agent.h"
#include "process_loop.h"

class Application;

class MigChannel: public RedChannelBase {
public:
    MigChannel(uint32_t type, uint32_t id, const ChannelCaps& common_caps, const ChannelCaps& caps)
        : RedChannelBase(type, id, common_caps, caps)
        , _valid(false) {}
    bool is_valid() { return _valid;}
    void set_valid(bool val) { _valid = val;}

private:
    bool _valid;
};

class Migrate {
public:
    Migrate(RedClient& client);
    ~Migrate();

    void start(const RedMigrationBegin* migrate);
    bool abort();
    void add_channel(MigChannel* channel);
    void clear_channels();
    void swap_peer(RedChannelBase& other);

private:
    void connect_one(MigChannel& channel, const RedPeer::ConnectionOptions& options,
                     uint32_t connection_id);
    void disconnect_channels();
    void close_channels();
    void delete_channels();
    void run();
    static void* worker_main(void *data);

private:
    RedClient& _client;
    typedef std::list<MigChannel*> MigChannels;
    MigChannels _channels;
    bool _running;
    bool _aborting;
    bool _connected;
    std::string _password;
    std::string _host;
    int _port;
    int _sport;
    Thread* _thread;
    Mutex _lock;
    Condition _cond;
    int _pending_con;
};

class ChannelFactory {
public:
    ChannelFactory(uint32_t type) : _type (type) {}
    virtual ~ChannelFactory() {}

    uint32_t type() { return _type;}
    virtual RedChannel* construct(RedClient& client, uint32_t id) = 0;

private:
    uint32_t _type;
};

class GlzDecoderWindowDebug: public GlzDecoderDebug {
public:
    virtual void error(const std::string& str)
    {
        throw Exception(str);
    }

    virtual void warn(const std::string& str)
    {
        LOG_WARN("%s", str.c_str());
    }

    virtual void info(const std::string& str)
    {
        LOG_INFO("%s", str.c_str());
    }
};

class AgentTimer: public Timer {
    virtual void response(AbstractProcessLoop& events_loop);
};

typedef std::map< int, RedPeer::ConnectionOptions::Type> PeerConnectionOptMap;

class RedClient: public RedChannel {
public:
    friend class RedChannel;
    friend class Migrate;

    RedClient(Application& application);
    ~RedClient();

    void init(const char* host, int port, int sport, const char *password, bool auto_display_res);

    void register_channel_factory(ChannelFactory& factory);

    virtual void connect();
    virtual void disconnect();
    virtual bool abort();

    void push_event(Event* event);
    void activate_interval_timer(Timer* timer, unsigned int millisec);
    void deactivate_interval_timer(Timer* timer);

    void set_target(const char* host, uint16_t port, uint16_t sport);
    const char* get_password() { return _password.c_str();}
    const char* get_host() { return _host.c_str();}
    int get_port() { return _port;}
    int get_sport() { return _sport;}
    virtual uint32_t get_connection_id() { return _connection_id;}
    uint32_t get_mouse_mode() { return _mouse_mode;}
    Application& get_application() { return _application;}
    bool is_auto_display_res() { return _auto_display_res;}
    RedPeer::ConnectionOptions::Type get_connection_options(uint32_t channel_type);
    void get_sync_info(uint8_t channel_type, uint8_t channel_id, SyncInfo& info);
    void wait_for_channels(int wait_list_size, RedWaitForChannel* wait_list);
    PixmapCache& get_pixmap_cache() {return _pixmap_cache;}
    uint64_t get_pixmap_cache_size() { return _pixmap_cache_size;}
    void on_display_mode_change();

    GlzDecoderWindow& get_glz_window() {return _glz_window;}
    int get_glz_window_size() { return _glz_window_size;}

    void set_mm_time(uint32_t time);
    uint32_t get_mm_time();

protected:
    virtual void on_connecting();
    virtual void on_connect();
    virtual void on_disconnect();

private:
    void on_channel_disconnected(RedChannel& channel);
    void migrate_channel(RedChannel& channel);
    void send_agent_monitors_config();
    void calc_pixmap_cach_and_glz_window_size(uint32_t display_channels_hint,
                                              uint32_t pci_mem_hint);
    void set_mouse_mode(uint32_t supported_modes, uint32_t current_mode);

    void handle_migrate_begin(RedPeer::InMessage* message);
    void handle_migrate_cancel(RedPeer::InMessage* message);
    void handle_init(RedPeer::InMessage* message);
    void handle_channels(RedPeer::InMessage* message);
    void handle_mouse_mode(RedPeer::InMessage* message);
    void handle_mm_time(RedPeer::InMessage* message);
    void handle_agent_connected(RedPeer::InMessage* message);
    void handle_agent_disconnected(RedPeer::InMessage* message);
    void handle_agent_data(RedPeer::InMessage* message);
    void handle_agent_tokens(RedPeer::InMessage* message);

    void on_agent_reply(VDAgentReply* reply);

    ChannelFactory* find_factory(uint32_t type);
    void create_channel(uint32_t type, uint32_t id);
    void disconnect_channels();
    void delete_channels();
    bool abort_channels();

private:
    Application& _application;

    std::string _password;
    std::string _host;
    int _port;
    int _sport;
    uint32_t _connection_id;
    uint32_t _mouse_mode;
    Mutex _notify_lock;
    bool _notify_disconnect;
    bool _auto_display_res;
    bool _aborting;

    bool _agent_connected;
    bool _agent_mon_config_sent;
    VDAgentMessage* _agent_msg;
    uint8_t* _agent_msg_data;
    uint32_t _agent_msg_pos;
    uint32_t _agent_tokens;
    AutoRef<AgentTimer> _agent_timer;

    PeerConnectionOptMap _con_opt_map;
    Migrate _migrate;
    Mutex _channels_lock;
    typedef std::list<ChannelFactory*> Factorys;
    Factorys _factorys;
    typedef std::list<RedChannel*> Channels;
    Channels _channels;
    PixmapCache _pixmap_cache;
    uint64_t _pixmap_cache_size;
    Mutex _sync_lock;
    Condition _sync_condition;
    uint64_t _sync_info[RED_CHANNEL_END][256];

    GlzDecoderWindowDebug _glz_debug;
    GlzDecoderWindow _glz_window;
    int _glz_window_size; // in pixels

    Mutex _mm_clock_lock;
    uint64_t _mm_clock_last_update;
    uint32_t _mm_time;
};

#endif


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
#include <math.h>
#include "red_client.h"
#include "application.h"
#include "process_loop.h"
#include "utils.h"
#include "debug.h"

Migrate::Migrate(RedClient& client)
    : _client (client)
    , _running (false)
    , _aborting (false)
    , _connected (false)
    , _thread (NULL)
    , _pending_con (0)
{
}

Migrate::~Migrate()
{
    ASSERT(!_thread);
    delete_channels();
}

void Migrate::delete_channels()
{
    while (!_channels.empty()) {
        MigChannels::iterator iter = _channels.begin();
        delete *iter;
        _channels.erase(iter);
    }
}

void Migrate::clear_channels()
{
    Lock lock(_lock);
    ASSERT(!_running);
    delete_channels();
}

void Migrate::add_channel(MigChannel* channel)
{
    Lock lock(_lock);
    _channels.push_back(channel);
}

void Migrate::swap_peer(RedChannelBase& other)
{
    DBG(0, "channel type %u id %u", other.get_type(), other.get_id());
    try {
        Lock lock(_lock);
        MigChannels::iterator iter = _channels.begin();

        if (_running) {
            THROW("swap and running");
        }

        if (!_connected) {
            THROW("not connected");
        }

        for (; iter != _channels.end(); ++iter) {
            MigChannel* curr = *iter;
            if (curr->get_type() == other.get_type() && curr->get_id() == other.get_id()) {
                if (!curr->is_valid()) {
                    THROW("invalid");
                }
                other.swap(curr);
                curr->set_valid(false);
                if (!--_pending_con) {
                    lock.unlock();
                    _client.set_target(_host.c_str(), _port, _sport);
                    abort();
                }
                return;
            }
        }
        THROW("no channel");
    } catch (...) {
        abort();
        throw;
    }
}

void Migrate::connect_one(MigChannel& channel, const RedPeer::ConnectionOptions& options,
                          uint32_t connection_id)
{
    if (_aborting) {
        DBG(0, "aborting");
        THROW("aborting");
    }
    channel.connect(options, connection_id, _host.c_str(), _password);
    ++_pending_con;
    channel.set_valid(true);
}

void Migrate::run()
{
    uint32_t connection_id;

    DBG(0, "");
    try {
        RedPeer::ConnectionOptions con_opt(_client.get_connection_options(RED_CHANNEL_MAIN),
                                           _port, _port);
        MigChannels::iterator iter = _channels.begin();
        connection_id = _client.get_connection_id();
        connect_one(**iter, con_opt, connection_id);
        for (++iter; iter != _channels.end(); ++iter) {
            con_opt = RedPeer::ConnectionOptions(
                                                _client.get_connection_options((*iter)->get_type()),
                                                _port, _sport);
            connect_one(**iter, con_opt, connection_id);
        }
        _connected = true;
        DBG(0, "connected");
    } catch (...) {
        close_channels();
    }

    Lock lock(_lock);
    _cond.notify_one();
    if (_connected) {
        Message* message = new Message(REDC_MIGRATE_CONNECTED, 0);
        _client.post_message(message);
    } else {
        Message* message = new Message(REDC_MIGRATE_CONNECT_ERROR, 0);
        _client.post_message(message);
    }
    _running = false;
}

void* Migrate::worker_main(void *data)
{
    Migrate* mig = (Migrate*)data;
    mig->run();
    return NULL;
}

void Migrate::start(const RedMigrationBegin* migrate)
{
    DBG(0, "");
    abort();
    _host.assign(migrate->host);
    _port = migrate->port ? migrate->port : -1;
    _sport = migrate->sport ? migrate->sport : -1;
    _password = _client._password;
    Lock lock(_lock);
    _running = true;
    lock.unlock();
    _thread = new Thread(Migrate::worker_main, this);
}

void Migrate::disconnect_channels()
{
    MigChannels::iterator iter = _channels.begin();

    for (; iter != _channels.end(); ++iter) {
        (*iter)->disconnect();
        (*iter)->set_valid(false);
    }
}

void Migrate::close_channels()
{
    MigChannels::iterator iter = _channels.begin();

    for (; iter != _channels.end(); ++iter) {
        (*iter)->close();
        (*iter)->set_valid(false);
        (*iter)->enable();
    }
}

bool Migrate::abort()
{
    Lock lock(_lock);
    if (_aborting) {
        return false;
    }
    _aborting = true;
    for (;;) {
        disconnect_channels();
        if (!_running) {
            break;
        }
        uint64_t timout = 1000 * 1000 * 10; /*10ms*/
        _cond.timed_wait(lock, timout);
    }
    close_channels();
    _pending_con = 0;
    _connected = false;
    _aborting = false;
    if (_thread) {
        _thread->join();
        delete _thread;
        _thread = NULL;
    }
    return true;
}

#define AGENT_TIMEOUT (1000 * 30)

void AgentTimer::response(AbstractProcessLoop& events_loop)
{
    Application* app = static_cast<Application*>(events_loop.get_owner());
    app->deactivate_interval_timer(this);
    THROW_ERR(SPICEC_ERROR_CODE_AGENT_TIMEOUT, "vdagent timeout");
}

class MainChannelLoop: public MessageHandlerImp<RedClient, RED_MESSAGES_END> {
public:
    MainChannelLoop(RedClient& client): MessageHandlerImp<RedClient, RED_MESSAGES_END>(client) {}
};

RedClient::RedClient(Application& application)
    : RedChannel(*this, RED_CHANNEL_MAIN, 0, new MainChannelLoop(*this))
    , _application (application)
    , _connection_id (0)
    , _mouse_mode (RED_MOUSE_MODE_SERVER)
    , _notify_disconnect (false)
    , _aborting (false)
    , _agent_connected (false)
    , _agent_mon_config_sent (false)
    , _agent_msg (new VDAgentMessage)
    , _agent_msg_data (NULL)
    , _agent_msg_pos (0)
    , _agent_tokens (0)
    , _agent_timer (new AgentTimer())
    , _migrate (*this)
    , _glz_window (0, _glz_debug)
{
    MainChannelLoop* message_loop = static_cast<MainChannelLoop*>(get_message_handler());

    message_loop->set_handler(RED_MIGRATE, &RedClient::handle_migrate, 0);
    message_loop->set_handler(RED_SET_ACK, &RedClient::handle_set_ack, sizeof(RedSetAck));
    message_loop->set_handler(RED_PING, &RedClient::handle_ping, sizeof(RedPing));
    message_loop->set_handler(RED_WAIT_FOR_CHANNELS, &RedClient::handle_wait_for_channels,
                              sizeof(RedWaitForChannels));
    message_loop->set_handler(RED_DISCONNECTING, &RedClient::handle_disconnect,
                              sizeof(RedDisconnect));
    message_loop->set_handler(RED_NOTIFY, &RedClient::handle_notify, sizeof(RedNotify));

    message_loop->set_handler(RED_MIGRATE_BEGIN, &RedClient::handle_migrate_begin,
                              sizeof(RedMigrationBegin));
    message_loop->set_handler(RED_MIGRATE_CANCEL, &RedClient::handle_migrate_cancel, 0);
    message_loop->set_handler(RED_INIT, &RedClient::handle_init, sizeof(RedInit));
    message_loop->set_handler(RED_CHANNELS_LIST, &RedClient::handle_channels,
                              sizeof(RedChannels));
    message_loop->set_handler(RED_MOUSE_MODE, &RedClient::handle_mouse_mode,
                              sizeof(RedMouseMode));
    message_loop->set_handler(RED_MULTI_MEDIA_TIME, &RedClient::handle_mm_time,
                              sizeof(RedMultiMediaTime));

    message_loop->set_handler(RED_AGENT_CONNECTED, &RedClient::handle_agent_connected, 0);
    message_loop->set_handler(RED_AGENT_DISCONNECTED, &RedClient::handle_agent_disconnected,
                              sizeof(RedAgentDisconnect));
    message_loop->set_handler(RED_AGENT_DATA, &RedClient::handle_agent_data, 0);
    message_loop->set_handler(RED_AGENT_TOKEN, &RedClient::handle_agent_tokens,
                              sizeof(RedAgentTokens));
    start();
}

RedClient::~RedClient()
{
    ASSERT(_channels.empty());
    _application.deactivate_interval_timer(*_agent_timer);
    delete _agent_msg;
}

void RedClient::init(const char* host, int port, int sport, const char *password,
                     bool auto_display_res)
{
    _host = host;
    _port = port;
    _sport = sport;
    _auto_display_res = auto_display_res;

    if (password != NULL) {
        _password = password;
    } else {
        _password = "";
    }
}

void RedClient::set_target(const char* host, uint16_t port, uint16_t sport)
{
    _port = port;
    _sport = sport;
    _host.assign(host);
}

void RedClient::push_event(Event* event)
{
    _application.push_event(event);
}

void RedClient::activate_interval_timer(Timer* timer, unsigned int millisec)
{
    _application.activate_interval_timer(timer, millisec);
}

void RedClient::deactivate_interval_timer(Timer* timer)
{
    _application.deactivate_interval_timer(timer);
}

void RedClient::on_connecting()
{
    _notify_disconnect = true;
}

void RedClient::on_connect()
{
    AutoRef<ConnectedEvent> event(new ConnectedEvent());
    push_event(*event);
    _migrate.add_channel(new MigChannel(RED_CHANNEL_MAIN, 0, get_common_caps(),
                                        get_caps()));
}

void RedClient::on_disconnect()
{
    _migrate.abort();
    _connection_id = 0;
    _application.deactivate_interval_timer(*_agent_timer);
    _agent_mon_config_sent = false;
    delete[] _agent_msg_data;
    _agent_msg_data = NULL;
    _agent_msg_pos = 0;
    _agent_tokens = 0;
}

void RedClient::delete_channels()
{
    Lock lock(_channels_lock);
    Channels::iterator iter = _channels.begin();
    while (!_channels.empty()) {
        RedChannel *channel = *_channels.begin();
        _channels.pop_front();
        delete channel;
    }
}

RedPeer::ConnectionOptions::Type RedClient::get_connection_options(uint32_t channel_type)
{
    return _con_opt_map[channel_type];
}

void RedClient::connect()
{
    //todo wait for disconnect state
    if (_connection_id || !abort_channels()) {
        return;
    }
    _pixmap_cache.clear();
    _glz_window.clear();
    memset(_sync_info, 0, sizeof(_sync_info));
    _aborting = false;
    _migrate.clear_channels();
    delete_channels();
    enable();

    _con_opt_map.clear();
    PeerConnectionOptMap::const_iterator iter = _application.get_con_opt_map().begin();
    PeerConnectionOptMap::const_iterator end = _application.get_con_opt_map().end();
    for (; iter != end; iter++) {
        _con_opt_map[(*iter).first] = (*iter).second;
    }
    RedChannel::connect();
}

void RedClient::disconnect()
{
    _migrate.abort();
    RedChannel::disconnect();
}

void RedClient::disconnect_channels()
{
    Lock lock(_channels_lock);
    Channels::iterator iter = _channels.begin();
    for (; iter != _channels.end(); ++iter) {
        (*iter)->RedPeer::disconnect();
    }
}

void RedClient::on_channel_disconnected(RedChannel& channel)
{
    Lock lock(_notify_lock);
    if (_notify_disconnect) {
        _notify_disconnect = false;
        int connection_error = channel.get_connection_error();
        if (connection_error == SPICEC_ERROR_CODE_SUCCESS) {
            AutoRef<DisconnectedEvent> disconn_event(new DisconnectedEvent());
            LOG_INFO("disconneted");
            push_event(*disconn_event);
        } else {
             AutoRef<ConnectionErrorEvent> error_event(new ConnectionErrorEvent(connection_error));
             push_event(*error_event);
        }
    }
    disconnect_channels();
    RedPeer::disconnect();
}

bool RedClient::abort_channels()
{
    Lock lock(_channels_lock);
    Channels::iterator iter = _channels.begin();

    for (; iter != _channels.end(); ++iter) {
        if (!(*iter)->abort()) {
            return false;
        }
    }
    return true;
}

bool RedClient::abort()
{
    if (!_aborting) {
        Lock lock(_sync_lock);
        _aborting = true;
        _sync_condition.notify_all();
    }
    _pixmap_cache.abort();
    _glz_window.abort();
    if (RedChannel::abort() && abort_channels()) {
        delete_channels();
        _migrate.abort();
        return true;
    } else {
        return false;
    }
}

void RedClient::handle_migrate_begin(RedPeer::InMessage* message)
{
    DBG(0, "");
    RedMigrationBegin* migrate = (RedMigrationBegin*)message->data();
    //add mig channels
    _migrate.start(migrate);
}

void RedClient::handle_migrate_cancel(RedPeer::InMessage* message)
{
    _migrate.abort();
}

ChannelFactory* RedClient::find_factory(uint32_t type)
{
    Factorys::iterator iter = _factorys.begin();
    for (; iter != _factorys.end(); ++iter) {
        if ((*iter)->type() == type) {
            return *iter;
        }
    }
    LOG_WARN("no factory for %u", type);
    return NULL;
}

void RedClient::create_channel(uint32_t type, uint32_t id)
{
    ChannelFactory* factory = find_factory(type);
    if (!factory) {
        return;
    }
    RedChannel* channel = factory->construct(*this, id);
    ASSERT(channel);
    Lock lock(_channels_lock);
    _channels.push_back(channel);
    channel->start();
    channel->connect();
    _migrate.add_channel(new MigChannel(type, id, channel->get_common_caps(), channel->get_caps()));
}

void RedClient::send_agent_monitors_config()
{
    AutoRef<MonitorsQuery > qury(new MonitorsQuery());
    push_event(*qury);
    (*qury)->wait();
    if (!(*qury)->success()) {
        THROW(" monitors query failed");
    }

    double min_distance = HUGE;
    int dx = 0;
    int dy = 0;
    int i;

    std::vector<MonitorInfo>& monitors = (*qury)->get_monitors();
    std::vector<MonitorInfo>::iterator iter = monitors.begin();
    for (; iter != monitors.end(); iter++) {
        double distance = sqrt(pow((double)(*iter).position.x, 2) + pow((double)(*iter).position.y,
                                                                        2));
        if (distance < min_distance) {
            min_distance = distance;
            dx = -(*iter).position.x;
            dy = -(*iter).position.y;
        }
    }

    Message* message = new Message(REDC_AGENT_DATA,sizeof(VDAgentMessage) +
                                   sizeof(VDAgentMonitorsConfig) +
                                   monitors.size() * sizeof(VDAgentMonConfig));
    VDAgentMessage* msg = (VDAgentMessage*)message->data();
    msg->protocol = VD_AGENT_PROTOCOL;
    msg->type = VD_AGENT_MONITORS_CONFIG;
    msg->opaque = 0;
    msg->size = sizeof(VDAgentMonitorsConfig) + monitors.size() * sizeof(VDAgentMonConfig);

    VDAgentMonitorsConfig* mon_config = (VDAgentMonitorsConfig*)msg->data;
    mon_config->num_of_monitors = monitors.size();
    mon_config->flags = 0;
    if (Platform::is_monitors_pos_valid()) {
        mon_config->flags = VD_AGENT_CONFIG_MONITORS_FLAG_USE_POS;
    }
    for (iter = monitors.begin(), i = 0; iter != monitors.end(); iter++, i++) {
        mon_config->monitors[i].depth = (*iter).depth;
        mon_config->monitors[i].width = (*iter).size.x;
        mon_config->monitors[i].height = (*iter).size.y;
        mon_config->monitors[i].x = (*iter).position.x + dx;
        mon_config->monitors[i].y = (*iter).position.y + dy;
    }
    ASSERT(_agent_tokens)
    _agent_tokens--;
    post_message(message);
    _agent_mon_config_sent = true;
}

#define MIN_DISPLAY_PIXMAP_CACHE (1024 * 1024 * 20)
#define MAX_DISPLAY_PIXMAP_CACHE (1024 * 1024 * 80)
#define MIN_MEM_FOR_OTHERS (1024 * 1024 * 40)

// tmp till the pci mem will be shared by the qxls
#define MIN_GLZ_WINDOW_SIZE (1024 * 1024 * 12)
#define MAX_GLZ_WINDOW_SIZE MIN((LZ_MAX_WINDOW_SIZE * 4), 1024 * 1024 * 64)

void RedClient::calc_pixmap_cach_and_glz_window_size(uint32_t display_channels_hint,
                                                     uint32_t pci_mem_hint)
{
#ifdef WIN32
    display_channels_hint = MAX(1, display_channels_hint);
    int max_cache_size = display_channels_hint * MAX_DISPLAY_PIXMAP_CACHE;
    int min_cache_size = display_channels_hint * MIN_DISPLAY_PIXMAP_CACHE;

    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);

    if (!GlobalMemoryStatusEx(&mem_status)) {
        THROW("get mem status failed %u", GetLastError());
    }

    //ullTotalPageFile is physical memory plus the size of the page file, minus a small overhead
    uint64_t free_mem = mem_status.ullAvailPageFile;
    if (free_mem < (min_cache_size + MIN_MEM_FOR_OTHERS + MIN_GLZ_WINDOW_SIZE)) {
        THROW_ERR(SPICEC_ERROR_CODE_NOT_ENOUGH_MEMORY, "low memory condition");
    }
    free_mem -= MIN_MEM_FOR_OTHERS;
    _glz_window_size = MIN(MAX_GLZ_WINDOW_SIZE, pci_mem_hint / 2);
    _glz_window_size = (int)MIN(free_mem / 3, _glz_window_size);
    _glz_window_size = MAX(MIN_GLZ_WINDOW_SIZE, _glz_window_size);
    free_mem -= _glz_window_size;
    _pixmap_cache_size = MIN(free_mem, mem_status.ullAvailVirtual);
    _pixmap_cache_size = MIN(free_mem, max_cache_size);
#else
    //for now
    _glz_window_size = (int)MIN(MAX_GLZ_WINDOW_SIZE, pci_mem_hint / 2);
    _glz_window_size = MAX(MIN_GLZ_WINDOW_SIZE, _glz_window_size);
    _pixmap_cache_size = MAX_DISPLAY_PIXMAP_CACHE;
#endif

    _pixmap_cache_size /= 4;
    _glz_window_size /= 4;
}

void RedClient::on_display_mode_change()
{
#ifdef USE_OGL
    Lock lock(_channels_lock);
    Channels::iterator iter = _channels.begin();
    for (; iter != _channels.end(); ++iter) {
        if ((*iter)->get_type() == RED_CHANNEL_DISPLAY) {
            ((DisplayChannel *)(*iter))->recreate_ogl_context();
        }
    }
#endif
}

void RedClient::set_mouse_mode(uint32_t supported_modes, uint32_t current_mode)
{
    if (current_mode != _mouse_mode) {
        _mouse_mode = current_mode;
        Lock lock(_channels_lock);
        Channels::iterator iter = _channels.begin();
        for (; iter != _channels.end(); ++iter) {
            if ((*iter)->get_type() == RED_CHANNEL_CURSOR) {
                ((CursorChannel *)(*iter))->set_cursor_mode();
            }
        }
    }
    // FIXME: use configured mouse mode (currently, use client mouse mode if supported by server)
    if ((supported_modes & RED_MOUSE_MODE_CLIENT) && (current_mode != RED_MOUSE_MODE_CLIENT)) {
        Message* message = new Message(REDC_MOUSE_MODE_REQUEST, sizeof(RedcMouseModeRequest));
        RedcMouseModeRequest* mouse_mode_request = (RedcMouseModeRequest*)message->data();
        mouse_mode_request->mode = RED_MOUSE_MODE_CLIENT;
        post_message(message);
    }
}

void RedClient::handle_init(RedPeer::InMessage* message)
{
    RedInit *init = (RedInit *)message->data();
    _connection_id = init->session_id;
    set_mm_time(init->multi_media_time);
    calc_pixmap_cach_and_glz_window_size(init->display_channels_hint, init->ram_hint);
    _glz_window.set_pixels_capacity(_glz_window_size);
    set_mouse_mode(init->supported_mouse_modes, init->current_mouse_mode);
    _agent_tokens = init->agent_tokens;
    _agent_connected = !!init->agent_connected;
    if (_agent_connected) {
        Message* msg = new Message(REDC_AGENT_START, sizeof(RedcAgentStart));
        RedcAgentStart* agent_start = (RedcAgentStart *)msg->data();
        agent_start->num_tokens = ~0;
        post_message(msg);
    }
    if (_auto_display_res) {
        _application.activate_interval_timer(*_agent_timer, AGENT_TIMEOUT);
        if (_agent_connected) {
            send_agent_monitors_config();
        }
    } else {
        post_message(new Message(REDC_ATTACH_CHANNELS, 0));
    }
}

void RedClient::handle_channels(RedPeer::InMessage* message)
{
    RedChannels *init = (RedChannels *)message->data();
    RedChannelInit* channels = init->channels;
    for (unsigned int i = 0; i < init->num_of_channels; i++) {
        create_channel(channels[i].type, channels[i].id);
    }
}

void RedClient::handle_mouse_mode(RedPeer::InMessage* message)
{
    RedMouseMode *mouse_mode = (RedMouseMode *)message->data();
    set_mouse_mode(mouse_mode->supported_modes, mouse_mode->current_mode);
}

void RedClient::handle_mm_time(RedPeer::InMessage* message)
{
    RedMultiMediaTime *mm_time = (RedMultiMediaTime *)message->data();
    set_mm_time(mm_time->time);
}

void RedClient::handle_agent_connected(RedPeer::InMessage* message)
{
    DBG(0, "");
    _agent_connected = true;
    Message* msg = new Message(REDC_AGENT_START, sizeof(RedcAgentStart));
    RedcAgentStart* agent_start = (RedcAgentStart *)msg->data();
    agent_start->num_tokens = ~0;
    post_message(msg);
    if (_auto_display_res && !_agent_mon_config_sent) {
        send_agent_monitors_config();
    }
}

void RedClient::handle_agent_disconnected(RedPeer::InMessage* message)
{
    DBG(0, "");
    _agent_connected = false;
}

void RedClient::on_agent_reply(VDAgentReply* reply)
{
    switch (reply->error) {
    case VD_AGENT_SUCCESS:
        break;
    case VD_AGENT_ERROR:
        THROW_ERR(SPICEC_ERROR_CODE_AGENT_ERROR, "vdagent error");
    default:
        THROW("unknown vdagent error");
    }
    switch (reply->type) {
    case VD_AGENT_MONITORS_CONFIG:
        post_message(new Message(REDC_ATTACH_CHANNELS, 0));
        _application.deactivate_interval_timer(*_agent_timer);
        break;
    default:
        THROW("unexpected vdagent reply type");
    }
}

void RedClient::handle_agent_data(RedPeer::InMessage* message)
{
    uint32_t msg_size = message->size();
    uint8_t* msg_pos = message->data();
    uint32_t n;

    DBG(0, "");
    while (msg_size) {
        if (_agent_msg_pos < sizeof(VDAgentMessage)) {
            n = MIN(sizeof(VDAgentMessage) - _agent_msg_pos, msg_size);
            memcpy((uint8_t*)_agent_msg + _agent_msg_pos, msg_pos, n);
            _agent_msg_pos += n;
            msg_size -= n;
            msg_pos += n;
            if (_agent_msg_pos == sizeof(VDAgentMessage)) {
                if (_agent_msg->protocol != VD_AGENT_PROTOCOL) {
                    THROW("Invalid protocol %u", _agent_msg->protocol);
                }
                _agent_msg_data = new uint8_t[_agent_msg->size];
            }
        }
        if (_agent_msg_pos >= sizeof(VDAgentMessage)) {
            n = MIN(sizeof(VDAgentMessage) + _agent_msg->size - _agent_msg_pos, msg_size);
            memcpy(_agent_msg_data + _agent_msg_pos - sizeof(VDAgentMessage), msg_pos, n);
            _agent_msg_pos += n;
            msg_size -= n;
            msg_pos += n;
        }
        if (_agent_msg_pos == sizeof(VDAgentMessage) + _agent_msg->size) {
            switch (_agent_msg->type) {
            case VD_AGENT_REPLY: {
                on_agent_reply((VDAgentReply*)_agent_msg_data);
                break;
            }
            default:
                DBG(0, "Unsupported message type %u size %u", _agent_msg->type, _agent_msg->size);
            }
            delete[] _agent_msg_data;
            _agent_msg_data = NULL;
            _agent_msg_pos = 0;
        }
    }
}

void RedClient::handle_agent_tokens(RedPeer::InMessage* message)
{
    RedAgentTokens *token = (RedAgentTokens *)message->data();
    _agent_tokens += token->num_tokens;
}

void RedClient::migrate_channel(RedChannel& channel)
{
    DBG(0, "channel type %u id %u", channel.get_type(), channel.get_id());
    _migrate.swap_peer(channel);
}

void RedClient::get_sync_info(uint8_t channel_type, uint8_t channel_id, SyncInfo& info)
{
    info.lock = &_sync_lock;
    info.condition = &_sync_condition;
    info.message_serial = &_sync_info[channel_type][channel_id];
}

void RedClient::wait_for_channels(int wait_list_size, RedWaitForChannel* wait_list)
{
    for (int i = 0; i < wait_list_size; i++) {
        if (wait_list[i].channel_type >= RED_CHANNEL_END) {
            THROW("invalid channel type %u", wait_list[i].channel_type);
        }
        uint64_t& sync_cell = _sync_info[wait_list[i].channel_type][wait_list[i].channel_id];
#ifndef RED64
        Lock lock(_sync_lock);
#endif
        if (sync_cell >= wait_list[i].message_serial) {
            continue;
        }
#ifdef RED64
        Lock lock(_sync_lock);
#endif
        for (;;) {
            if (sync_cell >= wait_list[i].message_serial) {
                break;
            }
            if (_aborting) {
                THROW("aborting");
            }
            _sync_condition.wait(lock);
            continue;
        }
    }
}

void RedClient::set_mm_time(uint32_t time)
{
    Lock lock(_mm_clock_lock);
    _mm_clock_last_update = Platform::get_monolithic_time();
    _mm_time = time;
}

uint32_t RedClient::get_mm_time()
{
    Lock lock(_mm_clock_lock);
    return uint32_t((Platform::get_monolithic_time() - _mm_clock_last_update) / 1000 / 1000 +
                    _mm_time);
}

void RedClient::register_channel_factory(ChannelFactory& factory)
{
    _factorys.push_back(&factory);
}


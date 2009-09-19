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

#ifndef _H_APPLICATION
#define _H_APPLICATION

#include "common.h"
#include "threads.h"
#include "red_client.h"
#include "red_key.h"
#include "platform.h"
#include "menu.h"
#include "hot_keys.h"

class RedScreen;
class Application;
class ScreenLayer;
class GUILayer;
class InputsHandler;
class Monitor;
class CmdLineParser;
class Menu;

class Event {
public:
    Event() : _refs (1) {}

    virtual void responce(Application& application) = 0;

    Event* ref() { ++_refs; return this;}
    void unref() {if (--_refs == 0) delete this;}

protected:
    virtual ~Event() {}

    AtomicCount _refs;
    friend class Application;
    uint32_t _generation;
};

class SyncEvent: public Event {
public:
    SyncEvent();

    void wait();
    bool success() { return !_err;}

    virtual void do_responce(Application& application) {}

protected:
    virtual ~SyncEvent();

private:
    virtual void responce(Application& application);

private:
    Mutex _mutex;
    Condition _condition;
    bool _err;
    bool _ready;
};

class ConnectedEvent: public Event {
public:
    ConnectedEvent() : Event() {}
    virtual void responce(Application& application);
};

class DisconnectedEvent: public Event {
public:
    DisconnectedEvent() : Event() {}
    virtual void responce(Application& application);
};

class CoonnectionError: public Event {
public:
    CoonnectionError(int error_code) : Event(), _error_code (error_code) {}

    virtual void responce(Application& application);

private:
    int _error_code;
};

class ErrorEvent: public Event {
public:
    ErrorEvent() : Event() {}
    virtual void responce(Application& application);
};

struct MonitorInfo {
    int depth;
    Point size;
    Point position;
};

class MonitorsQuery: public SyncEvent {
public:
    MonitorsQuery() {}

    virtual void do_responce(Application& application);

    std::vector<MonitorInfo>& get_monitors() {return _monitors;}

private:
    std::vector<MonitorInfo> _monitors;
};

struct KeyInfo {
    uint32_t _make;
    uint32_t _break;
    bool press;
};

enum CanvasOption {
    CANVAS_OPTION_INVALID,
    CANVAS_OPTION_CAIRO,
#ifdef WIN32
    CANVAS_OPTION_GDI,
#endif
#ifdef USE_OGL
    CANVAS_OPTION_OGL_FBO,
    CANVAS_OPTION_OGL_PBUFF,
#endif
};

class Application : public Platform::EventListener,
                    public Platform::DisplayModeListner,
                    public CommandTarget {
public:
    Application();
    virtual ~Application();

    int run();
    void quit(int exit_code);
    void push_event(Event* event);
    void set_inputs_handler(InputsHandler& handler);
    void remove_inputs_handler(InputsHandler& handler);
    RedScreen* find_screen(int id);
    RedScreen* get_screen(int id);

    void on_screen_destroyed(int id, bool was_captured);
    void on_mouse_motion(int dx, int dy, int buttons_state);
    void on_mouse_position(int x, int y, int buttons_state, int display_id);
    void on_mouse_down(int button, int buttons_state);
    void on_mouse_up(int button, int buttons_state);
    void on_key_down(RedKey key);
    void on_key_up(RedKey key);
    void on_deactivate_screen(RedScreen* screen);
    void on_activate_screen(RedScreen* screen);
    virtual void on_app_activated();
    virtual void on_app_deactivated();
    virtual void on_monitors_change();
    virtual void on_display_mode_change();
    void on_connected();
    void on_disconnecting();

    bool rearrange_monitors(RedScreen& screen);
    void enter_full_screen();
    void exit_full_screen();
    bool toggle_full_screen();
    void minimize();
    void show_splash(int screen_id);
    void hide_splash(int screen_id);
    void set_title(std::wstring& title);
    void hide();
    void show();
    void external_show();
    void connect();
    const PeerConnectionOptMap& get_con_opt_map() {return _peer_con_opt;}
    uint32_t get_mouse_mode();
    const std::vector<int>& get_canvas_types() { return _canvas_types;}

    Menu* get_app_menu();
    virtual void do_command(int command);

    static int main(int argc, char** argv, const char* version_str);

private:
    bool set_channels_security(CmdLineParser& parser, bool on, char *val);
    bool set_enable_channels(CmdLineParser& parser, bool enable, char *val);
    bool set_canvas_option(CmdLineParser& parser, char *val);
    bool process_cmd_line(int argc, char** argv);
    void process_events();
    int message_loop();
    void abort();
    void init_scan_code(int index);
    void init_korean_scan_code(int index);
    void init_escape_scan_code(int index);
    void init_pause_scan_code();
    void init_key_table();
    void init_menu();
    uint32_t get_make_scan_code(RedKey key);
    uint32_t get_break_scan_code(RedKey key);
    void unpress_all();
    bool release_capture();
    bool do_connect();
    bool do_disconnect();

    Monitor* find_monitor(int id);
    Monitor* get_monitor(int id);
    void init_monitors();
    void destroy_monitors();
    void assign_monitors();
    void restore_monitors();
    void prepare_monitors();
    void position_screens();
    void show_full_screen();
    void send_key_down(RedKey key);
    void send_key_up(RedKey key);
    void send_alt_ctl_del();
    void send_ctrl_alt_end();
    void send_command_hotkey(int command);
    void send_hotkey_key_set(const HotkeySet& key_set);
    void menu_item_callback(unsigned int item_id);
    int get_hotkeys_commnad();
    bool is_key_set_pressed(const HotkeySet& key_set);
    bool is_cad_pressed();

    static void init_logger();
    static void init_globals();

    friend class MonitorsQuery;
    friend class AutoAbort;

private:
    RedClient _client;
    PeerConnectionOptMap _peer_con_opt;
    std::vector<bool> _enabled_channels;
    std::vector<RedScreen*> _screens;
    std::list<Event*> _events;
    RedScreen* _main_screen;
    Mutex _events_lock;
    bool _quitting;
    bool _active;
    bool _full_screen;
    bool _changing_screens;
    int _exit_code;
    uint32_t _events_gen;
    RedScreen* _active_screen;
    KeyInfo _key_table[REDKEY_NUM_KEYS];
    HotKeys _hot_keys;
    CommandsMap _commands_map;
    std::auto_ptr<GUILayer> _gui_layer;
    InputsHandler* _inputs_handler;
    const MonitorsList* _monitors;
    std::wstring _title;
    std::vector<int> _canvas_types;
    AutoRef<Menu> _app_menu;
};

#endif


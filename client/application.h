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
#include "process_loop.h"
#include "foreign_menu.h"
#include "controller.h"

class RedScreen;
class Application;
class ScreenLayer;
class GUILayer;
class InputsHandler;
class Monitor;
class CmdLineParser;
class Menu;


class ConnectedEvent: public Event {
public:
    virtual void response(AbstractProcessLoop& events_loop);
};

class DisconnectedEvent: public Event {
public:
    virtual void response(AbstractProcessLoop& events_loop);
};

class ConnectionErrorEvent: public Event {
public:
    ConnectionErrorEvent(int error_code) : _error_code (error_code) {}
    virtual void response(AbstractProcessLoop& events_loop);
private:
    int _error_code;
};

struct MonitorInfo {
    int depth;
    Point size;
    Point position;
};

class MonitorsQuery: public SyncEvent {
public:
    MonitorsQuery() {}

    virtual void do_response(AbstractProcessLoop& events_loop);
    std::vector<MonitorInfo>& get_monitors() {return _monitors;}

private:
    std::vector<MonitorInfo> _monitors;
};

class SwitchHostEvent: public Event {
public:
    SwitchHostEvent(const char* host, int port, int sport, const char* cert_subject);
    virtual void response(AbstractProcessLoop& events_loop);

private:
    std::string _host;
    int _port;
    int _sport;
    std::string _cert_subject;
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

class StickyKeyTimer: public Timer {
public:
    virtual void response(AbstractProcessLoop& events_loop);
};

typedef struct StickyInfo {
    bool trace_is_on;
    bool sticky_mode;
    bool key_first_down; // True when (1) a potential sticky key is pressed,
                         // and none of the other keys are pressed and (2) in the moment
                         // of pressing, _sticky_mode is false. When the key is up
                         // for the first time, it is set to false.
    bool key_down;       // The physical state of the sticky key. Valid only till
                         // stickiness is removed.
    RedKey key;          // the key that is currently being traced, or,
                         // if _sticky mode is on, the sticky key
    AutoRef<StickyKeyTimer> timer;
} StickyInfo;

class SwitchHostTimer: public Timer {
public:
    virtual void response(AbstractProcessLoop& events_loop);
};

enum AppMenuItemType {
    APP_MENU_ITEM_TYPE_INVALID,
    APP_MENU_ITEM_TYPE_FOREIGN,
    APP_MENU_ITEM_TYPE_CONTROLLER,
};

typedef struct AppMenuItem {
    AppMenuItemType type;
    int32_t conn_ref;
    uint32_t ext_id;
} AppMenuItem;

typedef std::map<int, AppMenuItem> AppMenuItemMap;

class Application : public ProcessLoop,
                    public Platform::EventListener,
                    public Platform::DisplayModeListner,
                    public ForeignMenuInterface,
                    public ControllerInterface {
public:
    Application();
    virtual ~Application();

    int run();

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
    void on_start_screen_key_interception(RedScreen* screen);
    void on_stop_screen_key_interception(RedScreen* screen);
    virtual void on_start_running();
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
    void set_title(const std::wstring& title);
    void hide();
    void show();
    void external_show();
    void connect();
    void switch_host(const std::string& host, int port, int sport, const std::string& cert_subject);

    const PeerConnectionOptMap& get_con_opt_map() {return _peer_con_opt;}
    const RedPeer::HostAuthOptions& get_host_auth_opt() { return _host_auth_opt;}
    const std::string& get_connection_ciphers() { return _con_ciphers;}
    uint32_t get_mouse_mode();
    const std::vector<int>& get_canvas_types() { return _canvas_types;}

    Menu* get_app_menu();
    virtual void do_command(int command);

    int get_foreign_menu_item_id(int32_t opaque_conn_ref, uint32_t msg_id);
    void clear_menu_items(int32_t opaque_conn_ref);
    void remove_menu_item(int item_id);
    void update_menu();

    bool connect(const std::string& host, int port, int sport, const std::string& password);
    void show_me(bool full_screen, bool auto_display_res);
    void hide_me();
    void set_hotkeys(const std::string& hotkeys);
    int get_controller_menu_item_id(int32_t opaque_conn_ref, uint32_t msg_id);
    void set_menu(Menu* menu);
    void delete_menu();

    static int main(int argc, char** argv, const char* version_str);

private:
    bool set_channels_security(CmdLineParser& parser, bool on, char *val, const char* arg0);
    bool set_channels_security(int port, int sport);
    bool set_connection_ciphers(const char* ciphers, const char* arg0);
    bool set_ca_file(const char* ca_file, const char* arg0);
    bool set_host_cert_subject(const char* subject, const char* arg0);
    bool set_enable_channels(CmdLineParser& parser, bool enable, char *val, const char* arg0);
    bool set_canvas_option(CmdLineParser& parser, char *val, const char* arg0);
    void on_cmd_line_invalid_arg(const char* arg0, const char* what, const char* val);
    bool process_cmd_line(int argc, char** argv);
    void register_channels();
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
    int get_menu_item_id(AppMenuItemType type, int32_t conn_ref, uint32_t ext_id);
    int get_hotkeys_commnad();
    bool is_key_set_pressed(const HotkeySet& key_set);
    bool is_cad_pressed();
    void do_on_key_up(RedKey key);

    // returns the press value before operation (i.e., if it was already pressed)
    bool press_key(RedKey key);
    bool unpress_key(RedKey key);
    void reset_sticky();
    static bool is_sticky_trace_key(RedKey key);
    void sync_keyboard_modifiers();

    static void init_logger();
    static void init_globals();
    static void cleanup_globals();

    friend class DisconnectedEvent;
    friend class ConnectionErrorEvent;
    friend class MonitorsQuery;
    friend class AutoAbort;
    friend class StickyKeyTimer;
    friend class SwitchHostTimer;

private:
    RedClient _client;
    PeerConnectionOptMap _peer_con_opt;
    RedPeer::HostAuthOptions _host_auth_opt;
    std::string _con_ciphers;
    std::vector<bool> _enabled_channels;
    std::vector<RedScreen*> _screens;
    RedScreen* _main_screen;
    bool _active;
    bool _full_screen;
    bool _changing_screens;
    int _exit_code;
    RedScreen* _active_screen;
    KeyInfo _key_table[REDKEY_NUM_KEYS];
    int _num_keys_pressed;
    HotKeys _hot_keys;
    CommandsMap _commands_map;
    std::auto_ptr<GUILayer> _gui_layer;
    InputsHandler* _inputs_handler;
    const MonitorsList* _monitors;
    std::wstring _title;
    bool _splash_mode;
    bool _sys_key_intercept_mode;
    StickyInfo _sticky_info;
    std::vector<int> _canvas_types;
    AutoRef<Menu> _app_menu;
    bool _during_host_switch;
    AutoRef<SwitchHostTimer> _switch_host_timer;
    AutoRef<ForeignMenu> _foreign_menu;
    bool _enable_controller;
    AutoRef<Controller> _controller;
    AppMenuItemMap _app_menu_items;
};

#endif


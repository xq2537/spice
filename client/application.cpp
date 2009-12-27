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

#ifndef WIN32
#include "config.h"
#endif

#include "common.h"
#ifdef WIN32
#include <io.h>
#endif

#include "application.h"
#include "screen.h"
#include "utils.h"
#include "debug.h"
#include "screen_layer.h"
#include "monitor.h"
#include "resource.h"
#ifdef WIN32
#include "red_gdi_canvas.h"
#endif
#include "platform.h"
#include "cairo_canvas.h"
#include "gl_canvas.h"
#include "quic.h"
#include "mutex.h"
#include "cmd_line_parser.h"
#include "tunnel_channel.h"
#include "rect.h"

#include <log4cpp/BasicConfigurator.hh>
#include <log4cpp/FileAppender.hh>
#include <log4cpp/RollingFileAppender.hh>

#define STICKY_KEY_PIXMAP ALT_IMAGE_RES_ID
#define STICKY_KEY_TIMEOUT 750

#ifdef CAIRO_CANVAS_CACH_IS_SHARED
mutex_t cairo_surface_user_data_mutex;
#endif

void ConnectedEvent::response(AbstractProcessLoop& events_loop)
{
    static_cast<Application*>(events_loop.get_owner())->on_connected();
}

void DisconnectedEvent::response(AbstractProcessLoop& events_loop)
{
    Application* app = static_cast<Application*>(events_loop.get_owner());
#ifdef RED_DEBUG
    app->show_splash(0);
#else
    app->do_quit(SPICEC_ERROR_CODE_SUCCESS);
#endif
}

void ConnectionErrorEvent::response(AbstractProcessLoop& events_loop)
{
    Application* app = static_cast<Application*>(events_loop.get_owner());
#ifdef RED_DEBUG
    app->show_splash(0);
#else
    app->do_quit(_error_code);
#endif
}

void MonitorsQuery::do_response(AbstractProcessLoop& events_loop)
{
    Monitor* mon;
    int i = 0;

    while ((mon = (static_cast<Application*>(events_loop.get_owner()))->find_monitor(i++))) {
        MonitorInfo info;
        info.size = mon->get_size();
        info.depth = 32;
        info.position = mon->get_position();
        _monitors.push_back(info);
    }
}

class GUILayer: public ScreenLayer {
public:
    GUILayer();

    virtual void copy_pixels(const QRegion& dest_region, RedDrawable& dest_dc);

    void set_splash_mode();
    void set_info_mode();
    void set_sticky(bool is_on);
    virtual void on_size_changed();

private:
    void draw_splash(const QRegion& dest_region, RedDrawable& dest);
    void draw_info(const QRegion& dest_region, RedDrawable& dest);

private:
    ImageFromRes _splash_pixmap;
    AlphaImageFromRes _info_pixmap;
    AlphaImageFromRes _sticky_pixmap;
    Point _splash_pos;
    Point _info_pos;
    Point _sticky_pos;
    Rect _sticky_rect;
    bool _splash_mode;
    bool _sticky_on;
    RecurciveMutex _update_lock;
};

GUILayer::GUILayer()
    : ScreenLayer(SCREEN_LAYER_GUI, false)
    , _splash_pixmap (SPLASH_IMAGE_RES_ID)
    , _info_pixmap (INFO_IMAGE_RES_ID)
    , _sticky_pixmap (STICKY_KEY_PIXMAP)
    , _splash_mode (false)
    , _sticky_on (false)
{
}

void GUILayer::draw_splash(const QRegion& dest_region, RedDrawable& dest)
{
    ASSERT(!_sticky_on);
    for (int i = 0; i < (int)dest_region.num_rects; i++) {
        Rect* r = &dest_region.rects[i];
        dest.copy_pixels(_splash_pixmap, r->left - _splash_pos.x, r->top - _splash_pos.y, *r);
    }
}

void GUILayer::draw_info(const QRegion& dest_region, RedDrawable& dest)
{
    for (int i = 0; i < (int)dest_region.num_rects; i++) {
        Rect* r = &dest_region.rects[i];
        /* is rect inside sticky region or info region? */
        if (_sticky_on && rect_intersects(*r, _sticky_rect)) {
            dest.blend_pixels(_sticky_pixmap, r->left - _sticky_pos.x, r->top - _sticky_pos.y, *r);
        } else {
            dest.blend_pixels(_info_pixmap, r->left - _info_pos.x, r->top - _info_pos.y, *r);
        }
    }
}

void GUILayer::copy_pixels(const QRegion& dest_region, RedDrawable& dest_dc)
{
    RecurciveLock lock(_update_lock);
    if (_splash_mode) {
        draw_splash(dest_region, dest_dc);
    } else {
        draw_info(dest_region, dest_dc);
    }
}

void GUILayer::set_splash_mode()
{
    RecurciveLock lock(_update_lock);
    Point size = _splash_pixmap.get_size();
    Point screen_size = screen()->get_size();
    Rect r;

    _splash_pos.y = r.top = (screen_size.y - size.y) / 2;
    _splash_pos.x = r.left = (screen_size.x - size.x) / 2;
    r.bottom = r.top + size.y;
    r.right = r.left + size.x;
    _splash_mode = true;
    lock.unlock();
    set_rect_area(r);
    ASSERT(!_sticky_on);
}

void GUILayer::set_info_mode()
{
    RecurciveLock lock(_update_lock);
    Point size = _info_pixmap.get_size();
    Point screen_size = screen()->get_size();
    Rect r;

    r.left = (screen_size.x - size.x) / 2;
    r.right = r.left + size.x;
    _info_pos.x = r.right - size.x;
    _info_pos.y = r.top = 0;
    r.bottom = r.top + size.y;
    _splash_mode = false;
    lock.unlock();
    set_rect_area(r);

    set_sticky(_sticky_on);
}

void GUILayer::set_sticky(bool is_on)
{
    RecurciveLock lock(_update_lock);
    if (!_sticky_on && !is_on) {
        return;
    }

    Point size = _sticky_pixmap.get_size();
    Point screen_size = screen()->get_size();

    _sticky_on = is_on;
    if (_sticky_on) {
        _sticky_pos.x = (screen_size.x - size.x) / 2;
        _sticky_pos.y = screen_size.y * 2 / 3;
        _sticky_rect.left = _sticky_pos.x;
        _sticky_rect.top = _sticky_pos.y;
        _sticky_rect.right = _sticky_rect.left + size.x;
        _sticky_rect.bottom = _sticky_rect.top + size.y;
        add_rect_area(_sticky_rect);
        invalidate();
    } else {
        remove_rect_area(_sticky_rect);
    }
}

void GUILayer::on_size_changed()
{
    if (_splash_mode) {
        set_splash_mode();
    } else {
        set_info_mode();
    }
}

void StickyKeyTimer::response(AbstractProcessLoop& events_loop)
{
    Application* app = (Application*)events_loop.get_owner();
    StickyInfo* sticky_info = &app->_sticky_info;
    ASSERT(app->is_sticky_trace_key(sticky_info->key));
    ASSERT(app->_keyboard_state[sticky_info->key]);
    ASSERT(sticky_info->key_first_down);
    ASSERT(sticky_info->key_down);
    sticky_info->sticky_mode = true;
    DBG(0, "ON sticky");
    app->_gui_layer->set_sticky(true);
    app->deactivate_interval_timer(this);
}

static MouseHandler default_mouse_handler;
static KeyHandler default_key_handler;

enum AppCommands {
    APP_CMD_INVALID,
    APP_CMD_SEND_CTL_ALT_DEL,
    APP_CMD_TOGGLE_FULL_SCREEN,
    APP_CMD_RELEASE_CAPTURE,
    APP_CMD_SEND_TOGGLE_KEYS,
    APP_CMD_SEND_RELEASE_KEYS,
    APP_CMD_SEND_CTL_ALT_END,
#ifdef RED_DEBUG
    APP_CMD_CONNECT,
    APP_CMD_DISCONNECT,
#endif
};

Application::Application()
    : ProcessLoop (this)
    , _client (*this)
    , _enabled_channels (RED_CHANNEL_END, true)
    , _main_screen (NULL)
    , _active (false)
    , _full_screen (false)
    , _changing_screens (false)
    , _exit_code (0)
    , _active_screen (NULL)
    , _num_keys_pressed (0)
    , _gui_layer (new GUILayer())
    , _key_handler (&default_key_handler)
    , _mouse_handler (&default_mouse_handler)
    , _monitors (NULL)
    , _title (L"SPICEc:%d")
    , _splash_mode (true)
    , _sys_key_intercept_mode (false)
{
    DBG(0, "");
    Platform::set_process_loop(*this);
    init_monitors();
    memset(_keyboard_state, 0, sizeof(_keyboard_state));
    init_menu();
    _main_screen = get_screen(0);
    _main_screen->attach_layer(*_gui_layer);
    _gui_layer->set_splash_mode();

    Platform::set_event_listener(this);
    Platform::set_display_mode_listner(this);

    _commands_map["toggle-fullscreen"] = APP_CMD_TOGGLE_FULL_SCREEN;
    _commands_map["release-cursor"] = APP_CMD_RELEASE_CAPTURE;
#ifdef RED_DEBUG
    _commands_map["connect"] = APP_CMD_CONNECT;
    _commands_map["disconnect"] = APP_CMD_DISCONNECT;
#endif

    _canvas_types.resize(1);
#ifdef WIN32
    _canvas_types[0] = CANVAS_OPTION_GDI;
#else
    _canvas_types[0] = CANVAS_OPTION_CAIRO;
#endif

    std::auto_ptr<HotKeysParser> parser(new HotKeysParser("toggle-fullscreen=shift+f11"
                                                          ",release-cursor=shift+f12"
#ifdef RED_DEBUG
                                                          ",connect=shift+f5"
                                                          ",disconnect=shift+f6"
#endif
                                                          , _commands_map));
    _hot_keys = parser->get();

    _sticky_info.trace_is_on = false;
    _sticky_info.sticky_mode = false;
    _sticky_info.key_first_down = false;
    _sticky_info.key_down = false;
    _sticky_info.key  = REDKEY_INVALID;
    _sticky_info.timer.reset(new StickyKeyTimer());
}

Application::~Application()
{
    _main_screen->detach_layer(*_gui_layer);
    _main_screen->unref();
    destroy_monitors();
}

void Application::init_menu()
{
    //fixme: menu items name need to be dynamically updated by hot keys configuration.
    AutoRef<Menu> root_menu(new Menu(*this, ""));
    (*root_menu)->add_command("Send Ctrl+Alt+Del\tCtrl+Alt+End", APP_CMD_SEND_CTL_ALT_DEL);
    (*root_menu)->add_command("Toggle full screen\tShift+F11", APP_CMD_TOGGLE_FULL_SCREEN);
    AutoRef<Menu> key_menu(new Menu(*this, "Special keys"));
    (*key_menu)->add_command("Send Shift+F11", APP_CMD_SEND_TOGGLE_KEYS);
    (*key_menu)->add_command("Send Shift+F12", APP_CMD_SEND_RELEASE_KEYS);
    (*key_menu)->add_command("Send Ctrl+Alt+End", APP_CMD_SEND_CTL_ALT_END);
    (*root_menu)->add_sub(key_menu.release());
    _app_menu.reset(root_menu.release());
}

void Application::__remove_key_handler(KeyHandler& handler)
{
    KeyHandlersStack::iterator iter = _key_handlers.begin();

    for (; iter != _key_handlers.end(); iter++) {
        if (*iter == &handler) {
            _key_handlers.erase(iter);
            return;
        }
    }
}

void Application::set_key_handler(KeyHandler& handler)
{
    if (&handler == _key_handler) {
        return;
    }

    __remove_key_handler(handler);
    if (!_key_handler->permit_focus_loss()) {
        _key_handlers.push_front(&handler);
        return;
    }

    unpress_all();
    if (_active) {
        _key_handler->on_focus_out();
    }

    _key_handlers.push_front(_key_handler);
    _key_handler = &handler;
    if (_active) {
        _key_handler->on_focus_in();
    }
}

void Application::remove_key_handler(KeyHandler& handler)
{
    bool is_current = (&handler == _key_handler);

    if (!is_current) {
        __remove_key_handler(handler);
        return;
    }

    KeyHandler damy_handler;
    _key_handler = &damy_handler;
    set_key_handler(**_key_handlers.begin());
    __remove_key_handler(damy_handler);
}

void Application::set_mouse_handler(MouseHandler& handler)
{
    _mouse_handler = &handler;
}

void Application::remove_mouse_handler(MouseHandler& handler)
{
    if (_mouse_handler != &handler) {
        return;
    }
    _mouse_handler = &default_mouse_handler;
}

void Application::capture_mouse()
{
    if (!_active_screen) {
        return;
    }
    _active_screen->capture_mouse();
}

void Application::release_mouse_capture()
{
    if (!_active_screen || !_active_screen->is_mouse_captured()) {
        return;
    }
    _active_screen->relase_mouse();
}

void Application::abort()
{
    Platform::set_event_listener(NULL);
    Platform::set_display_mode_listner(NULL);
    unpress_all();
    while (!_client.abort()) {
        ProcessLoop::process_events_queue();
        Platform::msleep(100);
    }
}

class AutoAbort {
public:
    AutoAbort(Application& app) : _app(app) {}
    ~AutoAbort() { _app.abort();}

private:
    Application& _app;
};

void Application::connect()
{
    _client.connect();
}

int Application::run()
{
       _client.connect();
       _exit_code = ProcessLoop::run();
       return _exit_code;

}

RedScreen* Application::find_screen(int id)
{
    if ((int)_screens.size() < id + 1) {
        return NULL;
    }
    return _screens[id];
}

bool Application::release_capture()
{
    unpress_all();
    if (!_active_screen || !_active_screen->is_mouse_captured()) {
        return false;
    }
    _active_screen->relase_mouse();
    return true;
}

bool Application::do_connect()
{
    _client.connect();
    return true;
}

bool Application::do_disconnect()
{
    on_disconnecting();
    _client.disconnect();
    return true;
}

#define SCREEN_INIT_WIDTH 800
#define SCREEN_INIT_HEIGHT 600

RedScreen* Application::get_screen(int id)
{
    RedScreen *screen;

    if ((int)_screens.size() < id + 1) {
        _screens.resize(id + 1);
    }

    if (!(screen = _screens[id])) {
        Monitor* mon = find_monitor(id);
        Point size;

        if (_full_screen && mon) {
            Point size = mon->get_size();
        } else {
            size.x = SCREEN_INIT_WIDTH;
            size.y = SCREEN_INIT_HEIGHT;
        }
        screen = _screens[id] = new RedScreen(*this, id, _title, size.x, size.y);

        if (id != 0) {
            if (_full_screen) {
                bool capture;

                mon = get_monitor(id);
                capture = release_capture();
                screen->set_monitor(mon);
                position_screens();
                screen->show_full_screen();
                prepare_monitors();

                if (capture) {
                    _main_screen->activate();
                    _main_screen->capture_mouse();
                }
            } else {
                screen->show(false, _main_screen);
            }
        }
    } else {
        screen = screen->ref();
    }

    return screen;
}

void Application::on_screen_destroyed(int id, bool was_captured)
{
    bool reposition = false;

    if ((int)_screens.size() < id + 1 || !_screens[id]) {
        THROW("no screen");
    }
    if (_active_screen == _screens[id]) {
        _active_screen = NULL;
    }

    if (_full_screen && _screens[id]->has_monitor()) {
        _screens[id]->get_monitor()->restore();
        reposition = true;
    }
    _screens[id] = NULL;
    if (reposition) {
        bool capture = was_captured || release_capture();
        prepare_monitors();
        position_screens();
        if (capture) {
            _main_screen->activate();
            _main_screen->capture_mouse();
        }
    }
}

void Application::on_mouse_motion(int dx, int dy, int buttons_state)
{
    _mouse_handler->on_mouse_motion(dx, dy, buttons_state);
}

void Application::on_mouse_down(int button, int buttons_state)
{
    _mouse_handler->on_mouse_down(button, buttons_state);
}

void Application::on_mouse_up(int button, int buttons_state)
{
    _mouse_handler->on_mouse_up(button, buttons_state);
}

void Application::unpress_all()
{
    reset_sticky();
    for (int i = 0; i < REDKEY_NUM_KEYS; i++) {
        if (_keyboard_state[i]) {
            _key_handler->on_key_up((RedKey)i);
            unpress_key((RedKey)i);
        }
    }
}

void Application::on_connected()
{
}

void Application::on_disconnecting()
{
    release_capture();
}

Menu* Application::get_app_menu()
{
    if (!*_app_menu) {
        return NULL;
    }
    return (*_app_menu)->ref();
}

void Application::do_command(int command)
{
    switch (command) {
    case APP_CMD_SEND_CTL_ALT_DEL:
        send_alt_ctl_del();
        break;
    case APP_CMD_TOGGLE_FULL_SCREEN:
        toggle_full_screen();
        break;
    case APP_CMD_SEND_TOGGLE_KEYS:
        send_command_hotkey(APP_CMD_SEND_TOGGLE_KEYS);
        break;
    case APP_CMD_SEND_RELEASE_KEYS:
        send_command_hotkey(APP_CMD_SEND_RELEASE_KEYS);
        break;
    case APP_CMD_SEND_CTL_ALT_END:
        send_ctrl_alt_end();
        break;
    case APP_CMD_RELEASE_CAPTURE:
        release_capture();
        break;
#ifdef RED_DEBUG
    case APP_CMD_CONNECT:
        do_connect();
        break;
    case APP_CMD_DISCONNECT:
        do_disconnect();
        break;
#endif
    }
}

#ifdef REDKEY_DEBUG

static void show_red_key(RedKey key)
{

#define KEYCASE(red_key)    \
    case red_key:           \
        DBG(0, #red_key);   \
        return;

    switch (key) {
        KEYCASE(REDKEY_INVALID);
        KEYCASE(REDKEY_ESCAPE);
        KEYCASE(REDKEY_1);
        KEYCASE(REDKEY_2);
        KEYCASE(REDKEY_3);
        KEYCASE(REDKEY_4);
        KEYCASE(REDKEY_5);
        KEYCASE(REDKEY_6);
        KEYCASE(REDKEY_7);
        KEYCASE(REDKEY_8);
        KEYCASE(REDKEY_9);
        KEYCASE(REDKEY_0);
        KEYCASE(REDKEY_MINUS);
        KEYCASE(REDKEY_EQUALS);
        KEYCASE(REDKEY_BACKSPACE);
        KEYCASE(REDKEY_TAB);
        KEYCASE(REDKEY_Q);
        KEYCASE(REDKEY_W);
        KEYCASE(REDKEY_E);
        KEYCASE(REDKEY_R);
        KEYCASE(REDKEY_T);
        KEYCASE(REDKEY_Y);
        KEYCASE(REDKEY_U);
        KEYCASE(REDKEY_I);
        KEYCASE(REDKEY_O);
        KEYCASE(REDKEY_P);
        KEYCASE(REDKEY_ENTER);
        KEYCASE(REDKEY_L_BRACKET);
        KEYCASE(REDKEY_R_BRACKET);
        KEYCASE(REDKEY_L_CTRL);
        KEYCASE(REDKEY_A);
        KEYCASE(REDKEY_S);
        KEYCASE(REDKEY_D);
        KEYCASE(REDKEY_F);
        KEYCASE(REDKEY_G);
        KEYCASE(REDKEY_H);
        KEYCASE(REDKEY_J);
        KEYCASE(REDKEY_K);
        KEYCASE(REDKEY_L);
        KEYCASE(REDKEY_SEMICOLON);
        KEYCASE(REDKEY_QUOTE);
        KEYCASE(REDKEY_BACK_QUOTE);
        KEYCASE(REDKEY_L_SHIFT);
        KEYCASE(REDKEY_BACK_SLASH);
        KEYCASE(REDKEY_Z);
        KEYCASE(REDKEY_X);
        KEYCASE(REDKEY_C);
        KEYCASE(REDKEY_V);
        KEYCASE(REDKEY_B);
        KEYCASE(REDKEY_N);
        KEYCASE(REDKEY_M);
        KEYCASE(REDKEY_COMMA);
        KEYCASE(REDKEY_PERIOD);
        KEYCASE(REDKEY_SLASH);
        KEYCASE(REDKEY_R_SHIFT);
        KEYCASE(REDKEY_PAD_MULTIPLY);
        KEYCASE(REDKEY_L_ALT);
        KEYCASE(REDKEY_SPACE);
        KEYCASE(REDKEY_CAPS_LOCK);
        KEYCASE(REDKEY_F1);
        KEYCASE(REDKEY_F2);
        KEYCASE(REDKEY_F3);
        KEYCASE(REDKEY_F4);
        KEYCASE(REDKEY_F5);
        KEYCASE(REDKEY_F6);
        KEYCASE(REDKEY_F7);
        KEYCASE(REDKEY_F8);
        KEYCASE(REDKEY_F9);
        KEYCASE(REDKEY_F10);
        KEYCASE(REDKEY_NUM_LOCK);
        KEYCASE(REDKEY_SCROLL_LOCK);
        KEYCASE(REDKEY_PAD_7);
        KEYCASE(REDKEY_PAD_8);
        KEYCASE(REDKEY_PAD_9);
        KEYCASE(REDKEY_PAD_MINUS);
        KEYCASE(REDKEY_PAD_4);
        KEYCASE(REDKEY_PAD_5);
        KEYCASE(REDKEY_PAD_6);
        KEYCASE(REDKEY_PAD_PLUS);
        KEYCASE(REDKEY_PAD_1);
        KEYCASE(REDKEY_PAD_2);
        KEYCASE(REDKEY_PAD_3);
        KEYCASE(REDKEY_PAD_0);
        KEYCASE(REDKEY_PAD_POINT);
        KEYCASE(REDKEY_F11);
        KEYCASE(REDKEY_F12);
        KEYCASE(REDKEY_PAD_ENTER);
        KEYCASE(REDKEY_R_CTRL);
        KEYCASE(REDKEY_FAKE_L_SHIFT);
        KEYCASE(REDKEY_PAD_DIVIDE);
        KEYCASE(REDKEY_FAKE_R_SHIFT);
        KEYCASE(REDKEY_CTRL_PRINT_SCREEN);
        KEYCASE(REDKEY_R_ALT);
        KEYCASE(REDKEY_CTRL_BREAK);
        KEYCASE(REDKEY_HOME);
        KEYCASE(REDKEY_UP);
        KEYCASE(REDKEY_PAGEUP);
        KEYCASE(REDKEY_LEFT);
        KEYCASE(REDKEY_RIGHT);
        KEYCASE(REDKEY_END);
        KEYCASE(REDKEY_DOWN);
        KEYCASE(REDKEY_PAGEDOWN);
        KEYCASE(REDKEY_INSERT);
        KEYCASE(REDKEY_DELETE);
        KEYCASE(REDKEY_LEFT_CMD);
        KEYCASE(REDKEY_RIGHT_CMD);
        KEYCASE(REDKEY_MENU);
    default:
        DBG(0, "???");
    }
}

#endif

bool Application::press_key(RedKey key)
{
    if (_keyboard_state[key]) {
        return true;
    } else {
        _keyboard_state[key] = true;
        _num_keys_pressed++;
        return false;
    }
}

bool Application::unpress_key(RedKey key)
{
    ASSERT(!_sticky_info.key_down || !is_sticky_trace_key(key));

    if (_keyboard_state[key]) {
        _keyboard_state[key] = false;
        _num_keys_pressed--;
        ASSERT(_num_keys_pressed >= 0);
        return true;
    } else {
        return false;
    }
}

inline bool Application::is_sticky_trace_key(RedKey key)
{
    return ((key == REDKEY_L_ALT) || (key == REDKEY_R_ALT));
}

void Application::reset_sticky()
{
    _sticky_info.trace_is_on = !_splash_mode && _sys_key_intercept_mode;
    _sticky_info.key_first_down = false;
    deactivate_interval_timer(*_sticky_info.timer);
    if (_sticky_info.sticky_mode) {
        ASSERT(_keyboard_state[_sticky_info.key]);
        // if it is physically down, we shouldn't unpress it
        if (!_sticky_info.key_down) {
            do_on_key_up(_sticky_info.key);
        }
        _sticky_info.sticky_mode = false;
        DBG(0, "OFF sticky");
        _gui_layer->set_sticky(false);
    }
    _sticky_info.key_down = false;

    _sticky_info.key = REDKEY_INVALID;

}

void Application::on_key_down(RedKey key)
{
    if (key <= 0 || key >= REDKEY_NUM_KEYS) {
        return;
    }

    bool was_pressed = press_key(key);
    if (_sticky_info.trace_is_on) {
        if (key == _sticky_info.key) {
            _sticky_info.key_down = true;
        }

        if (!_sticky_info.sticky_mode) {
            // during tracing (traced key was pressed and no keyboard event has occured till now)
            if (_sticky_info.key_first_down) {
                ASSERT(_sticky_info.key != REDKEY_INVALID);
                if (key != _sticky_info.key) {
                    reset_sticky();
                }
            } else if (is_sticky_trace_key(key) && (_num_keys_pressed == 1) && !was_pressed) {
                ASSERT(_sticky_info.key == REDKEY_INVALID);
                // start tracing
                _sticky_info.key =  key;
                _sticky_info.key_first_down = true;
                _sticky_info.key_down = true;
                activate_interval_timer(*_sticky_info.timer, STICKY_KEY_TIMEOUT);
            }
        }
    }

    int command = get_hotkeys_commnad();
    if (command != APP_CMD_INVALID) {
        do_command(command);
        return;
    }

#ifdef WIN32
    if (!_active_screen->intercepts_sys_key() &&
                                           (key == REDKEY_LEFT_CMD || key == REDKEY_RIGHT_CMD ||
                                            key == REDKEY_MENU || _keyboard_state[REDKEY_L_ALT])) {
        unpress_key(key);
        return;
    }
    if (!_sticky_info.sticky_mode &&
        ((_keyboard_state[REDKEY_L_CTRL] || _keyboard_state[REDKEY_R_CTRL]) &&
        (_keyboard_state[REDKEY_L_ALT] || _keyboard_state[REDKEY_R_ALT]))) {
        if (key == REDKEY_END || key == REDKEY_PAD_1) {
            unpress_key(key);
            _key_handler->on_key_down(REDKEY_DELETE);
            _key_handler->on_key_up(REDKEY_DELETE);
        } else if (key == REDKEY_DELETE || key == REDKEY_PAD_POINT) {
            unpress_key(key);
            return;
        }
    }
#endif

    _key_handler->on_key_down(key);
}

void Application::do_on_key_up(RedKey key)
{
    unpress_key(key);
    _key_handler->on_key_up(key);
}

void Application::on_key_up(RedKey key)
{
    if(key < 0 || key >= REDKEY_NUM_KEYS || !_keyboard_state[key]) {
        return;
    }

    if (_sticky_info.trace_is_on) {
        ASSERT(_sticky_info.sticky_mode || (key == _sticky_info.key) ||
               (_sticky_info.key == REDKEY_INVALID));
        if (key == _sticky_info.key) {
            _sticky_info.key_down = false;
            if (_sticky_info.key_first_down) {
                _sticky_info.key_first_down = false;
                if (!_sticky_info.sticky_mode) {
                    reset_sticky();
                } else {
                    return; // ignore the sticky-key first release
                }
            }
        }

        if (_sticky_info.sticky_mode) {
            RedKey old_sticky_key = _sticky_info.key;
            reset_sticky();
            if (key == old_sticky_key) {
                return; // no need to send key_up twice
            }
        }
    }

    do_on_key_up(key);
}

void Application::on_char(uint32_t ch)
{
    _key_handler->on_char(ch);
}

void Application::on_deactivate_screen(RedScreen* screen)
{
    if (_active_screen == screen) {
        _sys_key_intercept_mode = false;
        release_capture();
        _active_screen = NULL;
    }
}

void Application::on_activate_screen(RedScreen* screen)
{
    ASSERT(!_active_screen || (_active_screen == screen));
    _active_screen = screen;
}

void Application::on_start_screen_key_interception(RedScreen* screen)
{
    ASSERT(screen == _active_screen);

    _sys_key_intercept_mode = true;
    reset_sticky();
}

void Application::on_stop_screen_key_interception(RedScreen* screen)
{
    ASSERT(screen == _active_screen);

    _sys_key_intercept_mode = false;
    reset_sticky();
}

void Application::on_app_activated()
{
    _active = true;
    _key_handler->on_focus_in();
}

void Application::on_app_deactivated()
{
    _active = false;
    _key_handler->on_focus_out();
#ifdef WIN32
    if (!_changing_screens) {
        exit_full_screen();
    }
#endif
}

void Application::on_screen_unlocked(RedScreen& screen)
{
    if (_full_screen) {
        return;
    }

    screen.resize(SCREEN_INIT_WIDTH, SCREEN_INIT_HEIGHT);
}

bool Application::rearrange_monitors(RedScreen& screen)
{
    if (!_full_screen) {
        return false;
    }
    bool capture = release_capture();
    prepare_monitors();
    position_screens();
    if (capture && _main_screen != &screen) {
        capture = false;
        _main_screen->activate();
        _main_screen->capture_mouse();
    }
    return capture;
}

Monitor* Application::find_monitor(int id)
{
    ASSERT(_monitors);
    std::list<Monitor*>::const_iterator iter = _monitors->begin();
    for (; iter != _monitors->end(); iter++) {
        Monitor *mon = *iter;
        if (mon->get_id() == id) {
            return mon;
        }
    }
    return NULL;
}

Monitor* Application::get_monitor(int id)
{
    Monitor *mon = find_monitor(id);
    if ((mon = find_monitor(id))) {
        mon->set_used();
    }
    return mon;
}

void Application::assign_monitors()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]) {
            ASSERT(!_screens[i]->has_monitor());
            _screens[i]->set_monitor(get_monitor(i));
        }
    }
}

void Application::prepare_monitors()
{
    //todo: test match of monitors size/position against real world size/position
    for (int i = 0; i < (int)_screens.size(); i++) {
        Monitor* mon;
        if (_screens[i] && (mon = _screens[i]->get_monitor())) {

            if (_screens[i]->is_size_locked()) {
                Point size = _screens[i]->get_size();
                mon->set_mode(size.x, size.y);
            } else {
                Point size = mon->get_size();
                _screens[i]->resize(size.x, size.y);
            }
        }
    }
}

void Application::restore_monitors()
{
    //todo: renew monitors (destroy + init)
    std::list<Monitor*>::const_iterator iter = _monitors->begin();
    for (; iter != _monitors->end(); iter++) {
        (*iter)->restore();
    }
}

void Application::position_screens()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        Monitor* mon;
        if (_screens[i] && (mon = _screens[i]->get_monitor())) {
            _screens[i]->position_full_screen(mon->get_position());
        }
    }
}

void Application::hide()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]) {
            _screens[i]->hide();
        }
    }
}

void Application::show()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]) {
            _screens[i]->show();
        }
    }
}

void Application::external_show()
{
    DBG(0, "Entry, _screens.size()=%lu", _screens.size());
    for (size_t i = 0; i < _screens.size(); ++i) {
        DBG(0, "%lu", i);
        if (_screens[i]) {
            _screens[i]->external_show();
        }
    }
}

void Application::show_full_screen()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]) {
            _screens[i]->show_full_screen();
        }
    }
}

void Application::enter_full_screen()
{
    LOG_INFO("");
    _changing_screens = true;
    bool capture = release_capture();
    assign_monitors();
    hide();
    prepare_monitors();
    position_screens();
    show_full_screen();
    _main_screen->activate();
    if (capture) {
        _main_screen->capture_mouse();
    }
    _changing_screens = false;
    _full_screen = true;
}

void Application::restore_screens_size()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]->is_size_locked()) {
            continue;
        }
        _screens[i]->resize(SCREEN_INIT_WIDTH, SCREEN_INIT_HEIGHT);
    }
}

void Application::exit_full_screen()
{
    if (!_full_screen) {
        return;
    }
    LOG_INFO("");
    release_capture();
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]) {
            Monitor* mon;
            _screens[i]->exit_full_screen();
            if ((mon = _screens[i]->get_monitor())) {
                _screens[i]->set_monitor(NULL);
                mon->set_free();
            }
        }
    }
    restore_monitors();
    _full_screen = false;
    restore_screens_size();
    show();
    _main_screen->activate();
}

bool Application::toggle_full_screen()
{
    RedKey shift_pressed = REDKEY_INVALID;

    if (_keyboard_state[REDKEY_L_SHIFT]) {
        shift_pressed = REDKEY_L_SHIFT;
    } else if (_keyboard_state[REDKEY_R_SHIFT]) {
        shift_pressed = REDKEY_R_SHIFT;
    }
    if (_full_screen) {
        exit_full_screen();
    } else {
        enter_full_screen();
    }
    uint32_t modifiers = Platform::get_keyboard_modifiers();
    if ((shift_pressed == REDKEY_L_SHIFT && (modifiers & Platform::L_SHIFT_MODIFIER)) ||
           (shift_pressed == REDKEY_R_SHIFT && (modifiers & Platform::R_SHIFT_MODIFIER))) {
        on_key_down(shift_pressed);
    }
    return _full_screen;
}

void Application::minimize()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]) {
            _screens[i]->minimize();
        }
    }
}

void Application::destroy_monitors()
{
    for (int i = 0; i < (int)_screens.size(); i++) {
        if (_screens[i]) {
            _screens[i]->set_monitor(NULL);
        }
    }
    Platform::destroy_monitors();
    _monitors = NULL;
}

void Application::init_monitors()
{
    exit_full_screen();
    destroy_monitors();
    _monitors = &Platform::init_monitors();
}

void Application::on_monitors_change()
{
    if (Monitor::is_self_change()) {
        return;
    }
    exit_full_screen();
    init_monitors();
}

void Application::on_display_mode_change()
{
    _client.on_display_mode_change();
}

void Application::show_splash(int screen_id)
{
    if (screen_id != 0) {
        return;
    }
    _splash_mode = true;
    release_capture();
    ASSERT(!_sticky_info.trace_is_on);
    (*_gui_layer).set_splash_mode();
}

void Application::hide_splash(int screen_id)
{
    if (screen_id != 0) {
        return;
    }
    _splash_mode = false;
    (*_gui_layer).set_info_mode();
    reset_sticky();
}

uint32_t Application::get_mouse_mode()
{
    return _client.get_mouse_mode();
}

void Application::set_title(std::wstring& title)
{
    _title = title;

    for (size_t i = 0; i < _screens.size(); ++i) {
        if (_screens[i]) {
            _screens[i]->set_name(_title);
        }
    }
}

bool Application::is_key_set_pressed(const HotkeySet& key_set)
{
    HotkeySet::const_iterator iter = key_set.begin();

    while (iter != key_set.end()) {
        if (!(_keyboard_state[iter->main] || _keyboard_state[iter->alter])) {
            break;
        }
        ++iter;
    }

    return iter == key_set.end();
}

int Application::get_hotkeys_commnad()
{
    HotKeys::const_iterator iter = _hot_keys.begin();

    while (iter != _hot_keys.end()) {
        if (is_key_set_pressed(iter->second)) {
            break;
        }
        ++iter;
    }

    return (iter != _hot_keys.end()) ? iter->first : APP_CMD_INVALID;
}

void Application::send_key_down(RedKey key)
{
    _key_handler->on_key_down(key);
}

void Application::send_key_up(RedKey key)
{
    _key_handler->on_key_up(key);
}

void Application::send_alt_ctl_del()
{
    send_key_down(REDKEY_L_CTRL);
    send_key_down(REDKEY_L_ALT);
    send_key_down(REDKEY_DELETE);

    send_key_up(REDKEY_DELETE);
    send_key_up(REDKEY_L_ALT);
    send_key_up(REDKEY_L_CTRL);
}

void Application::send_ctrl_alt_end()
{
    send_key_down(REDKEY_L_CTRL);
    send_key_down(REDKEY_L_ALT);
    send_key_down(REDKEY_END);

    send_key_up(REDKEY_L_CTRL);
    send_key_up(REDKEY_L_ALT);
    send_key_up(REDKEY_END);
}

void Application::send_command_hotkey(int action)
{
    HotKeys::const_iterator iter = _hot_keys.find(action);
    if (iter != _hot_keys.end()) {
        send_hotkey_key_set(iter->second);
    }
}

void Application::send_hotkey_key_set(const HotkeySet& key_set)
{
    HotkeySet::const_iterator iter;

    for (iter = key_set.begin(); iter != key_set.end(); ++iter) {
        send_key_down(iter->main);
    }

    for (iter = key_set.begin(); iter != key_set.end(); ++iter) {
        send_key_up(iter->main);
    }
}

static inline int str_to_port(const char *str)
{
    long port;
    char *endptr;
    port = strtol(str, &endptr, 0);
    if (endptr != str + strlen(str) || port < 0 || port > 0xffff) {
        return -1;
    }
    return port;
}

bool Application::set_channels_security(CmdLineParser& parser, bool on, char *val)
{
    RedPeer::ConnectionOptions::Type option;
    option = (on) ? RedPeer::ConnectionOptions::CON_OP_SECURE :
                    RedPeer::ConnectionOptions::CON_OP_UNSECURE;

    typedef std::map< std::string, int> ChannelsNamesMap;
    ChannelsNamesMap channels_names;
    channels_names["main"] = RED_CHANNEL_MAIN;
    channels_names["display"] = RED_CHANNEL_DISPLAY;
    channels_names["inputs"] = RED_CHANNEL_INPUTS;
    channels_names["cursor"] = RED_CHANNEL_CURSOR;
    channels_names["playback"] = RED_CHANNEL_PLAYBACK;
    channels_names["record"] = RED_CHANNEL_RECORD;
    channels_names["tunnel"] = RED_CHANNEL_TUNNEL;

    if (!strcmp(val, "all")) {
        if ((val = parser.next_argument())) {
            std::cout << "\"all\" is exclusive in secure-channels\n";
            return false;
        }
        PeerConnectionOptMap::iterator iter = _peer_con_opt.begin();
        for (; iter != _peer_con_opt.end(); iter++) {
            (*iter).second = option;
        }
        return true;
    }

    do {
        ChannelsNamesMap::iterator iter = channels_names.find(val);
        if (iter == channels_names.end()) {
            std::cout << "bad channel name \"" << val << "\" in secure-channels\n";
            return false;
        }
        _peer_con_opt[(*iter).second] = option;
    } while ((val = parser.next_argument()));
    return true;
}

bool Application::set_canvas_option(CmdLineParser& parser, char *val)
{
    typedef std::map< std::string, CanvasOption> CanvasNamesMap;
    CanvasNamesMap canvas_types;

    canvas_types["cairo"] = CANVAS_OPTION_CAIRO;
#ifdef WIN32
    canvas_types["gdi"] = CANVAS_OPTION_GDI;
#endif
#ifdef USE_OGL
    canvas_types["gl_fbo"] = CANVAS_OPTION_OGL_FBO;
    canvas_types["gl_pbuff"] = CANVAS_OPTION_OGL_PBUFF;
#endif
    _canvas_types.clear();
    do {
        CanvasNamesMap::iterator iter = canvas_types.find(val);
        if (iter == canvas_types.end()) {
            std::cout << "bad canvas type \"" << val << "\"\n";
            return false;
        }
        _canvas_types.resize(_canvas_types.size() + 1);
        _canvas_types[_canvas_types.size() - 1] = (*iter).second;
    } while ((val = parser.next_argument()));
    return true;
}

bool Application::set_enable_channels(CmdLineParser& parser, bool enable, char *val)
{
    typedef std::map< std::string, int> ChannelsNamesMap;
    ChannelsNamesMap channels_names;
    channels_names["display"] = RED_CHANNEL_DISPLAY;
    channels_names["inputs"] = RED_CHANNEL_INPUTS;
    channels_names["cursor"] = RED_CHANNEL_CURSOR;
    channels_names["playback"] = RED_CHANNEL_PLAYBACK;
    channels_names["record"] = RED_CHANNEL_RECORD;
    channels_names["tunnel"] = RED_CHANNEL_TUNNEL;

    if (!strcmp(val, "all")) {
        if ((val = parser.next_argument())) {
            std::cout << "\"all\" is exclusive\n";
            return false;
        }
        for (unsigned int i = 0; i < _enabled_channels.size(); i++) {
            _enabled_channels[i] = enable;
        }
        return true;
    }

    do {
        ChannelsNamesMap::iterator iter = channels_names.find(val);
        if (iter == channels_names.end()) {
            std::cout << "bad channel name \"" << val << "\"\n";
            return false;
        }
        _enabled_channels[(*iter).second] = enable;
    } while ((val = parser.next_argument()));
    return true;
}

bool Application::process_cmd_line(int argc, char** argv)
{
    std::string host;
    int sport = -1;
    int port = -1;
    bool auto_display_res = false;
    bool full_screen = false;
    std::string password;

    enum {
        SPICE_OPT_HOST = CmdLineParser::OPTION_FIRST_AVILABLE,
        SPICE_OPT_PORT,
        SPICE_OPT_SPORT,
        SPICE_OPT_PASSWORD,
        SPICE_OPT_FULL_SCREEN,
        SPICE_OPT_SECURE_CHANNELS,
        SPICE_OPT_UNSECURE_CHANNELS,
        SPICE_OPT_ENABLE_CHANNELS,
        SPICE_OPT_DISABLE_CHANNELS,
        SPICE_OPT_CANVAS_TYPE,
    };

    CmdLineParser parser("Spice client", false);

    parser.add(SPICE_OPT_HOST, "host", "spice server address", "host", true, 'h');
    parser.set_reqired(SPICE_OPT_HOST);
    parser.add(SPICE_OPT_PORT, "port", "spice server port", "port", true, 'p');
    parser.add(SPICE_OPT_SPORT, "secure-port", "spice server secure port", "port", true, 's');
    parser.add(SPICE_OPT_SECURE_CHANNELS, "secure-channels",
               "force secure connection on the specified channels", "channel",
               true);
    parser.set_multi(SPICE_OPT_SECURE_CHANNELS, ',');
    parser.add(SPICE_OPT_UNSECURE_CHANNELS, "unsecure-channels",
               "force unsecure connection on the specified channels", "channel",
               true);
    parser.set_multi(SPICE_OPT_UNSECURE_CHANNELS, ',');
    parser.add(SPICE_OPT_PASSWORD, "password", "server password", "password", true, 'w');
    parser.add(SPICE_OPT_FULL_SCREEN, "full-screen", "open in full screen mode", "auto-conf",
               false, 'f');

    parser.add(SPICE_OPT_ENABLE_CHANNELS, "enable-channels", "enable channels", "channel", true);
    parser.set_multi(SPICE_OPT_ENABLE_CHANNELS, ',');

    parser.add(SPICE_OPT_DISABLE_CHANNELS, "disable-channels", "disable channels", "channel", true);
    parser.set_multi(SPICE_OPT_DISABLE_CHANNELS, ',');

    parser.add(SPICE_OPT_CANVAS_TYPE, "canvas-type", "set rendering canvas", "canvas_type", true);
    parser.set_multi(SPICE_OPT_CANVAS_TYPE, ',');

    _peer_con_opt[RED_CHANNEL_MAIN] = RedPeer::ConnectionOptions::CON_OP_INVALID;
    _peer_con_opt[RED_CHANNEL_DISPLAY] = RedPeer::ConnectionOptions::CON_OP_INVALID;
    _peer_con_opt[RED_CHANNEL_INPUTS] = RedPeer::ConnectionOptions::CON_OP_INVALID;
    _peer_con_opt[RED_CHANNEL_CURSOR] = RedPeer::ConnectionOptions::CON_OP_INVALID;
    _peer_con_opt[RED_CHANNEL_PLAYBACK] = RedPeer::ConnectionOptions::CON_OP_INVALID;
    _peer_con_opt[RED_CHANNEL_RECORD] = RedPeer::ConnectionOptions::CON_OP_INVALID;
    _peer_con_opt[RED_CHANNEL_TUNNEL] = RedPeer::ConnectionOptions::CON_OP_INVALID;

    parser.begin(argc, argv);

    char* val;
    int op;
    while ((op = parser.get_option(&val)) != CmdLineParser::OPTION_DONE) {
        switch (op) {
        case SPICE_OPT_HOST:
            host = val;
            break;
        case SPICE_OPT_PORT: {
            if ((port = str_to_port(val)) == -1) {
                std::cout << "invalid port " << val << "\n";
                _exit_code = SPICEC_ERROR_CODE_INVALID_ARG;
                return false;
            }
            break;
        }
        case SPICE_OPT_SPORT: {
            if ((port = str_to_port(val)) == -1) {
                std::cout << "invalid secure port " << val << "\n";
                _exit_code = SPICEC_ERROR_CODE_INVALID_ARG;
                return false;
            }
            sport = port;
            break;
        }
        case SPICE_OPT_FULL_SCREEN:
            if (val) {
                if (strcmp(val, "auto-conf")) {
                    std::cout << "invalid full screen mode " << val << "\n";
                    _exit_code = SPICEC_ERROR_CODE_INVALID_ARG;
                    return false;
                }
                auto_display_res = true;
            }
            full_screen = true;
            break;
        case SPICE_OPT_PASSWORD:
            password = val;
            break;
        case SPICE_OPT_SECURE_CHANNELS:
            if (!set_channels_security(parser, true, val)) {
                return false;
            }
            break;
        case SPICE_OPT_UNSECURE_CHANNELS:
            if (!set_channels_security(parser, false, val)) {
                return false;
            }
            break;
        case SPICE_OPT_ENABLE_CHANNELS:
            if (!set_enable_channels(parser, true, val)) {
                std::cout << "invalid channels " << val << "\n";
                _exit_code = SPICEC_ERROR_CODE_INVALID_ARG;
                return false;
            }
            break;
        case SPICE_OPT_DISABLE_CHANNELS:
            if (!set_enable_channels(parser, false, val)) {
                std::cout << "invalid channels " << val << "\n";
                _exit_code = SPICEC_ERROR_CODE_INVALID_ARG;
                return false;
            }
            break;
        case SPICE_OPT_CANVAS_TYPE:
            if (!set_canvas_option(parser, val)) {
                std::cout << "invalid canvas option " << val << "\n";
                _exit_code = SPICEC_ERROR_CODE_INVALID_ARG;
                return false;
            }
            break;
        case CmdLineParser::OPTION_HELP:
            parser.show_help();
            return false;
        case CmdLineParser::OPTION_ERROR:
            return false;
        default:
            throw Exception("cmd line error");
        }
    }

    if (parser.is_set(SPICE_OPT_SECURE_CHANNELS) && !parser.is_set(SPICE_OPT_SPORT)) {
        std::cout << "missing --secure-port\n";
        return false;
    }

    PeerConnectionOptMap::iterator iter = _peer_con_opt.begin();
    for (; iter != _peer_con_opt.end(); iter++) {
        if ((*iter).second == RedPeer::ConnectionOptions::CON_OP_SECURE) {
            continue;
        }

        if ((*iter).second == RedPeer::ConnectionOptions::CON_OP_UNSECURE) {
            continue;
        }

        if (parser.is_set(SPICE_OPT_PORT) && parser.is_set(SPICE_OPT_SPORT)) {
            (*iter).second = RedPeer::ConnectionOptions::CON_OP_BOTH;
            continue;
        }

        if (parser.is_set(SPICE_OPT_PORT)) {
            (*iter).second = RedPeer::ConnectionOptions::CON_OP_UNSECURE;
            continue;
        }

        if (parser.is_set(SPICE_OPT_SPORT)) {
            (*iter).second = RedPeer::ConnectionOptions::CON_OP_SECURE;
            continue;
        }
        std::cout << "missing --port or --sport\n";
        return false;
    }

    if (_enabled_channels[RED_CHANNEL_DISPLAY]) {
        _client.register_channel_factory(DisplayChannel::Factory());
    }

    if (_enabled_channels[RED_CHANNEL_CURSOR]) {
        _client.register_channel_factory(CursorChannel::Factory());
    }

    if (_enabled_channels[RED_CHANNEL_INPUTS]) {
        _client.register_channel_factory(InputsChannel::Factory());
    }

    if (_enabled_channels[RED_CHANNEL_PLAYBACK]) {
        _client.register_channel_factory(PlaybackChannel::Factory());
    }

    if (_enabled_channels[RED_CHANNEL_RECORD]) {
        _client.register_channel_factory(RecordChannel::Factory());
    }

    if (_enabled_channels[RED_CHANNEL_TUNNEL]) {
        _client.register_channel_factory(TunnelChannel::Factory());
    }

    _client.init(host.c_str(), port, sport, password.c_str(), auto_display_res);

    if (full_screen) {
        enter_full_screen();
    } else {
        _main_screen->show(true, NULL);
    }
    return true;
}

void Application::init_logger()
{
    std::string temp_dir_name;
    Platform::get_temp_dir(temp_dir_name);
    std::string log_file_name = temp_dir_name + "spicec.log";

    int fd = ::open(log_file_name.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fd == -1) {
        log4cpp::BasicConfigurator::configure();
        return;
    }
    log4cpp::Category& root = log4cpp::Category::getRoot();
#ifdef RED_DEBUG
    root.setPriority(log4cpp::Priority::DEBUG);
    root.removeAllAppenders();
    ::close(fd);
    root.addAppender(new log4cpp::RollingFileAppender("_", log_file_name));
#else
    root.setPriority(log4cpp::Priority::INFO);
    root.removeAllAppenders();
    root.addAppender(new log4cpp::FileAppender("_", fd));
#endif
}

void Application::init_globals()
{
    init_logger();
    srand((unsigned)time(NULL));

    SSL_library_init();
    SSL_load_error_strings();

    cairo_canvas_init();
#ifdef USE_OGL
    gl_canvas_init();
#endif
    quic_init();
#ifdef WIN32
    gdi_canvas_init();
#endif

#ifdef CAIRO_CANVAS_CACH_IS_SHARED
    MUTEX_INIT(cairo_surface_user_data_mutex);
#endif

    Platform::init();
    RedWindow::init();
}

int Application::main(int argc, char** argv, const char* version_str)
{
    init_globals();
    LOG_INFO("starting %s", version_str);
    std::auto_ptr<Application> app(new Application());
    AutoAbort auto_abort(*app.get());
    if (!app->process_cmd_line(argc, argv)) {
        return app->_exit_code;
    }
    return app->run();
}


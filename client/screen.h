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

#ifndef _H_SCREEN
#define _H_SCREEN

#include "common.h"
#include "region.h"
#include "red_key.h"
#include "GL/gl.h"

#include "red_window.h"
#include "platform.h"
#include "process_loop.h"
#include "threads.h"
#include "utils.h"

class Application;
class ScreenLayer;
class Monitor;
class RedScreen;

enum {
    SCREEN_LAYER_DISPLAY,
    SCREEN_LAYER_CURSOR,
    SCREEN_LAYER_GUI,
};

class UpdateTimer: public Timer {
public:
    UpdateTimer(RedScreen* screen) : _screen (screen) {}
    virtual void response(AbstractProcessLoop& events_loop);
private:
    RedScreen* _screen;
};

class RedScreen: public RedWindow::Listener {
public:
    RedScreen(Application& owner, int id, const std::wstring& name, int width, int height);

    RedScreen* ref();
    void unref();

    void attach_layer(ScreenLayer& layer);
    void detach_layer(ScreenLayer& layer);
    void set_mode(int width, int height, int depth);
    void set_name(const std::wstring& name);
    uint64_t invalidate(const Rect& rect, bool urgent);
    void invalidate(const QRegion &region);
    void relase_inputs();
    void capture_inputs();
    bool is_captured() { return _captured;}
    bool intercepts_sys_key() { return _key_interception;}
    Point get_size() { return _size;}
    bool has_monitor() { return _monitor != 0;}
    void set_monitor(Monitor *monitor) { _monitor = monitor;}
    Monitor* get_monitor() { return _monitor;}
    RedWindow* get_window() { return &_window;}
    void set_cursor(CursorData* cursor);
    void exit_full_screen();
    void minimize();
    void show(bool activate, RedScreen* pos);
    void show_full_screen();
    void position_full_screen(const Point& position);
    void hide();
    void show();
    void activate();
    void external_show();

    int get_id() { return _id;}
    int get_screen_id();

#ifdef USE_OGL
    void untouch_context();
    bool need_recreate_context_gl();
#endif
    void set_update_interrupt_trigger(EventSources::Trigger *trigger);
    bool update_by_interrupt();
    void interrupt_update();
    void set_type_gl();
    void unset_type_gl();

    void update();

private:
    friend class UpdateEvent;
    friend class UpdateTimer;

    virtual ~RedScreen();
    void create_composit_area();

    void destroy_composit_area();
    void erase_background(RedDrawable& dc, const QRegion& region);
    void notify_new_size();
    void adjust_window_rect(int x, int y);
    void save_position();
    bool is_out_of_sync() { return _out_of_sync;}
    void __show_full_screen();

    bool _invalidate(const Rect& rect, bool urgent, uint64_t& update_mark);
    void begin_update(QRegion& direct_rgn, QRegion& composit_rgn, QRegion& frame_rgn);
    void update_composit(QRegion& composit_rgn);
    void draw_direct(RedDrawable& win_dc, QRegion& direct_rgn, QRegion& composit_rgn,
                     QRegion& frame_rgn);
    void activate_timer();
    void update_done();
    void periodic_update();
    bool is_dirty() {return !region_is_empty(&_dirty_region);}
    void composit_to_screen(RedDrawable& win_dc, const QRegion& region);

    void reset_mouse_pos();
    bool mouse_is_captured() { return _captured;}
    void update_active_cursor();

    virtual void on_exposed_rect(const Rect& area);
    virtual void on_mouse_motion(int x, int y, unsigned int buttons_state);
    virtual void on_key_press(RedKey key);
    virtual void on_key_release(RedKey key);
    virtual void on_button_press(RedButton button, unsigned int buttons_state);
    virtual void on_button_release(RedButton button, unsigned int buttons_state);
    virtual void on_deactivate();
    virtual void on_activate();
    virtual void on_pointer_enter();
    virtual void on_pointer_leave();
    virtual void on_start_key_interception();
    virtual void on_stop_key_interception();
    virtual void enter_modal_loop();
    virtual void exit_modal_loop();

    virtual void pre_migrate();
    virtual void post_migrate();

private:
    Application& _owner;
    int _id;
    int _refs;
    std::wstring _name;
    RedWindow _window;
    std::vector<ScreenLayer*> _layes;
    QRegion _dirty_region;
    RecurciveMutex _update_lock;
    bool _active;
    bool _captured;
    bool _full_screen;
    bool _out_of_sync;
    bool _frame_area;
    bool _cursor_visible;
    bool _periodic_update;
    bool _key_interception;
    bool _update_by_timer;
    int _forec_update_timer;
    AutoRef<UpdateTimer> _update_timer;
    RedDrawable* _composit_area;
    uint64_t _update_mark;

    Point _size;
    Point _origin;
    Point _mouse_anchor_point;
    Point _save_pos;
    Monitor* _monitor;

    LocalCursor* _default_cursor;
    LocalCursor* _active_cursor;
    LocalCursor* _inactive_cursor;

    enum PointerLocation {
        POINTER_IN_ACTIVE_AREA,
        POINTER_IN_FRAME_AREA,
        POINTER_OUTSIDE_WINDOW,
    };
    PointerLocation _pointer_location;
    int _pixel_format_index;
    EventSources::Trigger *_update_interrupt_trigger;
};

#endif


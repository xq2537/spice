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
#include "screen.h"
#include "application.h"
#include "screen_layer.h"
#include "utils.h"
#include "debug.h"
#include "monitor.h"
#include "red_pixmap_cairo.h"
#include "resource.h"
#include "icon.h"

class UpdateEvent: public Event {
public:
    UpdateEvent(int screen) : _screen (screen) {}

    virtual void response(AbstractProcessLoop& events_loop)
    {
        Application* app = static_cast<Application*>(events_loop.get_owner());
        RedScreen* screen = app->find_screen(_screen);
        if (screen) {
            screen->update();
        }
    }

private:
    int _screen;
};

void UpdateTimer::response(AbstractProcessLoop& events_loop)
{
    _screen->periodic_update();
}

RedScreen::RedScreen(Application& owner, int id, const std::wstring& name, int width, int height)
    : _owner (owner)
    , _id (id)
    , _refs (1)
    , _window (*this)
    , _active (false)
    , _captured (false)
    , _full_screen (false)
    , _out_of_sync (false)
    , _frame_area (false)
    , _cursor_visible (true)
    , _periodic_update (false)
    , _key_interception (false)
    , _update_by_timer (true)
    , _forec_update_timer (0)
    , _update_timer (new UpdateTimer(this))
    , _composit_area (NULL)
    , _update_mark (1)
    , _monitor (NULL)
    , _default_cursor (NULL)
    , _active_cursor (NULL)
    , _inactive_cursor (NULL)
    , _pointer_location (POINTER_OUTSIDE_WINDOW)
    , _pixel_format_index (0)
    , _update_interrupt_trigger (NULL)
{
    region_init(&_dirty_region);
    set_name(name);
    _size.x = width;
    _size.y = height;
    _origin.x = _origin.y = 0;
    create_composit_area();
    _window.resize(_size.x, _size.y);
    save_position();
    if ((_default_cursor = Platform::create_default_cursor()) == NULL) {
        THROW("create default cursor failed");
    }
    if ((_inactive_cursor = Platform::create_inactive_cursor()) == NULL) {
        THROW("create inactive cursor failed");
    }
    _window.set_cursor(_default_cursor);
    AutoRef<Menu> menu(_owner.get_app_menu());
    _window.set_menu(*menu);
    AutoRef<Icon> icon(Platform::load_icon(RED_ICON_RES_ID));
    _window.set_icon(*icon);
}

RedScreen::~RedScreen()
{
    bool captured = is_captured();
    relase_inputs();
    destroy_composit_area();
    _owner.deactivate_interval_timer(*_update_timer);
    _owner.on_screen_destroyed(_id, captured);
    region_destroy(&_dirty_region);
    if (_default_cursor) {
        _default_cursor->unref();
    }
    if (_active_cursor) {
        _active_cursor->unref();
    }
    if (_inactive_cursor) {
        _inactive_cursor->unref();
    }
}

void RedScreen::show(bool activate, RedScreen* pos)
{
    _window.position_after((pos) ? &pos->_window : NULL);
    show();
    if (activate) {
        _window.activate();
    }
}

RedScreen* RedScreen::ref()
{
    _refs++;
    return this;
}

void RedScreen::unref()
{
    if (!--_refs) {
        delete this;
    }
}

void RedScreen::destroy_composit_area()
{
    if (_composit_area) {
        delete _composit_area;
        _composit_area = NULL;
    }
}

void RedScreen::create_composit_area()
{
    destroy_composit_area();
    _composit_area = new RedPixmapCairo(_size.x, _size.y, RedPixmap::RGB32,
                                        false, NULL, NULL);
}

void RedScreen::adjust_window_rect(int x, int y)
{
    _window.move_and_resize(x, y, _size.x, _size.y);
}

void RedScreen::set_mode(int width, int height, int depth)
{
    RecurciveLock lock(_update_lock);
    _size.x = width;
    _size.y = height;
    create_composit_area();
    if (_full_screen) {
        bool cuptur = _owner.rearrange_monitors(*this);
        __show_full_screen();
        if (cuptur) {
            capture_inputs();
        }
    } else {
        _window.resize(_size.x, _size.y);
    }
    notify_new_size();
}

void RedScreen::set_name(const std::wstring& name)
{
    if (!name.empty()) {
        wstring_printf(_name, name.c_str(), _id);
    }
    _window.set_title(_name);
}

void RedScreen::attach_layer(ScreenLayer& layer)
{
    RecurciveLock lock(_update_lock);
    int order = layer.z_order();

    if ((int)_layes.size() < order + 1) {
        _layes.resize(order + 1);
    }
    if (_layes[order]) {
        THROW("layer in use");
    }
    layer.set_screen(this);
    _layes[order] = &layer;
    ref();
    lock.unlock();
    layer.invalidate();
}

void RedScreen::detach_layer(ScreenLayer& layer)
{
    RecurciveLock lock(_update_lock);
    int order = layer.z_order();

    if ((int)_layes.size() < order + 1 || _layes[order] != &layer) {
        THROW("not found");
    }
    QRegion layer_area;
    region_clone(&layer_area, &layer.area());
    _layes[order]->set_screen(NULL);
    _layes[order] = NULL;
    lock.unlock();
    invalidate(layer_area);
    region_destroy(&layer_area);
    unref();
}

void RedScreen::composit_to_screen(RedDrawable& win_dc, const QRegion& region)
{
    for (int i = 0; i < (int)region.num_rects; i++) {
        Rect* r = &region.rects[i];
        win_dc.copy_pixels(*_composit_area, r->left, r->top, *r);
    }
}

void RedScreen::notify_new_size()
{
    for (int i = 0; i < (int)_layes.size(); i++) {
        if (!_layes[i]) {
            continue;
        }
        _layes[i]->on_size_changed();
    }
}

inline void RedScreen::begin_update(QRegion& direct_rgn, QRegion& composit_rgn,
                                    QRegion& frame_rgn)
{
    region_init(&composit_rgn);
    RecurciveLock lock(_update_lock);
    region_clone(&direct_rgn, &_dirty_region);
    region_clear(&_dirty_region);
    _update_mark++;
    lock.unlock();

    QRegion rect_rgn;
    Rect r;
    r.top = r.left = 0;
    r.right = _size.x;
    r.bottom = _size.y;
    region_init(&rect_rgn);
    region_add(&rect_rgn, &r);

    if (_frame_area) {
        region_clone(&frame_rgn, &direct_rgn);
        region_exclude(&frame_rgn, &rect_rgn);
    }
    region_and(&direct_rgn, &rect_rgn);
    region_destroy(&rect_rgn);

    for (int i = _layes.size() - 1; i >= 0; i--) {
        ScreenLayer* layer;

        if (!(layer = _layes[i])) {
            continue;
        }
        layer->begin_update(direct_rgn, composit_rgn);
    }
}

inline void RedScreen::update_done()
{
    for (unsigned int i = 0; i < _layes.size(); i++) {
        ScreenLayer* layer;

        if (!(layer = _layes[i])) {
            continue;
        }
        layer->on_update_completion(_update_mark - 1);
    }
}

inline void RedScreen::update_composit(QRegion& composit_rgn)
{
    erase_background(*_composit_area, composit_rgn);
    for (int i = 0; i < (int)_layes.size(); i++) {
        ScreenLayer* layer;

        if (!(layer = _layes[i])) {
            continue;
        }
        QRegion& dest_region = layer->composit_area();
        region_or(&composit_rgn, &dest_region);
        layer->copy_pixels(dest_region, *_composit_area);
    }
}

inline void RedScreen::draw_direct(RedDrawable& win_dc, QRegion& direct_rgn, QRegion& composit_rgn,
                                   QRegion& frame_rgn)
{
    erase_background(win_dc, direct_rgn);

    if (_frame_area) {
        erase_background(win_dc, frame_rgn);
        region_destroy(&frame_rgn);
    }

    for (int i = 0; i < (int)_layes.size(); i++) {
        ScreenLayer* layer;

        if (!(layer = _layes[i])) {
            continue;
        }
        QRegion& dest_region = layer->direct_area();
        layer->copy_pixels(dest_region, win_dc);
    }
}

void RedScreen::periodic_update()
{
    bool need_update;
    RecurciveLock lock(_update_lock);
    if (is_dirty()) {
        need_update = true;
    } else {
        if (!_forec_update_timer) {
            _owner.deactivate_interval_timer(*_update_timer);
            _periodic_update = false;
        }
        need_update = false;
    }
    lock.unlock();

    if (need_update) {
        if (update_by_interrupt()) {
            interrupt_update();
        } else {
            update();
        }
    }
}

void RedScreen::activate_timer()
{
    RecurciveLock lock(_update_lock);
    if (_periodic_update) {
        return;
    }
    _periodic_update = true;
    lock.unlock();
    _owner.activate_interval_timer(*_update_timer, 1000 / 30);
}

void RedScreen::update()
{
    if (is_out_of_sync()) {
        return;
    }

    QRegion direct_rgn;
    QRegion composit_rgn;
    QRegion frame_rgn;

    begin_update(direct_rgn, composit_rgn, frame_rgn);
    update_composit(composit_rgn);
    draw_direct(_window, direct_rgn, composit_rgn, frame_rgn);
    composit_to_screen(_window, composit_rgn);
    update_done();
    region_destroy(&direct_rgn);
    region_destroy(&composit_rgn);

    if (_update_by_timer) {
        activate_timer();
    }
}

bool RedScreen::_invalidate(const Rect& rect, bool urgent, uint64_t& update_mark)
{
    RecurciveLock lock(_update_lock);
    bool update_triger = !is_dirty() && (urgent || !_periodic_update);
    region_add(&_dirty_region, &rect);
    update_mark = _update_mark;
    return update_triger;
}

uint64_t RedScreen::invalidate(const Rect& rect, bool urgent)
{
    uint64_t update_mark;
    if (_invalidate(rect, urgent, update_mark)) {
        if (!urgent && _update_by_timer) {
            activate_timer();
        } else {
            if (update_by_interrupt()) {
                interrupt_update();
            } else {
                AutoRef<UpdateEvent> update_event(new UpdateEvent(_id));
                _owner.push_event(*update_event);
            }
        }
    }
    return update_mark;
}

void RedScreen::invalidate(const QRegion &region)
{
    Rect *r = region.rects;
    Rect *end = r + region.num_rects;
    while (r != end) {
        invalidate(*r++, false);
    }
}

inline void RedScreen::erase_background(RedDrawable& dc, const QRegion& composit_rgn)
{
    for (int i = 0; i < (int)composit_rgn.num_rects; i++) {
        dc.fill_rect(composit_rgn.rects[i], 0);
    }
}

void RedScreen::reset_mouse_pos()
{
    _window.set_mouse_position(_mouse_anchor_point.x, _mouse_anchor_point.y);
}

void RedScreen::capture_inputs()
{
    if (_captured || !_window.get_mouse_anchor_point(_mouse_anchor_point)) {
        return;
    }
    if (_owner.get_mouse_mode() == RED_MOUSE_MODE_SERVER) {
        _window.hide_cursor();
        reset_mouse_pos();
        _window.cupture_mouse();
    }
#ifndef NO_KEY_GRAB
    _window.start_key_interception();
#endif
    _captured = true;
}

void RedScreen::relase_inputs()
{
    if (!_captured) {
        return;
    }
#ifndef NO_KEY_GRAB
    _window.stop_key_interception();
#endif
    _captured = false;
    _window.release_mouse();
    if (_owner.get_mouse_mode() == RED_MOUSE_MODE_SERVER) {
        _window.set_cursor(_default_cursor);
    }
}

void RedScreen::set_cursor(CursorData* cursor)
{
    if (cursor) {
        if (_active_cursor) {
            _active_cursor->unref();
        }
        if (!cursor->get_local()) {
            AutoRef<LocalCursor> cur(Platform::create_local_cursor(cursor));
            if (*cur == NULL) {
                THROW("create local cursor failed");
            }
            cursor->set_local(*cur);
            _active_cursor = (*cur)->ref();
        } else {
            _active_cursor = (cursor->get_local())->ref();
        }
    }
    _cursor_visible = !!cursor;
    update_active_cursor();
}

void RedScreen::update_active_cursor()
{
    if (_owner.get_mouse_mode() == RED_MOUSE_MODE_CLIENT &&
                                                      _pointer_location == POINTER_IN_ACTIVE_AREA) {
        if (_cursor_visible && _active_cursor) {
            _window.set_cursor(_active_cursor);
        } else {
            _window.hide_cursor();
        }
    }
}

void RedScreen::on_mouse_motion(int x, int y, unsigned int buttons_state)
{
    switch (_owner.get_mouse_mode()) {
    case RED_MOUSE_MODE_CLIENT:
        if (!_frame_area) {
            _owner.on_mouse_position(x, y, buttons_state, _id);
        } else if (x >= 0 && x < _size.x && y >= 0 && y < _size.y) {
            _owner.on_mouse_position(x, y, buttons_state, _id);
            if (_pointer_location != POINTER_IN_ACTIVE_AREA) {
                _pointer_location = POINTER_IN_ACTIVE_AREA;
                update_active_cursor();
            }
        } else if (_pointer_location != POINTER_IN_FRAME_AREA) {
            _pointer_location = POINTER_IN_FRAME_AREA;
            _window.set_cursor(_inactive_cursor);
        }
        break;
    case RED_MOUSE_MODE_SERVER:
        if (_captured && (x != _mouse_anchor_point.x || y != _mouse_anchor_point.y)) {
            _owner.on_mouse_motion(x - _mouse_anchor_point.x,
                                   y - _mouse_anchor_point.y,
                                   buttons_state);
            reset_mouse_pos();
        }
        break;
    default:
        THROW("invalid mouse mode");
    }
}

void RedScreen::on_button_press(RedButton button, unsigned int buttons_state)
{
    if (_owner.get_mouse_mode() == RED_MOUSE_MODE_CLIENT &&
                                                      _pointer_location != POINTER_IN_ACTIVE_AREA) {
        return;
    }
    if (!mouse_is_captured()) {
        if (_owner.get_mouse_mode() == RED_MOUSE_MODE_SERVER && button != REDC_MOUSE_LBUTTON) {
            return;
        }
        capture_inputs();
        if (_owner.get_mouse_mode() == RED_MOUSE_MODE_SERVER) {
            return;
        }
    }
    _owner.on_mouse_down(button, buttons_state);
}

void RedScreen::on_button_release(RedButton button, unsigned int buttons_state)
{
    if (!mouse_is_captured()) {
        if (_owner.get_mouse_mode() == RED_MOUSE_MODE_SERVER) {
            return;
        }
        capture_inputs();
    }
    _owner.on_mouse_up(button, buttons_state);
}

void RedScreen::on_key_press(RedKey key)
{
    _owner.on_key_down(key);
}

void RedScreen::on_key_release(RedKey key)
{
    _owner.on_key_up(key);
}

void RedScreen::on_deactivate()
{
    relase_inputs();
    _active = false;
    _owner.on_deactivate_screen(this);
}

void RedScreen::on_activate()
{
    _active = true;
    _owner.on_activate_screen(this);
}

void RedScreen::on_pointer_enter()
{
    if (!_frame_area) {
        _pointer_location = POINTER_IN_ACTIVE_AREA;
        update_active_cursor();
    }
}

void RedScreen::on_pointer_leave()
{
    _pointer_location = POINTER_OUTSIDE_WINDOW;
}

void RedScreen::enter_modal_loop()
{
    _forec_update_timer++;
    activate_timer();
}

void RedScreen::exit_modal_loop()
{
    ASSERT(_forec_update_timer > 0)
    _forec_update_timer--;
}

void RedScreen::pre_migrate()
{
    for (int i = 0; i < (int)_layes.size(); i++) {
        if (!_layes[i]) {
            continue;
        }
        _layes[i]->pre_migrate();
    }
}

void RedScreen::post_migrate()
{
    for (int i = 0; i < (int)_layes.size(); i++) {
        if (!_layes[i]) {
            continue;
        }
        _layes[i]->post_migrate();
    }
}

void RedScreen::exit_full_screen()
{
    if (!_full_screen) {
        return;
    }
    RecurciveLock lock(_update_lock);
    _window.hide();
    region_clear(&_dirty_region);
    _window.set_type(RedWindow::TYPE_NORMAL);
    adjust_window_rect(_save_pos.x, _save_pos.y);
    _origin.x = _origin.y = 0;
    _window.set_origin(0, 0);
    show();
    _full_screen = false;
    _out_of_sync = false;
    _frame_area = false;
}

void RedScreen::save_position()
{
    _save_pos = _window.get_position();
}

void RedScreen::__show_full_screen()
{
    if (!_monitor) {
        hide();
        return;
    }
    Point position = _monitor->get_position();
    Point monitor_size = _monitor->get_size();
    _frame_area = false;
    region_clear(&_dirty_region);
    _window.set_type(RedWindow::TYPE_FULLSCREEN);
    _window.move_and_resize(position.x, position.y, monitor_size.x, monitor_size.y);

    if (!(_out_of_sync = _monitor->is_out_of_sync())) {
        ASSERT(monitor_size.x >= _size.x);
        ASSERT(monitor_size.y >= _size.y);
        _origin.x = (monitor_size.x - _size.x) / 2;
        _origin.y = (monitor_size.y - _size.y) / 2;
        _frame_area = monitor_size.x != _size.x || monitor_size.y != _size.y;
    } else {
        _origin.x = _origin.y = 0;
    }
    _window.set_origin(_origin.x, _origin.y);
    show();
}

void RedScreen::show_full_screen()
{
    if (_full_screen) {
        return;
    }
    RecurciveLock lock(_update_lock);
    hide();
    save_position();
    _full_screen = true;
    __show_full_screen();
}

void RedScreen::minimize()
{
    _window.minimize();
}

void RedScreen::position_full_screen(const Point& position)
{
    if (!_full_screen) {
        return;
    }

    _window.move(position.x, position.y);
}

void RedScreen::hide()
{
    _window.hide();
}

void RedScreen::show()
{
    RecurciveLock lock(_update_lock);
    _window.show(_monitor ? _monitor->get_screen_id() : 0);
}

void RedScreen::activate()
{
    _window.activate();
}

void RedScreen::external_show()
{
    DBG(0, "Entry");
    _window.external_show();
}

void RedScreen::on_exposed_rect(const Rect& area)
{
    if (is_out_of_sync()) {
        _window.fill_rect(area, rgb32_make(0xff, 0xff, 0xff));
        return;
    }
    invalidate(area, false);
}

int RedScreen::get_screen_id()
{
    return _monitor ? _monitor->get_screen_id() : 0;
}

#ifdef USE_OGL
void RedScreen::untouch_context()
{
    _window.untouch_context();
}

bool RedScreen::need_recreate_context_gl()
{
    if (_full_screen) {
        return true;
    }
    return false;
}

#endif

void RedScreen::set_update_interrupt_trigger(EventSources::Trigger *trigger)
{
    _update_interrupt_trigger = trigger;
}

bool RedScreen::update_by_interrupt()
{
    return _update_interrupt_trigger != NULL;
}

void RedScreen::interrupt_update()
{
    _update_interrupt_trigger->trigger();
}

void RedScreen::set_type_gl()
{
    _window.set_type_gl();
}

void RedScreen::unset_type_gl()
{
    _window.unset_type_gl();
}


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
#include "cursor_channel.h"
#include "cursor.h"
#include "red_client.h"
#include "application.h"
#include "debug.h"
#include "utils.h"
#include "screen.h"
#include "red_pixmap_cairo.h"
#include "rect.h"

static inline uint8_t revers_bits(uint8_t byte)
{
    uint8_t ret = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int shift = 7 - i * 2;
        ret |= (byte & (1 << i)) << shift;
        ret |= (byte & (0x80 >> i)) >> shift;
    }
    return ret;
}

class NaitivCursor: public CursorOpaque {
public:
    virtual void draw(RedDrawable& dest, int x, int y, const Rect& area) = 0;
};

class AlphaCursor: public NaitivCursor {
public:
    AlphaCursor(const CursorHeader& header, const uint8_t* data);

    virtual void draw(RedDrawable& dest, int x, int y, const Rect& area);

private:
    std::auto_ptr<RedPixmap> _pixmap;
};

class MonoCursor: public NaitivCursor {
public:
    MonoCursor(const CursorHeader& header, const uint8_t* data);

    virtual void draw(RedDrawable& dest, int x, int y, const Rect& area);

private:
    std::auto_ptr<RedPixmap> _pixmap;
    int _height;
};

class UnsupportedCursor: public NaitivCursor {
public:
    UnsupportedCursor(const CursorHeader& header);
    virtual void draw(RedDrawable& dest, int x, int y, const Rect& area);

private:
    int _hot_x;
    int _hot_y;
};

UnsupportedCursor::UnsupportedCursor(const CursorHeader& header)
    : _hot_x (header.hot_spot_x)
    , _hot_y (header.hot_spot_y)
{
    LOG_WARN("Unsupported cursor %hu", header.type);
}

void UnsupportedCursor::draw(RedDrawable& dest, int x, int y, const Rect& area)
{
    Rect dest_area;
    Rect rect;

    dest_area.left = area.left;
    dest_area.right = area.right;
    dest_area.top = area.top;
    dest_area.bottom = area.bottom;

    rect.left = x + _hot_x - 2;
    rect.right = rect.left + 8;
    rect.top = y + _hot_y - 2;
    rect.bottom = rect.top + 8;
    rect_sect(rect, dest_area);

    dest.fill_rect(rect, rgb32_make(0xf8, 0xf1, 0xb8));

    rect.left = x + _hot_x - 1;
    rect.right = rect.left + 6;
    rect.top = y + _hot_y - 1;
    rect.bottom = rect.top + 6;
    rect_sect(rect, dest_area);

    dest.frame_rect(rect, rgb32_make(0, 0, 0));
}

AlphaCursor::AlphaCursor(const CursorHeader& header, const uint8_t* data)
    : _pixmap (new RedPixmapCairo(header.width, header.height,
                                  RedPixmap::ARGB32, true, NULL, NULL))
{
    int stride = _pixmap->get_stride();
    uint8_t* dest = _pixmap->get_data();
    int line_size = header.width * sizeof(uint32_t);
    for (int i = 0; i < header.height; i++, data += line_size, dest += stride) {
        memcpy(dest, data, line_size);
    }
}

void AlphaCursor::draw(RedDrawable& dest, int x, int y, const Rect& area)
{
    dest.blend_pixels(*_pixmap, area.left - x, area.top - y, area);
}

MonoCursor::MonoCursor(const CursorHeader& header, const uint8_t* data)
    : _pixmap (NULL)
    , _height (header.height)
{
    rgb32_t pallete[2] = { rgb32_make(0x00, 0x00, 0x00), rgb32_make(0xff, 0xff, 0xff)};
    _pixmap.reset(new RedPixmapCairo(header.width, _height * 2, RedPixmap::A1,
                                     true, pallete, NULL));

    int dest_stride = _pixmap->get_stride();
    uint8_t *dest_line = _pixmap->get_data();
    int src_stride = ALIGN(header.width, 8) >> 3;
    const uint8_t* src_line = data;
    const uint8_t* end_line = src_line + _pixmap->get_height() * src_stride;

    if (_pixmap->is_big_endian_bits()) {
        for (; src_line < end_line; src_line += src_stride, dest_line += dest_stride) {
            memcpy(dest_line, src_line, src_stride);
        }
    } else {
        for (; src_line < end_line; src_line += src_stride, dest_line += dest_stride) {
            for (int i = 0; i < src_stride; i++) {
                dest_line[i] = revers_bits(src_line[i]);
            }
        }
    }
}

void MonoCursor::draw(RedDrawable& dest, int x, int y, const Rect& area)
{
    dest.combine_pixels(*_pixmap, area.left - x, area.top - y, area, RedDrawable::OP_AND);
    dest.combine_pixels(*_pixmap, area.left - x, area.top - y + _height, area, RedDrawable::OP_XOR);
}

class ColorCursor: public NaitivCursor {
public:
    ColorCursor(const CursorHeader& header);

    virtual void draw(RedDrawable& dest, int x, int y, const Rect& area);

protected:
    void init_pixels(const CursorHeader& header, const uint8_t* _pixels, const uint8_t *and_mask);
    virtual uint32_t get_pixel_color(const uint8_t *data, int row, int col) = 0;

private:
    std::auto_ptr<RedPixmap> _pixmap;
    std::auto_ptr<RedPixmap> _invers;
};

ColorCursor::ColorCursor(const CursorHeader& header)
    : _pixmap (new RedPixmapCairo(header.width, header.height,
                                  RedPixmap::ARGB32, true, NULL, NULL))
    , _invers (NULL)
{
    rgb32_t pallete[2] = { rgb32_make(0x00, 0x00, 0x00), rgb32_make(0xff, 0xff, 0xff)};
    _invers.reset(new RedPixmapCairo(header.width, header.height, RedPixmap::A1,
                                     true, pallete, NULL));
}

void ColorCursor::init_pixels(const CursorHeader& header, const uint8_t* pixels,
                              const uint8_t *and_mask)
{
    int mask_stride = ALIGN(header.width, 8) / 8;
    int invers_stride = _invers->get_stride();
    int pixmap_stride = _pixmap->get_stride();
    uint8_t *_pixmap_line = _pixmap->get_data();
    uint8_t* invers_line = _invers->get_data();
    bool be_bits = _invers->is_big_endian_bits();
    memset(invers_line, 0, header.height * invers_stride);
    for (int i = 0; i < header.height; i++, and_mask += mask_stride, invers_line += invers_stride,
                                            _pixmap_line += pixmap_stride) {
        uint32_t *line_32 = (uint32_t *)_pixmap_line;
        for (int j = 0; j < header.width; j++) {
            uint32_t pixel_val = get_pixel_color(pixels, i, j);
            int and_val = test_bit_be(and_mask, j);
            if ((pixel_val & 0x00ffffff) == 0 && and_val) {
                line_32[j] = 0;
            } else if ((pixel_val & 0x00ffffff) == 0x00ffffff && and_val) {
                line_32[j] = 0;
                if (be_bits) {
                    set_bit_be(invers_line, j);
                } else {
                    set_bit(invers_line, j);
                }
            } else {
                line_32[j] = pixel_val | 0xff000000;
            }
        }
    }
}

void ColorCursor::draw(RedDrawable& dest, int x, int y, const Rect& area)
{
    dest.blend_pixels(*_pixmap, area.left - x, area.top - y, area);
    dest.combine_pixels(*_invers, area.left - x, area.top - y, area, RedDrawable::OP_XOR);
}

class ColorCursor32: public ColorCursor {
public:
    ColorCursor32(const CursorHeader& header, const uint8_t* data)
        : ColorCursor(header)
        , _src_stride (header.width * sizeof(uint32_t))
    {
        init_pixels(header, data, data + _src_stride * header.height);
    }

private:
    uint32_t get_pixel_color(const uint8_t *data, int row, int col)
    {
        return *((uint32_t *)(data + row * _src_stride) + col);
    }

private:
    int _src_stride;
};

class ColorCursor16: public ColorCursor {
public:
    ColorCursor16(const CursorHeader& header, const uint8_t* data)
        : ColorCursor(header)
        , _src_stride (header.width * sizeof(uint16_t))
    {
        init_pixels(header, data, data + _src_stride * header.height);
    }

private:
    uint32_t get_pixel_color(const uint8_t *data, int row, int col)
    {
        uint32_t pix = *((uint16_t*)(data + row * _src_stride) + col);
        return ((pix & 0x1f) << 3) | ((pix & 0x3e0) << 6) | ((pix & 0x7c00) << 9);
    }

private:
    int _src_stride;
};

class ColorCursor4: public ColorCursor {
public:
    ColorCursor4(const CursorHeader& header, const uint8_t* data)
        : ColorCursor(header)
        , _src_stride (ALIGN(header.width, 2) >> 1)
        , _palette ((uint32_t*)(data + _src_stride * header.height))
    {
        init_pixels(header, data, (uint8_t*)(_palette + 16));
    }

private:
    uint32_t get_pixel_color(const uint8_t *data, int row, int col)
    {
        data += _src_stride * row + (col >> 1);
        return (col & 1) ? _palette[*data & 0x0f] : _palette[*data >> 4];
    }

private:
    int _src_stride;
    uint32_t* _palette;
};

class CursorSetEvent: public Event {
public:
    CursorSetEvent(CursorChannel& channel, CursorData *cursor, int x, int y, bool visable)
        : _channel (channel)
        , _cursor (cursor->ref())
        , _x (x)
        , _y (y)
        , _visible (visable)
    {
    }

    void create_cursor()
    {
        CursorData *cursor = *_cursor;
        CursorOpaque* native_cursor = cursor->get_opaque();

        if (native_cursor) {
            return;
        }

        switch (cursor->header().type) {
        case CURSOR_TYPE_ALPHA:
            native_cursor = new AlphaCursor(cursor->header(), cursor->data());
            break;
        case CURSOR_TYPE_COLOR32:
            native_cursor = new ColorCursor32(cursor->header(), cursor->data());
            break;
        case CURSOR_TYPE_MONO:
            native_cursor = new MonoCursor(cursor->header(), cursor->data());
            break;
        case CURSOR_TYPE_COLOR4:
            native_cursor = new ColorCursor4(cursor->header(), cursor->data());
            break;
        case CURSOR_TYPE_COLOR8:
            native_cursor = new UnsupportedCursor(cursor->header());
            break;
        case CURSOR_TYPE_COLOR16:
            native_cursor = new ColorCursor16(cursor->header(), cursor->data());
            break;
        case CURSOR_TYPE_COLOR24:
            native_cursor = new UnsupportedCursor(cursor->header());
            break;
        default:
            THROW("invalid curosr type");
        }
        cursor->set_opaque(native_cursor);
    }

    virtual void response(AbstractProcessLoop& events_loop)
    {
        CursorData *cursor = *_cursor;
        create_cursor();
        Lock lock(_channel._update_lock);

        _channel._hot_pos.x = _x;
        _channel._hot_pos.y = _y;
        _channel._cursor_visible = _visible;
        _channel._cursor_rect.left = _x - cursor->header().hot_spot_x;
        _channel._cursor_rect.right = _channel._cursor_rect.left + cursor->header().width;
        _channel._cursor_rect.top = _y - cursor->header().hot_spot_y;
        _channel._cursor_rect.bottom = _channel._cursor_rect.top + cursor->header().height;

        if (static_cast<Application*>(events_loop.get_owner())->get_mouse_mode() ==
            RED_MOUSE_MODE_CLIENT) {
            RedScreen* screen = _channel.screen();
            ASSERT(screen);
            screen->set_cursor(_visible ? cursor : NULL);
        } else {
            if (_visible) {
                _channel.set_rect_area(_channel._cursor_rect);
            } else {
                _channel.clear_area();
            }
        }

        if (_channel._cursor) {
            _channel._cursor->unref();
        }

        _channel._cursor = cursor->ref();
    }

private:
    CursorChannel& _channel;
    AutoRef<CursorData> _cursor;
    int _x;
    int _y;
    bool _visible;
};

class CursorMoveEvent: public Event {
public:
    CursorMoveEvent(CursorChannel& channel, int x, int y)
        : _channel (channel)
        , _x (x)
        , _y (y)
    {
    }

    virtual void response(AbstractProcessLoop& events_loop)
    {
        _channel._cursor_visible = true;
        if (static_cast<Application*>(events_loop.get_owner())->get_mouse_mode() ==
            RED_MOUSE_MODE_CLIENT) {
            RedScreen* screen = _channel.screen();
            ASSERT(screen);
            screen->set_cursor(_channel._cursor);
        } else {
            Lock lock(_channel._update_lock);
            int dx = _x - _channel._hot_pos.x;
            int dy = _y - _channel._hot_pos.y;
            _channel._hot_pos.x += dx;
            _channel._hot_pos.y += dy;
            _channel._cursor_rect.left += dx;
            _channel._cursor_rect.right += dx;
            _channel._cursor_rect.top += dy;
            _channel._cursor_rect.bottom += dy;
            lock.unlock();
            _channel.set_rect_area(_channel._cursor_rect);
        }
    }

private:
    CursorChannel& _channel;
    int _x;
    int _y;
};

class CursorHideEvent: public Event {
public:
    CursorHideEvent(CursorChannel& channel): _channel (channel) {}
    virtual void response(AbstractProcessLoop& events_loop)
    {
        _channel._cursor_visible = false;
        if (static_cast<Application*>(events_loop.get_owner())->get_mouse_mode() ==
            RED_MOUSE_MODE_CLIENT) {
            RedScreen* screen = _channel.screen();
            ASSERT(screen);
            screen->set_cursor(NULL);
        } else {
            _channel.clear_area();
        }
    }

private:
    CursorChannel& _channel;
};

class CursorRemoveEvent: public Event {
public:
    CursorRemoveEvent(CursorChannel& channel): _channel (channel) {}
    virtual void response(AbstractProcessLoop& events_loop)
    {
        _channel._cursor_visible = false;
        _channel.clear_area();
        if (_channel._cursor) {
            _channel._cursor->unref();
            _channel._cursor = NULL;
        }
    }

private:
    CursorChannel& _channel;
};

class CursorHandler: public MessageHandlerImp<CursorChannel, RED_CURSOR_MESSAGES_END> {
public:
    CursorHandler(CursorChannel& channel)
        : MessageHandlerImp<CursorChannel, RED_CURSOR_MESSAGES_END>(channel) {}
};

class CursorModeEvent: public Event {
public:
    CursorModeEvent(CursorChannel& channel): _channel (channel) {}

    virtual void response(AbstractProcessLoop& events_loop)
    {
        RedScreen* screen = _channel.screen();
        if (!screen) {
            return;
        }
        if (static_cast<Application*>(events_loop.get_owner())->get_mouse_mode() ==
            RED_MOUSE_MODE_CLIENT) {
            _channel.clear_area();
            screen->set_cursor(_channel._cursor_visible ? _channel._cursor : NULL);
        } else {
            if (_channel._cursor_visible && _channel._cursor) {
                _channel.set_rect_area(_channel._cursor_rect);
            } else {
                _channel.clear_area();
            }
        }
        screen->relase_inputs();
    }

private:
    CursorChannel& _channel;
};

CursorModeTrigger::CursorModeTrigger(CursorChannel& channel)
    : _channel (channel)
{
}

void CursorModeTrigger::on_event()
{
    AutoRef<CursorModeEvent> set_event(new CursorModeEvent(_channel));
    _channel.get_client().push_event(*set_event);
}

CursorChannel::CursorChannel(RedClient& client, uint32_t id)
    : RedChannel(client, RED_CHANNEL_CURSOR, id, new CursorHandler(*this))
    , ScreenLayer(SCREEN_LAYER_CURSOR, false)
    , _cursor (NULL)
    , _cursor_trigger (*this)
    , _cursor_visible (false)
{
    CursorHandler* handler = static_cast<CursorHandler*>(get_message_handler());

    handler->set_handler(RED_MIGRATE, &CursorChannel::handle_migrate, 0);
    handler->set_handler(RED_SET_ACK, &CursorChannel::handle_set_ack, sizeof(RedSetAck));
    handler->set_handler(RED_PING, &CursorChannel::handle_ping, sizeof(RedPing));
    handler->set_handler(RED_WAIT_FOR_CHANNELS, &CursorChannel::handle_wait_for_channels,
                         sizeof(RedWaitForChannels));
    handler->set_handler(RED_DISCONNECTING, &CursorChannel::handle_disconnect,
                         sizeof(RedDisconnect));
    handler->set_handler(RED_NOTIFY, &CursorChannel::handle_notify, sizeof(RedNotify));

    handler->set_handler(RED_CURSOR_INIT, &CursorChannel::handle_init, sizeof(RedCursorInit));
    handler->set_handler(RED_CURSOR_RESET, &CursorChannel::handle_reset, 0);
    handler->set_handler(RED_CURSOR_SET, &CursorChannel::handle_cursor_set,
                         sizeof(RedCursorSet));
    handler->set_handler(RED_CURSOR_MOVE, &CursorChannel::handle_cursor_move,
                         sizeof(RedCursorMove));
    handler->set_handler(RED_CURSOR_HIDE, &CursorChannel::handle_cursor_hide, 0);
    handler->set_handler(RED_CURSOR_TRAIL, &CursorChannel::handle_cursor_trail,
                         sizeof(RedCursorTrail));
    handler->set_handler(RED_CURSOR_INVAL_ONE, &CursorChannel::handle_inval_one,
                         sizeof(RedInvalOne));
    handler->set_handler(RED_CURSOR_INVAL_ALL, &CursorChannel::handle_inval_all, 0);

    get_events_loop().add_trigger(_cursor_trigger);
}

CursorChannel::~CursorChannel()
{
    ASSERT(!_cursor);
}

void CursorChannel::on_connect()
{
}

void CursorChannel::on_disconnect()
{
    remove_cursor();
    _cursor_cache.clear();
    AutoRef<SyncEvent> sync_event(new SyncEvent());
    get_client().push_event(*sync_event);
    (*sync_event)->wait();
    detach_from_screen(get_client().get_application());
}

void CursorChannel::remove_cursor()
{
    AutoRef<CursorRemoveEvent> event(new CursorRemoveEvent(*this));
    get_client().push_event(*event);
}

void CursorChannel::copy_pixels(const QRegion& dest_region, RedDrawable& dest_dc)
{
    Lock lock(_update_lock);
    for (int i = 0; i < (int)dest_region.num_rects; i++) {
        ASSERT(_cursor && _cursor->get_opaque());
        ((NaitivCursor*)_cursor->get_opaque())->draw(dest_dc, _cursor_rect.left, _cursor_rect.top,
                                                     dest_region.rects[i]);
    }
}

void CursorChannel::set_cursor(RedCursor& red_cursor, int data_size, int x, int y, bool visible)
{
    CursorData *cursor;

    if (red_cursor.flags & RED_CURSOR_NONE) {
        remove_cursor();
        return;
    }

    if (red_cursor.flags & RED_CURSOR_FROM_CACHE) {
        cursor = _cursor_cache.get(red_cursor.header.unique);
    } else {
        cursor = new CursorData(red_cursor, data_size);
        if (red_cursor.flags & RED_CURSOR_CACHE_ME) {
            ASSERT(red_cursor.header.unique);
            _cursor_cache.add(red_cursor.header.unique, cursor);
        }
    }

    AutoRef<CursorData> cursor_ref(cursor);
    AutoRef<CursorSetEvent> set_event(new CursorSetEvent(*this, *cursor_ref, x, y, visible));
    get_client().push_event(*set_event);
}

void CursorChannel::handle_init(RedPeer::InMessage *message)
{
    RedCursorInit *init = (RedCursorInit*)message->data();
    attach_to_screen(get_client().get_application(), get_id());
    remove_cursor();
    _cursor_cache.clear();
    set_cursor_mode();
    set_cursor(init->cursor, message->size() - sizeof(RedCursorInit), init->position.x,
               init->position.y, init->visible != 0);
}

void CursorChannel::handle_reset(RedPeer::InMessage *message)
{
    remove_cursor();
    detach_from_screen(get_client().get_application());
    _cursor_cache.clear();
}

void CursorChannel::handle_cursor_set(RedPeer::InMessage* message)
{
    RedCursorSet* set = (RedCursorSet*)message->data();
    set_cursor(set->cursor, message->size() - sizeof(RedCursorSet), set->postition.x,
               set->postition.y, set->visible != 0);
}

void CursorChannel::handle_cursor_move(RedPeer::InMessage* message)
{
    RedCursorMove* move = (RedCursorMove*)message->data();
    AutoRef<CursorMoveEvent> event(new CursorMoveEvent(*this, move->postition.x,
                                                       move->postition.y));
    get_client().push_event(*event);
}

void CursorChannel::handle_cursor_hide(RedPeer::InMessage* message)
{
    AutoRef<CursorHideEvent> event(new CursorHideEvent(*this));
    get_client().push_event(*event);
}

void CursorChannel::handle_cursor_trail(RedPeer::InMessage* message)
{
    RedCursorTrail* trail = (RedCursorTrail*)message->data();
    DBG(0, "length %u frequency %u", trail->length, trail->frequency)
}

void CursorChannel::handle_inval_one(RedPeer::InMessage* message)
{
    RedInvalOne* inval = (RedInvalOne*)message->data();
    _cursor_cache.remove(inval->id);
}

void CursorChannel::handle_inval_all(RedPeer::InMessage* message)
{
    _cursor_cache.clear();
}

void CursorChannel::set_cursor_mode()
{
    _cursor_trigger.trigger();
}

class CursorFactory: public ChannelFactory {
public:
    CursorFactory() : ChannelFactory(RED_CHANNEL_CURSOR) {}
    virtual RedChannel* construct(RedClient& client, uint32_t id)
    {
        return new CursorChannel(client, id);
    }
};

static CursorFactory factory;

ChannelFactory& CursorChannel::Factory()
{
    return factory;
}


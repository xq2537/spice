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

#ifndef _H_CURSOR_CHANNEL
#define _H_CURSOR_CHANNEL

#include "red_channel.h"
#include "cache.hpp"
#include "cursor.h"
#include "screen_layer.h"

class ChannelFactory;
class CursorChannel;

class CursorCacheTreat {
public:
    static inline CursorData* get(CursorData* cursor)
    {
        return cursor->ref();
    }

    static inline void release(CursorData* cursor)
    {
        cursor->unref();
    }

    static const char* name() { return "cursor";}
};

typedef Cache<CursorData, CursorCacheTreat, 1024> CursorCache;

class CursorModeTrigger: public EventSources::Trigger {
public:
    CursorModeTrigger(CursorChannel& channel);
    virtual void on_event();

private:
    CursorChannel& _channel;
};

class CursorChannel: public RedChannel, public ScreenLayer {
public:
    CursorChannel(RedClient& client, uint32_t id);
    virtual ~CursorChannel();

    static ChannelFactory& Factory();
    void set_cursor_mode();

protected:
    virtual void on_connect();
    virtual void on_disconnect();

private:
    void set_cursor(RedCursor& red_cursor, int data_size, int x, int y, bool visible);
    void remove_cursor();

    virtual void copy_pixels(const QRegion& dest_region, RedDrawable& dest_dc);

    void handle_init(RedPeer::InMessage* message);
    void handle_reset(RedPeer::InMessage* message);
    void handle_cursor_set(RedPeer::InMessage* message);
    void handle_cursor_move(RedPeer::InMessage* message);
    void handle_cursor_hide(RedPeer::InMessage* message);
    void handle_cursor_trail(RedPeer::InMessage* message);
    void handle_inval_one(RedPeer::InMessage* message);
    void handle_inval_all(RedPeer::InMessage* message);

    friend class CursorSetEvent;
    friend class CursorMoveEvent;
    friend class CursorHideEvent;
    friend class CursorRemoveEvent;
    friend class CursorModeTrigger;
    friend class CursorModeEvent;

private:
    CursorCache _cursor_cache;
    CursorData* _cursor;
    CursorModeTrigger _cursor_trigger;
    Point _hot_pos;
    Rect _cursor_rect;
    Mutex _update_lock;
    bool _cursor_visible;
};

#endif


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

#ifndef _H_DISPLAY_CHANNEL
#define _H_DISPLAY_CHANNEL

#include "common.h"
#include "canvas.h"
#include "region.h"
#include "red_channel.h"
#include "cairo.h"
#include "cache.hpp"
#include "screen_layer.h"
#include "events_loop.h"
#ifdef USE_OGL
#include "red_pixmap_gl.h"
#endif
#include "glz_decoder_window.h"

class RedScreen;
class ChannelFactory;
class VideoStream;
class DisplayChannel;

class StreamsTrigger: public EventsLoop::Trigger {
public:
    StreamsTrigger(DisplayChannel& channel);

    virtual void on_event();

private:
    DisplayChannel& _channel;
};

#ifdef USE_OGL
class GLInterruptRecreate: public EventsLoop::Trigger {
public:
    GLInterruptRecreate(DisplayChannel& channel);
    virtual void trigger();
    virtual void on_event();

private:
    DisplayChannel& _channel;
    Mutex _lock;
    Condition _cond;
};
#endif

class InterruptUpdate: public EventsLoop::Trigger {
public:
    InterruptUpdate(DisplayChannel& channel);

    virtual void on_event();

private:
    DisplayChannel& _channel;
};


class DisplayChannel: public RedChannel, public ScreenLayer {
public:
    DisplayChannel(RedClient& client, uint32_t id,
                   PixmapCache& pixmap_cache, GlzDecoderWindow& glz_window);
    virtual ~DisplayChannel();

    virtual void copy_pixels(const QRegion& dest_region, RedDrawable& dest_dc);
    virtual void copy_pixels(const QRegion& dest_region, const PixmapHeader &dest);
#ifdef USE_OGL
    virtual void recreate_ogl_context();
    virtual void recreate_ogl_context_interrupt();
    virtual void pre_migrate();
    virtual void post_migrate();
#endif
    virtual void update_interrupt();

    static ChannelFactory& Factory();

protected:
    virtual void on_connect();
    virtual void on_disconnect();

private:
    void set_draw_handlers();
    void clear_draw_handlers();
    bool create_cairo_canvas(int width, int height, int depth);
#ifdef USE_OGL
    bool create_ogl_canvas(int width, int height, int depth, bool recreate,
                           RenderType rendertype);
#endif
#ifdef WIN32
    bool create_gdi_canvas(int width, int height, int depth);
#endif
    void destroy_canvas();
    void create_canvas(const std::vector<int>& canvas_type, int width, int height,
                       int depth);
    void destroy_strams();

    void handle_mode(RedPeer::InMessage* message);
    void handle_mark(RedPeer::InMessage* message);
    void handle_reset(RedPeer::InMessage* message);

    void handle_inval_list(RedPeer::InMessage* message);
    void handle_inval_all_pixmaps(RedPeer::InMessage* message);
    void handle_inval_palette(RedPeer::InMessage* message);
    void handle_inval_all_palettes(RedPeer::InMessage* message);
    void handle_copy_bits(RedPeer::InMessage* message);
    void handle_stream_create(RedPeer::InMessage* message);
    void handle_stream_data(RedPeer::InMessage* message);
    void handle_stream_clip(RedPeer::InMessage* message);
    void handle_stream_destroy(RedPeer::InMessage* message);
    void handle_stream_destroy_all(RedPeer::InMessage* message);

    void handle_draw_fill(RedPeer::InMessage* message);
    void handle_draw_opaque(RedPeer::InMessage* message);
    void handle_draw_copy(RedPeer::InMessage* message);
    void handle_draw_blend(RedPeer::InMessage* message);
    void handle_draw_blackness(RedPeer::InMessage* message);
    void handle_draw_whiteness(RedPeer::InMessage* message);
    void handle_draw_invers(RedPeer::InMessage* message);
    void handle_draw_rop3(RedPeer::InMessage* message);
    void handle_draw_stroke(RedPeer::InMessage* message);
    void handle_draw_text(RedPeer::InMessage* message);
    void handle_draw_transparent(RedPeer::InMessage* message);
    void handle_draw_alpha_blend(RedPeer::InMessage* message);

    void on_streams_trigger();
    virtual void on_update_completion(uint64_t mark);
    void streams_time();
    void activate_streams_timer();
    void stream_update_request(uint32_t update_time);

    static void set_clip_rects(const Clip& clip, uint32_t& num_clip_rects, Rect*& clip_rects,
                               unsigned long addr_offset, uint8_t *min, uint8_t *max);
    static void streams_timer_callback(void* opaque, TimerID timer);
    static void reset_timer_callback(void* opaque, TimerID timer);

private:
    std::auto_ptr<Canvas> _canvas;
    PixmapCache& _pixmap_cache;
    PaletteCache _palette_cache;
    GlzDecoderWindow& _glz_window;
    bool _mark;
    int _x_res;
    int _y_res;
    int _depth;
#ifdef USE_OGL
    RenderType _rendertype;
#endif

#ifndef RED64
    Mutex _mark_lock;
#endif
    uint64_t _update_mark;
    Mutex _streams_lock;

    Mutex _timer_lock;
    TimerID _streams_timer;
    uint32_t _next_timer_time;

    std::vector<VideoStream*> _streams;
    VideoStream* _active_streams;
    StreamsTrigger _streams_trigger;
#ifdef USE_OGL
    GLInterruptRecreate _gl_interrupt_recreate;
#endif
    InterruptUpdate _interrupt_update;
    friend class SetModeEvent;
    friend class ActivateTimerEvent;
    friend class VideoStream;
    friend class StreamsTrigger;
    friend class GLInterupt;
    friend void streams_timer_callback(void* opaque, TimerID timer);
};

#endif


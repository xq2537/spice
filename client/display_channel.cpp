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
#include "canvas.h"
#include "red_pixmap.h"
#ifdef USE_OGL
#include "red_pixmap_gl.h"
#endif
#include "debug.h"
#include "utils.h"
#include "common.h"
#include "display_channel.h"
#include "application.h"
#include "screen.h"
#ifdef USE_OGL
#include "red_gl_canvas.h"
#endif
#include "red_cairo_canvas.h"
#include "red_client.h"
#include "utils.h"
#include "debug.h"
#ifdef WIN32
#include "red_gdi_canvas.h"
#endif
#include "platform_utils.h"
#include "ffmpeg_inc.h"
#include "inputs_channel.h"
#include "cursor_channel.h"

static Mutex avcodec_mutex;

class CreatePrimarySurfaceEvent: public SyncEvent {
public:
   CreatePrimarySurfaceEvent(DisplayChannel& channel, int width, int height, int depth)
        : _channel (channel)
        , _width (width)
        , _height (height)
        , _depth (depth)
    {
    }

    virtual void do_response(AbstractProcessLoop& events_loop)
    {
        Application* app = (Application*)events_loop.get_owner();
        _channel.screen()->lock_size();
        _channel.screen()->resize(_width, _height);
        _channel.create_canvas(app->get_canvas_types(), _width, _height, _depth);
    }

private:
    DisplayChannel& _channel;
    int _width;
    int _height;
    int _depth;
};

class DestroyPrimarySurfaceEvent: public SyncEvent {
public:
    DestroyPrimarySurfaceEvent(DisplayChannel& channel)
        : _channel (channel)
    {
    }

    virtual void do_response(AbstractProcessLoop& events_loop)
    {
        _channel.destroy_canvas();
    }

private:
    DisplayChannel& _channel;
};

class UnlockScreenEvent: public Event {
public:
    UnlockScreenEvent(RedScreen* screen)
        : _screen (screen->ref())
    {
    }

    virtual void response(AbstractProcessLoop& events_loop)
    {
        (*_screen)->unlock_size();
    }

private:
    AutoRef<RedScreen> _screen;
};

#define CLIP_ARRAY_SIZE 1500
#define CLIP_ARRAY_SHIFT 500

static uint8_t clip_array[CLIP_ARRAY_SIZE];
static uint8_t *clip_zero = &clip_array[CLIP_ARRAY_SHIFT];

static void init_clip_array()
{
    int i;
    for (i = 0; i < CLIP_ARRAY_SHIFT; i++) {
        clip_array[i] = 0;
    }

    for (i = CLIP_ARRAY_SHIFT + 256; i < CLIP_ARRAY_SIZE; i++) {
        clip_array[i] = 0xff;
    }

    for (i = 0; i < 256; i++) {
        clip_zero[i] = i;
    }
}

#define  CLIP(val) clip_zero[(int)(val)]

#define R(pixel) (((uint8_t*)(pixel))[0])
#define G(pixel) (((uint8_t*)(pixel))[1])
#define B(pixel) (((uint8_t*)(pixel))[2])

#define YUV_TO_RGB(pixel, y, u , v) {                           \
    int Y = (y) - 16;                                           \
    int U = (u) - 128;                                          \
    int V = (v) - 128;                                          \
    R(pixel) = CLIP((298 * Y + 409 * V + 128) >> 8);            \
    G(pixel) = CLIP((298 * Y - 100 * U - 208 * V + 128) >> 8);  \
    B(pixel) = CLIP((298 * Y + 516 * U + 128) >> 8);            \
}

static inline void yuv420_to_rgb(AVFrame* frame, uint8_t* data, uint32_t width, uint32_t height,
                                 int stride, int top_down)
{
    ASSERT(width % 2 == 0);
    ASSERT(height % 2 == 0);

    /* turning it to be down to top */
    if (top_down) {
        data += stride * (height - 1);
        stride = -stride;
    }

    // B = 1.164(Y - 16) + 2.018(U - 128)
    // G = 1.164(Y - 16) - 0.813(V - 128) - 0.391(U - 128)
    // R = 1.164(Y - 16) + 1.596(V - 128)

    uint8_t* y_line = frame->data[0];
    uint8_t* u = frame->data[1];
    uint8_t* v = frame->data[2];

    uint32_t y_stride = frame->linesize[0];
    uint32_t u_stride = frame->linesize[1];
    uint32_t v_stride = frame->linesize[2];

    for (unsigned int i = 0; i < height / 2; i++) {
        uint8_t* now = data;
        uint8_t* y = y_line;

        for (unsigned int j = 0; j < width / 2; j++) {
            YUV_TO_RGB(now, *y, u[j], v[j]);
            YUV_TO_RGB(now + sizeof(uint32_t), *(y + 1), u[j], v[j]);
            YUV_TO_RGB(now + stride, *(y + y_stride), u[j], v[j]);
            YUV_TO_RGB(now + stride + sizeof(uint32_t), *(y + y_stride + 1), u[j], v[j]);
            y += 2;
            now += 2 * sizeof(uint32_t);
        }
        data += stride * 2;
        y_line += y_stride * 2;
        u += u_stride;
        v += v_stride;
    }
}

#define MAX_VIDEO_FRAMES 30
#define MAX_OVER 15
#define MAX_UNDER -15


class VideoStream {
public:
    VideoStream(RedClient& client, Canvas& canvas, DisplayChannel& channel,
                uint32_t codec_type, bool top_down, uint32_t stream_width,
                uint32_t stream_height, uint32_t src_width, uint32_t src_height,
                SpiceRect* dest, int clip_type, uint32_t num_clip_rects, SpiceRect* clip_rects);
    ~VideoStream();

    void push_data(uint32_t mm_time, uint32_t length, uint8_t* data, uint32_t ped_size);
    void set_clip(int type, uint32_t num_clip_rects, SpiceRect* clip_rects);
    const SpiceRect& get_dest() {return _dest;}
    void handle_update_mark(uint64_t update_mark);
    uint32_t handle_timer_update(uint32_t now);

    static void init();

private:
    void free_frame(uint32_t frame_index);
    void release_all_bufs();
    void remove_dead_frames(uint32_t mm_time);
    uint32_t alloc_frame_slot();
    void maintenance();
    void drop_one_frame();
    uint32_t frame_slot(uint32_t frame_index) { return frame_index % MAX_VIDEO_FRAMES;}
    static bool is_time_to_display(uint32_t now, uint32_t frame_time);

private:
    RedClient& _client;
    Canvas& _canvas;
    DisplayChannel& _channel;
    AVCodecContext* _ctx;
    AVFrame* _frame;
    int _stream_width;
    int _stream_height;
    int _stride;
    bool _top_down;
    SpiceRect _dest;
    QRegion _clip_region;
    QRegion* _clip;

    struct VideoFrame {
        uint32_t mm_time;
        uint32_t compressed_data_size;
        uint8_t* compressed_data;
    };

    uint32_t _frames_head;
    uint32_t _frames_tail;
    uint32_t _kill_mark;
    VideoFrame _frames[MAX_VIDEO_FRAMES];

#ifdef WIN32
    HBITMAP _prev_bitmap;
    HDC _dc;
#endif
    uint8_t *_uncompressed_data;
    PixmapHeader _pixmap;
    uint64_t _update_mark;
    uint32_t _update_time;

public:
    VideoStream* next;
};

#ifdef WIN32
static int create_bitmap(HDC *dc, HBITMAP *prev_bitmap,
                         uint8_t **data, int *nstride,
                         int width, int height)
{
    HBITMAP bitmap;
    struct {
        BITMAPINFO inf;
        RGBQUAD palette[255];
    } bitmap_info;

    memset(&bitmap_info, 0, sizeof(bitmap_info));
    bitmap_info.inf.bmiHeader.biSize = sizeof(bitmap_info.inf.bmiHeader);
    bitmap_info.inf.bmiHeader.biWidth = width;
    bitmap_info.inf.bmiHeader.biHeight = height;

    bitmap_info.inf.bmiHeader.biPlanes = 1;
    bitmap_info.inf.bmiHeader.biBitCount = 32;
    bitmap_info.inf.bmiHeader.biCompression = BI_RGB;
    *nstride = width * 4;

    *dc = create_compatible_dc();
    if (!*dc) {
        return 0;
    }

    bitmap = CreateDIBSection(*dc, &bitmap_info.inf, 0, (void **)data, NULL, 0);
    if (!bitmap) {
        DeleteObject(*dc);
        return 0;
    }

    *prev_bitmap = (HBITMAP)SelectObject(*dc, bitmap);
    return 1;
}

#endif

VideoStream::VideoStream(RedClient& client, Canvas& canvas, DisplayChannel& channel,
                         uint32_t codec_type, bool top_down, uint32_t stream_width,
                         uint32_t stream_height, uint32_t src_width, uint32_t src_height,
                         SpiceRect* dest, int clip_type, uint32_t num_clip_rects,
                         SpiceRect* clip_rects)
    : _client (client)
    , _canvas (canvas)
    , _channel (channel)
    , _ctx (NULL)
    , _frame (NULL)
    , _stream_width (stream_width)
    , _stream_height (stream_height)
    , _stride (stream_width * sizeof(uint32_t))
    , _top_down (top_down)
    , _dest (*dest)
    , _clip (NULL)
    , _frames_head (0)
    , _frames_tail (0)
    , _uncompressed_data (NULL)
    , _update_mark (0)
    , _update_time (0)
    , next (NULL)
{
    AVCodecContext* ctx = NULL;
    AVCodec* codec;
    AVFrame* frame = NULL;

    enum CodecID type;

    memset(_frames, 0, sizeof(_frames));
    region_init(&_clip_region);
    switch (codec_type) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        type = CODEC_ID_MJPEG;
        break;
    default:
        THROW("invalid vide codec type %u", codec_type);
    }

    if (!(codec = avcodec_find_decoder(type))) {
        THROW("can't find codec %u", type);
    }

    if (!(ctx = avcodec_alloc_context())) {
        THROW("alloc codec ctx failed");
    }
    try {
        if (!(frame = avcodec_alloc_frame())) {
            THROW("alloc frame failed");
        }

#ifdef WIN32
        if (!create_bitmap(&_dc, &_prev_bitmap, &_uncompressed_data, &_stride,
                           stream_width, stream_height)) {
            THROW("create_bitmap failed");
        }
#else
        _uncompressed_data = new uint8_t[_stride * stream_height];
#endif
        _pixmap.width = src_width;
        _pixmap.height = src_height;

#ifdef WIN32
        SetViewportOrgEx(_dc, 0, stream_height - src_height, NULL);
#endif
        _pixmap.data = _uncompressed_data + _stride * (src_height - 1);
        _pixmap.stride = -_stride;

        set_clip(clip_type, num_clip_rects, clip_rects);

        Lock lock(avcodec_mutex);
        if (avcodec_open(ctx, codec) < 0) {
            THROW("open avcodec failed");
        }
    } catch (...) {
        av_free(frame);
        av_free(ctx);
        release_all_bufs();
        throw;
    }
    _frame = frame;
    _ctx = ctx;
}

VideoStream::~VideoStream()
{
    if (_ctx) {
        Lock lock(avcodec_mutex);
        avcodec_close(_ctx);
        av_free(_ctx);
        av_free(_frame);
    }
    release_all_bufs();
    region_destroy(&_clip_region);
}

void VideoStream::release_all_bufs()
{
    for (int i = 0; i < MAX_VIDEO_FRAMES; i++) {
        delete[] _frames[i].compressed_data;
    }
#ifdef WIN32
    if (_dc) {
        HBITMAP bitmap = (HBITMAP)SelectObject(_dc, _prev_bitmap);
        DeleteObject(bitmap);
        DeleteObject(_dc);
    }
#else
    delete[] _uncompressed_data;
#endif
}

void VideoStream::free_frame(uint32_t frame_index)
{
    int slot = frame_slot(frame_index);
    delete[] _frames[slot].compressed_data;
    _frames[slot].compressed_data = NULL;
}

void VideoStream::remove_dead_frames(uint32_t mm_time)
{
    while (_frames_head != _frames_tail) {
        if (int(_frames[frame_slot(_frames_tail)].mm_time - mm_time) >= MAX_UNDER) {
            return;
        }
        free_frame(_frames_tail);
        _frames_tail++;
    }
}

void VideoStream::drop_one_frame()
{
    ASSERT(MAX_VIDEO_FRAMES > 2 && (_frames_head - _frames_tail) == MAX_VIDEO_FRAMES);
    int frame_index = _frames_head - _kill_mark++ % (MAX_VIDEO_FRAMES - 2) - 2;

    free_frame(frame_index);

    while (frame_index != _frames_tail) {
        --frame_index;
        _frames[frame_slot(frame_index + 1)] = _frames[frame_slot(frame_index)];
    }
    _frames_tail++;
}

bool VideoStream::is_time_to_display(uint32_t now, uint32_t frame_time)
{
    int delta = frame_time - now;
    return delta <= MAX_OVER && delta >= MAX_UNDER;
}

void VideoStream::maintenance()
{
    uint32_t mm_time = _client.get_mm_time();

    remove_dead_frames(mm_time);
    if (!_update_mark && !_update_time && _frames_head != _frames_tail) {
        VideoFrame* tail = &_frames[frame_slot(_frames_tail)];

        ASSERT(tail->compressed_data);
        uint8_t* data = tail->compressed_data;
        uint32_t length = tail->compressed_data_size;
        int got_picture = 0;
        while (length > 0) {
            int n = avcodec_decode_video(_ctx, _frame, &got_picture, data, length);
            if (n < 0) {
                THROW("decoding eror");
            }
            if (got_picture) {
                yuv420_to_rgb(_frame, _uncompressed_data, _stream_width, _stream_height, _stride,
                              _top_down);
                ASSERT(length - n == 0);
                break;
            }
            length -= n;
            data += n;
        }
        if (got_picture) {
#ifdef WIN32
            _canvas.put_image(_dc, _pixmap, _dest, _clip);
#else
            _canvas.put_image(_pixmap, _dest, _clip);
#endif
            if (is_time_to_display(mm_time, tail->mm_time)) {
                _update_mark = _channel.invalidate(_dest, true);
                Platform::yield();
            } else {
                _update_time = tail->mm_time;
                _channel.stream_update_request(_update_time);
            }
        }
        free_frame(_frames_tail++);
    }
}

uint32_t VideoStream::handle_timer_update(uint32_t now)
{
    if (!_update_time) {
        return 0;
    }

    if (is_time_to_display(now, _update_time)) {
        _update_time = 0;
        _update_mark = _channel.invalidate(_dest, true);
    } else if ((int)(_update_time - now) < 0) {
        DBG(0, "to late");
        _update_time = 0;
    }
    return _update_time;
}

void VideoStream::handle_update_mark(uint64_t update_mark)
{
    if (!_update_mark || update_mark < _update_mark) {
        return;
    }
    _update_mark = 0;
    maintenance();
}

uint32_t VideoStream::alloc_frame_slot()
{
    if ((_frames_head - _frames_tail) == MAX_VIDEO_FRAMES) {
        drop_one_frame();
    }
    return frame_slot(_frames_head++);
}

void VideoStream::push_data(uint32_t mm_time, uint32_t length, uint8_t* data, uint32_t ped_size)
{
    if (ped_size < FF_INPUT_BUFFER_PADDING_SIZE) {
        THROW("insufficieant pedding");
    }
    maintenance();
    uint32_t frame_slot = alloc_frame_slot();
    _frames[frame_slot].compressed_data = new uint8_t[length];
    memcpy(_frames[frame_slot].compressed_data, data, length);
    _frames[frame_slot].compressed_data_size = length;
    _frames[frame_slot].mm_time = mm_time ? mm_time : 1;
    maintenance();
}

void VideoStream::set_clip(int type, uint32_t num_clip_rects, SpiceRect* clip_rects)
{
    if (type == SPICE_CLIP_TYPE_NONE) {
        _clip = NULL;
        return;
    }

    ASSERT(type == SPICE_CLIP_TYPE_RECTS)
    region_clear(&_clip_region);

    for (unsigned int i = 0; i < num_clip_rects; i++) {
        region_add(&_clip_region, &clip_rects[i]);
    }
    _clip = &_clip_region;
}

void VideoStream::init()
{
    avcodec_init();
    avcodec_register_all();
    init_clip_array();
}

class AutoVStreamInit {
public:
    AutoVStreamInit()
    {
        VideoStream::init();
    }
};

static AutoVStreamInit auto_init;


StreamsTrigger::StreamsTrigger(DisplayChannel& channel)
    : _channel (channel)
{
}

void StreamsTrigger::on_event()
{
    _channel.on_streams_trigger();
}

#ifdef USE_OGL

GLInterruptRecreate::GLInterruptRecreate(DisplayChannel& channel)
    : _channel (channel)
{
}

void GLInterruptRecreate::trigger()
{
    Lock lock(_lock);
    EventSources::Trigger::trigger();
    _cond.wait(lock);
}

void GLInterruptRecreate::on_event()
{
    Lock lock(_lock);
    _channel.recreate_ogl_context_interrupt();
    _cond.notify_one();
}

#endif

void InterruptUpdate::on_event()
{
    _channel.update_interrupt();
}

InterruptUpdate::InterruptUpdate(DisplayChannel& channel)
    : _channel (channel)
{
}

StreamsTimer::StreamsTimer(DisplayChannel& channel)
    : _channel (channel)
{
}

void StreamsTimer::response(AbstractProcessLoop& events_loop)
{
    _channel.streams_time();
}

#define RESET_TIMEOUT (1000 * 5)

class ResetTimer: public Timer {
public:
    ResetTimer(RedScreen* screen, RedClient& client) : _screen(screen), _client(client) {}
    virtual void response(AbstractProcessLoop& events_loop);
private:
    RedScreen* _screen;
    RedClient& _client;
};

void ResetTimer::response(AbstractProcessLoop& events_loop)
{
    _screen->unref();
    _client.deactivate_interval_timer(this);
}

class DisplayHandler: public MessageHandlerImp<DisplayChannel, SPICE_MSG_END_DISPLAY> {
public:
    DisplayHandler(DisplayChannel& channel)
        : MessageHandlerImp<DisplayChannel, SPICE_MSG_END_DISPLAY>(channel) {}
};

DisplayChannel::DisplayChannel(RedClient& client, uint32_t id,
                               PixmapCache& pixmap_cache, GlzDecoderWindow& glz_window)
    : RedChannel(client, SPICE_CHANNEL_DISPLAY, id, new DisplayHandler(*this),
                 Platform::PRIORITY_LOW)
    , ScreenLayer (SCREEN_LAYER_DISPLAY, true)
    , _canvas (NULL)
    , _pixmap_cache (pixmap_cache)
    , _glz_window (glz_window)
    , _mark (false)
    , _update_mark (0)
    , _streams_timer (new StreamsTimer(*this))
    , _next_timer_time (0)
    , _cursor_visibal (false)
    , _active_pointer (false)
    , _capture_mouse_mode (false)
    , _inputs_channel (NULL)
    , _active_streams (NULL)
    , _streams_trigger (*this)
#ifdef USE_OGL
    , _gl_interrupt_recreate (*this)
#endif
    , _interrupt_update (*this)
{
    DisplayHandler* handler = static_cast<DisplayHandler*>(get_message_handler());

    handler->set_handler(SPICE_MSG_MIGRATE, &DisplayChannel::handle_migrate, 0);
    handler->set_handler(SPICE_MSG_SET_ACK, &DisplayChannel::handle_set_ack, sizeof(SpiceMsgSetAck));
    handler->set_handler(SPICE_MSG_PING, &DisplayChannel::handle_ping, sizeof(SpiceMsgPing));
    handler->set_handler(SPICE_MSG_WAIT_FOR_CHANNELS, &DisplayChannel::handle_wait_for_channels,
                         sizeof(SpiceMsgWaitForChannels));
    handler->set_handler(SPICE_MSG_DISCONNECTING, &DisplayChannel::handle_disconnect,
                         sizeof(SpiceMsgDisconnect));
    handler->set_handler(SPICE_MSG_NOTIFY, &DisplayChannel::handle_notify, sizeof(SpiceMsgNotify));

    handler->set_handler(SPICE_MSG_DISPLAY_MARK, &DisplayChannel::handle_mark, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_RESET, &DisplayChannel::handle_reset, 0);

    handler->set_handler(SPICE_MSG_DISPLAY_INVAL_LIST,
                         &DisplayChannel::handle_inval_list,
                         sizeof(SpiceResorceList));
    handler->set_handler(SPICE_MSG_DISPLAY_INVAL_ALL_PIXMAPS,
                         &DisplayChannel::handle_inval_all_pixmaps,
                         sizeof(SpiceMsgWaitForChannels));
    handler->set_handler(SPICE_MSG_DISPLAY_INVAL_PALETTE,
                         &DisplayChannel::handle_inval_palette, sizeof(SpiceMsgDisplayInvalOne));
    handler->set_handler(SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES,
                         &DisplayChannel::handle_inval_all_palettes, 0);

    handler->set_handler(SPICE_MSG_DISPLAY_STREAM_CREATE, &DisplayChannel::handle_stream_create,
                         sizeof(SpiceMsgDisplayStreamCreate));
    handler->set_handler(SPICE_MSG_DISPLAY_STREAM_CLIP, &DisplayChannel::handle_stream_clip,
                         sizeof(SpiceMsgDisplayStreamClip));
    handler->set_handler(SPICE_MSG_DISPLAY_STREAM_DESTROY, &DisplayChannel::handle_stream_destroy,
                         sizeof(SpiceMsgDisplayStreamDestroy));
    handler->set_handler(SPICE_MSG_DISPLAY_STREAM_DESTROY_ALL,
                         &DisplayChannel::handle_stream_destroy_all, 0);

    handler->set_handler(SPICE_MSG_DISPLAY_SURFACE_CREATE, &DisplayChannel::handle_surface_create,
                         sizeof(SpiceMsgSurfaceCreate));
    handler->set_handler(SPICE_MSG_DISPLAY_SURFACE_DESTROY, &DisplayChannel::handle_surface_destroy,
                         sizeof(SpiceMsgSurfaceDestroy));

    get_process_loop().add_trigger(_streams_trigger);
#ifdef USE_OGL
    get_process_loop().add_trigger(_gl_interrupt_recreate);
#endif
    get_process_loop().add_trigger(_interrupt_update);
}

DisplayChannel::~DisplayChannel()
{
    if (screen()) {
        screen()->set_update_interrupt_trigger(NULL);
    }

    destroy_canvas();
    destroy_strams();
}

void DisplayChannel::destroy_strams()
{
    Lock lock(_streams_lock);
    for (unsigned int i = 0; i < _streams.size(); i++) {
        delete _streams[i];
        _streams[i] = NULL;
    }
    _active_streams = NULL;
}

void DisplayChannel::set_draw_handlers()
{
    DisplayHandler* handler = static_cast<DisplayHandler*>(get_message_handler());

    handler->set_handler(SPICE_MSG_DISPLAY_COPY_BITS, &DisplayChannel::handle_copy_bits,
                         sizeof(SpiceMsgDisplayCopyBits));

    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_FILL, &DisplayChannel::handle_draw_fill,
                         sizeof(SpiceMsgDisplayDrawFill));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_OPAQUE, &DisplayChannel::handle_draw_opaque,
                         sizeof(SpiceMsgDisplayDrawOpaque));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_COPY, &DisplayChannel::handle_draw_copy,
                         sizeof(SpiceMsgDisplayDrawCopy));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_BLEND, &DisplayChannel::handle_draw_blend,
                         sizeof(SpiceMsgDisplayDrawBlend));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_BLACKNESS, &DisplayChannel::handle_draw_blackness,
                         sizeof(SpiceMsgDisplayDrawBlackness));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_WHITENESS, &DisplayChannel::handle_draw_whiteness,
                         sizeof(SpiceMsgDisplayDrawWhiteness));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_INVERS, &DisplayChannel::handle_draw_invers,
                         sizeof(SpiceMsgDisplayDrawInvers));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_ROP3, &DisplayChannel::handle_draw_rop3,
                         sizeof(SpiceMsgDisplayDrawRop3));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_STROKE, &DisplayChannel::handle_draw_stroke,
                         sizeof(SpiceMsgDisplayDrawStroke));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_TEXT, &DisplayChannel::handle_draw_text,
                         sizeof(SpiceMsgDisplayDrawText));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_TRANSPARENT,
                         &DisplayChannel::handle_draw_transparent, sizeof(SpiceMsgDisplayDrawTransparent));
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND,
                         &DisplayChannel::handle_draw_alpha_blend, sizeof(SpiceMsgDisplayDrawAlphaBlend));
    handler->set_handler(SPICE_MSG_DISPLAY_STREAM_DATA, &DisplayChannel::handle_stream_data,
                         sizeof(SpiceMsgDisplayStreamData));
}

void DisplayChannel::clear_draw_handlers()
{
    DisplayHandler* handler = static_cast<DisplayHandler*>(get_message_handler());

    handler->set_handler(SPICE_MSG_DISPLAY_COPY_BITS, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_FILL, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_OPAQUE, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_COPY, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_BLEND, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_BLACKNESS, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_WHITENESS, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_INVERS, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_ROP3, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_STROKE, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_TEXT, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_TRANSPARENT, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND, NULL, 0);
    handler->set_handler(SPICE_MSG_DISPLAY_STREAM_DATA, NULL, 0);
}

void DisplayChannel::copy_pixels(const QRegion& dest_region,
                                 const PixmapHeader &dest_pixmap)
{
    if (!_canvas.get()) {
        return;
    }

    _canvas->copy_pixels(dest_region, NULL, &dest_pixmap);
}

#ifdef USE_OGL
void DisplayChannel::recreate_ogl_context_interrupt()
{
    Canvas* canvas = _canvas.release();
    if (canvas) {
        ((GCanvas *)(canvas))->touch_context();
        ((GCanvas *)canvas)->textures_lost();
        delete canvas;
    }

    if (!create_ogl_canvas(_x_res, _y_res, _depth, 0, _rendertype)) {
        THROW("create_ogl_canvas failed");
    }

    ((GCanvas *)(_canvas.get()))->touch_context();
}

void DisplayChannel::recreate_ogl_context()
{
    if (_canvas.get() && _canvas->get_pixmap_type() == CANVAS_TYPE_GL) {
        if (!screen()->need_recreate_context_gl()) {
            _gl_interrupt_recreate.trigger();
        }
    }
}

#endif

void DisplayChannel::update_cursor()
{
    if (!screen() || !_active_pointer) {
        return;
    }

    if (_capture_mouse_mode) {
        //todo: use special cursor for capture mode
        AutoRef<LocalCursor> default_cursor(Platform::create_default_cursor());
        screen()->set_cursor(*default_cursor);
        return;
    }

    if (!_cursor_visibal || !*_cursor) {
        screen()->hide_cursor();
        return;
    }


    if (!(*_cursor)->get_local()) {
        AutoRef<LocalCursor> local_cursor(Platform::create_local_cursor(*_cursor));
        if (*local_cursor == NULL) {
            THROW("create local cursor failed");
        }
        (*_cursor)->set_local(*local_cursor);
    }
    screen()->set_cursor((*_cursor)->get_local());
}

void DisplayChannel::set_cursor(CursorData* cursor)
{
    ASSERT(cursor);
    _cursor.reset(cursor->ref());
    _cursor_visibal = true;
    update_cursor();
}

void DisplayChannel::hide_cursor()
{
    _cursor_visibal = false;
    update_cursor();
}

void DisplayChannel::attach_inputs(InputsChannel* inputs_channel)
{
    if (_inputs_channel) {
        return;
    }

    _inputs_channel = inputs_channel;
    if (_active_pointer && !_capture_mouse_mode) {
        _inputs_channel->on_mouse_position(_pointer_pos.x, _pointer_pos.y,
                                           _buttons_state, get_id());
    }
}

void DisplayChannel::detach_inputs()
{
    _inputs_channel = NULL;
}

bool DisplayChannel::pointer_test(int x, int y)
{
    return contains_point(x, y);
}

void DisplayChannel::on_pointer_enter(int x, int y, unsigned int buttons_state)
{
    _active_pointer = true;
    update_cursor();
    on_pointer_motion(x, y, buttons_state);
}

void DisplayChannel::on_pointer_motion(int x, int y, unsigned int buttons_state)
{
    _pointer_pos.x = x;
    _pointer_pos.y = y;
    _buttons_state = buttons_state;
    if (!_capture_mouse_mode && _inputs_channel) {
        _inputs_channel->on_mouse_position(x, y, buttons_state, get_id());
    }
}

void DisplayChannel::on_pointer_leave()
{
    _active_pointer = false;
}

void DisplayChannel::on_mouse_button_press(int button, int buttons_state)
{
    _buttons_state = buttons_state;
    if (!_capture_mouse_mode && _inputs_channel) {
        _inputs_channel->on_mouse_down(button, buttons_state);
    }
}

void DisplayChannel::on_mouse_button_release(int button, int buttons_state)
{
    _buttons_state = buttons_state;
    if (_capture_mouse_mode) {
        if (button == SPICE_MOUSE_BUTTON_LEFT) {
            get_client().on_mouse_capture_trigger(*screen());
        }
        return;
    }

    if (_inputs_channel) {
        _inputs_channel->on_mouse_up(button, buttons_state);
    }
}

void DisplayChannel::set_capture_mode(bool on)
{
    if (_capture_mouse_mode == on) {
        return;
    }
    _capture_mouse_mode = on;
    update_cursor();
    if (_inputs_channel && !_capture_mouse_mode && _active_pointer) {
        _inputs_channel->on_mouse_position(_pointer_pos.x, _pointer_pos.y, _buttons_state,
                                           get_id());
    }
}

void DisplayChannel::update_interrupt()
{
    if (!_canvas.get() || !screen()) {
        return;
    }

#ifdef USE_OGL
    if (_canvas->get_pixmap_type() == CANVAS_TYPE_GL) {
        ((GCanvas *)(_canvas.get()))->pre_gl_copy();
    }
#endif

    screen()->update();

#ifdef USE_OGL
    if (_canvas->get_pixmap_type() == CANVAS_TYPE_GL) {
        ((GCanvas *)(_canvas.get()))->post_gl_copy();
    }
#endif
}

#ifdef USE_OGL

void DisplayChannel::pre_migrate()
{
}

void DisplayChannel::post_migrate()
{
#ifdef USE_OGL
    if (_canvas->get_pixmap_type() == CANVAS_TYPE_GL) {
        _gl_interrupt_recreate.trigger();
    }
#endif
}

#endif

void DisplayChannel::copy_pixels(const QRegion& dest_region,
                                 RedDrawable& dest_dc)
{
    if (!_canvas.get()) {
        return;
    }

    _canvas->copy_pixels(dest_region, dest_dc);
}

class ActivateTimerEvent: public Event {
public:
    ActivateTimerEvent(DisplayChannel& channel)
        : _channel (channel)
    {
    }

    virtual void response(AbstractProcessLoop& events_loop)
    {
        _channel.activate_streams_timer();
    }

private:
    DisplayChannel& _channel;
};

class AttachChannelsEvent : public Event {
public:
    AttachChannelsEvent(DisplayChannel& channel) : Event(), _channel (channel) {}

    class AttachChannels: public ForEachChannelFunc {
    public:
        AttachChannels(DisplayChannel& channel)
            : _channel (channel)
        {
        }

        virtual bool operator() (RedChannel& channel)
        {
            if (channel.get_type() == SPICE_CHANNEL_CURSOR && channel.get_id() == _channel.get_id()) {
                static_cast<CursorChannel&>(channel).attach_display(&_channel);
            } else if (channel.get_type() == SPICE_CHANNEL_INPUTS) {
                _channel.attach_inputs(&static_cast<InputsChannel&>(channel));
            }
            return false;
        }

    private:
        DisplayChannel& _channel;
    };

    virtual void response(AbstractProcessLoop& events_loop)
    {
        uint32_t mouse_mode = _channel.get_client().get_mouse_mode();
        _channel._capture_mouse_mode =  (mouse_mode == SPICE_MOUSE_MODE_SERVER);
        AttachChannels for_each_func(_channel);
        _channel.get_client().for_each_channel(for_each_func);
    }

private:
    DisplayChannel& _channel;
};

class DetachChannelsEvent : public Event {
public:
    DetachChannelsEvent(DisplayChannel& channel) : Event(), _channel (channel) {}

    class DetatchChannels: public ForEachChannelFunc {
    public:
        DetatchChannels(DisplayChannel& channel)
            : _channel (channel)
        {
        }

        virtual bool operator() (RedChannel& channel)
        {
            if (channel.get_type() == SPICE_CHANNEL_CURSOR && channel.get_id() == _channel.get_id()) {
                static_cast<CursorChannel&>(channel).detach_display();
                return true;
            }
            return false;
        }

    private:
        DisplayChannel& _channel;
    };

    virtual void response(AbstractProcessLoop& events_loop)
    {
        DetatchChannels for_each_func(_channel);
        _channel.get_client().for_each_channel(for_each_func);
    }

private:
    DisplayChannel& _channel;
};

void DisplayChannel::on_connect()
{
    Message* message = new Message(SPICE_MSGC_DISPLAY_INIT, sizeof(SpiceMsgcDisplayInit));
    SpiceMsgcDisplayInit* init = (SpiceMsgcDisplayInit*)message->data();
    init->pixmap_cache_id = 1;
    init->pixmap_cache_size = get_client().get_pixmap_cache_size();
    init->glz_dictionary_id = 1;
    init->glz_dictionary_window_size = get_client().get_glz_window_size();
    post_message(message);
    AutoRef<AttachChannelsEvent> attach_channels(new AttachChannelsEvent(*this));
    get_client().push_event(*attach_channels);
}

void DisplayChannel::on_disconnect()
{
    if (_canvas.get()) {
        _canvas->clear();
    }

    if (screen()) {
        screen()->set_update_interrupt_trigger(NULL);
    }

    AutoRef<DetachChannelsEvent> detach_channels(new DetachChannelsEvent(*this));
    get_client().push_event(*detach_channels);
    if (screen()) {
        AutoRef<UnlockScreenEvent> unlock_event(new UnlockScreenEvent(screen()));
        get_client().push_event(*unlock_event);
        detach_from_screen(get_client().get_application());
    }
    get_client().deactivate_interval_timer(*_streams_timer);
    AutoRef<SyncEvent> sync_event(new SyncEvent());
    get_client().push_event(*sync_event);
    (*sync_event)->wait();
}

bool DisplayChannel::create_cairo_canvas(int width, int height, int depth)
{
    try {
        std::auto_ptr<CCanvas> canvas(new CCanvas(_pixmap_cache, _palette_cache, _glz_window));
        canvas->set_mode(width, height, depth, screen()->get_window());
        _canvas.reset(canvas.release());
        LOG_INFO("display %d: using cairo", get_id());
    } catch (...) {
        return false;
    }
    return true;
}

#ifdef USE_OGL
bool DisplayChannel::create_ogl_canvas(int width, int height, int depth,
                                       bool recreate, RenderType rendertype)
{
    try {
        RedWindow *win;
        int ret = 1;

        std::auto_ptr<GCanvas> canvas(new GCanvas(_pixmap_cache,
                                                  _palette_cache,
                                                  _glz_window));

        win = screen()->get_window();
        if (!ret) {
            return false;
        }

        canvas->set_mode(width, height, depth, win, rendertype);

        screen()->untouch_context();

        _canvas.reset(canvas.release());
        _rendertype = rendertype;
        LOG_INFO("display %d: using ogl", get_id());
    } catch (...) {
        return false;
    }
    return true;
}

#endif

#ifdef WIN32
bool DisplayChannel::create_gdi_canvas(int width, int height, int depth)
{
    try {
        std::auto_ptr<GDICanvas> canvas(
            new GDICanvas(_pixmap_cache, _palette_cache, _glz_window));
        canvas->set_mode(width, height, depth);
        _canvas.reset(canvas.release());
        LOG_INFO("display %d: using gdi", get_id());
    } catch (...) {
        return false;
    }
    return true;
}

#endif

void DisplayChannel::destroy_canvas()
{
    Canvas* canvas = _canvas.release();

    if (canvas) {
#ifdef USE_OGL
        if (canvas->get_pixmap_type() == CANVAS_TYPE_GL) {
            ((GCanvas *)(canvas))->touch_context();
        }
#endif
        delete canvas;
    }
}

void DisplayChannel::create_canvas(const std::vector<int>& canvas_types, int width,
                                   int height, int depth)
{
#ifdef USE_OGL
    bool recreate = true;
#endif
    unsigned int i;

    clear_draw_handlers();

#ifdef USE_OGL
    if (screen()->need_recreate_context_gl()) {
        recreate = false;
    }
#endif

    screen()->set_update_interrupt_trigger(NULL);

    for (i = 0; i < canvas_types.size(); i++) {

        if (canvas_types[i] == CANVAS_OPTION_CAIRO && create_cairo_canvas(width, height, depth)) {
            break;
        }
#ifdef USE_OGL
        if (canvas_types[i] == CANVAS_OPTION_OGL_FBO && create_ogl_canvas(width, height, depth,
                                                                          recreate,
                                                                          RENDER_TYPE_FBO)) {
            break;
        }
        if (canvas_types[i] == CANVAS_OPTION_OGL_PBUFF && create_ogl_canvas(width, height, depth,
                                                                            recreate,
                                                                            RENDER_TYPE_PBUFF)) {
            break;
        }
#endif
#ifdef WIN32
        if (canvas_types[i] == CANVAS_OPTION_GDI && create_gdi_canvas(width, height, depth)) {
            break;
        }
#endif
    }

    if (i == canvas_types.size()) {
        THROW("create canvas failed");
    }

    set_draw_handlers();
}

void DisplayChannel::handle_mark(RedPeer::InMessage *message)
{
    _mark = true;
    SpiceRect area;
    area.top = area.left = 0;
    area.right = _x_res;
    area.bottom = _y_res;

    AutoRef<VisibilityEvent> event(new VisibilityEvent(get_id()));
    get_client().push_event(*event);
    set_rect_area(area);
}

void DisplayChannel::handle_reset(RedPeer::InMessage *message)
{
    screen()->set_update_interrupt_trigger(NULL);

    if (_canvas.get()) {
        _canvas->clear();
    }

    AutoRef<ResetTimer> reset_timer(new ResetTimer(screen()->ref(), get_client()));

    AutoRef<UnlockScreenEvent> unlock_event(new UnlockScreenEvent(screen()));
    get_client().push_event(*unlock_event);

    detach_from_screen(get_client().get_application());
    _palette_cache.clear();

    get_client().activate_interval_timer(*reset_timer, RESET_TIMEOUT);
}

void DisplayChannel::handle_inval_list(RedPeer::InMessage* message)
{
    SpiceResorceList *inval_list = (SpiceResorceList *)message->data();

    if (message->size() <
                        sizeof(*inval_list) + inval_list->count * sizeof(inval_list->resorces[0])) {
        THROW("access violation");
    }

    for (int i = 0; i < inval_list->count; i++) {
        if (inval_list->resorces[i].type != SPICE_RES_TYPE_PIXMAP) {
            THROW("invalid res type");
        }

        _pixmap_cache.remove(inval_list->resorces[i].id);
    }
}

void DisplayChannel::handle_inval_all_pixmaps(RedPeer::InMessage* message)
{
    SpiceMsgWaitForChannels *wait = (SpiceMsgWaitForChannels *)message->data();
    if (message->size() < sizeof(*wait) + wait->wait_count * sizeof(wait->wait_list[0])) {
        THROW("access violation");
    }
    get_client().wait_for_channels(wait->wait_count, wait->wait_list);
    _pixmap_cache.clear();
}

void DisplayChannel::handle_inval_palette(RedPeer::InMessage* message)
{
    SpiceMsgDisplayInvalOne* inval = (SpiceMsgDisplayInvalOne*)message->data();
    _palette_cache.remove(inval->id);
}

void DisplayChannel::handle_inval_all_palettes(RedPeer::InMessage* message)
{
    _palette_cache.clear();
}

void DisplayChannel::set_clip_rects(const SpiceClip& clip, uint32_t& num_clip_rects,
                                    SpiceRect*& clip_rects, unsigned long addr_offset,
                                    uint8_t *min, uint8_t *max)
{
    switch (clip.type) {
    case SPICE_CLIP_TYPE_RECTS: {
        uint32_t* n = (uint32_t*)SPICE_GET_ADDRESS(clip.data + addr_offset);
        if (n < (uint32_t*)min || n + 1 > (uint32_t*)max) {
            THROW("access violation");
        }
        num_clip_rects = *n;
        clip_rects = (SpiceRect *)(n + 1);
        if (clip_rects + num_clip_rects > (SpiceRect*)max) {
            THROW("access violation");
        }
        break;
    }
    case SPICE_CLIP_TYPE_NONE:
        num_clip_rects = 0;
        clip_rects = NULL;
        break;
    case SPICE_CLIP_TYPE_PATH:
        THROW("unexpected clip type");
    }
}

void DisplayChannel::handle_stream_create(RedPeer::InMessage* message)
{
    SpiceMsgDisplayStreamCreate* stream_create = (SpiceMsgDisplayStreamCreate*)message->data();

    PANIC_ON(stream_create->surface_id != 0);
    Lock lock(_streams_lock);
    if (_streams.size() <= stream_create->id) {
        _streams.resize(stream_create->id + 1);
    }

    if (_streams[stream_create->id]) {
        THROW("stream exist");
    }

    uint32_t num_clip_rects;
    SpiceRect* clip_rects;
    set_clip_rects(stream_create->clip, num_clip_rects, clip_rects,
                   (unsigned long)message->data(), (uint8_t*)(stream_create + 1),
                   message->data() + message->size());
    _streams[stream_create->id] = new VideoStream(get_client(), *_canvas.get(),
                                                  *this, stream_create->codec_type,
                                                  !!(stream_create->flags & SPICE_STREAM_FLAGS_TOP_DOWN),
                                                  stream_create->stream_width,
                                                  stream_create->stream_height,
                                                  stream_create->src_width,
                                                  stream_create->src_height,
                                                  &stream_create->dest,
                                                  stream_create->clip.type,
                                                  num_clip_rects,
                                                  clip_rects);
    _streams[stream_create->id]->next = _active_streams;
    _active_streams = _streams[stream_create->id];
}

void DisplayChannel::handle_stream_data(RedPeer::InMessage* message)
{
    SpiceMsgDisplayStreamData* stream_data = (SpiceMsgDisplayStreamData*)message->data();
    VideoStream* stream;

    if (stream_data->id >= _streams.size() || !(stream = _streams[stream_data->id])) {
        THROW("invalid stream");
    }

    if (message->size() < sizeof(SpiceMsgDisplayStreamData) + stream_data->data_size + stream_data->ped_size) {
        THROW("access violation");
    }

    stream->push_data(stream_data->multi_media_time, stream_data->data_size, stream_data->data,
                      stream_data->ped_size);
}

void DisplayChannel::handle_stream_clip(RedPeer::InMessage* message)
{
    SpiceMsgDisplayStreamClip* clip_data = (SpiceMsgDisplayStreamClip*)message->data();
    VideoStream* stream;
    uint32_t num_clip_rects;
    SpiceRect* clip_rects;

    if (clip_data->id >= _streams.size() || !(stream = _streams[clip_data->id])) {
        THROW("invalid stream");
    }

    if (message->size() < sizeof(SpiceMsgDisplayStreamClip)) {
        THROW("access violation");
    }
    set_clip_rects(clip_data->clip, num_clip_rects, clip_rects,
                   (unsigned long)message->data(), (uint8_t*)(clip_data + 1),
                   message->data() + message->size());
    Lock lock(_streams_lock);
    stream->set_clip(clip_data->clip.type, num_clip_rects, clip_rects);
}

void DisplayChannel::handle_stream_destroy(RedPeer::InMessage* message)
{
    SpiceMsgDisplayStreamDestroy* stream_destroy = (SpiceMsgDisplayStreamDestroy*)message->data();

    if (stream_destroy->id >= _streams.size() || !_streams[stream_destroy->id]) {
        THROW("invalid stream");
    }
    Lock lock(_streams_lock);

    VideoStream **active_stream = &_active_streams;
    for (;;) {
        if (!*active_stream) {
            THROW("not in actibe streams");
        }

        if (*active_stream == _streams[stream_destroy->id]) {
            *active_stream = _streams[stream_destroy->id]->next;
            break;
        }
        active_stream = &(*active_stream)->next;
    }

    delete _streams[stream_destroy->id];
    _streams[stream_destroy->id] = NULL;
}

void DisplayChannel::handle_stream_destroy_all(RedPeer::InMessage* message)
{
    destroy_strams();
}

void DisplayChannel::create_primary_surface(int width, int height, int depth)
{
   _mark = false;
    attach_to_screen(get_client().get_application(), get_id());
    clear_area();

    AutoRef<CreatePrimarySurfaceEvent> event(new CreatePrimarySurfaceEvent(*this, width, height,
                                                                           depth));
    get_client().push_event(*event);
    (*event)->wait();
    if (!(*event)->success()) {
        THROW("Create primary surface failed");
    }

    _x_res = width;
    _y_res = height;
    _depth = depth;

#ifdef USE_OGL
    if (_canvas->get_pixmap_type() == CANVAS_TYPE_GL) {
        ((GCanvas *)(_canvas.get()))->touch_context();
        screen()->set_update_interrupt_trigger(&_interrupt_update);
        screen()->set_type_gl();
    }
#endif
}

void DisplayChannel::destroy_primary_surface()
{
#ifdef USE_OGL
    if (screen()) {
        if (_canvas.get()) {
            if (_canvas->get_pixmap_type() == CANVAS_TYPE_GL) {
                screen()->unset_type_gl();
                screen()->untouch_context();
            }
        }
    }
#endif

    AutoRef<DestroyPrimarySurfaceEvent> event(new DestroyPrimarySurfaceEvent(*this));
    get_client().push_event(*event);
    (*event)->wait();
    if (!(*event)->success()) {
        THROW("Destroying primary surface failed");
    }
}

void DisplayChannel::handle_surface_create(RedPeer::InMessage* message)
{
    SpiceMsgSurfaceCreate* surface_create = (SpiceMsgSurfaceCreate*)message->data();
    PANIC_ON(surface_create->surface_id != 0);
    PANIC_ON(surface_create->flags != SPICE_SURFACE_FLAGS_PRIMARY);

    create_primary_surface(surface_create->width, surface_create->height, surface_create->depth);
}

void DisplayChannel::handle_surface_destroy(RedPeer::InMessage* message)
{
    SpiceMsgSurfaceDestroy* surface_destroy = (SpiceMsgSurfaceDestroy*)message->data();
    PANIC_ON(surface_destroy->surface_id != 0);

    destroy_primary_surface();
}

#define PRE_DRAW
#define POST_DRAW

#define DRAW(type) {                                \
    PRE_DRAW;                                       \
    _canvas->draw_##type(*type, message->size());   \
    POST_DRAW;                                      \
    invalidate(type->base.box, false);              \
}

void DisplayChannel::handle_copy_bits(RedPeer::InMessage* message)
{
    SpiceMsgDisplayCopyBits* copy_bits = (SpiceMsgDisplayCopyBits*)message->data();
    PANIC_ON(copy_bits->base.surface_id != 0);
    PRE_DRAW;
    _canvas->copy_bits(*copy_bits, message->size());
    POST_DRAW;
    invalidate(copy_bits->base.box, false);
}

void DisplayChannel::handle_draw_fill(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawFill* fill = (SpiceMsgDisplayDrawFill*)message->data();
    PANIC_ON(fill->base.surface_id != 0);
    DRAW(fill);
}

void DisplayChannel::handle_draw_opaque(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawOpaque* opaque = (SpiceMsgDisplayDrawOpaque*)message->data();
    PANIC_ON(opaque->base.surface_id != 0);
    DRAW(opaque);
}

void DisplayChannel::handle_draw_copy(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawCopy* copy = (SpiceMsgDisplayDrawCopy*)message->data();
    PANIC_ON(copy->base.surface_id != 0);
    DRAW(copy);
}

void DisplayChannel::handle_draw_blend(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawBlend* blend = (SpiceMsgDisplayDrawBlend*)message->data();
    PANIC_ON(blend->base.surface_id != 0);
    DRAW(blend);
}

void DisplayChannel::handle_draw_blackness(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawBlackness* blackness = (SpiceMsgDisplayDrawBlackness*)message->data();
    PANIC_ON(blackness->base.surface_id != 0);
    DRAW(blackness);
}

void DisplayChannel::handle_draw_whiteness(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawWhiteness* whiteness = (SpiceMsgDisplayDrawWhiteness*)message->data();
    PANIC_ON(whiteness->base.surface_id != 0);
    DRAW(whiteness);
}

void DisplayChannel::handle_draw_invers(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawInvers* invers = (SpiceMsgDisplayDrawInvers*)message->data();
    PANIC_ON(invers->base.surface_id != 0);
    DRAW(invers);
}

void DisplayChannel::handle_draw_rop3(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawRop3* rop3 = (SpiceMsgDisplayDrawRop3*)message->data();
    PANIC_ON(rop3->base.surface_id != 0);
    DRAW(rop3);
}

void DisplayChannel::handle_draw_stroke(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawStroke* stroke = (SpiceMsgDisplayDrawStroke*)message->data();
    PANIC_ON(stroke->base.surface_id != 0);
    DRAW(stroke);
}

void DisplayChannel::handle_draw_text(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawText* text = (SpiceMsgDisplayDrawText*)message->data();
    PANIC_ON(text->base.surface_id != 0);
    DRAW(text);
}

void DisplayChannel::handle_draw_transparent(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawTransparent* transparent = (SpiceMsgDisplayDrawTransparent*)message->data();
    PANIC_ON(transparent->base.surface_id != 0);
    DRAW(transparent);
}

void DisplayChannel::handle_draw_alpha_blend(RedPeer::InMessage* message)
{
    SpiceMsgDisplayDrawAlphaBlend* alpha_blend = (SpiceMsgDisplayDrawAlphaBlend*)message->data();
    PANIC_ON(alpha_blend->base.surface_id != 0);
    DRAW(alpha_blend);
}

void DisplayChannel::streams_time()
{
    _next_timer_time = 0;
    Lock lock(_streams_lock);
    uint32_t mm_time = get_client().get_mm_time();
    uint32_t next_time = 0;
    VideoStream* stream = _active_streams;
    while (stream) {
        uint32_t next_frame_time;
        if ((next_frame_time = stream->handle_timer_update(mm_time))) {
            if (!next_time || int(next_frame_time - next_time) < 0) {
                next_time = next_frame_time;
            }
        }
        stream = stream->next;
    }
    Lock timer_lock(_timer_lock);
    mm_time = get_client().get_mm_time();
    next_time = mm_time + 15;
    if (next_time && (!_next_timer_time || int(next_time - _next_timer_time) < 0)) {
        get_client().activate_interval_timer(*_streams_timer, MAX(int(next_time - mm_time), 0));
        _next_timer_time = next_time;
    } else if (!_next_timer_time) {
        get_client().deactivate_interval_timer(*_streams_timer);
    }
    timer_lock.unlock();
    lock.unlock();
    Platform::yield();
}

void DisplayChannel::activate_streams_timer()
{
    uint32_t next_time = _next_timer_time;
    if (!next_time) {
        return;
    }

    int delta = next_time - get_client().get_mm_time();
    if (delta <= 0) {
        streams_time();
    } else {
        Lock timer_lock(_timer_lock);
        if (!_next_timer_time) {
            return;
        }
        delta = _next_timer_time - get_client().get_mm_time();
        get_client().activate_interval_timer(*_streams_timer, delta);
    }
}

void DisplayChannel::stream_update_request(uint32_t mm_time)
{
    Lock lock(_timer_lock);
    if (_next_timer_time && int(mm_time - _next_timer_time) > 0) {
        return;
    }
    _next_timer_time = mm_time;
    lock.unlock();
    AutoRef<ActivateTimerEvent> event(new ActivateTimerEvent(*this));
    get_client().push_event(*event);
}

void DisplayChannel::on_update_completion(uint64_t mark)
{
#ifndef RED64
    Lock lock(_mark_lock);
#endif
    _update_mark = mark;
#ifndef RED64
    lock.unlock();
#endif
    _streams_trigger.trigger();
}

void DisplayChannel::on_streams_trigger()
{
#ifndef RED64
    Lock lock(_mark_lock);
#endif
    uint64_t update_mark = _update_mark;
#ifndef RED64
    lock.unlock();
#endif
    VideoStream* stream = _active_streams;
    while (stream) {
        stream->handle_update_mark(update_mark);
        stream = stream->next;
    }
}

class DisplayFactory: public ChannelFactory {
public:
    DisplayFactory() : ChannelFactory(SPICE_CHANNEL_DISPLAY) {}
    virtual RedChannel* construct(RedClient& client, uint32_t id)
    {
        return new DisplayChannel(client, id,
                                  client.get_pixmap_cache(), client.get_glz_window());
    }
};

static DisplayFactory factory;

ChannelFactory& DisplayChannel::Factory()
{
    return factory;
}


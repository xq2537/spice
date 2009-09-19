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

#include "platform.h"
#include "win_platform.h"
#include "utils.h"
#include "threads.h"
#include "debug.h"
#include "monitor.h"
#include "record.h"
#include "playback.h"
#include "cursor.h"
#include "named_pipe.h"

#define WM_USER_WAKEUP WM_USER
#define NUM_TIMERS 100

int gdi_handlers = 0;
extern HINSTANCE instance;

class DefaultEventListener: public Platform::EventListener {
public:
    virtual void on_app_activated() {}
    virtual void on_app_deactivated() {}
    virtual void on_monitors_change() {}
};

static DefaultEventListener default_event_listener;
static Platform::EventListener* event_listener = &default_event_listener;
static HWND paltform_win;
static HANDLE main_tread;

struct Timer {
    TimerID id;
    timer_proc_t proc;
    void* opaque;
    Timer *next;
};

Timer timers[NUM_TIMERS];
Timer* free_timers = NULL;
Mutex timers_lock;

static void free_timer(Timer* timer)
{
    Lock lock(timers_lock);
    timer->proc = NULL;
    timer->next = free_timers;
    free_timers = timer;
}

static void init_timers()
{
    for (int i = 0; i < NUM_TIMERS; i++) {
        timers[i].id = i;
        free_timer(&timers[i]);
    }
}

static Timer* alloc_timer()
{
    Timer* timer;

    Lock lock(timers_lock);
    if (!(timer = free_timers)) {
        return NULL;
    }

    free_timers = free_timers->next;
    return timer;
}

static LRESULT CALLBACK PlatformWinProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_TIMER: {
        TimerID id = wParam - 1;
        ASSERT(id < NUM_TIMERS);
        Timer* timer = &timers[id];
        timer->proc(timer->opaque, id);
        break;
    }
    case WM_USER_WAKEUP: {
        break;
    }
    case WM_ACTIVATEAPP:
        if (wParam) {
            event_listener->on_app_activated();
        } else {
            event_listener->on_app_deactivated();
        }
        break;
    case WM_DISPLAYCHANGE:
        event_listener->on_monitors_change();
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

static void create_message_wind()
{
    WNDCLASSEX wclass;
    ATOM class_atom;
    HWND window;

    const LPCWSTR class_name = L"spicec_platform_wclass";

    wclass.cbSize = sizeof(WNDCLASSEX);
    wclass.style = 0;
    wclass.lpfnWndProc = PlatformWinProc;
    wclass.cbClsExtra = 0;
    wclass.cbWndExtra = 0;
    wclass.hInstance = instance;
    wclass.hIcon = NULL;
    wclass.hCursor = NULL;
    wclass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wclass.lpszMenuName = NULL;
    wclass.lpszClassName = class_name;
    wclass.hIconSm = NULL;

    if ((class_atom = RegisterClassEx(&wclass)) == 0) {
        THROW("register class failed");
    }

    if (!(window = CreateWindow(class_name, L"", 0, 0, 0, 0, 0, NULL, NULL, instance, NULL))) {
        THROW("create message window failed");
    }

    paltform_win = window;
}

void Platform::send_quit_request()
{
    ASSERT(GetCurrentThread() == main_tread);
    PostQuitMessage(0);
}

static std::vector<HANDLE> events;
static std::vector<EventOwner*> events_owners;

void WinPlatform::add_event(EventOwner& event_owner)
{
    ASSERT(main_tread == GetCurrentThread());
    int size = events.size();
    if (size == MAXIMUM_WAIT_OBJECTS - 1) {
        THROW("reached maximum allowed events to wait for");
    }
    events.resize(size + 1);
    events_owners.resize(size + 1);
    events[size] = event_owner.get_event_handle();
    events_owners[size] = &event_owner;
}

void WinPlatform::remove_event(EventOwner& event_owner)
{
    ASSERT(main_tread == GetCurrentThread());
    int size = events.size();
    for (int i = 0; i < size; i++) {
        if (events_owners[i] == &event_owner) {
            for (i++; i < size; i++) {
                events[i - 1] = events[i];
                events_owners[i - 1] = events_owners[i];
            }
            events.resize(size - 1);
            events_owners.resize(size - 1);
            return;
        }
    }
    THROW("event owner not found");
}

void Platform::wait_events()
{
    if (!events.size()) {
        if (!WaitMessage()) {
            THROW("wait failed %d", GetLastError());
        }
        return;
    }

    DWORD r = MsgWaitForMultipleObjectsEx(events.size(), &events[0], INFINITE, QS_ALLINPUT, 0);
    if (r == WAIT_OBJECT_0 + events.size()) {
        return;
    }
    if (r >= WAIT_OBJECT_0 && r <= WAIT_OBJECT_0 + events.size() - 1) {
        events_owners[r - WAIT_OBJECT_0]->on_event();
    } else if (r == WAIT_FAILED) {
        THROW("wait multiple failed %d", GetLastError());
    } else {
        THROW("unexpected wait return %u", r);
    }
}

NamedPipe::ListenerRef NamedPipe::create(const char *name, ListenerInterface& listener_interface)
{
    return (ListenerRef)(new WinListener(name, listener_interface));
}

void NamedPipe::destroy(ListenerRef listener_ref)
{
    delete (WinListener *)listener_ref;
}

void NamedPipe::destroy_connection(ConnectionRef conn_ref)
{
    delete (WinConnection *)conn_ref;
}

int32_t NamedPipe::read(ConnectionRef conn_ref, uint8_t *buf, int32_t size)
{
    return ((WinConnection *)conn_ref)->read(buf, size);
}

int32_t NamedPipe::write(ConnectionRef conn_ref, const uint8_t *buf, int32_t size)
{
    return ((WinConnection *)conn_ref)->write(buf, size);
}

void Platform::wakeup()
{
    if (!PostMessage(paltform_win, WM_USER_WAKEUP, 0, 0)) {
        THROW("post failed %d", GetLastError());
    }
}

bool Platform::process_events()
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return true;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return false;
}

void Platform::msleep(unsigned int msec)
{
    Sleep(msec);
}

void Platform::yield()
{
    Sleep(0);
}

void Platform::set_thread_priority(void* thread, Platform::ThreadPriority in_priority)
{
    ASSERT(thread == NULL);
    int priority;

    switch (in_priority) {
    case PRIORITY_TIME_CRITICAL:
        priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    case PRIORITY_HIGH:
        priority = THREAD_PRIORITY_HIGHEST;
        break;
    case PRIORITY_ABOVE_NORMAL:
        priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case PRIORITY_NORMAL:
        priority = THREAD_PRIORITY_NORMAL;
        break;
    case PRIORITY_BELOW_NORMAL:
        priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case PRIORITY_LOW:
        priority = THREAD_PRIORITY_LOWEST;
        break;
    case PRIORITY_IDLE:
        priority = THREAD_PRIORITY_IDLE;
        break;
    default:
        THROW("invalid priority %d", in_priority);
    }
    SetThreadPriority(GetCurrentThread(), priority);
}

void Platform::set_event_listener(EventListener* listener)
{
    event_listener = listener ? listener : &default_event_listener;
}

TimerID Platform::create_interval_timer(timer_proc_t proc, void* opaque)
{
    Timer* timer = alloc_timer();
    if (!timer) {
        return INVALID_TIMER;
    }
    timer->proc = proc;
    timer->opaque = opaque;
    return timer->id;
}

bool Platform::activate_interval_timer(TimerID timer, unsigned int millisec)
{
    if (timer >= NUM_TIMERS) {
        return false;
    }

    if (!SetTimer(paltform_win, timer + 1, millisec, NULL)) {
        return false;
    }
    return true;
}

bool Platform::deactivate_interval_timer(TimerID timer)
{
    if (timer >= NUM_TIMERS) {
        return false;
    }
    KillTimer(paltform_win, timer + 1);
    return true;
}

void Platform::destroy_interval_timer(TimerID timer)
{
    if (timer == INVALID_TIMER) {
        return;
    }
    ASSERT(timer < NUM_TIMERS);
    KillTimer(paltform_win, timer + 1);
    free_timer(&timers[timer]);
}

uint64_t Platform::get_monolithic_time()
{
    return uint64_t(GetTickCount()) * 1000 * 1000;
}

void Platform::get_temp_dir(std::string& path)
{
    DWORD len = GetTempPathA(0, NULL);
    if (len <= 0) {
        throw Exception("get temp patch failed");
    }
    char* tmp_path = new char[len + 1];
    GetTempPathA(len, tmp_path);
    path = tmp_path;
    delete[] tmp_path;
}

class WinMonitor: public Monitor {
public:
    WinMonitor(int id, const wchar_t* name, const wchar_t* string);

    virtual void set_mode(int width, int height);
    virtual void restore();
    virtual int get_depth() { return _depth;}
    virtual Point get_position();
    virtual Point get_size() const { Point size = {_width, _height}; return size;}
    virtual bool is_out_of_sync() { return _out_of_sync;}
    virtual int get_screen_id() { return 0;}

protected:
    virtual ~WinMonitor();

private:
    void update_position();
    bool change_display_settings(int width, int height, int depth);
    bool best_display_setting(uint32_t width, uint32_t height, uint32_t depth);

private:
    std::wstring _dev_name;
    std::wstring _dev_string;
    bool _active;
    Point _position;
    int _width;
    int _height;
    int _depth;
    bool _out_of_sync;
};

WinMonitor::WinMonitor(int id, const wchar_t* name, const wchar_t* string)
    : Monitor(id)
    , _dev_name (name)
    , _dev_string (string)
    , _active (false)
    , _out_of_sync (false)
{
    update_position();
}

WinMonitor::~WinMonitor()
{
    restore();
}

void WinMonitor::update_position()
{
    DEVMODE mode;
    mode.dmSize = sizeof(DEVMODE);
    mode.dmDriverExtra = 0;
    EnumDisplaySettings(_dev_name.c_str(), ENUM_CURRENT_SETTINGS, &mode);
    _position.x = mode.dmPosition.x;
    _position.y = mode.dmPosition.y;
    _width = mode.dmPelsWidth;
    _height = mode.dmPelsHeight;
    _depth = mode.dmBitsPerPel;
}

Point WinMonitor::get_position()
{
    update_position();
    return _position;
}

bool WinMonitor::change_display_settings(int width, int height, int depth)
{
    DEVMODE mode;
    mode.dmSize = sizeof(DEVMODE);
    mode.dmDriverExtra = 0;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmBitsPerPel = depth;

    return ChangeDisplaySettingsEx(_dev_name.c_str(), &mode, NULL, CDS_FULLSCREEN, NULL)
                                                                        == DISP_CHANGE_SUCCESSFUL;
}

bool WinMonitor::best_display_setting(uint32_t width, uint32_t height, uint32_t depth)
{
    DEVMODE mode;
    DWORD mode_id = 0;
    uint32_t mod_waste = ~0;
    DWORD mod_width;
    DWORD mod_height;
    DWORD mod_depth;
    DWORD mod_frequency;

    mode.dmSize = sizeof(DEVMODE);
    mode.dmDriverExtra = 0;
    while (EnumDisplaySettings(_dev_name.c_str(), mode_id++, &mode)) {
        // Workaround for
        // Lenovo T61p, Nvidia Quadro FX 570M and
        // Lenovo T61, Nvidia Quadro NVS 140M
        //
        // with dual monitors configuration
        //
        // we get strange values from EnumDisplaySettings 640x480x4 frequency 1
        // and calling ChangeDisplaySettingsEx with that configuration result with
        // machine that is stucked for a long period of time
        if (mode.dmDisplayFrequency == 1) {
            continue;
        }

        if (mode.dmPelsWidth >= width && mode.dmPelsHeight >= height) {
            bool replace = false;
            uint32_t curr_waste = mode.dmPelsWidth * mode.dmPelsHeight - width * height;
            if (curr_waste < mod_waste) {
                replace = true;
            } else if (curr_waste == mod_waste) {
                if (mod_depth == mode.dmBitsPerPel) {
                    replace = mode.dmDisplayFrequency > mod_frequency;
                } else if (mod_depth != depth && mode.dmBitsPerPel > mod_depth) {
                    replace = true;
                }
            }
            if (replace) {
                mod_waste = curr_waste;
                mod_width = mode.dmPelsWidth;
                mod_height = mode.dmPelsHeight;
                mod_depth = mode.dmBitsPerPel;
                mod_frequency = mode.dmDisplayFrequency;
            }
        }
    }
    if (mod_waste == ~0) {
        return false;
    }
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    mode.dmPelsWidth = mod_width;
    mode.dmPelsHeight = mod_height;
    mode.dmBitsPerPel = mod_depth;
    mode.dmDisplayFrequency = mod_frequency;

    return ChangeDisplaySettingsEx(_dev_name.c_str(), &mode, NULL, CDS_FULLSCREEN, NULL)
                                                                        == DISP_CHANGE_SUCCESSFUL;
}

void WinMonitor::set_mode(int width, int height)
{
    update_position();
    if (width == _width && height == _height) {
        _out_of_sync = false;
        return;
    }
    self_monitors_change++;
    if (!change_display_settings(width, height, 32) && !best_display_setting(width, height, 32)) {
        _out_of_sync = true;
    } else {
        _out_of_sync = false;
    }
    self_monitors_change--;
    _active = true;
    update_position();
}

void WinMonitor::restore()
{
    if (_active) {
        _active = false;
        self_monitors_change++;
        ChangeDisplaySettingsEx(_dev_name.c_str(), NULL, NULL, 0, NULL);
        self_monitors_change--;
    }
}

static MonitorsList monitors;

const MonitorsList& Platform::init_monitors()
{
    ASSERT(monitors.empty());

    int id = 0;
    DISPLAY_DEVICE device_info;
    DWORD device_id = 0;
    device_info.cb = sizeof(device_info);
    while (EnumDisplayDevices(NULL, device_id, &device_info, 0)) {
        if ((device_info.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
                                      !(device_info.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) {
            monitors.push_back(new WinMonitor(id++, device_info.DeviceName,
                                              device_info.DeviceString));
        }
        device_id++;
    }
    return monitors;
}

void Platform::destroy_monitors()
{
    while (!monitors.empty()) {
        Monitor* monitor = monitors.front();
        monitors.pop_front();
        delete monitor;
    }
}

bool Platform::is_monitors_pos_valid()
{
    return true;
}

void Platform::init()
{
    main_tread = GetCurrentThread();
    create_message_wind();
    init_timers();
}

WaveRecordAbstract* Platform::create_recorder(RecordClinet& client,
                                              uint32_t sampels_per_sec,
                                              uint32_t bits_per_sample,
                                              uint32_t channels)
{
    return new WaveRecorder(client, sampels_per_sec, bits_per_sample, channels);
}

WavePlaybackAbstract* Platform::create_player(uint32_t sampels_per_sec,
                                              uint32_t bits_per_sample,
                                              uint32_t channels)
{
    return new WavePlayer(sampels_per_sec, bits_per_sample, channels);
}

static void toggle_modifier(int key)
{
    INPUT inputs[2];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = inputs[1].ki.wVk = key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

uint32_t Platform::get_keyboard_modifiers()
{
    uint32_t modifiers = 0;
    if ((GetKeyState(VK_SCROLL) & 1)) {
        modifiers |= SCROLL_LOCK_MODIFIER;
    }
    if ((GetKeyState(VK_NUMLOCK) & 1)) {
        modifiers |= NUM_LOCK_MODIFIER;
    }
    if ((GetKeyState(VK_CAPITAL) & 1)) {
        modifiers |= CAPS_LOCK_MODIFIER;
    }
    return modifiers;
}

void Platform::set_keyboard_modifiers(uint32_t modifiers)
{
    if (((modifiers >> SCROLL_LOCK_MODIFIER_SHIFT) & 1) != (GetKeyState(VK_SCROLL) & 1)) {
        toggle_modifier(VK_SCROLL);
    }

    if (((modifiers >> Platform::NUM_LOCK_MODIFIER_SHIFT) & 1) != (GetKeyState(VK_NUMLOCK) & 1)) {
        toggle_modifier(VK_NUMLOCK);
    }

    if (((modifiers >> CAPS_LOCK_MODIFIER_SHIFT) & 1) != (GetKeyState(VK_CAPITAL) & 1)) {
        toggle_modifier(VK_CAPITAL);
    }
}

class WinBaseLocalCursor: public LocalCursor {
public:
    WinBaseLocalCursor() : _handle (0) {}
    void set(Window window) { SetCursor(_handle);}

protected:
    HCURSOR _handle;
};

class WinLocalCursor: public WinBaseLocalCursor {
public:
    WinLocalCursor(CursorData* cursor_data);
    ~WinLocalCursor();

private:
    bool _shared;
};

WinLocalCursor::WinLocalCursor(CursorData* cursor_data)
    : _shared (false)
{
    const CursorHeader& header = cursor_data->header();
    const uint8_t* data = cursor_data->data();
    int cur_size;
    int bits = get_size_bits(header, cur_size);
    if (!bits) {
        THROW("invalid curosr type");
    }
    if (header.type == CURSOR_TYPE_MONO) {
        _handle = CreateCursor(NULL, header.hot_spot_x, header.hot_spot_y,
                               header.width, header.height, data, data + cur_size);
        return;
    }
    ICONINFO icon;
    icon.fIcon = FALSE;
    icon.xHotspot = header.hot_spot_x;
    icon.yHotspot = header.hot_spot_y;
    icon.hbmColor = icon.hbmMask = NULL;
    HDC hdc = GetDC(NULL);
    
    switch (header.type) {
    case CURSOR_TYPE_ALPHA:
    case CURSOR_TYPE_COLOR32:
    case CURSOR_TYPE_COLOR16: {
        BITMAPV5HEADER bmp_hdr;
        ZeroMemory(&bmp_hdr, sizeof(bmp_hdr));
        bmp_hdr.bV5Size = sizeof(bmp_hdr);
        bmp_hdr.bV5Width = header.width;
        bmp_hdr.bV5Height = -header.height;
        bmp_hdr.bV5Planes = 1;
        bmp_hdr.bV5BitCount = bits;
        bmp_hdr.bV5Compression = BI_BITFIELDS;
        if (bits == 32) {
            bmp_hdr.bV5RedMask   = 0x00FF0000;
            bmp_hdr.bV5GreenMask = 0x0000FF00;
            bmp_hdr.bV5BlueMask  = 0x000000FF;
        } else if (bits == 16) {
            bmp_hdr.bV5RedMask   = 0x00007C00;
            bmp_hdr.bV5GreenMask = 0x000003E0;
            bmp_hdr.bV5BlueMask  = 0x0000001F;
        }
        if (header.type == CURSOR_TYPE_ALPHA) {
            bmp_hdr.bV5AlphaMask = 0xFF000000;
        }
        void* bmp_pixels = NULL;
        icon.hbmColor = CreateDIBSection(hdc, (BITMAPINFO *)&bmp_hdr, DIB_RGB_COLORS, &bmp_pixels,
                                         NULL, 0);
        memcpy(bmp_pixels, data, cur_size);
        icon.hbmMask = CreateBitmap(header.width, header.height, 1, 1,
                                    (header.type == CURSOR_TYPE_ALPHA) ? NULL :
                                                                   (CONST VOID *)(data + cur_size));
        break;
    }
    case CURSOR_TYPE_COLOR4: {
        BITMAPINFO* bmp_info;
        bmp_info = (BITMAPINFO *)new uint8_t[sizeof(BITMAPINFO) + (sizeof(RGBQUAD) << bits)];
        ZeroMemory(bmp_info, sizeof(BITMAPINFO));
        bmp_info->bmiHeader.biSize = sizeof(bmp_info->bmiHeader);
        bmp_info->bmiHeader.biWidth = header.width;
        bmp_info->bmiHeader.biHeight = -header.height;
        bmp_info->bmiHeader.biPlanes = 1;
        bmp_info->bmiHeader.biBitCount = bits;
        bmp_info->bmiHeader.biCompression =  BI_RGB;
        memcpy(bmp_info->bmiColors, data + cur_size, sizeof(RGBQUAD) << bits);
        icon.hbmColor = CreateDIBitmap(hdc, &bmp_info->bmiHeader, CBM_INIT, data,
                                       bmp_info, DIB_RGB_COLORS);
        icon.hbmMask = CreateBitmap(header.width, header.height, 1, 1,
                                    (CONST VOID *)(data + cur_size + (sizeof(uint32_t) << bits)));
        delete[] (uint8_t *)bmp_info;
        break;
    }
    case CURSOR_TYPE_COLOR24:
    case CURSOR_TYPE_COLOR8:
    default:
        LOG_WARN("unsupported cursor type %d", header.type);
        _handle = LoadCursor(NULL, IDC_ARROW);
        _shared = true;
        ReleaseDC(NULL, hdc);
        return;
    }

    ReleaseDC(NULL, hdc);

    if (icon.hbmColor && icon.hbmMask) {
        _handle = CreateIconIndirect(&icon);
    }
    if (icon.hbmMask) {
        DeleteObject(icon.hbmMask);
    }
    if (icon.hbmColor) {
        DeleteObject(icon.hbmColor);
    }
}

WinLocalCursor::~WinLocalCursor()
{
    if (_handle && !_shared) {
        DestroyCursor(_handle);
    }
}

LocalCursor* Platform::create_local_cursor(CursorData* cursor_data)
{
    return new WinLocalCursor(cursor_data);
}

class WinInactiveCursor: public WinBaseLocalCursor {
public:
    WinInactiveCursor() { _handle = LoadCursor(NULL, IDC_NO);}
};

LocalCursor* Platform::create_inactive_cursor()
{
    return new WinInactiveCursor();
}

class WinDefaultCursor: public WinBaseLocalCursor {
public:
    WinDefaultCursor() { _handle = LoadCursor(NULL, IDC_ARROW);}
};

LocalCursor* Platform::create_default_cursor()
{
    return new WinDefaultCursor();
}

void Platform::set_display_mode_listner(DisplayModeListner* listener)
{
}

Icon* Platform::load_icon(int id)
{
    HICON icon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(id));
    if (!icon) {
        return NULL;
    }
    return new WinIcon(icon);
}


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

#include <shlobj.h>

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

#define SPICE_CONFIG_DIR "spicec\\"

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
static ProcessLoop* main_loop = NULL;

static const unsigned long MODAL_LOOP_TIMER_ID = 1;
static const int MODAL_LOOP_DEFAULT_TIMEOUT = 100;
static bool modal_loop_active = false;
static bool set_modal_loop_timer();

void Platform::send_quit_request()
{
    ASSERT(main_loop);
    main_loop->quit(0);
}

static LRESULT CALLBACK PlatformWinProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_TIMER:
        if (modal_loop_active) {
            main_loop->timers_action();
            if (!set_modal_loop_timer()) {
                LOG_WARN("failed to set modal loop timer");
            }
        } else {
            LOG_WARN("received WM_TIMER not inside a modal loop");
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

NamedPipe::ListenerRef NamedPipe::create(const char *name, ListenerInterface& listener_interface)
{
    ASSERT(main_loop && main_loop->is_same_thread(pthread_self()));
    return (ListenerRef)(new WinListener(name, listener_interface, *main_loop));
}

void NamedPipe::destroy(ListenerRef listener_ref)
{
    ASSERT(main_loop && main_loop->is_same_thread(pthread_self()));
    delete (WinListener *)listener_ref;
}

void NamedPipe::destroy_connection(ConnectionRef conn_ref)
{
    ASSERT(main_loop && main_loop->is_same_thread(pthread_self()));
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

uint64_t Platform::get_process_id()
{
    static uint64_t pid = GetCurrentProcessId();
    return pid;
}

uint64_t Platform::get_thread_id()
{
    return GetCurrentThreadId();
}

class WinMonitor: public Monitor {
public:
    WinMonitor(int id, const wchar_t* name, const wchar_t* string);

    virtual int get_depth() { return _depth;}
    virtual Point get_position();
    virtual Point get_size() const { Point size = {_width, _height}; return size;}
    virtual bool is_out_of_sync() { return _out_of_sync;}
    virtual int get_screen_id() { return 0;}

protected:
    virtual ~WinMonitor();
    virtual void do_set_mode(int width, int height);
    virtual void do_restore();

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
    do_restore();
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

void WinMonitor::do_set_mode(int width, int height)
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

void WinMonitor::do_restore()
{
    if (_active) {
        _active = false;
        self_monitors_change++;
        ChangeDisplaySettingsEx(_dev_name.c_str(), NULL, NULL, 0, NULL);
        self_monitors_change--;
        update_position();
    }
}

static MonitorsList monitors;
static Monitor* primary_monitor = NULL;

const MonitorsList& Platform::init_monitors()
{
    ASSERT(monitors.empty());

    int id = 0;
    Monitor* mon;
    DISPLAY_DEVICE device_info;
    DWORD device_id = 0;
    device_info.cb = sizeof(device_info);
    while (EnumDisplayDevices(NULL, device_id, &device_info, 0)) {
        if ((device_info.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
                 !(device_info.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) {
            mon = new WinMonitor(id++, device_info.DeviceName, device_info.DeviceString);
            monitors.push_back(mon);
            if (device_info.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
                primary_monitor = mon;
            }
        }
        device_id++;
    }
    return monitors;
}

void Platform::destroy_monitors()
{
    primary_monitor = NULL;
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

void Platform::get_spice_config_dir(std::string& path)
{
    char app_data_path[MAX_PATH];
    HRESULT res = SHGetFolderPathA(NULL, CSIDL_APPDATA,  NULL, 0, app_data_path);
    if (res != S_OK) {
        throw Exception("get user app data dir failed");
    }

    path = app_data_path;
    if (strcmp((app_data_path + strlen(app_data_path) - 2), "\\") != 0) {
        path += "\\";
    }
    path += SPICE_CONFIG_DIR;
}

void Platform::init()
{
    create_message_wind();
}

void Platform::set_process_loop(ProcessLoop& main_process_loop)
{
    main_loop = &main_process_loop;
}

WaveRecordAbstract* Platform::create_recorder(RecordClient& client,
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

uint32_t Platform::get_keyboard_lock_modifiers()
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

void Platform::set_keyboard_lock_modifiers(uint32_t modifiers)
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

#define KEY_BIT(keymap, key, bit) (keymap[key] & 0x80 ? bit : 0)

uint32_t Platform::get_keyboard_modifiers()
{
    BYTE keymap[256];

    if (!GetKeyboardState(keymap)) {
        return 0;
    }
    return KEY_BIT(keymap, VK_LSHIFT, L_SHIFT_MODIFIER) |
           KEY_BIT(keymap, VK_RSHIFT, R_SHIFT_MODIFIER) |
           KEY_BIT(keymap, VK_LCONTROL, L_CTRL_MODIFIER) |
           KEY_BIT(keymap, VK_RCONTROL, R_CTRL_MODIFIER) |
           KEY_BIT(keymap, VK_LMENU, L_ALT_MODIFIER) |
           KEY_BIT(keymap, VK_RMENU, R_ALT_MODIFIER);
}

void Platform::reset_cursor_pos()
{
    if (!primary_monitor) {
        return;
    }
    Point pos =  primary_monitor->get_position();
    Point size =  primary_monitor->get_size();
    SetCursorPos(pos.x + size.x / 2, pos.y + size.y / 2);
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

void WinPlatform::enter_modal_loop()
{
    if (modal_loop_active) {
        LOG_INFO("modal loop already active");
        return;
    }

    if (set_modal_loop_timer()) {
        modal_loop_active = true;
    } else {
        LOG_WARN("failed to create modal loop timer");
    }
}

static bool set_modal_loop_timer()
{
    int timeout = main_loop->get_soonest_timeout();
    if (timeout == INFINITE) {
        timeout = MODAL_LOOP_DEFAULT_TIMEOUT; /* for cases timeouts are added after
                                                 the enterance to the loop*/
    }

    if (!SetTimer(paltform_win, MODAL_LOOP_TIMER_ID, timeout, NULL)) {
        return false;
    }
    return true;
}

void WinPlatform::exit_modal_loop()
{
    if (!modal_loop_active) {
        LOG_INFO("not inside the loop");
        return;
    }
    KillTimer(paltform_win, MODAL_LOOP_TIMER_ID);
    modal_loop_active = false;
}

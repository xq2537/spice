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
#include <fstream>
#include <windows.h>
extern "C" {
#include "pthread.h"
}

//#define OPEN_CONSOLE
#ifdef OPEN_CONSOLE
#include <io.h>
#include <conio.h>
#endif

#include "application.h"
#include "debug.h"
#include "utils.h"

HINSTANCE instance = NULL;

static void init_winsock()
{
    WSADATA wsaData;
    int res;

    if ((res = WSAStartup(MAKEWORD(2, 2), &wsaData)) != 0) {
        THROW("WSAStartup failed %d", res);
    }
}

char* version_string = "???";
static char _version_string[40];

static void init_version_string()
{
    DWORD handle;
    DWORD verrsion_inf_size = GetFileVersionInfoSizeA(__argv[0], &handle);
    if (verrsion_inf_size == 0) {
        return;
    }
    AutoArray<uint8_t> info_buf (new uint8_t[verrsion_inf_size]);
    if (!GetFileVersionInfoA(__argv[0], handle, verrsion_inf_size, info_buf.get())) {
         return;
    }
    UINT size;
    VS_FIXEDFILEINFO *file_info;
    if (!VerQueryValueA(info_buf.get(), "\\", (VOID**)&file_info, &size) ||
            size < sizeof(VS_FIXEDFILEINFO)) {
        return;
    }
    sprintf(_version_string, "%d.%d.%d.%d",
        file_info->dwFileVersionMS >> 16,
        file_info->dwFileVersionMS & 0x0ffff,
        file_info->dwFileVersionLS >> 16,
        file_info->dwFileVersionLS & 0x0ffff);
    version_string = _version_string;
}

int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine,
                   int nCmdShow)
{
    int exit_val;

    instance = hInstance;

    try {
        init_version_string();
#ifdef OPEN_CONSOLE
        AllocConsole();
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        int hConHandle = _open_osfhandle((intptr_t)h, _O_TEXT);
        FILE * fp = _fdopen(hConHandle, "w");
        *stdout = *fp;

        h = GetStdHandle(STD_INPUT_HANDLE);
        hConHandle = _open_osfhandle((intptr_t)h, _O_TEXT);
        fp = _fdopen(hConHandle, "r");
        *stdin = *fp;

        h = GetStdHandle(STD_ERROR_HANDLE);
        hConHandle = _open_osfhandle((intptr_t)h, _O_TEXT);
        fp = _fdopen(hConHandle, "w");
        *stderr = *fp;
#endif
        pthread_win32_process_attach_np();
        init_winsock();
        exit_val = Application::main(__argc, __argv, version_string);
        LOG_INFO("Spice client terminated (exitcode = %d)", exit_val);
    } catch (Exception& e) {
        LOG_ERROR("unhandle exception: %s", e.what());
        exit_val = e.get_error_code();
    } catch (std::exception& e) {
        LOG_ERROR("unhandle exception: %s", e.what());
        exit_val = SPICEC_ERROR_CODE_ERROR;
    } catch (...) {
        LOG_ERROR("unhandled exception");
        exit_val = SPICEC_ERROR_CODE_ERROR;
    }
    log4cpp::Category::shutdown();
#ifdef OPEN_CONSOLE
    _getch();
#endif
    pthread_win32_process_detach_np();
    return exit_val;
}


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

#include <stdarg.h>
#include "utils.h"

void string_vprintf(std::string& str, const char* format, va_list ap)
{
    va_list ap_test;
    va_copy(ap_test, ap);
    int len = vsnprintf(NULL, 0, format, ap_test) + 1;
    va_end(ap_test);
    AutoArray<char> buf(new char[len]);
    vsnprintf(buf.get(), len, format, ap);
    str = buf.get();
}

void wstring_vprintf(std::wstring& str, const wchar_t* format, va_list ap)
{
    int buf_size = 256;
    for (;;) {
        AutoArray<wchar_t> buf(new wchar_t[buf_size]);
        va_list ap_test;
        va_copy(ap_test, ap);
        int r = vswprintf(buf.get(), buf_size, format, ap_test);
        va_end(ap_test);
        if (r != -1) {
            str = buf.get();
            return;
        }
        buf_size *= 2;
    }
}


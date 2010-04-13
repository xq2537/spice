/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _H_DEBUG
#define _H_DEBUG

#include <stdlib.h>
#include <sstream>

#include <log4cpp/Category.hh>
#include <log4cpp/convenience.h>

#include "platform.h"

#ifdef WIN32
#define snprintf _snprintf
#endif

#define ON_PANIC() ::abort()

#ifdef RED_DEBUG

#ifdef WIN32
#define ASSERTBREAK DebugBreak()
#else
#define ASSERTBREAK ::abort()
#endif

#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    ASSERTBREAK;                                            \
}

#else

#define ASSERT(cond)

#endif

#ifdef __GNUC__
static inline std::string pretty_func_to_func_name(const std::string& f_name)
{
    std::string name(f_name);
    std::string::size_type end_pos = f_name.find('(');
    if (end_pos == std::string::npos) {
        return f_name;
    }
    std::string::size_type start = f_name.rfind(' ', end_pos);
    if (start == std::string::npos) {
        return f_name;
    }
    end_pos -= ++start;
    return name.substr(start, end_pos);
}

#define FUNC_NAME pretty_func_to_func_name(__PRETTY_FUNCTION__).c_str()
#else
#define FUNC_NAME __FUNCTION__
#endif

#define LOGGER_SECTION(section) LOG4CPP_LOGGER(section)

LOG4CPP_LOGGER("spice")

#define LOG(type, format, ...) {                                                        \
    std::string log_message;                                                            \
    string_printf(log_message, "[%llu:%llu] %s: " format, Platform::get_process_id(),   \
                  Platform::get_thread_id(), FUNC_NAME, ## __VA_ARGS__);                \
    LOG4CPP_##type(logger, log_message.c_str());                                        \
}

#define LOG_INFO(format, ...) LOG(INFO, format, ## __VA_ARGS__)
#define LOG_WARN(format, ...) LOG(WARN, format, ## __VA_ARGS__)
#define LOG_ERROR(format, ...) LOG(ERROR, format, ## __VA_ARGS__)

#define PANIC(format, ...) {                \
    LOG(FATAL, format, ## __VA_ARGS__);     \
    ON_PANIC();                             \
}

#define PANIC_ON(x) if ((x)) {                      \
    LOG(FATAL, "%s panic %s\n", __FUNCTION__, #x);  \
    ON_PANIC();                                     \
}

#define DBGLEVEL 1000

#define DBG(level, format, ...) {               \
    if (level <= DBGLEVEL) {                    \
        LOG(DEBUG, format, ## __VA_ARGS__);     \
    }                                           \
}

#endif // _H_DEBUG


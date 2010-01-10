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
#include "application.h"
#include "config.h"

static void cleanup()
{
    log4cpp::Category::shutdown();
}

static std::string full_version_str;

static void init_version_str()
{
    full_version_str += VERSION;

    if (strlen(PATCHID)) {
        full_version_str += "-";
        full_version_str += PATCHID;
    }

    if (strlen(DISTRIBUTION)) {
        full_version_str += ".";
        full_version_str += DISTRIBUTION;
    }
}

int main(int argc, char** argv)
{
    int exit_val;

    atexit(cleanup);
    try {
        init_version_str();
        exit_val = Application::main(argc, argv, full_version_str.c_str());
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

    return exit_val;
}


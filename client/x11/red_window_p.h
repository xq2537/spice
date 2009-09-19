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

#ifndef _H_RED_WINDOW_P
#define _H_RED_WINDOW_P

#include <GL/glx.h>

typedef Window Win;
typedef GLXContext RedGlContext;
typedef GLXPbuffer RedPbuffer;

class RedWindow;
class Icon;
struct PixelsSource_p;

class RedWindow_p {
public:
    RedWindow_p();

    void migrate(RedWindow& red_window, PixelsSource_p& pix_source, int dest_screen);
    void create(RedWindow& red_window, PixelsSource_p& pix_source, int x, int y,
                unsigned int width, unsigned int height, int in_screen);
    void destroy(PixelsSource_p& pix_source);
    void set_minmax(PixelsSource_p& pix_source, int width, int height);
    void wait_for_reparent();
    void wait_for_map();
    void wait_for_unmap();
    void sync();
    void set_visibale(bool vis) { _visibale = vis;}
    void move_to_current_desktop();
    Window get_window() {return _win;}

    static void win_proc(XEvent& event);
    static Cursor create_invisible_cursor(Window window);

    void set_glx(int width, int height);

protected:
    int _screen;
    Window _win;
    Cursor _invisible_cursor;
    bool _visibale;
    bool _expect_parent;
    Point _show_pos;
    GLXContext _glcont_copy;
    Icon* _icon;
    bool _ignore_foucs;
    bool _shadow_foucs_state;
};

#endif


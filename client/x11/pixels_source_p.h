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

#ifndef _H_PIXELE_SOURSR_P
#define _H_PIXELE_SOURSR_P

#include <X11/X.h>
#ifdef USE_OGL
#include <GL/glu.h>
#endif // USE_OGL
#include <X11/Xdefs.h>
#include <X11/Xutil.h> // required by Xshm.h, but not included by it
#include <X11/extensions/XShm.h>
#include "red_window.h"
#ifdef USE_OGL
#include "red_pixmap_gl.h"
#endif // USE_OGL
#include "pixman_utils.h"

enum {
    PIXELS_SOURCE_TYPE_INVALID,
    PIXELS_SOURCE_TYPE_X_DRAWABLE,
    PIXELS_SOURCE_TYPE_PIXMAP,
#ifdef USE_OGL
    PIXELS_SOURCE_TYPE_GL_TEXTURE,
    PIXELS_SOURCE_TYPE_GL_DRAWABLE,
#endif // USE_OGL
};

struct PixelsSource_p {
    int type;
    union {
        struct {
            XImage* x_image;
            XShmSegmentInfo *shminfo;
            pixman_image_t* pixman_image;
            RedDrawable::Format format;
        } pixmap;

        struct {
            Drawable drawable;
            int screen;
            GC gc;
            int width, height;
#ifdef USE_OGL
            RenderType rendertype;
            union {
                GLXPbuffer pbuff;
                GLuint fbo;
            };
            RedGlContext context;
#endif // USE_OGL
        } x_drawable;

#ifdef USE_OGL
        struct {
            RenderType rendertype;
            Win win;
            GLuint tex;
            GLuint stencil_tex;
            int width, height;
            int width_powed, height_powed;
            union {
                GLXPbuffer pbuff;
                GLuint fbo;
            };
            RedGlContext context;
        } gl;
#endif // USE_OGL
    };
};

struct RedDrawable_p {
    PixelsSource_p source;
};

#endif


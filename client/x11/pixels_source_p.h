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
#include <GL/glu.h>
#include <X11/extensions/XShm.h>
#include "red_window.h"
#include "red_pixmap_gl.h"
#include "pixman_utils.h"

enum {
    PIXELS_SOURCE_TYPE_INVALID,
    PIXELS_SOURCE_TYPE_X_DRAWABLE,
    PIXELS_SOURCE_TYPE_XSHM_DRAWABLE,
    PIXELS_SOURCE_TYPE_PIXMAP,
    PIXELS_SOURCE_TYPE_GL_TEXTURE,
    PIXELS_SOURCE_TYPE_GL_DRAWABLE,
};

struct PixelsSource_p {
    int type;
    union {
        struct {
            Drawable drawable;
            GC gc;
            int width, height;
            RenderType rendertype;
            union {
                GLXPbuffer pbuff;
                GLuint fbo;
            };
            RedGlContext context;
        } x_drawable;

        struct {
            XImage* x_image;
            XShmSegmentInfo *shminfo;
            pixman_image_t* pixman_image;
        } x_shm_drawable;

        struct {
            XImage* x_image;
            pixman_image_t* pixman_image;
        } pixmap;

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
    };
};

struct RedDrawable_p {
    PixelsSource_p source;
};

#endif


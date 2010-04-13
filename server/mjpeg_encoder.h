/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#ifndef _H_MJPEG_ENCODER
#define _H_MJPEG_ENCODER

#include "red_common.h"

typedef struct MJpegEncoder MJpegEncoder;

MJpegEncoder *mjpeg_encoder_new(int width, int height);
void mjpeg_encoder_destroy(MJpegEncoder *encoder);

uint8_t *mjpeg_encoder_get_frame(MJpegEncoder *encoder);
size_t mjpeg_encoder_get_frame_stride(MJpegEncoder *encoder);
int mjpeg_encoder_encode_frame(MJpegEncoder *encoder,
                               uint8_t *buffer, size_t buffer_len);


#endif

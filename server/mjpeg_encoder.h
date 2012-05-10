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

MJpegEncoder *mjpeg_encoder_new(void);
void mjpeg_encoder_destroy(MJpegEncoder *encoder);

uint8_t mjpeg_encoder_get_bytes_per_pixel(MJpegEncoder *encoder);

/*
 * *dest must be either NULL or allocated by malloc, since it might be freed
 * during the encoding, if its size is too small.
 */
int mjpeg_encoder_start_frame(MJpegEncoder *encoder, SpiceBitmapFmt format,
                              int width, int height,
                              uint8_t **dest, size_t *dest_len);
int mjpeg_encoder_encode_scanline(MJpegEncoder *encoder, uint8_t *src_pixels,
                                  size_t image_width);
size_t mjpeg_encoder_end_frame(MJpegEncoder *encoder);


#endif

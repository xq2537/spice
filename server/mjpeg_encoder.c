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

#include "red_common.h"
#include "mjpeg_encoder.h"
#include <jpeglib.h>

struct MJpegEncoder {
    int width;
    int height;
    int stride;
    uint8_t *frame;
    int first_frame;
    int quality;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
};

MJpegEncoder *mjpeg_encoder_new(int width, int height)
{
    MJpegEncoder *enc;

    enc = spice_new0(MJpegEncoder, 1);

    enc->first_frame = TRUE;
    enc->width = width;
    enc->height = height;
    enc->stride = width * 3;
    enc->quality = 70;
    if (enc->stride < width) {
        abort();
    }
    enc->frame = spice_malloc_n(enc->stride, height);

    enc->cinfo.err = jpeg_std_error(&enc->jerr);

    jpeg_create_compress(&enc->cinfo);

    return enc;
}

void mjpeg_encoder_destroy(MJpegEncoder *encoder)
{
    jpeg_destroy_compress(&encoder->cinfo);
    free(encoder->frame);
    free(encoder);
}

uint8_t *mjpeg_encoder_get_frame(MJpegEncoder *encoder)
{
    return encoder->frame;
}
size_t mjpeg_encoder_get_frame_stride(MJpegEncoder *encoder)
{
    return encoder->stride;
}

void init_destination(j_compress_ptr cinfo)
{
}

boolean empty_output_buffer(j_compress_ptr cinfo)
{
    return FALSE;
}

void term_destination(j_compress_ptr cinfo)
{
}

int mjpeg_encoder_encode_frame(MJpegEncoder *encoder,
                               uint8_t *buffer, size_t buffer_len)
{
    struct jpeg_destination_mgr destmgr;
    uint8_t *frame;
    int n;

    destmgr.next_output_byte = buffer;
    destmgr.free_in_buffer = buffer_len;
    destmgr.init_destination = init_destination;
    destmgr.empty_output_buffer = empty_output_buffer;
    destmgr.term_destination = term_destination;

    encoder->cinfo.dest = &destmgr;

    encoder->cinfo.image_width      = encoder->width;
    encoder->cinfo.image_height     = encoder->height;
    encoder->cinfo.input_components = 3;
    encoder->cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&encoder->cinfo);
    encoder->cinfo.dct_method       = JDCT_IFAST;
    jpeg_set_quality(&encoder->cinfo, encoder->quality, TRUE);
    jpeg_start_compress(&encoder->cinfo, encoder->first_frame);

    frame = encoder->frame;
    while (encoder->cinfo.next_scanline < encoder->cinfo.image_height) {
        n = jpeg_write_scanlines(&encoder->cinfo, &frame, 1);
        if (n == 0) { /* Not enough space */
            jpeg_abort_compress(&encoder->cinfo);
            return 0;
        }
        frame += encoder->stride;
    }

    jpeg_finish_compress(&encoder->cinfo);

    encoder->first_frame = FALSE;
    return destmgr.next_output_byte - buffer;
}

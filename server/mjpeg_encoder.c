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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "red_common.h"
#include "mjpeg_encoder.h"
#include <jerror.h>
#include <jpeglib.h>

struct MJpegEncoder {
    uint8_t *row;
    uint32_t row_size;
    int first_frame;
    int quality;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    unsigned int bytes_per_pixel; /* bytes per pixel of the input buffer */
    void (*pixel_converter)(uint8_t *src, uint8_t *dest);
};

MJpegEncoder *mjpeg_encoder_new(void)
{
    MJpegEncoder *enc;

    enc = spice_new0(MJpegEncoder, 1);

    enc->first_frame = TRUE;
    enc->quality = 70;
    enc->cinfo.err = jpeg_std_error(&enc->jerr);
    jpeg_create_compress(&enc->cinfo);

    return enc;
}

void mjpeg_encoder_destroy(MJpegEncoder *encoder)
{
    jpeg_destroy_compress(&encoder->cinfo);
    free(encoder->row);
    free(encoder);
}

uint8_t mjpeg_encoder_get_bytes_per_pixel(MJpegEncoder *encoder)
{
    return encoder->bytes_per_pixel;
}

#ifndef JCS_EXTENSIONS
/* Pixel conversion routines */
static void pixel_rgb24bpp_to_24(uint8_t *src, uint8_t *dest)
{
    /* libjpegs stores rgb, spice/win32 stores bgr */
    *dest++ = src[2]; /* red */
    *dest++ = src[1]; /* green */
    *dest++ = src[0]; /* blue */
}

static void pixel_rgb32bpp_to_24(uint8_t *src, uint8_t *dest)
{
    uint32_t pixel = *(uint32_t *)src;
    *dest++ = (pixel >> 16) & 0xff;
    *dest++ = (pixel >>  8) & 0xff;
    *dest++ = (pixel >>  0) & 0xff;
}
#endif

static void pixel_rgb16bpp_to_24(uint8_t *src, uint8_t *dest)
{
    uint16_t pixel = *(uint16_t *)src;
    *dest++ = ((pixel >> 7) & 0xf8) | ((pixel >> 12) & 0x7);
    *dest++ = ((pixel >> 2) & 0xf8) | ((pixel >> 7) & 0x7);
    *dest++ = ((pixel << 3) & 0xf8) | ((pixel >> 2) & 0x7);
}


/* code from libjpeg 8 to handle compression to a memory buffer
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * Modified 2009 by Guido Vollbeding.
 * This file is part of the Independent JPEG Group's software.
 */
typedef struct {
  struct jpeg_destination_mgr pub; /* public fields */

  unsigned char ** outbuffer;	/* target buffer */
  size_t * outsize;
  uint8_t * buffer;		/* start of buffer */
  size_t bufsize;
} mem_destination_mgr;

static void init_mem_destination(j_compress_ptr cinfo)
{
}

static boolean empty_mem_output_buffer(j_compress_ptr cinfo)
{
  size_t nextsize;
  uint8_t * nextbuffer;
  mem_destination_mgr *dest = (mem_destination_mgr *) cinfo->dest;

  /* Try to allocate new buffer with double size */
  nextsize = dest->bufsize * 2;
  nextbuffer = malloc(nextsize);

  if (nextbuffer == NULL)
    ERREXIT1(cinfo, JERR_OUT_OF_MEMORY, 10);

  memcpy(nextbuffer, dest->buffer, dest->bufsize);

  free(dest->buffer);

  dest->pub.next_output_byte = nextbuffer + dest->bufsize;
  dest->pub.free_in_buffer = dest->bufsize;

  dest->buffer = nextbuffer;
  dest->bufsize = nextsize;

  return TRUE;
}

static void term_mem_destination(j_compress_ptr cinfo)
{
  mem_destination_mgr *dest = (mem_destination_mgr *) cinfo->dest;

  *dest->outbuffer = dest->buffer;
  *dest->outsize = dest->bufsize;
}

/*
 * Prepare for output to a memory buffer.
 * The caller may supply an own initial buffer with appropriate size.
 * Otherwise, or when the actual data output exceeds the given size,
 * the library adapts the buffer size as necessary.
 * The standard library functions malloc/free are used for allocating
 * larger memory, so the buffer is available to the application after
 * finishing compression, and then the application is responsible for
 * freeing the requested memory.
 */

static void
spice_jpeg_mem_dest(j_compress_ptr cinfo,
                    unsigned char ** outbuffer, size_t * outsize)
{
  mem_destination_mgr *dest;
#define OUTPUT_BUF_SIZE  4096	/* choose an efficiently fwrite'able size */

  if (outbuffer == NULL || outsize == NULL)	/* sanity check */
    ERREXIT(cinfo, JERR_BUFFER_SIZE);

  /* The destination object is made permanent so that multiple JPEG images
   * can be written to the same buffer without re-executing jpeg_mem_dest.
   */
  if (cinfo->dest == NULL) {	/* first time for this JPEG object? */
    cinfo->dest = spice_malloc(sizeof(mem_destination_mgr));
  }

  dest = (mem_destination_mgr *) cinfo->dest;
  dest->pub.init_destination = init_mem_destination;
  dest->pub.empty_output_buffer = empty_mem_output_buffer;
  dest->pub.term_destination = term_mem_destination;
  dest->outbuffer = outbuffer;
  dest->outsize = outsize;
  if (*outbuffer == NULL || *outsize == 0) {
    /* Allocate initial buffer */
    *outbuffer = malloc(OUTPUT_BUF_SIZE);
    if (*outbuffer == NULL)
      ERREXIT1(cinfo, JERR_OUT_OF_MEMORY, 10);
    *outsize = OUTPUT_BUF_SIZE;
  }

  dest->pub.next_output_byte = dest->buffer = *outbuffer;
  dest->pub.free_in_buffer = dest->bufsize = *outsize;
}
/* end of code from libjpeg */

int mjpeg_encoder_start_frame(MJpegEncoder *encoder, SpiceBitmapFmt format,
                              int width, int height,
                              uint8_t **dest, size_t *dest_len)
{
    encoder->cinfo.in_color_space   = JCS_RGB;
    encoder->cinfo.input_components = 3;
    encoder->pixel_converter = NULL;

    switch (format) {
    case SPICE_BITMAP_FMT_32BIT:
    case SPICE_BITMAP_FMT_RGBA:
        encoder->bytes_per_pixel = 4;
#ifdef JCS_EXTENSIONS
        encoder->cinfo.in_color_space   = JCS_EXT_BGRX;
        encoder->cinfo.input_components = 4;
#else
        encoder->pixel_converter = pixel_rgb32bpp_to_24;
#endif
        break;
    case SPICE_BITMAP_FMT_16BIT:
        encoder->bytes_per_pixel = 2;
        encoder->pixel_converter = pixel_rgb16bpp_to_24;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        encoder->bytes_per_pixel = 3;
#ifdef JCS_EXTENSIONS
        encoder->cinfo.in_color_space = JCS_EXT_BGR;
#else
        encoder->pixel_converter = pixel_rgb24bpp_to_24;
#endif
        break;
    default:
        spice_warning("unsupported format %d", format);
        return FALSE;
    }

    if (encoder->pixel_converter != NULL) {
        unsigned int stride = width * 3;
        /* check for integer overflow */
        if (stride < width) {
            return FALSE;
        }
        if (encoder->row_size < stride) {
            encoder->row = spice_realloc(encoder->row, stride);
            encoder->row_size = stride;
        }
    }

    spice_jpeg_mem_dest(&encoder->cinfo, dest, dest_len);

    encoder->cinfo.image_width      = width;
    encoder->cinfo.image_height     = height;
    jpeg_set_defaults(&encoder->cinfo);
    encoder->cinfo.dct_method       = JDCT_IFAST;
    jpeg_set_quality(&encoder->cinfo, encoder->quality, TRUE);
    jpeg_start_compress(&encoder->cinfo, encoder->first_frame);

    return TRUE;
}

int mjpeg_encoder_encode_scanline(MJpegEncoder *encoder, uint8_t *src_pixels,
                                  size_t image_width)
{
    unsigned int scanlines_written;
    uint8_t *row;

    row = encoder->row;
    if (encoder->pixel_converter) {
        unsigned int x;
        for (x = 0; x < image_width; x++) {
            encoder->pixel_converter(src_pixels, row);
            row += 3;
            src_pixels += encoder->bytes_per_pixel;
        }
        scanlines_written = jpeg_write_scanlines(&encoder->cinfo, &encoder->row, 1);
    } else {
        scanlines_written = jpeg_write_scanlines(&encoder->cinfo, &src_pixels, 1);
    }
    if (scanlines_written == 0) { /* Not enough space */
        jpeg_abort_compress(&encoder->cinfo);
        return 0;
    }

    return scanlines_written;
}

size_t mjpeg_encoder_end_frame(MJpegEncoder *encoder)
{
    mem_destination_mgr *dest = (mem_destination_mgr *) encoder->cinfo.dest;

    jpeg_finish_compress(&encoder->cinfo);

    encoder->first_frame = FALSE;
    return dest->pub.next_output_byte - dest->buffer;
}

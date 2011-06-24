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


/* code from libjpeg 8 to handle compression to a memory buffer
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * Modified 2009 by Guido Vollbeding.
 * This file is part of the Independent JPEG Group's software.
 */
typedef struct {
  struct jpeg_destination_mgr pub; /* public fields */

  unsigned char ** outbuffer;	/* target buffer */
  unsigned long * outsize;
  unsigned char * newbuffer;	/* newly allocated buffer */
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

  if (dest->newbuffer != NULL)
    free(dest->newbuffer);

  dest->newbuffer = nextbuffer;

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
  *dest->outsize = dest->bufsize - dest->pub.free_in_buffer;
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
jpeg_mem_dest (j_compress_ptr cinfo,
	       unsigned char ** outbuffer, unsigned long * outsize)
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
  dest->newbuffer = NULL;

  if (*outbuffer == NULL || *outsize == 0) {
    /* Allocate initial buffer */
    dest->newbuffer = *outbuffer = malloc(OUTPUT_BUF_SIZE);
    if (dest->newbuffer == NULL)
      ERREXIT1(cinfo, JERR_OUT_OF_MEMORY, 10);
    *outsize = OUTPUT_BUF_SIZE;
  }

  dest->pub.next_output_byte = dest->buffer = *outbuffer;
  dest->pub.free_in_buffer = dest->bufsize = *outsize;
}
/* end of code from libjpeg */

int mjpeg_encoder_encode_frame(MJpegEncoder *encoder,
                               uint8_t **buffer, size_t *buffer_len)
{
    uint8_t *frame;
    int n;

    jpeg_mem_dest(&encoder->cinfo, buffer, buffer_len);

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
    return encoder->cinfo.dest->next_output_byte - *buffer;
}

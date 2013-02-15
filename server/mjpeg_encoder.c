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

#define MJPEG_MAX_FPS 25
#define MJPEG_MIN_FPS 1

#define MJPEG_QUALITY_SAMPLE_NUM 7
static const int mjpeg_quality_samples[MJPEG_QUALITY_SAMPLE_NUM] = {20, 30, 40, 50, 60, 70, 80};

#define MJPEG_LEGACY_STATIC_QUALITY_ID 5 // jpeg quality 70

#define MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH 10
#define MJPEG_IMPROVE_QUALITY_FPS_PERMISSIVE_TH 5

#define MJPEG_AVERAGE_SIZE_WINDOW 3

enum {
    MJPEG_QUALITY_EVAL_TYPE_SET,
    MJPEG_QUALITY_EVAL_TYPE_UPGRADE,
    MJPEG_QUALITY_EVAL_TYPE_DOWNGRADE,
};

enum {
    MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE,
    MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE,
};

typedef struct MJpegEncoderQualityEval {
    int type;
    int reason;

    uint64_t encoded_size_by_quality[MJPEG_QUALITY_SAMPLE_NUM];
    /* lower limit for the current evaluation round */
    int min_quality_id;
    int min_quality_fps; // min fps for the given quality
    /* upper limit for the current evaluation round */
    int max_quality_id;
    int max_quality_fps; // max fps for the given quality
    /* tracking the best sampled fps so far */
    int max_sampled_fps;
    int max_sampled_fps_quality_id;
} MJpegEncoderQualityEval;

/*
 * Adjusting the stream jpeg quality and frame rate (fps):
 * When during_quality_eval=TRUE, we compress different frames with different
 * jpeg quality. By considering (1) the resulting compression ratio, and (2) the available
 * bit rate, we evaluate the max frame frequency for the stream with the given quality,
 * and we choose the highest quality that will allow a reasonable frame rate.
 * during_quality_eval is set for new streams and can also be set any time we want
 * to re-evaluate the stream parameters (e.g., when the bit rate and/or
 * compressed frame size significantly change).
 */
typedef struct MJpegEncoderRateControl {
    int during_quality_eval;
    MJpegEncoderQualityEval quality_eval_data;

    uint64_t byte_rate;
    int quality_id;
    uint32_t fps;
    /* the encoded frame size which the quality and the fps evaluation was based upon */
    uint64_t base_enc_size;

    uint64_t last_enc_size;

    uint64_t sum_recent_enc_size;
    uint32_t num_recent_enc_frames;
} MJpegEncoderRateControl;

struct MJpegEncoder {
    uint8_t *row;
    uint32_t row_size;
    int first_frame;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    unsigned int bytes_per_pixel; /* bytes per pixel of the input buffer */
    void (*pixel_converter)(uint8_t *src, uint8_t *dest);

    int rate_control_is_active;
    MJpegEncoderRateControl rate_control;
    MJpegEncoderRateControlCbs cbs;
    void *cbs_opaque;
};

static inline void mjpeg_encoder_reset_quality(MJpegEncoder *encoder,
                                               int quality_id,
                                               uint32_t fps,
                                               uint64_t frame_enc_size);
static uint32_t get_max_fps(uint64_t frame_size, uint64_t bytes_per_sec);

MJpegEncoder *mjpeg_encoder_new(int bit_rate_control, uint64_t starting_bit_rate,
                                MJpegEncoderRateControlCbs *cbs, void *opaque)
{
    MJpegEncoder *enc;

    spice_assert(!bit_rate_control || (cbs && cbs->get_roundtrip_ms && cbs->get_source_fps));

    enc = spice_new0(MJpegEncoder, 1);

    enc->first_frame = TRUE;
    enc->rate_control_is_active = bit_rate_control;
    enc->rate_control.byte_rate = starting_bit_rate / 8;
    if (bit_rate_control) {
        enc->cbs = *cbs;
        enc->cbs_opaque = opaque;
        mjpeg_encoder_reset_quality(enc, MJPEG_QUALITY_SAMPLE_NUM / 2, 5, 0);
        enc->rate_control.during_quality_eval = TRUE;
        enc->rate_control.quality_eval_data.type = MJPEG_QUALITY_EVAL_TYPE_SET;
        enc->rate_control.quality_eval_data.reason = MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE;
    } else {
        mjpeg_encoder_reset_quality(enc, MJPEG_LEGACY_STATIC_QUALITY_ID, MJPEG_MAX_FPS, 0);
    }

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

static inline uint32_t mjpeg_encoder_get_latency(MJpegEncoder *encoder)
{
    return encoder->cbs.get_roundtrip_ms ?
        encoder->cbs.get_roundtrip_ms(encoder->cbs_opaque) / 2 : 0;
}

static uint32_t get_max_fps(uint64_t frame_size, uint64_t bytes_per_sec)
{
    double fps;
    double send_time_ms;

    if (!bytes_per_sec) {
        return 0;
    }
    send_time_ms = frame_size * 1000.0 / bytes_per_sec;
    fps = send_time_ms ? 1000 / send_time_ms : MJPEG_MAX_FPS;
    return fps;
}

static inline void mjpeg_encoder_reset_quality(MJpegEncoder *encoder,
                                               int quality_id,
                                               uint32_t fps,
                                               uint64_t frame_enc_size)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;

    rate_control->during_quality_eval = FALSE;

    if (rate_control->quality_id != quality_id) {
        rate_control->last_enc_size = 0;
    }

    rate_control->quality_id = quality_id;
    memset(&rate_control->quality_eval_data, 0, sizeof(MJpegEncoderQualityEval));
    rate_control->quality_eval_data.max_quality_id = MJPEG_QUALITY_SAMPLE_NUM - 1;
    rate_control->quality_eval_data.max_quality_fps = MJPEG_MAX_FPS;

    rate_control->fps = MAX(MJPEG_MIN_FPS, fps);
    rate_control->fps = MIN(MJPEG_MAX_FPS, rate_control->fps);
    rate_control->base_enc_size = frame_enc_size;

    rate_control->sum_recent_enc_size = 0;
    rate_control->num_recent_enc_frames = 0;
}

#define QUALITY_WAS_EVALUATED(encoder, quality) \
    ((encoder)->rate_control.quality_eval_data.encoded_size_by_quality[(quality)] != 0)

/*
 * Adjust the stream's jpeg quality and frame rate.
 * We evaluate the compression ratio of different jpeg qualities;
 * We compress successive frames with different qualities,
 * and then we estimate the stream frame rate according to the currently
 * evaluated jpeg quality and available bit rate.
 *
 * During quality evaluation, mjpeg_encoder_eval_quality is called before a new
 * frame is encoded. mjpeg_encoder_eval_quality examines the encoding size of
 * the previously encoded frame, and determines whether to continue evaluation
 * (and chnages the quality for the frame that is going to be encoded),
 * or stop evaluation (and sets the quality and frame rate for the stream).
 * When qualities are scanned, we assume monotonicity of compression ratio
 * as a function of jpeg quality. When we reach a quality with too small, or
 * big enough compression ratio, we stop the evaluation and set the stream parameters.
*/
static inline void mjpeg_encoder_eval_quality(MJpegEncoder *encoder)
{
    MJpegEncoderRateControl *rate_control;
    MJpegEncoderQualityEval *quality_eval;
    uint32_t fps, src_fps;
    uint64_t enc_size;
    uint32_t final_quality_id;
    uint32_t final_fps;
    uint64_t final_quality_enc_size;

    rate_control = &encoder->rate_control;
    quality_eval = &rate_control->quality_eval_data;

    spice_assert(rate_control->during_quality_eval);

    /* retrieving the encoded size of the last encoded frame */
    enc_size = quality_eval->encoded_size_by_quality[rate_control->quality_id];
    if (enc_size == 0) {
        spice_debug("size info missing");
        return;
    }

    src_fps = encoder->cbs.get_source_fps(encoder->cbs_opaque);

    fps = get_max_fps(enc_size, rate_control->byte_rate);
    spice_debug("mjpeg %p: jpeg %d: %.2f (KB) fps %d src-fps %u",
                encoder,
                mjpeg_quality_samples[rate_control->quality_id],
                enc_size / 1024.0,
                fps,
                src_fps);

    if (fps > quality_eval->max_sampled_fps ||
        ((fps == quality_eval->max_sampled_fps || fps >= src_fps) &&
         rate_control->quality_id > quality_eval->max_sampled_fps_quality_id)) {
        quality_eval->max_sampled_fps = fps;
        quality_eval->max_sampled_fps_quality_id = rate_control->quality_id;
    }

    /*
     * Choosing whether to evaluate another quality, or to complete evaluation
     * and set the stream parameters according to one of the qualities that
     * were already sampled.
     */

    if (rate_control->quality_id > MJPEG_QUALITY_SAMPLE_NUM / 2 &&
        fps < MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH &&
        fps < src_fps) {
        /*
         * When the jpeg quality is bigger than the median quality, prefer a reasonable
         * frame rate over improving the quality
         */
        spice_debug("fps < %d && (fps < src_fps), quality %d",
                MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH,
                mjpeg_quality_samples[rate_control->quality_id]);
        if (QUALITY_WAS_EVALUATED(encoder, rate_control->quality_id - 1)) {
            /* the next worse quality was already evaluated and it passed the frame
             * rate thresholds (we know that, because we continued evaluating a better
             * quality) */
            rate_control->quality_id--;
            goto complete_sample;
        } else {
            /* evaluate the next worse quality */
            rate_control->quality_id--;
        }
    } else if ((fps > MJPEG_IMPROVE_QUALITY_FPS_PERMISSIVE_TH &&
                fps >= 0.66 * quality_eval->min_quality_fps) || fps >= src_fps) {
        /* When the jpeg quality is worse than the median one (see first condition), we allow a less
           strict threshold for fps, in order to improve the jpeg quality */
        if (rate_control->quality_id + 1 == MJPEG_QUALITY_SAMPLE_NUM ||
            rate_control->quality_id >= quality_eval->max_quality_id ||
            QUALITY_WAS_EVALUATED(encoder, rate_control->quality_id + 1)) {
            /* best quality has been reached, or the next (better) quality was
             * already evaluated and didn't pass the fps thresholds */
            goto complete_sample;
        } else {
            if (rate_control->quality_id == MJPEG_QUALITY_SAMPLE_NUM / 2 &&
                fps < MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH &&
                fps < src_fps) {
                goto complete_sample;
            }
            /* evaluate the next quality as well*/
            rate_control->quality_id++;
        }
    } else { // very small frame rate, try to improve by downgrading the quality
        if (rate_control->quality_id == 0 ||
            rate_control->quality_id <= quality_eval->min_quality_id) {
            goto complete_sample;
        } else if (QUALITY_WAS_EVALUATED(encoder, rate_control->quality_id - 1)) {
            rate_control->quality_id--;
            goto complete_sample;
        } else {
            /* evaluate the next worse quality */
            rate_control->quality_id--;
        }
    }
    return;

complete_sample:
    if (quality_eval->max_sampled_fps != 0) {
        /* covering a case were monotonicity was violated and we sampled
           a better jepg quality, with better frame rate. */
        final_quality_id = MAX(rate_control->quality_id,
                               quality_eval->max_sampled_fps_quality_id);
    } else {
        final_quality_id = rate_control->quality_id;
    }
    final_quality_enc_size = quality_eval->encoded_size_by_quality[final_quality_id];
    final_fps = get_max_fps(final_quality_enc_size,
                            rate_control->byte_rate);

    if (final_quality_id == quality_eval->min_quality_id) {
        final_fps = MAX(final_fps, quality_eval->min_quality_fps);
    }
    if (final_quality_id == quality_eval->max_quality_id) {
        final_fps = MIN(final_fps, quality_eval->max_quality_fps);
    }
    mjpeg_encoder_reset_quality(encoder, final_quality_id, final_fps, final_quality_enc_size);
    rate_control->sum_recent_enc_size = final_quality_enc_size;
    rate_control->num_recent_enc_frames = 1;

    spice_debug("MJpeg quality sample end %p: quality %d fps %d",
                encoder, mjpeg_quality_samples[rate_control->quality_id], rate_control->fps);
}

static void mjpeg_encoder_quality_eval_set_upgrade(MJpegEncoder *encoder,
                                                   int reason,
                                                   uint32_t min_quality_id,
                                                   uint32_t min_quality_fps)
{
    MJpegEncoderQualityEval *quality_eval = &encoder->rate_control.quality_eval_data;

    encoder->rate_control.during_quality_eval = TRUE;
    quality_eval->type = MJPEG_QUALITY_EVAL_TYPE_UPGRADE;
    quality_eval->reason = reason;
    quality_eval->min_quality_id = min_quality_id;
    quality_eval->min_quality_fps = min_quality_fps;
}

static void mjpeg_encoder_quality_eval_set_downgrade(MJpegEncoder *encoder,
                                                     int reason,
                                                     uint32_t max_quality_id,
                                                     uint32_t max_quality_fps)
{
    MJpegEncoderQualityEval *quality_eval = &encoder->rate_control.quality_eval_data;

    encoder->rate_control.during_quality_eval = TRUE;
    quality_eval->type = MJPEG_QUALITY_EVAL_TYPE_DOWNGRADE;
    quality_eval->reason = reason;
    quality_eval->max_quality_id = max_quality_id;
    quality_eval->max_quality_fps = max_quality_fps;
}

static void mjpeg_encoder_adjust_params_to_bit_rate(MJpegEncoder *encoder)
{
    MJpegEncoderRateControl *rate_control;
    MJpegEncoderQualityEval *quality_eval;
    uint64_t new_avg_enc_size;
    uint32_t new_fps;
    uint32_t latency = 0;
    uint32_t src_fps;

    if (!encoder->rate_control_is_active) {
        return;
    }

    rate_control = &encoder->rate_control;
    quality_eval = &rate_control->quality_eval_data;

    if (!rate_control->last_enc_size) {
        spice_debug("missing sample size");
        return;
    }

    if (rate_control->during_quality_eval) {
        quality_eval->encoded_size_by_quality[rate_control->quality_id] = rate_control->last_enc_size;
        mjpeg_encoder_eval_quality(encoder);
        return;
    }

    spice_assert(rate_control->num_recent_enc_frames);

    if (rate_control->num_recent_enc_frames < MJPEG_AVERAGE_SIZE_WINDOW &&
        rate_control->num_recent_enc_frames < rate_control->fps) {
        return;
    }

    latency = mjpeg_encoder_get_latency(encoder);
    new_avg_enc_size = rate_control->sum_recent_enc_size /
                       rate_control->num_recent_enc_frames;
    new_fps = get_max_fps(new_avg_enc_size, rate_control->byte_rate);

    spice_debug("cur-fps=%u new-fps=%u (new/old=%.2f) |"
                "bit-rate=%.2f (Mbps) latency=%u (ms) quality=%d |"
                " new-size-avg %lu , base-size %lu, (new/old=%.2f) ",
                rate_control->fps, new_fps, ((double)new_fps)/rate_control->fps,
                ((double)rate_control->byte_rate*8)/1024/1024,
                latency,
                mjpeg_quality_samples[rate_control->quality_id],
                new_avg_enc_size, rate_control->base_enc_size,
                rate_control->base_enc_size ?
                    ((double)new_avg_enc_size) / rate_control->base_enc_size :
                    1);

     src_fps = encoder->cbs.get_source_fps(encoder->cbs_opaque);

    /*
     * The ratio between the new_fps and the current fps reflects the changes
     * in latency and frame size. When the change passes a threshold,
     * we re-evaluate the quality and frame rate.
     */
    if (new_fps > rate_control->fps &&
        (rate_control->fps < src_fps || rate_control->quality_id < MJPEG_QUALITY_SAMPLE_NUM - 1)) {
        spice_debug("mjpeg %p FPS CHANGE >> :  re-evaluating params", encoder);
        mjpeg_encoder_quality_eval_set_upgrade(encoder, MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE,
                                               rate_control->quality_id, /* fps has improved -->
                                                                            don't allow stream quality
                                                                            to deteriorate */
                                               rate_control->fps);
    } else if (new_fps < rate_control->fps && new_fps < src_fps) {
        spice_debug("mjpeg %p FPS CHANGE << : re-evaluating params", encoder);
        mjpeg_encoder_quality_eval_set_downgrade(encoder, MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE,
                                                 rate_control->quality_id,
                                                 rate_control->fps);
    }

    if (rate_control->during_quality_eval) {
        quality_eval->encoded_size_by_quality[rate_control->quality_id] = new_avg_enc_size;
        mjpeg_encoder_eval_quality(encoder);
    }
}

int mjpeg_encoder_start_frame(MJpegEncoder *encoder, SpiceBitmapFmt format,
                              int width, int height,
                              uint8_t **dest, size_t *dest_len)
{
    uint32_t quality;

    mjpeg_encoder_adjust_params_to_bit_rate(encoder);

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
    quality = mjpeg_quality_samples[encoder->rate_control.quality_id];
    jpeg_set_quality(&encoder->cinfo, quality, TRUE);
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
        encoder->rate_control.last_enc_size = 0;
        return 0;
    }

    return scanlines_written;
}

size_t mjpeg_encoder_end_frame(MJpegEncoder *encoder)
{
    mem_destination_mgr *dest = (mem_destination_mgr *) encoder->cinfo.dest;
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;

    jpeg_finish_compress(&encoder->cinfo);

    encoder->first_frame = FALSE;
    rate_control->last_enc_size = dest->pub.next_output_byte - dest->buffer;

    if (!rate_control->during_quality_eval) {
        if (rate_control->num_recent_enc_frames >= MJPEG_AVERAGE_SIZE_WINDOW) {
            rate_control->num_recent_enc_frames = 0;
            rate_control->sum_recent_enc_size = 0;
        }
        rate_control->sum_recent_enc_size += rate_control->last_enc_size;
        rate_control->num_recent_enc_frames++;
    }
    return encoder->rate_control.last_enc_size;
}

uint32_t mjpeg_encoder_get_fps(MJpegEncoder *encoder)
{
    if (!encoder->rate_control_is_active) {
        spice_warning("bit rate control is not active");
    }
    return encoder->rate_control.fps;
}

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

#if defined(YUV32)
#define PIXEL_SIZE 4
#define R(pixel) (((uint8_t*)(pixel))[0])
#define G(pixel) (((uint8_t*)(pixel))[1])
#define B(pixel) (((uint8_t*)(pixel))[2])

#define FUNC_NAME(name) name##_32bpp

#elif defined(YUV24)
#define PIXEL_SIZE 3
#define R(pixel) (((uint8_t*)(pixel))[0])
#define G(pixel) (((uint8_t*)(pixel))[1])
#define B(pixel) (((uint8_t*)(pixel))[2])

#define FUNC_NAME(name) name##_24bpp

#elif defined(YUV16)
#define PIXEL_SIZE 2
#define PIX16(pixel) (*(uint16_t*)(pixel))

#define R(pixel) ((PIX16(pixel) << 3) & 0xff)
#define G(pixel) ((PIX16(pixel) >> 2) & 0xff)
#define B(pixel) ((PIX16(pixel) >> 7) & 0xff)

#define FUNC_NAME(name) name##_16bpp

#else
#error "invalid format."
#endif

#define Y(pixel) (((66 * R(pixel) + 129 * G(pixel) + 25 * B(pixel) + 128) >> 8) + 16)
#define U(pixel) (((-38 * R(pixel) - 74 * G(pixel) + 112 * B(pixel) + 128) >> 8) + 128)
#define V(pixel) (((112 * R(pixel) - 94 * G(pixel) - 18 * B(pixel) + 128) >> 8) + 128)

static inline void FUNC_NAME(red_rgb_to_yuv420_line)(const uint8_t* line0, const uint8_t* line1,
                                                     const uint32_t width, uint8_t* y, uint8_t *u,
                                                     uint8_t *v, int y_stride)
{
    int i;

    // Y = (0.257 * R) + (0.504 * G) + (0.098 * B) + 16
    // Cb = U = -(0.148 * R) - (0.291 * G) + (0.439 * B) + 128
    // Cr = V = (0.439 * R) - (0.368 * G) - (0.071 * B) + 128

    for (i = 0; i < width / 2; i++) {
        *y = Y(line0);
        *(y + 1) = Y(line0 + PIXEL_SIZE);
        *(y + y_stride) = Y(line1);
        *(y + y_stride + 1) = Y(line1 + PIXEL_SIZE);

        u[i] = (U(line0) + U(line0 + PIXEL_SIZE) + U(line1) + U(line1 + PIXEL_SIZE)) / 4;
        v[i] = (V(line0) + V(line0 + PIXEL_SIZE) + V(line1) + V(line1 + PIXEL_SIZE)) / 4;

        line0 += 2 * PIXEL_SIZE;
        line1 += 2 * PIXEL_SIZE;
        y += 2;
    }

    if ((width & 1)) {
        *y = Y(line0);
        *(y + 1) = *y;
        *(y + y_stride) = Y(line1);
        *(y + y_stride + 1) = *(y + y_stride);
        u[i] = (U(line0) + U(line1)) / 2;
        v[i] = (V(line0) + V(line1)) / 2;
    }
}

static inline int FUNC_NAME(red_rgb_to_yuv420)(RedWorker *worker, const Rect *src,
                                               const Bitmap *image, AVFrame *frame,
                                               long phys_delta, int memslot_id, int id,
                                               Stream *stream)
{
    QXLDataChunk *chunk;
    uint32_t image_stride;
    int y_stride;
    uint8_t* y;
    uint8_t* u;
    uint8_t* v;
    int offset;
    int i;

    y = frame->data[0];
    u = frame->data[1];
    v = frame->data[2];
    y_stride = frame->linesize[0];

    offset = 0;
    chunk = (QXLDataChunk *)(image->data + phys_delta);
    image_stride = image->stride;

    const int skip_lines = stream->top_down ? src->top : image->y - (src->bottom - 0);
    for (i = 0; i < skip_lines; i++) {
        red_get_image_line(worker, &chunk, &offset, image_stride, phys_delta, memslot_id);
    }

    const int image_hight = src->bottom - src->top;
    const int image_width = src->right - src->left;
    for (i = 0; i < image_hight / 2; i++) {
        uint8_t* line0 = red_get_image_line(worker, &chunk, &offset, image_stride, phys_delta,
                                            memslot_id);
        uint8_t* line1 = red_get_image_line(worker, &chunk, &offset, image_stride, phys_delta,
                                            memslot_id);

        if (!line0 || !line1) {
            return FALSE;
        }

        line0 += src->left * PIXEL_SIZE;
        line1 += src->left * PIXEL_SIZE;

        FUNC_NAME(red_rgb_to_yuv420_line)(line0, line1, image_width, y, u, v, y_stride);

        y += 2 * y_stride;
        u += frame->linesize[1];
        v += frame->linesize[2];
    }

    if ((image_hight & 1)) {
        uint8_t* line = red_get_image_line(worker, &chunk, &offset, image_stride, phys_delta,
                                           memslot_id);
        if (!line) {
            return FALSE;
        }
        line += src->left * PIXEL_SIZE;
        FUNC_NAME(red_rgb_to_yuv420_line)(line, line, image_width, y, u, v, y_stride);
    }
    return TRUE;
}


#undef R
#undef G
#undef B
#undef Y
#undef U
#undef V
#undef FUNC_NAME
#undef PIXEL_SIZE
#undef PIX16


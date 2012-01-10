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

#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <spice/macros.h>


#define TAB "    "

#define ERROR(str) printf("%s: error: %s\n", prog_name, str); exit(-1);

static char *prog_name = NULL;

static size_t read_input(const char *file_name, uint8_t** out_buf)
{
    int fd = open(file_name, O_RDONLY);
    if (fd == -1) {
        ERROR("open source file failed");
        return 0;
    }
    struct stat file_stat;
    if (fstat(fd, &file_stat) == -1) {
        ERROR("fstat on source file failed");
        return 0;
    }

    uint8_t *buf = malloc(file_stat.st_size);

    if (!buf) {
        close(fd);
        ERROR("alloc mem failed");
        return 0;
    }

    size_t to_read = file_stat.st_size;
    uint8_t *buf_pos = buf;
    while (to_read) {
        int n = read(fd, buf_pos, to_read);
        if (n <= 0) {
            ERROR("read from source file failed");
            close(fd);
            free(buf);
            return 0;
        }
        to_read -= n;
        buf_pos += n;
    }
    close(fd);
    *out_buf = buf;
    return file_stat.st_size;
}

typedef struct __attribute__ ((__packed__)) BitmpaHeader {
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint16_t plans;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t num_colors;
    uint32_t important_colors;
} BitmpaHeader;

typedef struct __attribute__ ((__packed__)) BMPFileHeader {
    uint16_t magic;
    uint32_t file_size;
    uint32_t reserved;
    uint32_t data_offset;
    BitmpaHeader header;
} BMPFileHeader;

#define BI_RGB 0

typedef struct Pixmap {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bpp;
    uint8_t* data;
} Pixmap;

static Pixmap *init_bitmap(size_t input_size, uint8_t *buf)
{
    BMPFileHeader *file_header;
    uint8_t *pixels;
    Pixmap *pixmap;
    uint32_t stride;


    if (input_size < sizeof(BMPFileHeader)) {
        ERROR("invalid source file");
        return NULL;
    }

    file_header = (BMPFileHeader *)buf;

    if (file_header->magic != 0x4d42) {
        ERROR("bad bitmap magic");
        return NULL;
    }

    if (file_header->file_size != input_size) {
        ERROR("invalid source file");
        return NULL;
    }

    if (file_header->header.header_size != 40 || file_header->header.plans != 1 ||
                                                        file_header->header.compression != BI_RGB ||
                                                        !file_header->header.width ||
                                                        !file_header->header.height) {
        ERROR("invalid bitmap header");
        return NULL;
    }

    if (file_header->header.bpp == 32) {
        stride = file_header->header.width * sizeof(uint32_t);
    } else if (file_header->header.bpp == 24) {
        stride = SPICE_ALIGN(file_header->header.width * 3, 4);
    } else {
        ERROR("unsupported bpp");
        return NULL;
    }

    if (file_header->header.height * stride > file_header->header.image_size) {
        ERROR("image size is to small");
        return NULL;
    }
    pixels = buf + file_header->data_offset;
    if (pixels < (uint8_t *)(file_header + 1) ||
                                       pixels + file_header->header.image_size > buf + input_size) {
        ERROR("bad data offset");
        return NULL;
    }

    if (!(pixmap = (Pixmap *)malloc(sizeof(*pixmap)))) {
        ERROR("alloc mem failed");
        return NULL;
    }

    pixmap->width = file_header->header.width;
    pixmap->height = file_header->header.height;
    pixmap->stride = stride;
    pixmap->bpp = file_header->header.bpp;
    pixmap->data = pixels;
    return pixmap;
}

static inline void put_char(FILE* f, uint8_t val)
{
    fprintf(f, "0x%.2x,", val);
}

static void do_dump_with_alpha_conversion(const Pixmap *pixmap, FILE *f)
{
    uint8_t *line = (uint8_t *)(pixmap->data + ((pixmap->height - 1) * pixmap->stride));
    int line_size = 0;
    int i, j;

    for (i = 0; i < pixmap->height; i++) {
        uint8_t *now = line;
        for (j = 0; j < pixmap->width; j++, now += 4) {
            if ((line_size++ % 4) == 0) {
                fprintf(f, "\n" TAB);
            }
            double alpha = (double)now[3] / 0xff;
            put_char(f, alpha * now[0]);
            put_char(f, alpha * now[1]);
            put_char(f, alpha * now[2]);
            put_char(f, now[3]);
        }
        line -= pixmap->stride;
    }
}

static void do_dump_32bpp(const Pixmap *pixmap, FILE *f)
{
    uint8_t *line = (uint8_t *)(pixmap->data + ((pixmap->height - 1) * pixmap->stride));
    int line_size = 0;
    int i, j;

    for (i = 0; i < pixmap->height; i++) {
        uint8_t *now = line;
        for (j = 0; j < pixmap->width; j++, now += 4) {
            if ((line_size++ % 4) == 0) {
                fprintf(f, "\n" TAB);
            }
            put_char(f, now[0]);
            put_char(f, now[1]);
            put_char(f, now[2]);
            put_char(f, now[3]);
        }
        line -= pixmap->stride;
    }
}

static void do_dump_24bpp(const Pixmap *pixmap, FILE *f)
{
    uint8_t *line = (uint8_t *)(pixmap->data + ((pixmap->height - 1) * pixmap->stride));
    int line_size = 0;
    int i, j;

    for (i = 0; i < pixmap->height; i++) {
        uint8_t *now = line;
        for (j = 0; j < pixmap->width; j++, now += 3) {
            if ((line_size++ % 4) == 0) {
                fprintf(f, "\n" TAB);
            }
            put_char(f, now[0]);
            put_char(f, now[1]);
            put_char(f, now[2]);
            put_char(f, 0);
        }
        line -= pixmap->stride;
    }
}

static int pixmap_to_c_struct(const Pixmap *pixmap, const char *dest_file, const char *image_name,
                              int alpha_convertion)
{
    int fd;
    FILE *f;

    if ((fd = open(dest_file, O_WRONLY | O_CREAT | O_TRUNC, 0644)) == -1) {
        ERROR("open dest file failed");
        return -1;
    }

    if (!(f = fdopen(fd, "w"))) {
        ERROR("fdopen failed");
        close(fd);
        return -1;
    }

    uint32_t data_size = pixmap->width * sizeof(uint32_t) * pixmap->height;
    fprintf(f, "static const struct {\n"
               TAB "uint32_t width;\n"
               TAB "uint32_t height;\n"
               TAB "uint8_t pixel_data[%u];\n"
               "} %s = { %u, %u, {",
            data_size, image_name, pixmap->width, pixmap->height);

    if (alpha_convertion) {
        if (pixmap->bpp != 32) {
            ERROR("32 bpp is requred for alpha option")
        }
        do_dump_with_alpha_conversion(pixmap, f);
    } else if (pixmap->bpp == 32) {
        do_dump_32bpp(pixmap, f);
    } else {
        do_dump_24bpp(pixmap, f);
    }

    fseek(f, -1, SEEK_CUR);
    fprintf(f, "}\n};\n");
    fclose(f);
    close(fd);
    return 0;
}

enum {
    ALPHA_OPT = 'a',
    HELP_OPT = 'h',
    NAME_OPT = 'n',
};

static void usage()
{
    printf("usage: %s [--alpha] [--name <struct name>] SOURCE [DEST]\n", prog_name);
    printf("       %s --help\n", prog_name);
}

const struct option longopts[] = {
    {"help", no_argument, NULL, HELP_OPT},
    {"alpha", no_argument, NULL, ALPHA_OPT},
    {"name", required_argument, NULL, NAME_OPT},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv)
{
    size_t input_size;
    uint8_t* buf;
    Pixmap *pixmap;
    int opt;
    int alpha_convertion = FALSE;
    char *struct_name = NULL;
    char *src = NULL;
    char *dest = NULL;


    if (!(prog_name = strrchr(argv[0], '/'))) {
        prog_name = argv[0];
    } else {
        prog_name++;
    }


    while ((opt = getopt_long(argc, argv, "ah", longopts, NULL)) != -1) {
        switch (opt) {
        case 0:
        case '?':
            usage();
            exit(-1);
        case ALPHA_OPT:
            alpha_convertion = TRUE;
            break;
        case HELP_OPT:
            usage();
            exit(0);
        case NAME_OPT:
            struct_name = optarg;
            break;
        }
    }

    int more = argc - optind;
    switch (more) {
    case 1: {
        char *slash;
        char *dot;

        dest = malloc(strlen(argv[optind]) + 3);
        strcpy(dest, argv[optind]);
        dot = strrchr(dest, '.');
        slash = strrchr(dest, '/');
        if (!dot || (slash && slash > dot)) {
            strcat(dest, ".c");
        } else {
            strcpy(dot, ".c");
        }
        break;
    }
    case 2:
        dest = argv[optind + 1];
        //todo: if dir strcat src name
        break;
    default:
        usage();
        exit(-1);
    }

    src = argv[optind];

    if (!struct_name) {
        char *struct_name_src;
        char *dot;

        struct_name_src = strrchr(dest, '/');
        if (!struct_name_src) {
            struct_name_src = dest;
        } else {
            ++struct_name_src;
        }
        struct_name = malloc(strlen(struct_name_src) + 1);
        strcpy(struct_name, struct_name_src);
        if ((dot = strchr(struct_name, '.'))) {
            *dot = 0;
        }
    }

    if (!(input_size = read_input(src, &buf))) {
        return -1;
    }

    if (!(pixmap = init_bitmap(input_size, buf))) {
        return -1;
    }
    return pixmap_to_c_struct(pixmap, dest, struct_name, alpha_convertion);
}

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


typedef struct __attribute__ ((__packed__)) ICOHeader {
    uint8_t width;
    uint8_t height;
    uint8_t colors_count;
    uint8_t reserved;
    uint16_t plans;
    uint16_t bpp;
    uint32_t bitmap_size;
    uint32_t bitmap_offset;
} ICOHeader;

typedef struct __attribute__ ((__packed__)) ICOFileHeader {
    uint16_t reserved;
    uint16_t type;
    uint16_t image_count;
    ICOHeader directory[0];
} ICOFileHeader;


typedef struct Icon {
    int width;
    int height;
    uint8_t* pixmap;
    uint8_t* mask;
} Icon;


static Icon *init_icon(uint8_t *buf, size_t buf_size)
{
    ICOFileHeader *file_header;
    int i;

    uint8_t *buf_end = buf + buf_size;

    if (buf_size < sizeof(ICOFileHeader)) {
        ERROR("invalid source file");
        return NULL;
    }

    file_header = (ICOFileHeader *)buf;

    if (file_header->reserved != 0 || file_header->type != 1) {
        ERROR("invalid icon");
        return NULL;
    }

    if (sizeof(ICOFileHeader) + file_header->image_count * sizeof(ICOHeader) > buf_size) {
        ERROR("invalid source file");
    }

    for (i = 0; i < file_header->image_count; i++) {
        int j;

        ICOHeader *ico = &file_header->directory[i];
        printf("%ux%ux%u size %u\n", (unsigned)ico->width, (unsigned)ico->height,
               (unsigned)ico->bpp, ico->bitmap_size);

        if (ico->bitmap_offset + ico->bitmap_size > buf_size) {
            ERROR("invalid source file");
        }
        BitmpaHeader *bitmap = (BitmpaHeader *)(buf + ico->bitmap_offset);
        if (bitmap->header_size != 40) {
            ERROR("invalid bitmap header");
        }

        if (ico->plans == 0) { // 0 and 1 are equivalent
            ico->plans = 1;
        }

        if (bitmap->width != ico->width || bitmap->height != ico->height * 2 ||
            bitmap->plans != ico->plans || bitmap->bpp != ico->bpp) {
            ERROR("invalid bitmap header");
        }

        if (!bitmap->image_size) {
            bitmap->image_size = SPICE_ALIGN(bitmap->bpp * bitmap->width, 32) / 8 * bitmap->height;
        }

        if (bitmap->compression || bitmap->horizontal_resolution || bitmap->vertical_resolution ||
                                                   bitmap->num_colors || bitmap->important_colors) {
            ERROR("invalid bitmap header");
        }

        if (ico->width != 32 || ico->height != 32 || ico->bpp != 32) {
            continue;
        }

        int pixmap_size = bitmap->width * sizeof(uint32_t) * ico->height;
        int mask_size = SPICE_ALIGN(bitmap->width, 8) / 8 * ico->height;
        Icon* icon = malloc(sizeof(*icon) + pixmap_size + mask_size);
        icon->width = ico->width;
        icon->height = ico->height;
        icon->pixmap = (uint8_t *)(icon + 1);
        icon->mask = icon->pixmap + pixmap_size;

        if ((uint8_t *)(bitmap + 1) + pixmap_size + mask_size > buf_end) {
            ERROR("invalid source file");
        }
        memcpy(icon->pixmap, bitmap + 1, pixmap_size);
        memcpy(icon->mask, (uint8_t *)(bitmap + 1) + pixmap_size, mask_size);
        for (j = 0; j < mask_size; j++) {
            icon->mask[j] = ~icon->mask[j];
        }
        return icon;
    }
    printf("%s: missing 32x32x32\n", prog_name);
    return NULL;
}

static inline void put_char(FILE* f, uint8_t val)
{
    fprintf(f, "0x%.2x,", val);
}

static int icon_to_c_struct(const Icon *icon, const char *dest_file, const char *image_name)
{
    int i, j;
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
    uint32_t pixmap_stride = icon->width * sizeof(uint32_t);
    uint32_t pixmap_size = pixmap_stride * icon->height;
    uint32_t mask_stride = SPICE_ALIGN(icon->width, 8) / 8;
    uint32_t mask_size = mask_stride * icon->height;
    fprintf(f, "static const struct {\n"
               TAB "uint32_t width;\n"
               TAB "uint32_t height;\n"
               TAB "uint8_t pixmap[%u];\n"
               TAB "uint8_t mask[%u];\n"
               "} %s = { %u, %u, {",
            pixmap_size, mask_size, image_name, icon->width, icon->height);

    uint8_t *line = (uint8_t *)(icon->pixmap + ((icon->height - 1) * pixmap_stride));
    int line_size = 0;

    for (i = 0; i < icon->height; i++) {
        uint8_t *now = line;
        for (j = 0; j < icon->width; j++, now += 4) {
            if ((line_size++ % 4) == 0) {
                fprintf(f, "\n" TAB);
            }
            put_char(f, now[0]);
            put_char(f, now[1]);
            put_char(f, now[2]);
            put_char(f, now[3]);
        }
        line -= pixmap_stride;
    }


    fseek(f, -1, SEEK_CUR);
    fprintf(f, "},\n\n" TAB TAB "{");

    line = (uint8_t *)(icon->mask + ((icon->height - 1) * mask_stride));
    line_size = 0;
    for (i = 0; i < icon->height; i++) {
        for (j = 0; j < mask_stride; j++) {
            if (line_size && (line_size % 12) == 0) {
                fprintf(f, "\n" TAB);
            }
            line_size++;
            put_char(f, line[j]);
        }
        line -= mask_stride;
    }

    fseek(f, -1, SEEK_CUR);
    fprintf(f, "}\n};\n");
    fclose(f);
    close(fd);
    return 0;
}

enum {
    HELP_OPT = 'h',
    NAME_OPT = 'n',
};

static void usage()
{
    printf("usage: %s [--name <struct name>] SOURCE [DEST]\n", prog_name);
    printf("       %s --help\n", prog_name);
}

const struct option longopts[] = {
    {"help", no_argument, NULL, HELP_OPT},
    {"name", required_argument, NULL, NAME_OPT},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv)
{
    size_t input_size;
    uint8_t* buf;
    Icon *icon;
    int opt;
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

    if (!(icon = init_icon(buf, input_size))) {
        return -1;
    }

    return icon_to_c_struct(icon, dest, struct_name);
}


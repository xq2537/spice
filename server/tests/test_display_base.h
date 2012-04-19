#ifndef __TEST_DISPLAY_BASE_H__
#define __TEST_DISPLAY_BASE_H__

#include <spice.h>
#include "basic_event_loop.h"

#define COUNT(x) ((sizeof(x)/sizeof(x[0])))

/*
 * simple queue for commands.
 * each command can have up to two parameters (grow as needed)
 *
 * TODO: switch to gtk main loop. Then add gobject-introspection. then
 * write tests in python/guile/whatever.
 */
typedef enum {
    PATH_PROGRESS,
    SIMPLE_CREATE_SURFACE,
    SIMPLE_DRAW,
    SIMPLE_COPY_BITS,
    SIMPLE_DESTROY_SURFACE,
    SIMPLE_UPDATE,
    DESTROY_PRIMARY,
    CREATE_PRIMARY,
} CommandType;

typedef struct CommandCreatePrimary {
    uint32_t width;
    uint32_t height;
} CommandCreatePrimary;

typedef struct CommandDrawBitmap {
    QXLRect bbox;
    uint8_t *bitmap;
    uint32_t surface_id;
} CommandDrawBitmap;

typedef struct CommandDrawSolid {
    QXLRect bbox;
    uint32_t color;
    uint32_t surface_id;
} CommandDrawSolid;

typedef struct Command Command;

struct Command {
    CommandType command;
    void (*cb)(Command *command);
    void *cb_opaque;
    union {
        CommandCreatePrimary create_primary;
        CommandDrawBitmap bitmap;
        CommandDrawSolid solid;
    };
};

void test_set_simple_command_list(int *command, int num_commands);
void test_set_command_list(Command *command, int num_commands);
void test_add_display_interface(SpiceServer *server);
SpiceServer* test_init(SpiceCoreInterface* core);

uint32_t test_get_width(void);
uint32_t test_get_height(void);

void spice_test_config_parse_args(int argc, char **argv);

#endif /* __TEST_DISPLAY_BASE_H__ */

#ifndef __TEST_DISPLAY_BASE_H__
#define __TEST_DISPLAY_BASE_H__

#include <spice.h>
#include "basic_event_loop.h"

#define COUNT(x) ((sizeof(x)/sizeof(x[0])))

void test_set_simple_command_list(int* commands, int num_commands);
void test_add_display_interface(SpiceServer *server);
SpiceServer* test_init(SpiceCoreInterface* core);

// simple queue for commands
enum {
    PATH_PROGRESS,
    SIMPLE_CREATE_SURFACE,
    SIMPLE_DRAW,
    SIMPLE_COPY_BITS,
    SIMPLE_DESTROY_SURFACE,
    SIMPLE_UPDATE,
};

#endif // __TEST_DISPLAY_BASE_H__

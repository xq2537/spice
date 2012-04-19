/* Do repeated updates to the same rectangle to trigger stream creation.
 *
 * TODO: check that stream actually starts programatically (maybe stap?)
 * TODO: stop updating same rect, check (prog) that stream stops
 */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "test_display_base.h"

static int sized;

void create_update(Command *command)
{
    static int count = 0;
    CommandDrawSolid *cmd = &command->solid;
    cmd->surface_id = 0;

    cmd->bbox.left = 0;
    cmd->bbox.right = test_get_width();
    cmd->bbox.top = 0;
    cmd->color = 0xffff00 + ((count * 10) % 256);
    assert(test_get_height() > 50);
    cmd->bbox.bottom = test_get_height() - 50;
    if (count < 20) {
    } else if (sized && count % 5 == 0) {
        cmd->bbox.bottom = test_get_height();
        cmd->color = 0xff;
    }
    count++;
    printf("%d %d\n", count, cmd->bbox.bottom);
}

static Command commands[] = {
    {SIMPLE_DRAW_SOLID, create_update},
};

SpiceCoreInterface *core;
SpiceServer *server;

int main(int argc, char **argv)
{
    int i;
    spice_test_config_parse_args(argc, argv);
    sized = 0;
    for (i = 1 ; i < argc; ++i) {
        if (strcmp(argv[i], "sized") == 0) {
            sized = 1;
        }
    }
    core = basic_event_loop_init();
    server = test_init(core);
    spice_server_set_streaming_video(server, SPICE_STREAM_VIDEO_ALL);
    test_add_display_interface(server);
    test_set_command_list(commands, COUNT(commands));
    basic_event_loop_mainloop();
    return 0;
}

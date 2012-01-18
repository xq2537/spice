/* Do repeated updates to the same rectangle to trigger stream creation.
 *
 * TODO: check that stream actually starts programatically (maybe stap?)
 * TODO: stop updating same rect, check (prog) that stream stops
 */

#include <config.h>
#include "test_display_base.h"

int simple_commands[] = {
    SIMPLE_DRAW,
    SIMPLE_UPDATE,
};

SpiceCoreInterface *core;
SpiceServer *server;

int main(void)
{
    core = basic_event_loop_init();
    server = test_init(core);
    spice_server_set_streaming_video(server, SPICE_STREAM_VIDEO_ALL);
    test_add_display_interface(server);
    test_set_simple_command_list(simple_commands, COUNT(simple_commands));
    basic_event_loop_mainloop();
    return 0;
}

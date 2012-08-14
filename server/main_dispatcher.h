#ifndef MAIN_DISPATCHER_H
#define MAIN_DISPATCHER_H

#include <spice.h>

void main_dispatcher_channel_event(int event, SpiceChannelEventInfo *info);
void main_dispatcher_seamless_migrate_dst_complete(RedClient *client);
void main_dispatcher_init(SpiceCoreInterface *core);

#endif //MAIN_DISPATCHER_H

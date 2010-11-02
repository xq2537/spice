#include <stdlib.h>
#include <stdio.h>

#include "basic_event_loop.h"

#define NOT_IMPLEMENTED printf("%s not implemented\n", __func__);

static SpiceCoreInterface core;

static SpiceTimer* timer_add(SpiceTimerFunc func, void *opaque)
{
    NOT_IMPLEMENTED
}

static void timer_start(SpiceTimer *timer, uint32_t ms)
{
    NOT_IMPLEMENTED
}

static void timer_cancel(SpiceTimer *timer)
{
    NOT_IMPLEMENTED
}

static void timer_remove(SpiceTimer *timer)
{
    NOT_IMPLEMENTED
}

struct SpiceWatch {
    int id;
};

typedef struct Watch {
    SpiceWatch id;
    int fd;
    int event_mask;
    SpiceWatchFunc func;
    void *opaque;
} Watch;

Watch watches[100];
int watch_count = 0;
int next_id = 1;

static SpiceWatch *watch_add(int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    watches[watch_count].fd = fd;
    watches[watch_count].event_mask = event_mask;
    watches[watch_count].func = func;
    watches[watch_count].opaque = opaque;
    watches[watch_count].id.id = next_id++;
    return &watches[watch_count++].id;
}

static void watch_update_mask(SpiceWatch *watch, int event_mask)
{
    int i;
    Watch *my_watch;

    for (i = 0 ; i < watch_count; ++i) {
        if (watches[i].id.id == watch->id) {
            my_watch = &watches[i];
            if (my_watch->event_mask != event_mask) {
                my_watch->event_mask = event_mask;
            }
            return;
        }
    }
}

static void watch_remove(SpiceWatch *watch)
{
    int i;

    for (i = 0 ; i < watch_count ; ++i) {
        if (watches[i].id.id == watch->id) {
            watches[i] = watches[--watch_count];
            return;
        }
    }
}

static void channel_event(int event, SpiceChannelEventInfo *info)
{
    NOT_IMPLEMENTED
}

void basic_event_loop_mainloop(void)
{
    fd_set rfds, wfds;
    int max_fd = -1;
    int i;
    int retval;

    while (1) {
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        for (i = 0 ; i < watch_count; ++i) {
            Watch* watch = &watches[i];
            if (watch->event_mask & SPICE_WATCH_EVENT_READ) {
                FD_SET(watch->fd, &rfds);
                max_fd = watch->fd > max_fd ? watch->fd : max_fd;
            }
            if (watch->event_mask & SPICE_WATCH_EVENT_WRITE) {
                FD_SET(watch->fd, &wfds);
                max_fd = watch->fd > max_fd ? watch->fd : max_fd;
            }
        }
        retval = select(max_fd + 1, &rfds, &wfds, NULL, NULL);
        if (retval == -1) {
            printf("error in select - exiting\n");
            exit(-1);
        }
        if (retval) {
            for (i = 0 ; i < watch_count; ++i) {
                Watch* watch = &watches[i];
                if ((watch->event_mask & SPICE_WATCH_EVENT_READ) && FD_ISSET(watch->fd, &rfds)) {
                    watch->func(watch->fd, SPICE_WATCH_EVENT_READ, watch->opaque);
                }
                if ((watch->event_mask & SPICE_WATCH_EVENT_WRITE) && FD_ISSET(watch->fd, &wfds)) {
                    watch->func(watch->fd, SPICE_WATCH_EVENT_WRITE, watch->opaque);
                }
            }
        }
    }
}

SpiceCoreInterface *basic_event_loop_init(void)
{
    bzero(&core, sizeof(core));
    core.base.major_version = SPICE_INTERFACE_CORE_MAJOR;
    core.base.minor_version = SPICE_INTERFACE_CORE_MINOR; // anything less then 3 and channel_event isn't called
    core.timer_add = timer_add;
    core.timer_start = timer_start;
    core.timer_cancel = timer_cancel;
    core.timer_remove = timer_remove;
    core.watch_add = watch_add;
    core.watch_update_mask = watch_update_mask;
    core.watch_remove = watch_remove;
    core.channel_event = channel_event;
    return &core;
}


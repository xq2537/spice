#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>

#include "red_common.h"
#include "main_dispatcher.h"

/*
 * Main Dispatcher
 * ===============
 *
 * Communication channel between any non main thread and the main thread.
 *
 * The main thread is that from which spice_server_init is called.
 *
 * Messages are single sized, sent from the non-main thread to the main-thread.
 * No acknowledge is sent back. This prevents a possible deadlock with the main
 * thread already waiting on a response for the existing red_dispatcher used
 * by the worker thread.
 *
 * All events have three functions:
 * main_dispatcher_<event_name> - non static, public function
 * main_dispatcher_self_<event_name> - handler for main thread
 * main_dispatcher_handle_<event_name> - handler for callback from main thread
 *   seperate from self because it may send an ack or do other work in the future.
 */

typedef struct {
    SpiceCoreInterface *core;
    int main_fd;
    int other_fd;
    pthread_t self;
    pthread_mutex_t lock;
} MainDispatcher;

MainDispatcher main_dispatcher;

enum {
    MAIN_DISPATCHER_CHANNEL_EVENT = 0,

    MAIN_DISPATCHER_NUM_MESSAGES
};

typedef struct MainDispatcherMessage {
    uint32_t type;
    union {
        struct {
            int event;
            SpiceChannelEventInfo *info;
        } channel_event;
    } data;
} MainDispatcherMessage;

/* channel_event - calls core->channel_event, must be done in main thread */
static void main_dispatcher_self_handle_channel_event(
                                                int event,
                                                SpiceChannelEventInfo *info)
{
    main_dispatcher.core->channel_event(event, info);
}

static void main_dispatcher_handle_channel_event(MainDispatcherMessage *msg)
{
    main_dispatcher_self_handle_channel_event(msg->data.channel_event.event,
                                              msg->data.channel_event.info);
}

void main_dispatcher_channel_event(int event, SpiceChannelEventInfo *info)
{
    MainDispatcherMessage msg;
    ssize_t written = 0;
    ssize_t ret;

    if (pthread_self() == main_dispatcher.self) {
        main_dispatcher_self_handle_channel_event(event, info);
        return;
    }
    msg.type = MAIN_DISPATCHER_CHANNEL_EVENT;
    msg.data.channel_event.event = event;
    msg.data.channel_event.info = info;
    pthread_mutex_lock(&main_dispatcher.lock);
    while (written < sizeof(msg)) {
        ret = write(main_dispatcher.other_fd, &msg + written,
                    sizeof(msg) - written);
        if (ret == -1) {
            assert(errno == EINTR);
            continue;
        }
        written += ret;
    }
    pthread_mutex_unlock(&main_dispatcher.lock);
}


static void main_dispatcher_handle_read(int fd, int event, void *opaque)
{
    int ret;
    MainDispatcher *md = opaque;
    MainDispatcherMessage msg;
    int read_size = 0;

    while (read_size < sizeof(msg)) {
        /* blocks until sizeof(msg) is read */
        ret = read(md->main_fd, &msg + read_size, sizeof(msg) - read_size);
        if (ret == -1) {
            if (errno != EINTR) {
                red_printf("error reading from main dispatcher: %d\n", errno);
                /* TODO: close channel? */
                return;
            }
            continue;
        }
        read_size += ret;
    }
    switch (msg.type) {
        case MAIN_DISPATCHER_CHANNEL_EVENT:
            main_dispatcher_handle_channel_event(&msg);
            break;
        default:
            red_printf("error: unhandled main dispatcher message type %d\n",
                       msg.type);
    }
}

void main_dispatcher_init(SpiceCoreInterface *core)
{
    int channels[2];

    memset(&main_dispatcher, 0, sizeof(main_dispatcher));
    main_dispatcher.core = core;

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, channels) == -1) {
        red_error("socketpair failed %s", strerror(errno));
        return;
    }
    pthread_mutex_init(&main_dispatcher.lock, NULL);
    main_dispatcher.main_fd = channels[0];
    main_dispatcher.other_fd = channels[1];
    main_dispatcher.self = pthread_self();

    core->watch_add(main_dispatcher.main_fd, SPICE_WATCH_EVENT_READ,
                    main_dispatcher_handle_read, &main_dispatcher);
}

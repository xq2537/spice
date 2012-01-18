#include <config.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>

#include "mem.h"
#include "spice_common.h"
#include "dispatcher.h"

#define DISPATCHER_DEBUG_PRINTF(level, ...) \
    red_printf_debug(level, "DISP", ##__VA_ARGS__)

//#define DEBUG_DISPATCHER

#ifdef DEBUG_DISPATCHER
#include <signal.h>
#endif

#define ACK 0xffffffff

/*
 * read_safe
 * helper. reads until size bytes accumulated in buf, if an error other then
 * EINTR is encountered returns -1, otherwise returns 0.
 * @block if 1 the read will block (the fd is always blocking).
 *        if 0 poll first, return immediately if no bytes available, otherwise
 *         read size in blocking mode.
 */
static int read_safe(int fd, void *buf, size_t size, int block)
{
    int read_size = 0;
    int ret;
    struct pollfd pollfd = {.fd = fd, .events = POLLIN, .revents = 0};

    if (size == 0) {
        return 0;
    }

    if (!block) {
        while ((ret = poll(&pollfd, 1, 0)) == -1) {
            if (errno == EINTR) {
                DISPATCHER_DEBUG_PRINTF(3, "EINTR in poll");
                continue;
            }
            red_error("poll failed");
            return -1;
        }
        if (!(pollfd.revents & POLLIN)) {
            return 0;
        }
    }
    while (read_size < size) {
        ret = read(fd, buf + read_size, size - read_size);
        if (ret == -1) {
            if (errno == EINTR) {
                DISPATCHER_DEBUG_PRINTF(3, "EINTR in read");
                continue;
            }
            return -1;
        }
        if (ret == 0) {
            red_error("broken pipe on read");
            return -1;
        }
        read_size += ret;
    }
    return read_size;
}

/*
 * write_safe
 * @return -1 for error, otherwise number of written bytes. may be zero.
 */
static int write_safe(int fd, void *buf, size_t size)
{
    int written_size = 0;
    int ret;

    while (written_size < size) {
        ret = write(fd, buf + written_size, size - written_size);
        if (ret == -1) {
            if (errno != EINTR) {
                DISPATCHER_DEBUG_PRINTF(3, "EINTR in write\n");
                return -1;
            }
            continue;
        }
        written_size += ret;
    }
    return written_size;
}

static int dispatcher_handle_single_read(Dispatcher *dispatcher)
{
    int ret;
    uint32_t type;
    DispatcherMessage *msg = NULL;
    uint8_t *payload = dispatcher->payload;
    uint32_t ack = ACK;

    if ((ret = read_safe(dispatcher->recv_fd, &type, sizeof(type), 0)) == -1) {
        red_printf("error reading from dispatcher: %d\n", errno);
        return 0;
    }
    if (ret == 0) {
        /* no messsage */
        return 0;
    }
    msg = &dispatcher->messages[type];
    if (read_safe(dispatcher->recv_fd, payload, msg->size, 1) == -1) {
        red_printf("error reading from dispatcher: %d\n", errno);
        /* TODO: close socketpair? */
        return 0;
    }
    if (msg->handler) {
        msg->handler(dispatcher->opaque, (void *)payload);
    } else {
        red_printf("error: no handler for message type %d\n", type);
    }
    if (msg->ack == DISPATCHER_ACK) {
        if (write_safe(dispatcher->recv_fd,
                       &ack, sizeof(ack)) == -1) {
            red_printf("error writing ack for message %d\n", type);
            /* TODO: close socketpair? */
        }
    } else if (msg->ack == DISPATCHER_ASYNC && dispatcher->handle_async_done) {
        dispatcher->handle_async_done(dispatcher->opaque, type,
                                      (void *)payload);
    }
    return 1;
}

/*
 * dispatcher_handle_recv_read
 * doesn't handle being in the middle of a message. all reads are blocking.
 */
void dispatcher_handle_recv_read(Dispatcher *dispatcher)
{
    while (dispatcher_handle_single_read(dispatcher)) {
    }
}

void dispatcher_send_message(Dispatcher *dispatcher, uint32_t message_type,
                             void *payload)
{
    DispatcherMessage *msg;
    uint32_t ack;
    int send_fd = dispatcher->send_fd;

    assert(dispatcher->max_message_type > message_type);
    assert(dispatcher->messages[message_type].handler);
    msg = &dispatcher->messages[message_type];
    pthread_mutex_lock(&dispatcher->lock);
    if (write_safe(send_fd, &message_type, sizeof(message_type)) == -1) {
        red_printf("error: failed to send message type for message %d\n",
                   message_type);
        goto unlock;
    }
    if (write_safe(send_fd, payload, msg->size) == -1) {
        red_printf("error: failed to send message body for message %d\n",
                   message_type);
        goto unlock;
    }
    if (msg->ack == DISPATCHER_ACK) {
        if (read_safe(send_fd, &ack, sizeof(ack), 1) == -1) {
            red_printf("error: failed to read ack");
        } else if (ack != ACK) {
            red_printf("error: got wrong ack value in dispatcher "
                       "for message %d\n", message_type);
            /* TODO handling error? */
        }
    }
unlock:
    pthread_mutex_unlock(&dispatcher->lock);
}

void dispatcher_register_async_done_callback(
                                        Dispatcher *dispatcher,
                                        dispatcher_handle_async_done handler)
{
    assert(dispatcher->handle_async_done == NULL);
    dispatcher->handle_async_done = handler;
}

void dispatcher_register_handler(Dispatcher *dispatcher, uint32_t message_type,
                                 dispatcher_handle_message handler,
                                 size_t size, int ack)
{
    DispatcherMessage *msg;

    assert(message_type < dispatcher->max_message_type);
    assert(dispatcher->messages[message_type].handler == 0);
    msg = &dispatcher->messages[message_type];
    msg->handler = handler;
    msg->size = size;
    msg->ack = ack;
    if (msg->size > dispatcher->payload_size) {
        dispatcher->payload = realloc(dispatcher->payload, msg->size);
        dispatcher->payload_size = msg->size;
    }
}

#ifdef DEBUG_DISPATCHER
static void dummy_handler(int bla)
{
}

static void setup_dummy_signal_handler(void)
{
    static int inited = 0;
    struct sigaction act = {
        .sa_handler = &dummy_handler,
    };
    if (inited) {
        return;
    }
    inited = 1;
    /* handle SIGRTMIN+10 in order to test the loops for EINTR */
    if (sigaction(SIGRTMIN + 10, &act, NULL) == -1) {
        fprintf(stderr,
            "failed to set dummy sigaction for DEBUG_DISPATCHER\n");
        exit(1);
    }
}
#endif

void dispatcher_init(Dispatcher *dispatcher, size_t max_message_type,
                     void *opaque)
{
    int channels[2];

#ifdef DEBUG_DISPATCHER
    setup_dummy_signal_handler();
#endif
    dispatcher->opaque = opaque;
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, channels) == -1) {
        red_error("socketpair failed %s", strerror(errno));
        return;
    }
    pthread_mutex_init(&dispatcher->lock, NULL);
    dispatcher->recv_fd = channels[0];
    dispatcher->send_fd = channels[1];
    dispatcher->self = pthread_self();

    dispatcher->messages = spice_malloc0_n(max_message_type,
                                           sizeof(dispatcher->messages[0]));
    dispatcher->max_message_type = max_message_type;
}

void dispatcher_set_opaque(Dispatcher *dispatcher, void *opaque)
{
    dispatcher->opaque = opaque;
}

int dispatcher_get_recv_fd(Dispatcher *dispatcher)
{
    return dispatcher->recv_fd;
}

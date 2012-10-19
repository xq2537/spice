#include "config.h"

#include <unistd.h>
#include <errno.h>
#include <libwebsockets.h>

#include "spice.h"
#include "reds.h"
#include "reds-private.h"
#include "reds_websockets.h"

static ssize_t stream_write_ws_cb(RedsStream *s, const void *buf, size_t size)
{
    /* TODO: better way to handle the requirement of libwebsocket, perhaps
     * we should make a writev version for libwebsocket. Assuming writev doesn't
     * cause a linearlizing copy itself. */
    ssize_t ret;
    unsigned char *padded_buf = spice_malloc(size + LWS_SEND_BUFFER_PRE_PADDING +
                                          LWS_SEND_BUFFER_POST_PADDING);
    spice_assert(s && s->ws);
    memcpy(padded_buf + LWS_SEND_BUFFER_PRE_PADDING, buf, size);
    ret = libwebsocket_write(s->ws->wsi, &padded_buf[LWS_SEND_BUFFER_PRE_PADDING], size,
                             LWS_WRITE_BINARY);
    free(padded_buf);
    return ret == 0 ? size : -1; /* XXX exact bytes required? if not this is
                                    good enough, else need to change
                                    libwebsocket */
}

static void reds_websocket_append_data(RedsWebSocket *ws, unsigned char *buf,
                                       size_t size)
{
    if (!ws->data) {
        ws->data = spice_malloc(size);
        ws->data_len = size;
        ws->data_avail = 0;
    }
    if (ws->data_len < size + ws->data_avail) {
        ws->data_len = size + ws->data_avail;
        ws->data = spice_realloc(ws->data, ws->data_len);
    }
    memcpy(ws->data + ws->data_avail, buf, size);
    ws->data_avail += size;
}

static ssize_t reds_websocket_read_data(RedsWebSocket *ws, unsigned char *buf,
                                        size_t size)
{
    ssize_t ret;

    ret = ws->data_avail > size ? size : ws->data_avail;
    if (ret > 0) {
        memcpy(buf, ws->data, ret);
    }
    if (ret > 0 && ret < ws->data_avail) {
        memmove(ws->data, ws->data + ret, ws->data_avail - ret);
    }
    ws->data_avail -= ret;
    if (ws->data_avail == 0 && ret == size) {
        free(ws->data);
        ws->data = NULL;
        ws->data_len = ws->data_avail = 0;
    }
    return ret;
}

static int reds_libwebsocket_service_fd(RedsState *s, struct pollfd *pfd)
{
    int ret;
    if (s->ws_in_service_fd) {
        return 0;
    }
    s->ws_in_service_fd = 1;
    ret = libwebsocket_service_fd(s->ws_context, pfd);
    s->ws_in_service_fd = 0;
    if (ret != 0) {
        if (errno == EAGAIN) {
            spice_debug("libwebsocket_servide_fd EAGAIN, pfd->revents = %d",
                        pfd->revents);
            return 0;
        }
        /* since read is the last systemcall, errno should be set correctly */
        spice_debug("libwebsocket_service_fd errored; (%d) %s",
                    errno, sys_errlist[errno]);
        return -1;
    }
    return 0;
}

static ssize_t stream_read_ws_cb(RedsStream *s, void *buf, size_t size)
{
    RedsWebSocket *ws;
    struct pollfd pfd;
    RedsState *reds_state;

    /* TODO: perhaps change libwebsocket to allow a socket like read. Then
     * we can avoid the whole RedsWebSocket->data{,_len,_avail}. */
    spice_assert(s && s->ws);
    ws = s->ws;
    reds_state = libwebsocket_context_user(ws->context);
    if (size == 0) {
        return 0;
    }
    spice_debug("%p %d / %d", ws->data, ws->data_avail, ws->data_len);
    if (ws->data_avail < size && !reds_state->ws_in_service_fd) {
        pfd.fd = ws->fd;
        pfd.events = ws->events;
        pfd.revents = POLLIN;
        if (reds_libwebsocket_service_fd(reds_state, &pfd)) {
            return -1;
        }
    }
    if (ws->data_avail == 0) {
        errno = EAGAIN; /* force a reset of the watch on the fd, so that
                           libwebsocket_service_fd has a chance to run */
        return -1;
    }
    return reds_websocket_read_data(ws, buf, size);
}

static RedsWebSocket *reds_ws_from_fd(RedsState *s, int fd)
{
    int i;

    for (i = 0 ; i < s->ws_count ; ++i) {
        if (s->ws[i].fd == fd) {
            return &s->ws[i];
        }
    }
    spice_error("%s: no match for %d (%d ws sockets)\n", __func__,
                fd, s->ws_count);
    return NULL;
}

static int callback_http(struct libwebsocket_context *context,
                         struct libwebsocket *wsi,
                         void *user, void *in, size_t len)
{
    const char *message = "TODO: serve spice-html5";
    char buf[512];
    int n;

    n = snprintf(buf, sizeof(buf),
                "HTTP/1.0 200 OK\x0d\x0a"
                "Server: spice\x0d\x0a"
                "Content-Type: text/html\x0d\x0a"
                "Content-Length: %zu\x0d\x0a"
                "\x0d\x0a"
                "%s", strlen(message), message);
    libwebsocket_write(wsi, (unsigned char *)buf, n, LWS_WRITE_HTTP);
    return 0;
}

static int spice_server_add_ws_client(SpiceServer *s, int socket, int skip_auth,
                                      RedsWebSocket *ws)
{
    RedLinkInfo *link;
    RedsStream *stream;

    link = spice_server_add_client_create_link(s, socket, skip_auth);
    if (!link) {
        return -1;
    }
    stream = link->stream;
    stream->read = stream_read_ws_cb;
    stream->write = stream_write_ws_cb;
    stream->writev = NULL; /* falls back to write iteration */
    stream->ws = ws;
    reds_handle_new_link(link);
    return 0;
}

static void watch_ws(int fd, int event, void *data)
{
    struct libwebsocket_context *context = data;
    RedsState *s = libwebsocket_context_user(context);
    struct pollfd pfd = {
        .fd = fd,
        .events = reds_ws_from_fd(s, fd)->events,
        .revents = (event & SPICE_WATCH_EVENT_READ ? POLLIN : 0) |
                   (event & SPICE_WATCH_EVENT_WRITE ? POLLOUT : 0)
    };

    reds_libwebsocket_service_fd(s, &pfd);
}

static int callback_ws(struct libwebsocket_context *context,
                       struct libwebsocket *wsi,
                       enum libwebsocket_callback_reasons reason, void *user,
                       void *in, size_t len)
{
    int fd;
    RedsState *s = libwebsocket_context_user(context);
    int n;
    RedsWebSocket *ws;
    int events;

    spice_debug("%s: reason %d user %lu len %zd \n", __func__, reason,
                (unsigned long)user, len);
    switch (reason) {
    case LWS_CALLBACK_HTTP:
        return callback_http(context, wsi, user, in, len);

    case LWS_CALLBACK_ADD_POLL_FD:
        if (s->ws_count >= REDS_MAX_WEBSOCKETS) {
            spice_warning("exceeded websockets maximum watches");
            return 1; /* close connection */
        }
        spice_debug("adding ws for fd %d", (int)(long)user);
        events = (int)(long)len;
        ws = &s->ws[s->ws_count];
        ws->watch = core->watch_add((int)(long)user,
                                (events & POLLIN ? SPICE_WATCH_EVENT_READ: 0) |
                                (events & POLLOUT ? SPICE_WATCH_EVENT_WRITE : 0),
                                watch_ws, (void *)context);
        ws->fd = (int)(long)user;
        ws->events = events;
        s->ws_count++;
        break;

    case LWS_CALLBACK_DEL_POLL_FD:
        spice_debug("removing ws for fd %d", (int)(long)user);
        for (n = 0; n < s->ws_count; n++) {
            if (s->ws[n].fd == (int)(long)user) {
                s->ws[n] = s->ws[s->ws_count - 1];
            }
            s->ws_count--;
        }
        break;

    case LWS_CALLBACK_SET_MODE_POLL_FD:
        reds_ws_from_fd(s, (int)(long)user)->events |= (int)(long)len;
        break;

    case LWS_CALLBACK_CLEAR_MODE_POLL_FD:
        reds_ws_from_fd(s, (int)(long)user)->events &= (int)(long)len;
        break;

    case LWS_CALLBACK_ESTABLISHED:
        fd = libwebsocket_get_socket_fd(wsi);
        ws = reds_ws_from_fd(s, fd);
        *(RedsWebSocket **)user = ws;
        ws->wsi = wsi;
        ws->context = context;
        ws->data_avail = 0;
        spice_debug("LWS_CALLBACK_ESTABLISHED\n");
        spice_server_add_ws_client(s, fd, 0, ws);
        break;

    case LWS_CALLBACK_RECEIVE:
        spice_debug("LWS_CALLBACK_CLIENT_RECEIVE\n");
        spice_assert(user != NULL);
        ws = *(RedsWebSocket **)user;
        spice_assert(ws != NULL);
        reds_websocket_append_data(ws, in, len);
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_CLIENT_RECEIVE:
    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
    case LWS_CALLBACK_CLIENT_WRITEABLE:
    case LWS_CALLBACK_SERVER_WRITEABLE:
    case LWS_CALLBACK_BROADCAST:
    case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS:
    case LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION:
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
    case LWS_CALLBACK_CONFIRM_EXTENSION_OKAY:
    case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
        break;
    }
    return 0;
}

static struct libwebsocket_protocols ws_protocols[] = {
	/* first protocol must always be HTTP handler */

	{
		"binary",		/* name  - based on spice-html5 :) */
		callback_ws,	/* callback */
		sizeof(void*),	/* per_session_data_size */
        /* below initializing library used values to avoid warning */
        NULL,
        0,
        0,
        0
	},
	{
		NULL, NULL, 0, NULL, 0, 0, 0		/* End of list */
	}
};

void reds_init_websocket(RedsState *s, const char *addr,
                         int ws_port, int wss_port)
{
    if (ws_port != -1) {
        s->ws_context = libwebsocket_create_context(ws_port,
                strlen(addr) ? addr : NULL,
                ws_protocols, libwebsocket_internal_extensions,
                NULL /*cert_path*/, NULL /*key_path*/, -1, -1, 0 /*opts*/,
                s);
    }
    if (wss_port != -1) {
        spice_error("TODO: secure websocket not supported");
    }
}

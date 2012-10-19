#ifndef REDS_WEBSOCKETS_H
#define REDS_WEBSOCKETS_H

#include "reds-private.h"

void reds_init_websocket(RedsState *s, const char *addr,
                         int ws_port, int wss_port);

#endif

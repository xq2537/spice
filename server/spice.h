/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _H_SPICE
#define _H_SPICE

#include <sys/socket.h>
#include "vd_interface.h"

/* old interface */
void spice_init(CoreInterface *core);

/* new interface */
typedef struct RedsState SpiceServer;
SpiceServer *spice_server_new(void);
int spice_server_init(SpiceServer *s, CoreInterface *core);
void spice_server_destroy(SpiceServer *s);

#define SPICE_ADDR_FLAG_IPV4_ONLY (1 << 0)
#define SPICE_ADDR_FLAG_IPV6_ONLY (1 << 1)

int spice_server_set_port(SpiceServer *s, int port);
void spice_server_set_addr(SpiceServer *s, const char *addr, int flags);
int spice_server_set_noauth(SpiceServer *s);
int spice_server_set_ticket(SpiceServer *s, const char *passwd, int lifetime,
                            int fail_if_connected, int disconnect_if_connected);
int spice_server_set_tls(SpiceServer *s, int port,
                         const char *ca_cert_file, const char *certs_file,
                         const char *private_key_file, const char *key_passwd,
                         const char *dh_key_file, const char *ciphersuite);

int spice_server_add_interface(SpiceServer *s, VDInterface *interface);
int spice_server_remove_interface(SpiceServer *s, VDInterface *interface);
int spice_server_kbd_leds(SpiceServer *s, KeyboardInterface *kbd, int leds);

typedef enum {
    SPICE_IMAGE_COMPRESS_INVALID  = 0,
    SPICE_IMAGE_COMPRESS_OFF      = 1,
    SPICE_IMAGE_COMPRESS_AUTO_GLZ = 2,
    SPICE_IMAGE_COMPRESS_AUTO_LZ  = 3,
    SPICE_IMAGE_COMPRESS_QUIC     = 4,
    SPICE_IMAGE_COMPRESS_GLZ      = 5,
    SPICE_IMAGE_COMPRESS_LZ       = 6,
} spice_image_compression_t;

int spice_server_set_image_compression(SpiceServer *s,
                                       spice_image_compression_t comp);
spice_image_compression_t spice_server_get_image_compression(SpiceServer *s);

#define SPICE_CHANNEL_SECURITY_NONE (1 << 0)
#define SPICE_CHANNEL_SECURITY_SSL (1 << 1)

int spice_server_set_channel_security(SpiceServer *s, const char *channel, int security);

int spice_server_set_mouse_absolute(SpiceServer *s, int absolute);

int spice_server_add_renderer(SpiceServer *s, const char *name);

int spice_server_get_sock_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen);
int spice_server_get_peer_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen);

#endif

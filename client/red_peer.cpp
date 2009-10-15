/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "red.h"
#include "red_peer.h"
#include "utils.h"
#include "debug.h"
#include "platform_utils.h"

static void ssl_error()
{
    ERR_print_errors_fp(stderr);
    THROW_ERR(SPICEC_ERROR_CODE_SSL_ERROR, "SSL Error");
}

RedPeer::RedPeer()
    : _peer (INVALID_SOCKET)
    , _shut (false)
    , _ctx (NULL)
    , _ssl (NULL)
{
}

RedPeer::~RedPeer()
{
    cleanup();
}

void RedPeer::cleanup()
{
    if (_ssl) {
        SSL_free(_ssl);
        _ssl = NULL;
    }

    if (_ctx) {
        SSL_CTX_free(_ctx);
        _ctx = NULL;
    }

    if (_peer != INVALID_SOCKET) {
        closesocket(_peer);
        _peer = INVALID_SOCKET;
    }
}

uint32_t RedPeer::host_by_name(const char* host)
{
    struct addrinfo *result = NULL;
    struct sockaddr_in *addr;
    uint32_t return_value;
    int rc;

    rc = getaddrinfo(host, NULL, NULL, &result);
    if (rc != 0 || result == NULL) {
        THROW_ERR(SPICEC_ERROR_CODE_GETHOSTBYNAME_FAILED, "cannot resolve host address %s", host);
    }

    addr = (sockaddr_in *)result->ai_addr;
    return_value = addr->sin_addr.s_addr;

    freeaddrinfo(result);

    DBG(0, "%s = %u", host, return_value);
    return ntohl(return_value);
}

void RedPeer::connect_unsecure(uint32_t ip, int port)
{
    struct sockaddr_in addr;
    int no_delay;

    ASSERT(_ctx == NULL && _ssl == NULL && _peer == INVALID_SOCKET);
    try {
        addr.sin_port = htons(port);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);

        Lock lock(_lock);
        if ((_peer = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) {
            int err = sock_error();
            THROW_ERR(SPICEC_ERROR_CODE_SOCKET_FAILED, "failed to create socket: %s (%d)",
                      sock_err_message(err), err);
        }

        no_delay = 1;
        if (setsockopt(_peer, IPPROTO_TCP, TCP_NODELAY, (const char*)&no_delay, sizeof(no_delay)) ==
                                                                                     SOCKET_ERROR) {
            LOG_WARN("set TCP_NODELAY failed");
        }

        LOG_INFO("Connecting %s %d", inet_ntoa(addr.sin_addr), port);
        lock.unlock();
        if (::connect(_peer, (struct sockaddr *)&addr, sizeof(sockaddr_in)) == SOCKET_ERROR) {
            int err = sock_error();
            closesocket(_peer);
            THROW_ERR(SPICEC_ERROR_CODE_CONNECT_FAILED, "failed to connect: %s (%d)",
                      sock_err_message(err), err);
        }
        _serial = 0;
    } catch (...) {
        Lock lock(_lock);
        cleanup();
        throw;
    }
}

void RedPeer::connect_unsecure(const char* host, int port)
{
    connect_unsecure(host_by_name(host), port);
}

// todo: use SSL_CTX_set_cipher_list, SSL_CTX_load_verify_location etc.
void RedPeer::connect_secure(const ConnectionOptions& options, uint32_t ip)
{
    connect_unsecure(ip, options.secure_port);
    ASSERT(_ctx == NULL && _ssl == NULL && _peer != INVALID_SOCKET);

    try {
        SSL_METHOD *ssl_method = TLSv1_method();

        _ctx = SSL_CTX_new(ssl_method);
        if (_ctx == NULL) {
            ssl_error();
        }

        _ssl = SSL_new(_ctx);
        if (!_ssl) {
            THROW("create ssl failed");
        }

        BIO* sbio = BIO_new_socket(_peer, BIO_NOCLOSE);
        if (!sbio) {
            THROW("alloc new socket bio failed");
        }

        SSL_set_bio(_ssl, sbio, sbio);

        int return_code = SSL_connect(_ssl);
        if (return_code <= 0) {
            SSL_get_error(_ssl, return_code);
            ssl_error();
        }
    } catch (...) {
        Lock lock(_lock);
        cleanup();
        throw;
    }
}

void RedPeer::connect_secure(const ConnectionOptions& options, const char* host)
{
    connect_secure(options, host_by_name(host));
}

void RedPeer::shutdown()
{
    if (_peer != INVALID_SOCKET) {
        if (_ssl) {
            SSL_shutdown(_ssl);
        }
        ::shutdown(_peer, SHUT_RDWR);
    }
    _shut = true;
}

void RedPeer::disconnect()
{
    Lock lock(_lock);
    shutdown();
}

void RedPeer::close()
{
    Lock lock(_lock);
    if (_peer != INVALID_SOCKET) {
        if (_ctx) {
            SSL_free(_ssl);
            _ssl = NULL;
            SSL_CTX_free(_ctx);
            _ctx = NULL;
        }

        closesocket(_peer);
        _peer = INVALID_SOCKET;
    }
}

void RedPeer::swap(RedPeer* other)
{
    Lock lock(_lock);
    SOCKET temp_peer = _peer;
    SSL_CTX *temp_ctx = _ctx;
    SSL *temp_ssl = _ssl;

    _peer = other->_peer;
    other->_peer = temp_peer;

    if (_ctx) {
        _ctx = other->_ctx;
        _ssl = other->_ssl;

        other->_ctx = temp_ctx;
        other->_ssl = temp_ssl;
    }

    if (_shut) {
        shutdown();
    }
}

uint32_t RedPeer::recive(uint8_t *buf, uint32_t size)
{
    uint8_t *pos = buf;
    while (size) {
        int now;
        if (_ctx == NULL) {
            if ((now = recv(_peer, (char *)pos, size, 0)) <= 0) {
                int err = sock_error();
                if (now == SOCKET_ERROR && err == WOULDBLOCK_ERR) {
                    break;
                }

                if (now == 0 || err == SHUTDOWN_ERR) {
                    throw RedPeer::DisconnectedException();
                }

                if (err == INTERRUPTED_ERR) {
                    continue;
                }
                THROW_ERR(SPICEC_ERROR_CODE_RECV_FAILED, "%s (%d)", sock_err_message(err), err);
            }
            size -= now;
            pos += now;
        } else {
            if ((now = SSL_read(_ssl, pos, size)) <= 0) {
                int ssl_error = SSL_get_error(_ssl, now);

                if (ssl_error == SSL_ERROR_WANT_READ) {
                    break;
                }

                if (ssl_error == SSL_ERROR_SYSCALL) {
                    int err = sock_error();
                    if (now == -1) {
                        if (err == WOULDBLOCK_ERR) {
                            break;
                        }
                        if (err == INTERRUPTED_ERR) {
                            continue;
                        }
                    }
                    if (now == 0 || (now == -1 && err == SHUTDOWN_ERR)) {
                        throw RedPeer::DisconnectedException();
                    }
                    THROW_ERR(SPICEC_ERROR_CODE_SEND_FAILED, "%s (%d)", sock_err_message(err), err);
                } else if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                    throw RedPeer::DisconnectedException();
                }
                THROW_ERR(SPICEC_ERROR_CODE_RECV_FAILED, "ssl error %d", ssl_error);
            }
            size -= now;
            pos += now;
        }
    }
    return pos - buf;
}

RedPeer::CompundInMessage* RedPeer::recive()
{
    RedDataHeader header;
    AutoRef<CompundInMessage> message;

    recive((uint8_t*)&header, sizeof(RedDataHeader));
    message.reset(new CompundInMessage(header.serial, header.type, header.size, header.sub_list));
    recive((*message)->data(), (*message)->compund_size());
    return message.release();
}

uint32_t RedPeer::send(uint8_t *buf, uint32_t size)
{
    uint8_t *pos = buf;
    while (size) {
        int now;

        if (_ctx == NULL) {
            if ((now = ::send(_peer, (char *)pos, size, 0)) == SOCKET_ERROR) {
                int err = sock_error();
                if (err == WOULDBLOCK_ERR) {
                    break;
                }
                if (err == SHUTDOWN_ERR) {
                    throw RedPeer::DisconnectedException();
                }
                if (err == INTERRUPTED_ERR) {
                    continue;
                }
                THROW_ERR(SPICEC_ERROR_CODE_SEND_FAILED, "%s (%d)", sock_err_message(err), err);
            }
            size -= now;
            pos += now;
        } else {
            if ((now = SSL_write(_ssl, pos, size)) <= 0) {
                int ssl_error = SSL_get_error(_ssl, now);

                if (ssl_error == SSL_ERROR_WANT_WRITE) {
                    break;
                }
                if (ssl_error == SSL_ERROR_SYSCALL) {
                    int err = sock_error();
                    if (now == -1) {
                        if (err == WOULDBLOCK_ERR) {
                            break;
                        }
                        if (err == INTERRUPTED_ERR) {
                            continue;
                        }
                    }
                    if (now == 0 || (now == -1 && err == SHUTDOWN_ERR)) {
                        throw RedPeer::DisconnectedException();
                    }
                    THROW_ERR(SPICEC_ERROR_CODE_SEND_FAILED, "%s (%d)", sock_err_message(err), err);
                } else if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                    throw RedPeer::DisconnectedException();
                }
                THROW_ERR(SPICEC_ERROR_CODE_SEND_FAILED, "ssl error %d", ssl_error);
            }
            size -= now;
            pos += now;
        }
    }
    return pos - buf;
}

uint32_t RedPeer::send(RedPeer::OutMessage& message)
{
    message.header().serial = ++_serial;
    return send(message.base(), message.message_size());
}

RedPeer::OutMessage::OutMessage(uint32_t type, uint32_t size)
    : _data (new uint8_t[size + sizeof(RedDataHeader)])
    , _size (size)
{
    header().type = type;
    header().size = size;
}

RedPeer::OutMessage::~OutMessage()
{
    delete[] _data;
}

void RedPeer::OutMessage::resize(uint32_t size)
{
    if (size <= _size) {
        header().size = size;
        return;
    }
    uint32_t type = header().type;
    delete[] _data;
    _data = NULL;
    _size = 0;
    _data = new uint8_t[size + sizeof(RedDataHeader)];
    _size = size;
    header().type = type;
    header().size = size;
}


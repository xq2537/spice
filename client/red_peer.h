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

#ifndef _H_REDPEER
#define _H_REDPEER

#ifdef _WIN32
#include <winsock.h>
#else
typedef int SOCKET;
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "common.h"
#include "red.h"
#include "process_loop.h"
#include "threads.h"

class RedPeer: protected EventSources::Socket {
public:
    RedPeer();
    virtual ~RedPeer();

    class InMessage;
    class CompundInMessage;
    class OutMessage;
    class DisconnectedException {};

    class HostAuthOptions {
    public:

        enum Type {
            HOST_AUTH_OP_PUBKEY = 1,
            HOST_AUTH_OP_NAME = (1 << 1),
            HOST_AUTH_OP_SUBJECT = (1 << 2),
        };

        typedef std::vector<uint8_t> PublicKey;
        typedef std::pair<std::string, std::string> CertFieldValuePair;
        typedef std::list<CertFieldValuePair> CertFieldValueList;

        HostAuthOptions() : type_flags(0) {}

    public:

        int type_flags;

        PublicKey host_pubkey;
        CertFieldValueList host_subject;
        std::string CA_file;
    };

    class ConnectionOptions {
    public:

        enum Type {
            CON_OP_INVALID,
            CON_OP_SECURE,
            CON_OP_UNSECURE,
            CON_OP_BOTH,
        };

        ConnectionOptions(Type in_type, int in_port, int in_sport,
                          const HostAuthOptions& in_host_auth)
            : type (in_type)
            , unsecure_port (in_port)
            , secure_port (in_sport)
            , host_auth (in_host_auth)
        {
        }

        virtual ~ConnectionOptions() {}

        bool allow_secure() const
        {
            return (type == CON_OP_BOTH || type == CON_OP_SECURE) && secure_port != -1;
        }

        bool allow_unsecure() const
        {
            return (type == CON_OP_BOTH || type == CON_OP_UNSECURE) && unsecure_port != -1;
        }

    public:
        Type type;
        int unsecure_port;
        int secure_port;
        HostAuthOptions host_auth; // for secure connection
    };

    void connect_unsecure(const char* host, int port);
    void connect_secure(const ConnectionOptions& options, const char* host);

    void disconnect();
    void swap(RedPeer* other);
    void close();
    void enable() { _shut = false;}

    virtual CompundInMessage* recive();
    uint32_t send(OutMessage& message);

    uint32_t recive(uint8_t* buf, uint32_t size);
    uint32_t send(uint8_t* buf, uint32_t size);

    static uint32_t host_by_name(const char *host);

protected:
    virtual void on_event() {}
    virtual int get_socket() { return _peer;}

    static bool x509_cert_host_name_compare(const char *cert_name, int cert_name_size,
                                            const char *host_name);

    static bool verify_pubkey(X509* cert, const HostAuthOptions::PublicKey& key);
    static bool verify_host_name(X509* cert, const char* host_name);
    static bool verify_subject(X509* cert, const HostAuthOptions::CertFieldValueList& subject);

    static int ssl_verify_callback(int preverify_ok, X509_STORE_CTX *ctx);

private:
    void shutdown();
    void cleanup();

private:
    SOCKET _peer;
    Mutex _lock;
    bool _shut;
    uint64_t _serial;

    SSL_CTX *_ctx;
    SSL *_ssl;
};

class RedPeer::InMessage {
public:
    InMessage(uint16_t type, uint32_t size, uint8_t* data)
        : _type (type)
        , _size (size)
        , _data (data)
    {
    }

    virtual ~InMessage() {}

    uint16_t type() { return _type;}
    uint8_t* data() { return _data;}
    virtual uint32_t size() { return _size;}

protected:
    uint16_t _type;
    uint32_t _size;
    uint8_t* _data;
};

class RedPeer::CompundInMessage: public RedPeer::InMessage {
public:
    CompundInMessage(uint64_t _serial, uint16_t type, uint32_t size, uint32_t sub_list)
        : InMessage(type, size, new uint8_t[size])
        , _serial (_serial)
        , _sub_list (sub_list)
    {
    }

    virtual ~CompundInMessage() { delete[] _data;}

    uint64_t serial() { return _serial;}
    uint32_t sub_list() { return _sub_list;}

    virtual uint32_t size() { return _sub_list ? _sub_list : _size;}
    uint32_t compund_size() {return _size;}

private:
    uint64_t _serial;
    uint32_t _sub_list;
};

class RedPeer::OutMessage {
public:
    OutMessage(uint32_t type, uint32_t size);
    virtual ~OutMessage();

    RedDataHeader& header() { return *(RedDataHeader *)_data;}
    uint8_t* data() { return _data + sizeof(RedDataHeader);}
    void resize(uint32_t size);

private:
    uint32_t message_size() { return header().size + sizeof(RedDataHeader);}
    uint8_t* base() { return _data;}

private:
    uint8_t* _data;
    uint32_t _size;

    friend class RedPeer;
    friend class RedChannel;
};

#endif


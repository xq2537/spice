/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#include "common.h"
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <spice/protocol.h>
#include "red_peer.h"
#include "utils.h"
#include "debug.h"
#include "platform_utils.h"

typedef struct SslVerifyCbData {
    RedPeer::HostAuthOptions info;
    const char* host_name;
    bool all_preverify_ok;
} SslVerifyCbData;

static void ssl_error()
{
    unsigned long last_error = ERR_peek_last_error();

    ERR_print_errors_fp(stderr);
    THROW_ERR(SPICEC_ERROR_CODE_SSL_ERROR, "SSL Error:", ERR_error_string(last_error, NULL));
}

bool RedPeer::HostAuthOptions::set_cert_subject(const char* subject)
{
    std::string subject_str(subject);
    std::string::const_iterator iter = subject_str.begin();
    std::string entry;
    this->type_flags = RedPeer::HostAuthOptions::HOST_AUTH_OP_SUBJECT;
    this->host_subject.clear();

    while (true) {
        if ((iter == subject_str.end()) || (*iter == ',')) {
            RedPeer::HostAuthOptions::CertFieldValuePair entry_pair;
            int value_pos = entry.find_first_of('=');
            if ((value_pos == std::string::npos) || (value_pos == (entry.length() - 1))) {
                LOG_ERROR("host_subject bad format: assignment for %s is missing\n", entry.c_str());
                return false;
            }
            size_t start_pos = entry.find_first_not_of(' ');
            if ((start_pos == std::string::npos) || (start_pos == value_pos)) {
                LOG_ERROR("host_subject bad format: first part of assignment"
                         " must be non empty in %s\n", entry.c_str());
                return false;
            }
            entry_pair.first = entry.substr(start_pos, value_pos - start_pos);
            entry_pair.second = entry.substr(value_pos + 1);
            this->host_subject.push_back(entry_pair);
            DBG(0, "subject entry: %s=%s", entry_pair.first.c_str(), entry_pair.second.c_str());
            if (iter == subject_str.end()) {
                break;
            }
            entry.clear();
        } else if (*iter == '\\') {
            iter++;
            if (iter == subject_str.end()) {
                LOG_WARN("single \\ in host subject");
                entry.append(1, '\\');
                continue;
            } else if ((*iter == '\\') || (*iter == ',')) {
                entry.append(1, *iter);
            } else {
                LOG_WARN("single \\ in host subject");
                entry.append(1, '\\');
                continue;
            }
        } else {
            entry.append(1, *iter);
        }
        iter++;
    }
    return true;
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

void RedPeer::connect_to_peer(const char* host, int portnr)
{
    struct addrinfo ai, *result = NULL, *e;
    char uaddr[INET6_ADDRSTRLEN+1];
    char uport[33], port[33];
    int err = 0, rc, no_delay = 1;
    ASSERT(_ctx == NULL && _ssl == NULL && _peer == INVALID_SOCKET);
    try {
        memset(&ai,0, sizeof(ai));
        ai.ai_flags = AI_CANONNAME;
#ifdef AI_ADDRCONFIG
        ai.ai_flags |= AI_ADDRCONFIG;
#endif
        ai.ai_family = PF_UNSPEC;
        ai.ai_socktype = SOCK_STREAM;
        snprintf(port, sizeof(port), "%d", portnr);
        rc = getaddrinfo(host, port, &ai, &result);
        if (rc != 0) {
            THROW_ERR(SPICEC_ERROR_CODE_GETHOSTBYNAME_FAILED, "cannot resolve host address %s", host);
        }
        Lock lock(_lock);
        _peer = -1;
        for (e = result; e != NULL; e = e->ai_next) {
            if ((_peer = socket(e->ai_family, e->ai_socktype, e->ai_protocol)) == INVALID_SOCKET) {
                int err = sock_error();
                THROW_ERR(SPICEC_ERROR_CODE_SOCKET_FAILED, "failed to create socket: %s (%d)",
                          sock_err_message(err), err);
            }
            if (setsockopt(_peer, IPPROTO_TCP, TCP_NODELAY, (const char*)&no_delay, sizeof(no_delay)) ==
                SOCKET_ERROR) {
                LOG_WARN("set TCP_NODELAY failed");
            }

            getnameinfo((struct sockaddr*)e->ai_addr, e->ai_addrlen,
                        uaddr,INET6_ADDRSTRLEN, uport,32,
                        NI_NUMERICHOST | NI_NUMERICSERV);
            DBG(0, "Trying %s %s", uaddr, uport);
            if (::connect(_peer, e->ai_addr, e->ai_addrlen) == SOCKET_ERROR) {
                err = sock_error();
                LOG_INFO("Connect failed: %s (%d)",
                         sock_err_message(err), err);
                closesocket(_peer);
                _peer = -1;
                continue;
            }
            DBG(0, "Connected to %s %s", uaddr, uport);
            break;
        }
        lock.unlock();
        freeaddrinfo(result);
        if (_peer == -1) {
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

void RedPeer::connect_unsecure(const char* host, int portnr)
{
    connect_to_peer(host, portnr);
    ASSERT(_ctx == NULL && _ssl == NULL && _peer != INVALID_SOCKET);
    LOG_INFO("Connected to %s %d", host, portnr);
}

bool RedPeer::verify_pubkey(X509* cert, const HostAuthOptions::PublicKey& key)
{
    EVP_PKEY* cert_pubkey = NULL;
    EVP_PKEY* orig_pubkey = NULL;
    BIO* bio = NULL;
    uint8_t* c_key = NULL;
    int ret = 0;

    if (key.empty()) {
        return false;
    }

    ASSERT(cert);

    try {
        cert_pubkey = X509_get_pubkey(cert);
        if (!cert_pubkey) {
            THROW("reading public key from certificate failed");
        }

        c_key = new uint8_t[key.size()];
        memcpy(c_key, &key[0], key.size());

        bio = BIO_new_mem_buf((void*)c_key, key.size());
        if (!bio) {
            THROW("creating BIO failed");
        }

        orig_pubkey = d2i_PUBKEY_bio(bio, NULL);
        if (!orig_pubkey) {
            THROW("reading pubkey from bio failed");
        }

        ret = EVP_PKEY_cmp(orig_pubkey, cert_pubkey);

        BIO_free(bio);
        EVP_PKEY_free(orig_pubkey);
        EVP_PKEY_free(cert_pubkey);
        delete []c_key;
        if (ret == 1) {
            DBG(0, "public keys match");
            return true;
        } else if (ret == 0) {
            DBG(0, "public keys mismatch");
            return false;
        } else {
            DBG(0, "public keys types mismatch");
            return false;
        }
    } catch (Exception& e) {
        LOG_WARN("%s", e.what());

        if (bio) {
            BIO_free(bio);
        }

        if (orig_pubkey) {
            EVP_PKEY_free(orig_pubkey);
        }

        if (cert_pubkey) {
            EVP_PKEY_free(cert_pubkey);
        }
        delete []c_key;
        return false;
    }
}

/* From gnutls: compare host_name against certificate, taking account of wildcards.
 * return true on success or false on error.
 *
 * note: cert_name_size is required as X509 certs can contain embedded NULs in
 * the strings such as CN or subjectAltName
 */
bool RedPeer::x509_cert_host_name_compare(const char *cert_name, int cert_name_size,
                                          const char *host_name)
{
    /* find the first different character */
    for (; *cert_name && *host_name && (toupper(*cert_name) == toupper(*host_name));
         cert_name++, host_name++, cert_name_size--);

    /* the strings are the same */
    if (cert_name_size == 0 && *host_name == '\0')
        return true;

    if (*cert_name == '*')
    {
        /* a wildcard certificate */
        cert_name++;
        cert_name_size--;

        while (true)
        {
            /* Use a recursive call to allow multiple wildcards */
            if (RedPeer::x509_cert_host_name_compare(cert_name, cert_name_size, host_name)) {
                return true;
            }

            /* wildcards are only allowed to match a single domain
               component or component fragment */
            if (*host_name == '\0' || *host_name == '.')
                break;
            host_name++;
        }

        return false;
    }

  return false;
}

/*
 * From gnutls_x509_crt_check_hostname - compares the hostname with certificate's hostname
 *
 * This function will check if the given certificate's subject matches
 * the hostname.  This is a basic implementation of the matching
 * described in RFC2818 (HTTPS), which takes into account wildcards,
 * and the DNSName/IPAddress subject alternative name PKIX extension.
 *
 */
bool RedPeer::verify_host_name(X509* cert, const char* host_name)
{
    GENERAL_NAMES* subject_alt_names;
    bool found_dns_name = false;
    struct in_addr addr;
    int addr_len = 0;
    bool cn_match = false;

    ASSERT(cert);

    // only IpV4 supported
    if (inet_aton(host_name, &addr)) {
        addr_len = sizeof(struct in_addr);
    }

    /* try matching against:
    *  1) a DNS name or IP address as an alternative name (subjectAltName) extension
    *     in the certificate
    *  2) the common name (CN) in the certificate
    *
    *  either of these may be of the form: *.domain.tld
    *
    *  only try (2) if there is no subjectAltName extension of
    *  type dNSName
    */


    subject_alt_names = (GENERAL_NAMES*)X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);

    if (subject_alt_names) {
        int num_alts = sk_GENERAL_NAME_num(subject_alt_names);
        for (int i = 0; i < num_alts; i++) {
            const GENERAL_NAME* name = sk_GENERAL_NAME_value(subject_alt_names, i);
            if (name->type == GEN_DNS) {
                found_dns_name = true;
                if (RedPeer::x509_cert_host_name_compare((char *)ASN1_STRING_data(name->d.dNSName),
                                                         ASN1_STRING_length(name->d.dNSName),
                                                         host_name)) {
                    DBG(0, "alt name match=%s", ASN1_STRING_data(name->d.dNSName));
                    GENERAL_NAMES_free(subject_alt_names);
                    return true;
                }
            } else if (name->type == GEN_IPADD) {
                int alt_ip_len = ASN1_STRING_length(name->d.iPAddress);
                found_dns_name = true;
                if ((addr_len == alt_ip_len)&&
                    !memcmp(ASN1_STRING_data(name->d.iPAddress), &addr, addr_len)) {
                         DBG(0, "alt name IP match=%s",
                             inet_ntoa(*((struct in_addr*)ASN1_STRING_data(name->d.dNSName))));
                    GENERAL_NAMES_free(subject_alt_names);
                    return true;
                }
            }
        }
        GENERAL_NAMES_free(subject_alt_names);
    }

    if (found_dns_name)
    {
        DBG(0, "SubjectAltName mismatch");
        return false;
    }

    /* extracting commonNames */
    X509_NAME* subject = X509_get_subject_name(cert);
    if (subject) {
        int pos = -1;
        X509_NAME_ENTRY* cn_entry;
        ASN1_STRING* cn_asn1;

        while ((pos = X509_NAME_get_index_by_NID(subject, NID_commonName, pos)) != -1) {
            cn_entry = X509_NAME_get_entry(subject, pos);
            if (!cn_entry) {
                continue;
            }
            cn_asn1 = X509_NAME_ENTRY_get_data(cn_entry);
            if (!cn_asn1) {
                continue;
            }

            if (RedPeer::x509_cert_host_name_compare((char*)ASN1_STRING_data(cn_asn1),
                                                     ASN1_STRING_length(cn_asn1),
                                                     host_name)) {
                DBG(0, "common name match=%s", (char*)ASN1_STRING_data(cn_asn1));
                cn_match = true;
                break;
            }
        }
    }

    if (!cn_match) {
        DBG(0, "common name mismatch");
    }
    return cn_match;

}

bool RedPeer::verify_subject(X509* cert, const HostAuthOptions::CertFieldValueList& subject)
{
    X509_NAME* cert_subject = NULL;
    HostAuthOptions::CertFieldValueList::const_iterator subject_iter;
    X509_NAME* in_subject;
    int ret;

    ASSERT(cert);

    cert_subject = X509_get_subject_name(cert);
    if (!cert_subject) {
        LOG_WARN("reading certificate subject failed");
        return false;
    }

    if (X509_NAME_entry_count(cert_subject) != subject.size()) {
        LOG_ERROR("subject mismatch: #entries cert=%d, input=%d",
            X509_NAME_entry_count(cert_subject), subject.size());
        return false;
    }

    in_subject = X509_NAME_new();
    if (!in_subject) {
        LOG_WARN("failed to allocate X509_NAME");
        return false;
    }

    for (subject_iter = subject.begin(); subject_iter != subject.end(); subject_iter++) {
        if (!X509_NAME_add_entry_by_txt(in_subject,
                                        subject_iter->first.c_str(),
                                        MBSTRING_UTF8,
                                        (const unsigned char*)subject_iter->second.c_str(),
                                        subject_iter->second.length(), -1, 0)) {
            LOG_WARN("failed to add entry %s=%s to X509_NAME",
                     subject_iter->first.c_str(), subject_iter->second.c_str());
             X509_NAME_free(in_subject);
             return false;
        }
    }

    ret = X509_NAME_cmp(cert_subject, in_subject);
    X509_NAME_free(in_subject);

    if (ret == 0) {
         DBG(0, "subjects match");
         return true;
    } else {
         LOG_ERROR("host-subject mismatch");
         return false;
    }
}

int RedPeer::ssl_verify_callback(int preverify_ok, X509_STORE_CTX *ctx)
{
    int depth;
    SSL *ssl;
    X509* cert;
    SslVerifyCbData* verify_data;
    int auth_flags;

    depth = X509_STORE_CTX_get_error_depth(ctx);

    ssl = (SSL*)X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    if (!ssl) {
        LOG_WARN("failed to get ssl connection");
        return 0;
    }

    verify_data = (SslVerifyCbData*)SSL_get_app_data(ssl);
    auth_flags = verify_data->info.type_flags;

    if (depth > 0) {
        // if certificate verification failed, we can still authorize the server
        // if its public key matches the one we hold in the peer_connect_options.
        if (!preverify_ok) {
            DBG(0, "openssl verify failed at depth=%d", depth);
            verify_data->all_preverify_ok = false;
            if (auth_flags & HostAuthOptions::HOST_AUTH_OP_PUBKEY) {
                return 1;
            } else {
                return 0;
            }
        } else {
            return preverify_ok;
        }
    }

    /* depth == 0 */
    cert = X509_STORE_CTX_get_current_cert(ctx);
    if (!cert) {
        LOG_WARN("failed to get server certificate");
        return 0;
    }

    if (auth_flags & HostAuthOptions::HOST_AUTH_OP_PUBKEY) {
        if (verify_pubkey(cert, verify_data->info.host_pubkey)) {
            return 1;
        }
    }

    if (!verify_data->all_preverify_ok || !preverify_ok) {
        return 0;
    }

    if (auth_flags & HostAuthOptions::HOST_AUTH_OP_NAME) {
        if (verify_host_name(cert, verify_data->host_name)) {
            return 1;
        }
    }

    if (auth_flags & HostAuthOptions::HOST_AUTH_OP_SUBJECT) {
        if (verify_subject(cert, verify_data->info.host_subject)) {
            return 1;
        }
    }
    return 0;
}

void RedPeer::connect_secure(const ConnectionOptions& options, const char* host)
{
    int return_code;
    int auth_flags;
    SslVerifyCbData auth_data;
    int portnr = options.secure_port;

    connect_to_peer(host, portnr);
    ASSERT(_ctx == NULL && _ssl == NULL && _peer != INVALID_SOCKET);
    LOG_INFO("Connected to %s %d", host, portnr);

    try {
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
        const SSL_METHOD *ssl_method = TLSv1_method();
#else
        SSL_METHOD *ssl_method = TLSv1_method();
#endif
        auth_data.info = options.host_auth;
        auth_data.host_name = host;
        auth_data.all_preverify_ok = true;

        _ctx = SSL_CTX_new(ssl_method);
        if (_ctx == NULL) {
            ssl_error();
        }

        auth_flags = auth_data.info.type_flags;
        if ((auth_flags & RedPeer::HostAuthOptions::HOST_AUTH_OP_NAME) ||
            (auth_flags & RedPeer::HostAuthOptions::HOST_AUTH_OP_SUBJECT)) {
            std::string CA_file = auth_data.info.CA_file;
            ASSERT(!CA_file.empty());

            return_code = SSL_CTX_load_verify_locations(_ctx, CA_file.c_str(), NULL);
            if (return_code != 1) {
                if (auth_flags & RedPeer::HostAuthOptions::HOST_AUTH_OP_PUBKEY) {
                    LOG_WARN("SSL_CTX_load_verify_locations failed, CA_file=%s. "
                             "only pubkey authentication is active", CA_file.c_str());
                    auth_data.info.type_flags = RedPeer::HostAuthOptions::HOST_AUTH_OP_PUBKEY;
                }
                else {
                    LOG_ERROR("SSL_CTX_load_verify_locations failed CA_file=%s", CA_file.c_str());
                    ssl_error();
                }
            }
        }

        if (auth_flags) {
            SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER, ssl_verify_callback);
        }

        return_code = SSL_CTX_set_cipher_list(_ctx, options.ciphers.c_str());
        if (return_code != 1) {
            LOG_ERROR("SSL_CTX_set_cipher_list failed, ciphers=%s", options.ciphers.c_str());
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
        SSL_set_app_data(_ssl, &auth_data);

        return_code = SSL_connect(_ssl);
        if (return_code <= 0) {
            int ssl_error_code = SSL_get_error(_ssl, return_code);
            LOG_ERROR("failed to connect w/SSL, ssl_error %s",
                     ERR_error_string(ssl_error_code, NULL));
            ssl_error();
        }
    } catch (...) {
        Lock lock(_lock);
        cleanup();
        throw;
    }
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

RedPeer::CompoundInMessage* RedPeer::recive()
{
    SpiceDataHeader header;
    AutoRef<CompoundInMessage> message;

    recive((uint8_t*)&header, sizeof(SpiceDataHeader));
    message.reset(new CompoundInMessage(header.serial, header.type, header.size, header.sub_list));
    recive((*message)->data(), (*message)->compound_size());
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

uint32_t RedPeer::do_send(RedPeer::OutMessage& message, uint32_t skip_bytes)
{
    uint8_t *data;
    int free_data;
    size_t len;
    uint32_t res;

    data = spice_marshaller_linearize(message.marshaller(), skip_bytes,
                                      &len, &free_data);

    res = send(data, len);

    if (free_data) {
        free(data);
    }
    return res;
}

uint32_t RedPeer::send(RedPeer::OutMessage& message)
{

    message.header().serial = ++_serial;
    message.header().size = message.message_size() - sizeof(SpiceDataHeader);

    return do_send(message, 0);
}

RedPeer::OutMessage::OutMessage(uint32_t type)
    : _marshaller (spice_marshaller_new())
{
    SpiceDataHeader *header;
    header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(_marshaller, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(_marshaller, sizeof(SpiceDataHeader));

    header->type = type;
    header->sub_list = 0;
}

void RedPeer::OutMessage::reset(uint32_t type)
{
    spice_marshaller_reset(_marshaller);

    SpiceDataHeader *header;
    header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(_marshaller, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(_marshaller, sizeof(SpiceDataHeader));

    header->type = type;
    header->sub_list = 0;
}

RedPeer::OutMessage::~OutMessage()
{
    spice_marshaller_destroy(_marshaller);
}

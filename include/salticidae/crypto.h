/**
 * Copyright (c) 2018 Cornell University.
 *
 * Author: Ted Yin <tederminant@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SALTICIDAE_CRYPTO_H
#define _SALTICIDAE_CRYPTO_H

#include "salticidae/type.h"
#include "salticidae/util.h"
#include <openssl/sha.h>
#include <openssl/ssl.h>

namespace salticidae {

class SHA256 {
    SHA256_CTX ctx;

    public:
    SHA256() { reset(); }

    void reset() {
        if (!SHA256_Init(&ctx))
            throw std::runtime_error("openssl SHA256 init error");
    }

    template<typename T>
    void update(const T &data) {
        update(reinterpret_cast<const uint8_t *>(&*data.begin()), data.size());
    }

    void update(const bytearray_t::const_iterator &it, size_t length) {
        update(&*it, length);
    }

    void update(const uint8_t *ptr, size_t length) {
        if (!SHA256_Update(&ctx, ptr, length))
            throw std::runtime_error("openssl SHA256 update error");
    }

    void _digest(bytearray_t &md) {
        if (!SHA256_Final(&*md.begin(), &ctx))
            throw std::runtime_error("openssl SHA256 error");
    }

    void digest(bytearray_t &md) {
        md.resize(32);
        _digest(md);
    }

    bytearray_t digest() {
        bytearray_t md(32);
        _digest(md);
        return std::move(md);
    }
};

class SHA1 {
    SHA_CTX ctx;

    public:
    SHA1() { reset(); }

    void reset() {
        if (!SHA1_Init(&ctx))
            throw std::runtime_error("openssl SHA1 init error");
    }

    template<typename T>
    void update(const T &data) {
        update(reinterpret_cast<const uint8_t *>(&*data.begin()), data.size());
    }

    void update(const bytearray_t::const_iterator &it, size_t length) {
        update(&*it, length);
    }

    void update(const uint8_t *ptr, size_t length) {
        if (!SHA1_Update(&ctx, ptr, length))
            throw std::runtime_error("openssl SHA1 update error");
    }

    void _digest(bytearray_t &md) {
        if (!SHA1_Final(&*md.begin(), &ctx))
            throw std::runtime_error("openssl SHA1 error");
    }

    void digest(bytearray_t &md) {
        md.resize(32);
        _digest(md);
    }

    bytearray_t digest() {
        bytearray_t md(32);
        _digest(md);
        return std::move(md);
    }
};

class TLSContext {
    SSL_CTX *ctx;
    friend class TLS;
    public:
    static void init_tls() { SSL_library_init(); }
    TLSContext(): ctx(SSL_CTX_new(TLS_method())) {
        if (ctx == nullptr)
            throw std::runtime_error("TLSContext init error");
    }

    void use_cert_file(const std::string &fname) {
        auto ret = SSL_CTX_use_certificate_file(ctx, fname.c_str(), SSL_FILETYPE_PEM);
        if (ret <= 0)
            throw SalticidaeError(SALTI_ERROR_TLS_CERT_ERROR);
    }

    void use_priv_key_file(const std::string &fname) {
        auto ret = SSL_CTX_use_PrivateKey_file(ctx, fname.c_str(), SSL_FILETYPE_PEM);
        if (ret <= 0)
            throw SalticidaeError(SALTI_ERROR_TLS_KEY_ERROR);
    }

    bool check_priv_key() {
        return SSL_CTX_check_private_key(ctx) > 0;
    }

    ~TLSContext() { SSL_CTX_free(ctx); }
};

using tls_context_t = ArcObj<TLSContext>;

class TLS {
    SSL *ssl;
    public:
    TLS(const tls_context_t &ctx, int fd, bool accept): ssl(SSL_new(ctx->ctx)) {
        if (ssl == nullptr)
            throw std::runtime_error("TLS init error");
        if (!SSL_set_fd(ssl, fd))
            throw SalticidaeError(SALTI_ERROR_TLS_GENERIC_ERROR);
        if (accept)
            SSL_set_accept_state(ssl);
        else
            SSL_set_connect_state(ssl);
    }

    bool do_handshake(int &want_io_type) { /* 0 for read, 1 for write */
        auto ret = SSL_do_handshake(ssl);
        if (ret == 1) return true;
        auto err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_WRITE)
            want_io_type = 1;
        else if (err == SSL_ERROR_WANT_READ)
            want_io_type = 0;
        else
            throw SalticidaeError(SALTI_ERROR_TLS_GENERIC_ERROR);
        return false;
    }

    inline int send(const void *buff, size_t size) {
        return SSL_write(ssl, buff, size);
    }

    inline int recv(void *buff, size_t size) {
        return SSL_read(ssl, buff, size);
    }

    int get_error(int ret) {
        return SSL_get_error(ssl, ret);
    }

    ~TLS() {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
};

}

#endif

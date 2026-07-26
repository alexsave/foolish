// conn.c — see conn.h.
#define _GNU_SOURCE
#include "conn.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifndef FOOLISH_NO_OPENSSL
#include <openssl/err.h>
#endif

ssize_t conn_read(Conn *c, void *buf, size_t n) {
    if (c->buf_out) return -1;   // Stage 6: a buffered Conn is a write-only encode sink, never a read source
    if (!c->ssl) {
        ssize_t r;
        do { r = read(c->fd, buf, n); } while (r < 0 && errno == EINTR);
        return r;
    }
#ifndef FOOLISH_NO_OPENSSL
    if (n == 0) return 0;
    for (;;) {
        int r = SSL_read(c->ssl, buf, (int)n);
        if (r > 0) return (ssize_t)r;
        int err = SSL_get_error(c->ssl, r);
        // WANT_READ/WANT_WRITE on a blocking socket only happens around a
        // renegotiation needing the opposite direction — SSL_MODE_AUTO_RETRY
        // (set in tls_*_ctx_create) makes OpenSSL retry this internally in
        // practice, but handle it explicitly too rather than depend on that
        // mode alone (correctness over relying on a single guard).
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
        if (err == SSL_ERROR_ZERO_RETURN) return 0;   // peer's clean close_notify — same as read()==0
        if (err == SSL_ERROR_SYSCALL && errno == EINTR) continue;
        return -1;   // peer gone / real error — caller already treats <0 as "tear this connection down", never crashes the process
    }
#else
    return -1;   // unreachable: c->ssl is always NULL in the no-OpenSSL (QUIC) build
#endif
}

ssize_t conn_write(Conn *c, const void *buf, size_t n) {
    if (c->buf_out) {   // Stage 6: append into the caller's memory buffer instead of a real write()
        int room = c->buf_cap - c->buf_len;
        int take = (int)n; if (take > room) take = room;
        if (take > 0) { memcpy(c->buf_out + c->buf_len, buf, (size_t)take); c->buf_len += take; }
        return take;   // short iff it wouldn't fit — callers (ws_write_full et al.) already treat a short result as "stop"
    }
    if (!c->ssl) {
        ssize_t r;
        do { r = write(c->fd, buf, n); } while (r < 0 && errno == EINTR);
        return r;
    }
#ifndef FOOLISH_NO_OPENSSL
    if (n == 0) return 0;
    for (;;) {
        int r = SSL_write(c->ssl, buf, (int)n);
        if (r > 0) return (ssize_t)r;
        int err = SSL_get_error(c->ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
        if (err == SSL_ERROR_SYSCALL && errno == EINTR) continue;
        return -1;
    }
#else
    return -1;   // unreachable: c->ssl is always NULL in the no-OpenSSL (QUIC) build
#endif
}

void conn_close(Conn *c) {
#ifndef FOOLISH_NO_OPENSSL
    if (c->ssl) {
        SSL_shutdown(c->ssl);   // best-effort clean shutdown; result never checked, see conn.h's doc
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#endif
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

#ifdef FOOLISH_NO_OPENSSL
// QUIC build: OpenSSL isn't linked. These never run (g_tls_ctx stays NULL, so
// every --tls branch upstream is dead code). Stubbed so the call sites still
// compile and link. TLS for the TCP listener is terminated at the edge;
// encrypted transport in this build is QUIC (see quic_wt.c).
bool conn_tls_accept(Conn *out, SSL_CTX *ctx, int fd) { (void)out; (void)ctx; (void)fd; return false; }
bool conn_tls_connect(Conn *out, SSL_CTX *ctx, int fd, const char *sni) { (void)out; (void)ctx; (void)fd; (void)sni; return false; }
SSL_CTX *tls_server_ctx_create(const char *cert_path, const char *key_path) { (void)cert_path; (void)key_path; return NULL; }
SSL_CTX *tls_client_ctx_create(void) { return NULL; }
#else
bool conn_tls_accept(Conn *out, SSL_CTX *ctx, int fd) {
    SSL *ssl = SSL_new(ctx);
    if (!ssl) return false;
    SSL_set_fd(ssl, fd);
    int r = SSL_accept(ssl);
    if (r != 1) { SSL_free(ssl); return false; }
    out->fd = fd;
    out->ssl = ssl;
    // Fully initialize the Stage 6 buffered-mode fields: this Conn is a real
    // socket, never a memory sink. Callers stack-allocate `Conn conn;` and only
    // ever set fd/ssl through us, so leaving buf_out uninitialized lets stack
    // garbage make conn_read()'s `if (c->buf_out) return -1;` guard fire on the
    // very first read — a TLS connection that handshakes fine then can't read a
    // byte (intermittent, stack-layout dependent). conn_init_plain zeroes these;
    // the TLS path must too.
    out->buf_out = NULL;
    out->buf_len = 0;
    out->buf_cap = 0;
    return true;
}

bool conn_tls_connect(Conn *out, SSL_CTX *ctx, int fd, const char *sni_hostname) {
    SSL *ssl = SSL_new(ctx);
    if (!ssl) return false;
    SSL_set_fd(ssl, fd);
    if (sni_hostname && *sni_hostname) SSL_set_tlsext_host_name(ssl, sni_hostname);
    int r = SSL_connect(ssl);
    if (r != 1) { SSL_free(ssl); return false; }
    out->fd = fd;
    out->ssl = ssl;
    out->buf_out = NULL;   // see conn_tls_accept: full-init the buffered-mode fields, never a memory sink
    out->buf_len = 0;
    out->buf_cap = 0;
    return true;
}

SSL_CTX *tls_server_ctx_create(const char *cert_path, const char *key_path) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

SSL_CTX *tls_client_ctx_create(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);   // load tool vs. a self-signed test cert — see conn.h's doc
    return ctx;
}
#endif   // FOOLISH_NO_OPENSSL

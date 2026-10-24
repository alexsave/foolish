// tls.c - see tls.h.
#include "net/tls.h"

#include "util/err.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cb_tls {
    SSL_CTX *ctx;
    _Atomic long long full, resumed, failed;
};

struct cb_tls_conn {
    SSL    *ssl;
    cb_tls *t;
    int     counted;   // the handshake's outcome has been counted
};

static void err_string(char *err, size_t cap, const char *what) {
    unsigned long e = ERR_get_error();
    char buf[256] = "";
    if (e) ERR_error_string_n(e, buf, sizeof buf);
    snprintf(err, cap, "tls: %s%s%s", what, *buf ? ": " : "", buf);
    ERR_clear_error();
}

int cb_tls_new(const char *cert_pem, const char *key_pem, cb_tls **out, char *err, size_t cap) {
    *out = NULL;
    cb_tls *t = calloc(1, sizeof *t);
    if (!t) { snprintf(err, cap, "tls: out of memory"); return CB_E_NOMEM; }
    t->ctx = SSL_CTX_new(TLS_server_method());
    if (!t->ctx) { err_string(err, cap, "no context"); free(t); return CB_E_INVALID; }
    // TLS 1.2 at least. With the cipher list below (AEAD suites only, which need 1.2) this is also
    // what the list alone would enforce; it stays so that widening the list never opens 1.0 or 1.1.
    SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION);
    // TLS 1.2: ECDHE with an AEAD only.
    if (!SSL_CTX_set_cipher_list(t->ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
                                         "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305")) {
        err_string(err, cap, "the cipher list");
        cb_tls_free(t);
        return CB_E_INVALID;
    }
    // TLS 1.3: AES-128-GCM first (hardware AES makes it the cheapest), ChaCha20 for clients without
    // it, AES-256 last.
    if (!SSL_CTX_set_ciphersuites(t->ctx, "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384")) {
        err_string(err, cap, "the TLS 1.3 suites");
        cb_tls_free(t);
        return CB_E_INVALID;
    }
    SSL_CTX_set_options(t->ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    // Writes may stop part way (a full socket) and go on later from a buffer that has moved.
    SSL_CTX_set_mode(t->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_RELEASE_BUFFERS);
    // Resumption: tickets (stateless, the default) and the server's own cache for resumption by id.
    SSL_CTX_set_session_cache_mode(t->ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_session_id_context(t->ctx, (const unsigned char *)"cbase", 5);
    SSL_CTX_set_timeout(t->ctx, 24 * 3600);
    if (SSL_CTX_use_certificate_chain_file(t->ctx, cert_pem) != 1) { err_string(err, cap, cert_pem); cb_tls_free(t); return CB_E_INVALID; }
    if (SSL_CTX_use_PrivateKey_file(t->ctx, key_pem, SSL_FILETYPE_PEM) != 1) { err_string(err, cap, key_pem); cb_tls_free(t); return CB_E_INVALID; }
    if (SSL_CTX_check_private_key(t->ctx) != 1) { err_string(err, cap, "the key does not match the certificate"); cb_tls_free(t); return CB_E_INVALID; }
    *out = t;
    return CB_OK;
}

void cb_tls_free(cb_tls *t) {
    if (!t) return;
    if (t->ctx) SSL_CTX_free(t->ctx);
    free(t);
}

cb_tls_conn *cb_tls_conn_new(cb_tls *t, int fd) {
    cb_tls_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->t = t;
    if (!(c->ssl = SSL_new(t->ctx)) || SSL_set_fd(c->ssl, fd) != 1) {
        if (c->ssl) SSL_free(c->ssl);
        free(c);
        ERR_clear_error();
        return NULL;
    }
    SSL_set_accept_state(c->ssl);
    return c;
}

void cb_tls_conn_free(cb_tls_conn *c) {
    if (!c) return;
    if (!c->counted) atomic_fetch_add(&c->t->failed, 1);   // closed before its handshake finished
    SSL_free(c->ssl);
    free(c);
}

static void count(cb_tls_conn *c) {
    if (c->counted || !SSL_is_init_finished(c->ssl)) return;
    c->counted = 1;
    atomic_fetch_add(SSL_session_reused(c->ssl) ? &c->t->resumed : &c->t->full, 1);
}

// An SSL_* call's result as tls.h's: the bytes, 0 for a closed peer, WANT or FAIL.
static ssize_t outcome(cb_tls_conn *c, int r, int *want) {
    if (r > 0) { count(c); return r; }
    switch (SSL_get_error(c->ssl, r)) {
    case SSL_ERROR_WANT_READ: *want = CB_TLS_WANT_READ; return CB_TLS_WANT;
    case SSL_ERROR_WANT_WRITE: *want = CB_TLS_WANT_WRITE; return CB_TLS_WANT;
    case SSL_ERROR_ZERO_RETURN: ERR_clear_error(); return 0;   // the peer's close_notify
    default: ERR_clear_error(); return CB_TLS_FAIL;          // a bad record, a reset, a handshake refused
    }
}

ssize_t cb_tls_read(cb_tls_conn *c, void *buf, size_t n, int *want) {
    *want = 0;
    return outcome(c, SSL_read(c->ssl, buf, n > 0x7fffffff ? 0x7fffffff : (int)n), want);
}

ssize_t cb_tls_write(cb_tls_conn *c, const void *buf, size_t n, int *want) {
    *want = 0;
    if (n == 0) return 0;
    return outcome(c, SSL_write(c->ssl, buf, n > 0x7fffffff ? 0x7fffffff : (int)n), want);
}

void cb_tls_shutdown(cb_tls_conn *c) {
    if (!c || !SSL_is_init_finished(c->ssl)) return;
    SSL_shutdown(c->ssl);   /* writes close_notify if the socket takes it; the answer is not awaited */
    ERR_clear_error();
}

int cb_tls_ready(const cb_tls_conn *c) { return SSL_is_init_finished(c->ssl); }
int cb_tls_resumed(const cb_tls_conn *c) { return SSL_session_reused(c->ssl); }

void cb_tls_counts(const cb_tls *t, long long *full, long long *resumed, long long *failed) {
    *full = atomic_load(&t->full);
    *resumed = atomic_load(&t->resumed);
    *failed = atomic_load(&t->failed);
}

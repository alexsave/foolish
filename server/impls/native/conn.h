// conn.h — a per-connection I/O abstraction: EITHER a plain TCP fd OR an
// OpenSSL TLS session over one (Stage 3, TLS — foolish_server.c's old
// "STAGE 3 SEAM" comment and SERVER_SCALING.md/DURABILITY.md's "Seams left"
// sections named this exact shape ahead of time: `Conn { int fd; SSL *ssl; }`
// in place of a bare `int`). Every socket byte foolish_server.c, ws.c, and
// foolish_hammer.c move now goes through one of the functions below instead
// of touching read()/write()/close() directly, so plaintext and TLS share
// EXACTLY one code path above this layer — nothing upstream inspects
// c->fd/c->ssl directly. See TLS.md for the full design writeup.
#ifndef FOOLISH_CONN_H
#define FOOLISH_CONN_H

#include <stdbool.h>
#include <sys/types.h>
#include <openssl/ssl.h>

// fd is always valid (>= 0) for an open connection; ssl is NULL for a
// plaintext connection and a live per-connection SSL* for a TLS one. The
// SSL_CTX a Conn's ssl was created from IS shared read-only across every
// connection/thread (see tls_server_ctx_create's doc), but this SSL* itself
// is never shared: each accepted/connected socket gets its own, fresh, in
// conn_tls_accept/conn_tls_connect — never touched from more than the one
// thread that owns this Conn.
typedef struct {
    int fd;
    SSL *ssl;
} Conn;

static inline void conn_init_plain(Conn *c, int fd) { c->fd = fd; c->ssl = NULL; }

// Single-call, EINTR-transparent read/write — the SAME contract a bare
// read()/write() on a blocking fd has (a short result is normal, not an
// error; 0 means the peer is done / cleanly closed; <0 means a real error,
// never crashes the process — SIGPIPE is separately ignored, see
// foolish_server.c's/foolish_hammer.c's main()). Dispatches to read()/
// write() when c->ssl is NULL (byte-for-byte the same behavior a plain `int
// fd` had before Stage 3 — see TLS.md's "plaintext is unchanged" claim),
// else SSL_read()/SSL_write(), retrying internally on SSL_ERROR_WANT_READ/
// WANT_WRITE (the only way those can happen on a blocking socket — a
// renegotiation needing the other direction — see tls_server_ctx_create's
// SSL_MODE_AUTO_RETRY, which usually makes this loop moot) and translating
// SSL_ERROR_ZERO_RETURN (peer's clean TLS close_notify) to a plain 0, same
// as EOF.
ssize_t conn_read(Conn *c, void *buf, size_t n);
ssize_t conn_write(Conn *c, const void *buf, size_t n);

// SSL_shutdown (best-effort, result never checked — a peer that's already
// gone, mid-crash or past its own timeout, is normal here, same posture
// ws_recv_message's -1 contract takes) + SSL_free, then close(fd). Safe to
// call on an already-plaintext Conn (ssl == NULL: just closes fd). Call
// exactly once per open Conn.
void conn_close(Conn *c);

// Server-side: SSL_new off `ctx` + SSL_set_fd(fd) + SSL_accept, all on the
// CALLING thread (this connection's own servicing thread — never call this
// for the same fd from two threads, and never share the resulting SSL*
// across threads afterward either). On success fills *out (fd + the fresh
// SSL*) and returns true; on failure frees anything it allocated and
// returns false — the caller still owns and must close `fd` itself (this
// mirrors persist_start's "a requested guarantee that fails to set up is
// the caller's problem to surface, not silently swallowed" posture, just
// scoped to one connection instead of the whole process).
bool conn_tls_accept(Conn *out, SSL_CTX *ctx, int fd);

// Client-side (foolish_hammer.c --tls): SSL_new + SSL_set_fd + (if
// `sni_hostname` is non-NULL/non-empty) SSL_set_tlsext_host_name +
// SSL_connect. Same success/failure contract as conn_tls_accept.
bool conn_tls_connect(Conn *out, SSL_CTX *ctx, int fd, const char *sni_hostname);

// One-time process startup: builds the server- or client-side SSL_CTX.
// Created ONCE (main(), before any worker/connection thread exists) and
// shared READ-ONLY across every thread afterward — safe under OpenSSL 3's
// default library context (no explicit locking callbacks needed, unlike
// pre-1.1.0 OpenSSL); nothing here is mutated post-setup, and every
// accepted/connected socket gets its OWN SSL* off it (conn_tls_accept/
// conn_tls_connect), never a shared one — see TLS.md's Helgrind section.
// Min version TLS 1.2 (this task's brief: "a sane min version TLS1.2+" —
// no SSLv3/TLS1.0/1.1). Returns NULL on any setup failure (missing/invalid
// cert or key file, a key that doesn't match the cert, ...) — main()
// treats a requested-but-failed TLS setup as fatal, the same posture Stage
// 2 takes for a requested-but-failed --db (see persist_start's doc).
SSL_CTX *tls_server_ctx_create(const char *cert_path, const char *key_path);

// Client-side context (foolish_hammer.c --tls): verification OFF
// (SSL_VERIFY_NONE) — this is a load-test tool hitting the server's own
// self-signed test cert (tls_test.sh generates it with the openssl CLI),
// not a browser talking to a real CA-issued cert; a real client would
// pin/verify. Stated plainly rather than hidden: see TLS.md.
SSL_CTX *tls_client_ctx_create(void);

#endif

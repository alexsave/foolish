// tls.h - TLS for the server's own listener (net/server.c), with OpenSSL: the server terminates TLS
// itself when CBASE_TLS_CERT and CBASE_TLS_KEY are set, on the loop thread, non-blocking, so a
// browser's https and wss reach the app with no proxy in front. Off (the default), the server is
// plaintext and none of this runs: a connection's `tls` is NULL and its paths are the plaintext ones.
//
// THE POLICY. TLS 1.2 and 1.3 only; on 1.2, ECDHE with AES-GCM or ChaCha20-Poly1305 and nothing
// else (no CBC, no RSA key exchange); 1.3's suites are all AEAD. Resumption: stateless session
// tickets (a resumed handshake is one round trip and no certificate signature), and the server's
// session cache for clients that resume by id. The certificate and key are PEM files.
//
// A connection's I/O: cb_tls_read and cb_tls_write return the bytes moved, 0 for a closed peer, or
// CB_TLS_WANT (nothing now: *want says whether the socket must become readable or writable first)
// or CB_TLS_FAIL. The handshake runs inside the first reads and writes, so the server needs no
// state of its own for it.
#ifndef CB_NET_TLS_H
#define CB_NET_TLS_H

#include <stddef.h>
#include <sys/types.h>

typedef struct cb_tls cb_tls;
typedef struct cb_tls_conn cb_tls_conn;

#define CB_TLS_WANT (-1)
#define CB_TLS_FAIL (-2)
#define CB_TLS_WANT_READ 1
#define CB_TLS_WANT_WRITE 2

// CB_OK and *out, or CB_E_INVALID with the reason in err (an unreadable file, a key that does not
// match the certificate).
int  cb_tls_new(const char *cert_pem, const char *key_pem, cb_tls **out, char *err, size_t cap);
void cb_tls_free(cb_tls *t);

cb_tls_conn *cb_tls_conn_new(cb_tls *t, int fd);   // NULL out of memory
void         cb_tls_conn_free(cb_tls_conn *c);
ssize_t      cb_tls_read(cb_tls_conn *c, void *buf, size_t n, int *want);
ssize_t      cb_tls_write(cb_tls_conn *c, const void *buf, size_t n, int *want);
// Sends close_notify (once, best effort, never waiting): a peer that saw the connection end without
// it treats the session as truncated and will not resume it.
void         cb_tls_shutdown(cb_tls_conn *c);
// Whether the handshake is done, and whether it resumed a session (after it is done).
int          cb_tls_ready(const cb_tls_conn *c);
int          cb_tls_resumed(const cb_tls_conn *c);

// Counters (any thread): full handshakes, resumed handshakes, handshakes that failed.
void cb_tls_counts(const cb_tls *t, long long *full, long long *resumed, long long *failed);

#endif

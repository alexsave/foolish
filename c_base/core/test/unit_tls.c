// unit_tls.c - the server speaking TLS itself (net/tls.h): a server on a loopback port with a
// certificate made here, and an OpenSSL client. HTTP and a WebSocket through TLS 1.3, a response
// larger than the socket's buffers written in parts, session resumption by ticket, the policy (no
// TLS 1.1, no CBC suite on 1.2), and what a plaintext or garbage client gets: closed, the server on.
#include "t.h"

#include "config.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/tls.h"
#include "net/ws.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static int g_port;
static pthread_t g_thread;
static cb_tls *g_tls;
static char g_dir[256], g_cert[300], g_key[300];

#define BIG (3 * 1024 * 1024)

static void h_echo(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { cb_res_send(s, 200, "application/octet-stream", q->body, q->body_len); }
static void h_big(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    char *b = malloc(BIG);
    for (size_t i = 0; i < BIG; i++) b[i] = (char)('a' + i % 26);
    cb_res_send(s, 200, "text/plain", b, BIG);
    free(b);
}
static void ws_msg(cb_ws *ws, int op, const uint8_t *d, size_t n, void *ud) { cb_ws_send(ws, op, d, n); }
static void ws_open(cb_ws *ws, void *ud) { cb_ws_send_text(ws, "hello", 5); }
static const cb_ws_handlers WS = { ws_open, ws_msg, NULL };
static void h_ws(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { cb_ws_upgrade(q, s, &WS, NULL); }

static void *run(void *arg) { cb_loop_run(g_ctx.loop); return NULL; }

// A self-signed P-256 certificate for "localhost" and its key, as PEM files in a temporary directory.
static void make_cert(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/cbase-tls-XXXXXX");
    T_CHECK(mkdtemp(g_dir) != NULL);
    snprintf(g_cert, sizeof g_cert, "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof g_key, "%s/key.pem", g_dir);
    EVP_PKEY *k = EVP_EC_gen("P-256");
    X509 *x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, k);
    X509_NAME *n = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1, 0);
    X509_set_issuer_name(x, n);
    T_CHECK(X509_sign(x, k, EVP_sha256()) > 0);
    FILE *f = fopen(g_cert, "w");
    PEM_write_X509(f, x);
    fclose(f);
    f = fopen(g_key, "w");
    PEM_write_PrivateKey(f, k, NULL, NULL, 0, NULL, NULL);
    fclose(f);
    X509_free(x);
    EVP_PKEY_free(k);
}

static void start(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    make_cert();
    char err[256];
    T_EQ_INT(cb_tls_new(g_cert, g_key, &g_tls, err, sizeof err), CB_OK);
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(2, "treq");
    g_ctx.router = cb_router_new();
    cb_router_add(g_ctx.router, "POST", "/echo", CB_ROUTE_POOL, h_echo);
    cb_router_add(g_ctx.router, "GET", "/big", CB_ROUTE_POOL, h_big);
    cb_router_add(g_ctx.router, "GET", "/ws", CB_ROUTE_LOOP, h_ws);
    cb_server_opts o;
    cb_server_opts_default(&o);
    o.max_body = 1 << 20;
    g_ctx.server = cb_server_new(&g_ctx, &o);
    cb_server_set_tls(g_ctx.server, g_tls);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, run, NULL);
}

static int tcp(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static SSL_CTX *client_ctx(int max_version) {
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (max_version) SSL_CTX_set_max_proto_version(c, max_version);
    SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_CLIENT);
    return c;
}

// A TLS connection (resuming `sess` when given), or NULL when the handshake failed.
static SSL *tls_connect(SSL_CTX *cc, SSL_SESSION *sess, int *fd_out) {
    int fd = tcp();
    if (fd < 0) return NULL;
    SSL *s = SSL_new(cc);
    SSL_set_fd(s, fd);
    SSL_set_tlsext_host_name(s, "localhost");
    if (sess) SSL_set_session(s, sess);
    if (SSL_connect(s) != 1) { SSL_free(s); close(fd); ERR_clear_error(); return NULL; }
    *fd_out = fd;
    return s;
}

static void ssl_write_all(SSL *s, const void *p, size_t n) {
    while (n) {
        int w = SSL_write(s, p, (int)n);
        if (w <= 0) return;
        p = (const char *)p + w;
        n -= (size_t)w;
    }
}

// Reads an HTTP response with a Content-Length body: the body's length, -1 on failure; the body in out.
static long read_response(SSL *s, char *out, size_t cap, int slow) {
    size_t have = 0;
    long need = -1;
    size_t head = 0;
    for (;;) {
        if (slow) usleep(2000);
        int r = SSL_read(s, out + have, (int)(cap - have > 65536 ? 65536 : cap - have));
        if (r <= 0) return -1;
        have += (size_t)r;
        if (need < 0) {
            out[have < cap ? have : cap - 1] = 0;
            char *e = strstr(out, "\r\n\r\n");
            if (!e) continue;
            head = (size_t)(e + 4 - out);
            char *cl = strcasestr(out, "content-length:");
            if (!cl) return -1;
            need = atol(cl + 15);
        }
        if (have >= head + (size_t)need) {
            memmove(out, out + head, (size_t)need);
            return need;
        }
    }
}

static void http_over_tls13(void) {
    SSL_CTX *cc = client_ctx(0);
    int fd;
    SSL *s = tls_connect(cc, NULL, &fd);
    T_CHECK(s != NULL);
    if (!s) { SSL_CTX_free(cc); return; }
    T_EQ_STR(SSL_get_version(s), "TLSv1.3");
    const char *req = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\nabc\0def";
    ssl_write_all(s, req, strlen(req) + 4);
    char buf[4096];
    long n = read_response(s, buf, sizeof buf, 0);
    T_EQ_MEM(buf, n < 0 ? 0 : (size_t)n, "abc\0def", 7);
    // a second request on the same connection
    ssl_write_all(s, req, strlen(req) + 4);
    T_EQ_INT(read_response(s, buf, sizeof buf, 0), 7);
    SSL_free(s);
    close(fd);
    SSL_CTX_free(cc);
}

// 3 MB, read slowly: the server's writes stop on a full socket (WANT_WRITE) and go on.
static void big_response_in_parts(void) {
    SSL_CTX *cc = client_ctx(0);
    int fd;
    SSL *s = tls_connect(cc, NULL, &fd);
    T_CHECK(s != NULL);
    if (!s) { SSL_CTX_free(cc); return; }
    const char *req = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
    ssl_write_all(s, req, strlen(req));
    char *buf = malloc(BIG + 4096);
    long n = read_response(s, buf, BIG + 4096, 1);
    T_EQ_INT(n, BIG);
    bool same = n == BIG;
    for (long i = 0; same && i < n; i++) same = buf[i] == (char)('a' + i % 26);
    T_CHECK(same);
    free(buf);
    SSL_free(s);
    close(fd);
    SSL_CTX_free(cc);
}

static void websocket_over_tls(void) {
    SSL_CTX *cc = client_ctx(0);
    int fd;
    SSL *s = tls_connect(cc, NULL, &fd);
    T_CHECK(s != NULL);
    if (!s) { SSL_CTX_free(cc); return; }
    const char *up = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    ssl_write_all(s, up, strlen(up));
    char buf[4096];
    size_t have = 0;
    char *e = NULL;
    while (!e) {
        int r = SSL_read(s, buf + have, (int)(sizeof buf - 1 - have));
        if (r <= 0) break;
        have += (size_t)r;
        buf[have] = 0;
        e = strstr(buf, "\r\n\r\n");
    }
    T_CHECK(e && strncmp(buf, "HTTP/1.1 101", 12) == 0);
    size_t at = e ? (size_t)(e + 4 - buf) : have;
    // "hello" from on_open: an unmasked text frame
    while (have - at < 7) { int r = SSL_read(s, buf + have, (int)(sizeof buf - have)); if (r <= 0) break; have += (size_t)r; }
    T_CHECK(have - at >= 7 && (uint8_t)buf[at] == 0x81 && buf[at + 1] == 5 && memcmp(buf + at + 2, "hello", 5) == 0);
    // a masked frame, echoed
    uint8_t f[9] = { 0x82, 0x83, 1, 2, 3, 4, 'a' ^ 1, 'b' ^ 2, 'c' ^ 3 };
    ssl_write_all(s, f, sizeof f);
    uint8_t got[16];
    int r = SSL_read(s, got, sizeof got);
    T_CHECK(r == 5 && got[0] == 0x82 && got[1] == 3 && memcmp(got + 2, "abc", 3) == 0);
    SSL_free(s);
    close(fd);
    SSL_CTX_free(cc);
}

static void resumption(void) {
    SSL_CTX *cc = client_ctx(0);
    int fd;
    SSL *s = tls_connect(cc, NULL, &fd);
    T_CHECK(s != NULL);
    if (!s) { SSL_CTX_free(cc); return; }
    // TLS 1.3's tickets come after the handshake: a request and its answer bring them
    const char *req = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nz";
    ssl_write_all(s, req, strlen(req));
    char buf[512];
    T_EQ_INT(read_response(s, buf, sizeof buf, 0), 1);
    SSL_SESSION *sess = SSL_get1_session(s);
    T_CHECK(sess && SSL_SESSION_is_resumable(sess));
    T_CHECK(!SSL_session_reused(s));
    SSL_shutdown(s);   // a session closed without close_notify is marked not resumable by the client
    SSL_free(s);
    close(fd);
    s = tls_connect(cc, sess, &fd);
    T_CHECK(s != NULL);
    if (s) {
        T_CHECK(SSL_session_reused(s));
        ssl_write_all(s, req, strlen(req));
        T_EQ_INT(read_response(s, buf, sizeof buf, 0), 1);
        SSL_free(s);
        close(fd);
    }
    SSL_SESSION_free(sess);
    SSL_CTX_free(cc);
    long long full, resumed, failed;
    cb_tls_counts(g_tls, &full, &resumed, &failed);
    T_CHECK(resumed >= 1);
    T_CHECK(full >= 1);
}

static void policy(void) {
    // TLS 1.1 is refused
    SSL_CTX *cc = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_max_proto_version(cc, TLS1_1_VERSION);
    SSL_CTX_set_security_level(cc, 0);
    int fd;
    SSL *s = tls_connect(cc, NULL, &fd);
    T_CHECK(s == NULL);
    if (s) { SSL_free(s); close(fd); }
    SSL_CTX_free(cc);
    // TLS 1.2 with a CBC suite only is refused; with an ECDHE AEAD suite it is taken
    cc = client_ctx(TLS1_2_VERSION);
    SSL_CTX_set_cipher_list(cc, "ECDHE-ECDSA-AES128-SHA");
    s = tls_connect(cc, NULL, &fd);
    T_CHECK(s == NULL);
    if (s) { SSL_free(s); close(fd); }
    SSL_CTX_set_cipher_list(cc, "ECDHE-ECDSA-CHACHA20-POLY1305");
    s = tls_connect(cc, NULL, &fd);
    T_CHECK(s != NULL);
    if (s) { T_EQ_STR(SSL_get_cipher(s), "ECDHE-ECDSA-CHACHA20-POLY1305"); SSL_free(s); close(fd); }
    SSL_CTX_free(cc);
    // TLS 1.3 prefers AES-128-GCM (the server's order)
    cc = client_ctx(0);
    s = tls_connect(cc, NULL, &fd);
    if (s) { T_EQ_STR(SSL_get_cipher(s), "TLS_AES_128_GCM_SHA256"); SSL_free(s); close(fd); }
    SSL_CTX_free(cc);
}

// What is not TLS is closed, and the server goes on.
static void not_tls(void) {
    long long full0, resumed0, failed0;
    cb_tls_counts(g_tls, &full0, &resumed0, &failed0);
    const char *probes[] = { "GET /echo HTTP/1.1\r\nHost: x\r\n\r\n", "\x16\x03\x01\x00\x05garbage-that-is-not-a-hello", "\x00\x01\x02" };
    for (size_t i = 0; i < sizeof probes / sizeof *probes; i++) {
        int fd = tcp();
        T_CHECK(fd >= 0);
        if (write(fd, probes[i], strlen(probes[i]) + (i == 2 ? 3 : 0)) < 0) {}
        char b[64];
        ssize_t r = read(fd, b, sizeof b);
        T_CHECK(r <= 0 || (uint8_t)b[0] == 0x15);   // closed, or a TLS alert, never an HTTP answer
        close(fd);
    }
    usleep(100 * 1000);
    long long full, resumed, failed;
    cb_tls_counts(g_tls, &full, &resumed, &failed);
    T_CHECK(failed >= failed0 + 3);
    http_over_tls13();   // still serving
}

static void config_pairs(void) {
    cb_config c;
    char err[256];
    setenv("CBASE_TLS_CERT", g_cert, 1);
    unsetenv("CBASE_TLS_KEY");
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_TLS_KEY") != NULL);
    setenv("CBASE_TLS_KEY", g_key, 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_STR(c.tls_key, g_key);
    unsetenv("CBASE_TLS_CERT");
    unsetenv("CBASE_TLS_KEY");
    cb_tls *t = NULL;
    T_EQ_INT(cb_tls_new(g_cert, "/nonexistent.pem", &t, err, sizeof err), CB_E_INVALID);
    T_CHECK(t == NULL);
}

int main(void) {
    setenv("CBASE_DEV", "1", 1);
    start();
    T_RUN(http_over_tls13);
    T_RUN(big_response_in_parts);
    T_RUN(websocket_over_tls);
    T_RUN(resumption);
    T_RUN(policy);
    T_RUN(not_tls);
    T_RUN(config_pairs);
    cb_loop_stop(g_ctx.loop);
    pthread_join(g_thread, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_tls_free(g_tls);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) {}
    return T_DONE();
}

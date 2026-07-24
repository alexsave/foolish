// fuzz_client.c — an ADVERSARIAL load client. Where foolish_hammer plays valid
// games, this one tries to break the server: malformed HTTP, junk/oversized
// signups, forged/garbage Bearer tokens, meta abuse (spamming bots to overflow
// seats, starting games that don't exist), unparseable binary /action bodies,
// hostile /state seat values (the VIEW_UNMASKED disclosure), and malformed
// WebSocket handshakes + frames (bogus lengths, opcodes, unmasked frames).
//
// It is a DEFENSIVE tool: run it against a foolish_server_asan build and watch
// for AddressSanitizer/UBSan reports, crashes, or hangs. The fuzzer never trusts
// or parses the server's replies strictly; it just throws hostile bytes and
// counts. A spike in connect() failures means the server died.
//   usage: fuzz_client <host> <port> <threads> <seconds>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static const char *g_host = "127.0.0.1";
static int g_port = 8099;
static volatile int g_stop = 0;
static _Atomic long g_ops = 0, g_conn_fail = 0, g_anomaly = 0;

// -------- small helpers --------
static uint32_t rr(unsigned *s) { return (uint32_t)rand_r(s); }
static int ri(unsigned *s, int n) { return n <= 0 ? 0 : (int)(rr(s) % (unsigned)n); }
static void rbytes(unsigned *s, uint8_t *b, int n) { for (int i = 0; i < n; i++) b[i] = (uint8_t)rr(s); }

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const uint8_t *in, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int r = n - i, v = in[i] << 16 | (r > 1 ? in[i+1] : 0) << 8 | (r > 2 ? in[i+2] : 0);
        out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
        out[o++] = r > 1 ? B64[(v >> 6) & 63] : '='; out[o++] = r > 2 ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

// Connect with a short timeout; -1 on failure (server down / refused / backlog).
static int dial(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    // Short I/O deadline: many attacks deliberately send an incomplete request,
    // so the server (correctly) holds the connection open waiting for more. We
    // must not block the fuzz loop on those — give up fast and move to the next
    // hostile payload, so the throughput stays high enough to be a real fuzz.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = inet_addr(g_host);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}
static void send_all(int fd, const void *p, int n) {
    const char *b = p; int off = 0;
    while (off < n) { ssize_t w = write(fd, b + off, (size_t)(n - off)); if (w <= 0) break; off += (int)w; }
}
// Drain up to `cap` bytes of the reply into buf (NUL-terminated); returns count.
static int drain(int fd, char *buf, int cap) {
    int tot = 0;
    for (;;) {
        ssize_t r = read(fd, buf + tot, (size_t)(cap - 1 - tot));
        if (r <= 0) break;
        tot += (int)r; if (tot >= cap - 1) break;
    }
    buf[tot] = 0; return tot;
}
// Send a raw request and read a bounded reply. Returns bytes read, or -1 if the
// connection couldn't even be made (counted as a possible server death).
static int hit(const void *req, int len, char *reply, int rcap) {
    int fd = dial();
    if (fd < 0) { g_conn_fail++; return -1; }
    send_all(fd, req, len);
    int n = reply ? drain(fd, reply, rcap) : 0;
    close(fd);
    g_ops++;
    return n;
}

// A short buffer of random-ish "hostile" text: control chars, quotes,
// backslashes, braces, high bytes — the stuff that breaks naive parsers.
static void hostile_str(unsigned *s, char *out, int n) {
    static const char pool[] = "\"'\\{}[]:,<>&%$#\n\r\t\0 ABxz09\x7f\xff\xfe/../";
    for (int i = 0; i < n; i++) out[i] = pool[ri(s, (int)sizeof pool)];
    out[n] = 0;
}

// -------- per-worker material (a real token + game, so authed/game/ws attacks
// have something valid to corrupt) --------
typedef struct { char token[128]; char game[32]; } Cred;

static void extract(const char *reply, const char *key, char *out, int cap) {
    out[0] = 0;
    const char *p = strstr(reply, key);
    if (!p) return;
    p += strlen(key);
    int i = 0; while (p[i] && p[i] != '"' && i < cap - 1) { out[i] = p[i]; i++; } out[i] = 0;
}
static void get_cred(unsigned *s, Cred *c) {
    char req[512], rep[8192];
    char uname[24]; snprintf(uname, sizeof uname, "fz%u_%u", rr(s), rr(s));
    int n = snprintf(req, sizeof req,
        "POST /auth/signup HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n{\"username\":\"%s\"}",
        (int)strlen(uname) + 15, uname);
    // build body length correctly
    char body[64]; int bl = snprintf(body, sizeof body, "{\"username\":\"%s\"}", uname);
    n = snprintf(req, sizeof req,
        "POST /auth/signup HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n%s", bl, body);
    int r = hit(req, n, rep, sizeof rep);
    c->token[0] = c->game[0] = 0;
    if (r > 0) extract(rep, "\"token\":\"", c->token, sizeof c->token);
    if (!c->token[0]) return;
    // create a game
    n = snprintf(req, sizeof req,
        "POST /create HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", c->token);
    r = hit(req, n, rep, sizeof rep);
    if (r > 0) extract(rep, "\"game_id\":\"", c->game, sizeof c->game);
}

// ============ ATTACKS ============
static char g_rep[65536];   // per-thread via TLS below
static _Thread_local char t_rep[65536];

// 1. Malformed HTTP: garbage lines, absurd Content-Length, huge path, no CRLF.
static void atk_http(unsigned *s) {
    char req[9000]; int n;
    switch (ri(s, 8)) {
        case 0: { rbytes(s, (uint8_t *)req, sizeof req); n = sizeof req; break; }              // pure garbage
        case 1: n = snprintf(req, sizeof req, "POST /create HTTP/1.1\r\nContent-Length: 999999999\r\n\r\nx"); break; // lying CL
        case 2: n = snprintf(req, sizeof req, "POST /create HTTP/1.1\r\nContent-Length: -5\r\n\r\n"); break;         // negative CL
        case 3: { n = snprintf(req, sizeof req, "GET /"); for (int i = 0; i < 8000 && n < (int)sizeof req - 2; i++) req[n++] = 'A'; n += snprintf(req + n, sizeof req - n, " HTTP/1.1\r\n\r\n"); break; } // huge path
        case 4: n = snprintf(req, sizeof req, "%.*sZZZZ", 100, "PPPPPPPPP"); break;              // junk method, no CRLF
        case 5: n = snprintf(req, sizeof req, "POST /auth/signup HTTP/1.1\r\nContent-Length: 100\r\n\r\n{\"username\":\"a\"}"); break; // CL > body
        case 6: { n = 0; for (int i = 0; i < 500 && n < (int)sizeof req - 20; i++) n += snprintf(req + n, sizeof req - n, "X-H%d: v\r\n", i); n += snprintf(req + n, sizeof req - n, "\r\n"); break; } // header flood
        default: { n = snprintf(req, sizeof req, "POST /action?game_id=X HTTP/1.1\r\n\r\n"); req[21] = 0; req[22] = (char)0xff; break; } // null byte in query
    }
    if (n > (int)sizeof req) n = (int)sizeof req;
    hit(req, n, t_rep, sizeof t_rep);
}

// 2. Hostile signups: empty/huge/binary usernames, non-JSON, JSON injection.
static void atk_signup(unsigned *s) {
    char body[9000], req[9200]; int bl;
    switch (ri(s, 7)) {
        case 0: bl = snprintf(body, sizeof body, "{\"username\":\"\"}"); break;
        case 1: { char h[512]; hostile_str(s, h, 200); bl = snprintf(body, sizeof body, "{\"username\":\"%s\"}", h); break; }
        case 2: { bl = snprintf(body, sizeof body, "{\"username\":\""); for (; bl < 8000; bl++) body[bl] = 'A'; bl += snprintf(body + bl, sizeof body - bl, "\"}"); break; } // 8k username
        case 3: bl = snprintf(body, sizeof body, "not json at all %u", rr(s)); break;
        case 4: bl = snprintf(body, sizeof body, "{\"username\":\"x\",\"admin\":true,\"user_id\":\"root\"}"); break; // injection
        case 5: bl = snprintf(body, sizeof body, "{\"username\":"); break;                        // truncated json
        default: { rbytes(s, (uint8_t *)body, 300); bl = 300; break; }                            // binary body
    }
    if (bl > (int)sizeof body) bl = (int)sizeof body;
    const char *path = ri(s, 2) ? "/auth/signup" : "/auth/signin";
    int n = snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", path, bl);
    if (n + bl <= (int)sizeof req) { memcpy(req + n, body, (size_t)bl); n += bl; }
    hit(req, n, t_rep, sizeof t_rep);
}

// 3. Forged/garbage Bearer tokens on every authed endpoint.
static void atk_token(unsigned *s) {
    char tok[256];
    switch (ri(s, 6)) {
        case 0: tok[0] = 0; break;                                                     // empty
        case 1: { int m = 1 + ri(s, 200); for (int i = 0; i < m; i++) tok[i] = "ABCDEF0123456789-_/=+ .\t"[ri(s, 24)]; tok[m] = 0; break; } // random base64ish
        case 2: memset(tok, 'A', 200), tok[200] = 0; break;                            // huge
        case 3: snprintf(tok, sizeof tok, "%%%%%%%%../../etc/passwd"); break;          // path-ish junk
        default: rbytes(s, (uint8_t *)tok, 60); tok[60] = 0; for (int i=0;i<60;i++) if(!tok[i]) tok[i]='?'; break; // binary
    }
    const char *ep[] = { "POST /create", "POST /meta", "POST /action?game_id=abc", "GET /state?game_id=abc&seat=0" };
    const char *e = ep[ri(s, 4)];
    char req[512];
    int n = snprintf(req, sizeof req, "%s HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", e, tok);
    hit(req, n, t_rep, sizeof t_rep);
}

// 4. Meta abuse with a VALID token: spam bots (overflow seats), start/join junk.
static void atk_meta(unsigned *s, Cred *c) {
    if (!c->token[0]) return;
    char body[256], req[512];
    const char *g = c->game[0] ? c->game : "deadbeef0000";
    int bl;
    switch (ri(s, 6)) {
        case 0: bl = snprintf(body, sizeof body, "{\"type\":\"add-bot\",\"game_id\":\"%s\",\"strategy\":\"cordite\"}", g); break; // spam bots -> seat overflow
        case 1: bl = snprintf(body, sizeof body, "{\"type\":\"join\",\"game_id\":\"%s\"}", g); break;
        case 2: bl = snprintf(body, sizeof body, "{\"type\":\"start\",\"game_id\":\"nonexistent%u\"}", rr(s)); break;
        case 3: { char h[64]; hostile_str(s, h, 30); bl = snprintf(body, sizeof body, "{\"type\":\"%s\",\"game_id\":\"%s\"}", h, g); break; } // junk type
        case 4: bl = snprintf(body, sizeof body, "{\"type\":\"add-bot\",\"game_id\":\"%s\",\"strategy\":\"../../nope\"}", g); break; // bad strategy
        default: bl = snprintf(body, sizeof body, "{\"game_id\":\"%s\"}", g); break;    // missing type
    }
    int n = snprintf(req, sizeof req, "POST /meta HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n%s", c->token, bl, body);
    hit(req, n, t_rep, sizeof t_rep);
}

// 5. Unparseable binary /action bodies (the awire move decoder's hostile input).
static void atk_action(unsigned *s, Cred *c) {
    if (!c->token[0]) return;
    uint8_t body[512]; int bl = ri(s, (int)sizeof body);
    rbytes(s, body, bl);
    const char *g = c->game[0] ? c->game : "deadbeef0000";
    int seatvals[] = { 0, 1, -1, -2, 99, 2147483647 };
    char req[1024];
    int n = snprintf(req, sizeof req, "POST /action?game_id=%s&seat=%d HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n",
                     g, seatvals[ri(s, 6)], c->token, bl);
    if (n + bl <= (int)sizeof req) { memcpy(req + n, body, (size_t)bl); n += bl; }
    hit(req, n, t_rep, sizeof t_rep);
}

// 6. Hostile /state seat values (the VIEW_UNMASKED disclosure + overflow).
static void atk_state(unsigned *s, Cred *c) {
    const char *g = c->game[0] ? c->game : "deadbeef0000";
    const char *seats[] = { "-2", "-1", "-999999999", "2147483648", "999999", "abc", "", "0x10", "-0" };
    char req[512];
    int n = snprintf(req, sizeof req, "GET /state?game_id=%s&seat=%s HTTP/1.1\r\nHost: x\r\n\r\n", g, seats[ri(s, 9)]);
    int r = hit(req, n, t_rep, sizeof t_rep);
    // Disclosure check: seat=-2 must NEVER return a large (full-state) body.
    if (r > 0 && strstr(req, "seat=-2") && strstr(t_rep, "200 OK") && r > 200) g_anomaly++;
}

// 7. Malformed WebSocket: bad handshakes, then (if it upgrades) garbage frames.
static void atk_ws(unsigned *s, Cred *c) {
    char req[1024], key[32]; uint8_t k[16]; rbytes(s, k, 16); b64(k, 16, key);
    const char *g = c->game[0] ? c->game : "deadbeef0000";
    int mode = ri(s, 5);
    int fd = dial();
    if (fd < 0) { g_conn_fail++; return; }
    if (mode == 0) {   // missing key
        int n = snprintf(req, sizeof req, "GET /ws?game_id=%s&seat=0 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nAuthorization: Bearer %s\r\n\r\n", g, c->token);
        send_all(fd, req, n);
    } else if (mode == 1) { // huge/garbage key
        char hk[512]; memset(hk, 'A', 500); hk[500] = 0;
        int n = snprintf(req, sizeof req, "GET /ws?game_id=%s&seat=0 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nAuthorization: Bearer %s\r\n\r\n", g, hk, c->token);
        send_all(fd, req, n);
    } else {   // valid-ish handshake, then blast garbage frames at the parser
        int n = snprintf(req, sizeof req, "GET /ws?game_id=%s&seat=0 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer %s\r\n\r\n", g, key, c->token[0] ? c->token : "x");
        send_all(fd, req, n);
        char tmp[512]; struct timeval tv = { .tv_sec = 1 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t eat = read(fd, tmp, sizeof tmp); (void)eat;   // eat the 101 (or error)
        // Frame fuzzing: bogus opcodes, lengths that lie, unmasked frames, 64-bit lengths.
        uint8_t f[300];
        for (int rep = 0; rep < 3; rep++) {
            int fl = 2 + ri(s, 200);
            rbytes(s, f, fl);
            if (ri(s, 2)) { f[0] = (uint8_t)(0x80 | ri(s, 16)); f[1] = (uint8_t)(0x80 | 126); }  // masked, 126 => 16-bit len follows (claims big)
            else            { f[0] = 0x82; f[1] = 127; }                                              // 64-bit length (claims huge)
            send_all(fd, f, fl);
        }
    }
    char tmp[256]; (void)drain(fd, tmp, sizeof tmp);
    close(fd);
    g_ops++;
}

// 8. Wrong methods + non-existent endpoints: DELETE /account (there is no such
//    route — "delete an account that doesn't exist"), PUT/PATCH on real paths,
//    OPTIONS/TRACE, deep unknown paths. Every one must 404/close cleanly, never
//    crash the router or the method dispatch.
static void atk_route(unsigned *s, Cred *c) {
    static const char *methods[] = { "DELETE", "PUT", "PATCH", "TRACE", "OPTIONS", "HEAD", "CONNECT", "FROB" };
    static const char *paths[] = {
        "/account", "/account/nonexistent", "/auth/signup", "/create", "/user/../../etc/passwd",
        "/ws", "/state", "/metrics", "/a/b/c/d/e/f", "/auth/delete", "/admin", "/",
    };
    char req[512];
    const char *m = methods[ri(s, 8)], *p = paths[ri(s, 12)];
    int n = snprintf(req, sizeof req,
        "%s %s HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n",
        m, p, c->token[0] ? c->token : "x");
    hit(req, n, t_rep, sizeof t_rep);
}

static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg ^ (unsigned)time(NULL);
    Cred cred; cred.token[0] = cred.game[0] = 0;
    int since_cred = 0;
    while (!g_stop) {
        if (!cred.token[0] || since_cred++ > 200) { get_cred(&seed, &cred); since_cred = 0; }
        switch (ri(&seed, 9)) {
            case 0: atk_http(&seed); break;
            case 1: atk_signup(&seed); break;
            case 2: atk_token(&seed); break;
            case 3: atk_meta(&seed, &cred); break;
            case 4: atk_action(&seed, &cred); break;
            case 5: atk_state(&seed, &cred); break;
            case 6: atk_ws(&seed, &cred); break;
            case 7: atk_route(&seed, &cred); break;
            default: atk_signup(&seed); break;   // extra signup pressure (table growth + rate limit)
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s host port threads seconds\n", argv[0]); return 2; }
    g_host = argv[1]; g_port = atoi(argv[2]);
    int nthreads = atoi(argv[3]), secs = atoi(argv[4]);
    (void)g_rep;
    signal(SIGPIPE, SIG_IGN);   // a server-closed socket must not kill the fuzzer

    pthread_t *th = calloc((size_t)nthreads, sizeof *th);
    for (long i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, worker, (void *)(i + 1));
    sleep(secs);
    g_stop = 1;
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);

    printf("fuzz done: ops=%ld connect_failures=%ld disclosure_anomalies=%ld (%d threads, %ds)\n",
           (long)g_ops, (long)g_conn_fail, (long)g_anomaly, nthreads, secs);
    if (g_anomaly) printf("!! DISCLOSURE ANOMALY: /state seat=-2 returned a large 200 body — full-state leak!\n");
    return 0;
}

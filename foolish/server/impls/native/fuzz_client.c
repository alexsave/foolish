// fuzz_client.c — an ADVERSARIAL load client. Where foolish_hammer plays valid
// games, this one tries to break the server: malformed HTTP, junk/oversized
// signups, forged/garbage Bearer tokens, meta abuse (spamming bots to overflow
// seats, starting games that don't exist), unparseable binary /action bodies,
// hostile /state seat values (the VIEW_UNMASKED disclosure), malformed
// WebSocket handshakes + frames (bogus lengths, opcodes, unmasked frames), and
// — against a live, dealt game — well-formed but ILLEGAL moves: structurally
// valid awire frames that pass the decoder and reach awire_apply (the legality
// engine) carrying cards you don't hold / wrong-phase kinds / out-of-turn plays.
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

#include "ctl_wire.h"   // the control plane is packed bytes now - this client builds it, then breaks it

static const char *g_host = "127.0.0.1";
static int g_port = 8099;
static volatile int g_stop = 0;
static _Atomic long g_ops = 0, g_conn_fail = 0, g_anomaly = 0;
// Illegal-move fuzzing (atk_move): how many well-formed frames reached a live
// (PLAYING) game — i.e. actually exercised awire_apply — and how many of those
// the engine reported applied. Accepts aren't necessarily bugs (a random frame
// can be a legal PASS/PICKUP, or a card that happens to be in hand on your
// turn); the number is here for visibility that the legality engine is hit.
static _Atomic long g_move_tests = 0, g_move_accepts = 0;

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

// Per-thread reply scratch (declared up here so the setup helpers below, not
// just the attacks, can drain into it).
static char g_rep[65536];   // unused shared buffer, referenced once to silence -Wunused
static _Thread_local char t_rep[65536];

// -------- per-worker material (a real token + game, so authed/game/ws attacks
// have something valid to corrupt) --------
typedef struct { char token[128]; char game[32]; } Cred;

// Locate the response BODY inside a drained reply. The control plane answers
// packed frames now, so the body is binary and may hold NUL bytes - it can
// never be found with strstr, and its length has to come back as a number.
// Returns NULL if the reply has no header terminator.
static const unsigned char *reply_body(const char *reply, int n, int *out_len) {
    for (int i = 0; i + 3 < n; i++)
        if (reply[i] == '\r' && reply[i+1] == '\n' && reply[i+2] == '\r' && reply[i+3] == '\n') {
            *out_len = n - (i + 4);
            return (const unsigned char *)reply + i + 4;
        }
    return NULL;
}

// Build "POST <path> HTTP/1.1" with a BINARY body of exactly bl bytes. Every
// control-plane request the fuzzer sends - well-formed or hostile - goes
// through here, because a packed frame cannot be pasted into a format string
// the way a JSON body could.
static int build_post(char *req, int cap, const char *path, const char *token,
                      const void *body, int bl) {
    int n;
    if (token && token[0])
        n = snprintf(req, (size_t)cap, "POST %s HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n", path, token, bl);
    else
        n = snprintf(req, (size_t)cap, "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", path, bl);
    if (n < 0 || n >= cap) return -1;
    if (bl > 0) {
        if (n + bl > cap) return -1;
        memcpy(req + n, body, (size_t)bl);
        n += bl;
    }
    return n;
}

static void get_cred(unsigned *s, Cred *c) {
    char req[512], rep[8192];
    char uname[24]; snprintf(uname, sizeof uname, "fz%u_%u", rr(s), rr(s));
    unsigned char body[CTL_FRAME_MAX];
    int bl = ctl_enc_auth(uname, body, sizeof body);
    c->token[0] = c->game[0] = 0;
    if (bl < 0) return;
    int n = build_post(req, sizeof req, "/auth/signup", NULL, body, bl);
    if (n < 0) return;
    int r = hit(req, n, rep, sizeof rep);
    int blen = 0;
    const unsigned char *b = r > 0 ? reply_body(rep, r, &blen) : NULL;
    CtlSession sess;
    if (b && ctl_dec_session(b, blen, &sess)) snprintf(c->token, sizeof c->token, "%s", sess.token);
    if (!c->token[0]) return;
    // create a game
    n = snprintf(req, sizeof req,
        "POST /create HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", c->token);
    r = hit(req, n, rep, sizeof rep);
    b = r > 0 ? reply_body(rep, r, &blen) : NULL;
    if (b) ctl_dec_game(b, blen, c->game, sizeof c->game);
}

// Drive this worker's game to GAME_STATUS_PLAYING so the move fuzzer's frames
// actually reach awire_apply (the legality engine) instead of bouncing off the
// "not playing" guard: add a bot (always ready, fills a seat) then start (the
// creator readies -> the kernel deals, 2 seated). The creator stays a human
// seat, so /action with this token has a real seat_of() >= 0.
static void start_game(unsigned *s, Cred *c) {
    (void)s;
    if (!c->token[0] || !c->game[0]) return;
    unsigned char body[CTL_FRAME_MAX];
    char req[512];
    int bl = ctl_enc_meta(CTL_META_ADD_BOT, c->game, "random", body, sizeof body);
    int n = build_post(req, sizeof req, "/meta", c->token, body, bl);
    if (n > 0) hit(req, n, t_rep, sizeof t_rep);
    bl = ctl_enc_meta(CTL_META_START, c->game, "", body, sizeof body);
    n  = build_post(req, sizeof req, "/meta", c->token, body, bl);
    if (n > 0) hit(req, n, t_rep, sizeof t_rep);
}

// ============ ATTACKS ============

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

// 2. Hostile signups, as malformed CONTROL WIRE (ctl_wire.h). Same intent the
//    JSON version had - empty/huge/binary usernames, a body that is not the
//    format at all, extra fields smuggled in, a truncated body - expressed in
//    the shape the server now parses. The packed wire's failure modes are
//    different from a scraper's and that is exactly why they are worth firing
//    at: a length prefix that LIES about how much follows, a frame that claims
//    a payload it does not carry, a kind byte nobody serves.
static void atk_signup(unsigned *s) {
    uint8_t body[9000]; char req[9200]; int bl;
    switch (ri(s, 8)) {
        case 0:   // well-formed frame, empty username
            bl = ctl_enc_auth("", body, sizeof body); break;
        case 1: { // well-formed frame, control chars / quotes / high bytes in the name
            char h[512]; hostile_str(s, h, 200);
            bl = ctl_enc_auth(h, body, sizeof body); break; }
        case 2: { // an 8k username behind a length prefix that says 255: the
                  // frame LIES about its own size in both directions at once
            bl = 0;
            body[bl++] = CTL_WIRE_VERSION; body[bl++] = CTL_AUTH;
            body[bl++] = 0xff; body[bl++] = 0x1f;          // claims 8191 payload bytes
            body[bl++] = 255;                               // string claims 255
            for (; bl < 8000; bl++) body[bl] = 'A';         // carries 7995
            break; }
        case 3: { // not a control frame at all
            bl = snprintf((char *)body, sizeof body, "not a frame at all %u", rr(s)); break; }
        case 4: { // a valid CTL_AUTH frame with EXTRA fields smuggled past the
                  // declared payload - the packed-wire analogue of the JSON
                  // injection case ({"username":"x","admin":true,...})
            bl = ctl_enc_auth("x", body, sizeof body);
            if (bl > 0 && bl + 8 < (int)sizeof body) {
                body[bl++] = 4; memcpy(body + bl, "root", 4); bl += 4;
                body[bl++] = 1;                             // a trailing "admin" byte
            }
            break; }
        case 5:   // the head only: a frame that promises a payload and stops
            body[0] = CTL_WIRE_VERSION; body[1] = CTL_AUTH; body[2] = 40; body[3] = 0; bl = 4; break;
        case 6:   // a kind byte no endpoint serves, with a plausible payload
            bl = ctl_enc_auth("x", body, sizeof body);
            if (bl > 1) body[1] = (uint8_t)ri(s, 256);
            break;
        default: { rbytes(s, body, 300); bl = 300; break; }  // binary body
    }
    if (bl < 0) bl = 0;
    if (bl > (int)sizeof body) bl = (int)sizeof body;
    const char *path = ri(s, 2) ? "/auth/signup" : "/auth/signin";
    int n = build_post(req, sizeof req, path, NULL, body, bl);
    if (n > 0) hit(req, n, t_rep, sizeof t_rep);
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

// 4. Meta abuse with a VALID token, as control wire: spam bots (overflow
//    seats), start/join junk, a verb byte nobody serves, a frame whose game_id
//    runs off its own payload.
static void atk_meta(unsigned *s, Cred *c) {
    if (!c->token[0]) return;
    uint8_t body[256]; char req[512];
    const char *g = c->game[0] ? c->game : "deadbeef0000";
    int bl;
    switch (ri(s, 7)) {
        case 0: bl = ctl_enc_meta(CTL_META_ADD_BOT, g, "cordite", body, sizeof body); break;   // spam bots -> seat overflow
        case 1: bl = ctl_enc_meta(CTL_META_JOIN, g, "", body, sizeof body); break;
        case 2: { char bogus[32]; snprintf(bogus, sizeof bogus, "nonexistent%u", rr(s));
                  bl = ctl_enc_meta(CTL_META_START, bogus, "", body, sizeof body); break; }
        case 3: bl = ctl_enc_meta(ri(s, 256), g, "", body, sizeof body); break;                 // junk verb byte
        case 4: bl = ctl_enc_meta(CTL_META_ADD_BOT, g, "../../nope", body, sizeof body); break; // bad strategy
        case 5: { // a game_id length prefix that runs past the declared payload
            bl = ctl_enc_meta(CTL_META_JOIN, g, "", body, sizeof body);
            if (bl > 5) body[5] = 200;
            break; }
        default: bl = ctl_enc_meta(0, "", "", body, sizeof body); break;   // no verb, no game (the "missing type" case)
    }
    if (bl < 0) bl = 0;
    int n = build_post(req, sizeof req, "/meta", c->token, body, bl);
    if (n > 0) hit(req, n, t_rep, sizeof t_rep);
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

// 9. Well-formed but ILLEGAL moves (the point of the whole exercise). Unlike
//    atk_action's random bytes — which almost never survive awire_decode's
//    strict (valid kind, n<=28, EXACT length) check and so bounce off the
//    DECODER — this builds a STRUCTURALLY VALID awire frame so it passes the
//    decoder and reaches awire_apply, the game-rule LEGALITY engine, with
//    hostile content: random card ids (cards you don't hold), a kind that's
//    wrong for the phase, plays when it isn't your turn. Per awire.h the kernel
//    must clamp hostile ids to real cards and reject them (ok:false) rather than
//    corrupt state or crash — exactly what ASan/UBSan is watching for here.
static void atk_move(unsigned *s, Cred *c) {
    if (!c->token[0]) return;
    if (!c->game[0]) { get_cred(s, c); start_game(s, c); if (!c->game[0]) return; }

    uint8_t body[64];
    int kind = ri(s, 5);                                    // 0..4: ATTACK COVER PASS PICKUP GOOD
    int n    = (kind == 3 || kind == 4) ? 0 : ri(s, 29);    // PICKUP/GOOD must carry n==0, else 0..28
    int bl   = 2 + n * (kind == 1 ? 2 : 1);                 // COVER frames carry 2n card bytes
    if (bl > (int)sizeof body) { kind = 0; n = 1; bl = 3; }
    body[0] = (uint8_t)kind;
    body[1] = (uint8_t)n;
    for (int i = 2; i < bl; i++) body[i] = (uint8_t)rr(s);  // hostile wire-card ids

    char req[256];
    int hn = snprintf(req, sizeof req,
        "POST /action?game_id=%s HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n",
        c->game, c->token, bl);
    if (hn + bl > (int)sizeof req) return;
    memcpy(req + hn, body, (size_t)bl); hn += bl;
    int r = hit(req, hn, t_rep, sizeof t_rep);

    if (r > 0) {
        // The reply is a packed frame, so read it as one: a CTL_ERROR carries
        // the refusal as a byte and a CTL_APPLIED means the frame got all the
        // way to awire_apply. This is the one place the fuzzer parses the
        // server strictly - everywhere else it just throws bytes and counts.
        int blen = 0;
        const unsigned char *b = reply_body(t_rep, r, &blen);
        int reason = 0;
        bool applied = false;
        if (b && ctl_dec_error(b, blen, &reason) &&
            (reason == CTL_ERR_NOT_PLAYING || reason == CTL_ERR_NOT_SEATED)) {
            // game ended (bot loop ran it out) - mint a fresh PLAYING one
            get_cred(s, c); start_game(s, c);
        } else if (b && ctl_dec_applied(b, blen, &applied, NULL)) {
            g_move_tests++;                                 // frame reached awire_apply
            if (applied) g_move_accepts++;
        }
    }
}

static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg ^ (unsigned)time(NULL);
    Cred cred; cred.token[0] = cred.game[0] = 0;
    int since_cred = 0;
    while (!g_stop) {
        if (!cred.token[0] || since_cred++ > 200) { get_cred(&seed, &cred); start_game(&seed, &cred); since_cred = 0; }
        switch (ri(&seed, 10)) {
            case 0: atk_http(&seed); break;
            case 1: atk_signup(&seed); break;
            case 2: atk_token(&seed); break;
            case 3: atk_meta(&seed, &cred); break;
            case 4: atk_action(&seed, &cred); break;
            case 5: atk_state(&seed, &cred); break;
            case 6: atk_ws(&seed, &cred); break;
            case 7: atk_route(&seed, &cred); break;
            case 8: atk_move(&seed, &cred); break;   // well-formed ILLEGAL moves -> awire_apply legality engine
            default: atk_move(&seed, &cred); break;   // extra legality-engine pressure (2/10 weight)
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
    printf("  illegal-move frames that reached awire_apply: %ld (engine reported applied: %ld)\n",
           (long)g_move_tests, (long)g_move_accepts);
    if (g_anomaly) printf("!! DISCLOSURE ANOMALY: /state seat=-2 returned a large 200 body — full-state leak!\n");
    return 0;
}

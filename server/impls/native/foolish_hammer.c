// foolish_hammer.c — a native C load-test client for foolish_server.
//
// Acts as many CONCURRENT HUMAN clients (raw TCP sockets, hand-rolled
// HTTP/1.1, no libcurl) to overload the server. There are NO bots on the
// server side — every seat in every game is a human this program controls,
// via /meta join (never /meta add-bot) — so every unit of action volume is
// client-driven and the server's CPU is pure request-handling + kernel
// apply, with zero Monte-Carlo bot brain running server-side. This isolates
// the "server hit path" from the bot-thinking path (see PROFILE_HOTPATH.md,
// target T1).
//
// Links the SAME kernel sources as foolish_server (see Makefile's
// KERNEL_SRC) so it can build real awire move frames and, in --mode=mixed
// or --mode=ws, decode a real masked /state view and compute real legal
// moves — but it is a wholly separate process that only ever talks to the
// server over the loopback socket, exactly like a browser or phone client
// would.
//
// CLI (all optional, sensible defaults):
//   --host=127.0.0.1 --port=8099 --games=20 --seats=4 --conns=32 --secs=15
//   --mode=action|mixed|ws
//
// Setup phase (all three modes): signs up games*seats distinct users, POSTs
// /create for each game (creator = seat 0), joins the rest via /meta join,
// then /meta start for every seated human so each game deals (status ->
// PLAYING). NEVER calls /meta add-bot — every seat is a human this program
// drives, in every mode.
//
// --mode=action (the default) / --mode=mixed: `conns` threads hammer the
// server for `secs` seconds, one fresh HTTP connection per request (the
// server always sends "Connection: close"), with a mix of
//   - POST /action?game_id=..   random-but-well-FRAMED awire bytes (most are
//     rejected as illegal moves — that still drives the full decode +
//     validate + apply hit path);
//   - GET  /state?game_id=..&seat=..  and GET /status?game_id=..;
//   - occasionally POST /create + join + start a fresh 2-human game (grows
//     load over the run).
// --mode=mixed additionally (BEST EFFORT ONLY — never blocks delivery): for
// a slice of action requests, decodes a real /state view and submits a
// genuinely LEGAL move so games actually progress, instead of just cycling
// through illegal-move rejections forever.
//
// --mode=ws: one persistent WebSocket connection PER SEAT (games*seats
// connections total — `--conns` is ignored in this mode; see ws_run_load
// below for why "one connection per client" doesn't map onto a `conns`
// concurrency knob the way stateless HTTP requests do). Each connection's
// worker thread loops: receive the server's pushed [ok][masked state] ->
// state_get -> calculate_legal_moves for its own seat -> if it has a legal
// move, awire_encode a RANDOMLY CHOSEN one and send it; if not (this seat
// isn't currently eligible — Durak often has several eligible seats at
// once, e.g. multiple attackers + one defender, and just as often seats
// that aren't), send an empty poll frame instead so it still notices when
// another seat's move makes it eligible. On game-over, POST /meta continue
// + /meta start (a rematch, reusing the SAME game_id/seat/connection) so
// load keeps flowing instead of idling out. Every submitted move is
// genuinely legal, so the server's awire_apply actually applies it — no
// cheap-reject easy-out (PROFILE_HOTPATH.md T1's random-frame mode measured
// ~0.15% of submitted actions as legal; this mode is ~100%).
//
// Robustness: every parse step is bounds-checked against the bytes actually
// read/received, and no response — malformed, truncated, or a flat
// connection failure — can crash this program; it is just counted.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "legal.h"
#include "view.h"
#include "awire.h"
#include "cli_util.h"   // get_arg / parse_int — shared with the cnitro_* tools
#include "ws.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

typedef struct {
    char host[64];
    int  port;
    int  games;
    int  seats;
    int  conns;
    int  secs;
    bool mixed;
    bool ws;   // --mode=ws: persistent WebSocket connections + legal moves only
} Config;

static Config g_cfg;
static atomic_bool g_stop = false;

// How long a seat with no legal move waits before sending its next empty
// "poll" frame (the only way it learns another seat's move made it
// eligible — see ws_worker). Measured on this 4-core box: at 1ms, idle
// polling from ~120 of 160 seats (only ~1-2 seats per game are ever
// eligible at once in Durak) saturated all 4 cores in syscall/
// context-switch overhead (69% sys, load 4.0) and *reduced* real
// applied-moves/sec despite TCP_NODELAY making each individual round trip
// cheap. Widening the idle interval trades poll responsiveness for giving
// the CPU back to the round trips that actually apply a move.
#define WS_IDLE_POLL_US 1000

// Round-trip latency sampling (--mode=ws): how many samples EACH ws_worker
// thread keeps, via reservoir sampling (classic algorithm R) so an
// arbitrarily long/high-concurrency run reports accurate percentiles without
// unbounded memory — a run's total round trips (lat_seen) can vastly exceed
// this cap; the reservoir stays an unbiased random subsample of the whole
// run. Combined across e.g. 400 connections that's up to 1.64M doubles
// (~13 MB) for percentile computation — a load-test client concern, not a
// server one.
#define LAT_RESERVOIR_CAP 4096

// ---------------------------------------------------------------------------
// User / game pools. Append-only (initial fill during setup, occasional
// growth during the load phase), capped to match the server's own
// MAX_USERS/MAX_GAMES (foolish_server.c) so we never build more state than
// the server can hold — past the cap, /create and /auth/signup just start
// returning {"error":"full"} (400), which the load loop counts like any
// other status code, not a crash.
// ---------------------------------------------------------------------------

#define HAMMER_MAX_USERS 512   // == server MAX_USERS
#define HAMMER_MAX_GAMES 256   // == server MAX_GAMES

typedef struct {
    char token[40];
    char user_id[16];
} HUser;

typedef struct {
    char id[16];
    int  n_seats;
    int  user_idx[MAX_PLAYERS];   // seat i -> index into g_users[]
} HGame;

static HUser g_users[HAMMER_MAX_USERS];
static int   g_n_users = 0;
static pthread_mutex_t g_users_lock = PTHREAD_MUTEX_INITIALIZER;

static HGame g_games[HAMMER_MAX_GAMES];
static int   g_n_games = 0;
static pthread_mutex_t g_games_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic unsigned long g_grow_ctr = 0;

static double now_secs(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 client.
//
// One fresh connection per request (matches the server's own "Connection:
// close" contract). Header + body are assembled into ONE buffer and sent
// with a single write() so Nagle can't stall a two-write request/body
// split. The response is read to EOF (the server always closes when it is
// done sending), into a thread-owned buffer, then parsed defensively: every
// index is bounds-checked against the bytes actually received, so a short
// read, a dropped connection, or a garbled response degrades to
// status==0 / empty body rather than a crash.
// ---------------------------------------------------------------------------

#define RESP_CAP (1 << 17)   // 128 KiB: /state's packed view tops out well under this

typedef struct {
    int status;              // 0 = connect/send/no-status-line failure
    unsigned char *body;     // points INTO the caller's respbuf, or NULL
    int body_len;
} HttpResp;

static int connect_to(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return -1; }
    return fd;
}

static bool http_do(const char *host, int port, const char *method, const char *path,
                     const char *token, const unsigned char *body, int body_len,
                     unsigned char *respbuf, int respcap, HttpResp *resp) {
    resp->status = 0; resp->body = NULL; resp->body_len = 0;
    int fd = connect_to(host, port);
    if (fd < 0) return false;

    char hdr[512];
    int hn;
    if (token && body_len > 0)
        hn = snprintf(hdr, sizeof hdr,
            "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
            "Connection: close\r\nContent-Length: %d\r\n\r\n", method, path, host, token, body_len);
    else if (token)
        hn = snprintf(hdr, sizeof hdr,
            "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nConnection: close\r\n\r\n",
            method, path, host, token);
    else if (body_len > 0)
        hn = snprintf(hdr, sizeof hdr,
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %d\r\n\r\n",
            method, path, host, body_len);
    else
        hn = snprintf(hdr, sizeof hdr, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            method, path, host);
    if (hn < 0 || hn >= (int)sizeof hdr) { close(fd); return false; }

    unsigned char req[4096];
    if ((size_t)hn > sizeof req) { close(fd); return false; }
    memcpy(req, hdr, (size_t)hn);
    int total_out = hn;
    if (body_len > 0) {
        if (hn + body_len > (int)sizeof req) { close(fd); return false; }
        memcpy(req + hn, body, (size_t)body_len);
        total_out = hn + body_len;
    }
    int off = 0;
    while (off < total_out) {
        ssize_t w = write(fd, req + off, (size_t)(total_out - off));
        if (w < 0) { if (errno == EINTR) continue; close(fd); return false; }
        if (w == 0) break;
        off += (int)w;
    }
    shutdown(fd, SHUT_WR);

    int total_in = 0;
    for (;;) {
        if (total_in >= respcap) break;
        ssize_t n = read(fd, respbuf + total_in, (size_t)(respcap - total_in));
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        total_in += (int)n;
    }
    close(fd);

    if (total_in < 12) return true;   // too short to hold a status line; status stays 0
    unsigned char *sp1 = memchr(respbuf, ' ', (size_t)total_in);
    if (!sp1 || sp1 + 1 >= respbuf + total_in) return true;
    resp->status = atoi((char *)sp1 + 1);
    for (int i = 0; i + 3 < total_in; i++) {
        if (respbuf[i] == '\r' && respbuf[i+1] == '\n' && respbuf[i+2] == '\r' && respbuf[i+3] == '\n') {
            resp->body = respbuf + i + 4;
            resp->body_len = total_in - (i + 4);
            break;
        }
    }
    return true;
}

// Bounded flat-JSON string scraper mirroring the server's own json_str
// (foolish_server.c) — the wire is the same minimal flat objects both ways.
// Bounds-checked against body_len (the body may be binary /state bytes with
// no NUL terminator), never reads past it.
static bool json_str(const unsigned char *body, int body_len, const char *key, char *out, int cap) {
    if (!body || body_len <= 0 || cap <= 0) return false;
    char pat[64];
    int pl = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (pl <= 0 || pl >= (int)sizeof pat) return false;
    for (int i = 0; i + pl <= body_len; i++) {
        if (memcmp(body + i, pat, (size_t)pl) != 0) continue;
        const unsigned char *p = body + i + pl;
        const unsigned char *end = body + body_len;
        while (p < end && *p != ':') p++;
        if (p >= end) return false;
        p++;
        while (p < end && (*p == ' ' || *p == '"')) p++;
        int n = 0;
        while (p < end && *p != '"' && *p != ',' && *p != '}' && n < cap - 1) out[n++] = (char)*p++;
        out[n] = 0;
        return n > 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Setup: signup / create / meta helpers (used both by the one-time setup
// pass and by the load-phase "grow a fresh game" path).
// ---------------------------------------------------------------------------

static bool do_signup(const Config *cfg, const char *username, HUser *out) {
    char body[128];
    int bn = snprintf(body, sizeof body, "{\"username\":\"%s\"}", username);
    unsigned char resp[RESP_CAP]; HttpResp r;
    if (!http_do(cfg->host, cfg->port, "POST", "/auth/signup", NULL, (unsigned char *)body, bn, resp, sizeof resp, &r))
        return false;
    if (r.status != 200) return false;
    char tok[40], uid[16];
    if (!json_str(r.body, r.body_len, "token", tok, sizeof tok)) return false;
    if (!json_str(r.body, r.body_len, "user_id", uid, sizeof uid)) return false;
    snprintf(out->token, sizeof out->token, "%s", tok);
    snprintf(out->user_id, sizeof out->user_id, "%s", uid);
    return true;
}

static bool do_create(const Config *cfg, const char *token, char *gid_out, int cap) {
    unsigned char resp[RESP_CAP]; HttpResp r;
    if (!http_do(cfg->host, cfg->port, "POST", "/create", token, NULL, 0, resp, sizeof resp, &r)) return false;
    if (r.status != 200) return false;
    return json_str(r.body, r.body_len, "game_id", gid_out, cap);
}

static bool do_meta(const Config *cfg, const char *token, const char *type, const char *gid) {
    char body[128];
    int bn = snprintf(body, sizeof body, "{\"type\":\"%s\",\"game_id\":\"%s\"}", type, gid);
    unsigned char resp[RESP_CAP]; HttpResp r;
    if (!http_do(cfg->host, cfg->port, "POST", "/meta", token, (unsigned char *)body, bn, resp, sizeof resp, &r))
        return false;
    return r.status == 200;
}

// One-time setup: sign up games*seats users, create+join+start each game so
// it deals. Sequential and simple — a few hundred local requests, trivial
// next to the timed load phase that follows. NEVER calls /meta add-bot: the
// whole point of this client is that every seat is a human it controls.
static void setup(const Config *cfg) {
    int need = cfg->games * cfg->seats;
    printf("== setup: signing up up to %d users, dealing up to %d games x %d seats ==\n",
           need, cfg->games, cfg->seats);
    for (int i = 0; i < need && g_n_users < HAMMER_MAX_USERS; i++) {
        char uname[40];
        snprintf(uname, sizeof uname, "hmr_%lx_%d_%d", (unsigned long)time(NULL), (int)getpid(), i);
        HUser u;
        if (do_signup(cfg, uname, &u)) g_users[g_n_users++] = u;
        else fprintf(stderr, "  signup failed for %s\n", uname);
    }
    printf("   signed up %d users\n", g_n_users);

    int uptr = 0;
    for (int gi = 0; gi < cfg->games; gi++) {
        if (uptr + cfg->seats > g_n_users) break;
        char gid[16];
        if (!do_create(cfg, g_users[uptr].token, gid, sizeof gid)) { uptr += cfg->seats; continue; }
        for (int s = 1; s < cfg->seats; s++) do_meta(cfg, g_users[uptr + s].token, "join", gid);
        for (int s = 0; s < cfg->seats; s++) do_meta(cfg, g_users[uptr + s].token, "start", gid);

        HGame hg; memset(&hg, 0, sizeof hg);
        snprintf(hg.id, sizeof hg.id, "%s", gid);
        hg.n_seats = cfg->seats;
        for (int s = 0; s < cfg->seats; s++) hg.user_idx[s] = uptr + s;
        g_games[g_n_games++] = hg;
        // Grep-able line (server/impls/native/crash_test.sh scrapes this to
        // find the exact game_id it should verify across a kill -9 restart —
        // /state and /status take no auth, so the id alone is enough).
        printf("   dealt game[%d]: id=%s\n", gi, hg.id);
        uptr += cfg->seats;
    }
    printf("   dealt %d games\n", g_n_games);
}

// Grows the load by spinning up ONE fresh 2-human game and publishing it
// into the shared pool, so later iterations of every thread can also target
// it. Called occasionally from the load loop; never blocks the caller for
// long (a handful of local requests) and any failure just aborts this one
// growth attempt — the caller keeps hammering regardless.
static void grow_one_game(const Config *cfg, unsigned int *seed) {
    (void)seed;
    pthread_mutex_lock(&g_users_lock);
    bool users_room = g_n_users + 2 <= HAMMER_MAX_USERS;
    pthread_mutex_unlock(&g_users_lock);
    pthread_mutex_lock(&g_games_lock);
    bool games_room = g_n_games < HAMMER_MAX_GAMES;
    pthread_mutex_unlock(&g_games_lock);
    if (!users_room || !games_room) return;

    unsigned long n = atomic_fetch_add(&g_grow_ctr, 1);
    char un1[40], un2[40];
    snprintf(un1, sizeof un1, "grow_%lx_%lu_a", (unsigned long)getpid(), n);
    snprintf(un2, sizeof un2, "grow_%lx_%lu_b", (unsigned long)getpid(), n);
    HUser u1, u2;
    if (!do_signup(cfg, un1, &u1)) return;
    if (!do_signup(cfg, un2, &u2)) return;
    char gid[16];
    if (!do_create(cfg, u1.token, gid, sizeof gid)) return;
    if (!do_meta(cfg, u2.token, "join", gid)) return;
    do_meta(cfg, u1.token, "start", gid);
    do_meta(cfg, u2.token, "start", gid);

    pthread_mutex_lock(&g_users_lock);
    int i1 = -1, i2 = -1;
    if (g_n_users < HAMMER_MAX_USERS) { i1 = g_n_users++; g_users[i1] = u1; }
    if (g_n_users < HAMMER_MAX_USERS) { i2 = g_n_users++; g_users[i2] = u2; }
    pthread_mutex_unlock(&g_users_lock);
    if (i1 < 0 || i2 < 0) return;

    pthread_mutex_lock(&g_games_lock);
    if (g_n_games < HAMMER_MAX_GAMES) {
        HGame hg; memset(&hg, 0, sizeof hg);
        snprintf(hg.id, sizeof hg.id, "%s", gid);
        hg.n_seats = 2; hg.user_idx[0] = i1; hg.user_idx[1] = i2;
        g_games[g_n_games++] = hg;
    }
    pthread_mutex_unlock(&g_games_lock);
}

// ---------------------------------------------------------------------------
// Move-frame builders.
// ---------------------------------------------------------------------------

// A random but WELL-FRAMED awire action: valid kind (0..4), a length-legal n
// for that kind, and card bytes drawn from the full 0..255 wire range (a raw
// byte reinterpretation of Card — sizeof(Card)==1 is asserted in card.h — so
// this reaches every wire_from_card branch: real card ids, the hidden
// sentinel, and everything in between). This is the default load: the frame
// always DECODES (awire_decode sees a well-formed header), so the request
// drives the full decode + validate + apply path, and then almost always
// gets rejected by the kernel's rule checks (not in hand, wrong turn, wrong
// value, ...) — exactly the "mostly illegal, still real work" hit path
// described in PROFILE_HOTPATH.md T1.
static int build_random_frame(unsigned char *buf, int cap, unsigned int *seed) {
    AwireAction a; memset(&a, 0, sizeof a);
    a.kind = (int)(rand_r(seed) % 5);
    if (a.kind == AWIRE_PICKUP || a.kind == AWIRE_GOOD) a.n = 0;
    else a.n = (int)(rand_r(seed) % (AWIRE_MAX_CARDS + 1));
    for (int i = 0; i < a.n; i++) {
        unsigned char rb = (unsigned char)(rand_r(seed) & 0xff);
        memcpy(&a.cards[i], &rb, 1);
        if (a.kind == AWIRE_COVER) {
            unsigned char rb2 = (unsigned char)(rand_r(seed) & 0xff);
            memcpy(&a.attacks[i], &rb2, 1);
        }
    }
    return awire_encode(&a, buf, cap);
}

// --mode=mixed bonus path: fetch this seat's real masked view, decode it
// with the SAME kernel (view.h state_get), enumerate real legal moves
// (legal.h calculate_legal_moves), and encode one of them for real. Returns
// 0 (never blocks/retries) on any failure — a bad HTTP round, a
// not-currently-playing game, or zero legal moves — so the caller always
// has the random-frame path as a fallback.
static int build_legal_frame(const Config *cfg, const char *gid, int seat,
                              unsigned char *buf, int cap, unsigned int *seed) {
    unsigned char resp[RESP_CAP]; HttpResp r;
    char path[64];
    snprintf(path, sizeof path, "/state?game_id=%s&seat=%d", gid, seat);
    if (!http_do(cfg->host, cfg->port, "GET", path, NULL, NULL, 0, resp, sizeof resp, &r)) return 0;
    if (r.status != 200 || !r.body || r.body_len <= 0) return 0;

    static __thread Game g;
    static __thread LegalMoves moves;
    memset(&g, 0, sizeof g);
    state_get(&g, r.body, /*masked=*/1);
    if (g.status != GAME_STATUS_PLAYING || seat < 0 || seat >= g.num_players) return 0;

    calculate_legal_moves(&g, seat, &moves);
    if (moves.n <= 0) return 0;
    const LegalMove *m = &moves.moves[rand_r(seed) % (unsigned)moves.n];

    AwireAction a; memset(&a, 0, sizeof a);
    switch (m->type) {
        case MOVE_ATTACK: a.kind = AWIRE_ATTACK; break;
        case MOVE_COVER:  a.kind = AWIRE_COVER;  break;
        case MOVE_PASS:   a.kind = AWIRE_PASS;   break;
        case MOVE_PICKUP: a.kind = AWIRE_PICKUP; break;
        case MOVE_GOOD:   a.kind = AWIRE_GOOD;   break;
        default: return 0;   // MOVE_WAIT or anything unrecognized
    }
    a.n = (a.kind == AWIRE_PICKUP || a.kind == AWIRE_GOOD) ? 0 : m->n_cards;
    if (a.n < 0) a.n = 0;
    if (a.n > AWIRE_MAX_CARDS) a.n = AWIRE_MAX_CARDS;
    for (int i = 0; i < a.n; i++) {
        a.cards[i] = m->cards[i];
        if (a.kind == AWIRE_COVER) a.attacks[i] = m->attack_cards[i];
    }
    return awire_encode(&a, buf, cap);
}

// ---------------------------------------------------------------------------
// Load-phase stats + worker loop.
// ---------------------------------------------------------------------------

typedef struct {
    long actions_sent, actions_ok;
    long state_gets, status_gets;
    long grow_attempts;
    long total_requests;
    long code_200, code_400, code_401, code_404, code_5xx, code_other, code_fail;
} ThreadStats;

static void record_status(ThreadStats *st, bool okc, int status) {
    if (!okc) { st->code_fail++; return; }
    switch (status) {
        case 200: st->code_200++; break;
        case 400: st->code_400++; break;
        case 401: st->code_401++; break;
        case 404: st->code_404++; break;
        default:
            if (status >= 500 && status < 600) st->code_5xx++;
            else st->code_other++;
    }
}

static void *loader_thread(void *arg) {
    ThreadStats *st = (ThreadStats *)arg;
    unsigned int seed = (unsigned int)((uintptr_t)pthread_self() ^ (uintptr_t)time(NULL) ^ (uintptr_t)st);
    unsigned char *respbuf = malloc(RESP_CAP);
    if (!respbuf) return NULL;

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        pthread_mutex_lock(&g_games_lock);
        int ng = g_n_games;
        pthread_mutex_unlock(&g_games_lock);
        if (ng == 0) { usleep(1000); continue; }

        int gi = (int)(rand_r(&seed) % (unsigned)ng);
        HGame hg = g_games[gi];             // safe: append-only, published under g_games_lock
        int seat = (int)(rand_r(&seed) % (unsigned)hg.n_seats);
        int uidx = hg.user_idx[seat];
        pthread_mutex_lock(&g_users_lock);
        HUser hu = g_users[uidx];
        pthread_mutex_unlock(&g_users_lock);

        unsigned long roll = rand_r(&seed) % 1000;
        HttpResp r;
        if (roll < 700) {
            unsigned char frame[64];
            int flen = 0;
            if (g_cfg.mixed && (rand_r(&seed) % 5 == 0))
                flen = build_legal_frame(&g_cfg, hg.id, seat, frame, sizeof frame, &seed);
            if (flen <= 0) flen = build_random_frame(frame, sizeof frame, &seed);
            char path[48];
            snprintf(path, sizeof path, "/action?game_id=%s", hg.id);
            bool okc = http_do(g_cfg.host, g_cfg.port, "POST", path, hu.token, frame, flen, respbuf, RESP_CAP, &r);
            st->total_requests++; st->actions_sent++;
            record_status(st, okc, r.status);
            if (okc && r.status == 200 && r.body) {
                char okf[8];
                if (json_str(r.body, r.body_len, "ok", okf, sizeof okf) && !strcmp(okf, "true")) st->actions_ok++;
            }
        } else if (roll < 900) {
            char path[64];
            snprintf(path, sizeof path, "/state?game_id=%s&seat=%d", hg.id, seat);
            bool okc = http_do(g_cfg.host, g_cfg.port, "GET", path, NULL, NULL, 0, respbuf, RESP_CAP, &r);
            st->total_requests++; st->state_gets++;
            record_status(st, okc, r.status);
        } else if (roll < 995) {
            char path[48];
            snprintf(path, sizeof path, "/status?game_id=%s", hg.id);
            bool okc = http_do(g_cfg.host, g_cfg.port, "GET", path, NULL, NULL, 0, respbuf, RESP_CAP, &r);
            st->total_requests++; st->status_gets++;
            record_status(st, okc, r.status);
        } else {
            grow_one_game(&g_cfg, &seed);
            st->grow_attempts++;
        }
    }
    free(respbuf);
    return NULL;
}

// ---------------------------------------------------------------------------
// --mode=ws: persistent WebSocket connections + genuinely legal moves.
//
// Unlike --mode=action/mixed (stateless HTTP, `conns` generic worker threads
// each firing at a random game+seat every iteration), a WebSocket client IS
// a specific (game, seat) for the life of its connection — that's the whole
// point (one persistent connection replaces one HTTP request per action).
// So here there is no pool of interchangeable workers to round-robin: we
// spawn exactly one thread per (game, seat) pair from the setup() pool
// (games*seats threads total) and each owns ONE long-lived WS connection for
// the whole run. `--conns` doesn't map onto that model, so it's ignored in
// this mode (logged once, not silently dropped).
// ---------------------------------------------------------------------------

typedef struct {
    long msgs_recv;          // [ok][state] pushes received
    long actions_sent;       // non-empty (real move) frames sent
    long actions_applied;    // of those, how many the server's awire_apply accepted
    long polls_sent;         // empty frames sent (seat not eligible / game not playing)
    long rematches;          // /meta continue+start pairs issued on game-over
    long connects;           // successful WS handshakes (incl. reconnects)
    long connect_failures;   // failed connects/handshakes (counted, never fatal)
    // Round-trip latency: moment this connection SENDS a move/poll frame ->
    // moment it receives the server's pushed [ok][state] answer. Network
    // latency is ~0 on loopback, so this is the SERVER-attributable latency
    // floor (request parse + lock + apply/serialize + response write) and
    // how it degrades as concurrency rises — see PROFILE_HOTPATH.md "T1c".
    double lat_us[LAT_RESERVOIR_CAP];   // reservoir of sampled round trips, microseconds
    long   lat_count;                   // samples currently held (<= LAT_RESERVOIR_CAP)
    long   lat_seen;                    // total round trips timed (for the reservoir's odds)
} WsThreadStats;

// Reservoir sampling (algorithm R): after the reservoir fills, the k-th new
// sample replaces a uniformly-random existing slot with probability
// LAT_RESERVOIR_CAP/k, leaving every sample seen so far equally likely to
// survive — an unbiased random subsample of the whole run's round trips.
static void lat_record(WsThreadStats *st, unsigned int *seed, double us) {
    st->lat_seen++;
    if (st->lat_count < LAT_RESERVOIR_CAP) {
        st->lat_us[st->lat_count++] = us;
    } else {
        long k = st->lat_seen;
        long j = (long)((unsigned long)rand_r(seed) % (unsigned long)k);
        if (j < LAT_RESERVOIR_CAP) st->lat_us[j] = us;
    }
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

typedef struct {
    Config      *cfg;
    char         gid[16];
    char         token[40];
    int          seat;
    WsThreadStats *st;
} WsClientArg;

// Minimal WS *client* handshake: send the GET /ws upgrade over an already-
// connected `fd`, read the 101 response, verify Sec-WebSocket-Accept against
// what ws_accept_from_key computes for the key we sent (belt-and-suspenders
// correctness check — not required to proceed, but cheap and it means a
// broken handshake fails loudly instead of silently). Any leftover bytes
// read past the header block (the server may have already written its
// first WS frame into the same TCP segment) are primed into the returned
// WsConn so nothing is lost — see ws_conn_prime's doc in ws.h.
static bool ws_client_handshake(int fd, const char *host, const char *path,
                                 const char *token, WsConn *out) {
    unsigned char keyraw[16];
    for (int i = 0; i < 16; i++) keyraw[i] = (unsigned char)(rand() & 0xFF);
    char key_b64[32];
    if (!ws_base64_encode(keyraw, 16, key_b64, sizeof key_b64)) return false;

    char req[512];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n\r\n", path, host, key_b64, token);
    if (n <= 0 || n >= (int)sizeof req) return false;
    if (ws_write_full(fd, req, n) != n) return false;

    char resp[1024]; int total = 0;
    char *hdr_end = NULL;
    while (total < (int)sizeof resp - 1) {
        ssize_t r = read(fd, resp + total, sizeof resp - 1 - (size_t)total);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        total += (int)r; resp[total] = 0;
        for (int i = 0; i + 3 < total; i++)
            if (resp[i] == '\r' && resp[i+1] == '\n' && resp[i+2] == '\r' && resp[i+3] == '\n') { hdr_end = resp + i + 4; break; }
        if (hdr_end) break;
    }
    if (!hdr_end) return false;
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) return false;

    char expect[64];
    ws_accept_from_key(key_b64, expect, sizeof expect);
    // Search for the accept value ONLY inside the header block: any bytes
    // past hdr_end may already be the server's first WS frame (coalesced
    // into the same read), and those must reach ws_conn_prime byte-for-byte
    // untouched — copy the headers into a scratch buffer rather than
    // temporarily truncating `resp` in place.
    char hdrs_only[1024];
    int hdrs_len = (int)(hdr_end - resp); if (hdrs_len > (int)sizeof hdrs_only - 1) hdrs_len = (int)sizeof hdrs_only - 1;
    memcpy(hdrs_only, resp, (size_t)hdrs_len); hdrs_only[hdrs_len] = 0;
    if (!strstr(hdrs_only, expect)) return false;

    ws_conn_init(out, fd, /*mask_outgoing=*/1);   // we are the client: our frames must be masked
    int leftover = total - (int)(hdr_end - resp);
    if (leftover > 0) ws_conn_prime(out, (const unsigned char *)hdr_end, leftover);
    return true;
}

// One (game, seat)'s whole session: connect, upgrade, then loop receiving
// pushed state and submitting a randomly-chosen LEGAL move whenever this
// seat has one (else an empty poll frame, so it still notices when another
// seat's move makes it eligible). On disconnect/error, reconnect with a
// short backoff so the run keeps producing load instead of one seat quietly
// going idle for good.
// Sends a frame and, on success, stamps *t_pending with "now" so the NEXT
// successful ws_recv_message in this connection's loop can compute a
// round-trip sample (send this frame -> receive the server's pushed answer
// to it). *have_pending gates that: the very first receive on a fresh
// connection answers the server's post-handshake initial push, which has no
// preceding send in this loop, so it must not be timed as a round trip.
static int ws_send_timed(WsConn *wc, const unsigned char *payload, int64_t len,
                          double *t_pending, bool *have_pending) {
    int r = ws_send_frame(wc, WS_OP_BIN, payload, len);
    if (r >= 0) { *t_pending = now_secs(); *have_pending = true; }
    return r;
}

static void *ws_worker(void *argp) {
    WsClientArg *a = argp;
    WsThreadStats *st = a->st;
    unsigned int seed = (unsigned int)((uintptr_t)pthread_self() ^ (uintptr_t)time(NULL) ^ (uintptr_t)a);
    static __thread Game g;
    static __thread LegalMoves moves;
    unsigned char *msgbuf = malloc(1 + 65536);
    if (!msgbuf) return NULL;

    char path[64];
    snprintf(path, sizeof path, "/ws?game_id=%s&seat=%d", a->gid, a->seat);

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        int fd = connect_to(a->cfg->host, a->cfg->port);
        WsConn wc;
        if (fd < 0 || !ws_client_handshake(fd, a->cfg->host, path, a->token, &wc)) {
            if (fd >= 0) close(fd);
            st->connect_failures++;
            usleep(20 * 1000);
            continue;
        }
        st->connects++;

        // Reset per-connection so a dropped connection's stale timestamp
        // never leaks a bogus latency sample into the reconnected session.
        double t_pending = 0;
        bool have_pending = false;

        while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
            int opcode;
            int mlen = ws_recv_message(&wc, msgbuf, 1 + 65536, &opcode);
            if (mlen < 1) break;   // error/close/undersized — reconnect
            if (have_pending) {
                lat_record(st, &seed, (now_secs() - t_pending) * 1e6);
                have_pending = false;
            }
            st->msgs_recv++;
            unsigned char ok = msgbuf[0];
            if (ok) st->actions_applied++;

            memset(&g, 0, sizeof g);
            state_get(&g, msgbuf + 1, /*masked=*/1);

            if (g.status == GAME_STATUS_GAME_OVER) {
                do_meta(a->cfg, a->token, "continue", a->gid);
                do_meta(a->cfg, a->token, "start", a->gid);
                st->rematches++;
                usleep(2000);
                if (ws_send_timed(&wc, NULL, 0, &t_pending, &have_pending) < 0) break;
                st->polls_sent++;
                continue;
            }
            if (g.status != GAME_STATUS_PLAYING || a->seat < 0 || a->seat >= g.num_players) {
                usleep(WS_IDLE_POLL_US);
                if (ws_send_timed(&wc, NULL, 0, &t_pending, &have_pending) < 0) break;
                st->polls_sent++;
                continue;
            }

            calculate_legal_moves(&g, a->seat, &moves);
            if (moves.n <= 0) {
                usleep(WS_IDLE_POLL_US);   // not this seat's turn — poll gently instead of busy-spinning
                if (ws_send_timed(&wc, NULL, 0, &t_pending, &have_pending) < 0) break;
                st->polls_sent++;
                continue;
            }

            const LegalMove *m = &moves.moves[rand_r(&seed) % (unsigned)moves.n];
            AwireAction act; memset(&act, 0, sizeof act);
            switch (m->type) {
                case MOVE_ATTACK: act.kind = AWIRE_ATTACK; break;
                case MOVE_COVER:  act.kind = AWIRE_COVER;  break;
                case MOVE_PASS:   act.kind = AWIRE_PASS;   break;
                case MOVE_PICKUP: act.kind = AWIRE_PICKUP; break;
                case MOVE_GOOD:   act.kind = AWIRE_GOOD;   break;
                default:
                    if (ws_send_timed(&wc, NULL, 0, &t_pending, &have_pending) < 0) goto reconnect;
                    st->polls_sent++;
                    continue;   // MOVE_WAIT or unrecognized — poll instead
            }
            act.n = (act.kind == AWIRE_PICKUP || act.kind == AWIRE_GOOD) ? 0 : m->n_cards;
            if (act.n < 0) act.n = 0;
            if (act.n > AWIRE_MAX_CARDS) act.n = AWIRE_MAX_CARDS;
            for (int i = 0; i < act.n; i++) {
                act.cards[i] = m->cards[i];
                if (act.kind == AWIRE_COVER) act.attacks[i] = m->attack_cards[i];
            }
            unsigned char frame[128];
            int flen = awire_encode(&act, frame, sizeof frame);
            if (flen <= 0) {
                if (ws_send_timed(&wc, NULL, 0, &t_pending, &have_pending) < 0) break;
                st->polls_sent++;
                continue;
            }
            if (ws_send_timed(&wc, frame, flen, &t_pending, &have_pending) < 0) break;
            st->actions_sent++;
        }
    reconnect:
        close(fd);
        if (!atomic_load_explicit(&g_stop, memory_order_relaxed)) usleep(20 * 1000);
    }
    free(msgbuf);
    return NULL;
}

static void run_ws_load(Config *cfg) {
    int total_seats = 0;
    for (int gi = 0; gi < g_n_games; gi++) total_seats += g_games[gi].n_seats;
    if (total_seats == 0) { fprintf(stderr, "no seats to drive — aborting ws load phase\n"); return; }

    WsClientArg *args = calloc((size_t)total_seats, sizeof(WsClientArg));
    WsThreadStats *stats = calloc((size_t)total_seats, sizeof(WsThreadStats));
    pthread_t *tids = calloc((size_t)total_seats, sizeof(pthread_t));
    if (!args || !stats || !tids) { fprintf(stderr, "out of memory\n"); return; }

    int idx = 0;
    for (int gi = 0; gi < g_n_games; gi++) {
        HGame *hg = &g_games[gi];
        for (int s = 0; s < hg->n_seats; s++) {
            WsClientArg *a = &args[idx];
            a->cfg = cfg;
            snprintf(a->gid, sizeof a->gid, "%s", hg->id);
            snprintf(a->token, sizeof a->token, "%s", g_users[hg->user_idx[s]].token);
            a->seat = s;
            a->st = &stats[idx];
            idx++;
        }
    }

    printf("== ws load phase: %d persistent connections (one per seat; --conns ignored) for %ds ==\n",
           total_seats, cfg->secs);
    double t0 = now_secs();
    int nthreads = 0;
    for (int i = 0; i < total_seats; i++) {
        if (pthread_create(&tids[i], NULL, ws_worker, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed at ws worker %d, continuing with fewer\n", i);
            break;
        }
        nthreads++;
    }
    while (now_secs() - t0 < cfg->secs) usleep(50 * 1000);
    atomic_store(&g_stop, true);
    for (int i = 0; i < nthreads; i++) pthread_join(tids[i], NULL);
    double elapsed = now_secs() - t0;

    WsThreadStats tot; memset(&tot, 0, sizeof tot);
    for (int i = 0; i < nthreads; i++) {
        tot.msgs_recv        += stats[i].msgs_recv;
        tot.actions_sent     += stats[i].actions_sent;
        tot.actions_applied  += stats[i].actions_applied;
        tot.polls_sent       += stats[i].polls_sent;
        tot.rematches        += stats[i].rematches;
        tot.connects         += stats[i].connects;
        tot.connect_failures += stats[i].connect_failures;
    }

    // ---- round-trip latency percentiles (Deliverable A) ----
    // Merge every worker's reservoir into one array and sort it — with
    // LAT_RESERVOIR_CAP*nthreads capped in the low millions of doubles even
    // at hundreds of connections, a plain sort is simplest and exact for the
    // sample actually held (percentiles over a proper random subsample, not
    // an approximation on top of an approximation).
    long lat_seen_total = 0;
    for (int i = 0; i < nthreads; i++) lat_seen_total += stats[i].lat_seen;
    long lat_cap_total = 0;
    for (int i = 0; i < nthreads; i++) lat_cap_total += stats[i].lat_count;
    double *all_lat = lat_cap_total > 0 ? malloc(sizeof(double) * (size_t)lat_cap_total) : NULL;
    long n_lat = 0;
    if (all_lat) {
        for (int i = 0; i < nthreads; i++)
            for (long j = 0; j < stats[i].lat_count; j++) all_lat[n_lat++] = stats[i].lat_us[j];
        qsort(all_lat, (size_t)n_lat, sizeof(double), cmp_double);
    }
    double lat_mean = 0, lat_p50 = 0, lat_p90 = 0, lat_p99 = 0, lat_max = 0;
    if (n_lat > 0) {
        double sum = 0; for (long i = 0; i < n_lat; i++) sum += all_lat[i];
        lat_mean = sum / (double)n_lat;
        lat_p50 = all_lat[(size_t)((double)(n_lat - 1) * 0.50)];
        lat_p90 = all_lat[(size_t)((double)(n_lat - 1) * 0.90)];
        lat_p99 = all_lat[(size_t)((double)(n_lat - 1) * 0.99)];
        lat_max = all_lat[n_lat - 1];
    }
    free(all_lat);

    printf("\n============= foolish_hammer summary (mode=ws) =============\n");
    printf("wall clock (load phase):     %.2fs\n", elapsed);
    printf("persistent WS connections:   %d (one per seat)\n", nthreads);
    printf("messages received (pushes):  %ld  (%.1f msgs/s)\n", tot.msgs_recv, tot.msgs_recv / elapsed);
    printf("actions submitted:           %ld  applied(ok=true): %ld  (%.1f applied/s)\n",
           tot.actions_sent, tot.actions_applied, tot.actions_applied / elapsed);
    printf("poll frames sent:            %ld\n", tot.polls_sent);
    printf("round-trip latency (send -> next push received), microseconds:\n");
    printf("  round trips timed: %ld  (percentile sample: %ld%s)\n", lat_seen_total, n_lat,
           lat_seen_total > n_lat ? ", reservoir-capped" : "");
    printf("  mean=%.1f  p50=%.1f  p90=%.1f  p99=%.1f  max=%.1f\n",
           lat_mean, lat_p50, lat_p90, lat_p99, lat_max);
    // One grep-able line, so a wrapper sweeping concurrency (mode=ws has no
    // --conns knob — connections = games*seats, see --games/--seats) can
    // collect this across several runs and tabulate latency vs. concurrency.
    printf("latency_summary_us: conns=%d count=%ld mean=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f\n",
           nthreads, n_lat, lat_mean, lat_p50, lat_p90, lat_p99, lat_max);
    printf("rematches (continue+start):  %ld\n", tot.rematches);
    printf("connects: %ld  connect failures: %ld\n", tot.connects, tot.connect_failures);
    printf("===============================================================\n");

    free(args); free(stats); free(tids);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    Config cfg;
    snprintf(cfg.host, sizeof cfg.host, "%s", get_arg(argc, argv, "host", "127.0.0.1"));
    cfg.port  = parse_int(get_arg(argc, argv, "port", NULL), 8099);
    cfg.games = parse_int(get_arg(argc, argv, "games", NULL), 20);
    cfg.seats = parse_int(get_arg(argc, argv, "seats", NULL), 4);
    cfg.conns = parse_int(get_arg(argc, argv, "conns", NULL), 32);
    cfg.secs  = parse_int(get_arg(argc, argv, "secs", NULL), 15);
    const char *mode = get_arg(argc, argv, "mode", "action");
    cfg.mixed = (strcmp(mode, "mixed") == 0);
    cfg.ws    = (strcmp(mode, "ws") == 0);
    srand((unsigned)(time(NULL) ^ getpid()));   // seeds the WS masking-key / handshake-key rand() calls
    signal(SIGPIPE, SIG_IGN);   // persistent WS sockets: a dead peer must not kill this whole process

    if (cfg.seats < 2) cfg.seats = 2;
    if (cfg.seats > MAX_PLAYERS) cfg.seats = MAX_PLAYERS;
    if (cfg.games < 1) cfg.games = 1;
    if (cfg.games > HAMMER_MAX_GAMES) cfg.games = HAMMER_MAX_GAMES;
    if (cfg.games * cfg.seats > HAMMER_MAX_USERS) {
        int clamped = HAMMER_MAX_USERS / cfg.seats;
        fprintf(stderr, "warning: games*seats > %d (server MAX_USERS) — clamping games %d -> %d\n",
                HAMMER_MAX_USERS, cfg.games, clamped);
        cfg.games = clamped > 0 ? clamped : 1;
    }
    if (cfg.conns < 1) cfg.conns = 1;
    if (cfg.secs < 1) cfg.secs = 1;
    g_cfg = cfg;

    printf("foolish_hammer: host=%s:%d games=%d seats=%d conns=%d secs=%d mode=%s\n",
           cfg.host, cfg.port, cfg.games, cfg.seats, cfg.conns, cfg.secs,
           cfg.ws ? "ws" : (cfg.mixed ? "mixed" : "action"));

    {
        unsigned char resp[RESP_CAP]; HttpResp r;
        if (!http_do(cfg.host, cfg.port, "GET", "/health", NULL, NULL, 0, resp, sizeof resp, &r) || r.status != 200) {
            fprintf(stderr, "server not reachable at %s:%d — aborting\n", cfg.host, cfg.port);
            return 1;
        }
    }

    double t_setup0 = now_secs();
    setup(&cfg);
    double t_setup1 = now_secs();
    if (g_n_games == 0) { fprintf(stderr, "no games dealt — aborting load phase\n"); return 1; }
    printf("== setup done in %.2fs: %d users, %d games ==\n", t_setup1 - t_setup0, g_n_users, g_n_games);

    if (cfg.ws) {
        run_ws_load(&cfg);
        return 0;
    }

    int nthreads = cfg.conns;
    pthread_t *tids = calloc((size_t)nthreads, sizeof(pthread_t));
    ThreadStats *stats = calloc((size_t)nthreads, sizeof(ThreadStats));
    if (!tids || !stats) { fprintf(stderr, "out of memory\n"); return 1; }

    printf("== load phase: %d threads for %ds (mode=%s) ==\n", nthreads, cfg.secs, cfg.mixed ? "mixed" : "action");
    double t0 = now_secs();
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, loader_thread, &stats[i]) != 0) {
            fprintf(stderr, "pthread_create failed at thread %d, continuing with fewer\n", i);
            nthreads = i;
            break;
        }
    }
    while (now_secs() - t0 < cfg.secs) usleep(50 * 1000);
    atomic_store(&g_stop, true);
    for (int i = 0; i < nthreads; i++) pthread_join(tids[i], NULL);
    double elapsed = now_secs() - t0;

    ThreadStats tot; memset(&tot, 0, sizeof tot);
    for (int i = 0; i < nthreads; i++) {
        tot.actions_sent    += stats[i].actions_sent;
        tot.actions_ok      += stats[i].actions_ok;
        tot.state_gets      += stats[i].state_gets;
        tot.status_gets     += stats[i].status_gets;
        tot.grow_attempts   += stats[i].grow_attempts;
        tot.total_requests  += stats[i].total_requests;
        tot.code_200 += stats[i].code_200; tot.code_400 += stats[i].code_400;
        tot.code_401 += stats[i].code_401; tot.code_404 += stats[i].code_404;
        tot.code_5xx += stats[i].code_5xx; tot.code_other += stats[i].code_other;
        tot.code_fail += stats[i].code_fail;
    }

    printf("\n================ foolish_hammer summary ================\n");
    printf("wall clock (load phase): %.2fs\n", elapsed);
    printf("total requests:  %ld  (%.1f req/s)\n", tot.total_requests, tot.total_requests / elapsed);
    printf("  actions sent:  %ld  ok=true: %ld  (%.1f applied/s)\n",
           tot.actions_sent, tot.actions_ok, tot.actions_ok / elapsed);
    printf("  state gets:    %ld\n", tot.state_gets);
    printf("  status gets:   %ld\n", tot.status_gets);
    printf("  grow attempts: %ld  (final games=%d users=%d)\n", tot.grow_attempts, g_n_games, g_n_users);
    printf("status codes:    200=%ld 400=%ld 401=%ld 404=%ld 5xx=%ld other=%ld conn_fail=%ld\n",
           tot.code_200, tot.code_400, tot.code_401, tot.code_404, tot.code_5xx, tot.code_other, tot.code_fail);
    printf("==========================================================\n");

    free(tids); free(stats);
    return 0;
}

// wsbench.c - the load generator for the race (SCOPE.md 3.3): thousands of WebSocket clients
// speaking lichess's protocol to a gateway (the real lila-ws or ours), and a fake lila on Redis
// that answers their moves with versioned move events, both in this one process on c_base's
// loop. It measures what a player and a watcher see.
//
//   wsbench [--port 9664] [--redis-port 9679] [--mongo-port 27117] [--games 500] [--watchers 2]
//           [--site 1000] [--lobby 200] [--move-ms 2000] [--ping-ms 2000] [--duration 60]
//           [--warmup 15] [--pid <server pid>] [--connect-rate 500] [--seed-only] [--quiet]
//
// THE WORKLOAD. G games, each with two logged-in players on /play sockets moving in turn every
// move-ms (the next move goes move-ms after the previous move's event arrived), W anonymous
// watchers per game on /watch sockets, S site sockets pinging every ping-ms, L lobby sockets
// pinging too; every socket, players and watchers included, pings every ping-ms as a browser does
// (both servers stop a socket that has not pinged for 30 s). The fake lila subscribes to r-in:0..15, site-in and lobby-in, answers each r/move
// with `r/ver <game> <v> <W|B> move <~300 bytes of JSON: uci, san, fen, ply, dests, clock>`, each
// `ping` with `pong`, and keeps lila's boot handshake.
//
// THE NUMBERS. Move relay latency from the player's `move` frame to the move event's arrival
// (p50, p90, p99, p999, max) as the mover sees it and as a watcher sees it; the same for a site
// ping's `0`; moves and events per second; bytes; the server's RSS, user and system CPU time
// over the measured window (from its pid), hence CPU microseconds per message. Everything is
// measured after the warm-up, for `duration` seconds.
#include "bson.h"
#include "lila_wire.h"
#include "mongo.h"
#include "net/loop.h"
#include "net/ws.h"
#include "resp.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#endif

// ---- options ----
static struct {
    int port, redis_port, mongo_port, games, watchers, site, lobby, move_ms, ping_ms, duration, warmup, connect_rate;
    pid_t pid;
    bool seed_only, quiet;
} O = { .port = 9664, .redis_port = 9679, .mongo_port = 27117, .games = 500, .watchers = 2, .site = 1000, .lobby = 200,
        .move_ms = 2000, .ping_ms = 2000, .duration = 60, .warmup = 15, .connect_rate = 500 };

// ---- a histogram of microseconds ----
typedef struct { uint32_t *v; size_t n, cap; double sum; uint64_t max; } hist;
static void hist_add(hist *h, uint64_t us) {
    if (h->n == h->cap) { h->cap = h->cap ? h->cap * 2 : 4096; h->v = realloc(h->v, h->cap * sizeof *h->v); }
    h->v[h->n++] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    h->sum += (double)us;
    if (us > h->max) h->max = us;
}
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static uint32_t hist_pct(hist *h, double p) {
    if (!h->n) return 0;
    size_t i = (size_t)(p * (double)(h->n - 1) + 0.5);
    return h->v[i < h->n ? i : h->n - 1];
}
static void hist_sort(hist *h) { qsort(h->v, h->n, sizeof *h->v, cmp_u32); }

// ---- a WebSocket client on the loop ----
typedef enum { R_PLAYER, R_WATCHER, R_SITE, R_LOBBY } role;
typedef struct game game;
typedef struct client {
    int     fd;
    role    role;
    int     state;        // 0 connecting, 1 upgrading, 2 open, 3 closed
    cb_buf  in, out;
    game   *game;
    int     color;        // players
    int     idx;
    int64_t ping_sent_us; // the ping in flight (0 none)
    uint64_t timer;       // a player: the next move
    uint64_t ping_timer;  // every socket: the next ping
    int64_t opened_ms;
    int     events;       // frames received
} client;

struct game {
    char    id[9];
    int     idx;
    int     v;            // the version lila will give the next event
    int     turn;         // 0 white to move
    int     ply;
    client *players[2];
    int64_t move_sent_us; // the move in flight, 0 none
    int     move_v;       // the version that move's event will carry
    bool    mover_seen;
    int     watchers_seen;
};

static cb_loop *L;
static resp_client *redis;
static client *clients;
static int n_clients, n_open, n_failed, n_closed;
static game *games;
static int64_t t_start_us, t_measure_us, t_end_us;
static bool measuring, done_flag;
static hist h_mover, h_watcher, h_ping;
static long moves_sent, events_recv, pings_sent, pongs_recv, bytes_in, bytes_out, frames_in;
static long warm_moves, warm_events;
static long fake_moves, fake_pings;
static bool lila_ready;

static int64_t now_us(void) { return cb_mono_us(); }

static void client_close(client *c) {
    if (c->state == 3) return;
    if (c->fd >= 0) { cb_loop_remove(L, c->fd); close(c->fd); }
    c->fd = -1;
    c->state = 3;
    n_closed++;
    if (c->timer) { cb_loop_cancel(L, c->timer); c->timer = 0; }
    if (c->ping_timer) { cb_loop_cancel(L, c->ping_timer); c->ping_timer = 0; }
}

static void client_want(client *c) {
    if (c->fd < 0 || c->state == 3) return;
    cb_loop_set(L, c->fd, CB_EV_READ | (c->out.len ? CB_EV_WRITE : 0));
}

static void client_flush(client *c) {
    while (c->out.len) {
        ssize_t k = send(c->fd, c->out.p, c->out.len, MSG_DONTWAIT);
        if (k < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; if (errno == EINTR) continue; client_close(c); return; }
        bytes_out += k;
        cb_buf_consume(&c->out, (size_t)k);
    }
    client_want(c);
}

static void client_send_text(client *c, const char *s, size_t n) {
    uint8_t hdr[14], mask[4];
    uint32_t r = (uint32_t)rand();
    memcpy(mask, &r, 4);
    size_t hl = cb_ws_frame_header(hdr, true, CB_WS_OP_TEXT, n, mask);
    cb_buf_append(&c->out, hdr, hl);
    size_t at = c->out.len;
    cb_buf_append(&c->out, s, n);
    cb_ws_unmask(c->out.p + at, n, mask);
    client_flush(c);
}

static void client_send_pong(client *c, const uint8_t *p, size_t n) {
    uint8_t hdr[14], mask[4] = { 1, 2, 3, 4 };
    size_t hl = cb_ws_frame_header(hdr, true, CB_WS_OP_PONG, n, mask);
    cb_buf_append(&c->out, hdr, hl);
    size_t at = c->out.len;
    cb_buf_append(&c->out, p, n);
    cb_ws_unmask(c->out.p + at, n, mask);
    client_flush(c);
}

static void send_move(cb_loop *l, void *ud);
static void send_ping(cb_loop *l, void *ud);

static void on_text(client *c, const char *s, size_t n) {
    int64_t now = now_us();
    c->events++;
    // every socket pings, as a browser's does (lila-ws's broom stops a socket silent for 30 s)
    if ((n == 1 && s[0] == '0') || (n > 8 && memcmp(s, "{\"t\":\"n\"", 8) == 0)) {
        if (c->ping_sent_us) {
            pongs_recv++;
            if (measuring) hist_add(&h_ping, (uint64_t)(now - c->ping_sent_us));
            c->ping_sent_us = 0;
        }
        return;
    }
    if (c->role == R_SITE || c->role == R_LOBBY) return;
    // a round socket: a move event {"t":"move","v":N,...
    if (n > 20 && memcmp(s, "{\"t\":\"move\",\"v\":", 16) == 0) {
        int v = atoi(s + 16);
        game *g = c->game;
        if (measuring) events_recv++; else warm_events++;
        if (g->move_sent_us && v == g->move_v) {
            uint64_t lat = (uint64_t)(now - g->move_sent_us);
            if (c->role == R_PLAYER && c->color != g->turn) {   // the mover: turn already advanced
                if (!g->mover_seen) {
                    g->mover_seen = true;
                    if (measuring) hist_add(&h_mover, lat);
                }
            } else if (c->role == R_WATCHER) {
                if (measuring && g->watchers_seen < O.watchers) hist_add(&h_watcher, lat);
                g->watchers_seen++;
            }
            if (g->mover_seen && (g->watchers_seen >= O.watchers || O.watchers == 0)) {
                g->move_sent_us = 0;
                // the next move, by the other player, move_ms later
                client *next = g->players[g->turn];
                if (next && next->state == 2 && !next->timer && !done_flag) next->timer = cb_loop_timer(L, O.move_ms, send_move, next);
            }
        }
    }
}

static void client_frames(client *c) {
    for (;;) {
        cb_ws_frame f;
        int n = cb_ws_frame_parse(c->in.p, c->in.len, CB_WS_MAX_MESSAGE, &f);
        if (n == 0) return;
        if (n < 0) { client_close(c); return; }
        uint8_t *payload = c->in.p + f.header_len;
        frames_in++;
        if (f.opcode == CB_WS_OP_TEXT) on_text(c, (const char *)payload, (size_t)f.len);
        else if (f.opcode == CB_WS_OP_PING) client_send_pong(c, payload, (size_t)f.len);
        else if (f.opcode == CB_WS_OP_CLOSE) { client_close(c); return; }
        cb_buf_consume(&c->in, (size_t)n);
    }
}

static void client_io(cb_loop *l, int fd, int events, void *ud) {
    client *c = ud;
    if (c->state == 0) {
        int err = 0; socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { n_failed++; client_close(c); return; }
        c->state = 1;
        client_flush(c);
        cb_loop_set(L, fd, CB_EV_READ | (c->out.len ? CB_EV_WRITE : 0));
        return;
    }
    if (events & CB_EV_ERR) { client_close(c); return; }
    if (events & CB_EV_READ) {
        for (;;) {
            if (!cb_buf_reserve(&c->in, 16384)) { client_close(c); return; }
            ssize_t k = recv(fd, c->in.p + c->in.len, 16384, MSG_DONTWAIT);
            if (k < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; if (errno == EINTR) continue; client_close(c); return; }
            if (k == 0) { client_close(c); return; }
            bytes_in += k;
            c->in.len += (size_t)k;
            c->in.p[c->in.len] = 0;
            if ((size_t)k < 16384) break;
        }
        if (c->state == 1) {
            char *end = strstr((char *)c->in.p, "\r\n\r\n");
            if (!end) return;
            if (strncmp((char *)c->in.p, "HTTP/1.1 101", 12) != 0) { n_failed++; if (!O.quiet) fprintf(stderr, "upgrade refused: %.60s\n", (char *)c->in.p); client_close(c); return; }
            cb_buf_consume(&c->in, (size_t)(end + 4 - (char *)c->in.p));
            c->state = 2;
            c->opened_ms = cb_mono_ms();
            n_open++;
            c->ping_timer = cb_loop_timer(L, O.ping_ms / 2 + rand() % O.ping_ms, send_ping, c);
            if (c->role == R_PLAYER && c->color == 0 && !c->timer) c->timer = cb_loop_timer(L, 1000 + rand() % O.move_ms, send_move, c);
        }
        if (c->state == 2) client_frames(c);
    }
    if ((events & CB_EV_WRITE) && c->state >= 1 && c->state != 3) client_flush(c);
}

static int client_connect(client *c, const char *path, const char *cookie) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)O.port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    c->fd = fd;
    c->state = rc == 0 ? 1 : 0;
    cb_buf_printf(&c->out, "GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nUser-Agent: wsbench\r\n%s\r\n", path, cookie ? cookie : "");
    if (cb_loop_add(L, fd, c->state == 0 ? CB_EV_WRITE : CB_EV_READ | CB_EV_WRITE, client_io, c) != CB_OK) { close(fd); c->fd = -1; return -1; }
    return 0;
}

// ---- the clients' behaviour ----

static const char *const UCIS[] = { "e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "d2d3", "d7d6", "c2c3", "g8f6", "b1d2", "e8g8", "e1g1", "a7a6", "a2a4", "h7h6" };

static void send_move(cb_loop *l, void *ud) {
    client *c = ud;
    c->timer = 0;
    if (c->state != 2 || done_flag) return;
    game *g = c->game;
    if (g->move_sent_us) return;   // one in flight
    char msg[128];
    snprintf(msg, sizeof msg, "{\"t\":\"move\",\"d\":{\"u\":\"%s\",\"b\":0,\"l\":%d,\"a\":%d}}", UCIS[g->ply % 16], 20 + rand() % 40, g->ply);
    g->move_sent_us = now_us();
    g->move_v = g->v;      // the fake lila numbers the next event g->v
    g->mover_seen = false;
    g->watchers_seen = 0;
    g->turn = 1 - g->turn;
    g->ply++;
    if (measuring) moves_sent++; else warm_moves++;
    client_send_text(c, msg, strlen(msg));
}

static void send_ping(cb_loop *l, void *ud) {
    client *c = ud;
    c->ping_timer = 0;
    if (c->state != 2 || done_flag) return;
    if (!c->ping_sent_us) {
        c->ping_sent_us = now_us();
        pings_sent++;
        client_send_text(c, "p", 1);
    }
    c->ping_timer = cb_loop_timer(L, O.ping_ms, send_ping, c);
}

// ---- the fake lila ----

static const char FENS[][72] = {
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR", "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR",
    "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R", "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R",
};
static const char DESTS[] = "{\"a2\":\"a3a4\",\"b2\":\"b3b4\",\"c2\":\"c3c4\",\"d2\":\"d3d4\",\"e2\":\"e3e4\",\"f2\":\"f3f4\",\"g2\":\"g3g4\",\"h2\":\"h3h4\",\"b1\":\"a3c3\",\"g1\":\"f3h3\",\"e1\":\"e2f1\",\"d1\":\"e2f3g4h5\",\"f1\":\"e2d3c4b5a6\",\"c1\":\"d2e3f4g5h6\"}";

static void fake_lila(const char *chan, size_t clen, const char *msg, size_t n, void *ud) {
    lo_msg dummy;
    (void)dummy;
    static cb_buf out;
    cb_buf_reset(&out);
    if (n > 7 && memcmp(msg, "r/move ", 7) == 0) {
        // r/move <fullId> <uci> <blur> <lag> <mt> <frame>
        char full[16], uci[8];
        if (sscanf(msg + 7, "%15s %7s", full, uci) != 2 || strlen(full) < 12) return;
        char gid[9];
        memcpy(gid, full, 8); gid[8] = 0;
        int gi = atoi(gid + 2);   // ids are "wb%06d"
        if (gi < 0 || gi >= O.games) return;
        game *g = &games[gi];
        int v = g->v++;
        fake_moves++;
        char line[16];
        cb_buf_printf(&out, "r/ver %s %d %c move {\"uci\":\"%s\",\"san\":\"%s\",\"fen\":\"%s\",\"ply\":%d,\"dests\":%s,\"clock\":{\"white\":%d.%d,\"black\":%d,\"lag\":2}}",
                      gid, v, (g->ply % 2) ? 'W' : 'B', uci, uci + 2, FENS[v % 4], v + 1, DESTS, 170 - v % 60, v % 10, 175 - v % 50);
        snprintf(line, sizeof line, "r-out:%d", li_round_lane(gid, 8));
        resp_publish(redis, line, cb_buf_cstr(&out), out.len);
        return;
    }
    if (n > 5 && memcmp(msg, "ping ", 5) == 0) {
        fake_pings++;
        cb_buf_printf(&out, "pong %.*s", (int)(n - 5), msg + 5);
        resp_publish(redis, strcmp(chan, "site-in") == 0 ? "site-out" : "r-out", cb_buf_cstr(&out), out.len);
        return;
    }
    if (n == 4 && memcmp(msg, "boot", 4) == 0 && strncmp(chan, "r-in", 4) == 0) {
        // lila-ws booted: lila answers nothing to that; but if lila booted it would say boot on r-out
        return;
    }
    if (n > 14 && memcmp(msg, "room/versions ", 14) == 0) {
        resp_publish(redis, "r-out", "r/versioning-ready", 18);
        return;
    }
}

static void fake_state(bool up, void *ud) {
    if (!up) { fprintf(stderr, "redis down\n"); return; }
    // lila's own boot: ask the gateway for its versions, then declare versioning ready
    resp_publish(redis, "r-out", "boot", 4);
    lila_ready = true;
}

// ---- seeding MongoDB ----

static int seed(void) {
    mongo_cfg cfg = { .host = "127.0.0.1", .port = O.mongo_port, .db = "lichess", .timeout_ms = 5000 };
    mongo_conn *m = NULL;
    if (mongo_connect(&m, &cfg) != CB_OK) { fprintf(stderr, "no MongoDB on %d\n", O.mongo_port); return -1; }
    cb_buf d;
    cb_buf_init(&d);
    bson_w w;
    int made = 0;
    for (int i = 0; i < O.games; i++) {
        char gid[9], uw[16], ub[16], sw[24], sb[24];
        snprintf(gid, sizeof gid, "wb%06d", i);
        snprintf(uw, sizeof uw, "wbw%06d", i);
        snprintf(ub, sizeof ub, "wbb%06d", i);
        snprintf(sw, sizeof sw, "sid%s", uw);
        snprintf(sb, sizeof sb, "sid%s", ub);
        const char *us[2] = { uw, ub }, *ss[2] = { sw, sb };
        for (int k = 0; k < 2; k++) {
            cb_buf_reset(&d);
            bson_w_init(&w, &d); bson_w_begin(&w);
            bson_w_str(&w, "_id", us[k]); bson_w_str(&w, "username", us[k]); bson_w_bool(&w, "enabled", true);
            bson_w_end(&w);
            int rc = mongo_insert_one(m, "user4", &d);
            if (rc != CB_OK && rc != CB_E_CONFLICT) { mongo_close(m); return -1; }
            cb_buf_reset(&d);
            bson_w_init(&w, &d); bson_w_begin(&w);
            bson_w_str(&w, "_id", ss[k]); bson_w_str(&w, "user", us[k]); bson_w_bool(&w, "up", true);
            bson_w_end(&w);
            rc = mongo_insert_one(m, "security", &d);
            if (rc != CB_OK && rc != CB_E_CONFLICT) { mongo_close(m); return -1; }
        }
        cb_buf_reset(&d);
        bson_w_init(&w, &d); bson_w_begin(&w);
        bson_w_str(&w, "_id", gid); bson_w_str(&w, "is", "aaaabbbb");
        bson_w_array(&w, "us"); bson_w_str(&w, "0", uw); bson_w_str(&w, "1", ub); bson_w_end(&w);
        bson_w_int32(&w, "s", 20);
        bson_w_end(&w);
        int rc = mongo_insert_one(m, "game5", &d);
        if (rc == CB_OK) made++;
        else if (rc != CB_E_CONFLICT) { mongo_close(m); return -1; }
    }
    cb_buf_free(&d);
    mongo_close(m);
    if (!O.quiet) fprintf(stderr, "seeded %d games (%d new)\n", O.games, made);
    return 0;
}

// ---- the server's resources ----

typedef struct { double user_s, sys_s; long rss_kb; } proc_usage;
static bool read_usage(pid_t pid, proc_usage *u) {
    memset(u, 0, sizeof *u);
    if (pid <= 0) return false;
#ifdef __APPLE__
    struct rusage_info_v2 ri;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) != 0) return false;
    // ri_user_time and ri_system_time are Mach absolute time units (on Apple Silicon 125/3 ns each)
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    double ns_per_unit = (double)tb.numer / (double)tb.denom;
    u->user_s = (double)ri.ri_user_time * ns_per_unit / 1e9;
    u->sys_s = (double)ri.ri_system_time * ns_per_unit / 1e9;
    u->rss_kb = (long)(ri.ri_resident_size / 1024);
    return true;
#else
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[2048];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return false; }
    fclose(f);
    char *p = strrchr(buf, ')');
    if (!p) return false;
    unsigned long ut = 0, st = 0, rss = 0;
    // fields after ')': state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15) ... rss(24)
    char state; int ppid, pgrp, session, tty, tpgid; unsigned flags; unsigned long minflt, cminflt, majflt, cmajflt;
    long cutime, cstime, prio, nice, nth, itreal; unsigned long long start; unsigned long vsize;
    if (sscanf(p + 2, "%c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu %lu %lu", &state, &ppid, &pgrp, &session, &tty, &tpgid,
               &flags, &minflt, &cminflt, &majflt, &cmajflt, &ut, &st, &cutime, &cstime, &prio, &nice, &nth, &itreal, &start, &vsize, &rss) < 22) return false;
    long hz = sysconf(_SC_CLK_TCK);
    u->user_s = (double)ut / hz;
    u->sys_s = (double)st / hz;
    u->rss_kb = (long)rss * (sysconf(_SC_PAGESIZE) / 1024);
    return true;
#endif
}

// ---- the schedule ----

static int next_to_connect;
static proc_usage u_before, u_after, u_idle;

static void connect_some(cb_loop *l, void *ud) {
    int per_tick = O.connect_rate / 20 + 1;   // every 50 ms
    for (int i = 0; i < per_tick && next_to_connect < n_clients; i++, next_to_connect++) {
        client *c = &clients[next_to_connect];
        char path[160], cookie[80];
        const char *ck = NULL;
        switch (c->role) {
        case R_PLAYER:
            snprintf(path, sizeof path, "/play/%s%s/v6?sri=p%s%d", c->game->id, c->color ? "bbbb" : "aaaa", c->game->id, c->color);
            snprintf(cookie, sizeof cookie, "Cookie: lila2=sessionId=sidwb%c%06d\r\n", c->color ? 'b' : 'w', c->game->idx);
            ck = cookie;
            break;
        case R_WATCHER: snprintf(path, sizeof path, "/watch/%s/white/v6?sri=w%s%d", c->game->id, c->game->id, c->idx); break;
        case R_SITE: snprintf(path, sizeof path, "/socket/v5?sri=s%08d", c->idx); break;
        case R_LOBBY: snprintf(path, sizeof path, "/lobby/socket/v5?sri=l%08d", c->idx); break;
        }
        if (client_connect(c, path, ck) != 0) { n_failed++; c->state = 3; }
    }
    if (next_to_connect < n_clients) cb_loop_timer(l, 50, connect_some, NULL);
}

static void end_measure(cb_loop *l, void *ud) {
    measuring = false;
    done_flag = true;
    t_end_us = now_us();
    read_usage(O.pid, &u_after);
    cb_loop_stop(l);
}

static void start_measure(cb_loop *l, void *ud) {
    measuring = true;
    t_measure_us = now_us();
    read_usage(O.pid, &u_before);
    cb_loop_timer(l, O.duration * 1000, end_measure, NULL);
    if (!O.quiet) fprintf(stderr, "measuring for %d s: %d open, %d failed, %d closed, warm-up moves %ld events %ld\n", O.duration, n_open, n_failed, n_closed, warm_moves, warm_events);
}

static void usage(void) {
    fprintf(stderr, "usage: wsbench [--port N] [--redis-port N] [--mongo-port N] [--games N] [--watchers N] [--site N] [--lobby N] [--move-ms N] [--ping-ms N] [--duration S] [--warmup S] [--pid PID] [--connect-rate N] [--seed-only] [--quiet]\n");
    exit(2);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int *ip = NULL;
        if (!strcmp(a, "--port")) ip = &O.port; else if (!strcmp(a, "--redis-port")) ip = &O.redis_port; else if (!strcmp(a, "--mongo-port")) ip = &O.mongo_port;
        else if (!strcmp(a, "--games")) ip = &O.games; else if (!strcmp(a, "--watchers")) ip = &O.watchers; else if (!strcmp(a, "--site")) ip = &O.site;
        else if (!strcmp(a, "--lobby")) ip = &O.lobby; else if (!strcmp(a, "--move-ms")) ip = &O.move_ms; else if (!strcmp(a, "--ping-ms")) ip = &O.ping_ms;
        else if (!strcmp(a, "--duration")) ip = &O.duration; else if (!strcmp(a, "--warmup")) ip = &O.warmup; else if (!strcmp(a, "--connect-rate")) ip = &O.connect_rate;
        else if (!strcmp(a, "--pid")) { if (++i >= argc) usage(); O.pid = (pid_t)atoi(argv[i]); continue; }
        else if (!strcmp(a, "--seed-only")) { O.seed_only = true; continue; }
        else if (!strcmp(a, "--quiet")) { O.quiet = true; continue; }
        else usage();
        if (++i >= argc) usage();
        *ip = atoi(argv[i]);
    }
    signal(SIGPIPE, SIG_IGN);
    cb_log_init(CB_LOG_WARN, 0);
    srand(12345);
    if (seed() != 0) return 1;
    if (O.seed_only) return 0;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) { rl.rlim_cur = rl.rlim_max < 65536 ? rl.rlim_max : 65536; setrlimit(RLIMIT_NOFILE, &rl); }

    L = cb_loop_new();
    games = calloc((size_t)O.games, sizeof *games);
    n_clients = O.games * (2 + O.watchers) + O.site + O.lobby;
    clients = calloc((size_t)n_clients, sizeof *clients);
    int ci = 0;
    for (int i = 0; i < O.games; i++) {
        game *g = &games[i];
        snprintf(g->id, sizeof g->id, "wb%06d", i);
        g->idx = i;
        for (int k = 0; k < 2; k++) {
            client *c = &clients[ci++];
            c->role = R_PLAYER; c->game = g; c->color = k; c->fd = -1; c->idx = i;
            cb_buf_init(&c->in); cb_buf_init(&c->out);
            g->players[k] = c;
        }
        for (int k = 0; k < O.watchers; k++) {
            client *c = &clients[ci++];
            c->role = R_WATCHER; c->game = g; c->fd = -1; c->idx = k;
            cb_buf_init(&c->in); cb_buf_init(&c->out);
        }
    }
    for (int i = 0; i < O.site; i++) { client *c = &clients[ci++]; c->role = R_SITE; c->fd = -1; c->idx = i; cb_buf_init(&c->in); cb_buf_init(&c->out); }
    for (int i = 0; i < O.lobby; i++) { client *c = &clients[ci++]; c->role = R_LOBBY; c->fd = -1; c->idx = i; cb_buf_init(&c->in); cb_buf_init(&c->out); }
    // interleave the connection order so the games fill evenly
    for (int i = n_clients - 1; i > 0; i--) { int j = rand() % (i + 1); client t = clients[i]; clients[i] = clients[j]; clients[j] = t; }
    for (int i = 0; i < n_clients; i++) if (clients[i].role == R_PLAYER) clients[i].game->players[clients[i].color] = &clients[i];

    redis = resp_new(L, "127.0.0.1", O.redis_port, fake_lila, fake_state, NULL);
    for (int i = 0; i < 16; i++) { char ch[16]; snprintf(ch, sizeof ch, "r-in:%d", i); resp_subscribe(redis, ch); }
    resp_subscribe(redis, "site-in");
    resp_subscribe(redis, "lobby-in");
    resp_start(redis);

    read_usage(O.pid, &u_idle);
    t_start_us = now_us();
    cb_loop_timer(L, 200, connect_some, NULL);
    int connect_s = n_clients / (O.connect_rate > 0 ? O.connect_rate : 1) + 2;
    cb_loop_timer(L, (int64_t)(connect_s + O.warmup) * 1000, start_measure, NULL);
    cb_loop_run(L);

    double secs = (double)(t_end_us - t_measure_us) / 1e6;
    hist_sort(&h_mover); hist_sort(&h_watcher); hist_sort(&h_ping);
    long msgs = moves_sent + events_recv + pings_sent + pongs_recv;
    double cpu = (u_after.user_s + u_after.sys_s) - (u_before.user_s + u_before.sys_s);
    printf("wsbench: port %d, %d games x (2 players + %d watchers) + %d site + %d lobby = %d sockets, move every %d ms, ping every %d ms\n",
           O.port, O.games, O.watchers, O.site, O.lobby, n_clients, O.move_ms, O.ping_ms);
    printf("connections: open %d, failed %d, closed %d, connect phase %d s\n", n_open, n_failed, n_closed, connect_s);
    printf("measured %.1f s: moves %ld (%.0f/s), move events received %ld (%.0f/s), pings %ld, pongs %ld, frames in %ld, bytes in %ld out %ld\n",
           secs, moves_sent, moves_sent / secs, events_recv, events_recv / secs, pings_sent, pongs_recv, frames_in, bytes_in, bytes_out);
    printf("fake lila: %ld moves answered, %ld pings answered\n", fake_moves, fake_pings);
    printf("move relay latency, mover   (us): n %zu p50 %u p90 %u p99 %u p999 %u max %llu mean %.0f\n", h_mover.n, hist_pct(&h_mover, 0.5), hist_pct(&h_mover, 0.9),
           hist_pct(&h_mover, 0.99), hist_pct(&h_mover, 0.999), (unsigned long long)h_mover.max, h_mover.n ? h_mover.sum / (double)h_mover.n : 0);
    printf("move relay latency, watcher (us): n %zu p50 %u p90 %u p99 %u p999 %u max %llu mean %.0f\n", h_watcher.n, hist_pct(&h_watcher, 0.5), hist_pct(&h_watcher, 0.9),
           hist_pct(&h_watcher, 0.99), hist_pct(&h_watcher, 0.999), (unsigned long long)h_watcher.max, h_watcher.n ? h_watcher.sum / (double)h_watcher.n : 0);
    printf("ping round trip             (us): n %zu p50 %u p90 %u p99 %u p999 %u max %llu mean %.0f\n", h_ping.n, hist_pct(&h_ping, 0.5), hist_pct(&h_ping, 0.9),
           hist_pct(&h_ping, 0.99), hist_pct(&h_ping, 0.999), (unsigned long long)h_ping.max, h_ping.n ? h_ping.sum / (double)h_ping.n : 0);
    if (O.pid > 0 && u_after.rss_kb) {
        printf("server pid %d: rss %ld KB idle-before-connect, %ld KB at end (%.1f KB per socket); cpu over the window user %.2f s sys %.2f s = %.1f%% of one core, %.1f us per client message (%ld messages)\n",
               (int)O.pid, u_idle.rss_kb, u_after.rss_kb, n_open ? (double)(u_after.rss_kb - u_idle.rss_kb) / n_open : 0.0,
               u_after.user_s - u_before.user_s, u_after.sys_s - u_before.sys_s, 100.0 * cpu / secs, msgs ? cpu * 1e6 / (double)msgs : 0.0, msgs);
    }
    resp_free(redis);
    // Close with a reset, not a FIN: 10k sockets in TIME_WAIT would hold the Mac's 16k ephemeral
    // ports for 30 s, and the next run (the other server's) would find no port to connect from.
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    for (int i = 0; i < n_clients; i++) {
        if (clients[i].fd >= 0) { setsockopt(clients[i].fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg); close(clients[i].fd); }
        cb_buf_free(&clients[i].in); cb_buf_free(&clients[i].out);
    }
    free(clients); free(games);
    cb_loop_free(L);
    return 0;
}

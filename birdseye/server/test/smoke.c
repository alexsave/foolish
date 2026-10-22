// smoke.c - the whole thing end to end: a real `cbase serve` (foolish's app), a real `birdseye
// serve` playing four fake humans through it, the site's stream heard through the platform's
// socket (HELLO, SUB, every push a frame of the wire), the list, a detail and the ops record
// answering, a game finished, no hostile move taken, every move the server took.
//
//   build/test/smoke build/birdseye ../../c_base/build/cbase
#include "lib_client.h"
#include "lib_proc.h"
#include "net/ws.h"
#include "t.h"
#include "util/clock.h"
#include "wire.h"
#include "../../../shared/c/stream_frame.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NW_SUB_GAME 2
#define PLAYERS 4
#define GAME_WAIT_MS 150000

static char g_dir[256];

static int port_of(tp_child *c) {
    char line[512];
    if (tp_wait_line(c, "listening on 127.0.0.1:", 30000, line, sizeof line) != 0) return -1;
    const char *p = strstr(line, "listening on 127.0.0.1:");
    return atoi(p + strlen("listening on 127.0.0.1:"));
}

static int start_game(tp_child *c, const char *bin) {
    static char db[600];
    snprintf(db, sizeof db, "CBASE_DB=%s/game.sqlite", g_dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_LOG_LEVEL=info", db,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", "CBASE_RATED_MIN_AGE=0", "CBASE_BOT_SPEEDUP=20",
                    "CBASE_LIMIT_SIGNUP_IP=1000000", "CBASE_LIMIT_SIGNUP_ALL=1000000", "CBASE_LIMIT_CREATE_USER=1000000",
                    "CBASE_LIMIT_SUB_USER=1000000", "CBASE_LIMIT_BUMP_USER=1000000", "CBASE_REQ_WORKERS=2", NULL };
    char *argv[] = { (char *)bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    return port_of(c);
}

static int start_birdseye(tp_child *c, const char *bin, int game_port, pid_t game_pid) {
    static char db[600], server[64], spid[64];
    snprintf(db, sizeof db, "CBASE_DB=%s/birdseye.sqlite", g_dir);
    snprintf(server, sizeof server, "BE_SERVER=127.0.0.1:%d", game_port);
    snprintf(spid, sizeof spid, "BE_SERVER_PID=%d", (int)game_pid);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_LOG_LEVEL=info", db,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", server, spid,
                    "BE_PLAYERS=4", "BE_TABLES=2", "BE_THREADS=2", "BE_THINK_MS=20-80", "BE_SEED=3",
                    "BE_BOT_SHARE=50", "BE_PODKIDNOY=50", "BE_HOSTILE_EVERY=10", "BE_REMATCH=50", NULL };
    char *argv[] = { (char *)bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    return port_of(c);
}

static int g_port;

static int fetch(const char *path, tc_response *r) { return tc_http(g_port, "GET", path, NULL, NULL, 0, r); }

static void session_token(char *out, size_t cap) {
    tc_response r;
    out[0] = 0;
    T_EQ_INT(tc_http(g_port, "POST", "/be/session", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(r.body.len, sizeof(be_w_session));
    if (r.body.len == sizeof(be_w_session)) {
        be_w_session s;
        memcpy(&s, r.body.p, sizeof s);
        T_EQ_INT(s.hash, be_wire_hash());
        T_CHECK(s.token_len > 20 && s.token_len <= BE_TOKEN_MAX);
        snprintf(out, cap, "%.*s", (int)s.token_len, s.token);
    }
    tc_response_free(&r);
    // The second call signs the same account in again: a new token, the same shape.
    T_EQ_INT(tc_http(g_port, "POST", "/be/session", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    tc_response_free(&r);
}

// Opens the stream as the site does: HELLO, READY, SUB "all", REPLY 200. The socket, or -1.
static int open_stream(const char *token) {
    int fd = tc_ws_open(g_port, "/ws", NULL);
    T_CHECK(fd >= 0);
    if (fd < 0) return -1;
    uint8_t frame[SF_TOKEN_MAX + 16];
    int n = sf_put_hello(frame, sizeof frame, token, strlen(token));
    T_CHECK(n > 0 && tc_ws_send(fd, CB_WS_OP_BIN, frame, (size_t)n) == 0);
    cb_buf in;
    cb_buf_init(&in);
    sf_frame f;
    T_CHECK(tc_ws_recv(fd, 10000, &in) == CB_WS_OP_BIN && sf_parse(in.p, in.len, &f) == 0 && f.kind == SF_READY);
    n = sf_put_sub(frame, sizeof frame, SF_SUB, 77, NW_SUB_GAME, "all", 3);
    T_CHECK(n > 0 && tc_ws_send(fd, CB_WS_OP_BIN, frame, (size_t)n) == 0);
    bool replied = false;
    for (int tries = 0; tries < 50 && !replied; tries++) {   // pushes may come before the reply
        cb_buf_reset(&in);
        if (tc_ws_recv(fd, 10000, &in) != CB_WS_OP_BIN) break;
        if (sf_parse(in.p, in.len, &f) == 0 && f.kind == SF_REPLY) {
            T_EQ_INT(f.id, 77);
            T_EQ_INT(f.status, 200);
            replied = true;
        }
    }
    T_CHECK(replied);
    cb_buf_free(&in);
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: smoke <birdseye> <cbase>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    T_CHECK(tp_tempdir(g_dir, sizeof g_dir) == 0);
    tp_child game, be;
    int game_port = start_game(&game, argv[2]);
    T_CHECK(game_port > 0);
    if (game_port <= 0) return t_done("smoke");
    g_port = start_birdseye(&be, argv[1], game_port, game.pid);
    T_CHECK(g_port > 0);
    if (g_port <= 0) {
        tp_stop(&game, SIGTERM, 5000);
        return t_done("smoke");
    }

    tc_response r;
    T_CHECK(fetch("/health", &r) == 0 && r.status == 200);
    tc_response_free(&r);
    // The ops record before anything was played: the configuration, no games.
    T_CHECK(fetch("/be/ops", &r) == 0 && r.status == 200 && r.body.len == sizeof(be_w_ops));
    be_w_ops ops;
    memcpy(&ops, r.body.p, sizeof ops);
    tc_response_free(&r);
    T_EQ_INT(ops.hash, be_wire_hash());
    T_EQ_INT(ops.players, PLAYERS);
    T_EQ_INT(ops.threads, 2);
    T_EQ_INT(ops.hostile_accepted, 0);

    char token[BE_TOKEN_MAX + 1];
    session_token(token, sizeof token);
    int fd = open_stream(token);
    if (fd < 0) {
        tp_stop(&be, SIGTERM, 5000);
        tp_stop(&game, SIGTERM, 5000);
        return t_done("smoke");
    }

    // Hear the stream until a game is over: every push is one frame of the wire, exactly sized.
    int n_ops = 0, n_tables = 0, n_events = 0, n_over = 0, n_bad = 0, n_hostile = 0, n_hostile_taken = 0, n_dealt = 0;
    char over_gid[BE_GID + 1] = "";
    cb_buf in;
    cb_buf_init(&in);
    int64_t until = cb_mono_ms() + GAME_WAIT_MS;
    while (cb_mono_ms() < until && (n_over == 0 || n_ops == 0)) {
        cb_buf_reset(&in);
        int op = tc_ws_recv(fd, 5000, &in);
        if (op == CB_WS_OP_PING) continue;
        if (op != CB_WS_OP_BIN) {
            if (op < 0) continue;   // a timeout: the game is slow, keep waiting
            break;
        }
        if (!sf_is_push(in.p, in.len)) continue;   // a PONG, a late reply
        const uint8_t *p = in.p;
        if (p[1] == BE_F_OPS && in.len == 2 + sizeof(be_w_ops)) {
            be_w_ops o;
            memcpy(&o, p + 2, sizeof o);
            if (o.hash == be_wire_hash()) n_ops++; else n_bad++;
        } else if (p[1] == BE_F_TABLE && in.len == 2 + sizeof(be_w_table)) {
            be_w_table t;
            memcpy(&t, p + 2, sizeof t);
            if (t.gid_len > 0 && t.gid_len <= BE_GID && t.num_players <= BE_SEATS && t.num_battles <= BE_BATTLES) n_tables++;
            else n_bad++;
        } else if (p[1] == BE_F_EVENT && in.len == 2 + sizeof(be_w_event)) {
            be_w_event e;
            memcpy(&e, p + 2, sizeof e);
            n_events++;
            if (e.kind == BE_EV_DEALT) n_dealt++;
            if (e.kind == BE_EV_HOSTILE) {
                n_hostile++;
                if (!e.refused) n_hostile_taken++;
            }
            if (e.kind == BE_EV_OVER) {
                n_over++;
                snprintf(over_gid, sizeof over_gid, "%.*s", (int)e.gid_len, e.gid);
            }
        } else n_bad++;
    }
    cb_buf_free(&in);
    close(fd);
    fprintf(stderr, "smoke: heard %d ops, %d tables, %d events (%d dealt, %d over, %d hostile), %d bad frames\n", n_ops, n_tables,
            n_events, n_dealt, n_over, n_hostile, n_bad);
    T_CHECK(n_ops > 0);
    T_CHECK(n_tables > 10);
    T_CHECK(n_events > 10);
    T_CHECK(n_dealt >= 1);
    T_EQ_INT(n_over >= 1, 1);
    T_EQ_INT(n_bad, 0);
    T_EQ_INT(n_hostile_taken, 0);

    // The list: a head with this build's hash and sizes, then rows; the finished table is in it.
    T_CHECK(fetch("/be/tables", &r) == 0 && r.status == 200 && r.body.len >= sizeof(be_w_list));
    be_w_list head;
    memcpy(&head, r.body.p, sizeof head);
    T_EQ_INT(head.hash, be_wire_hash());
    T_EQ_INT(head.first, sizeof(be_w_list));
    T_EQ_INT(head.size, sizeof(be_w_table));
    T_EQ_INT(r.body.len, head.first + (size_t)head.n * head.size);
    T_CHECK(head.n >= 1);
    bool found = false;
    for (uint32_t i = 0; i < head.n; i++) {
        be_w_table t;
        memcpy(&t, r.body.p + head.first + i * head.size, sizeof t);
        if (t.gid_len == strlen(over_gid) && memcmp(t.gid, over_gid, t.gid_len) == 0) {
            found = true;
            T_CHECK(t.phase == BE_PHASE_OVER || t.phase == BE_PHASE_GONE || t.phase == BE_PHASE_LOBBY || t.phase == BE_PHASE_PLAYING);
            T_CHECK(t.n_seats >= 2 && t.n_seats <= BE_SEATS);
            T_CHECK(t.num_players >= 1);
        }
    }
    T_CHECK(found);
    tc_response_free(&r);

    // One table's detail: the row again, its hands (a sim seat's hand is known), its events.
    char path[64];
    snprintf(path, sizeof path, "/be/table/%s", over_gid);
    T_CHECK(fetch(path, &r) == 0 && r.status == 200 && r.body.len == sizeof(be_w_detail));
    if (r.body.len == sizeof(be_w_detail)) {
        be_w_detail *d = malloc(sizeof *d);
        memcpy(d, r.body.p, sizeof *d);
        T_EQ_INT(d->table.gid_len, strlen(over_gid));
        T_CHECK(d->n_events >= 1 && d->n_events <= BE_EVENTS);
        bool over_seen = false;
        for (int i = 0; i < d->n_events; i++) {
            if (d->events[i].kind == BE_EV_OVER) over_seen = true;
            T_EQ_INT(d->events[i].gid_len, strlen(over_gid));
        }
        T_CHECK(over_seen);
        for (int i = 0; i < d->table.num_players; i++) T_CHECK(d->hands[i].n <= BE_HAND);
        free(d);
    }
    tc_response_free(&r);
    T_CHECK(fetch("/be/table/nope", &r) == 0 && r.status == 404);
    tc_response_free(&r);

    // The ops record after: games finished, moves taken, nothing refused, nothing hostile taken,
    // the server's memory read through its pid.
    T_CHECK(fetch("/be/ops", &r) == 0 && r.status == 200 && r.body.len == sizeof(be_w_ops));
    memcpy(&ops, r.body.p, sizeof ops);
    tc_response_free(&r);
    T_CHECK(ops.games_total >= 1);
    T_CHECK(ops.moves_total > 10);
    T_EQ_INT(ops.refused_total, 0);
    T_EQ_INT(ops.hostile_accepted, 0);
    T_CHECK(ops.hostile_total >= 1);
    T_EQ_INT(ops.stalls_total, 0);
    T_EQ_INT(ops.sockets, PLAYERS);
    T_CHECK(ops.act_n > 10);
    T_CHECK(ops.act_p50_us > 0 && ops.act_p99_us >= ops.act_p50_us && ops.act_max_us >= ops.act_p99_us);
    T_CHECK(ops.push_p50_us > 0);
    T_CHECK(ops.server_rss_mb > 0);
    T_CHECK(ops.self_rss_mb > 0);
    T_CHECK(ops.uptime_s >= 1);

    // A clean stop: the players leave their tables and the process exits 0.
    T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
    T_EQ_INT(tp_stop(&game, SIGTERM, 15000), 0);
    return t_done("smoke");
}

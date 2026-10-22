// smoke_league.c - the bot league end to end: a real `cbase serve` (foolish's app, dev mode, its
// admin token, the bots sped up), a real `birdseye serve` in league mode, the site's stream heard.
// Tables of bots alone are dealt, played to their fool and rated; every result the league keeps is
// the server's stamp (after = before + delta, the bots' rows equal the server's leaderboard); the
// stream carries each result and the bots' new standings; the history survives a restart.
//
//   build/test/smoke_league build/birdseye ../../c_base/build/cbase
#include "lib_client.h"
#include "lib_proc.h"
#include "net/ws.h"
#include "t.h"
#include "util/clock.h"
#include "netwire.h"
#include "wire.h"
#include "../../../shared/c/stream_frame.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADMIN "smoke-league-admin-token-0123456789abcdef"
#define WANT_RESULTS 4
#define WAIT_MS 240000

static char g_dir[256];
static int g_port, g_game_port;

static int port_of(tp_child *c) {
    char line[512];
    if (tp_wait_line(c, "listening on 127.0.0.1:", 30000, line, sizeof line) != 0) return -1;
    const char *p = strstr(line, "listening on 127.0.0.1:");
    return atoi(p + strlen("listening on 127.0.0.1:"));
}

static int start_game_server(tp_child *c, const char *bin) {
    static char db[600];
    snprintf(db, sizeof db, "CBASE_DB=%s/game.sqlite", g_dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_LOG_LEVEL=info", db, "CBASE_ADMIN_TOKEN=" ADMIN,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", "CBASE_RATED_MIN_AGE=0", "CBASE_BOT_SPEEDUP=50",
                    "CBASE_LIMIT_SIGNUP_IP=1000000", "CBASE_LIMIT_SUB_USER=1000000", "CBASE_REQ_WORKERS=2", NULL };
    char *argv[] = { (char *)bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    return port_of(c);
}

static int start_birdseye(tp_child *c, const char *bin) {
    static char db[600], server[64], file[600];
    snprintf(db, sizeof db, "CBASE_DB=%s/birdseye.sqlite", g_dir);
    snprintf(server, sizeof server, "BE_SERVER=127.0.0.1:%d", g_game_port);
    snprintf(file, sizeof file, "BE_LEAGUE_FILE=%s/league.bin", g_dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_LOG_LEVEL=info", db,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", server, file, "BE_ADMIN_TOKEN=" ADMIN,
                    "BE_LEAGUE_TABLES=3", "BE_PODKIDNOY=50", "BE_SEED=5", NULL };
    char *argv[] = { (char *)bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    return port_of(c);
}

static int open_stream(void) {
    tc_response r;
    char token[BE_TOKEN_MAX + 1] = "";
    if (tc_http(g_port, "POST", "/be/session", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len == sizeof(be_w_session)) {
        be_w_session s;
        memcpy(&s, r.body.p, sizeof s);
        snprintf(token, sizeof token, "%.*s", (int)s.token_len, s.token);
    }
    tc_response_free(&r);
    T_CHECK(token[0]);
    int fd = tc_ws_open(g_port, "/ws", NULL);
    T_CHECK(fd >= 0);
    if (fd < 0) return -1;
    uint8_t frame[SF_TOKEN_MAX + 16];
    int n = sf_put_hello(frame, sizeof frame, token, strlen(token));
    tc_ws_send(fd, CB_WS_OP_BIN, frame, (size_t)n);
    n = sf_put_sub(frame, sizeof frame, SF_SUB, 9, NW_SUB_GAME, "all", 3);
    tc_ws_send(fd, CB_WS_OP_BIN, frame, (size_t)n);
    return fd;
}

typedef struct {
    be_w_league h;
    be_w_bot    bots[BE_LEAGUE_BOTS];
    be_w_result *results;
} league;

static bool get_league(league *L) {
    tc_response r;
    bool ok = false;
    memset(L, 0, sizeof *L);
    if (tc_http(g_port, "GET", "/be/league", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len >= sizeof(be_w_league)) {
        memcpy(&L->h, r.body.p, sizeof L->h);
        T_EQ_INT(L->h.hash, be_wire_hash());
        T_EQ_INT(L->h.bot_size, sizeof(be_w_bot));
        T_EQ_INT(L->h.result_size, sizeof(be_w_result));
        T_EQ_INT(r.body.len, L->h.results_at + (size_t)L->h.n_results * L->h.result_size);
        if (L->h.n_bots <= BE_LEAGUE_BOTS && r.body.len == L->h.results_at + (size_t)L->h.n_results * L->h.result_size) {
            memcpy(L->bots, r.body.p + L->h.bots_at, (size_t)L->h.n_bots * sizeof(be_w_bot));
            L->results = malloc((size_t)L->h.n_results * sizeof(be_w_result) + 1);
            memcpy(L->results, r.body.p + L->h.results_at, (size_t)L->h.n_results * sizeof(be_w_result));
            ok = true;
        }
    }
    tc_response_free(&r);
    return ok;
}

// The server's own leaderboard row of a bot: rating and games.
static bool board_of(const be_w_bot *b, uint32_t *rating, uint32_t *games) {
    tc_response r;
    bool found = false;
    if (tc_http(g_game_port, "GET", "/v1/leaderboard", NULL, NULL, 0, &r) == 0 && r.status == 200) {
        int n = nw_list_open(r.body.p, (int)r.body.len, NW_BOARD);
        for (int i = 0; i < n && !found; i++) {
            NwBoardRow row;
            if (nw_get_board(r.body.p, (int)r.body.len, i, &row) == NW_OK && row.is_bot && row.id_len == b->id_len &&
                memcmp(row.id, b->id, b->id_len) == 0) {
                *rating = row.rating;
                *games = row.games;
                found = true;
            }
        }
    }
    tc_response_free(&r);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: smoke_league <birdseye> <cbase>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    T_CHECK(tp_tempdir(g_dir, sizeof g_dir) == 0);
    tp_child game, be;
    g_game_port = start_game_server(&game, argv[2]);
    T_CHECK(g_game_port > 0);
    if (g_game_port <= 0) return t_done("smoke_league");
    g_port = start_birdseye(&be, argv[1]);
    T_CHECK(g_port > 0);
    if (g_port <= 0) {
        tp_stop(&game, SIGTERM, 5000);
        return t_done("smoke_league");
    }
    int fd = open_stream();

    // Hear the stream until WANT_RESULTS games are rated: every result and bot row a frame of
    // the wire; tables of bots only (every seat flagged a bot); the events of a dealt game.
    int n_results = 0, n_bot_rows = 0, n_tables = 0, n_human_seats = 0, n_dealt = 0, n_bad = 0;
    int sizes_seen = 0;
    cb_buf in;
    cb_buf_init(&in);
    int64_t until = cb_mono_ms() + WAIT_MS;
    while (fd >= 0 && cb_mono_ms() < until && n_results < WANT_RESULTS) {
        cb_buf_reset(&in);
        int op = tc_ws_recv(fd, 5000, &in);
        if (op != CB_WS_OP_BIN) {
            if (op < 0 || op == CB_WS_OP_PING) continue;
            break;
        }
        if (!sf_is_push(in.p, in.len)) continue;
        const uint8_t *p = in.p;
        if (p[1] == BE_F_RESULT && in.len == 2 + sizeof(be_w_result)) {
            be_w_result r;
            memcpy(&r, p + 2, sizeof r);
            n_results++;
            T_CHECK(r.n >= 2 && r.n <= BE_SEATS);
            sizes_seen |= 1 << r.n;
            for (int s = 0; s < r.n; s++) T_EQ_INT(r.after[s], r.before[s] + r.delta[s]);
            if (r.fool >= 0) T_CHECK(r.delta[r.fool] < 0);
        } else if (p[1] == BE_F_BOT && in.len == 2 + sizeof(be_w_bot)) {
            n_bot_rows++;
        } else if (p[1] == BE_F_TABLE && in.len == 2 + sizeof(be_w_table)) {
            be_w_table t;
            memcpy(&t, p + 2, sizeof t);
            n_tables++;
            for (int s = 0; s < t.num_players && s < BE_SEATS; s++)
                if (!(t.seats[s].flags & BE_SEAT_BOT)) n_human_seats++;
        } else if (p[1] == BE_F_EVENT && in.len == 2 + sizeof(be_w_event)) {
            be_w_event e;
            memcpy(&e, p + 2, sizeof e);
            if (e.kind == BE_EV_DEALT) n_dealt++;
        } else if (p[1] != BE_F_OPS) n_bad++;
    }
    cb_buf_free(&in);
    if (fd >= 0) close(fd);
    fprintf(stderr, "smoke_league: %d results (sizes mask %#x), %d bot rows, %d table rows, %d dealt, %d bad\n", n_results, sizes_seen,
            n_bot_rows, n_tables, n_dealt, n_bad);
    T_CHECK(n_results >= WANT_RESULTS);
    T_CHECK(n_bot_rows >= 2 * n_results);
    T_CHECK(n_tables > 10);
    T_EQ_INT(n_human_seats, 0);
    T_CHECK(n_dealt >= 1);
    T_EQ_INT(n_bad, 0);

    // The standings are the server's: every bot that played reads the leaderboard's rating and
    // games, and its last result's `after`.
    league L;
    T_CHECK(get_league(&L));
    T_EQ_INT(L.h.mode, BE_MODE_LEAGUE);
    T_CHECK(L.h.n_bots >= 2);
    T_CHECK(L.h.n_results >= (uint32_t)n_results);
    int played = 0;
    for (uint32_t i = 0; i < L.h.n_bots; i++) {
        const be_w_bot *b = &L.bots[i];
        if (b->league_games == 0 || b->seated) continue;   // a seated bot may be mid-commit
        played++;
        uint32_t rating = 0, games = 0;
        T_CHECK(board_of(b, &rating, &games));
        T_EQ_INT(b->rating, (int32_t)rating);
        T_EQ_INT(b->games, games);
        int32_t last_after = -1;
        uint32_t counted = 0;
        for (uint32_t k = 0; k < L.h.n_results; k++)
            for (int s = 0; s < L.results[k].n; s++)
                if (L.results[k].bot[s] == i) {
                    last_after = L.results[k].after[s];
                    counted++;
                }
        T_EQ_INT(b->rating, last_after);
        T_EQ_INT(b->league_games, counted);
    }
    T_CHECK(played >= 2);
    for (uint32_t k = 0; k < L.h.n_results; k++) T_EQ_INT(L.results[k].seq, k + 1);
    uint32_t before_restart = L.h.n_results;
    free(L.results);

    // A restart keeps the history (the file), in order.
    T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
    g_port = start_birdseye(&be, argv[1]);
    T_CHECK(g_port > 0);
    T_CHECK(get_league(&L));
    T_CHECK(L.h.n_results >= before_restart);
    free(L.results);

    T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
    T_EQ_INT(tp_stop(&game, SIGTERM, 15000), 0);
    return t_done("smoke_league");
}

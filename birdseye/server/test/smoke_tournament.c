// smoke_tournament.c - a whole tournament end to end (tournament.h), at the bots-only pace sped up:
// 48 bots drawn and seeded, every match played to three wins by real rated server games through the
// dev-only bot table, no round begun before the one below it was decided, the champion crowned, the bracket on the stream and at GET /be/tournament, and
// the same bracket read back from its file by a birdseye restarted in league mode.
//
// (Helpers shared in shape with smoke_league.c.)
//
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
static char *g_mode = "BE_MODE=tournament";

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
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", "CBASE_RATED_MIN_AGE=0", "CBASE_BOT_SPEEDUP=100",
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
    static char tour[600];
    snprintf(tour, sizeof tour, "BE_TOURNAMENT_FILE=%s/tournament.bin", g_dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_LOG_LEVEL=info", db,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", server, file, "BE_ADMIN_TOKEN=" ADMIN,
                    g_mode, tour, "BE_LEAGUE_TABLES=16", "BE_SEED=5", NULL };
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

static bool get_tour(be_w_tour *t) {
    tc_response r;
    bool ok = tc_http(g_port, "GET", "/be/tournament", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len == sizeof *t;
    if (ok) memcpy(t, r.body.p, sizeof *t);
    tc_response_free(&r);
    return ok;
}

// The rounds are barriers (tournament.h): no match is live while a match of a lower round is undecided.
static bool barrier_holds(const be_w_tour *t) {
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        if (!t->matches[i].live) continue;
        for (int j = 0; j < BE_TOUR_MATCHES; j++)
            if (t->matches[j].round < t->matches[i].round && t->matches[j].winner == BE_TOUR_NONE) return false;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: smoke_tournament <birdseye> <cbase>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    T_CHECK(tp_tempdir(g_dir, sizeof g_dir) == 0);
    tp_child game, be;
    g_game_port = start_game_server(&game, argv[2]);
    T_CHECK(g_game_port > 0);
    if (g_game_port <= 0) return t_done("smoke_tournament");
    g_port = start_birdseye(&be, argv[1]);
    T_CHECK(g_port > 0);
    int fd = open_stream();
    // The bracket on the stream as it changes, until the champion is crowned.
    static be_w_tour t;
    int n_frames = 0, broken = 0;
    bool done = false;
    cb_buf in;
    cb_buf_init(&in);
    int64_t until = cb_mono_ms() + 600000, ping = cb_mono_ms() + 15000;
    while (fd >= 0 && cb_mono_ms() < until && !done) {
        if (cb_mono_ms() >= ping) {   // the platform closes a stream that says nothing for a minute
            uint8_t pg[2];
            int pn = sf_put_bare(pg, sizeof pg, SF_PING);
            tc_ws_send(fd, CB_WS_OP_BIN, pg, (size_t)pn);
            ping = cb_mono_ms() + 15000;
        }
        cb_buf_reset(&in);
        int op = tc_ws_recv(fd, 5000, &in);
        if (op != CB_WS_OP_BIN) {
            if (op < 0 || op == CB_WS_OP_PING) continue;
            break;
        }
        if (!sf_is_push(in.p, in.len) || in.p[1] != BE_F_TOUR) continue;
        T_EQ_INT(in.len, 2 + sizeof t);
        if (in.len != 2 + sizeof t) break;
        memcpy(&t, in.p + 2, sizeof t);
        n_frames++;
        if (!barrier_holds(&t)) broken++;
        done = t.state == BE_TOUR_DONE;
    }
    cb_buf_free(&in);
    if (fd >= 0) close(fd);
    fprintf(stderr, "smoke_tournament: %d bracket frames, %u games, champion %d\n", n_frames, t.games, t.champion);
    T_CHECK(done);
    T_CHECK(n_frames > 47);
    T_EQ_INT(broken, 0);   // on every frame, at the bots' speed
    // Every match decided at three wins (or by the drawn-out rule), each winner sent on.
    uint32_t games = 0;
    int by_wins = 0;
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        const be_w_match *m = &t.matches[i];
        if (m->wins_a == BE_TOUR_WIN || m->wins_b == BE_TOUR_WIN) by_wins++;
        T_CHECK(m->winner != BE_TOUR_NONE && (m->winner == m->a || m->winner == m->b));
        T_CHECK(m->wins_a == BE_TOUR_WIN || m->wins_b == BE_TOUR_WIN || m->games >= 9);
        T_EQ_INT(m->live, 0);
        games += m->games;
    }
    T_EQ_INT(games, t.games);
    T_CHECK(by_wins >= 40);   // a drawn 2p game is rare: nearly every match is won at three
    T_EQ_INT(t.champion, t.matches[BE_TOUR_MATCHES - 1].winner);
    T_CHECK(t.dropped != BE_TOUR_NONE);
    // Every game was a rated server game: the league's history holds at least as many results.
    league L;
    T_CHECK(get_league(&L));
    T_CHECK(L.h.n_results >= t.games);
    T_EQ_INT(L.h.mode, BE_MODE_TOURNAMENT);
    // Two-seat games only.
    for (uint32_t i = 0; i < L.h.n_results; i++) T_EQ_INT(L.results[i].n, 2);
    // The champion's standing is the server's.
    uint32_t rating = 0, sgames = 0;
    if (t.champion < L.h.n_bots) {
        T_CHECK(board_of(&L.bots[t.champion], &rating, &sgames));
        T_EQ_INT(L.bots[t.champion].rating, (int32_t)rating);
    }
    free(L.results);

    // Every table's first view came (16 opened at once at the start): none was given up unseen.
    char gave_up[512];
    T_CHECK(tp_wait_line(&be, "no view of", 10, gave_up, sizeof gave_up) != 0);
    // Restarted in league mode: the same bracket, from its file.
    T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
    g_mode = "BE_MODE=league";
    g_port = start_birdseye(&be, argv[1]);
    T_CHECK(g_port > 0);
    static be_w_tour back;
    T_CHECK(get_tour(&back));
    T_EQ_INT(back.state, BE_TOUR_DONE);
    T_EQ_INT(back.champion, t.champion);
    T_EQ_INT(back.games, t.games);
    T_EQ_MEM(back.matches, sizeof back.matches, t.matches, sizeof t.matches);
    T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
    T_EQ_INT(tp_stop(&game, SIGTERM, 15000), 0);
    return t_done("smoke_tournament");
}

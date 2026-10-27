// smoke_gauntlet.c - the gauntlet (gauntlet.h), end to end: a real `cbase serve` (dev mode, the bots
// sped up) and a real `birdseye serve` in gauntlet mode with a human account. The ladder is the
// seven brains, one bot each, each its brain's best by the server's ratings, in ascending order; rung
// 1's game is a locked two-seat lobby of his titled for the rung; the admin route is refused without
// the token and while a run is going on; he plays (the handwritten brain's moves on rung 1, then a
// defender who always picks up, to lose on purpose) game after game in the real lobby, each next game
// a new table; a birdseye restart in the middle of a rung reads the run back from its file and
// watches the same lobby again; a rung won climbs (the next rung's lobby is titled for it), a rung
// lost ends the run where it stood, and the admin route then starts run 2 at rung 1 with the best
// run kept.
//
//   build/test/smoke_gauntlet build/birdseye ../../c_base/build/cbase
#include "board.h"
#include "client.h"
#include "gauntlet.h"
#include "lib_client.h"
#include "lib_proc.h"
#include "t.h"
#include "util/clock.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "legal.h"
#include "netwire.h"
#include "table.h"
#include "wire.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADMIN    "smoke-gauntlet-admin-token-0123456789abcdef"
#define HUMAN    "GauntletHuman"
#define PASSWORD "smoke-gauntlet-pw-5e1d0b"

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
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", "CBASE_RATED_MIN_AGE=0", "CBASE_BOT_SPEEDUP=100",
                    "CBASE_LIMIT_SIGNUP_IP=1000000", "CBASE_LIMIT_SIGNIN_IP=1000000", "CBASE_LIMIT_SUB_USER=1000000",
                    "CBASE_REQ_WORKERS=2", NULL };
    char *argv[] = { (char *)bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    return port_of(c);
}

static int start_birdseye(tp_child *c, const char *bin) {
    static char db[600], server[64], file[600], tour[600], gaunt[600];
    snprintf(db, sizeof db, "CBASE_DB=%s/birdseye.sqlite", g_dir);
    snprintf(server, sizeof server, "BE_SERVER=127.0.0.1:%d", g_game_port);
    snprintf(file, sizeof file, "BE_LEAGUE_FILE=%s/league.bin", g_dir);
    snprintf(tour, sizeof tour, "BE_TOURNAMENT_FILE=%s/tournament.bin", g_dir);
    snprintf(gaunt, sizeof gaunt, "BE_GAUNTLET_FILE=%s/gauntlet.bin", g_dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_LOG_LEVEL=info", db,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", server, file, tour, gaunt, "BE_ADMIN_TOKEN=" ADMIN,
                    "BE_MODE=gauntlet", "BE_SEED=6", "BE_STALL_MS=1500", "BE_HUMAN_NAME=" HUMAN, "BE_HUMAN_PASSWORD=" PASSWORD, NULL };
    char *argv[] = { (char *)bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    return port_of(c);
}

static bool get_gauntlet(be_w_gauntlet *g) {
    tc_response r;
    bool ok = tc_http(g_port, "GET", "/be/gauntlet", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len == sizeof *g;
    if (ok) memcpy(g, r.body.p, sizeof *g);
    tc_response_free(&r);
    return ok;
}

static be_w_bot g_bots[BE_LEAGUE_BOTS];
static int g_n_bots;

static bool get_bots(void) {
    tc_response r;
    bool ok = false;
    be_w_league h;
    if (tc_http(g_port, "GET", "/be/league", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len >= sizeof h) {
        memcpy(&h, r.body.p, sizeof h);
        if (h.n_bots <= BE_LEAGUE_BOTS && r.body.len >= h.bots_at + (size_t)h.n_bots * sizeof(be_w_bot)) {
            memcpy(g_bots, r.body.p + h.bots_at, (size_t)h.n_bots * sizeof(be_w_bot));
            g_n_bots = (int)h.n_bots;
            ok = true;
        }
    }
    tc_response_free(&r);
    return ok;
}

static bool key_is(const be_w_bot *b, const char *k) { return b->key_len == strlen(k) && memcmp(b->key, k, b->key_len) == 0; }

static int post_new(const char *token) {
    char headers[256] = "";
    if (token) snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n", token);
    tc_response r;
    if (tc_http(g_port, "POST", "/be/gauntlet/new", token ? headers : NULL, NULL, 0, &r) != 0) return -1;
    int st = r.status;
    tc_response_free(&r);
    return st;
}

// ---- the human, as the web UI talks to the game server ---------------------------------------

static be_client g_h;
static ClientTable g_ct;
static ClientSlot g_slot;
static LegalMoves g_lm;

static int human_req(const char *method, const char *path, const void *body, size_t n, cb_buf *out) {
    char headers[NW_ACCESS_MAX + 128];
    snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n%s", g_h.token, body ? "Content-Type: application/octet-stream\r\n" : "");
    tc_response r;
    if (tc_http(g_game_port, method, path, headers, body, n, &r) != 0) return -1;
    int st = r.status;
    if (out) cb_buf_append(out, r.body.p, r.body.len);
    tc_response_free(&r);
    return st;
}

static bool human_view(const char *gid) {
    char path[96];
    snprintf(path, sizeof path, "/v1/games/%s", gid);
    cb_buf out;
    cb_buf_init(&out);
    bool ok = false;
    uint32_t version;
    NwBlob env;
    if (human_req("GET", path, NULL, 0, &out) == 200 && nw_view_decode(out.p, (int)out.len, &version, &env) == NW_OK && env.len) {
        client_init(&g_ct, &g_slot);
        ok = client_adopt_envelope(&g_ct, env.p, (int)env.len) >= 0;
    }
    cb_buf_free(&out);
    return ok;
}

static int human_start(const char *gid) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = ASK_START;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    return n > 0 ? human_req("POST", "/v1/meta", body, (size_t)n, NULL) : -1;
}

// A pickup whenever the board offers one (a defender who always takes the table loses).
static int pickup_wire(uint8_t *wire, int cap) {
    if (g_ct.view.my_seat < 0 || g_ct.view.status != GAME_STATUS_PLAYING) return 0;
    calculate_legal_moves(g_ct.g, g_ct.view.my_seat, &g_lm);
    for (int k = 0; k < g_lm.n; k++) {
        if (g_lm.moves[k].type != MOVE_PICKUP) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = MOVE_PICKUP;
        int n = awire_encode(&a, wire, cap);
        if (n > 0 && client_validate(&g_ct, &g_ct.view, wire, n) == 0) return n;
    }
    return 0;
}

static bool human_move(const char *gid, uint64_t *rng, bool to_lose) {
    uint8_t wire[2 + 2 * AWIRE_MAX_CARDS], buf[NW_GID_MAX + 8 + sizeof wire];
    int wn = to_lose ? pickup_wire(wire, sizeof wire) : 0;
    if (wn <= 0) wn = be_legal_wire(&g_ct, &g_lm, rng, 0, wire, sizeof wire);
    if (wn <= 0) return false;
    int gl = (int)strlen(gid);
    memcpy(buf, gid, (size_t)gl);
    memcpy(buf + gl, wire, (size_t)wn);
    int n = table_request_encode(buf, sizeof buf, gl, wn, true, g_ct.view.version);
    cb_buf out;
    cb_buf_init(&out);
    TableResponse r;
    bool ok = n > 0 && human_req("POST", "/v1/action", buf, (size_t)n, &out) == 200 && table_response_decode(out.p, (int)out.len, &r) == 0 &&
              r.status == TABLE_STATUS_APPLIED;
    cb_buf_free(&out);
    return ok;
}

// The current rung's live game: waits for birdseye to open it. Its gid into out, or false.
static bool live_gid(be_w_gauntlet *g, char *out, const char *not_this, int ms) {
    int64_t until = cb_mono_ms() + ms;
    while (cb_mono_ms() < until) {
        if (get_gauntlet(g) && g->state == BE_GAUNT_RUNNING && g->rung < BE_GAUNT_RUNGS) {
            const be_w_rung *x = &g->rungs[g->rung];
            if (x->live && x->gid_len && (!not_this || strlen(not_this) != x->gid_len || memcmp(not_this, x->gid, x->gid_len) != 0)) {
                memcpy(out, x->gid, x->gid_len);
                out[x->gid_len] = 0;
                return true;
            }
        }
        usleep(200000);
    }
    return false;
}

// The expected title of the current rung's next game.
static void want_title(const be_w_gauntlet *g, char *out, int cap) {
    const be_w_bot *b = &g_bots[g->rungs[g->rung].bot];
    int skip = b->name_len && b->name[0] == '%';
    be_gaunt_game_title(g, b->name + skip, b->name_len - skip, out, cap);
}

// One game of his in the lobby `gid`: the lobby checked (two seats, locked, titled for the rung),
// Ready pressed, played to the fool. His seat and the fool into *me, *fool.
static bool play_game(const be_w_gauntlet *g, const char *gid, bool to_lose, uint64_t *rng, int *me, int *fool) {
    T_CHECK(human_view(gid));
    T_EQ_INT(g_ct.view.status, GAME_STATUS_WAITING);
    T_EQ_INT(g_ct.view.num_players, 2);
    T_CHECK(g_ct.view.seats_locked);
    char title[96];
    want_title(g, title, sizeof title);
    T_EQ_STR(g_ct.view.title, title);
    *me = g_ct.view.my_seat;
    T_CHECK(*me == 0 || *me == 1);
    // He sits first in the rung's odd games (game 1, 3, 5).
    T_EQ_INT(*me, g->rungs[g->rung].games % 2 == 0 ? 0 : 1);
    T_EQ_INT(human_start(gid), 200);
    int64_t until = cb_mono_ms() + 90000;
    while (cb_mono_ms() < until) {
        if (!human_view(gid)) {
            usleep(100000);
            continue;
        }
        if (g_ct.view.status == GAME_STATUS_GAME_OVER) {
            *fool = g_ct.view.fool;
            return true;
        }
        if (g_ct.view.status == GAME_STATUS_PLAYING) human_move(gid, rng, to_lose);
        usleep(40000);
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: smoke_gauntlet <birdseye> <cbase>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    T_CHECK(tp_tempdir(g_dir, sizeof g_dir) == 0);
    tp_child game, be;
    g_game_port = start_game_server(&game, argv[2]);
    T_CHECK(g_game_port > 0);
    if (g_game_port <= 0) return t_done("smoke_gauntlet");
    g_port = start_birdseye(&be, argv[1]);
    T_CHECK(g_port > 0);
    if (g_port <= 0) {
        tp_stop(&game, SIGTERM, 15000);
        return t_done("smoke_gauntlet");
    }
    T_CHECK(be_client_init(&g_h, HUMAN, PASSWORD));
    T_CHECK(be_client_sign_in(&g_h, g_game_port));

    // THE LADDER: run 1, rung 1, seven brains each once, each its brain's best, ascending.
    be_w_gauntlet g;
    char gid[BE_GID + 1] = { 0 };
    T_CHECK(live_gid(&g, gid, NULL, 15000));
    T_CHECK(get_bots());
    T_EQ_INT(g.state, BE_GAUNT_RUNNING);
    T_EQ_INT(g.run, 1);
    T_EQ_INT(g.rung, 0);
    T_EQ_INT(g.human, g_n_bots - 1);
    T_CHECK(key_is(&g_bots[g.human], "human"));
    bool seen[BE_GAUNT_N_BRAINS] = { false };
    for (int r = 0; r < BE_GAUNT_RUNGS; r++) {
        const be_w_rung *x = &g.rungs[r];
        T_CHECK(x->bot < g_n_bots);
        if (x->bot >= g_n_bots) continue;
        int brain = -1;
        for (int b = 0; b < BE_GAUNT_N_BRAINS; b++)
            if (key_is(&g_bots[x->bot], BE_GAUNT_BRAINS[b])) brain = b;
        T_CHECK(brain >= 0);
        if (brain < 0) continue;
        T_CHECK(!seen[brain]);
        seen[brain] = true;
        for (int i = 0; i < g_n_bots; i++)
            if (i != g.human && key_is(&g_bots[i], BE_GAUNT_BRAINS[brain])) T_CHECK(g_bots[i].rating <= x->rating);
        if (r) T_CHECK(x->rating >= g.rungs[r - 1].rating);
        fprintf(stderr, "smoke_gauntlet: rung %d: %.*s (%.*s) %d\n", r + 1, g_bots[x->bot].name_len, g_bots[x->bot].name,
                g_bots[x->bot].key_len, g_bots[x->bot].key, x->rating);
    }

    // THE ROUTE: the token, and no new run while one is going on.
    T_EQ_INT(post_new(NULL), 401);
    T_EQ_INT(post_new("not-the-token"), 401);
    T_EQ_INT(post_new(ADMIN), 409);

    uint64_t rng = 7;
    bool restarted = false, climbed = false, fell = false;
    int64_t until = cb_mono_ms() + 420000;
    while (cb_mono_ms() < until && !(climbed && fell)) {
        T_CHECK(live_gid(&g, gid, NULL, 20000));
        if (!gid[0]) break;
        be_w_gauntlet before = g;
        const be_w_rung *x = &before.rungs[before.rung];
        int me = -1, fool = -2;
        // He plays to win rung 1 (the handwritten brain against the weakest) and to lose above it.
        bool ok = play_game(&before, gid, before.rung > 0, &rng, &me, &fool);
        T_CHECK(ok);
        if (!ok) break;
        uint8_t want = fool == me ? BE_GAUNT_G_BOT : fool == 1 - me ? BE_GAUNT_G_HUMAN : BE_GAUNT_G_DRAW;
        // The game is recorded: the rung's next result, the score.
        int64_t t_end = cb_mono_ms() + 20000;
        while (cb_mono_ms() < t_end && get_gauntlet(&g) && g.games == before.games) usleep(200000);
        T_EQ_INT(g.games, before.games + 1);
        const be_w_rung *y = &g.rungs[before.rung];
        T_EQ_INT(y->games, x->games + 1);
        T_EQ_INT(y->result[x->games], want);
        T_EQ_INT(y->wins_h, x->wins_h + (want == BE_GAUNT_G_HUMAN));
        T_EQ_INT(y->wins_b, x->wins_b + (want == BE_GAUNT_G_BOT));
        fprintf(stderr, "smoke_gauntlet: run %u rung %d game %d: %s, %d-%d\n", g.run, before.rung + 1, y->games,
                want == BE_GAUNT_G_HUMAN ? "won" : want == BE_GAUNT_G_BOT ? "lost" : "drawn", y->wins_h, y->wins_b);
        if (y->wins_h >= BE_TOUR_WIN) {
            // A rung won climbs; the next rung's game opens as a new table, titled for it.
            T_EQ_INT(g.rung, before.rung + 1);
            T_EQ_INT(g.state, BE_GAUNT_RUNNING);
            T_CHECK(g.best_rung >= before.rung + 2);
            climbed = true;
        } else if (y->wins_b >= BE_TOUR_WIN) {
            // A rung lost ends the run there; nothing more is asked of it.
            T_EQ_INT(g.state, BE_GAUNT_LOST);
            T_EQ_INT(g.rung, before.rung);
            T_EQ_INT(g.rungs[g.rung].live, 0);
            T_CHECK(g.best_rung >= before.rung + 1);
            fell = true;
            uint8_t best = g.best_rung;
            uint32_t run = g.run;
            sleep(10);   // past the linger: still nothing live
            T_CHECK(get_gauntlet(&g));
            T_EQ_INT(g.state, BE_GAUNT_LOST);
            T_EQ_INT(g.rungs[g.rung].live, 0);
            // The admin route: a new run from rung 1, the best run kept.
            T_EQ_INT(post_new(ADMIN), 202);
            T_CHECK(live_gid(&g, gid, NULL, 20000));
            T_EQ_INT(g.run, run + 1);
            T_EQ_INT(g.rung, 0);
            T_EQ_INT(g.games, 0);
            T_EQ_INT(g.best_rung, best);
            continue;
        } else {
            // The rung goes on: its next game is a NEW table.
            char next[BE_GID + 1] = { 0 };
            T_CHECK(live_gid(&g, next, gid, 20000));
            T_CHECK(strcmp(next, gid) != 0);
            if (!restarted) {
                // A RESTART in the middle of the rung: the run read back from its file, the same
                // lobby watched again (not a new table).
                restarted = true;
                be_w_gauntlet keep = g;
                T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
                g_port = start_birdseye(&be, argv[1]);
                T_CHECK(g_port > 0);
                if (g_port <= 0) break;
                T_CHECK(get_gauntlet(&g));
                T_EQ_INT(g.run, keep.run);
                T_EQ_INT(g.rung, keep.rung);
                T_EQ_INT(g.games, keep.games);
                T_EQ_INT(g.rungs[g.rung].games, keep.rungs[keep.rung].games);
                T_CHECK(memcmp(g.rungs[g.rung].result, keep.rungs[keep.rung].result, sizeof g.rungs[0].result) == 0);
                for (int r = 0; r < BE_GAUNT_RUNGS; r++) T_EQ_INT(g.rungs[r].bot, keep.rungs[r].bot);
                sleep(3);
                T_CHECK(get_gauntlet(&g));
                T_EQ_INT(g.rungs[g.rung].live, 1);
                T_CHECK(g.rungs[g.rung].gid_len == strlen(next) && memcmp(g.rungs[g.rung].gid, next, strlen(next)) == 0);
            }
        }
    }
    fprintf(stderr, "smoke_gauntlet: restarted %d, climbed %d, fell %d\n", restarted, climbed, fell);
    T_CHECK(restarted);
    T_CHECK(climbed);
    T_CHECK(fell);
    be_client_free(&g_h);
    T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
    T_EQ_INT(tp_stop(&game, SIGTERM, 15000), 0);
    return t_done("smoke_gauntlet");
}

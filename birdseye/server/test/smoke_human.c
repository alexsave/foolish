// smoke_human.c - a human in the tournament (league.h THE HUMAN), end to end: a real `cbase serve`
// (dev mode, the bots sped up) and a real `birdseye serve` in tournament mode with one table, a
// short stall limit and a human account given seed 32 (whose opening match, 32 v 33, is match 0 and
// so the first and only live table). The human's row is the bot list's last; the bracket holds him
// in seed 32 and match 0; the server seated him UNREADY (format 2) and the watcher read the lobby;
// the lobby outlasts the stall limit; his Start deals; he plays at a human's pace, a pause longer
// than the stall limit included, to the fool; the bracket and the league's history record the game
// with his row in a seat. And on every bracket frame: no round-1 match is live while match 0 is
// undecided (the rounds are barriers, tournament.h).
//
//   build/test/smoke_human build/birdseye ../../c_base/build/cbase
#include "board.h"
#include "client.h"
#include "lib_client.h"
#include "lib_proc.h"
#include "net/ws.h"
#include "t.h"
#include "util/clock.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "legal.h"
#include "netwire.h"
#include "table.h"
#include "wire.h"
#include "../../../shared/c/stream_frame.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADMIN    "smoke-human-admin-token-0123456789abcdef"
#define HUMAN    "SMOKEHUMAN"
#define PASSWORD "smoke-human-pw-7f3a9c"
#define STALL_MS 1500
#define PAUSE_US 4000000   // longer than STALL_MS, in the lobby and mid-game

static char g_dir[256];
static int g_port, g_game_port;
static int g_fd = -1;
static cb_buf g_in;
static int g_frames, g_barrier_broken;
static be_w_tour g_last;

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
    static char db[600], server[64], file[600], tour[600];
    snprintf(db, sizeof db, "CBASE_DB=%s/birdseye.sqlite", g_dir);
    snprintf(server, sizeof server, "BE_SERVER=127.0.0.1:%d", g_game_port);
    snprintf(file, sizeof file, "BE_LEAGUE_FILE=%s/league.bin", g_dir);
    snprintf(tour, sizeof tour, "BE_TOURNAMENT_FILE=%s/tournament.bin", g_dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_LOG_LEVEL=info", db,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", server, file, tour, "BE_ADMIN_TOKEN=" ADMIN,
                    "BE_MODE=tournament", "BE_LEAGUE_TABLES=1", "BE_SEED=5", "BE_STALL_MS=1500", "BE_HUMAN_NAME=" HUMAN,
                    "BE_HUMAN_PASSWORD=" PASSWORD, "BE_HUMAN_SEED=32", NULL };
    char *argv[] = { (char *)bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    return port_of(c);
}

static void open_stream(void) {
    tc_response r;
    char token[BE_TOKEN_MAX + 1] = "";
    if (tc_http(g_port, "POST", "/be/session", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len == sizeof(be_w_session)) {
        be_w_session s;
        memcpy(&s, r.body.p, sizeof s);
        snprintf(token, sizeof token, "%.*s", (int)s.token_len, s.token);
    }
    tc_response_free(&r);
    T_CHECK(token[0]);
    g_fd = tc_ws_open(g_port, "/ws", NULL);
    T_CHECK(g_fd >= 0);
    if (g_fd < 0) return;
    uint8_t frame[SF_TOKEN_MAX + 16];
    int n = sf_put_hello(frame, sizeof frame, token, strlen(token));
    tc_ws_send(g_fd, CB_WS_OP_BIN, frame, (size_t)n);
    n = sf_put_sub(frame, sizeof frame, SF_SUB, 9, NW_SUB_GAME, "all", 3);
    tc_ws_send(g_fd, CB_WS_OP_BIN, frame, (size_t)n);
}

// The barrier on a bracket: no match is live while a match of a lower round is undecided.
static bool barrier_holds(const be_w_tour *t) {
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        if (!t->matches[i].live) continue;
        for (int j = 0; j < BE_TOUR_MATCHES; j++)
            if (t->matches[j].round < t->matches[i].round && t->matches[j].winner == BE_TOUR_NONE) return false;
    }
    return true;
}

// Reads every frame the stream holds for up to `ms`, checking each bracket; a ping keeps it open.
static void drain(int ms) {
    if (g_fd < 0) {
        usleep((useconds_t)ms * 1000);
        return;
    }
    uint8_t pg[2];
    int pn = sf_put_bare(pg, sizeof pg, SF_PING);
    tc_ws_send(g_fd, CB_WS_OP_BIN, pg, (size_t)pn);
    int64_t until = cb_mono_ms() + ms;
    while (cb_mono_ms() < until) {
        cb_buf_reset(&g_in);
        int op = tc_ws_recv(g_fd, (int)(until - cb_mono_ms()) > 0 ? (int)(until - cb_mono_ms()) : 1, &g_in);
        if (op != CB_WS_OP_BIN) {
            if (op < 0 || op == CB_WS_OP_PING) continue;
            break;
        }
        if (!sf_is_push(g_in.p, g_in.len) || g_in.p[1] != BE_F_TOUR || g_in.len != 2 + sizeof g_last) continue;
        memcpy(&g_last, g_in.p + 2, sizeof g_last);
        g_frames++;
        if (!barrier_holds(&g_last)) g_barrier_broken++;
    }
}

static bool get_tour(be_w_tour *t) {
    tc_response r;
    bool ok = tc_http(g_port, "GET", "/be/tournament", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len == sizeof *t;
    if (ok) memcpy(t, r.body.p, sizeof *t);
    tc_response_free(&r);
    return ok;
}

typedef struct {
    be_w_league  h;
    be_w_bot     bots[BE_LEAGUE_BOTS];
    be_w_result *results;
} league;

static bool get_league(league *L) {
    tc_response r;
    bool ok = false;
    memset(L, 0, sizeof *L);
    if (tc_http(g_port, "GET", "/be/league", NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len >= sizeof(be_w_league)) {
        memcpy(&L->h, r.body.p, sizeof L->h);
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

// GET /v1/games/<gid> as the human, adopted into g_ct.
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

// One move of the human's on the board just read, if it is his to make: true when one was applied.
static bool human_move(const char *gid, uint64_t *rng) {
    uint8_t wire[2 + 2 * AWIRE_MAX_CARDS], buf[NW_GID_MAX + 8 + sizeof wire];
    int wn = be_legal_wire(&g_ct, &g_lm, rng, 0, wire, sizeof wire);
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

// Match 0 still live at the same game.
static void still_live(const char *gid, const char *when) {
    be_w_tour t;
    T_CHECK(get_tour(&t));
    fprintf(stderr, "smoke_human: %s: match 0 live %d at %.*s, games %d\n", when, t.matches[0].live, t.matches[0].gid_len, t.matches[0].gid,
            t.matches[0].games);
    T_EQ_INT(t.matches[0].live, 1);
    T_EQ_INT(t.matches[0].games, 0);
    T_CHECK(t.matches[0].gid_len == strlen(gid) && memcmp(t.matches[0].gid, gid, strlen(gid)) == 0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: smoke_human <birdseye> <cbase>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    cb_buf_init(&g_in);
    T_CHECK(tp_tempdir(g_dir, sizeof g_dir) == 0);
    tp_child game, be;
    g_game_port = start_game_server(&game, argv[2]);
    T_CHECK(g_game_port > 0);
    if (g_game_port <= 0) return t_done("smoke_human");
    g_port = start_birdseye(&be, argv[1]);
    T_CHECK(g_port > 0);
    if (g_port <= 0) {
        tp_stop(&game, SIGTERM, 15000);
        return t_done("smoke_human");
    }
    open_stream();

    // The human's row: the last, key "human", his account's id.
    T_CHECK(be_client_init(&g_h, HUMAN, PASSWORD));
    T_CHECK(be_client_sign_in(&g_h, g_game_port));
    league L;
    T_CHECK(get_league(&L));
    int hu = (int)L.h.n_bots - 1;
    T_CHECK(hu > 48);
    T_CHECK(L.bots[hu].key_len == 5 && memcmp(L.bots[hu].key, "human", 5) == 0);
    T_CHECK(L.bots[hu].id_len == strlen(g_h.uid) && memcmp(L.bots[hu].id, g_h.uid, L.bots[hu].id_len) == 0);
    T_CHECK(L.bots[hu].name_len == strlen(HUMAN) && memcmp(L.bots[hu].name, HUMAN, L.bots[hu].name_len) == 0);
    free(L.results);

    // Seed 32 is his, and match 0 (32 v 33) is live with a game.
    be_w_tour t;
    int64_t until = cb_mono_ms() + 15000;
    while (cb_mono_ms() < until && !(get_tour(&t) && t.matches[0].live)) drain(200);
    T_EQ_INT(t.seed_bot[31], hu);
    T_EQ_INT(t.matches[0].seed_a, 32);
    T_EQ_INT(t.matches[0].a, hu);
    T_EQ_INT(t.matches[0].live, 1);
    T_CHECK(t.matches[0].gid_len > 0);
    char gid[BE_GID + 1] = { 0 };
    memcpy(gid, t.matches[0].gid, t.matches[0].gid_len);
    uint8_t bot = t.matches[0].b;

    // The lobby as he sees it: two seats, he UNREADY, the bot READY, nothing dealt.
    T_CHECK(human_view(gid));
    T_EQ_INT(g_ct.view.status, GAME_STATUS_WAITING);
    T_EQ_INT(g_ct.view.num_players, 2);
    T_EQ_STR(g_ct.view.title, "Opening round - Game 1 of 5");   // the match, not "house's Game"
    int me = g_ct.view.my_seat;
    T_CHECK(me == 0 || me == 1);
    if (me == 0 || me == 1) {
        T_CHECK(!g_ct.view.seats[me].is_ai);
        T_CHECK(g_ct.view.seats[me].status != PLAYER_STATUS_READY);
        T_CHECK(g_ct.view.seats[1 - me].is_ai);
        T_EQ_INT(g_ct.view.seats[1 - me].status, PLAYER_STATUS_READY);
    }
    // The watcher read the undealt lobby (the site's row has its seats).
    {
        char path[64];
        snprintf(path, sizeof path, "/be/table/%s", gid);
        tc_response r;
        be_w_detail *d = malloc(sizeof *d);
        bool ok = false;
        for (int tries = 0; tries < 30 && !ok; tries++) {
            ok = tc_http(g_port, "GET", path, NULL, NULL, 0, &r) == 0 && r.status == 200 && r.body.len == sizeof *d;
            if (ok) {
                memcpy(d, r.body.p, sizeof *d);
                ok = d->table.num_players == 2;
            }
            tc_response_free(&r);
            if (!ok) drain(200);
        }
        T_CHECK(ok);
        if (ok) T_EQ_INT(d->table.phase, BE_PHASE_LOBBY);
        free(d);
    }
    // He takes his time in the lobby: longer than the stall limit, the match keeps its game.
    drain(PAUSE_US / 1000);
    still_live(gid, "after the lobby pause");
    T_CHECK(human_view(gid));
    T_EQ_INT(g_ct.view.status, GAME_STATUS_WAITING);

    // His Start deals; he plays to the fool, pausing once mid-game.
    T_EQ_INT(human_start(gid), 200);
    uint64_t rng = 99;
    int moves = 0;
    bool paused = false, over = false;
    until = cb_mono_ms() + 90000;
    while (cb_mono_ms() < until && !over) {
        if (!human_view(gid)) {
            drain(100);
            continue;
        }
        if (g_ct.view.status == GAME_STATUS_GAME_OVER) {
            over = true;
            break;
        }
        if (g_ct.view.status == GAME_STATUS_PLAYING && human_move(gid, &rng)) moves++;
        if (moves == 3 && !paused) {
            paused = true;
            drain(PAUSE_US / 1000);
            still_live(gid, "after the mid-game pause");
        }
        drain(60);
    }
    fprintf(stderr, "smoke_human: %d moves of his, fool %d (he is seat %d)\n", moves, g_ct.view.fool, me);
    T_CHECK(over);
    T_CHECK(paused);
    int fool = g_ct.view.fool;

    // The bracket: one game played in match 0, its winner the side the fool was not, no game live.
    until = cb_mono_ms() + 20000;
    while (cb_mono_ms() < until && g_last.matches[0].games == 0) drain(200);
    t = g_last;
    T_EQ_INT(t.matches[0].games, 1);
    T_EQ_INT(t.matches[0].live, 0);
    if (fool >= 0) {
        T_EQ_INT(t.matches[0].wins_a + t.matches[0].wins_b, 1);
        bool he_lost = fool == me;
        // He is side a (seed 32 v 33).
        T_EQ_INT(t.matches[0].wins_b, he_lost ? 1 : 0);
        T_EQ_INT(t.matches[0].wins_a, he_lost ? 0 : 1);
    } else T_EQ_INT(t.matches[0].wins_a + t.matches[0].wins_b, 0);
    // The league's history holds the game, his row in his seat and the bot's in the other.
    T_CHECK(get_league(&L));
    bool found = false;
    for (uint32_t i = 0; i < L.h.n_results; i++) {
        const be_w_result *r = &L.results[i];
        if (r->gid_len != strlen(gid) || memcmp(r->gid, gid, r->gid_len) != 0) continue;
        found = true;
        T_EQ_INT(r->n, 2);
        if (me == 0 || me == 1) {
            T_EQ_INT(r->bot[me], hu);
            T_EQ_INT(r->bot[1 - me], bot);
        }
        T_EQ_INT(r->fool, fool);
    }
    T_CHECK(found);
    free(L.results);

    fprintf(stderr, "smoke_human: %d bracket frames heard\n", g_frames);
    T_CHECK(g_frames > 0);
    T_EQ_INT(g_barrier_broken, 0);
    if (g_fd >= 0) close(g_fd);
    cb_buf_free(&g_in);
    be_client_free(&g_h);
    T_EQ_INT(tp_stop(&be, SIGTERM, 15000), 0);
    T_EQ_INT(tp_stop(&game, SIGTERM, 15000), 0);
    return t_done("smoke_human");
}

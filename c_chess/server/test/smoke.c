// smoke.c - the real server, played over its real socket by the real client (c/src/client.c):
// accounts, seeks and the lobby, a whole game to mate with a spectator, the ratings, profile,
// history and PGN, a refused move, a rematch, a restart in the middle of a game, a challenge link,
// an abort and a flag.
//
//   build/test/smoke build/c_chess build/chessbench
#include "client.h"
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"
#include "net/ws.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *g_bin;
static tp_child g_child;
static int g_port;
static char g_dbdir[400];

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void start_server(void) {
    char line[512];
    static char envdb[512];
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", g_dbdir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret",
                    "CBASE_ADMIN_TOKEN=smoke-admin-token-0123456789abcdef0123", "CBASE_LOG_LEVEL=info", NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    g_port = 0;
    T_EQ_INT(tp_start(&g_child, argv, env), 0);
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    T_CHECK(g_port > 0);
}

typedef struct {
    cc_client c;
    int fd;
    uint32_t seen;   // events gathered by pump
} player;

static player A, B, C;   // alice, bob, carol (the spectator)

static void flush(player *p) {
    const uint8_t *f;
    size_t n;
    while ((n = cc_client_next_out(&p->c, &f)) > 0) T_EQ_INT(tc_ws_send(p->fd, CB_WS_OP_BIN, f, n), 0);
}

// Reads frames until an event of `want` has been seen (gathered in p->seen, which the caller
// clears), or timeout_ms. true when seen.
static bool pump(player *p, uint32_t want, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        flush(p);
        p->seen |= cc_client_events(&p->c);
        if (p->seen & want) return true;
        int64_t left = end - now_ms();
        if (left <= 0) return false;
        cb_buf b;
        cb_buf_init(&b);
        int op = tc_ws_recv(p->fd, (int)left, &b);
        if (op < 0) { cb_buf_free(&b); return false; }
        if (op == CB_WS_OP_BIN) cc_client_in(&p->c, (const uint8_t *)b.p, b.len, now_ms());
        cb_buf_free(&b);
    }
}

// Every frame that is already there, without waiting.
static void drain(player *p) {
    for (;;) {
        cb_buf b;
        cb_buf_init(&b);
        int op = tc_ws_recv(p->fd, 50, &b);
        if (op == CB_WS_OP_BIN) cc_client_in(&p->c, (const uint8_t *)b.p, b.len, now_ms());
        cb_buf_free(&b);
        if (op < 0) break;
    }
    p->seen |= cc_client_events(&p->c);
}

static int auth(player *p, const char *name, const char *pw, int mode) {
    cw_auth_req q;
    cc_client_auth_body(name, pw, mode, &q);
    tc_response r;
    T_EQ_INT(tc_http(g_port, "POST", "/c/auth", NULL, &q, sizeof q, &r), 0);
    cc_client_auth_answer(&p->c, r.status, (const uint8_t *)r.body.p, r.body.len);
    int st = r.status;
    tc_response_free(&r);
    return st;
}

static void connect_ws(player *p) {
    p->fd = tc_ws_open(g_port, "/ws", NULL);
    T_CHECK(p->fd >= 0);
    cc_client_hello(&p->c);
    p->seen = 0;
    T_CHECK(pump(p, CC_EV_READY, 5000));
}

static void bearer(const player *p, char *out, size_t cap) { snprintf(out, cap, "Authorization: Bearer %s\r\n", p->c.token); }

static bool play(player *p, const char *uci) {
    chess_pos *pos = &p->c.g.pos;
    ch_move m = ch_parse_uci(pos, uci, strlen(uci));
    if (m == CH_MOVE_NONE) return false;
    return cc_client_move(&p->c, CH_FROM(m), CH_TO(m), CH_PROMO(m), now_ms()) == CC_OK;
}

// Plays the moves in turn from whichever of a and b holds that color, each waiting to see the
// other's move arrive before it plays.
static void play_line(player *w, player *b, const char *const *moves, int n) {
    for (int i = 0; i < n; i++) {
        player *mover = i % 2 == 0 ? w : b, *other = i % 2 == 0 ? b : w;
        int ply = mover->c.g.ply;
        T_CHECK(play(mover, moves[i]));
        flush(mover);
        other->seen = 0;
        if (!pump(other, CC_EV_MOVED, 5000)) {
            drain(mover);
            fprintf(stderr, "  %s: no move arrived; the mover's events %x, refusal %d/%d (status %d)\n", moves[i], mover->seen,
                    mover->c.err.code, mover->c.err.rc, mover->c.err_status);
            T_CHECK(0);
        }
        T_EQ_INT(other->c.g.ply, ply + 1);
    }
}

static void accounts(void) {
    cc_client_init(&A.c);
    cc_client_init(&B.c);
    cc_client_init(&C.c);
    T_EQ_INT(auth(&A, "alice", "correct horse battery", CW_AUTH_SIGNUP), 200);
    T_EQ_STR(A.c.name, "alice");
    T_CHECK(A.c.token_len > 20);
    T_EQ_INT(auth(&B, "bob", "a long enough password", CW_AUTH_SIGNUP), 200);
    T_EQ_INT(auth(&C, "carol", "carol's password here", CW_AUTH_SIGNUP), 200);
    player x;
    cc_client_init(&x.c);
    T_EQ_INT(auth(&x, "Alice", "something else entirely", CW_AUTH_SIGNUP), 409);   // names are case-blind
    T_EQ_INT(x.c.auth_code, CW_E_NAME_TAKEN);
    T_EQ_INT(auth(&x, "alice", "the wrong password", CW_AUTH_SIGNIN), 401);
    T_EQ_INT(auth(&x, "a b", "a long enough password", CW_AUTH_SIGNUP), 400);
    T_EQ_INT(x.c.auth_code, CW_E_BAD_NAME);
    T_EQ_INT(auth(&x, "dave", "short", CW_AUTH_SIGNUP), 400);
    T_EQ_INT(x.c.auth_code, CW_E_WEAK_PASSWORD);
    T_EQ_INT(auth(&x, "alice", "correct horse battery", CW_AUTH_SIGNIN), 200);
    // a refresh hands a new session
    cw_refresh_req rq;
    cc_client_refresh_body(&x.c, &rq);
    tc_response r;
    T_EQ_INT(tc_http(g_port, "POST", "/c/refresh", NULL, &rq, sizeof rq, &r), 0);
    T_EQ_INT(r.status, 200);
    tc_response_free(&r);
    // without a token, a seek is refused
    cw_seek s = { 60, 0, 1 };
    T_EQ_INT(tc_http(g_port, "POST", "/c/seek", NULL, &s, sizeof s, &r), 0);
    T_EQ_INT(r.status, 401);
    tc_response_free(&r);
    connect_ws(&A);
    connect_ws(&B);
    connect_ws(&C);
}

static uint32_t g_game;

static void seek_and_pair(void) {
    cc_client_lobby(&B.c);
    B.seen = 0;
    T_CHECK(pump(&B, CC_EV_LOBBY, 3000));
    T_EQ_INT(B.c.lobby.n, 0);
    cc_client_seek(&A.c, 60, 0, 1);
    A.seen = 0;
    T_CHECK(pump(&A, CC_EV_SEEK, 3000));
    B.seen = 0;
    T_CHECK(pump(&B, CC_EV_LOBBY, 3000));   // the lobby push
    T_EQ_INT(B.c.lobby.n, 1);
    T_EQ_STR(B.c.lobby.seeks[0].name, "alice");
    T_EQ_INT(B.c.lobby.seeks[0].base_s, 60);
    // a seek of another time control waits beside it
    cc_client_seek(&C.c, 180, 2, 1);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_SEEK, 3000));
    cc_client_unseek(&C.c);
    flush(&C);
    // bob's matches alice's: a game for both, the lobby empty again
    cc_client_seek(&B.c, 60, 0, 1);
    B.seen = 0;
    A.seen = 0;
    T_CHECK(pump(&B, CC_EV_START, 3000));
    T_CHECK(pump(&A, CC_EV_START, 3000));
    T_CHECK(A.c.start_game != 0 && A.c.start_game == B.c.start_game);
    T_CHECK(A.c.start_color != B.c.start_color);
    g_game = A.c.start_game;
    drain(&B);
    T_EQ_INT(B.c.lobby.n, 0);
    cc_client_lobby_leave(&B.c);
}

static player *white_of(player *a, player *b) { return a->c.color == CH_WHITE ? a : b; }
static player *black_of(player *a, player *b) { return a->c.color == CH_WHITE ? b : a; }

static void a_game_to_mate(void) {
    cc_client_open(&A.c, g_game);
    cc_client_open(&B.c, g_game);
    A.seen = B.seen = 0;
    T_CHECK(pump(&A, CC_EV_GAME, 3000));
    T_CHECK(pump(&B, CC_EV_GAME, 3000));
    T_CHECK(A.c.synced && B.c.synced);
    T_CHECK(A.c.color >= 0 && B.c.color >= 0 && A.c.color != B.c.color);
    player *w = white_of(&A, &B), *b = black_of(&A, &B);
    T_CHECK(cc_client_my_turn(&w->c) && !cc_client_my_turn(&b->c));
    // legal targets of the e2 pawn: e3 and e4
    T_CHECK(cc_client_targets(&w->c, 12) == ((1ULL << 20) | (1ULL << 28)));
    T_CHECK(cc_client_targets(&b->c, 52) == 0);   // not black's turn
    // a move out of turn is refused locally
    T_EQ_INT(cc_client_move(&b->c, 52, 36, 0, now_ms()), CC_E_TURN);
    static const char *const first[] = { "f2f3", "e7e5" };
    play_line(w, b, first, 2);
    // the spectator joins in the middle
    cc_client_open(&C.c, g_game);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_GAME, 3000));
    T_EQ_INT(C.c.g.ply, 2);
    T_EQ_INT(C.c.color, -1);
    T_EQ_INT(cc_client_targets(&C.c, 6), 0);   // a spectator moves nothing
    static const char *const rest[] = { "g2g4", "d8h4" };
    play_line(w, b, rest, 2);
    // everyone sees the mate
    T_CHECK(pump(&C, CC_EV_OVER, 3000));
    T_CHECK(pump(w, CC_EV_OVER, 3000));
    T_CHECK(pump(b, CC_EV_OVER, 3000));
    T_EQ_INT(C.c.g.status, CC_MATE);
    T_EQ_INT(C.c.g.winner, CH_BLACK);
    T_EQ_INT(C.c.g.ply, 4);
    T_EQ_STR(C.c.san[3], "Qh4#");
    drain(w);
    drain(b);
    T_CHECK(b->c.diff[CH_BLACK] > 0);
    T_CHECK(w->c.diff[CH_WHITE] < 0);
    T_EQ_INT(b->c.diff[CH_BLACK], -w->c.diff[CH_WHITE]);   // equal ratings, equal deviations
    T_EQ_INT(b->c.moves_confirmed, 2);
}

static void after_the_game(void) {
    player *b = black_of(&A, &B);
    tc_response r;
    char path[64];
    // the profile: the winner's rating, one point of history
    snprintf(path, sizeof path, "/c/user/%s", b->c.name);
    T_EQ_INT(tc_http(g_port, "GET", path, NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    cw_profile pr;
    T_CHECK(r.body.len == sizeof pr + sizeof(cw_point));
    memcpy(&pr, r.body.p, sizeof pr);
    T_CHECK(pr.rating > 1500);
    T_EQ_INT(pr.games, 1);
    T_EQ_INT(pr.wins, 1);
    T_EQ_INT(pr.n_points, 1);
    tc_response_free(&r);
    // the history
    snprintf(path, sizeof path, "/c/games/%s", b->c.name);
    T_EQ_INT(tc_http(g_port, "GET", path, NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    cw_game_list gl;
    memcpy(&gl, r.body.p, r.body.len < sizeof gl ? r.body.len : sizeof gl);
    T_EQ_INT(gl.n, 1);
    T_EQ_INT((int)gl.rows[0].game, (int)g_game);
    T_EQ_INT(gl.rows[0].status, CC_MATE);
    T_EQ_INT(gl.rows[0].ply, 4);
    T_EQ_STR(gl.rows[0].opponent, black_of(&A, &B) == &A ? "bob" : "alice");
    tc_response_free(&r);
    // the same read over the socket (what the page does: no HTTP after sign-in)
    snprintf(path, sizeof path, "/c/user/%s", b->c.name);
    cc_client_read(&C.c, path, 77);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_READ, 3000));
    T_EQ_INT(C.c.read_tag, 77);
    T_EQ_INT(C.c.read_status, 200);
    T_EQ_INT((int)C.c.read_len, (int)(sizeof(cw_profile) + sizeof(cw_point)));
    cc_client_read(&C.c, "/c/user/nobody", 78);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_READ, 3000));
    T_EQ_INT(C.c.read_tag, 78);
    T_EQ_INT(C.c.read_status, 404);
    T_EQ_INT(tc_http(g_port, "GET", "/c/user/nobody", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
    // the PGN
    snprintf(path, sizeof path, "/c/pgn/%u", g_game);
    T_EQ_INT(tc_http(g_port, "GET", path, NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "1. f3 e5 2. g4 Qh4# 0-1") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "[Result \"0-1\"]") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "[WhiteElo \"1500\"]") != NULL);
    tc_response_free(&r);
    // a move in the finished game, over HTTP: refused by the rules
    char hdr[2200];
    bearer(white_of(&A, &B), hdr, sizeof hdr);
    cw_move_req q = { g_game, 4, CH_MOVE(12, 28, 0), 0xffffffffu };
    T_EQ_INT(tc_http(g_port, "POST", "/c/move", hdr, &q, sizeof q, &r), 0);
    T_EQ_INT(r.status, 409);
    cw_err e;
    memcpy(&e, r.body.p, sizeof e);
    T_EQ_INT(e.code, CW_E_RULES);
    T_EQ_INT(e.rc, CC_E_OVER);
    tc_response_free(&r);
    // the spectator is no player
    bearer(&C, hdr, sizeof hdr);
    T_EQ_INT(tc_http(g_port, "POST", "/c/move", hdr, &q, sizeof q, &r), 0);
    T_EQ_INT(r.status, 403);
    tc_response_free(&r);
}

static uint32_t g_rematch, g_last;

static void rematch(void) {
    cc_client_act(&A.c, CW_A_REMATCH);
    flush(&A);
    B.seen = 0;
    T_CHECK(pump(&B, CC_EV_GAME, 3000));
    T_EQ_INT(B.c.rematch_offer, 1u << A.c.color);
    int a_was = A.c.color;
    cc_client_act(&B.c, CW_A_REMATCH);
    flush(&B);
    A.seen = B.seen = 0;
    T_CHECK(pump(&A, CC_EV_START, 3000));
    T_CHECK(pump(&B, CC_EV_START, 3000));
    g_rematch = A.c.start_game;
    T_CHECK(g_rematch != 0 && g_rematch != g_game);
    T_EQ_INT(A.c.start_color, a_was ^ 1);   // colors swap
    cc_client_open(&A.c, g_rematch);
    cc_client_open(&B.c, g_rematch);
    A.seen = B.seen = 0;
    T_CHECK(pump(&A, CC_EV_GAME, 3000));
    T_CHECK(pump(&B, CC_EV_GAME, 3000));
    static const char *const two[] = { "e2e4", "c7c5" };
    play_line(white_of(&A, &B), black_of(&A, &B), two, 2);
    usleep(400 * 1000);   // white thinks: its clock runs (black's first move started it)
    static const char *const third[] = { "g1f3" };
    play_line(white_of(&A, &B), black_of(&A, &B), third, 1);
}

// The server is killed (SIGKILL: no clean stop) in the middle of the rematch and starts again: the
// game is there, three plies in, its clocks as they were.
static void restart(void) {
    close(A.fd); close(B.fd); close(C.fd);
    usleep(300 * 1000);   // the journal's writer hands the last move to the kernel within a few ms
    T_EQ_INT(tp_stop(&g_child, SIGKILL, 10000), -1);   // -1: ended by the signal, not an exit
    start_server();
    // the tokens still verify (the same secret): sign back in
    connect_ws(&A);
    connect_ws(&B);
    connect_ws(&C);
    cc_client_open(&A.c, g_rematch);
    cc_client_open(&B.c, g_rematch);
    A.seen = B.seen = 0;
    T_CHECK(pump(&A, CC_EV_GAME, 3000));
    T_CHECK(pump(&B, CC_EV_GAME, 3000));
    T_EQ_INT(A.c.g.ply, 3);
    T_EQ_INT(A.c.g.status, CC_PLAYING);
    T_EQ_STR(A.c.san[1], "c5");
    // the clocks as the journal left them: white's 400 ms of thought are spent, black's time untouched
    T_CHECK(A.c.left_ms[CH_WHITE] <= 59600 && A.c.left_ms[CH_WHITE] >= 50000);
    T_CHECK(A.c.left_ms[CH_BLACK] > 59000);
    static const char *const more[] = { "d7d6" };
    play_line(black_of(&A, &B), white_of(&A, &B), more, 1);
    // and resigns
    player *b = black_of(&A, &B);
    cc_client_act(&b->c, CW_A_RESIGN);
    flush(b);
    A.seen = B.seen = 0;
    T_CHECK(pump(&A, CC_EV_OVER, 3000));
    T_EQ_INT(A.c.g.status, CC_RESIGN);
    T_EQ_INT(A.c.g.winner, CH_WHITE);
    // the first game is out of memory only after ten minutes, but its snapshot reads either way
    cc_client_open(&C.c, g_game);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_GAME, 3000));
    T_EQ_INT(C.c.g.status, CC_MATE);
    T_EQ_INT(C.c.g.ply, 4);
    T_CHECK(C.c.left_ms[0] > 0 && C.c.left_ms[1] > 0);   // the clocks it ended with, from the journal
}

static void challenge_abort_flag(void) {
    // a challenge link: its creator cannot take it, another can
    cc_client_challenge(&A.c, 0, 1, 0);   // 0+1: no time at all once the clock starts
    A.seen = 0;
    T_CHECK(pump(&A, CC_EV_CHALLENGE, 3000));
    uint32_t ch = A.c.challenge_id;
    T_CHECK(ch != 0);
    cc_client_accept(&A.c, ch);
    A.seen = 0;
    T_CHECK(pump(&A, CC_EV_ERROR, 3000));
    T_EQ_INT(A.c.err.code, CW_E_SELF);
    cc_client_accept(&C.c, ch);
    C.seen = A.seen = 0;
    T_CHECK(pump(&C, CC_EV_START, 3000));
    T_CHECK(pump(&A, CC_EV_START, 3000));
    uint32_t g = C.c.start_game;
    T_CHECK(g > g_rematch);   // ids go on from the journal's after a restart, never reused
    cc_client_accept(&B.c, ch);   // taken: gone
    B.seen = 0;
    T_CHECK(pump(&B, CC_EV_ERROR, 3000));
    T_EQ_INT(B.c.err.code, CW_E_NO_GAME);
    cc_client_open(&A.c, g);
    cc_client_open(&C.c, g);
    A.seen = C.seen = 0;
    T_CHECK(pump(&A, CC_EV_GAME, 3000));
    T_CHECK(pump(&C, CC_EV_GAME, 3000));
    // the flag: white has no time once black has moved
    static const char *const two[] = { "d2d4", "d7d5" };
    player *w = white_of(&A, &C), *b = black_of(&A, &C);
    play_line(w, b, two, 2);
    T_CHECK(cc_game_over(&w->c.g) || pump(w, CC_EV_OVER, 3000));   // it may have come with black's move
    T_EQ_INT(w->c.g.status, CC_FLAG);
    T_EQ_INT(w->c.g.winner, CH_BLACK);
    // a casual game: no rating change
    T_EQ_INT(w->c.diff[0], 0);
    // abort: a new challenge, aborted before a move
    cc_client_challenge(&B.c, 300, 0, 0);
    B.seen = 0;
    T_CHECK(pump(&B, CC_EV_CHALLENGE, 3000));
    cc_client_accept(&C.c, B.c.challenge_id);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_START, 3000));
    cc_client_open(&C.c, C.c.start_game);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_GAME, 3000));
    g_last = C.c.start_game;
    cc_client_act(&C.c, CW_A_ABORT);
    flush(&C);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_OVER, 3000));
    T_EQ_INT(C.c.g.status, CC_ABORTED);
}

// No CC_ENGINE: a game against the computer is refused as unavailable.
static void no_engine(void) {
    cc_client_ai(&A.c, 300, 0, 3, 0);
    A.seen = 0;
    T_CHECK(pump(&A, CC_EV_ERROR, 3000));
    T_EQ_INT(A.c.err_status, 503);
}

// With CC_TEST_ENGINE (a Stockfish executable): a server with it, and a game against level 1 played
// to its end, the computer answering every move.
static void with_engine(const char *engine) {
    tp_stop(&g_child, SIGTERM, 10000);
    char line[512];
    static char envdb[512], enveng[700];
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", g_dbdir);
    snprintf(enveng, sizeof enveng, "CC_ENGINE=%s", engine);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret",
                    "CBASE_ADMIN_TOKEN=smoke-admin-token-0123456789abcdef0123", "CBASE_LOG_LEVEL=info", enveng, "CC_ENGINES=1", NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    T_EQ_INT(tp_start(&g_child, argv, env), 0);
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    close(A.fd);
    connect_ws(&A);
    cc_client_ai(&A.c, 600, 0, 1, 1);   // we are black: the computer opens
    A.seen = 0;
    T_CHECK(pump(&A, CC_EV_START, 5000));
    cc_client_open(&A.c, A.c.start_game);
    A.seen = 0;
    T_CHECK(pump(&A, CC_EV_GAME, 3000));
    T_EQ_INT(A.c.color, CH_BLACK);
    T_CHECK(A.c.g.ply == 1 || pump(&A, CC_EV_MOVED, 5000));   // its first move
    T_EQ_INT(A.c.g.ply, 1);
    int moves = 0;
    while (!cc_game_over(&A.c.g) && moves < 400) {
        if (!cc_client_my_turn(&A.c)) { A.seen = 0; if (!pump(&A, CC_EV_MOVED | CC_EV_OVER, 10000)) break; continue; }
        ch_moves ms;
        ch_legal(&A.c.g.pos, &ms);
        ch_move m = ms.m[rand() % ms.n];
        T_EQ_INT(cc_client_move(&A.c, CH_FROM(m), CH_TO(m), CH_PROMO(m), now_ms()), CC_OK);
        flush(&A);
        moves++;
    }
    drain(&A);
    T_CHECK(cc_game_over(&A.c.g));
    // the computer is charged for its thinking like any player (from black's first move on)
    T_CHECK(A.c.left_ms[CH_WHITE] < 600000);
    T_CHECK(A.c.left_ms[CH_WHITE] > 500000);
    // random moves against even the weakest level lose
    T_CHECK(A.c.g.status == CC_MATE ? A.c.g.winner == CH_WHITE : true);
    fprintf(stderr, "  against Stockfish-1: %d plies, status %d winner %d\n", A.c.g.ply, A.c.g.status, A.c.g.winner);
}

static void stats_and_site(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/c/stats", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "games_live 0\n") != NULL);
    tc_response_free(&r);
    // a restart with no game in play: new games still number on from the journal's last
    close(A.fd); close(B.fd); close(C.fd);
    T_EQ_INT(tp_stop(&g_child, SIGTERM, 10000), 0);
    start_server();
    connect_ws(&A);
    connect_ws(&C);
    cc_client_challenge(&A.c, 300, 0, 0);
    A.seen = 0;
    T_CHECK(pump(&A, CC_EV_CHALLENGE, 3000));
    cc_client_accept(&C.c, A.c.challenge_id);
    C.seen = 0;
    T_CHECK(pump(&C, CC_EV_START, 3000));
    T_CHECK(C.c.start_game > g_last);
    // no CC_WEB_DIR: the site's route answers 404
    T_EQ_INT(tc_http(g_port, "GET", "/", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: smoke <c_chess>\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    g_bin = argv[1];
    if (tp_tempdir(g_dbdir, sizeof g_dbdir) != 0) return 1;
    start_server();
    T_RUN(accounts);
    T_RUN(seek_and_pair);
    T_RUN(a_game_to_mate);
    T_RUN(after_the_game);
    T_RUN(rematch);
    T_RUN(restart);
    T_RUN(challenge_abort_flag);
    T_RUN(stats_and_site);
    T_RUN(no_engine);
    if (getenv("CC_TEST_ENGINE")) with_engine(getenv("CC_TEST_ENGINE"));
    tp_stop(&g_child, SIGTERM, 10000);
    return T_DONE();
}

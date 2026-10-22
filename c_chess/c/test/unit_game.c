// unit_game.c - a game with a clock: every ending, the clock's arithmetic, offers, and refusals;
// then Glicko-2 against the paper's worked example, the site's rating wrapper, and the PGN.
#include "game.h"
#include "rating.h"
#include "pgn.h"
#include "t.h"

#include <math.h>

static ch_move mv(const cc_game *g, const char *uci) { return ch_parse_uci(&g->pos, uci, strlen(uci)); }

/* Plays a space-separated list of UCI moves at 1 s apart from t, returning the time after. */
static int64_t play(cc_game *g, const char *list, int64_t t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        t += 1000;
        int rc = cc_game_move(g, g->pos.stm, g->ply, mv(g, tok), t);
        CHECK(rc == CC_OK, "move %s: rc %d", tok, rc);
    }
    return t;
}

static void endings(void) {
    cc_game g;
    TEST("mate");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "f2f3 e7e5 g2g4 d8h4", 0);
    CHECK(g.status == CC_MATE && g.winner == CH_BLACK, "status %d winner %d", g.status, g.winner);
    CHECK(cc_game_move(&g, CH_WHITE, g.ply, mv(&g, "a2a3"), 9000) == CC_E_OVER, "moved after mate");
    CHECK(cc_game_white_score(&g) == 0.0, "score");

    TEST("stalemate");
    cc_game_new(&g, 60000, 0, 0);
    ch_from_fen(&g.pos, "7k/5Q2/8/6K1/8/8/8/8 w - - 0 1");
    play(&g, "g5g6", 0);
    CHECK(g.status == CC_STALEMATE && g.winner == CC_NO_WINNER, "status %d", g.status);
    CHECK(cc_game_white_score(&g) == 0.5, "score");

    TEST("threefold");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1", 0);
    CHECK(g.status == CC_PLAYING, "drawn too early (twice is not three times)");
    play(&g, "f6g8", 10000);
    CHECK(g.status == CC_DRAW_REPETITION, "status %d", g.status);

    TEST("threefold counts castling rights");
    cc_game_new(&g, 60000, 0, 0);
    /* the kings walk out and back: the same squares as after 1. e4 e5, but without the rights */
    play(&g, "e2e4 e7e5 e1e2 e8e7 e2e1 e7e8 e1e2 e8e7 e2e1 e7e8", 0);
    CHECK(g.status == CC_PLAYING, "status %d: the position with castling rights was counted", g.status);
    play(&g, "e1e2 e8e7", 20000);   /* kings on e2 and e7 the third time (plies 4, 8, 12) */
    CHECK(g.status == CC_DRAW_REPETITION, "status %d", g.status);

    TEST("fifty moves");
    cc_game_new(&g, 60000, 0, 0);
    ch_from_fen(&g.pos, "4k3/8/8/8/8/8/R7/4K3 w - - 99 80");
    play(&g, "a2a3", 0);
    CHECK(g.status == CC_DRAW_FIFTY, "status %d", g.status);
    cc_game_new(&g, 60000, 0, 0);
    ch_from_fen(&g.pos, "4k3/8/8/8/8/8/R7/4K3 w - - 98 80");
    play(&g, "a2a3", 0);
    CHECK(g.status == CC_PLAYING, "drawn at 99");

    TEST("insufficient material by capture");
    cc_game_new(&g, 60000, 0, 0);
    ch_from_fen(&g.pos, "4k3/8/8/8/8/8/3n4/4K3 w - - 0 1");
    play(&g, "e1d2", 0);
    CHECK(g.status == CC_DRAW_MATERIAL, "status %d", g.status);

    TEST("max ply");
    cc_game_new(&g, 60000, 0, 0);
    g.ply = CC_MAX_PLY - 1;
    play(&g, "e2e4", 0);
    CHECK(g.status == CC_DRAW_MAX_PLY, "status %d", g.status);
}

static void refusals(void) {
    cc_game g;
    TEST("refusals");
    cc_game_new(&g, 60000, 0, 0);
    CHECK(cc_game_move(&g, CH_BLACK, 0, mv(&g, "e2e4"), 1) == CC_E_TURN, "black moved first");
    CHECK(cc_game_move(&g, CH_WHITE, 1, mv(&g, "e2e4"), 1) == CC_E_PLY, "wrong ply accepted");
    CHECK(cc_game_move(&g, CH_WHITE, 0, CH_MOVE(12, 36, 0), 1) == CC_E_ILLEGAL, "e2e5 accepted");
    CHECK(cc_game_move(&g, CH_WHITE, 0, mv(&g, "e2e4"), 1) == CC_OK, "e2e4");
    CHECK(cc_game_move(&g, CH_WHITE, 0, mv(&g, "e2e4"), 2) == CC_E_TURN, "the resend");
    CHECK(cc_game_draw_decline(&g, CH_WHITE) == CC_E_STATE, "declined a draw nobody offered");
}

static void clock(void) {
    cc_game g;
    int64_t next;
    TEST("clock does not run before black's first move");
    cc_game_new(&g, 60000, 2000, 0);
    CHECK(!cc_game_clock_running(&g), "running at start");
    cc_game_move(&g, CH_WHITE, 0, mv(&g, "e2e4"), 20000);
    cc_game_move(&g, CH_BLACK, 1, mv(&g, "e7e5"), 25000);
    CHECK(g.left_ms[0] == 60000 && g.left_ms[1] == 60000, "left %lld %lld", (long long)g.left_ms[0], (long long)g.left_ms[1]);
    CHECK(cc_game_clock_running(&g), "not running after black's first move");

    TEST("a move deducts the time used and adds the increment");
    cc_game_move(&g, CH_WHITE, 2, mv(&g, "g1f3"), 32000);   /* 7 s used */
    CHECK(g.left_ms[0] == 60000 - 7000 + 2000, "white left %lld", (long long)g.left_ms[0]);
    CHECK(cc_game_left(&g, CH_BLACK, 35000) == 57000, "black's running time %lld", (long long)cc_game_left(&g, CH_BLACK, 35000));
    CHECK(cc_game_left(&g, CH_WHITE, 35000) == 55000, "white's time ran while black's turn");

    TEST("the flag by tick, after the grace");
    CHECK(!cc_game_tick(&g, 40000, &next), "flagged early");
    CHECK(next == 60000 + 1000 - 8000, "next %lld", (long long)next);   /* black's 52 s and a 1 s grace */
    CHECK(!cc_game_tick(&g, 92999, &next), "flagged inside the grace");
    CHECK(cc_game_tick(&g, 93000, &next), "no flag after black's 60 s and the grace");
    CHECK(g.status == CC_FLAG && g.winner == CH_WHITE && next == -1, "status %d winner %d next %lld", g.status, g.winner, (long long)next);

    TEST("lag: the network's share is given back from the quota");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4 e7e5", 0);                                       /* the clock runs from 2000 */
    CHECK(cc_game_move_lag(&g, CH_WHITE, 2, mv(&g, "g1f3"), 7000, 4600) == CC_OK, "move");
    CHECK(g.left_ms[0] == 60000 - 4600, "white left %lld: 400 ms of lag not given back", (long long)g.left_ms[0]);
    CHECK(g.lag_quota[0] == 1000 - 400 + CC_LAG_QUOTA_GAIN, "quota %d", g.lag_quota[0]);
    CHECK(cc_game_move_lag(&g, CH_BLACK, 3, mv(&g, "b8c6"), 12000, 0) == CC_OK, "move");   /* claims 0 ms of thought */
    CHECK(g.left_ms[1] == 60000 - 5000 + 1000, "black left %lld: more than the quota given back", (long long)g.left_ms[1]);
    CHECK(g.lag_quota[1] == CC_LAG_QUOTA_GAIN, "quota %d", g.lag_quota[1]);
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    static const char *const quiet[] = { "g1f3", "g8f6", "b1c3", "b8c6", "a2a3", "a7a6", "h2h3", "h7h6",
                                          "b2b3", "b7b6", "g2g3", "g7g6", "d2d3", "d7d6", "c1d2", "c8d7" };
    for (int i = 0; i < 16; i++)   /* no lag at all: the quota refills to its cap and no further */
        CHECK(cc_game_move_lag(&g, g.pos.stm, g.ply, mv(&g, quiet[i]), 4000 + i * 2000, 2000) == CC_OK, "%s", quiet[i]);
    CHECK(g.lag_quota[0] == CC_LAG_QUOTA_MAX, "white's quota %d", g.lag_quota[0]);
    TEST("lag: a move in flight beats the server's flag");
    cc_game_new(&g, 10000, 0, 0);
    play(&g, "e2e4 e7e5", 0);                                       /* white's 10 s run from 2000 */
    CHECK(!cc_game_tick(&g, 12500, NULL), "flagged inside the grace");
    CHECK(cc_game_move_lag(&g, CH_WHITE, 2, mv(&g, "g1f3"), 12500, 9800) == CC_OK, "the late-arriving move refused");
    CHECK(g.left_ms[0] == 200, "white left %lld", (long long)g.left_ms[0]);

    TEST("the flag on a late move");
    cc_game_new(&g, 10000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    CHECK(cc_game_move(&g, CH_WHITE, 2, mv(&g, "g1f3"), 2000 + 10000) == CC_E_OVER, "a move on the flag counted");
    CHECK(g.status == CC_FLAG && g.winner == CH_BLACK, "status %d", g.status);
    cc_game_new(&g, 10000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    CHECK(cc_game_move(&g, CH_WHITE, 2, mv(&g, "g1f3"), 2000 + 9999) == CC_OK, "a move 1 ms before the flag refused");

    TEST("flag against a lone king is a draw");
    cc_game_new(&g, 10000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    ch_from_fen(&g.pos, "4k3/8/8/8/8/8/8/R3K3 b - - 0 2");   /* black flags; white has a rook */
    cc_game_tick(&g, 2000 + 10000 + 1000, NULL);   /* after the grace */
    CHECK(g.status == CC_FLAG && g.winner == CH_WHITE, "status %d", g.status);
    cc_game_new(&g, 10000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    ch_from_fen(&g.pos, "4k3/8/8/8/8/8/8/N3K3 b - - 0 2");   /* white has only a knight */
    cc_game_tick(&g, 2000 + 10000 + 1000, NULL);
    CHECK(g.status == CC_DRAW_FLAG_MATERIAL && g.winner == CC_NO_WINNER, "status %d", g.status);

    TEST("first move abort");
    cc_game_new(&g, 60000, 0, 0);
    CHECK(!cc_game_tick(&g, CC_FIRST_MOVE_MS - 1, &next) && next == 1, "next %lld", (long long)next);
    CHECK(cc_game_tick(&g, CC_FIRST_MOVE_MS, NULL) && g.status == CC_ABORTED, "status %d", g.status);
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4", 0);   /* black's 30 s count from white's move */
    CHECK(!cc_game_tick(&g, 1000 + CC_FIRST_MOVE_MS - 1, NULL), "aborted early");
    CHECK(cc_game_tick(&g, 1000 + CC_FIRST_MOVE_MS, NULL) && g.status == CC_ABORTED, "status %d", g.status);
}

static void actions(void) {
    cc_game g;
    TEST("resign");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    CHECK(cc_game_resign(&g, CH_BLACK, 3000) == CC_OK && g.status == CC_RESIGN && g.winner == CH_WHITE, "status %d", g.status);
    CHECK(cc_game_resign(&g, CH_WHITE, 3000) == CC_E_OVER, "resigned twice");

    TEST("abort only before both have moved");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4", 0);
    CHECK(cc_game_abort(&g, CH_WHITE, 2000) == CC_OK && g.status == CC_ABORTED, "abort at ply 1");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    CHECK(cc_game_abort(&g, CH_WHITE, 3000) == CC_E_STATE && g.status == CC_PLAYING, "abort at ply 2");

    TEST("draw offer and acceptance");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    CHECK(cc_game_draw(&g, CH_WHITE, 3000) == CC_OK && g.draw_offer == 1, "offer");
    CHECK(cc_game_draw(&g, CH_WHITE, 3000) == CC_E_STATE, "offered twice");
    CHECK(cc_game_draw(&g, CH_BLACK, 3500) == CC_OK && g.status == CC_DRAW_AGREED, "accept");

    TEST("a move by the other side declines; the offerer's own move does not");
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4 e7e5", 0);
    cc_game_draw(&g, CH_WHITE, 3000);
    play(&g, "g1f3", 3000);
    CHECK(g.draw_offer == 1, "the offerer's move withdrew it");
    play(&g, "b8c6", 4000);
    CHECK(g.draw_offer == 0, "the reply did not decline it");
    cc_game_draw(&g, CH_WHITE, 6000);
    CHECK(cc_game_draw_decline(&g, CH_BLACK) == CC_OK && g.draw_offer == 0, "decline");

    TEST("replay");
    cc_game h;
    cc_game_new(&g, 60000, 0, 0);
    play(&g, "e2e4 e7e5 g1f3 b8c6 f1b5", 0);
    cc_game_new(&h, 60000, 0, 0);
    CHECK(cc_game_replay(&h, g.moves, g.ply) == CC_OK && h.pos.key == g.pos.key && h.ply == 5, "replay");
    ch_move bad[2] = { g.moves[0], g.moves[0] };
    cc_game_new(&h, 60000, 0, 0);
    CHECK(cc_game_replay(&h, bad, 2) == CC_E_ILLEGAL, "replayed an illegal move");
}

static void ratings(void) {
    TEST("glicko-2: the paper's example");
    /* Glickman, "Example of the Glicko-2 system": 1500/200/0.06 beats 1400/30, loses to 1550/100
     * and to 1700/300, tau 0.5: 1464.06, 151.52, 0.05999 */
    g2_result r[3] = { { { 1400, 30, 0.06 }, 1 }, { { 1550, 100, 0.06 }, 0 }, { { 1700, 300, 0.06 }, 0 } };
    g2_player p = g2_update((g2_player){ 1500, 200, 0.06 }, r, 3, 0.5, 1e-6);
    CHECK(fabs(p.rating - 1464.06) < 0.01, "rating %.4f", p.rating);
    CHECK(fabs(p.deviation - 151.52) < 0.01, "deviation %.4f", p.deviation);
    CHECK(fabs(p.volatility - 0.05999) < 0.00001, "volatility %.6f", p.volatility);
    g2_player idle = g2_update((g2_player){ 1500, 200, 0.06 }, NULL, 0, 0.5, 1e-6);
    CHECK(fabs(idle.deviation - sqrt(200.0 * 200.0 + 0.06 * 0.06 * 173.7178 * 173.7178)) < 1e-6 && idle.rating == 1500, "idle %.4f", idle.deviation);

    TEST("the site's ratings");
    g2_player w = cc_rating_new(), b = cc_rating_new();
    cc_rate(&w, &b, 1.0);
    CHECK(cc_rating_int(&w) > 1500 && cc_rating_int(&b) < 1500, "%d %d", cc_rating_int(&w), cc_rating_int(&b));
    CHECK(cc_rating_int(&w) - 1500 == 1500 - cc_rating_int(&b), "asymmetric %d %d", cc_rating_int(&w), cc_rating_int(&b));
    CHECK(w.deviation < CC_RD_START && w.deviation >= CC_RD_MIN, "rd %.2f", w.deviation);
    g2_player a = cc_rating_new(), c = cc_rating_new();
    cc_rate(&a, &c, 0.5);
    CHECK(cc_rating_int(&a) == 1500 && cc_rating_int(&c) == 1500, "a draw between equals moved them");
    g2_player lo = { 1500, 10, 0.0001 }, hi = { 1500, 10, 0.0001 };   /* a deviation the update keeps under the floor */
    cc_rate(&lo, &hi, 0.5);
    CHECK(lo.deviation == CC_RD_MIN, "deviation %.2f not clamped to the floor", lo.deviation);
    g2_player wild = { 1500, 200, 0.5 }, calm = { 1500, 200, 0.06 };
    cc_rate(&wild, &calm, 1.0);
    CHECK(wild.volatility == CC_VOL_MAX, "volatility %.3f not clamped", wild.volatility);
}

static void pgn(void) {
    TEST("pgn");
    cc_game g;
    cc_game_new(&g, 300000, 3000, 0);
    play(&g, "e2e4 e7e5 f1c4 b8c6 d1h5 g8f6 h5f7", 0);
    cc_pgn_info in = { "alice", "bob \"the\" builder", "https://chess.local/g/1", "2026.10.22", 1532, 0, 300, 3, g.status, g.winner };
    char out[1024];
    size_t n = cc_pgn(&in, g.moves, g.ply, out, sizeof out);
    const char *want =
        "[Event \"Casual game\"]\n[Site \"https://chess.local/g/1\"]\n[Date \"2026.10.22\"]\n[Round \"-\"]\n"
        "[White \"alice\"]\n[Black \"bob \\\"the\\\" builder\"]\n[Result \"1-0\"]\n[WhiteElo \"1532\"]\n"
        "[TimeControl \"300+3\"]\n[Termination \"Normal\"]\n\n"
        "1. e4 e5 2. Bc4 Nc6 3. Qh5 Nf6 4. Qxf7# 1-0\n";
    STREQ(out, want);
    CHECK(n == strlen(want), "length %zu", n);
    /* a long game wraps its moves at 80 columns */
    cc_game_new(&g, 300000, 3000, 0);
    play(&g, "e2e4 e7e5 d2d4 d7d5 c2c4 c7c5 b2b3 b7b6 a2a3 a7a6 h2h3 h7h6 g2g3 g7g6 f2f3 f7f6 "
             "c1d2 c8d7 f1e2 f8e7 d1c2 d8c7 b1c3 b8c6", 0);
    n = cc_pgn(&in, g.moves, g.ply, out, sizeof out);
    const char *moves = strstr(out, "\n\n") + 2;
    int lines = 0;
    for (const char *l = moves; *l; lines++) {
        const char *e = strchr(l, '\n');
        CHECK(e && e - l <= 80, "a line of %d columns", e ? (int)(e - l) : -1);
        if (!e) break;
        l = e + 1;
    }
    CHECK(lines >= 2, "%d lines: not wrapped", lines);
    char small[16];
    CHECK(cc_pgn(&in, g.moves, g.ply, small, sizeof small) == n && strlen(small) == sizeof small - 1, "truncation");
    STREQ(cc_result_str(CC_PLAYING, CC_NO_WINNER), "*");
    STREQ(cc_result_str(CC_DRAW_FIFTY, CC_NO_WINNER), "1/2-1/2");
    STREQ(cc_result_str(CC_ABORTED, CC_NO_WINNER), "*");
}

int main(void) {
    ch_init();
    endings();
    refusals();
    clock();
    actions();
    ratings();
    pgn();
    return report("unit_game");
}

/* The rules: the start (R1, R4), legality (R2, R5, R7), the call (R3), who
 * opens the next round (R4), elimination and the end (R1), the menu's order
 * (the body's format), refusal leaves the game untouched, replay equals
 * apply, and the longest game is exactly CN_MAX_MOVES_OF(n) moves. */
#include "cn_check.h"

static CnGame G, H;

/* A table with chosen dice: `dice` is n rows of up to 5, 0 for no die. */
static void table(CnGame *g, int n, const uint8_t dice[][CN_START_DICE])
{
    uint8_t seed[32];
    seed_wide(seed, 1);
    cn_new(g, seed, n);
    g->total = 0;
    for (int s = 0; s < n; s++) {
        int k = 0;
        while (k < CN_START_DICE && dice[s][k]) { g->dice[s][k] = dice[s][k]; k++; }
        for (int i = k; i < CN_START_DICE; i++) g->dice[s][i] = 0;
        g->dice_n[s] = (uint8_t)k;
        g->total = (uint8_t)(g->total + k);
    }
    /* a seat with no dice is not on turn */
    if (!g->dice_n[g->turn]) g->turn = (uint8_t)cn_next_live(g, g->turn);
}

static void test_start(void)
{
    TEST("R1 R4 the start");
    uint8_t seed[32];
    seed_wide(seed, 7);
    for (int n = 0; n <= 8; n++) {
        int ok = cn_new(&G, seed, n);
        CHECK(ok == (n >= 2 && n <= 6), "n %d accepted %d", n, ok);
        if (!ok) continue;
        CHECK(G.phase == CN_PH_BIDDING && G.turn == 0 && G.bid_q == 0 && G.round == 0, "n %d: seat 0 opens round 0", n);
        CHECK(G.total == 5 * n && G.hist_n == 0 && G.winner == CN_SEAT_NONE, "n %d: %d dice", n, G.total);
        for (int s = 0; s < n; s++) {
            CHECK(G.dice_n[s] == 5, "seat %d holds five", s);
            for (int i = 0; i < 5; i++) CHECK(G.dice[s][i] >= 1 && G.dice[s][i] <= 6, "a die is a face");
        }
    }
}

static void test_opening(void)
{
    TEST("R5 R7 the opening bid");
    uint8_t seed[32];
    seed_wide(seed, 3);
    cn_new(&G, seed, 2);
    CHECK(!cn_can_call(&G), "no call on the opening");
    CHECK(!cn_is_legal(&G, 0, call_move()), "the call refused with no bid");
    CnMove m[200];
    int n = cn_legal(&G, m, 200);
    CHECK(n == 10 * 5, "every (q, f) with q 1..10 and f 2..6: %d", n);
    CHECK(m[0].q == 1 && m[0].f == 2, "the lowest bid first");
    CHECK(m[n - 1].q == 10 && m[n - 1].f == 6, "the top bid last");
    for (int i = 1; i < n; i++) CHECK(cn_rank(m[i].q, m[i].f) == cn_rank(m[i - 1].q, m[i - 1].f) + 1, "ascending rank at %d", i);
    CHECK(cn_is_legal(&G, 0, bid(1, 2)), "one 2 is a bid");
    CHECK(cn_is_legal(&G, 0, bid(10, 6)), "ten 6s: the whole table");
    CHECK(!cn_is_legal(&G, 0, bid(11, 2)), "R7: no more than the dice on the table");
    CHECK(!cn_is_legal(&G, 0, bid(3, 1)), "R2: no bid on 1s");
    CHECK(!cn_is_legal(&G, 0, bid(3, 7)), "no seventh face");
    CHECK(!cn_is_legal(&G, 1, bid(3, 4)), "only the seat on turn");
    CHECK(cn_count(&G, 2) >= 0, "count");
}

static void test_raise(void)
{
    TEST("R2 raising");
    uint8_t seed[32];
    seed_wide(seed, 4);
    cn_new(&G, seed, 3);
    CHECK(cn_apply(&G, 0, bid(3, 4)), "seat 0 bids three 4s");
    CHECK(G.turn == 1 && G.bidder == 0 && G.bid_q == 3 && G.bid_f == 4 && G.phase == CN_PH_BIDDING, "seat 1 is on");
    CHECK(cn_can_call(&G), "a standing bid may be called");
    CHECK(!cn_is_legal(&G, 1, bid(3, 4)), "the same bid is not a raise");
    CHECK(!cn_is_legal(&G, 1, bid(3, 3)), "same quantity, lower face");
    CHECK(!cn_is_legal(&G, 1, bid(2, 6)), "lower quantity, higher face");
    CHECK(cn_is_legal(&G, 1, bid(3, 5)), "same quantity, higher face");
    CHECK(cn_is_legal(&G, 1, bid(4, 2)), "higher quantity, any face");
    int q, f;
    CHECK(cn_min_raise(&G, &q, &f) && q == 3 && f == 5, "the lowest raise is three 5s (%d %d)", q, f);
    CnMove m[200];
    int n = cn_legal(&G, m, 200);
    CHECK(m[0].q == 0 && m[1].q == 3 && m[1].f == 5, "the call first, then the lowest raise");
    CHECK(n == 1 + 15 * 5 - cn_rank(3, 5), "call plus every rank from three 5s: %d", n);
    /* the top of the table: only the call is left */
    G.bid_q = 15; G.bid_f = 6;
    CHECK(!cn_min_raise(&G, &q, &f), "nothing above fifteen 6s");
    CHECK(cn_legal(&G, m, 200) == 1 && m[0].q == 0, "the call alone");
}

static void test_call(void)
{
    TEST("R3 the call");
    static const uint8_t d[3][5] = { { 3, 3, 1, 5, 6 }, { 2, 3, 4, 4, 1 }, { 6, 6, 5, 2, 3 } };
    /* 3s: seat 0 has 3 3 1, seat 1 has 3 1, seat 2 has 3: six with the wilds */
    table(&G, 3, d);
    CHECK(cn_count(&G, 3) == 6, "three 3s plus two wild 1s plus one: %d", cn_count(&G, 3));
    CHECK(cn_count(&G, 6) == 5, "6s: 6, 6, 6 and the two 1s: %d", cn_count(&G, 6));
    H = G;
    cn_apply(&G, 0, bid(6, 3));                     /* exactly true */
    CHECK(cn_apply(&G, 1, call_move()), "seat 1 calls");
    CHECK(G.call_seat == 1 && G.call_bidder == 0 && G.call_count == 6, "the call recorded");
    CHECK(G.call_loser == 1 && G.dice_n[1] == 4 && G.dice_n[0] == 5, "exactly the count: the bid stands, the caller loses");
    CHECK(G.total == 14, "one die gone");
    G = H;
    cn_apply(&G, 0, bid(7, 3));                     /* one too many */
    cn_apply(&G, 1, call_move());
    CHECK(G.call_loser == 0 && G.dice_n[0] == 4 && G.dice_n[1] == 5, "one short: the bidder loses");
    CHECK(G.shown_n[2] == 5 && G.shown[0][2] == 1, "the dice at the call are kept for the reveal");
}

static void test_next_round(void)
{
    TEST("R4 who opens next");
    static const uint8_t d[3][5] = { { 2, 2, 2, 2, 2 }, { 4, 0 }, { 5, 5, 0 } };
    table(&G, 3, d);
    G.turn = 2;
    cn_apply(&G, 2, bid(5, 4));                     /* 4s: seat 1's one 4 only */
    CHECK(G.turn == 0, "seat 0 follows seat 2");
    cn_apply(&G, 0, bid(6, 4));
    CHECK(G.turn == 1, "seat 1");
    cn_apply(&G, 1, call_move());
    CHECK(G.call_loser == 0 && G.round == 1, "seat 0 bid too high, and lost");
    CHECK(G.turn == 0 && G.phase == CN_PH_REVEALED && G.bid_q == 0, "the loser opens the next round");
    CHECK(!cn_can_call(&G), "the opener cannot call");
    CHECK(G.round_at == 3, "the round opened after the call");

    /* an eliminated loser: the next live seat opens */
    table(&G, 3, d);
    G.turn = 0;
    cn_apply(&G, 0, bid(1, 5));                     /* 5s: two of them */
    cn_apply(&G, 1, call_move());                   /* seat 1 with one die calls a true bid */
    CHECK(G.call_loser == 1 && G.dice_n[1] == 0, "seat 1 is out");
    CHECK(G.turn == 2, "the next live seat after seat 1 opens (%d)", G.turn);
    cn_apply(&G, 2, bid(1, 2));
    CHECK(G.turn == 0, "seat 1 is skipped: 2 then 0 (%d)", G.turn);
    cn_apply(&G, 0, bid(2, 2));
    CHECK(G.turn == 2, "and 0 then 2 (%d)", G.turn);
}

static void test_over(void)
{
    TEST("R1 the end");
    static const uint8_t d[2][5] = { { 6, 0 }, { 2, 3, 0 } };
    table(&G, 2, d);
    cn_apply(&G, 0, bid(2, 6));                     /* one 6 on the table */
    cn_apply(&G, 1, call_move());
    CHECK(G.phase == CN_PH_OVER && G.winner == 1 && G.turn == CN_SEAT_NONE, "seat 0 out, seat 1 wins");
    CHECK(cn_legal(&G, 0, 0) == 0, "nothing is legal");
    H = G;
    CHECK(!cn_apply(&G, 1, bid(1, 2)) && !cn_apply(&G, 0, call_move()), "every move refused");
    CHECK(!memcmp(&G, &H, sizeof G), "and the game untouched");
}

static void test_refusal_untouched(void)
{
    TEST("refusal leaves the game untouched");
    uint8_t seed[32];
    seed_wide(seed, 9);
    cn_new(&G, seed, 4);
    cn_apply(&G, 0, bid(4, 4));
    H = G;
    CnMove bad[] = { bid(4, 4), bid(4, 3), bid(21, 2), bid(5, 1), bid(5, 0), { 0, 3 } };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!cn_apply(&G, 1, bad[i]), "bad move %u refused", i);
        CHECK(!memcmp(&G, &H, sizeof G), "bad move %u changed nothing", i);
    }
    CHECK(!cn_apply(&G, 2, bid(5, 5)), "out of turn refused");
    CHECK(!memcmp(&G, &H, sizeof G), "out of turn changed nothing");
}

static void test_replay(void)
{
    TEST("replay equals apply");
    for (uint32_t k = 0; k < 200; k++) {
        uint8_t seed[32];
        seed_wide(seed, 100 + k);
        int n = 2 + (int)(k % 5);
        cn_new(&G, seed, n);
        while (G.phase != CN_PH_OVER) {
            CnMove m = bot_move(&G);
            if (!cn_apply(&G, G.turn, m)) break;
            if (rnd(10) == 0) {
                CHECK(cn_replay(&H, &G, G.hist_n), "game %u replays", k);
                CHECK(cn_hash(&H) == cn_hash(&G) && !memcmp(H.dice, G.dice, sizeof G.dice), "game %u same at %d", k, G.hist_n);
            }
        }
        CHECK(G.phase == CN_PH_OVER, "game %u ends", k);
        CHECK(cn_replay(&H, &G, G.hist_n) && cn_hash(&H) == cn_hash(&G), "game %u: the end replays", k);
        /* a history that is not legal does not replay */
        if (G.hist_n > 2) {
            H = G;
            H.hist[1] = H.hist[0];                  /* the same bid twice is never a raise */
            CHECK(!cn_is_call(H.hist[0]) ? !cn_replay(&H, &H, H.hist_n) : 1, "game %u: a repeated bid refuses", k);
        }
    }
}

static void test_longest(void)
{
    TEST("the longest game");
    for (int n = 2; n <= 6; n++) {
        uint8_t seed[32];
        seed_wide(seed, 50 + n);
        cn_new(&G, seed, n);
        while (G.phase != CN_PH_OVER && cn_apply(&G, G.turn, long_move(&G))) {}
        CHECK(G.phase == CN_PH_OVER, "n %d ends", n);
        CHECK(G.hist_n == CN_MAX_MOVES_OF(n), "n %d: %d moves, the bound %d", n, G.hist_n, CN_MAX_MOVES_OF(n));
        CHECK(G.round == 5 * n - 2, "n %d: rounds %d", n, G.round + 1);
    }
    CHECK(CN_MAX_MOVES == CN_MAX_MOVES_OF(6), "the cap is six seats'");
}

int main(void)
{
    test_start();
    test_opening();
    test_raise();
    test_call();
    test_next_round();
    test_over();
    test_refusal_untouched();
    test_replay();
    test_longest();
    return report("cn_test");
}

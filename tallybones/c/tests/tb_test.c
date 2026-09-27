/* The rules (T3 to T5), the randomness (T6) and THE T11 GROUP: a reroll
 * cannot be previewed, the keep choice is an input, and the move alphabet
 * has no value field. MUTATIONS.md has the mutation each one was seen red on. */
#include "tb_check.h"
#include "../src/tb_internal.h"
#include "../src/tb_plan.h"

/* ---- T4: every category on hand-picked dice --------------------------------- */

static void t4_categories(void)
{
    TEST("T4 categories");
    static const struct { uint8_t d[5]; int cat, want; } row[] = {
        { { 1, 1, 2, 3, 1 }, TB_C_ONES, 3 },
        { { 2, 2, 2, 5, 6 }, TB_C_TWOS, 6 },
        { { 3, 1, 3, 4, 3 }, TB_C_THREES, 9 },
        { { 4, 4, 4, 4, 1 }, TB_C_FOURS, 16 },
        { { 5, 1, 2, 3, 4 }, TB_C_FIVES, 5 },
        { { 6, 6, 6, 6, 6 }, TB_C_SIXES, 30 },
        { { 1, 2, 3, 4, 5 }, TB_C_SIXES, 0 },
        { { 3, 3, 3, 2, 6 }, TB_C_THREE_ALIKE, 17 },
        { { 3, 3, 2, 2, 6 }, TB_C_THREE_ALIKE, 0 },
        { { 5, 5, 5, 5, 5 }, TB_C_THREE_ALIKE, 25 },
        { { 2, 2, 2, 2, 6 }, TB_C_FOUR_ALIKE, 14 },
        { { 2, 2, 2, 3, 6 }, TB_C_FOUR_ALIKE, 0 },
        { { 2, 2, 3, 3, 3 }, TB_C_FULL_HOUSE, 25 },
        { { 6, 1, 6, 1, 1 }, TB_C_FULL_HOUSE, 25 },
        { { 2, 2, 2, 2, 3 }, TB_C_FULL_HOUSE, 0 },
        { { 4, 4, 4, 4, 4 }, TB_C_FULL_HOUSE, 0 },          /* five alike is not a full house */
        { { 1, 2, 3, 4, 6 }, TB_C_SHORT_RUN, 30 },
        { { 3, 4, 5, 6, 6 }, TB_C_SHORT_RUN, 30 },
        { { 6, 2, 4, 3, 1 }, TB_C_SHORT_RUN, 30 },
        { { 1, 2, 3, 5, 6 }, TB_C_SHORT_RUN, 0 },
        { { 2, 3, 4, 5, 6 }, TB_C_SHORT_RUN, 30 },          /* a long run holds a short one */
        { { 5, 4, 3, 2, 1 }, TB_C_LONG_RUN, 40 },
        { { 2, 3, 4, 5, 6 }, TB_C_LONG_RUN, 40 },
        { { 1, 2, 3, 4, 6 }, TB_C_LONG_RUN, 0 },
        { { 3, 3, 3, 3, 3 }, TB_C_TALLYBONES, 50 },
        { { 3, 3, 3, 3, 2 }, TB_C_TALLYBONES, 0 },
        { { 1, 6, 2, 5, 3 }, TB_C_ANY, 17 },
        { { 6, 6, 6, 6, 6 }, TB_C_ANY, 30 },
    };
    for (size_t i = 0; i < sizeof row / sizeof row[0]; i++) {
        int got = tb_score_of(row[i].d, row[i].cat);
        CHECK(got == row[i].want, "row %zu: %d%d%d%d%d in %d scores %d, want %d", i, row[i].d[0], row[i].d[1],
              row[i].d[2], row[i].d[3], row[i].d[4], row[i].cat, got, row[i].want);
    }
    uint8_t bad[5] = { 1, 2, 0, 4, 5 };
    CHECK(tb_score_of(bad, TB_C_ANY) == -1, "an unknown die scores nothing");
    CHECK(tb_score_of(row[0].d, TB_CATS) == -1, "a category off the card");
}

/* ---- T4: the bonus and zero ---------------------------------------------------- */

static void t4_bonus(void)
{
    TEST("T4 bonus");
    TbGame g;
    uint8_t seed[32];
    seed_wide(seed, 1);
    tb_new(&g, seed, 2, 0);
    static const uint8_t three_each[6] = { 3, 6, 9, 12, 15, 18 };     /* 63 */
    for (int c = 0; c < 6; c++) g.score[0][c] = three_each[c];
    CHECK(tb_upper(&g, 0) == 63 && tb_bonus(&g, 0) == 35, "63 earns the bonus: %d", tb_bonus(&g, 0));
    CHECK(tb_total(&g, 0) == 98, "total 63 + 35 = %d", tb_total(&g, 0));
    g.score[0][0] = 2;
    CHECK(tb_upper(&g, 0) == 62 && tb_bonus(&g, 0) == 0, "62 does not: %d", tb_bonus(&g, 0));
    g.score[0][TB_C_ANY] = 20;
    CHECK(tb_total(&g, 0) == 82, "total 62 + 20 = %d", tb_total(&g, 0));
}

static void t4_zero(void)
{
    TEST("T4 zero");
    uint8_t seed[32];
    TbGame g;
    /* find a first roll that is not five alike, and score it as Tallybones */
    for (uint32_t k = 0;; k++) {
        seed_wide(seed, 100 + k);
        tb_new(&g, seed, 2, 0);
        if (tb_score_of(g.dice, TB_C_TALLYBONES) == 0) break;
    }
    TbMove h[1] = { mv(TB_M_SCORE, 0, TB_C_TALLYBONES) };
    CHECK(tb_is_legal(&g, h[0]), "any open category may be taken");
    CHECK(tb_replay(&g, seed, 2, 0, h, 1), "the replay");
    CHECK(g.score[0][TB_C_TALLYBONES] == 0 && (g.filled[0] >> TB_C_TALLYBONES & 1), "taken for zero, and filled");
    CHECK(g.turn == 1 && g.roll == 1 && g.turns == 1, "Bo to roll: turn %d roll %d", g.turn, g.roll);
}

/* ---- T3: legality ------------------------------------------------------------------ */

static void t3_legality(void)
{
    TEST("T3 legality");
    uint8_t seed[32];
    seed_wide(seed, 7);
    TbGame g;
    tb_new(&g, seed, 3, 0);
    CHECK(g.turn == 0 && g.roll == 1, "seat 0 rolls first");
    for (int i = 0; i < 5; i++) CHECK(g.dice[i] >= 1 && g.dice[i] <= 6, "roll 1 die %d = %d", i, g.dice[i]);
    CHECK(tb_is_legal(&g, mv(TB_M_KEEP, 0, 30)), "keep four");
    CHECK(!tb_is_legal(&g, mv(TB_M_KEEP, 0, 31)), "keeping all five is not a reroll");
    CHECK(!tb_is_legal(&g, mv(TB_M_KEEP, 1, 0)), "not seat 1's turn");
    CHECK(!tb_is_legal(&g, mv(TB_M_SCORE, 1, 0)), "not seat 1's card");
    CHECK(tb_is_legal(&g, mv(TB_M_LEAVE, 2, 0)), "anyone may leave");
    TbMove m[TB_MENU_MAX];
    int n = tb_menu(&g, m, TB_MENU_MAX);
    CHECK(n == 31 + 13 + 3, "menu 31 keeps, 13 scores, 3 leaves: %d", n);
    CHECK(m[0].kind == TB_M_KEEP && m[0].arg == 0 && m[30].arg == 30 && m[31].kind == TB_M_SCORE && m[43].arg == 12
          && m[44].kind == TB_M_LEAVE && m[44].seat == 0 && m[46].seat == 2, "the menu's order is the format's");
    TbMove h[3] = { mv(TB_M_KEEP, 0, 3), mv(TB_M_KEEP, 0, 7) };
    CHECK(tb_replay(&g, seed, 3, 0, h, 2), "keep, keep");
    CHECK(g.roll == 3 && g.kept == 7, "roll 3, kept 7: %d %d", g.roll, g.kept);
    CHECK(!tb_is_legal(&g, mv(TB_M_KEEP, 0, 0)), "no fourth roll");
    n = tb_menu(&g, m, TB_MENU_MAX);
    CHECK(n == 13 + 3 && m[0].kind == TB_M_SCORE, "roll 3: only scores and leaves (%d)", n);
    h[2] = mv(TB_M_SCORE, 0, TB_C_ANY);
    CHECK(tb_replay(&g, seed, 3, 0, h, 3), "and a score");
    CHECK(!tb_is_legal(&g, mv(TB_M_SCORE, 0, TB_C_ANY)), "not twice");
    CHECK(g.turn == 1, "seat order");
}

/* ---- T5: the end, ties, a left seat skipped ------------------------------------------ */

static void t5_game_end(void)
{
    TEST("T5 game end");
    for (int n = 2; n <= 8; n += 3) {
        uint8_t seed[32];
        seed_wide(seed, 50 + (uint32_t)n);
        TbGame g;
        tb_new(&g, seed, n, 0);
        int scores = 0, guard = 0;
        while (!g.over && guard++ < 1000) {
            int was = g.hist_n;
            CHECK(bot_step(&g, 0), "a bot move replays");
            scores += g.hist[was].kind == TB_M_SCORE;
        }
        CHECK(g.over && scores == 13 * n, "%d seats: over after %d scores", n, scores);
        for (int s = 0; s < n; s++) CHECK(g.filled[s] == TB_FULL_CARD, "seat %d full", s);
        CHECK(g.turn == TB_SEAT_NONE && tb_menu(&g, 0, 0) == 0, "nothing more to do");
        int w = tb_winners(&g), best = 0;
        for (int s = 0; s < n; s++) if (tb_total(&g, s) > best) best = tb_total(&g, s);
        for (int s = 0; s < n; s++) CHECK(((w >> s) & 1) == (tb_total(&g, s) == best), "winner bit %d", s);
    }
}

static void t5_ties(void)
{
    TEST("T5 ties");
    uint8_t seed[32];
    seed_wide(seed, 3);
    TbGame g;
    tb_new(&g, seed, 3, 0);
    CHECK(tb_winners(&g) == 0, "no winner while live");
    g.over = 1;
    g.score[0][TB_C_ANY] = 20;
    g.score[1][TB_C_ANY] = 20;
    g.score[2][TB_C_ANY] = 19;
    CHECK(tb_winners(&g) == 3, "seats 0 and 1 share it: %d", tb_winners(&g));
    g.score[2][TB_C_ONES] = 1;
    CHECK(tb_winners(&g) == 7, "a three-way tie: %d", tb_winners(&g));
    g.left = 1;
    CHECK(tb_winners(&g) == 6, "a seat that left does not win: %d", tb_winners(&g));
}

static void t5_left_skipped(void)
{
    TEST("T5 left skipped");
    uint8_t seed[32];
    seed_wide(seed, 9);
    TbGame g;
    TbMove h[8] = { mv(TB_M_LEAVE, 1, 0), mv(TB_M_SCORE, 0, TB_C_ANY) };
    CHECK(tb_replay(&g, seed, 3, 0, h, 2), "seat 1 leaves on seat 0's turn, seat 0 scores");
    CHECK(g.turn == 2 && !g.over, "seat 2 rolls next, seat 1 skipped: %d", g.turn);
    CHECK(!tb_is_legal(&g, mv(TB_M_LEAVE, 1, 0)), "gone is gone");
    h[2] = mv(TB_M_SCORE, 2, TB_C_ONES);
    CHECK(tb_replay(&g, seed, 3, 0, h, 3) && g.turn == 0, "and back to seat 0, skipping 1 again: %d", g.turn);
    /* the turn seat leaves: the next one rolls at once */
    h[3] = mv(TB_M_LEAVE, 0, 0);
    CHECK(tb_replay(&g, seed, 3, 0, h, 4), "seat 0 leaves on its own turn");
    CHECK(g.over && tb_winners(&g) == 4, "one seat left: over, seat 2 wins (%d, %d)", g.over, tb_winners(&g));
    int cards_stand = g.score[0][TB_C_ANY] > 0 && (g.filled[0] >> TB_C_ANY & 1);
    CHECK(cards_stand, "a leaver's card stands");
    TbMove h2[2] = { mv(TB_M_KEEP, 0, 0), mv(TB_M_LEAVE, 0, 0) };
    CHECK(tb_replay(&g, seed, 3, 0, h2, 2), "leave mid-turn");
    CHECK(g.turn == 1 && g.roll == 1 && g.kept == 0 && !g.over, "seat 1 rolls fresh: %d %d", g.turn, g.roll);
}

/* ---- T6: the derivation is the one the spec names --------------------------------------- */

static void t6_formula(void)
{
    TEST("T6 formula");
    for (uint32_t k = 0; k < 12; k++) {
        uint8_t seed[32], body[TB_CODE_MAX], want[5];
        seed_wide(seed, 900 + k);
        TbGame g;
        int n = 2 + (int)(k % 4);
        tb_new(&g, seed, n, 0);
        int bl = tb_code_body(&g, 0, body, sizeof body);
        CHECK(bl == 1 && body[0] == 1, "the empty history is the sentinel");
        spec_roll(seed, body, bl, 0, 0, 1, want);
        CHECK(!memcmp(g.dice, want, 5), "roll 1 of turn 0 from the seed and the empty history");
        for (int step = 0; step < 60 && !g.over; step++) {
            TbGame prev = g;
            bot_step(&g, 40);
            TbMove m = g.hist[g.hist_n - 1];
            int rolled = m.kind == TB_M_KEEP || g.turn != prev.turn || g.turns != prev.turns;
            if (!rolled || g.over) continue;
            bl = tb_code_body(&g, g.hist_n, body, sizeof body);
            CHECK(bl > 0, "a body");
            if (bl <= 0) continue;
            spec_roll(seed, body, bl, g.turn, g.turns, g.roll, want);
            for (int i = 0; i < 5; i++) {
                int kept = m.kind == TB_M_KEEP && (m.arg >> i & 1);
                CHECK(g.dice[i] == (kept ? prev.dice[i] : want[i]), "game %u bubble %d die %d: %d want %d", k,
                      g.hist_n, i, g.dice[i], kept ? prev.dice[i] : want[i]);
            }
        }
    }
}

static void t6_same_history(void)
{
    TEST("T6 same history");
    uint8_t seed[32];
    seed_wide(seed, 77);
    TbGame a, b;
    tb_new(&a, seed, 4, 1);
    while (a.hist_n < 40 && bot_step(&a, 0)) {}
    for (int i = 0; i < 5; i++) {
        CHECK(tb_replay(&b, seed, 4, 1, a.hist, a.hist_n), "replay %d", i);
        CHECK(tb_hash(&a) == tb_hash(&b) && !memcmp(a.dice, b.dice, 5), "same history, same dice (%d)", i);
    }
}

/* ---- THE T11 GROUP ------------------------------------------------------------------------ */

/* A resident game on roll 1 of some turn, and two keep subsets that both
 * reroll positions 3 and 4 (A keeps 0 and 1; B keeps 0, 1 and 2). */
#define KEEP_A 0x03
#define KEEP_B 0x07

static void resident_on_roll1(TbGame *g, uint32_t k)
{
    uint8_t seed[32];
    seed_wide(seed, 4000 + k);
    tb_new(g, seed, 2 + (int)(k % 3), 0);
    /* a turn or two in, so the history is not empty */
    int turns = (int)(k % 3);
    while (g->turns < turns && bot_step(g, 0)) {}
    while (g->roll != 1 && bot_step(g, 0)) {}
}

static void t11_draft_unknown(void)
{
    TEST("T11.1 draft unknown");
    for (uint32_t k = 0; k < 32; k++) {
        TbGame g, d;
        resident_on_roll1(&g, k);
        const TbGame before = g;
        const int keeps[2] = { KEEP_A, KEEP_B };
        for (int j = 0; j < 2; j++) {
            CHECK(tb_draft(&d, &g, mv(TB_M_KEEP, g.turn, keeps[j])), "stage keep %d", keeps[j]);
            CHECK(d.draft == 1 && d.roll == 2 && d.kept == keeps[j], "a draft of roll 2");
            for (int i = 0; i < 5; i++) {
                int kept = keeps[j] >> i & 1;
                CHECK(kept ? d.dice[i] == g.dice[i] : d.dice[i] == 0,
                      "game %u keep %d: die %d is %d, a draft has values only where kept", k, keeps[j], i, d.dice[i]);
            }
            TbEvent ev[16];
            int n = tb_plan_move(&g, mv(TB_M_KEEP, g.turn, keeps[j]), ev, 16);
            int roll_seen = 0;
            for (int e = 0; e < n; e++)
                if (ev[e].kind == TB_EV_ROLL) {
                    roll_seen = 1;
                    for (int i = 0; i < 5; i++)
                        CHECK(!(ev[e].mask >> i & 1) || ev[e].dice[i] == 0, "the draft's ROLL event has no value");
                }
            CHECK(roll_seen, "the draft's plan rolls, with nothing derived (%d events)", n);
            /* cancel: the resident is untouched, and a draft cannot be drafted on */
            CHECK(tb_hash(&g) == tb_hash(&before), "cancel: the resident never moved");
            CHECK(!tb_draft(&g, &d, mv(TB_M_KEEP, g.turn, 0)) && tb_menu(&d, 0, 0) == 0, "no draft on a draft");
        }
        /* a draft SCORE passes the turn and derives nothing for the next seat */
        CHECK(tb_draft(&d, &g, mv(TB_M_SCORE, g.turn, TB_C_ANY)), "stage a score");
        if (!d.over) for (int i = 0; i < 5; i++) CHECK(d.dice[i] == 0, "the next seat's roll 1 is unknown in a draft");
    }
}

static void t11_adopt(void)
{
    TEST("T11.2 adopt");
    int same = 0;
    for (uint32_t k = 0; k < 64; k++) {
        TbGame g, a, b, again;
        resident_on_roll1(&g, k);
        TbMove h[TB_HIST_CAP];
        memcpy(h, g.hist, sizeof(TbMove) * g.hist_n);
        h[g.hist_n] = mv(TB_M_KEEP, g.turn, KEEP_A);
        CHECK(tb_replay(&a, g.seed, g.n, g.starter, h, g.hist_n + 1), "adopt A");
        for (int i = 0; i < 5; i++) CHECK(a.dice[i] >= 1 && a.dice[i] <= 6, "adopted: die %d has a value", i);
        CHECK(a.dice[0] == g.dice[0] && a.dice[1] == g.dice[1], "the kept dice held");
        /* stable: again, and through the wire's bytes */
        CHECK(tb_replay(&again, g.seed, g.n, g.starter, h, g.hist_n + 1) && !memcmp(again.dice, a.dice, 5),
              "the same reroll every time it is asked");
        uint8_t body[TB_CODE_MAX];
        int bl = tb_code_encode(&a, body, sizeof body);
        for (int r = 0; r < 3; r++) {
            CHECK(tb_code_decode(&again, g.seed, g.n, g.starter, a.hist_n, body, bl, 1)
                  && !memcmp(again.dice, a.dice, 5) && tb_hash(&again) == tb_hash(&a), "decode %d", r);
        }
        h[g.hist_n] = mv(TB_M_KEEP, g.turn, KEEP_B);
        CHECK(tb_replay(&b, g.seed, g.n, g.starter, h, g.hist_n + 1), "adopt B");
        same += a.dice[3] == b.dice[3] && a.dice[4] == b.dice[4];
    }
    /* one seed in 36 matches by chance on two dice; a derivation blind to
     * the subset matches on all 64 */
    CHECK(same <= 8, "A and B produce the same reroll at positions 3 and 4 in %d of 64 games", same);
}

static void t11_no_value_field(void)
{
    TEST("T11.3 no value field");
    CHECK(sizeof(TbMove) == 4, "a move is four bytes: kind, seat, arg, pad");
    for (uint32_t k = 0; k < 16; k++) {
        TbGame g;
        resident_on_roll1(&g, k);
        while (g.hist_n < 30 && !g.over && bot_step(&g, 50)) {}
        /* the menu and the body are the same whatever the dice show */
        TbMove m1[TB_MENU_MAX], m2[TB_MENU_MAX];
        uint8_t b1[TB_CODE_MAX], b2[TB_CODE_MAX];
        int n1 = tb_menu(&g, m1, TB_MENU_MAX), l1 = tb_code_encode(&g, b1, sizeof b1);
        TbGame x = g;
        for (int i = 0; i < 5; i++) x.dice[i] = (uint8_t)(1 + (x.dice[i] % 6));
        for (int s = 0; s < x.n; s++) for (int c = 0; c < 13; c++) x.score[s][c] = (uint8_t)(x.score[s][c] + 1);
        int n2 = tb_menu(&x, m2, TB_MENU_MAX), l2 = tb_code_encode(&x, b2, sizeof b2);
        CHECK(n1 == n2 && !memcmp(m1, m2, sizeof(TbMove) * (size_t)n1), "the menu reads no die");
        CHECK(l1 > 0 && l1 == l2 && !memcmp(b1, b2, (size_t)l1), "the body carries no die");
        for (int i = 0; i < n1; i++)
            CHECK(m1[i].pad0 == 0 && (m1[i].kind != TB_M_KEEP || m1[i].arg < 31)
                  && (m1[i].kind != TB_M_SCORE || m1[i].arg < 13) && (m1[i].kind != TB_M_LEAVE || m1[i].arg == 0),
                  "a move's argument is a mask, a category or nothing");
    }
}

int main(void)
{
    t4_categories();
    t4_bonus();
    t4_zero();
    t3_legality();
    t5_game_end();
    t5_ties();
    t5_left_skipped();
    t6_formula();
    t6_same_history();
    t11_draft_unknown();
    t11_adopt();
    t11_no_value_field();
    return report("tb_test");
}

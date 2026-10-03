/* The greedy lookahead (src/uttt_big_bot.h), held to what it says it is.
 *
 *   THE SCORE IS THE WEIGHTS. Hand-built depth-4 positions: a move that wins
 *   a 3 x 3 is worth 1, one that wins the 3 x 3 that completes a 9 x 9 is
 *   worth 1 + 9 and is preferred, and a move that wins a 3 x 3 but sends the
 *   opponent where it wins a 27 x 27 is worth 1 - (1 + 9 + 81) two plies
 *   deep: the one-ply bot takes it and the two-ply bot does not.
 *
 *   A GAME IS ITS SEED. The same seed plays the same game move for move;
 *   another seed plays another.
 *
 *   IT IS A PLAYER. It beats uniform random play at depth 2 and depth 3 by a
 *   margin no coin makes.
 *
 *   IT IS FAST ENOUGH TO WATCH. Two bots at the page's settings play a whole
 *   depth-5 game, every move within its budget, and the speed is printed.
 *
 *     ./uttt/c/build/uttt_big_bot_test [games]
 */
#include "../src/uttt_big_bot.h"
#include "../../../shared/c/test/check.h"
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------ positions */

static int leaf4(int a, int b, int c, int d) { return ((a * 9 + b) * 9 + c) * 9 + d; }

static uint8_t cells[UTB_LEAVES_MAX];
static int32_t legal[UTB_LEAVES_MAX];

static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

/* The material arithmetic alone: no threats, no extension. The first tests
 * hold the weights to exact values with it; the threat and extension tests
 * below turn each back on. */
static void material_only(UtbBot *b) { b->threats = 0; b->extend = 0; }

/* A 3 x 3 won by `mark` on its top row. */
static void won3(int a, int b, int c, int mark)
{
    for (int k = 0; k < 3; k++) cells[leaf4(a, b, c, k)] = (uint8_t)mark;
}

/* A 9 x 9 won by `mark`: its top row of 3 x 3s, each won on its top row. */
static void won9(int a, int b, int mark)
{
    for (int c = 0; c < 3; c++) won3(a, b, c, mark);
}

/* Make the counts a game's (X to move: as many X as O) with single marks,
 * one to a 3 x 3, in the 27 x 27 at 8 past its first 9 x 9 - far from every
 * line the tests read. Then adopt with `last`. */
static int build(UtbGame *g, int last)
{
    int n[3] = { 0, 0, 0 };
    for (int i = 0; i < 6561; i++) n[cells[i]]++;
    int short_side = n[UTTT_X] < n[UTTT_O] ? UTTT_X : UTTT_O;
    int need = n[UTTT_X] > n[UTTT_O] ? n[UTTT_X] - n[UTTT_O] : n[UTTT_O] - n[UTTT_X];
    for (int b = 1; b < 9 && need; b++)
        for (int c = 0; c < 9 && need; c++, need--) cells[leaf4(8, b, c, 4)] = (uint8_t)short_side;
    return utb_adopt(g, 4, cells, last);
}

/* THE WEIGHTS: winning a 3 x 3 is 1; winning the 3 x 3 that completes a
 * 9 x 9 is 1 + 9, and it is the move. X to move, sent into the 9 x 9 (0,0)
 * because the block the last move named, (0,0,0), is X's. */
static void weights_rank(void)
{
    TEST("weights_rank");
    memset(cells, 0, sizeof cells);
    won3(0, 0, 0, UTTT_X);
    won3(0, 0, 1, UTTT_X);
    cells[leaf4(0, 0, 2, 0)] = cells[leaf4(0, 0, 2, 1)] = UTTT_X;     /* 2 wins it, and the row */
    cells[leaf4(0, 0, 4, 3)] = cells[leaf4(0, 0, 4, 4)] = UTTT_X;     /* 5 wins it, nothing more */
    int last = leaf4(8, 0, 0, 0);
    cells[last] = UTTT_O;
    UtbGame g;
    CHECK(build(&g, last), "the position adopts");
    CHECK(g.turn == UTTT_X, "X to move");
    CHECK(utb_region(&g) == utb_node_id(&g, 2, 0), "sent into the 9 x 9 (0,0)");

    const int nine = leaf4(0, 0, 2, 2), three = leaf4(0, 0, 4, 5), none = leaf4(0, 0, 3, 0);
    CHECK(utb_bot_gain(&g, nine) == UTB_BOT_W3 + UTB_BOT_W9, "the 9 x 9 move wins %d", utb_bot_gain(&g, nine));
    CHECK(utb_bot_gain(&g, three) == UTB_BOT_W3, "the 3 x 3 move wins %d", utb_bot_gain(&g, three));
    CHECK(utb_bot_gain(&g, none) == 0, "a quiet move wins %d", utb_bot_gain(&g, none));
    CHECK(utb_bot_gain(&g, leaf4(0, 1, 0, 0)) == -1, "outside the region is illegal");
    CHECK(utb_bot_material(&g, UTTT_X) == 2 * UTB_BOT_W3, "X holds two 3 x 3s: %d", utb_bot_material(&g, UTTT_X));

    UtbGame before = g;
    for (int plies = 1; plies <= 3; plies++) {
        for (int s = 0; s < 8; s++) {
            UtbBot b; utb_bot_init(&b, (uint64_t)s);
            b.plies = plies;
            int mv = utb_bot_move(&b, &g);
            CHECK(mv == nine, "plies %d seed %d: played %d, not the 9 x 9", plies, s, mv);
            CHECK(!memcmp(&g, &before, sizeof g), "plies %d: the game came back as it went", plies);
        }
        UtbBot b; utb_bot_init(&b, 1); material_only(&b);
        CHECK(utb_bot_value(&b, &g, nine, plies) > utb_bot_value(&b, &g, three, plies)
              && utb_bot_value(&b, &g, three, plies) > utb_bot_value(&b, &g, none, plies),
              "plies %d: 9 x 9 %d > 3 x 3 %d > quiet %d", plies,
              utb_bot_value(&b, &g, nine, plies), utb_bot_value(&b, &g, three, plies), utb_bot_value(&b, &g, none, plies));
    }
    /* fifty-nine open cells and room for three: the winners go first */
    for (int s = 0; s < 8; s++) {
        UtbBot b; utb_bot_init(&b, (uint64_t)s);
        b.cap_root = 3;
        CHECK(utb_bot_move(&b, &g) == nine, "seed %d, three candidates: the 9 x 9 is among them", s);
    }
    UtbBot b; utb_bot_init(&b, 1); material_only(&b);
    CHECK(utb_bot_value(&b, &g, nine, 2) == UTB_BOT_W3 + UTB_BOT_W9, "two plies: the reply wins nothing back");
    utb_play(&g, nine);
    CHECK(utb_bot_material(&g, UTTT_X) == 3 * UTB_BOT_W3 + UTB_BOT_W9, "after: %d", utb_bot_material(&g, UTTT_X));
}

/* THE GIFT: X's cell 2 of (0,4,0) wins that 3 x 3, and sends O to (4,0,2),
 * where O's cell 2 wins the 3 x 3, its 9 x 9 (4,0) and the 27 x 27 at 4.
 * Every other X move sends O to an empty 3 x 3. */
static void hands_over_27(void)
{
    TEST("hands_over_27");
    memset(cells, 0, sizeof cells);
    cells[leaf4(0, 4, 0, 0)] = cells[leaf4(0, 4, 0, 1)] = UTTT_X;
    won9(4, 1, UTTT_O);
    won9(4, 2, UTTT_O);
    won3(4, 0, 0, UTTT_O);
    won3(4, 0, 1, UTTT_O);
    cells[leaf4(4, 0, 2, 0)] = cells[leaf4(4, 0, 2, 1)] = UTTT_O;
    /* ...and the third ply: X has two in a row in every 3 x 3 of the 9 x 9
     * (0,3), so X's cell 3 sends O to (4,0,3), from where every O reply
     * sends X back into (0,3) to win a 3 x 3: worth 1 at three plies and
     * nothing at two. */
    for (int j = 0; j < 9; j++) cells[leaf4(0, 3, j, 0)] = cells[leaf4(0, 3, j, 1)] = UTTT_X;
    int last = leaf4(8, 0, 4, 0);                   /* names (0,4,0) */
    cells[last] = UTTT_O;
    UtbGame g;
    CHECK(build(&g, last), "the position adopts");
    CHECK(g.turn == UTTT_X, "X to move");
    CHECK(utb_region(&g) == utb_node_id(&g, 3, leaf4(0, 4, 0, 0) / 9), "sent into (0,4,0)");

    CHECK(utb_bot_material(&g, UTTT_O) == 2 * (3 * UTB_BOT_W3 + UTB_BOT_W9) + 2 * UTB_BOT_W3,
          "O holds two 9 x 9s and two more 3 x 3s: %d", utb_bot_material(&g, UTTT_O));
    CHECK(utb_bot_material(&g, UTTT_X) == 0, "X holds nothing: %d", utb_bot_material(&g, UTTT_X));
    const int gift = leaf4(0, 4, 0, 2);
    const int big = UTB_BOT_W3 + UTB_BOT_W9 + UTB_BOT_W27;
    UtbBot b; utb_bot_init(&b, 3); material_only(&b);
    CHECK(utb_bot_value(&b, &g, gift, 1) == UTB_BOT_W3, "one ply: the gift wins its 3 x 3");
    CHECK(utb_bot_value(&b, &g, gift, 2) == UTB_BOT_W3 - big,
          "two plies: 9 - 819, not %d", utb_bot_value(&b, &g, gift, 2));
    for (int k = 3; k < 9; k++)
        CHECK(utb_bot_value(&b, &g, leaf4(0, 4, 0, k), 2) == 0, "a quiet move %d is worth nothing", k);
    const int setup = leaf4(0, 4, 0, 3);
    CHECK(utb_bot_value(&b, &g, setup, 3) == UTB_BOT_W3, "three plies: the set-up is worth a 3 x 3, not %d", utb_bot_value(&b, &g, setup, 3));
    CHECK(utb_bot_value(&b, &g, leaf4(0, 4, 0, 4), 3) == 0, "three plies: another quiet move is worth nothing");
    CHECK(utb_bot_value(&b, &g, gift, 3) == UTB_BOT_W3 - big, "three plies: the gift is still 9 - 819");

    UtbGame h = g;
    utb_play(&h, gift);
    CHECK(utb_bot_gain(&h, leaf4(4, 0, 2, 2)) == big, "O's reply wins %d", utb_bot_gain(&h, leaf4(4, 0, 2, 2)));

    for (int s = 0; s < 8; s++) {
        UtbBot one; utb_bot_init(&one, (uint64_t)s); one.plies = 1; material_only(&one);
        CHECK(utb_bot_move(&one, &g) == gift, "seed %d: one ply takes the 3 x 3", s);
        for (int plies = 2; plies <= 4; plies++) {
            UtbBot two; utb_bot_init(&two, (uint64_t)s); two.plies = plies; material_only(&two);
            int mv = utb_bot_move(&two, &g);
            CHECK(mv != gift && utb_legal_at(&g, mv), "seed %d plies %d: does not hand over the 27 x 27 (%d)", s, plies, mv);
            if (plies == 2) CHECK(two.value == 0, "plies 2: value %d", two.value);
            else CHECK(mv == setup && two.value == UTB_BOT_W3, "seed %d plies %d: the set-up (%d, value %d)", s, plies, mv, two.value);
        }
    }
}

/* TIES ARE THE DICE: five moves that each win a 3 x 3 for 1 and nothing
 * else, and eight quiet moves of 0 - sixteen seeds pick many different
 * winners at one ply, never a quiet move, and on an empty 3 x 3 many
 * different quiet moves. */
static void tie_break(void)
{
    TEST("tie_break");
    memset(cells, 0, sizeof cells);
    cells[leaf4(2, 2, 2, 0)] = cells[leaf4(2, 2, 2, 1)] = UTTT_X;
    cells[leaf4(2, 2, 2, 3)] = cells[leaf4(2, 2, 2, 4)] = UTTT_X;
    int last = leaf4(8, 2, 2, 2);                   /* names (2,2,2) */
    cells[last] = UTTT_O;
    UtbGame g;
    CHECK(build(&g, last), "the position adopts");
    unsigned seen = 0;
    for (int s = 0; s < 16; s++) {
        UtbBot b; utb_bot_init(&b, (uint64_t)s); b.plies = 1;
        int mv = utb_bot_move(&b, &g);
        CHECK(utb_bot_gain(&g, mv) == UTB_BOT_W3, "seed %d: played %d, which wins nothing", s, mv);
        if (mv >= 0) seen |= 1u << (mv % 9);
    }
    int n = 0;
    for (int k = 0; k < 9; k++) n += (seen >> k) & 1u;
    CHECK(n >= 3, "sixteen seeds chose only %d different winners", n);

    utb_init(&g, 4);
    utb_play(&g, 0);                                /* O is sent to (0,0,0) */
    seen = 0;
    for (int s = 0; s < 16; s++) {
        UtbBot b; utb_bot_init(&b, (uint64_t)s); b.plies = 1;
        int mv = utb_bot_move(&b, &g);
        if (mv >= 0) seen |= 1u << (mv % 9);
    }
    n = 0;
    for (int k = 0; k < 9; k++) n += (seen >> k) & 1u;
    CHECK(n >= 4, "sixteen seeds chose only %d different quiet moves", n);
}

/* A DRAW IS WORTH NOTHING: X's only move fills (3,3,3) without a line. */
static void draw_is_nothing(void)
{
    TEST("draw_is_nothing");
    memset(cells, 0, sizeof cells);
    static const uint8_t fill[8] = { UTTT_X, UTTT_O, UTTT_X, UTTT_X, UTTT_O, UTTT_O, UTTT_O, UTTT_X };
    for (int k = 0; k < 8; k++) cells[leaf4(3, 3, 3, k)] = fill[k];
    int last = leaf4(8, 3, 3, 3);                   /* names (3,3,3) */
    cells[last] = UTTT_O;
    UtbGame g;
    CHECK(build(&g, last), "the position adopts");
    const int mv = leaf4(3, 3, 3, 8);
    CHECK(utb_bot_gain(&g, mv) == 0, "drawing a 3 x 3 gains %d", utb_bot_gain(&g, mv));
    UtbBot b; utb_bot_init(&b, 5); b.plies = 1;
    CHECK(utb_bot_move(&b, &g) == mv && b.value == 0, "the only move, worth %d", b.value);
    utb_play(&g, mv);
    CHECK(utb_node(&g, utb_ancestor(&g, mv, 3)) == UTTT_DRAW, "and it is a draw");
}

/* THREATS ARE SCORED, between the wins they sit between. In the 9 x 9
 * (0,0) of a depth-4 board: one 3 x 3 won is its weight; two won in a line
 * with the third open is their weight plus one 9 x 9 threat, which is more
 * than a 3 x 3 and less than the 9 x 9 it threatens; the third won is the
 * 9 x 9 and no threat. A threat is blocked by the other side's grid, or a
 * drawn one, in the line, and dies with the grid above it. */
static int fresh(UtbGame *g)
{
    memset(cells + leaf4(8, 0, 0, 0), 0, 729);      /* build's own marks, again */
    int last = leaf4(8, 0, 0, 0);
    cells[last] = UTTT_O;
    return build(g, last);
}

static void threats_rank(void)
{
    TEST("threats_rank");
    UtbGame g;
    const int t9 = UTB_BOT_THREAT_NUM * UTB_BOT_W9 / 9;
    memset(cells, 0, sizeof cells);
    won3(0, 0, 0, UTTT_X);
    CHECK(fresh(&g), "one 3 x 3 adopts");
    const int one = utb_bot_eval(&g, UTTT_X);
    CHECK(utb_bot_threats(&g, UTTT_X) == 0, "one 3 x 3 is no threat: %d", utb_bot_threats(&g, UTTT_X));
    CHECK(one == UTB_BOT_W3, "one 3 x 3 scores %d", one);

    won3(0, 0, 1, UTTT_X);
    CHECK(fresh(&g), "two 3 x 3s adopt");
    CHECK(utb_bot_threat_weight(&g, utb_node_id(&g, 2, 0)) == t9, "a 9 x 9 threat weighs %d", t9);
    CHECK(utb_bot_threats(&g, UTTT_X) == t9, "two in a row is one 9 x 9 threat: %d", utb_bot_threats(&g, UTTT_X));
    CHECK(utb_bot_threats(&g, UTTT_O) == -t9, "and it is O's loss: %d", utb_bot_threats(&g, UTTT_O));
    const int two = utb_bot_eval(&g, UTTT_X);
    const int threat = two - 2 * UTB_BOT_W3;
    CHECK(threat > UTB_BOT_W3 && threat < UTB_BOT_W9,
          "the threat (%d) sits between a 3 x 3 (%d) and the 9 x 9 (%d)", threat, UTB_BOT_W3, UTB_BOT_W9);

    won3(0, 0, 2, UTTT_X);
    CHECK(fresh(&g), "the 9 x 9 adopts");
    CHECK(utb_bot_threats(&g, UTTT_X) == 0, "a won 9 x 9 threatens nothing inside: %d", utb_bot_threats(&g, UTTT_X));
    CHECK(utb_bot_eval(&g, UTTT_X) > two, "the win (%d) is worth more than the threat (%d)", utb_bot_eval(&g, UTTT_X), two);

    /* blocked by O's 3 x 3, then by a drawn one */
    memset(cells, 0, sizeof cells);
    won3(0, 0, 0, UTTT_X); won3(0, 0, 1, UTTT_X); won3(0, 0, 2, UTTT_O);
    CHECK(fresh(&g), "blocked adopts");
    CHECK(utb_bot_threats(&g, UTTT_X) == 0, "O's 3 x 3 in the line blocks it: %d", utb_bot_threats(&g, UTTT_X));
    memset(cells, 0, sizeof cells);
    won3(0, 0, 0, UTTT_X); won3(0, 0, 1, UTTT_X);
    static const uint8_t drawn[9] = { UTTT_X, UTTT_O, UTTT_X, UTTT_X, UTTT_O, UTTT_O, UTTT_O, UTTT_X, UTTT_X };
    for (int k = 0; k < 9; k++) cells[leaf4(0, 0, 2, k)] = drawn[k];
    CHECK(fresh(&g), "drawn adopts");
    CHECK(utb_node(&g, utb_node_id(&g, 3, leaf4(0, 0, 2, 0) / 9)) == UTTT_DRAW, "the 3 x 3 is drawn");
    CHECK(utb_bot_threats(&g, UTTT_X) == 0, "a drawn 3 x 3 in the line blocks it: %d", utb_bot_threats(&g, UTTT_X));

    /* a 3 x 3 threat: two cells, the third open; dead under a decided 9 x 9 */
    memset(cells, 0, sizeof cells);
    cells[leaf4(1, 0, 0, 0)] = cells[leaf4(1, 0, 0, 1)] = UTTT_X;
    CHECK(fresh(&g), "a 3 x 3 threat adopts");
    const int t3 = UTB_BOT_THREAT_NUM * UTB_BOT_W3 / 9;
    CHECK(t3 > 0 && t3 < UTB_BOT_W3, "a 3 x 3 threat (%d) is under a 3 x 3 (%d) and over a cell (0)", t3, UTB_BOT_W3);
    CHECK(utb_bot_threats(&g, UTTT_X) == t3, "one 3 x 3 threat: %d", utb_bot_threats(&g, UTTT_X));
    won3(1, 0, 3, UTTT_O); won3(1, 0, 4, UTTT_O); won3(1, 0, 5, UTTT_O);    /* O takes the 9 x 9 (1,0) */
    CHECK(fresh(&g), "the dead threat adopts");
    CHECK(utb_bot_threats(&g, UTTT_X) == 0, "a threat under O's 9 x 9 is dead: %d", utb_bot_threats(&g, UTTT_X));

    /* a 27 x 27 threat sits between a 9 x 9 and the 27 x 27 */
    memset(cells, 0, sizeof cells);
    won9(2, 0, UTTT_X); won9(2, 1, UTTT_X);
    CHECK(fresh(&g), "two 9 x 9s adopt");
    const int t27 = utb_bot_threats(&g, UTTT_X);
    CHECK(t27 > UTB_BOT_W9 && t27 < UTB_BOT_W27, "the 27 x 27 threat (%d) is between %d and %d", t27, UTB_BOT_W9, UTB_BOT_W27);

    /* and the bot plays for it: X in (0,0,1) with two cells in its top row,
     * the 9 x 9 (0,0) holding (0,0,0) - the cell that wins the 3 x 3 makes
     * the 9 x 9 threat, and it is the one-ply move, worth the 3 x 3, less
     * the 3 x 3 threat it spends, plus the 9 x 9 threat */
    memset(cells, 0, sizeof cells);
    won3(0, 0, 0, UTTT_X);
    cells[leaf4(0, 0, 1, 0)] = cells[leaf4(0, 0, 1, 1)] = UTTT_X;
    int last = leaf4(8, 0, 0, 1);
    cells[last] = UTTT_O;
    CHECK(build(&g, last), "the threat position adopts");
    const int mv = leaf4(0, 0, 1, 2);
    CHECK(utb_bot_score(&g, mv) == UTB_BOT_W3 - t3 + t9, "the move scores %d", utb_bot_score(&g, mv));
    for (int s = 0; s < 8; s++) {
        UtbBot b; utb_bot_init(&b, (uint64_t)s); b.plies = 1;
        CHECK(utb_bot_move(&b, &g) == mv, "seed %d: one ply makes the 9 x 9 threat", s);
    }
}

/* THE SCORE IS KEPT EXACTLY. Whole random games at depths 2 to 4 (and a
 * stretch of depth 5): before every move, each legal move's one-ply score
 * (the incremental arithmetic the search runs on) equals the whole board's
 * score after it minus before, from the mover's side - except a move that
 * ends the game, which is worth the game. Up to 24 moves a position. */
static void score_is_exact(void)
{
    TEST("score_is_exact");
    int checked = 0, bad = 0;
    for (int depth = 2; depth <= 5; depth++) {
        for (uint64_t seed = 1; seed <= (depth == 5 ? 1u : depth == 4 ? 2u : 12u); seed++) {
            UtbGame g; utb_init(&g, depth);
            uint64_t rs = seed * 0x9e3779b97f4a7c15ull + (uint64_t)depth;
            while (!g.over && (depth < 5 || g.n_plies < 1000)) {
                int n = utb_legal(&g, legal, UTB_LEAVES_MAX);
                const int mover = g.turn;
                const int before = utb_bot_eval(&g, mover);
                for (int i = 0; i < n && i < 24; i++) {
                    int mv = legal[n <= 24 ? i : (int)(xs(&rs) % (uint64_t)n)];
                    int got = utb_bot_score(&g, mv);
                    UtbGame h = g;
                    utb_play(&h, mv);
                    if (h.over) continue;
                    checked++;
                    int want = utb_bot_eval(&h, mover) - before;
                    if (got != want && bad++ < 5)
                        CHECK(0, "depth %d seed %d ply %d move %d: scored %d, the board says %d",
                              depth, (int)seed, g.n_plies, mv, got, want);
                }
                utb_play(&g, legal[xs(&rs) % (uint64_t)n]);
            }
        }
    }
    printf("score_is_exact: %d moves held to the whole board, %d wrong\n", checked, bad);
    CHECK(bad == 0 && checked > 100000, "%d of %d moves scored wrong", bad, checked);
}

/* ONE PLY PAST THE HORIZON. X to move in (0,4,0), every move quiet. Its cell
 * 3 sends O to (4,0,3), where O's cell 2 wins that 3 x 3 - and sends X to
 * (0,3,2), whose cell 2 wins the 3 x 3 and the 9 x 9 (0,3). At two plies,
 * without the extension, cell 3 costs the 3 x 3 O wins; with it, O's win is
 * a grid decided on the last ply, so X's answer is seen, O declines it, and
 * cell 3 costs nothing - what a whole third ply sees. */
static void extension_sees_past(void)
{
    TEST("extension_sees_past");
    memset(cells, 0, sizeof cells);
    cells[leaf4(4, 0, 3, 0)] = cells[leaf4(4, 0, 3, 1)] = UTTT_O;
    won3(0, 3, 0, UTTT_X);
    won3(0, 3, 1, UTTT_X);
    cells[leaf4(0, 3, 2, 0)] = cells[leaf4(0, 3, 2, 1)] = UTTT_X;
    int last = leaf4(8, 0, 4, 0);                   /* names (0,4,0) */
    cells[last] = UTTT_O;
    UtbGame g;
    CHECK(build(&g, last), "the position adopts");
    CHECK(g.turn == UTTT_X && utb_region(&g) == utb_node_id(&g, 3, leaf4(0, 4, 0, 0) / 9), "X to move in (0,4,0)");
    const int a = leaf4(0, 4, 0, 3);
    UtbGame h = g;
    utb_play(&h, a);
    utb_play(&h, leaf4(4, 0, 3, 2));
    CHECK(utb_bot_gain(&h, leaf4(0, 3, 2, 2)) == UTB_BOT_W3 + UTB_BOT_W9, "X's answer wins the 9 x 9");

    UtbBot b; utb_bot_init(&b, 7); material_only(&b);
    CHECK(utb_bot_value(&b, &g, a, 2) == -UTB_BOT_W3, "two plies, no extension: %d", utb_bot_value(&b, &g, a, 2));
    b.extend = 1;
    CHECK(utb_bot_value(&b, &g, a, 2) == 0, "two plies and the extension: %d", utb_bot_value(&b, &g, a, 2));
    b.extend = 0;
    CHECK(utb_bot_value(&b, &g, a, 3) == 0, "three plies: %d", utb_bot_value(&b, &g, a, 3));
}

/* THE CAP SCALES: wider with the budget, narrower with the plies, inside
 * its bounds, and the default is the measured one. */
static void caps_scale(void)
{
    TEST("caps_scale");
    for (int p = 1; p <= UTB_BOT_PLIES_MAX; p++)
        for (int bud = 1000; bud <= UTB_BOT_BUDGET_MAX; bud *= 2) {
            int c = utb_bot_cap_for(p, bud);
            CHECK(c >= UTB_BOT_CAP_NODE_MIN && c <= UTB_BOT_CAP_NODE_MAX, "plies %d budget %d: cap %d", p, bud, c);
            CHECK(c <= utb_bot_cap_for(p, bud * 2), "plies %d: a bigger budget narrowed the cap", p);
            if (p > 1) CHECK(c <= utb_bot_cap_for(p - 1, bud), "budget %d: more plies widened the cap", bud);
        }
    CHECK(utb_bot_cap_for(8, 40000) < utb_bot_cap_for(4, 40000), "8 plies search narrower than 4");
    CHECK(utb_bot_cap_for(4, 4000) == 12, "the old setting keeps its cap of 12: %d", utb_bot_cap_for(4, 4000));
    UtbBot b; utb_bot_init(&b, 1);
    utb_bot_set(&b, 99, -5);
    CHECK(b.plies == UTB_BOT_PLIES_MAX && b.budget == 0, "clamped: %d plies, budget %d", b.plies, b.budget);
    utb_bot_set(&b, 3, 1 << 30);
    CHECK(b.plies == 3 && b.budget == UTB_BOT_BUDGET_MAX && b.cap_root >= b.cap_node, "clamped high");
}

/* ------------------------------------------------------------- games */


static int random_move(UtbGame *g, uint64_t *rs)
{
    int n = utb_legal(g, legal, UTB_LEAVES_MAX);
    return n ? legal[xs(rs) % (uint64_t)n] : -1;
}

/* `bot_side` is the seat the bot plays (0: both seats bots). Returns the
 * result; moves into out when given. */
static int play(int depth, uint64_t seed, int bot_side, int plies, int32_t *out, int *n_out)
{
    UtbGame g; utb_init(&g, depth);
    UtbBot bx, bo; utb_bot_seat(&bx, seed, UTTT_X); utb_bot_seat(&bo, seed, UTTT_O);
    bx.plies = bo.plies = plies;
    uint64_t rs = seed * 2654435761u + 7;
    int n = 0;
    while (!g.over) {
        int mv;
        if (bot_side && g.turn != bot_side) mv = random_move(&g, &rs);
        else mv = utb_bot_move(g.turn == UTTT_X ? &bx : &bo, &g);
        if (!utb_play(&g, mv)) { CHECK(0, "illegal move %d at ply %d", mv, g.n_plies); return -1; }
        if (out) out[n] = mv;
        n++;
    }
    if (n_out) *n_out = n;
    return g.over;
}

static int32_t m1[UTB_LEAVES_MAX], m2[UTB_LEAVES_MAX];

static void deterministic(void)
{
    TEST("deterministic");
    for (int depth = 2; depth <= 3; depth++)
        for (uint64_t s = 1; s <= 4; s++) {
            int n1, n2;
            int r1 = play(depth, s * 0x1234567ull, 0, 3, m1, &n1);
            int r2 = play(depth, s * 0x1234567ull, 0, 3, m2, &n2);
            CHECK(r1 == r2 && n1 == n2 && !memcmp(m1, m2, (size_t)n1 * sizeof m1[0]),
                  "depth %d seed %d: the replay diverged", depth, (int)s);
            int n3;
            play(depth, s * 0x1234567ull + 1, 0, 3, m2, &n3);
            CHECK(n1 != n3 || memcmp(m1, m2, (size_t)n1 * sizeof m1[0]),
                  "depth %d seed %d: the next seed played the same game", depth, (int)s);
        }
    /* and the two seats' streams differ: from one seed, X's first choice is
     * not tied to O's */
    UtbBot x, o; utb_bot_seat(&x, 42, UTTT_X); utb_bot_seat(&o, 42, UTTT_O);
    CHECK(x.rng != o.rng, "one seed, two streams");
}

static void beats_random(int depth, int games, int plies)
{
    TEST(depth == 2 ? "beats_random_d2" : "beats_random_d3");
    int win = 0, loss = 0, draw = 0;
    for (int i = 0; i < games; i++) {
        int side = (i & 1) ? UTTT_O : UTTT_X;
        int r = play(depth, 9000 + (uint64_t)i, side, plies, NULL, NULL);
        if (r == side) win++;
        else if (r == UTTT_DRAW) draw++;
        else loss++;
    }
    printf("depth %d, %d plies vs random: %d won, %d drawn, %d lost of %d\n", depth, plies, win, draw, loss, games);
    /* A coin's wins over the decisive games are binomial(n, 1/2): four
     * standard deviations above n/2 is a chance of about 3 in 100,000. */
    int dec = win + loss;
    CHECK(dec > 0 && 2.0 * win - dec > 4.0 * sqrt((double)dec), "won %d of %d decisive", win, dec);
}

/* LOOKING FURTHER WINS: a bot at `deep` plies against one at `shallow`,
 * both at `budget` and both with threats and the extension, seats
 * alternated, seeds 31000 on. Four standard deviations, as above. */
static void deeper_wins(int depth, int games, int deep, int shallow, int budget)
{
    TEST("deeper_wins");
    int win = 0, loss = 0, draw = 0;
    long plies = 0;
    clock_t t0 = clock();
    for (int i = 0; i < games; i++) {
        const int side = (i & 1) ? UTTT_O : UTTT_X;
        const uint64_t seed = 31000 + (uint64_t)i;
        UtbGame g; utb_init(&g, depth);
        UtbBot b[2];
        for (int s = UTTT_X; s <= UTTT_O; s++) {
            utb_bot_seat(&b[s - 1], seed, s);
            utb_bot_set(&b[s - 1], s == side ? deep : shallow, budget);
        }
        while (!g.over) {
            int mv = utb_bot_move(&b[g.turn - 1], &g);
            if (!utb_play(&g, mv)) { CHECK(0, "illegal move %d", mv); return; }
        }
        plies += g.n_plies;
        if (g.over == side) win++;
        else if (g.over == UTTT_DRAW) draw++;
        else loss++;
    }
    const int dec = win + loss;
    printf("depth %d, %d plies vs %d (budget %d): %d won, %d drawn, %d lost of %d, z %.2f, %.0f plies a game, %.1f s\n",
           depth, deep, shallow, budget, win, draw, loss, games, dec ? (2.0 * win - dec) / sqrt((double)dec) : 0.0,
           (double)plies / games, (double)(clock() - t0) / CLOCKS_PER_SEC);
    CHECK(dec > 0 && 2.0 * win - dec > 4.0 * sqrt((double)dec), "won %d of %d decisive", win, dec);
}

/* THE PAGE'S GAME: two bots on the depth-5 board. A whole game at the first
 * page's setting (4 plies, budget 4,000), and the first `stretch` plies at
 * the page's defaults: every move legal, every move within its budget plus
 * the one ply that always finishes (a region scan of at most 6,561 blocks,
 * the root's candidates, their gains when winners overflow the cap, and the
 * threats re-read under a grid one of them decides: at most 820 nodes, 92
 * units), and the speed printed. */
static void full_243(uint64_t seed, int plies, int budget, int stretch)
{
    TEST("full_243");
    UtbGame g; utb_init(&g, 5);
    UtbBot bx, bo; utb_bot_seat(&bx, seed, UTTT_X); utb_bot_seat(&bo, seed, UTTT_O);
    utb_bot_set(&bx, plies, budget);
    utb_bot_set(&bo, plies, budget);
    const int first_ply = 6561 + 2 * UTB_LEAVES_MAX / 9 + bx.cap_root * (1 + 92);
    int max_work = 0, deep = 0;
    long work = 0;
    clock_t t0 = clock();
    while (!g.over && (!stretch || g.n_plies < stretch)) {
        UtbBot *b = g.turn == UTTT_X ? &bx : &bo;
        int mv = utb_bot_move(b, &g);
        if (!utb_play(&g, mv)) { CHECK(0, "illegal move %d at ply %d", mv, g.n_plies); break; }
        if (b->work > max_work) max_work = b->work;
        work += b->work;
        deep += b->depth == b->plies;
        if (g.n_plies > UTB_LEAVES_MAX) { CHECK(0, "the game never ended"); break; }
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("depth 5, seed %llu, %d plies, budget %d, caps %d/%d: %s after %d plies in %.2f s, %.0f moves/s; "
           "work %.0f a move, at most %d; %.0f%% of moves searched all %d plies; "
           "material X %d O %d\n",
           (unsigned long long)seed, plies, budget, bx.cap_node, bx.cap_root,
           g.over == UTTT_DRAW ? "drawn" : g.over == UTTT_X ? "X won" : g.over == UTTT_O ? "O won" : "stopped",
           g.n_plies, s, g.n_plies / (s > 0 ? s : 1e-9), (double)work / g.n_plies, max_work,
           100.0 * deep / g.n_plies, plies,
           utb_bot_material(&g, UTTT_X), utb_bot_material(&g, UTTT_O));
    CHECK(stretch ? g.n_plies == stretch : g.over != 0, "the game ended, or ran its stretch");
    CHECK(max_work <= budget + first_ply, "a move spent %d work units", max_work);
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 40;
    weights_rank();
    hands_over_27();
    tie_break();
    draw_is_nothing();
    threats_rank();
    score_is_exact();
    extension_sees_past();
    caps_scale();
    deterministic();
    /* at least 30 games: fewer cannot clear four standard deviations even
     * winning every one (ten straight wins is 3.2) */
    beats_random(2, games < 30 ? 30 : games, 2);
    beats_random(3, games < 30 ? 30 : games, 3);
    /* 6 plies against 4 on the 9 x 9 (depth 2), where a game is 50 plies:
     * 120 games at the least: 60 cleared only 3.8 standard deviations */
    deeper_wins(2, games < 40 ? 120 : games * 3, 6, 4, UTB_BOT_BUDGET);
    full_243(20261003, 4, 4000, 0);
    full_243(20261003, UTB_BOT_PLIES, UTB_BOT_BUDGET, 1500);
    return report("uttt_big_bot_test");
}

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
        UtbBot b; utb_bot_init(&b, 1);
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
    UtbBot b; utb_bot_init(&b, 1);
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
    UtbBot b; utb_bot_init(&b, 3);
    CHECK(utb_bot_value(&b, &g, gift, 1) == UTB_BOT_W3, "one ply: the gift wins its 3 x 3");
    CHECK(utb_bot_value(&b, &g, gift, 2) == UTB_BOT_W3 - big,
          "two plies: 1 - 91, not %d", utb_bot_value(&b, &g, gift, 2));
    for (int k = 3; k < 9; k++)
        CHECK(utb_bot_value(&b, &g, leaf4(0, 4, 0, k), 2) == 0, "a quiet move %d is worth nothing", k);
    const int setup = leaf4(0, 4, 0, 3);
    CHECK(utb_bot_value(&b, &g, setup, 3) == UTB_BOT_W3, "three plies: the set-up is worth 1, not %d", utb_bot_value(&b, &g, setup, 3));
    CHECK(utb_bot_value(&b, &g, leaf4(0, 4, 0, 4), 3) == 0, "three plies: another quiet move is worth nothing");
    CHECK(utb_bot_value(&b, &g, gift, 3) == UTB_BOT_W3 - big, "three plies: the gift is still 1 - 91");

    UtbGame h = g;
    utb_play(&h, gift);
    CHECK(utb_bot_gain(&h, leaf4(4, 0, 2, 2)) == big, "O's reply wins %d", utb_bot_gain(&h, leaf4(4, 0, 2, 2)));

    for (int s = 0; s < 8; s++) {
        UtbBot one; utb_bot_init(&one, (uint64_t)s); one.plies = 1;
        CHECK(utb_bot_move(&one, &g) == gift, "seed %d: one ply takes the 3 x 3", s);
        for (int plies = 2; plies <= 4; plies++) {
            UtbBot two; utb_bot_init(&two, (uint64_t)s); two.plies = plies;
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

/* ------------------------------------------------------------- games */

static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static int32_t legal[UTB_LEAVES_MAX];

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

/* THE PAGE'S GAME: two bots at the defaults, a whole depth-5 game. Every
 * move legal, every move within its budget plus the one ply that always
 * finishes (a region scan of at most 6,561 blocks, the root's candidates,
 * and their gains when winners overflow the cap), and the speed printed. */
static void full_243(uint64_t seed)
{
    TEST("full_243");
    UtbGame g; utb_init(&g, 5);
    UtbBot bx, bo; utb_bot_seat(&bx, seed, UTTT_X); utb_bot_seat(&bo, seed, UTTT_O);
    const int first_ply = 6561 + 2 * UTB_LEAVES_MAX / 9 + UTB_BOT_CAP_ROOT;
    int max_work = 0, deep = 0;
    long work = 0;
    clock_t t0 = clock();
    while (!g.over) {
        UtbBot *b = g.turn == UTTT_X ? &bx : &bo;
        int mv = utb_bot_move(b, &g);
        if (!utb_play(&g, mv)) { CHECK(0, "illegal move %d at ply %d", mv, g.n_plies); break; }
        if (b->work > max_work) max_work = b->work;
        work += b->work;
        deep += b->depth == b->plies;
        if (g.n_plies > UTB_LEAVES_MAX) { CHECK(0, "the game never ended"); break; }
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("depth 5, seed %llu: %s after %d plies in %.2f s, %.0f moves/s; "
           "work %.0f a move, at most %d; %.0f%% of moves searched all %d plies; "
           "material X %d O %d\n",
           (unsigned long long)seed, g.over == UTTT_DRAW ? "drawn" : g.over == UTTT_X ? "X won" : "O won",
           g.n_plies, s, g.n_plies / (s > 0 ? s : 1e-9), (double)work / g.n_plies, max_work,
           100.0 * deep / g.n_plies, UTB_BOT_PLIES,
           utb_bot_material(&g, UTTT_X), utb_bot_material(&g, UTTT_O));
    CHECK(g.over, "the game ended");
    CHECK(max_work <= UTB_BOT_BUDGET + first_ply, "a move spent %d work units", max_work);
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 40;
    weights_rank();
    hands_over_27();
    tie_break();
    draw_is_nothing();
    deterministic();
    beats_random(2, games, 2);
    beats_random(3, games, 3);
    full_243(20261003);
    return report("uttt_big_bot_test");
}

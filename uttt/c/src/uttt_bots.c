#include "uttt_bots.h"
#include <string.h>

const char *UTTT_BOT_NAME[BOT_COUNT] =
    { "random", "biro", "roller", "crn", "bias", "nib", "sniper" };

static uint32_t rnd(uint64_t *s, uint32_t n)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x;
    return (uint32_t)(((x * 2685821657736338717ull) >> 33) % n);
}

/* ---------------------------------------------------------------- heuristic
 * Scores a MOVE, not a position. Cheap on purpose: it is also the playout
 * policy for the strongest bot, where it runs millions of times. */

static const int CELL_W[9]  = { 3, 2, 3,  2, 4, 2,  3, 2, 3 };
static const int BLOCK_W[9] = { 3, 2, 3,  2, 4, 2,  3, 2, 3 };

static int wins_block(const UtttGame *g, uint8_t mv, uint8_t mark)
{
    uint8_t nine[9];
    memcpy(nine, &g->cell[(mv / 9) * 9], 9);
    nine[mv % 9] = mark;
    return uttt_line(nine, mark);
}

static int meta_would_win(const UtttGame *g, uint8_t mv, uint8_t mark)
{
    if (!wins_block(g, mv, mark)) return 0;
    uint8_t nine[9];
    memcpy(nine, g->block, 9);
    nine[mv / 9] = mark;
    return uttt_line(nine, mark);
}

static int score_move(const UtttGame *g, uint8_t mv)
{
    const uint8_t me  = g->turn;
    const uint8_t opp = (uint8_t)(me == UTTT_X ? UTTT_O : UTTT_X);
    int b = mv / 9, c = mv % 9, s = 0;

    if (meta_would_win(g, mv, me)) return 1 << 20;     /* it ends the game */
    if (wins_block(g, mv, me))     s += 120;
    if (wins_block(g, mv, opp))    s += 70;            /* deny it */

    s += CELL_W[c] * 2 + BLOCK_W[b];

    /* WHERE IT SENDS THEM is most of the game. A decided target hands them a
     * free choice over the whole sheet, which is the worst thing you can give
     * anybody in this game - and it is exactly what the encoder measured as
     * expensive. */
    if (g->block[c] != UTTT_OPEN) {
        s -= 55;
    } else {
        /* do not send them somewhere they can close */
        for (int k = 0; k < 9; k++) {
            uint8_t t = (uint8_t)(c * 9 + k);
            if (g->cell[t] != UTTT_OPEN) continue;
            if (meta_would_win(g, t, opp)) { s -= 400; break; }
            if (wins_block(g, t, opp))     { s -= 45; }
        }
    }
    return s;
}

/* ------------------------------------------------------------------ playout
 * `biased` picks the best-scoring move most of the time and a random one
 * otherwise, which keeps the sample honest while steering it somewhere
 * plausible. A purely greedy playout is deterministic and therefore not a
 * sample at all. */
static uint8_t playout(UtttGame *g, uint64_t *rs, int biased, int *plies)
{
    uint8_t list[81];
    for (;;) {
        int n = uttt_legal(g, list);
        if (n <= 0) break;
        uint8_t mv;
        if (!biased || rnd(rs, 4) == 0) {
            mv = list[rnd(rs, (uint32_t)n)];
        } else {
            int best = -(1 << 30), nb = 0; uint8_t bl[81];
            for (int i = 0; i < n; i++) {
                int sc = score_move(g, list[i]);
                if (sc > best) { best = sc; nb = 0; }
                if (sc == best) bl[nb++] = list[i];
            }
            mv = bl[rnd(rs, (uint32_t)nb)];
        }
        uttt_play(g, mv);
    }
    if (plies) *plies = g->n_plies;
    return g->over;
}

/* ------------------------------------------------------------ exact endgame
 * Below a few empty cells the tree is small enough to prove. Returns +1 / 0 /
 * -1 for the side to move at the root of this call. */
static int solve(UtttGame *g, int depth_left)
{
    if (g->over)
        return g->over == UTTT_DRAW ? 0 : (g->over == g->turn ? 1 : -1);
    if (depth_left <= 0) return 2;                 /* unknown */

    uint8_t list[81];
    int n = uttt_legal(g, list);
    int best = -2;
    for (int i = 0; i < n; i++) {
        UtttGame t = *g;
        uttt_play(&t, list[i]);
        int v = solve(&t, depth_left - 1);
        if (v == 2) return 2;                      /* cannot prove the branch */
        v = -v;                                    /* it was the other side's */
        if (v > best) best = v;
        if (best == 1) break;
    }
    return best;
}

static int empties(const UtttGame *g)
{
    int e = 0;
    for (int b = 0; b < 9; b++)
        if (g->block[b] == UTTT_OPEN)
            for (int c = 0; c < 9; c++)
                if (g->cell[b * 9 + c] == UTTT_OPEN) e++;
    return e;
}

/* ------------------------------------------------------------ shallowest mate
 * A FORCED WIN, PROVED, and the shortest one there is.
 *
 * This is what "win in as few moves as possible" actually needs, and it is
 * not a weight on a rollout - the negative result in uttt_bots.h is the
 * afternoon that established as much. A rollout weight can only re-rank
 * candidates that a search already thinks are equal; winning sooner means
 * PROVING a line the opponent cannot escape and preferring the shortest
 * proof.
 *
 * AN AND/OR SEARCH, NOT A MINIMAX. Our nodes are OR - one move that forces a
 * win is enough. Theirs are AND - every reply has to still lose, and one
 * escape kills the line. There is no evaluation function anywhere in it,
 * which is the point: it returns a number of plies or it returns nothing,
 * and a "nothing" is honest rather than a guess dressed as a score.
 *
 * A draw is a failure here. A line that forces a draw is not a win, and the
 * bot that owns this search falls back to Monte Carlo the moment the proof
 * runs out - so it never trades a win for a short game, it just takes the
 * short one when it can see the whole thing.
 *
 * ITERATIVE DEEPENING IS WHAT MAKES IT SHALLOWEST. Odd depths only, because
 * a forced win always ends on our move: the first depth that answers is the
 * shortest mate, so there is nothing to compare afterwards.
 *
 * The node budget is the only thing stopping it. Depth grows as b^d with b
 * around six, so an unbounded search on an open board does not return; the
 * budget turns "no mate" and "no time" into the same answer, which for a bot
 * choosing a move is the same answer anyway. */
#define MATE_NONE 9999

static int mate_in(UtttGame *g, int depth, long *nodes);

/* One OR node. `out` receives the move when a mate is found. */
static int mate_root(UtttGame *g, int depth, long *nodes, uint8_t *out)
{
    if (depth <= 0 || (*nodes -= 1) <= 0) return MATE_NONE;

    uint8_t list[81];
    int n = uttt_legal(g, list);
    if (n <= 0) return MATE_NONE;
    const uint8_t me = g->turn;
    int best = MATE_NONE;

    for (int i = 0; i < n; i++) {
        UtttGame t = *g;
        uttt_play(&t, list[i]);
        if (t.over) {
            /* Over on our own move: a win is mate in one, a draw is not a
             * win, and we cannot lose by moving. */
            if (t.over == me && best > 1) { best = 1; if (out) *out = list[i]; }
            continue;
        }
        /* EVERY reply has to still lose. One escape and the line is not a
         * proof, which is the whole difference between this and a search
         * that averages. */
        uint8_t rl[81];
        int m = uttt_legal(&t, rl);
        int worst = 0;
        for (int j = 0; j < m; j++) {
            UtttGame u = t;
            uttt_play(&u, rl[j]);
            if (u.over) { worst = MATE_NONE; break; }   /* they won, or drew */
            /* No line longer than the best already found is worth proving. */
            int cap = (best < MATE_NONE ? best - 2 : depth) - 2;
            int v = mate_in(&u, cap < depth - 2 ? cap : depth - 2, nodes);
            if (v >= MATE_NONE) { worst = MATE_NONE; break; }
            if (v > worst) worst = v;
        }
        if (worst < MATE_NONE && 2 + worst < best) {
            best = 2 + worst;
            if (out) *out = list[i];
        }
    }
    return best;
}

static int mate_in(UtttGame *g, int depth, long *nodes)
{
    return mate_root(g, depth, nodes, NULL);
}

/* COULD A MATE IN `depth` EVEN EXIST? How many blocks the side to move
 * still needs on their best line, against how many moves they get.
 *
 * A win ends on three blocks in a line, so a line with anything of the
 * opponent's in it - or anything drawn - is dead and every line being dead
 * means no forced win at any depth. On a live line they must take every
 * block they do not already hold, and each one costs at least one of their
 * moves; `depth` plies give them (depth + 1) / 2. If no line is within
 * reach, the answer is no and it took nine comparisons to say so.
 *
 * WHY THIS EXISTS AT ALL. Measured at 160,000 nodes, per move, ungated:
 *
 *     plies into the game     0      10     20     30     40     50
 *     cost                  422ms  439ms  279ms  219ms  131ms  0.19ms
 *     mate found             0/20   0/20   0/20   0/20   0/20   20/20
 *
 * Four hundred milliseconds to prove there was nothing to find, on every
 * move, for the whole opening.
 *
 * MY FIRST GATE WANTED TWO BLOCKS OF A LINE ALREADY OWNED and it was wrong:
 * a mate in three can start from ONE, by taking two blocks that are both a
 * cell from falling. It rejected real wins. The depth is what makes it
 * sound - the same question, asked of the search's own horizon. */
static int line_in_reach(const UtttGame *g, int depth)
{
    static const uint8_t L[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6} };
    const uint8_t me = g->turn;
    const int moves = (depth + 1) / 2;
    for (int i = 0; i < 8; i++) {
        int need = 0, dead = 0;
        for (int j = 0; j < 3; j++) {
            uint8_t b = g->block[L[i][j]];
            if (b == me) continue;
            if (b == UTTT_OPEN) need++;
            else { dead = 1; break; }
        }
        if (!dead && need <= moves) return 1;
    }
    return 0;
}

/* The shortest forced win, or 0 if there is none inside the budget. */
int uttt_mate_in(const UtttGame *g, long nodes, uint8_t *out)
{
    for (int d = 1; d <= 13; d += 2) {
        if (!line_in_reach(g, d)) continue;
        UtttGame t = *g;
        long left = nodes;
        uint8_t mv = 0;
        int v = mate_root(&t, d, &left, &mv);
        nodes -= (nodes - left);
        if (v < MATE_NONE) { *out = mv; return v; }
        if (nodes <= 0) break;
    }
    return 0;
}

/* ---------------------------------------------------------------------- mc */
/* `speed`: look for a proved forced win FIRST and take the shortest one.
 *
 * Only when a whole line can be seen to the end. The moment the proof runs
 * out this is nib exactly, so a short game is never bought with a win. */
static uint8_t mc_move(const UtttGame *g, int budget, uint64_t *rs,
                       int crn, int biased, int endgame, int speed)
{
    uint8_t list[81];
    int n = uttt_legal(g, list);
    if (n <= 0) return 0;
    if (n == 1) return list[0];

    /* THE PROOF COMES FIRST. A line that ends the game is worth more than
     * any number of rollouts that only suggest it might. */
    if (speed) {
        /* A FLAT BUDGET, not one scaled off the rollout count: what a proof
         * costs has nothing to do with how many playouts somebody asked for.
         *
         * Two thousand nodes, measured. Worst case about three milliseconds
         * a move through the middlegame and a fifth of one in the endgame,
         * where it finds every mate a budget twenty thousand times larger
         * finds. Above this the money goes entirely on proving ABSENCE
         * deeper, which is worth nothing to a bot that has a Monte Carlo
         * search to fall back on - at 160,000 the same search cost 420ms a
         * move through the whole opening and returned nothing every time. */
        uint8_t mv = 0;
        if (uttt_mate_in(g, 2000L, &mv)) return mv;
    }

    if (endgame && empties(g) <= 11) {
        int bestv = -2; uint8_t bestm = list[0];
        for (int i = 0; i < n; i++) {
            UtttGame t = *g;
            uttt_play(&t, list[i]);
            int v = solve(&t, 12);
            if (v == 2) { bestv = -2; break; }      /* fall through to MC */
            v = -v;
            if (v > bestv) { bestv = v; bestm = list[i]; }
        }
        if (bestv > -2) return bestm;
    }

    const uint8_t me = g->turn;
    int score[81]; memset(score, 0, sizeof score);

    for (int r = 0; r < budget; r++) {
        /* COMMON RANDOM NUMBERS: every candidate is judged on the same
         * stream, so a difference between two of them is the move and not
         * the dice. Without this the ranking at small budgets is mostly
         * noise. */
        uint64_t base = *rs + (uint64_t)r * 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < n; i++) {
            uint64_t s = crn ? base : (*rs += 0x9E3779B97F4A7C15ull);
            UtttGame t = *g;
            uttt_play(&t, list[i]);
            uint8_t w = playout(&t, &s, biased, NULL);
            score[i] += (w == me) ? 2 : (w == UTTT_DRAW ? 1 : 0);
        }
    }
    *rs += 0x9E3779B97F4A7C15ull;

    /* TIE-BREAKS ARE A DECISION, not whatever the loop order gives. On equal
     * rollout score we take the move the heuristic prefers - which is
     * cordite's lesson, arrived at there by inverting one and finding it was
     * worth more than the search that fed it. */
    int best = -1, bh = 0; uint8_t bm = list[0];
    for (int i = 0; i < n; i++) {
        int h = score_move(g, list[i]);
        if (score[i] > best || (score[i] == best && h > bh)) {
            best = score[i]; bh = h; bm = list[i];
        }
    }
    return bm;
}

uint8_t uttt_bot_move(UtttBot bot, const UtttGame *g, int budget, uint64_t *rs)
{
    uint8_t list[81];
    int n = uttt_legal(g, list);
    if (n <= 0) return 0;

    switch (bot) {
    case BOT_RANDOM:
        return list[rnd(rs, (uint32_t)n)];
    case BOT_BIRO: {
        int best = -(1 << 30), nb = 0; uint8_t bl[81];
        for (int i = 0; i < n; i++) {
            int sc = score_move(g, list[i]);
            if (sc > best) { best = sc; nb = 0; }
            if (sc == best) bl[nb++] = list[i];
        }
        return bl[rnd(rs, (uint32_t)nb)];
    }
    case BOT_ROLLER: return mc_move(g, budget, rs, 0, 0, 0, 0);
    case BOT_CRN:    return mc_move(g, budget, rs, 1, 0, 0, 0);
    case BOT_BIAS:   return mc_move(g, budget, rs, 0, 1, 0, 0);
    case BOT_NIB:    return mc_move(g, budget, rs, 1, 1, 1, 0);
    case BOT_SNIPER: return mc_move(g, budget, rs, 1, 1, 1, 1);
    default:         return list[0];
    }
}

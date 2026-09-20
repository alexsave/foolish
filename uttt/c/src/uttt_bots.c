#include "uttt_bots.h"
#include <string.h>

const char *UTTT_BOT_NAME[BOT_COUNT] =
    { "random", "biro", "roller", "crn", "bias", "nib" };

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
static uint8_t playout(UtttGame *g, uint64_t *rs, int biased)
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

/* ---------------------------------------------------------------------- mc */
static uint8_t mc_move(const UtttGame *g, int budget, uint64_t *rs,
                       int crn, int biased, int endgame)
{
    uint8_t list[81];
    int n = uttt_legal(g, list);
    if (n <= 0) return 0;
    if (n == 1) return list[0];

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
            uint8_t w = playout(&t, &s, biased);
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
    case BOT_ROLLER: return mc_move(g, budget, rs, 0, 0, 0);
    case BOT_CRN:    return mc_move(g, budget, rs, 1, 0, 0);
    case BOT_BIAS:   return mc_move(g, budget, rs, 0, 1, 0);
    case BOT_NIB:    return mc_move(g, budget, rs, 1, 1, 1);
    default:         return list[0];
    }
}

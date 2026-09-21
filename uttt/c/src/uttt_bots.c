#include "uttt_bots.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

const char *UTTT_BOT_NAME[BOT_COUNT] =
    { "random", "biro", "roller", "crn", "bias", "nib", "sniper", "quill" };

static uint32_t rnd(uint64_t *s, uint32_t n)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x;
    uint32_t v = (uint32_t)((x * 2685821657736338717ull) >> 33);
    /* A POWER OF TWO IS A MASK, and the commonest call here asks for four.
     * `% n` with a runtime n is a hardware divide - the only one left in
     * this binary - and it fired twice per playout step. */
    if ((n & (n - 1)) == 0) return v & (n - 1);
    return v % n;
}

/* ---------------------------------------------------------------- heuristic
 * Scores a MOVE, not a position. Cheap on purpose: it is also the playout
 * policy for the strongest bot, where it runs millions of times. */

static UtttWeights W = {
    .win_block = 120, .deny_block = 70, .decided_target = -55,
    .closer = -25, .meta_gift = -400, .bias_one_in = 4, .leaf_cutoff = 12,
    .cell_w  = { 3, 2, 3,  2, 4, 2,  3, 2, 3 },
    .block_w = { 3, 2, 3,  2, 4, 2,  3, 2, 3 },
};

UtttWeights uttt_weights_default(void)
{
    UtttWeights d = {
        .win_block = 120, .deny_block = 70, .decided_target = -55,
        .closer = -25, .meta_gift = -400, .bias_one_in = 4, .leaf_cutoff = 12,
        .cell_w  = { 3, 2, 3,  2, 4, 2,  3, 2, 3 },
        .block_w = { 3, 2, 3,  2, 4, 2,  3, 2, 3 },
    };
    return d;
}

void uttt_weights_set(const UtttWeights *w) { W = *w; }

/* BOTH OF THESE WERE A NINE-BYTE COPY AND A NINE-BYTE SCAN to ask one
 * question about one block. They are the playout policy of the strongest
 * bot, so they run millions of times a move; now they are an OR and a table
 * lookup against the bitboards the kernel already keeps. */
static int wins_block(const UtttGame *g, uint8_t mv, uint8_t mark)
{
    return uttt_mask_line(g->cm[mark - 1][mv / 9] | (1u << (mv % 9)));
}

static int meta_would_win(const UtttGame *g, uint8_t mv, uint8_t mark)
{
    if (!wins_block(g, mv, mark)) return 0;
    return uttt_mask_line(g->bm[mark - 1] | (1u << (mv / 9)));
}

static int score_move(const UtttGame *g, uint8_t mv)
{
    const uint8_t me  = g->turn;
    const uint8_t opp = (uint8_t)(me == UTTT_X ? UTTT_O : UTTT_X);
    int b = mv / 9, c = mv % 9, s = 0;

    if (meta_would_win(g, mv, me)) return 1 << 20;     /* it ends the game */
    if (wins_block(g, mv, me))     s += W.win_block;
    if (wins_block(g, mv, opp))    s += W.deny_block;  /* deny it */

    s += W.cell_w[c] * 2 + W.block_w[b];

    /* WHERE IT SENDS THEM is most of the game. A decided target hands them a
     * free choice over the whole sheet, which is the worst thing you can give
     * anybody in this game - and it is exactly what the encoder measured as
     * expensive. */
    if (!((g->live >> c) & 1u)) {
        s += W.decided_target;
    } else {
        /* DO NOT SEND THEM SOMEWHERE THEY CAN CLOSE - and this was a walk
         * over nine squares asking two questions about each, which made it
         * 35% of a bot's runtime.
         *
         * The squares that would close block c for them are one lookup, and
         * intersecting that with the squares still empty is one AND. The
         * count is what the loop was really after: one such square is worth
         * a penalty, and if taking the block would also complete their LINE
         * of blocks then it does not matter how many there are. */
        const unsigned open = ~(unsigned)(g->cm[0][c] | g->cm[1][c]) & 0x1ffu;
        const unsigned closers = uttt_mask_wins(g->cm[opp - 1][c]) & open;
        if (closers) {
            if (uttt_mask_line(g->bm[opp - 1] | (1u << c))) s += W.meta_gift;
            else s += W.closer * __builtin_popcount(closers);
        }
    }
    return s;
}

/* --------------------------------------------------------------- the leaf
 * WHAT A POSITION IS WORTH, WITHOUT PLAYING IT OUT.
 *
 * Every rollout here runs to the end of the game because nobody trusts a
 * UTTT evaluator - that is the note at the top of this file and it is why
 * the bots are Monte Carlo at all. But a rollout that runs forty plies to
 * learn one bit is expensive, and the standard next move in this family is
 * to stop early and ask a cheap question instead.
 *
 * This is that question, deliberately crude: the blocks are the game, so
 * count them, weighted by where they sit, and give partial credit for a
 * block that is nearly taken. Returns 0..200 from `me`'s side - 0 a loss,
 * 100 even, 200 a win - which is the scale a finished game already uses.
 *
 * It is NOT meant to be good. It is meant to be good enough that stopping a
 * playout early and spending the savings on more playouts comes out ahead,
 * and whether it does is a measurement, not an opinion. */
static int leaf_eval(const UtttGame *g, uint8_t me)
{
    if (g->over)
        return g->over == UTTT_DRAW ? 100 : (g->over == me ? 200 : 0);

    const int opp = (me == UTTT_X ? UTTT_O : UTTT_X) - 1;
    const int mine = me - 1;
    int s = 0;

    for (int b = 0; b < 9; b++) {
        int w = W.block_w[b];
        if ((g->bm[mine] >> b) & 1u)      s += 10 * w;
        else if ((g->bm[opp] >> b) & 1u)  s -= 10 * w;
        else if ((g->live >> b) & 1u) {
            /* a block nobody has yet: who is closer to taking it */
            unsigned open = uttt_open_cells(g, b);
            int mc = __builtin_popcount(uttt_mask_wins(g->cm[mine][b]) & open);
            int oc = __builtin_popcount(uttt_mask_wins(g->cm[opp][b]) & open);
            s += (mc - oc) * w;
        }
    }
    /* A line of blocks is the whole object, so weigh the near-lines too. */
    for (int i = 0; i < 8; i++) {
        unsigned line = uttt_line_mask(i);
        int mb = __builtin_popcount(g->bm[mine] & line);
        int ob = __builtin_popcount(g->bm[opp] & line);
        if (ob == 0) s += mb * mb * 6;
        if (mb == 0) s -= ob * ob * 6;
    }

    /* squash into 0..200 without a branch per side */
    int v = 100 + s;
    return v < 2 ? 2 : v > 198 ? 198 : v;
}

/* ------------------------------------------------------------------ playout
 * `biased` picks the best-scoring move most of the time and a random one
 * otherwise, which keeps the sample honest while steering it somewhere
 * plausible. A purely greedy playout is deterministic and therefore not a
 * sample at all. */
/* NO LIST. A playout picks one move per step and throws the rest away, so
 * the eighty-one byte buffer was eighty bytes of waste every step - and a
 * playout is where a Monte Carlo bot spends its life. Both branches walk the
 * kernel's masks straight.
 *
 * THE ORDER IS PART OF THE CONTRACT, not an implementation detail. Blocks
 * ascending, cells ascending, is exactly what `uttt_legal` would have
 * produced, so the k-th move is the same move and a tie is broken the same
 * way; and the random draws happen in the same places, so the same stream
 * gives the same game. Every bot's play is byte-identical across this
 * change, which is what `/tmp` fingerprints and the ladders confirmed. */
static int playout(UtttGame *g, uint64_t *rs, int biased, uint8_t me,
                   int cutoff)
{
    /* THE SCALE IS 0..200, not win/draw/loss, so a playout that STOPS EARLY
     * can report a shade instead of a verdict. With `cutoff` at zero this
     * still only ever returns 0, 100 or 200.
     *
     * THE CUT IS THE TREE'S, NOT THE FLAT SEARCH'S, which is why it arrives
     * as an argument rather than being read from the weights here. The flat
     * bots have one number a candidate and nothing under it: cut their
     * playouts short and they are guessing from a guess. The tree has the
     * rest of itself underneath, so a leaf only has to rank its siblings,
     * and it can buy far more of them with what it saves. Every flat bot
     * passes 0 and plays the game it played before, to the byte. */
    const int stop = cutoff ? g->n_plies + cutoff : 0;
    for (;;) {
        if (stop && g->n_plies >= stop) return leaf_eval(g, me);
        unsigned blocks = uttt_legal_blocks(g);
        if (!blocks) break;

        /* how many moves there are, and the open cells of each live block */
        unsigned openm[9];
        int n = 0;
        for (unsigned bb = blocks; bb; bb &= bb - 1) {
            int b = __builtin_ctz(bb);
            openm[b] = uttt_open_cells(g, b);
            n += __builtin_popcount(openm[b]);
        }
        if (n <= 0) break;

        uint8_t mv = 0;
        if (!biased || rnd(rs, (uint32_t)W.bias_one_in) == 0) {
            /* the k-th legal move, blocks then cells, both ascending */
            int k = (int)rnd(rs, (uint32_t)n);
            for (unsigned bb = blocks; bb; bb &= bb - 1) {
                int b = __builtin_ctz(bb);
                int cnt = __builtin_popcount(openm[b]);
                if (k >= cnt) { k -= cnt; continue; }
                unsigned o = openm[b];
                while (k--) o &= o - 1;
                mv = (uint8_t)(b * 9 + __builtin_ctz(o));
                break;
            }
        } else {
            int best = -(1 << 30), nb = 0; uint8_t bl[81];
            for (unsigned bb = blocks; bb; bb &= bb - 1) {
                int b = __builtin_ctz(bb);
                for (unsigned o = openm[b]; o; o &= o - 1) {
                    uint8_t m = (uint8_t)(b * 9 + __builtin_ctz(o));
                    int sc = score_move(g, m);
                    if (sc > best) { best = sc; nb = 0; }
                    if (sc == best) bl[nb++] = m;
                }
            }
            mv = bl[rnd(rs, (uint32_t)nb)];
        }
        uttt_play(g, mv);
    }
    return g->over == UTTT_DRAW ? 100 : (g->over == me ? 200 : 0);
}

/* ------------------------------------------------------------ exact endgame
 * Below a few empty cells the tree is small enough to prove. Returns +1 / 0 /
 * -1 for the side to move at the root of this call. */
/* A TRANSPOSITION TABLE, because this game transposes constantly. The same
 * position arrives by many orders of moves - the block you are sent to
 * depends only on the last square, so whole permutations of earlier play
 * converge - and without a table every one of them is proved again.
 *
 * A PROVED RESULT IS ABSOLUTE and can be kept forever: "this position is a
 * win for the side to move" does not depend on how much depth was left when
 * it was established. An UNKNOWN is the opposite - it only means "not proved
 * within THIS much depth" - so it records the depth it failed at and is
 * reused only for a search no deeper.
 *
 * Allocated on first use rather than declared, so a library that links these
 * bots and never calls them carries no table. */
#define TT_BITS 18
#define TT_SIZE (1u << TT_BITS)
/* ONLY PROVED RESULTS GO IN. A proof is absolute - "this position is a win
 * for the side to move" does not depend on how the search reached it or on
 * what was left to spend - so an entry never expires and needs no depth
 * beside it. An UNKNOWN is the opposite: it means only "not settled within
 * the nodes that were left", which is a fact about a budget rather than
 * about the position, and storing it would let one starved search silence
 * every later one. */
typedef struct { uint64_t key; int8_t val; } TtEntry;
static TtEntry *tt;

/* THE EIGHT SYMMETRIES. The board is a 3x3 of 3x3s, and the rule that sends
 * you to the block matching your square is itself symmetric, so any rotation
 * or reflection of the small grid applied to the OUTER grid and to every
 * INNER grid at once maps a legal game onto a legal game. Eight of them:
 * four rotations and four reflections. SYM9[s] permutes a whole 9-bit block
 * mask in one lookup, so a transform is table reads, not bit twiddling.
 *
 * WHERE THIS PAYS AND WHERE IT CANNOT. The obvious use is the solver's
 * table: hash the smallest of all eight transforms and an orbit of eight
 * positions shares one entry. Measured, it merged 2,720 of 4.4 million
 * positions - 0.06% - and cost 3.7x in hashing for it.
 *
 * That is not a tuning failure, it is structural. The mirror of a node
 * extends the mirror of the ROOT. From the empty board the mirror of the
 * root IS the root, so every orbit member is reachable and collapses; from
 * a specific endgame position it is a different root, so no other orbit
 * member is ever reached and there is nothing to merge. Enumerated, raw
 * positions against canonical ones:
 *
 *     from the empty board       ply 1  5.40x   ply 4  7.92x
 *     from a real 55-ply root    ply 1  1.00x   ply 6  1.00x
 *
 * Exactly 1.00x, at every depth - the signature of impossible rather than
 * rare. So symmetry belongs at the OPENING, where the root is its own
 * mirror, and that is what `root_dedupe` below uses it for. */
static const uint8_t SYM[8][9] = {
    {0,1,2,3,4,5,6,7,8},   /* identity        */
    {6,3,0,7,4,1,8,5,2},   /* rotate 90       */
    {8,7,6,5,4,3,2,1,0},   /* rotate 180      */
    {2,5,8,1,4,7,0,3,6},   /* rotate 270      */
    {2,1,0,5,4,3,8,7,6},   /* flip horizontal */
    {6,7,8,3,4,5,0,1,2},   /* flip vertical   */
    {0,3,6,1,4,7,2,5,8},   /* transpose       */
    {8,5,2,7,4,1,6,3,0},   /* anti-transpose  */
};
static uint16_t SYM9[8][512];
static int sym_ready;

static void build_sym(void)
{
    for (int s = 0; s < 8; s++)
        for (int m = 0; m < 512; m++) {
            uint16_t o = 0;
            for (int i = 0; i < 9; i++)
                if (m & (1 << i)) o |= (uint16_t)(1 << SYM[s][i]);
            SYM9[s][m] = o;
        }
    sym_ready = 1;
}

/* Does transform `s` map this position onto itself? */
static int sym_fixes(const UtttGame *g, int s)
{
    const uint8_t *p = SYM[s];
    const uint16_t *t = SYM9[s];
    for (int b = 0; b < 9; b++) {
        if (t[g->cm[0][b]] != g->cm[0][p[b]]) return 0;
        if (t[g->cm[1][b]] != g->cm[1][p[b]]) return 0;
    }
    if (t[g->bm[0]] != g->bm[0] || t[g->bm[1]] != g->bm[1]) return 0;
    if (t[g->bdrawn] != g->bdrawn) return 0;
    if (g->forced != UTTT_ANY && p[g->forced] != g->forced) return 0;
    return 1;
}

/* TWO MOVES ARE ONE MOVE when a transform that fixes the position maps one
 * onto the other - playing either leads to the same game wearing a different
 * coat. Dropping the copies spends a fixed allowance of rollouts on real
 * alternatives. On the empty board that is 81 choices down to 15.
 *
 * It is an opening device and it stops paying almost at once, because a move
 * does not only place a stone, it also names the block the reply must go in,
 * and that alone breaks most of what symmetry is left. Over 20,000 random
 * games, positions still equal to some mirror of themselves:
 *
 *     ply 0   100.0%    81.0 -> 15.0 moves   5.40x
 *     ply 1    41.4%     8.7 ->  5.6         1.55x
 *     ply 2    13.9%
 *     ply 4     1.2%
 *     ply 8     never again
 *
 * Which is why the check runs first and leaves immediately: after the
 * opening it is seven comparisons that find nothing. */
static int use_root_sym = 1;

static int root_dedupe(const UtttGame *g, uint8_t *list, int n)
{
    if (!use_root_sym || n < 2) return n;
    if (!sym_ready) build_sym();
    int fix[8], nf = 0;
    for (int s = 1; s < 8; s++)
        if (sym_fixes(g, s)) fix[nf++] = s;
    if (!nf) return n;

    int out = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int k = 0; k < nf && !dup; k++) {
            const uint8_t *p = SYM[fix[k]];
            uint8_t m2 = (uint8_t)(p[list[i] / 9] * 9 + p[list[i] % 9]);
            for (int j = 0; j < out; j++)
                if (list[j] == m2) { dup = 1; break; }
        }
        if (!dup) list[out++] = list[i];
    }
    return out;
}

static uint64_t pos_key(const UtttGame *g)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)g->cm;
    for (size_t i = 0; i < sizeof g->cm; i++) h = (h ^ p[i]) * 1099511628211ull;
    h = (h ^ g->bm[0])  * 1099511628211ull;
    h = (h ^ g->bm[1])  * 1099511628211ull;
    h = (h ^ g->bdrawn) * 1099511628211ull;
    h = (h ^ g->forced) * 1099511628211ull;
    h = (h ^ g->turn)   * 1099511628211ull;
    /* Never 0: an untouched table slot is all zeroes, and a position whose
     * key happened to be 0 would read that empty slot back as its own
     * entry - a proved draw, for free, wrongly. */
    return h ? h : 1;
}

/* WHAT EXACT PLAY IS ALLOWED TO SPEND. Counted in nodes, not in empty
 * squares, and that correction was worth making twice over.
 *
 * The gate used to be "11 empty squares or fewer". Raising it by measuring
 * how long the solver took on positions at each emptiness said 24 was free,
 * at 5.8ms - and in real games a gate of 19 could not finish ten of them.
 * The bench was drawing its positions from RANDOM play, and random play
 * closes blocks fast; a closed block is a whole branch the solver never
 * walks. Bots keep blocks alive. So two positions with 22 empty squares can
 * differ by orders of magnitude in the size of the tree above them, and
 * emptiness never sees the difference.
 *
 * A node count does, because it is the thing that actually runs out. The
 * search stops when the budget is gone and says so, the table keeps every
 * proof it managed on the way, and the next move in the same game starts
 * from what the last one established. `uttt_mate_in` is budgeted the same
 * way and for the same reason.
 *
 * WHAT EACH BUDGET BUYS, on positions out of real quill games:
 *
 *       2,000 nodes   0.09 ms   settled 44.8%   perfect from 22 empties
 *      20,000         0.70      settled 50.8%   perfect from 24
 *     300,000         7.98      settled 63.9%   79% even at 30
 *
 * 20,000 is the shipped figure: it more than doubles the distance from the
 * end at which play is exact - the old gate was ELEVEN squares - for about
 * 6% more time a game.
 *
 * AND IT IS WORTH NO POINTS. Against the old gate, same bot and the same
 * rollouts, 400 games: 49.4%, -0.3 sigma, for 3.3% more time. 300,000
 * nodes reads the same over 200 games. Quill's tree already proves most of
 * what this proves, by itself, from the same
 * playouts - it settles 986 of 2000 endgame positions with no solver in
 * front of it at all - and in a position that is already won or already
 * drawn, replacing a good move with a perfect one changes no results. This
 * is bought for exactness, not for Elo, and the next person to wonder why
 * a stronger endgame did not show up in the ladder can stop here. */
#define SOLVE_NODES 20000L
static long solve_nodes = SOLVE_NODES;
/* A cheap filter so the opening does not pay the budget to learn nothing:
 * with this many squares still empty a proof is hopeless anyway. */
#define SOLVE_EMPTIES 30
static int solve_gate = SOLVE_EMPTIES;

static int solve(UtttGame *g, int depth_left, long *nodes)
{
    if (g->over)
        return g->over == UTTT_DRAW ? 0 : (g->over == g->turn ? 1 : -1);
    if (depth_left <= 0) return 2;
    if (--*nodes <= 0) return 2;                   /* out of budget */

    if (!tt) {
        tt = calloc(TT_SIZE, sizeof *tt);
        if (!tt) return 2;
    }
    uint64_t key = pos_key(g);
    TtEntry *e = &tt[key & (TT_SIZE - 1)];
    if (e->key == key) return e->val;

    uint8_t list[81];
    int n = uttt_legal(g, list);

    /* best first: a proof that ends early is a proof that costs nothing */
    int sc[81];
    for (int i = 0; i < n; i++) sc[i] = score_move(g, list[i]);
    for (int i = 1; i < n; i++) {
        uint8_t m = list[i]; int v = sc[i], j = i - 1;
        while (j >= 0 && sc[j] < v) { list[j + 1] = list[j]; sc[j + 1] = sc[j]; j--; }
        list[j + 1] = m; sc[j + 1] = v;
    }

    int best = -2, unknown = 0;
    for (int i = 0; i < n; i++) {
        UtttGame t = *g;
        uttt_play(&t, list[i]);
        int v = solve(&t, depth_left - 1, nodes);
        if (v == 2) { unknown = 1; continue; }
        v = -v;
        if (v > best) best = v;
        if (best == 1) break;                      /* a win is a win */
    }
    int out = (best == 1) ? 1 : (unknown ? 2 : best);
    if (out != 2) { e->key = key; e->val = (int8_t)out; }
    return out;
}


/* EIGHTY-ONE QUESTIONS BECOME NINE POPCOUNTS. Asking a square at a time was
 * fine when a square was a byte; now it is a bit, and the whole block
 * answers at once. */
static int empties(const UtttGame *g)
{
    int e = 0;
    for (unsigned live = g->live; live; live &= live - 1) {
        int b = __builtin_ctz(live);
        e += __builtin_popcount(uttt_open_cells(g, b));
    }
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

    /* MASKS, NOT A LIST. This is a proof search: it walks every move of
     * every position it visits, and it visits thousands per move, so the
     * eighty-one byte buffer was being filled and thrown away at every
     * node of the tree. */
    unsigned blocks = uttt_legal_blocks(g);
    if (!blocks) return MATE_NONE;
    const uint8_t me = g->turn;
    int best = MATE_NONE;

    for (unsigned bb = blocks; bb; bb &= bb - 1) {
      int b0 = __builtin_ctz(bb);
      for (unsigned o = uttt_open_cells(g, b0); o; o &= o - 1) {
        uint8_t mv = (uint8_t)(b0 * 9 + __builtin_ctz(o));
        UtttGame t = *g;
        uttt_play(&t, mv);
        if (t.over) {
            /* Over on our own move: a win is mate in one, a draw is not a
             * win, and we cannot lose by moving. */
            if (t.over == me && best > 1) { best = 1; if (out) *out = mv; }
            continue;
        }
        /* EVERY reply has to still lose. One escape and the line is not a
         * proof, which is the whole difference between this and a search
         * that averages. */
        unsigned rblocks = uttt_legal_blocks(&t);
        int worst = 0;
        for (unsigned rb = rblocks; rb && worst < MATE_NONE; rb &= rb - 1) {
          int b1 = __builtin_ctz(rb);
          for (unsigned ro = uttt_open_cells(&t, b1); ro; ro &= ro - 1) {
            UtttGame u = t;
            uttt_play(&u, (uint8_t)(b1 * 9 + __builtin_ctz(ro)));
            if (u.over) { worst = MATE_NONE; break; }   /* they won, or drew */
            /* No line longer than the best already found is worth proving. */
            int cap = (best < MATE_NONE ? best - 2 : depth) - 2;
            int v = mate_in(&u, cap < depth - 2 ? cap : depth - 2, nodes);
            if (v >= MATE_NONE) { worst = MATE_NONE; break; }
            if (v > worst) worst = v;
          }
        }
        if (worst < MATE_NONE && 2 + worst < best) {
            best = 2 + worst;
            if (out) *out = mv;
        }
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
            unsigned bit = 1u << L[i][j];
            if (g->bm[me - 1] & bit) continue;
            if (g->live & bit) need++;
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
/* EXACT PLAY AT THE ROOT, on one budget shared by every candidate. Returns
 * 1 and fills `out` when it settled the position.
 *
 * A proved win ends it on the spot - nothing beats winning, so the rest of
 * the list does not matter and neither does what is left to spend. Anything
 * short of that needs the whole list known before it can be trusted: a move
 * proved to draw is only the best move if no unproved sibling would have
 * won, so one unknown sends the decision back to the search that can live
 * with not knowing.
 *
 * A PROVED LOSS IS NOT A MOVE. When every reply loses, perfect play has
 * nothing left to say and picking the first proved move amounts to
 * resigning in place. The opponent still has to find the win, so the search
 * is the better adviser: it steers toward the line where most of their
 * replies throw it away. This never mattered while exact play began eleven
 * squares from the end, where a lost position is lost in practice too. It
 * matters a great deal starting thirty squares out, which is most of a
 * game, and it is what the first budgeted measurement was losing on. */
static int solve_root(const UtttGame *g, const uint8_t *list, int n,
                      uint8_t *out)
{
    if (empties(g) > solve_gate) return 0;
    long left = solve_nodes;
    int bestv = -2, unknown = 0;
    uint8_t bestm = list[0];
    for (int i = 0; i < n; i++) {
        UtttGame t = *g;
        uttt_play(&t, list[i]);
        int v = solve(&t, 81, &left);
        if (v == 2) { unknown = 1; continue; }
        v = -v;
        if (v > bestv) { bestv = v; bestm = list[i]; }
        if (bestv == 1) { *out = bestm; return 1; }
    }
    if (unknown || bestv <= -2) return 0;
    if (bestv < 0) return 0;                  /* lost anyway - go make it hard */
    *out = bestm;
    return 1;
}

static uint8_t mc_move(const UtttGame *g, int budget, uint64_t *rs,
                       int crn, int biased, int endgame, int speed)
{
    uint8_t list[81];
    int n = uttt_legal(g, list);
    if (n <= 0) return 0;
    n = root_dedupe(g, list, n);
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

    if (endgame) {
        uint8_t sm = 0;
        if (solve_root(g, list, n, &sm)) return sm;
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
            score[i] += playout(&t, &s, biased, me, 0);
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

/* -------------------------------------------------------------------- tree
 * UCT: the same biased playouts, spent by a TREE instead of a flat loop.
 *
 * Flat Monte Carlo gives every candidate the same forty playouts, so a move
 * that lost its first ten still gets thirty more, and nothing below the
 * first ply is ever remembered from one playout to the next. A tree keeps
 * the statistics of every position it has passed through and spends the
 * next playout where the numbers say the game is still undecided - which is
 * both fewer playouts on the losers and a search that reaches two, three,
 * four plies down the lines that matter.
 *
 * The playout count is IDENTICAL to sniper's: `budget` a legal move. This
 * is a comparison of how the playouts are spent, not how many.
 *
 * Three things ride on the tree:
 *
 *   PRIORS. Every child is born with the heuristic's opinion of its move,
 *   which decays as real visits arrive. A tree at a few hundred playouts is
 *   mostly frontier, and a frontier ordered by the heuristic is the biased
 *   playout's lesson applied to the tree itself.
 *
 *   PROOFS. A terminal child is a fact and is marked as one, and a node
 *   whose children are all facts becomes a fact too. A proved win is taken
 *   and a proved loss is never selected again, so the tree cannot talk
 *   itself into a line the opponent can simply end - which is the whole
 *   difference between this and averaging.
 *
 *   THE ROOT IS STILL THE SNIPER'S. The mate search and the exact endgame
 *   run first, exactly as they do for sniper. What is new is only what
 *   happens when neither can answer. */
/* Measured, 300 games a pairing against sniper; the table is in uttt_bots.h.
 * Less exploration and a heavier prior were both worth points; the heavier
 * prior only at this exploration. */
#define TREE_C      0.5     /* exploration                                    */
#define TREE_PB     1.5     /* progressive bias: prior weight at zero visits  */
#define TREE_FPU    0.55    /* an unvisited child is worth about a coin flip  */

enum { PV_UNKNOWN = 0, PV_WIN = 1, PV_LOSS = 2, PV_DRAW = 3 };

typedef struct {
    uint32_t visits;
    uint32_t score;       /* 2 a win, 1 a draw, for the player who MOVED here */
    uint32_t first;       /* index of the first child, when expanded         */
    float    prior;       /* the heuristic's opinion, in about [-1, 0.5]     */
    uint8_t  mv;
    uint8_t  nchild;      /* 0 = not expanded                                */
    uint8_t  pv;          /* for the player TO MOVE at this node             */
} TreeNode;

/* TWO POOLS, because the tree is KEPT between moves. The subtree under the
 * move we played and the reply we got is next move's starting point, and
 * re-rooting it means copying it out of one pool into the other so the
 * discarded siblings are gone rather than leaked. A cache of playouts
 * already spent: the allowance is the same, the tree just starts fuller. */
/* THE POOL FILLS, AND IT DOES NOT MATTER. Worth knowing before anyone
 * spends an afternoon on it, because the first measurement looks damning.
 *
 * The tree stops growing the moment this is full, and it fills early:
 *
 *       400 rollouts a candidate      6,000 playouts     11,873 nodes
 *     1,600                          24,000             71,620
 *     6,400                          96,000            222,737
 *    25,600                         384,000            250,000  FULL
 * 1,048,576                      15,728,640            250,000  FULL
 *
 * So at a million rollouts the search pours fifteen million playouts into a
 * tree that has been frozen since about twenty-five thousand. That looks
 * exactly like the reason more thinking stops helping - and it is not.
 * Raised to 4,000,000 nodes and played against this at a budget where this
 * one provably saturates: 49.2%, -0.1 sigma over 60 games, 51 of them drawn.
 * Sixteen times the tree is worth nothing at all.
 *
 * Which matches the rollouts themselves: 6,400 against 400 is +0.2 sigma,
 * 40,000 against 6,400 is +0.6 sigma. Neither the size of the tree nor the
 * number of playouts is what limits this bot.
 *
 * WHAT IS LEFT IS THE LEAF. Every node, however many there are, is scored
 * by twelve plies of playout and `leaf_eval`. More of them samples the same
 * biased estimate more finely; it cannot make the estimate better. Anyone
 * wanting a stronger quill should start there and not here. */
#define TREE_POOL 250000
static TreeNode tree_pool_a[TREE_POOL], tree_pool_b[TREE_POOL];
static TreeNode *tree_pool = tree_pool_a;
static uint8_t  tree_cache_move[UTTT_MAX_PLIES];
static int      tree_cache_plies = -1;     /* the history the pool was built at */
static uint32_t tree_cache_used;

static float tree_prior(const UtttGame *g, uint8_t mv)
{
    int h = score_move(g, mv);
    if (h > 200) h = 200;
    if (h < -400) h = -400;
    return (float)h / 400.0f;
}

/* Give a node its children. Terminal ones are marked on the spot. Returns 0
 * when the pool is full, in which case the node stays a leaf. */
static int tree_expand(uint32_t node, const UtttGame *g, uint32_t *used)
{
    uint8_t list[81];
    int n = uttt_legal(g, list);
    /* At the root only, where the opening still has symmetry to give and
     * where the saving is spent on every rollout that follows. Deeper the
     * check costs seven comparisons a node and finds nothing. */
    if (node == 0) n = root_dedupe(g, list, n);
    if (n <= 0 || *used + (uint32_t)n > TREE_POOL) return 0;
    TreeNode *nd = &tree_pool[node];
    nd->first = *used; nd->nchild = (uint8_t)n; *used += (uint32_t)n;
    for (int i = 0; i < n; i++) {
        TreeNode *c = &tree_pool[nd->first + i];
        memset(c, 0, sizeof *c);
        c->mv = list[i];
        c->prior = tree_prior(g, list[i]);
        UtttGame t = *g;
        uttt_play(&t, list[i]);
        /* The mover cannot lose by moving: it is a win for them, or a draw. */
        if (t.over) c->pv = (t.over == g->turn) ? PV_LOSS : PV_DRAW;
    }
    return 1;
}

/* Re-derive a node's proof from its children. */
static void tree_prove(uint32_t node)
{
    TreeNode *nd = &tree_pool[node];
    if (nd->pv != PV_UNKNOWN || nd->nchild == 0) return;
    int all_win = 1, any_draw = 0;
    for (int i = 0; i < nd->nchild; i++) {
        uint8_t cpv = tree_pool[nd->first + i].pv;
        if (cpv == PV_LOSS) { nd->pv = PV_WIN; return; }   /* they lose */
        if (cpv == PV_DRAW) any_draw = 1;
        if (cpv != PV_WIN) all_win = 0;
    }
    if (all_win) nd->pv = any_draw ? PV_DRAW : PV_LOSS;
}

/* The child to descend into. Proved losses (for us) are skipped; a proved
 * win never reaches here because the node itself would already be proved. */
/* WHY THERE IS NO log() CACHE HERE, having built one and thrown it away.
 *
 * The UCT term below wants log(visits + 1) and `visits` is an integer, so
 * libm is asked the same few thousand questions over and over - and an
 * instruction-level sample said the return from `bl _log` was 5.9% of the
 * runtime. A cache of what log() returned would be the same BITS, so the
 * bots would play the same games; it looked free.
 *
 * It bought nothing. 1.41 ms a move to 1.40 at forty rollouts, and 9.25 to
 * 9.25 at four hundred, where the tree is ten times the size and this is
 * called ten times as often.
 *
 * THE LESSON IS ABOUT THE MEASUREMENT, not about log(). Those instruction
 * counts are where the program counter was CAUGHT, and on this hardware it
 * is caught at the instruction after a `bl` far more often than that call
 * costs - the three hottest addresses in the whole binary were the return
 * sites of uttt_play, mate_root and log, at 59.8%, 26.8% and 5.9%. Return
 * site attribution is not cost attribution. Anything found that way has to
 * be confirmed by removing it and timing what is left.
 */
static uint32_t tree_select(uint32_t node, uint64_t *rs)
{
    const TreeNode *nd = &tree_pool[node];
    double lnN = log((double)nd->visits + 1.0);
    double best = -1e9; uint32_t bi = nd->first; int nb = 0;
    for (int i = 0; i < nd->nchild; i++) {
        const TreeNode *c = &tree_pool[nd->first + i];
        double u;
        if (c->pv == PV_WIN) continue;                   /* we lose there */
        if (c->pv == PV_DRAW) {
            u = 0.5 + TREE_C * sqrt(lnN / (c->visits + 1.0));
        } else {
            double mean = c->visits ? (double)c->score / (200.0 * c->visits)
                                    : TREE_FPU;
            u = mean + TREE_C * sqrt(lnN / (c->visits + 1.0))
                     + TREE_PB * c->prior / (c->visits + 1.0);
        }
        if (u > best + 1e-9) { best = u; bi = nd->first + (uint32_t)i; nb = 1; }
        else if (u > best - 1e-9 && rnd(rs, (uint32_t)++nb) == 0)
            bi = nd->first + (uint32_t)i;
    }
    return bi;
}

/* Grow the tree from `g` for `playouts` playouts, or until the root is a
 * proof. The pool holds the result. */
/* Copy the subtree at src[si] into dst[di], children after it. */
static void tree_copy(TreeNode *dst, uint32_t *dused, const TreeNode *src,
                      uint32_t si, uint32_t di)
{
    dst[di] = src[si];
    if (!src[si].nchild) return;
    uint32_t f = *dused; *dused += src[si].nchild;
    dst[di].first = f;
    for (int i = 0; i < src[si].nchild; i++)
        tree_copy(dst, dused, src, src[si].first + i, f + i);
}

/* Re-root the kept tree at `g`, if `g` continues the history it was built
 * at and every ply since is a child the tree had. Returns the pool's used
 * count, or 0 when there is nothing to keep. */
static uint32_t tree_reroot(const UtttGame *g)
{
    int k = g->n_plies - tree_cache_plies;
    if (tree_cache_plies < 0 || k <= 0 ||
        memcmp(g->move, tree_cache_move, (size_t)tree_cache_plies) != 0)
        return 0;
    uint32_t node = 0;
    for (int i = 0; i < k; i++) {
        const TreeNode *nd = &tree_pool[node];
        uint32_t next = 0;
        for (int c = 0; c < nd->nchild; c++)
            if (tree_pool[nd->first + c].mv == g->move[tree_cache_plies + i])
                next = nd->first + (uint32_t)c;
        if (!next) return 0;
        node = next;
    }
    if (!tree_pool[node].nchild) return 0;
    TreeNode *other = tree_pool == tree_pool_a ? tree_pool_b : tree_pool_a;
    uint32_t used = 1;
    tree_copy(other, &used, tree_pool, node, 0);
    tree_pool = other;
    return used;
}

static void tree_search(const UtttGame *g, long playouts, uint64_t *rs,
                        int keep)
{
    uint32_t used = keep ? tree_reroot(g) : 0;
    if (!used) {
        used = 1;
        memset(&tree_pool[0], 0, sizeof tree_pool[0]);
        tree_expand(0, g, &used);
    }


    uint32_t path[UTTT_MAX_PLIES + 1];
    for (long r = 0; r < playouts && tree_pool[0].pv == PV_UNKNOWN; r++) {
        UtttGame t = *g;
        int depth = 0;
        uint32_t node = 0;
        path[depth++] = node;

        /* DESCEND until a proof, an unexpanded node, or a fresh child. */
        for (;;) {
            TreeNode *nd = &tree_pool[node];
            if (nd->pv != PV_UNKNOWN) break;
            if (nd->nchild == 0) {
                if (nd->visits == 0 || !tree_expand(node, &t, &used)) break;
                tree_prove(node);
                if (nd->pv != PV_UNKNOWN) break;
            }
            node = tree_select(node, rs);
            uttt_play(&t, tree_pool[node].mv);
            path[depth++] = node;
            if (tree_pool[node].visits == 0) break;
        }

        /* THE RESULT: a proof where there is one, a playout otherwise. */
        /* 0..200 from the side to move AT THE LEAF; each node flips it. */
        const uint8_t leaf_turn = t.turn;
        int val;
        const TreeNode *leaf = &tree_pool[node];
        if (leaf->pv == PV_WIN)       val = 200;
        else if (leaf->pv == PV_LOSS) val = 0;
        else if (leaf->pv == PV_DRAW) val = 100;
        else {
            uint64_t s = (*rs += 0x9E3779B97F4A7C15ull);
            val = playout(&t, &s, 1, leaf_turn, W.leaf_cutoff);
        }

        /* BACK UP the score, then the proof. A node's score belongs to the
         * player who moved into it; the root belongs to nobody. */
        UtttGame u = *g;
        for (int d = 0; d < depth; d++) {
            TreeNode *nd = &tree_pool[path[d]];
            nd->visits++;
            if (d > 0) {
                uint8_t mover = u.turn;
                uttt_play(&u, nd->mv);
                nd->score += (mover == leaf_turn) ? val : 200 - val;
            }
        }
        for (int d = depth - 1; d >= 0; d--) tree_prove(path[d]);
    }
    *rs += 0x9E3779B97F4A7C15ull;

    /* Remember what this tree is a tree OF, for next time. */
    tree_cache_plies = keep ? g->n_plies : -1;
    memcpy(tree_cache_move, g->move, (size_t)g->n_plies);
    tree_cache_used = used;
}

/* What the tree can PROVE about the side to move from `g`, after
 * `playouts` playouts: +1 a win, 0 a draw, -1 a loss, 2 nothing yet.
 *
 * Exposed for the same reason `uttt_mate_in` is: a proof is a fact, and a
 * test can hold it against the exhaustive solver. */
int uttt_tree_proof(const UtttGame *g, long playouts, uint64_t *rs)
{
    if (g->over || uttt_legal(g, (uint8_t[81]){0}) <= 0) return 2;
    tree_search(g, playouts, rs, 0);
    switch (tree_pool[0].pv) {
    case PV_WIN:  return 1;
    case PV_LOSS: return -1;
    case PV_DRAW: return 0;
    default:      return 2;
    }
}

static uint8_t tree_move(const UtttGame *g, int budget, uint64_t *rs)
{
    uint8_t list[81];
    int n = uttt_legal(g, list);
    if (n <= 0) return 0;
    n = root_dedupe(g, list, n);
    if (n == 1) return list[0];

    /* THE SNIPER'S ROOT, unchanged. */
    uint8_t mv = 0;
    if (uttt_mate_in(g, 2000L, &mv)) return mv;
    {
        uint8_t sm = 0;
        if (solve_root(g, list, n, &sm)) return sm;
    }

    /* THE SAME ALLOWANCE AS THE FLAT SEARCH: `budget` a legal move. */
    tree_search(g, (long)budget * n, rs, 1);

    /* THE ANSWER. A proved win if the tree found one; otherwise the most
     * visited child that is not a proved loss, and on equal visits the
     * better score, and on equal score the heuristic - the same tie-break
     * as the flat search, for the same reason. */
    const TreeNode *root = &tree_pool[0];
    uint32_t bi = root->first;
    int b_alive = -1; long b_v = -1; double b_s = -1; int b_h = 0;
    for (int i = 0; i < root->nchild; i++) {
        const TreeNode *c = &tree_pool[root->first + i];
        if (c->pv == PV_LOSS) return c->mv;             /* proved win */
        int alive = c->pv != PV_WIN;                     /* not a proved loss */
        long v = (long)c->visits;
        double s = c->visits ? (double)c->score / (2.0 * c->visits)
                             : (c->pv == PV_DRAW ? 0.5 : 0.0);
        int h = score_move(g, c->mv);
        if (alive > b_alive || (alive == b_alive && (v > b_v ||
            (v == b_v && (s > b_s || (s == b_s && h > b_h)))))) {
            bi = root->first + (uint32_t)i;
            b_alive = alive; b_v = v; b_s = s; b_h = h;
        }
    }
    return tree_pool[bi].mv;
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
    case BOT_QUILL:  return tree_move(g, budget, rs);
    default:         return list[0];
    }
}

int uttt_solve(const UtttGame *g, int depth)
{
    UtttGame t = *g;
    long left = solve_nodes;
    return solve(&t, depth, &left);
}

/* For the measurement only: flip canonical-by-symmetry hashing on or off and
 * empty the table, so one binary can run both sides of the comparison. */
void uttt_solve_gate(int empties) { solve_gate = empties; }
void uttt_solve_budget(long nodes) { solve_nodes = nodes; }
void uttt_root_symmetry(int on) { use_root_sym = on; }



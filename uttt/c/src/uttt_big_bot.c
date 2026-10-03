/* The greedy lookahead for the recursive game; what it promises is in
 * uttt_big_bot.h. Every move it searches is a real utb_play, taken back by
 * restoring the few bytes the play changed (the cell, the statuses on the
 * leaf's path, and the game's header fields), so the rules live in
 * uttt_big.c alone and the search only reads what a play did. */
#include "uttt_big_bot.h"
#include <string.h>

/* The node numbering uttt_big.h states as its contract: the level-L node
 * with prefix p is id (9^L - 1) / 8 + p. */
static const int32_t OFF[UTB_DEPTH_MAX + 1]  = { 0, 1, 10, 91, 820, 7381 };
static const int32_t POW9[UTB_DEPTH_MAX + 1] = { 1, 9, 81, 729, 6561, 59049 };

/* By the side of the grid in cells: 3, 9, 27, 81 (index 1..4). */
static const int32_t W_SIZE[UTB_DEPTH_MAX] = { 0, UTB_BOT_W3, UTB_BOT_W9, UTB_BOT_W27, UTB_BOT_W81 };

#define INF (1 << 28)

int utb_bot_weight(const UtbGame *g, int id)
{
    int L = utb_node_level(g, id);
    if (L < 0) return 0;
    if (L == 0) return UTB_BOT_WGAME;
    return W_SIZE[g->depth - L];
}

/* ------------------------------------------------------------------ dice */

static uint64_t splitmix(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* xorshift64*, one multiply a draw */
static uint64_t next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545f4914f6cdd1dull;
}

/* 0..n-1, n >= 1: the high 32 bits scaled, so no modulo bias worth naming */
static uint32_t below(uint64_t *s, uint32_t n)
{
    return (uint32_t)(((next(s) >> 32) * (uint64_t)n) >> 32);
}

void utb_bot_init(UtbBot *b, uint64_t seed)
{
    memset(b, 0, sizeof *b);
    uint64_t s = seed;
    b->rng = splitmix(&s);
    if (!b->rng) b->rng = 0x9e3779b97f4a7c15ull;
    b->plies    = UTB_BOT_PLIES;
    b->budget   = UTB_BOT_BUDGET;
    b->cap_root = UTB_BOT_CAP_ROOT;
    b->cap_node = UTB_BOT_CAP_NODE;
}

void utb_bot_seat(UtbBot *b, uint64_t seed, int seat)
{
    utb_bot_init(b, seed + 0xd1b54a32d192ed03ull * (uint64_t)(seat & 0xff));
}

/* ------------------------------------------------------------ make, unmake */

typedef struct {
    int32_t last, prev, n_plies;
    uint8_t turn, over;
    uint8_t path[UTB_DEPTH_MAX];          /* the statuses on the leaf's path, by level */
} Undo;

/* Play mv (legal by construction) and return what the mover won by it: the
 * weight of every node on its path that was open and is now the mover's. */
static int make(UtbGame *g, int mv, Undo *u)
{
    const int D = g->depth, mover = g->turn;
    u->last = g->last; u->prev = g->prev; u->n_plies = g->n_plies;
    u->turn = g->turn; u->over = g->over;
    for (int L = 0; L < D; L++) u->path[L] = g->node[OFF[L] + mv / POW9[D - L]];
    if (!utb_play(g, mv)) return -1;
    int gain = 0;
    for (int L = 0; L < D; L++)
        if (u->path[L] == UTTT_OPEN && g->node[OFF[L] + mv / POW9[D - L]] == mover)
            gain += L == 0 ? UTB_BOT_WGAME : W_SIZE[D - L];
    return gain;
}

static void unmake(UtbGame *g, int mv, const Undo *u)
{
    const int D = g->depth;
    g->cell[mv] = UTTT_OPEN;
    for (int L = 0; L < D; L++) g->node[OFF[L] + mv / POW9[D - L]] = u->path[L];
    g->last = u->last; g->prev = u->prev; g->n_plies = u->n_plies;
    g->turn = u->turn; g->over = u->over;
}

/* ------------------------------------------------------------- candidates */

/* WIN[m]: the cells k that complete a line when added to the 9-bit mask m. */
static uint16_t WIN[512];
static int      win_ready;

static void win_init(void)
{
    if (win_ready) return;
    for (unsigned m = 0; m < 512; m++) {
        unsigned w = 0;
        for (int k = 0; k < 9; k++)
            if (!((m >> k) & 1u) && uttt_mask_line(m | (1u << k))) w |= 1u << k;
        WIN[m] = (uint16_t)w;
    }
    win_ready = 1;
}

/* One search at a time (a page runs one game, a test one bot at a time), so
 * the region scan has one scratch: every winner and every other open cell.
 * A node copies what it keeps onto its own stack before it recurses. Leaves
 * fit 16 bits (59,049 < 65,536). */
static uint16_t s_win[UTB_LEAVES_MAX], s_other[UTB_LEAVES_MAX];
static int32_t  s_gain[UTB_LEAVES_MAX];

typedef struct {
    UtbGame *g;
    UtbBot  *b;
    int      work, limited, stop;
    int      n_win, n_other, blocks;
} Ctx;

static void spend(Ctx *c, int units)
{
    c->work += units;
    if (c->limited && c->work > c->b->budget) c->stop = 1;
}

/* Every open bottom block under level-L node p, its cells sorted into the
 * mover's winners and the rest. The caller starts at the region, above
 * which nothing is decided. */
static void scan(Ctx *c, int L, int p)
{
    const UtbGame *g = c->g;
    if (g->node[OFF[L] + p] != UTTT_OPEN) return;
    if (L < g->depth - 1) {
        for (int k = 0; k < 9; k++) scan(c, L + 1, p * 9 + k);
        return;
    }
    c->blocks++;
    const uint8_t *cell = g->cell + p * 9;
    unsigned mine = 0, open = 0;
    for (int k = 0; k < 9; k++) {
        mine |= (unsigned)(cell[k] == g->turn) << k;
        open |= (unsigned)(cell[k] == UTTT_OPEN) << k;
    }
    unsigned win = WIN[mine] & open;
    for (int k = 0; k < 9; k++) {
        if (!((open >> k) & 1u)) continue;
        uint16_t mv = (uint16_t)(p * 9 + k);
        if ((win >> k) & 1u) s_win[c->n_win++] = mv;
        else                 s_other[c->n_other++] = mv;
    }
}

static void shuffle(uint64_t *rng, uint16_t *a, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = (int)below(rng, (uint32_t)(i + 1));
        uint16_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

/* The candidates at this node, at most `cap`, into out: winners first (by
 * what they win when there are more than fit), then a uniform sample of the
 * rest. `root` shuffles both, which is the bot's tie-break. */
static int cands(Ctx *c, int32_t *out, int cap, int root)
{
    UtbGame *g = c->g;
    int r = utb_region(g);
    if (r < 0) return 0;
    int L = utb_node_level(g, r);
    c->n_win = c->n_other = c->blocks = 0;
    scan(c, L, r - OFF[L]);
    spend(c, c->blocks);
    uint64_t *rng = &c->b->rng;
    int n = 0;

    if (c->n_win <= cap) {
        if (root) shuffle(rng, s_win, c->n_win);
        for (int i = 0; i < c->n_win; i++) out[n++] = s_win[i];
    } else {
        /* More winners than room: keep the ones that win the most. Shuffled
         * first, so the order among equals is the dice's; then pass by pass
         * from the largest gain down, which takes as many passes as there
         * are distinct gains (a handful). */
        shuffle(rng, s_win, c->n_win);
        for (int i = 0; i < c->n_win; i++) {
            Undo u;
            s_gain[i] = make(g, s_win[i], &u);
            unmake(g, s_win[i], &u);
        }
        spend(c, c->n_win);
        int above = INF;
        while (n < cap) {
            int top = -INF;
            for (int i = 0; i < c->n_win; i++)
                if (s_gain[i] < above && s_gain[i] > top) top = s_gain[i];
            if (top == -INF) break;
            for (int i = 0; i < c->n_win && n < cap; i++)
                if (s_gain[i] == top) out[n++] = s_win[i];
            above = top;
        }
    }

    int slots = cap - n;
    if (c->n_other <= slots) {
        if (root) shuffle(rng, s_other, c->n_other);
        for (int i = 0; i < c->n_other; i++) out[n++] = s_other[i];
    } else {
        /* a partial Fisher-Yates: the first `slots` of a uniform shuffle */
        for (int i = 0; i < slots; i++) {
            int j = i + (int)below(rng, (uint32_t)(c->n_other - i));
            uint16_t t = s_other[i]; s_other[i] = s_other[j]; s_other[j] = t;
            out[n++] = s_other[i];
        }
    }
    return n;
}

/* ----------------------------------------------------------------- search */

/* The best the side to move can make of the next `d` plies, by the weights,
 * from its own side. Fail-hard alpha-beta: a value at or outside the window
 * is only a bound, which is all the caller asks. */
static int negamax(Ctx *c, int d, int alpha, int beta)
{
    int32_t mv[UTB_BOT_CAP_MAX];
    int n = cands(c, mv, c->b->cap_node, 0);
    if (c->stop || n == 0) return 0;
    int best = -INF;
    for (int i = 0; i < n; i++) {
        Undo u;
        int gain = make(c->g, mv[i], &u);
        spend(c, 1);
        int v = gain;
        if (!c->g->over && d > 1 && !c->stop) v -= negamax(c, d - 1, gain - beta, gain - alpha);
        unmake(c->g, mv[i], &u);
        if (c->stop) return 0;
        if (v > best) {
            best = v;
            if (v > alpha) { alpha = v; if (alpha >= beta) break; }
        }
    }
    return best;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void sane(UtbBot *b)
{
    b->plies    = clampi(b->plies, 1, UTB_BOT_PLIES_MAX);
    b->cap_root = clampi(b->cap_root, 1, UTB_BOT_CAP_MAX);
    b->cap_node = clampi(b->cap_node, 1, UTB_BOT_CAP_MAX);
    if (b->budget < 0) b->budget = 0;
}

int utb_bot_move(UtbBot *b, UtbGame *g)
{
    if (g->over) return -1;
    sane(b);
    win_init();
    Ctx c = { g, b, 0, 0, 0, 0, 0, 0 };
    int32_t mv[UTB_BOT_CAP_MAX];
    int n = cands(&c, mv, b->cap_root, 1);
    if (n == 0) return -1;
    b->value = 0; b->depth = 0;
    for (int d = 1; d <= b->plies; d++) {
        c.limited = d > 1;              /* the first ply always finishes */
        int best = -INF, at = 0;
        for (int i = 0; i < n; i++) {
            Undo u;
            int gain = make(g, mv[i], &u);
            spend(&c, 1);
            int v = gain;
            if (!g->over && d > 1 && !c.stop) v -= negamax(&c, d - 1, gain - INF, gain - best);
            unmake(g, mv[i], &u);
            if (c.stop) break;
            if (v > best) { best = v; at = i; }
        }
        if (c.stop) break;
        /* the best to the front, the rest in their order: the next ply looks
         * at it first, and among equals the earlier (the dice's) still wins */
        int32_t top = mv[at];
        memmove(mv + 1, mv, (size_t)at * sizeof mv[0]);
        mv[0] = top;
        b->value = best; b->depth = d;
    }
    b->work = c.work;
    return mv[0];
}

int utb_bot_gain(UtbGame *g, int mv)
{
    if (!utb_legal_at(g, mv)) return -1;
    Undo u;
    int gain = make(g, mv, &u);
    unmake(g, mv, &u);
    return gain;
}

int utb_bot_value(UtbBot *b, UtbGame *g, int mv, int plies)
{
    if (!utb_legal_at(g, mv)) return INT32_MIN;
    sane(b);
    win_init();
    Ctx c = { g, b, 0, 0, 0, 0, 0, 0 };
    Undo u;
    int v = make(g, mv, &u);
    if (!g->over && plies > 1) v -= negamax(&c, plies - 1, -INF, INF);
    unmake(g, mv, &u);
    return v;
}

int utb_bot_material(const UtbGame *g, int side)
{
    int sum = 0;
    for (int id = 0; id < utb_nodes(g->depth); id++)
        if (g->node[id] == side) sum += utb_bot_weight(g, id);
    return sum;
}

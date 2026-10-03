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

/* A threat in a level-L node of a depth-D game: two ninths of that node's
 * win, which is two wins one level down. The root's win is the game, so its
 * threat is two of its children's wins, the same ratio to the level below as
 * every other level's. */
static int tw(int D, int L)
{
    return UTB_BOT_THREAT_NUM * (L == 0 ? W_SIZE[D - 1] : W_SIZE[D - L] / 9);
}

int utb_bot_threat_weight(const UtbGame *g, int id)
{
    int L = utb_node_level(g, id);
    return L < 0 ? 0 : tw(g->depth, L);
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

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int utb_bot_cap_for(int plies, int budget)
{
    /* The widest c whose tree of `plies` plies fits the budget. An alpha-beta
     * tree that meets its best move first has c^ceil(N/2) + c^floor(N/2) - 1
     * leaves, and a leaf with the scans and plays above it costs about
     * UTB_BOT_NODE_COST units. */
    plies = clampi(plies, 1, UTB_BOT_PLIES_MAX);
    if (budget <= 0) return UTB_BOT_CAP_NODE_MIN;
    const long room = (long)budget / UTB_BOT_NODE_COST;
    int c = UTB_BOT_CAP_NODE_MIN;
    for (int t = UTB_BOT_CAP_NODE_MIN + 1; t <= UTB_BOT_CAP_NODE_MAX; t++) {
        long hi = 1, lo = 1;
        for (int i = 0; i < (plies + 1) / 2 && hi <= room; i++) hi *= t;
        for (int i = 0; i < plies / 2 && lo <= room; i++) lo *= t;
        if (hi + lo - 1 > room) break;
        c = t;
    }
    return c;
}

void utb_bot_set(UtbBot *b, int plies, int budget)
{
    b->plies    = clampi(plies, 1, UTB_BOT_PLIES_MAX);
    b->budget   = clampi(budget, 0, UTB_BOT_BUDGET_MAX);
    b->cap_node = utb_bot_cap_for(b->plies, b->budget);
    b->cap_root = clampi(4 * b->cap_node, UTB_BOT_CAP_ROOT, UTB_BOT_CAP_MAX);
}

void utb_bot_init(UtbBot *b, uint64_t seed)
{
    memset(b, 0, sizeof *b);
    uint64_t s = seed;
    b->rng = splitmix(&s);
    if (!b->rng) b->rng = 0x9e3779b97f4a7c15ull;
    utb_bot_set(b, UTB_BOT_PLIES, UTB_BOT_BUDGET);
    b->threats = 1;
    b->extend  = 1;
}

void utb_bot_seat(UtbBot *b, uint64_t seed, int seat)
{
    utb_bot_init(b, seed + 0xd1b54a32d192ed03ull * (uint64_t)(seat & 0xff));
}

/* ---------------------------------------------------------------- threats */

/* The eight lines of a 3 x 3, as nine-bit masks (uttt_mask_line's). */
static const uint16_t LINE[8] = { 0x007, 0x038, 0x1c0, 0x049, 0x092, 0x124, 0x111, 0x054 };

/* PAIR[m]: the lines (bit i for LINE[i]) holding exactly two of the mask m. */
static uint8_t PAIR[512];
static int     tables_ready;

/* The open lines `m` threatens: two of its own and the third still open. */
static int threats_of(unsigned m, unsigned open)
{
    unsigned p = PAIR[m];
    int n = 0;
    for (int i = 0; p; i++, p >>= 1)
        if ((p & 1u) && (open & LINE[i] & ~m) == (LINE[i] & ~m)) n++;
    return n;
}

/* The level-L node with prefix p, its threats weighted, for `side` minus its
 * opponent's. Child `k` reads as `as` instead of what it holds (k < 0: no
 * override), which is how the position before a play is read after it. */
static int node_threats(const UtbGame *g, int L, int p, int side, int k, int as)
{
    const int D = g->depth;
    const uint8_t *ch = L == D - 1 ? g->cell + p * 9 : g->node + OFF[L + 1] + p * 9;
    unsigned x = 0, o = 0, open = 0;
    for (int i = 0; i < 9; i++) {
        int s = i == k ? as : ch[i];
        x    |= (unsigned)(s == UTTT_X) << i;
        o    |= (unsigned)(s == UTTT_O) << i;
        open |= (unsigned)(s == UTTT_OPEN) << i;
    }
    int d = threats_of(x, open) - threats_of(o, open);
    return tw(D, L) * (side == UTTT_X ? d : -d);
}

/* Every live threat under the level-L node p, not counting p itself: the
 * open nodes below it, for `side`. A node on the path of leaf `mv` (-1: no
 * path) is walked through whatever it holds and not counted itself: after a
 * play the path is decided, and what hung under it was alive the moment
 * before. `*seen` counts the nodes read. */
static int subtree_threats(const UtbGame *g, int L, int p, int side, int mv, int *seen)
{
    const int D = g->depth;
    if (L >= D - 1) return 0;
    const int on = mv < 0 ? -1 : mv / POW9[D - L - 1];
    int sum = 0;
    for (int k = 0; k < 9; k++) {
        int q = p * 9 + k;
        if (q == on) { sum += subtree_threats(g, L + 1, q, side, mv, seen); continue; }
        if (g->node[OFF[L + 1] + q] != UTTT_OPEN) continue;
        ++*seen;
        sum += node_threats(g, L + 1, q, side, -1, 0) + subtree_threats(g, L + 1, q, side, -1, seen);
    }
    return sum;
}

static void tables_init(void);

int utb_bot_threats(const UtbGame *g, int side)
{
    tables_init();
    if (g->node[0] != UTTT_OPEN) return 0;
    int seen = 0;
    return node_threats(g, 0, 0, side, -1, 0) + subtree_threats(g, 0, 0, side, -1, &seen);
}

static void tables_init(void)
{
    if (tables_ready) return;
    for (unsigned m = 0; m < 512; m++) {
        unsigned p = 0;
        for (int i = 0; i < 8; i++)
            if (__builtin_popcount(m & LINE[i]) == 2) p |= 1u << i;
        PAIR[m] = (uint8_t)p;
    }
    tables_ready = 1;
}

/* ------------------------------------------------------------ make, unmake */

typedef struct {
    int32_t last, prev, n_plies;
    uint8_t turn, over;
    uint8_t path[UTB_DEPTH_MAX];          /* the statuses on the leaf's path, by level */
    int     decided;                      /* the play decided a grid (won or drawn)  */
    int     seen;                         /* nodes read for the threats             */
} Undo;

/* Play mv (legal by construction) and return what the mover won by it: the
 * weight of every node on its path that was open and is now the mover's,
 * and with `threats` the change in the threats on the board, both sides',
 * from the mover's side. Only the path changes, so only the path's nodes are
 * read again - and when a grid is decided, every threat inside it dies, so
 * the open nodes under the highest one decided are read once to take theirs
 * away. The game's own end is worth the game and nothing else. */
static int make(UtbGame *g, int mv, Undo *u, int threats)
{
    const int D = g->depth, mover = g->turn;
    u->last = g->last; u->prev = g->prev; u->n_plies = g->n_plies;
    u->turn = g->turn; u->over = g->over;
    u->decided = 0; u->seen = 0;
    for (int L = 0; L < D; L++) u->path[L] = g->node[OFF[L] + mv / POW9[D - L]];
    if (!utb_play(g, mv)) return -1;
    int gain = 0, high = D;               /* the highest level decided by the play */
    for (int L = 0; L < D; L++) {
        uint8_t now = g->node[OFF[L] + mv / POW9[D - L]];
        if (u->path[L] == UTTT_OPEN && now != UTTT_OPEN) {
            if (L < high) high = L;
            if (now == mover) gain += L == 0 ? UTB_BOT_WGAME : W_SIZE[D - L];
        }
    }
    u->decided = high < D;
    if (!threats || high == 0) return gain;

    /* Below `high` the path's nodes are decided now: their threats, read as
     * they were, go. At `high - 1` (open) one child changed: the difference. */
    const int bottom = D - 1;
    if (high == D) {
        const int p = mv / 9;
        return gain + node_threats(g, bottom, p, mover, -1, 0)
                    - node_threats(g, bottom, p, mover, mv % 9, UTTT_OPEN);
    }
    for (int L = high; L <= bottom; L++) {
        const int p = mv / POW9[D - L];
        gain -= node_threats(g, L, p, mover, (mv / POW9[D - L - 1]) % 9, UTTT_OPEN);
    }
    gain -= subtree_threats(g, high, mv / POW9[D - high], mover, mv, &u->seen);
    {
        const int L = high - 1, p = mv / POW9[D - L], k = (mv / POW9[D - L - 1]) % 9;
        gain += node_threats(g, L, p, mover, -1, 0) - node_threats(g, L, p, mover, k, UTTT_OPEN);
    }
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
    tables_init();
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

/* A play in the search: one unit, and one more for every nine nodes its
 * threats read under a decided grid. */
static int play(Ctx *c, int mv, Undo *u)
{
    int v = make(c->g, mv, u, c->b->threats);
    spend(c, 1 + u->seen / 9);
    return v;
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
            s_gain[i] = make(g, s_win[i], &u, 0);
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
 * is only a bound, which is all the caller asks.
 *
 * THE EXTENSION: a move on the last ply (d == 1) that decides a grid is
 * answered by one more ply, the reply's best gain, when `ext` allows it; the
 * reply is never extended again. Deciding a grid is what changes where the
 * opponent may go (a decided target relaxes the region), so it is the move
 * whose cost a fixed horizon hides. */
static int quiesce(Ctx *c)
{
    int32_t mv[UTB_BOT_CAP_MAX];
    int n = cands(c, mv, c->b->cap_node, 0);
    if (c->n_win < n) n = c->n_win;       /* the winners lead the candidates */
    int best = 0;                         /* the reply may decline: stand pat */
    for (int i = 0; i < n && !c->stop; i++) {
        Undo u;
        int v = play(c, mv[i], &u);
        unmake(c->g, mv[i], &u);
        if (v > best) best = v;
    }
    return c->stop ? 0 : best;
}

static int negamax(Ctx *c, int d, int alpha, int beta, int ext)
{
    int32_t mv[UTB_BOT_CAP_MAX];
    int n = cands(c, mv, c->b->cap_node, 0);
    if (c->stop || n == 0) return 0;
    int best = -INF;
    for (int i = 0; i < n; i++) {
        Undo u;
        int gain = play(c, mv[i], &u);
        int v = gain;
        if (!c->g->over && !c->stop) {
            if (d > 1)                    v -= negamax(c, d - 1, gain - beta, gain - alpha, ext);
            else if (ext && u.decided)    v -= quiesce(c);
        }
        unmake(c->g, mv[i], &u);
        if (c->stop) return 0;
        if (v > best) {
            best = v;
            if (v > alpha) { alpha = v; if (alpha >= beta) break; }
        }
    }
    return best;
}

static void sane(UtbBot *b)
{
    b->plies    = clampi(b->plies, 1, UTB_BOT_PLIES_MAX);
    b->cap_root = clampi(b->cap_root, 1, UTB_BOT_CAP_MAX);
    b->cap_node = clampi(b->cap_node, 1, UTB_BOT_CAP_MAX);
    b->budget   = clampi(b->budget, 0, UTB_BOT_BUDGET_MAX);
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
        /* the first ply always finishes, and is never extended: it is the
         * floor that guarantees a move whatever the budget */
        c.limited = d > 1;
        int best = -INF, at = 0;
        for (int i = 0; i < n; i++) {
            Undo u;
            int gain = play(&c, mv[i], &u);
            int v = gain;
            if (!g->over && d > 1 && !c.stop) v -= negamax(&c, d - 1, gain - INF, gain - best, b->extend);
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
    int gain = make(g, mv, &u, 0);
    unmake(g, mv, &u);
    return gain;
}

int utb_bot_score(UtbGame *g, int mv)
{
    if (!utb_legal_at(g, mv)) return INT32_MIN;
    tables_init();
    Undo u;
    int v = make(g, mv, &u, 1);
    unmake(g, mv, &u);
    return v;
}

int utb_bot_value(UtbBot *b, UtbGame *g, int mv, int plies)
{
    if (!utb_legal_at(g, mv)) return INT32_MIN;
    sane(b);
    win_init();
    Ctx c = { g, b, 0, 0, 0, 0, 0, 0 };
    Undo u;
    int v = make(g, mv, &u, b->threats);
    if (!g->over && plies > 1) v -= negamax(&c, plies - 1, -INF, INF, b->extend);
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

int utb_bot_eval(const UtbGame *g, int side)
{
    tables_init();
    int other = side == UTTT_X ? UTTT_O : UTTT_X;
    return utb_bot_material(g, side) - utb_bot_material(g, other) + utb_bot_threats(g, side);
}

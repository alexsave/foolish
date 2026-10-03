/* Recursive Ultimate Tic-Tac-Toe - the rules are stated once, in uttt_big.h;
 * this file is them, and every function below asks one of three helpers
 * (status_of, the path walk in region_of, and the digit arithmetic) rather
 * than holding a copy of a rule. */
#include "uttt_big.h"
#include <string.h>

/* 9^L and the first id of level L, (9^L - 1) / 8: 0, 1, 10, 91, 820 - and
 * 7381, one past the last node of a depth-5 tree. 3^L for the geometry. */
static const int32_t POW9[UTB_DEPTH_MAX + 1] = { 1, 9, 81, 729, 6561, 59049 };
static const int32_t OFF[UTB_DEPTH_MAX + 1]  = { 0, 1, 10, 91, 820, 7381 };
static const int32_t POW3[UTB_DEPTH_MAX + 1] = { 1, 3, 9, 27, 81, 243 };

static int depth_ok(int depth)
{
    return depth >= UTB_DEPTH_MIN && depth <= UTB_DEPTH_MAX;
}

int utb_leaves(int depth) { return depth_ok(depth) ? POW9[depth] : 0; }
int utb_nodes(int depth)  { return depth_ok(depth) ? OFF[depth] : 0; }

int utb_init(UtbGame *g, int depth) { return utb_init_rule(g, depth, UTB_RULE_SHIFT); }

int utb_init_rule(UtbGame *g, int depth, int rule)
{
    if (!depth_ok(depth) || rule < UTB_RULE_SHIFT || rule > UTB_RULE_CLIMB_FREE) return 0;
    /* The whole struct, padding included, so two games that reached one
     * position by one route compare equal with memcmp (the tests do). */
    memset(g, 0, sizeof *g);
    g->depth   = (uint8_t)depth;
    g->turn    = UTTT_X;
    g->last    = UTB_NONE;
    g->prev    = UTB_NONE;
    g->rule    = (uint8_t)rule;
    return 1;
}

/* ---------------------------------------------------------------- the tree */

int utb_node_level(const UtbGame *g, int id)
{
    if (id < 0 || id >= OFF[g->depth]) return -1;
    int L = 0;
    while (id >= OFF[L + 1]) L++;
    return L;
}

int utb_node_prefix(const UtbGame *g, int id)
{
    int L = utb_node_level(g, id);
    return L < 0 ? -1 : id - OFF[L];
}

int utb_node_first(const UtbGame *g, int id)
{
    int L = utb_node_level(g, id);
    return L < 0 ? -1 : (id - OFF[L]) * POW9[g->depth - L];
}

int utb_node_span(const UtbGame *g, int id)
{
    int L = utb_node_level(g, id);
    return L < 0 ? 0 : POW9[g->depth - L];
}

int utb_node_id(const UtbGame *g, int level, int prefix)
{
    if (level < 0 || level >= g->depth || prefix < 0 || prefix >= POW9[level]) return -1;
    return OFF[level] + prefix;
}

int utb_ancestor(const UtbGame *g, int mv, int level)
{
    if (mv < 0 || mv >= POW9[g->depth]) return -1;
    if (level < 0 || level >= g->depth) return -1;
    return OFF[level] + mv / POW9[g->depth - level];
}

int utb_node(const UtbGame *g, int id)
{
    return (id >= 0 && id < OFF[g->depth]) ? g->node[id] : -1;
}

/* AN ID OFF THE TREE IS CLOSED: the question is "may anything under this be
 * played", and for nothing the safe answer is no. */
int utb_closed(const UtbGame *g, int id)
{
    int L = utb_node_level(g, id);
    if (L < 0) return 1;
    for (int p = id - OFF[L]; L >= 0; L--, p /= 9)
        if (g->node[OFF[L] + p] != UTTT_OPEN) return 1;
    return 0;
}

/* THE ONE STATUS RULE, read from a node's nine children: the cells at the
 * bottom level, the nodes above it. `node` is passed rather than read from
 * `g` so utb_adopt can build into a scratch table and leave *g untouched
 * when it refuses. Returns the status, or -1 for a node holding both an X
 * line and an O line - a position no sequence of moves reaches, because a
 * line decides the node and decided is forever. */
static int status_of(int depth, const uint8_t *cell, const uint8_t *node, int L, int p)
{
    const uint8_t *kids = (L == depth - 1) ? cell + p * 9 : node + OFF[L + 1] + p * 9;
    unsigned mx = 0, mo = 0, decided = 0;
    for (int k = 0; k < 9; k++) {
        mx |= (unsigned)(kids[k] == UTTT_X) << k;
        mo |= (unsigned)(kids[k] == UTTT_O) << k;
        decided |= (unsigned)(kids[k] != UTTT_OPEN) << k;
    }
    int x = uttt_mask_line(mx), o = uttt_mask_line(mo);
    if (x && o) return -1;
    if (x) return UTTT_X;
    if (o) return UTTT_O;
    return decided == 0x1ffu ? UTTT_DRAW : UTTT_OPEN;
}

/* EVERY STATUS ON LEAF mv's PATH, bottom up, from the children. Play and
 * undo both end here, so neither carries a "this move just won" shortcut
 * that the other would have to reverse: a node's status is a pure function
 * of its children, and recomputing it is the whole of both directions. The
 * root's status is the game's result. */
static void settle_path(UtbGame *g, int mv)
{
    const int D = g->depth;
    for (int L = D - 1; L >= 0; L--) {
        int p = mv / POW9[D - L];
        g->node[OFF[L] + p] = (uint8_t)status_of(D, g->cell, g->node, L, p);
    }
    g->over = g->node[0];
}

/* ---------------------------------------------------------------- the rules */

/* THE REGION FROM THE BOARD AND `last` ALONE, which is the whole of the
 * forced-move rule (the three send rules are stated in uttt_big.h).
 *
 * k is the level of the largest unit the last move completed: the shallowest
 * decided ancestor of its cell, or the depth (the cell alone) when none is.
 * Play inside a decided unit is illegal, so a decided ancestor of the last
 * move's cell was decided BY that move; the board and `last` are enough, and
 * undo and adopt need nothing stored. Rule A is k = 2 on every move. A k of
 * 1 has no level to climb to, so the cell's ordinary send stands (k = depth):
 * at depth 2 that is the shipped game's rule; deeper, its target lies inside
 * the decided level-1 node and relaxes to the root, "anywhere".
 *
 * The send drops digit k-1 of last's digits and keeps the rest (the bottom
 * block d1..d(k-2) d(k)..dD), or under CLIMB_FREE after a completion stops at
 * level k-1 (the block d1..d(k-2) d(k), free inside). Then the first decided
 * node on the walk down to the target relaxes it to that node's parent.
 * Nothing above the region is decided, by construction, which utb_legal
 * relies on. */
int utb_region(const UtbGame *g)
{
    if (g->over) return -1;
    if (g->last < 0) return UTB_ROOT;
    const int D = g->depth, last = g->last;
    int k = 2;
    if (g->rule != UTB_RULE_SHIFT) {
        k = D;
        for (int L = 1; L < D; L++)
            if (g->node[OFF[L] + last / POW9[D - L]] != UTTT_OPEN) { k = L; break; }
    }
    if (k == 1) k = D;
    /* the target: level TL, prefix tp */
    int TL, tp;
    if (g->rule == UTB_RULE_CLIMB_FREE && k < D) {
        TL = k - 1;
        tp = (last / POW9[D - k + 2]) * 9 + (last / POW9[D - k]) % 9;
    } else {
        TL = D - 1;
        tp = (last / POW9[D - k + 2]) * POW9[D - k + 1] + last % POW9[D - k + 1];
    }
    for (int L = 1; L <= TL; L++) {
        int p = tp / POW9[TL - L];
        if (g->node[OFF[L] + p] != UTTT_OPEN) return OFF[L - 1] + p / 9;
    }
    return OFF[TL] + tp;
}

int utb_legal_at(const UtbGame *g, int mv)
{
    const int D = g->depth;
    if (g->over || mv < 0 || mv >= POW9[D] || g->cell[mv] != UTTT_OPEN) return 0;
    if (utb_closed(g, OFF[D - 1] + mv / 9)) return 0;
    int r = utb_region(g);
    int L = utb_node_level(g, r);
    return mv / POW9[D - L] == r - OFF[L];
}

typedef struct { const UtbGame *g; int32_t *out; int cap, n; } Walk;

/* Depth first, children in digit order, so the moves come out ascending;
 * a decided node is skipped whole. The caller has already checked that
 * nothing above the start is decided. */
static void walk(Walk *w, int L, int p)
{
    const UtbGame *g = w->g;
    if (g->node[OFF[L] + p] != UTTT_OPEN) return;
    if (L == g->depth - 1) {
        for (int k = 0; k < 9; k++) {
            int mv = p * 9 + k;
            if (g->cell[mv] != UTTT_OPEN) continue;
            if (w->n < w->cap) w->out[w->n] = mv;
            w->n++;
        }
        return;
    }
    for (int k = 0; k < 9; k++) walk(w, L + 1, p * 9 + k);
}

int utb_legal(const UtbGame *g, int32_t *out, int cap)
{
    int r = utb_region(g);
    if (r < 0) return 0;
    int L = utb_node_level(g, r);
    Walk w = { g, out, cap < 0 ? 0 : cap, 0 };
    walk(&w, L, r - OFF[L]);
    return w.n;
}

int utb_play(UtbGame *g, int mv)
{
    if (!utb_legal_at(g, mv)) return 0;
    g->cell[mv] = g->turn;
    settle_path(g, mv);
    g->prev = g->last;
    g->last = mv;
    g->n_plies++;
    g->turn = (uint8_t)(g->turn == UTTT_X ? UTTT_O : UTTT_X);
    return 1;
}

int utb_undo(UtbGame *g)
{
    if (g->last < 0 || g->prev == UTB_UNKNOWN) return 0;
    int mv = g->last;
    g->cell[mv] = UTTT_OPEN;
    settle_path(g, mv);
    g->last = g->prev;
    g->prev = UTB_UNKNOWN;
    g->n_plies--;
    g->turn = (uint8_t)(g->turn == UTTT_X ? UTTT_O : UTTT_X);
    return 1;
}

int utb_adopt(UtbGame *g, int depth, const uint8_t *cells, int last)
{
    if (!depth_ok(depth)) return 0;
    const int n = POW9[depth];
    int count[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        if (cells[i] > UTTT_O) return 0;
        count[cells[i]]++;
    }
    const int plies = count[UTTT_X] + count[UTTT_O];
    if (count[UTTT_X] != count[UTTT_O] && count[UTTT_X] != count[UTTT_O] + 1) return 0;
    /* X moves first, so an odd count means X moved last. */
    const int mover = (plies & 1) ? UTTT_X : UTTT_O;
    if (plies == 0) {
        if (last != UTB_NONE) return 0;
    } else if (last < 0 || last >= n || cells[last] != mover) {
        return 0;
    }

    /* Bottom up into a scratch table (7 KB) so a refusal leaves *g as it
     * was. The cells are read where they are; they may BE g->cell. */
    uint8_t node[UTB_NODES_MAX];
    for (int L = depth - 1; L >= 0; L--)
        for (int p = 0; p < POW9[L]; p++) {
            int s = status_of(depth, cells, node, L, p);
            if (s < 0) return 0;
            node[OFF[L] + p] = (uint8_t)s;
        }

    memmove(g->cell, cells, (size_t)n);
    memset(g->cell + n, 0, sizeof g->cell - (size_t)n);
    memcpy(g->node, node, (size_t)OFF[depth]);
    memset(g->node + OFF[depth], 0, sizeof g->node - (size_t)OFF[depth]);
    g->depth   = (uint8_t)depth;
    g->turn    = (uint8_t)(mover == UTTT_X ? UTTT_O : UTTT_X);
    g->over    = node[0];
    g->rule    = UTB_RULE_SHIFT;          /* a picture carries no rule: the bubble's */
    g->last    = last;
    g->prev    = UTB_UNKNOWN;
    g->n_plies = plies;
    return 1;
}

void utb_count(const UtbGame *g, int n[3])
{
    n[0] = n[1] = n[2] = 0;
    for (int i = 0; i < POW9[g->depth]; i++) n[g->cell[i]]++;
}

/* ------------------------------------------------------------- geometry */

/* THE GRID IS INTEGER: a level-L node is column c, row r of a 3^L by 3^L
 * grid, built digit by digit (each digit is row-major in its 3 x 3), and its
 * rect runs from grid line c / N to grid line (c + 1) / N, each line divided
 * once. utb_hit reads the same lines back, so the two are one piece of
 * arithmetic in two directions rather than two sums of thirds that round
 * differently.
 *
 * THE WIDTH IS THE DIFFERENCE OF TWO LINES, not 1 / N: the two lines are
 * within a factor of two of each other (or the first is 0), so the
 * subtraction is exact and x + w lands exactly on the next cell's x. With
 * 1 / N the rects leave one-ulp gaps and overlaps between neighbours, and a
 * point in a gap would belong to no rect at all. */
static float line_at(int k, int N) { return (float)k / (float)N; }

static void rect_of(int L, int p, float r[4])
{
    int col = 0, row = 0;
    for (int l = 1; l <= L; l++) {
        int d = (p / POW9[L - l]) % 9;
        col = col * 3 + d % 3;
        row = row * 3 + d / 3;
    }
    const int N = POW3[L];
    r[0] = line_at(col, N);
    r[1] = line_at(row, N);
    r[2] = line_at(col + 1, N) - r[0];
    r[3] = line_at(row + 1, N) - r[1];
}

int utb_node_rect(const UtbGame *g, int id, float r[4])
{
    int L = utb_node_level(g, id);
    if (L < 0) { r[0] = r[1] = r[2] = r[3] = 0.f; return 0; }
    rect_of(L, id - OFF[L], r);
    return 1;
}

int utb_cell_rect(const UtbGame *g, int mv, float r[4])
{
    if (mv < 0 || mv >= POW9[g->depth]) { r[0] = r[1] = r[2] = r[3] = 0.f; return 0; }
    rect_of(g->depth, mv, r);
    return 1;
}

/* One axis: the grid line k / N at or left of u, read through line_at as
 * rect_of reads it. The first guess can be one off
 * at a boundary (u * N rounds), so it is nudged until the two agree; the far
 * edge (exactly 1) belongs to the last cell, as in uttt_hit. */
static int axis(float u, int N)
{
    int k = (int)(u * (float)N);
    if (k > N - 1) k = N - 1;
    if (k < 0) k = 0;
    while (k > 0 && u < line_at(k, N)) k--;
    while (k < N - 1 && u >= line_at(k + 1, N)) k++;
    return k;
}

int utb_hit(const UtbGame *g, float u, float v)
{
    if (!(u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f)) return -1;
    const int D = g->depth, N = POW3[D];
    int col = axis(u, N), row = axis(v, N), mv = 0;
    for (int l = 1; l <= D; l++) {
        int s = POW3[D - l];
        mv = mv * 9 + ((row / s) % 3) * 3 + (col / s) % 3;
    }
    return mv;
}

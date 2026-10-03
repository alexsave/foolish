/* The recursive game: depth 2 held to the shipped 9 x 9 move for move (under
 * all three send rules), depth 5 played to the end under each, and every
 * rule's corner built by hand.
 *
 *     make -C uttt/c run        (and asan)
 *     ./build/uttt_big_test [games]       depth-2 games, default 2000
 *     ./build/uttt_big_test [games] [n5]  ...and n5 depth-5 games, default 3 (run: 10)
 *
 * MUTATION CHECKS. Each rule below was broken in a copy of src/uttt_big.c and
 * the named assertion watched go red, then the file was copied back:
 *
 *  status: the O line never counts      -> depth 2 block/over/legal; adopt
 *                                          both-lines; draw: root draw
 *  status: drawn at 8 decided children  -> depth 2 block/over/legal
 *  play settles only the bottom block   -> undo: X's move decides ... root;
 *                                          depth 5 adopt reproduces
 *  over not mirrored from the root      -> depth 2 over; depth 5 game ends
 *  play does not flip turn              -> depth 2 turn (each direction
 *                                          flips it); undo struct before
 *  play does not count n_plies          -> depth 2 n_plies; depth 5 counts
 *  region: target = last / 9            -> depth 2 region; every relax case
 *  region: the decided node, not its    -> every relax case but "nothing
 *          parent                          decided"; closed d2 anywhere
 *  region: only the block is asked      -> relax d3/d5 ancestor cases (depth
 *                                          2 cannot see it: its only
 *                                          ancestor is the root)
 *  legal walk keeps decided subtrees    -> closed: lists nothing under a
 *                                          decided node; depth 5 no
 *                                          decided ancestor
 *  legal_at forgets "closed"            -> closed: not legal / play refuses;
 *                                          depth 2 and 5 membership
 *  legal writes cap + 1                 -> legal: only cap written
 *  closed asks only the node itself     -> closed: utb_closed is the node
 *                                          or any node above it
 *  undo does not resettle the path      -> undo: every status open again;
 *                                          depth 2/5 play then undo
 *  undo ignores an unknown prev         -> undo: second undo refused;
 *                                          refused straight after adopt
 *  adopt: cell 3 skipped, not refused   -> adopt refuses a cell value over 2
 *  adopt: counts unchecked              -> adopt refuses O ahead of X, X two
 *                                          ahead of O
 *  adopt: both lines read as X          -> adopt refuses ... X line and O
 *                                          line (block and level-1 node)
 *  adopt: last on an empty board        -> adopt refuses a last move on an
 *                                          empty board
 *  adopt: UTB_NONE on a non-empty board -> adopt refuses no last move ...
 *  adopt: no upper bound on last        -> adopt refuses ... off the board
 *  adopt: last's mark unchecked         -> adopt refuses ... empty cell /
 *                                          the mark that did not just move
 *  adopt: turn is the mover             -> adopt: the empty board, X then O;
 *                                          depth 5 adopt reproduces
 *  adopt: prev left UTB_NONE            -> undo: refused straight after adopt
 *  adopt writes n_plies before refusing -> adopt refuses ... struct untouched
 *  level boundary `>` for `>=`          -> tree: round-trips, boundaries
 *  ancestor divides by 9^level          -> tree: round-trips, ancestor of
 *                                          the last leaf
 *  count skips the last leaf            -> depth 5: utb_count is the plies
 *  rect: row and column swapped         -> depth 2 cell_rect and hit;
 *                                          geometry third, hit, tiling
 *  rect: width 1 / N                    -> geometry: neighbours share an
 *                                          edge exactly; grid lines
 *  hit: no nudge after the floor        -> the same two
 *  hit: no far-edge clamp               -> geometry corners; depth 2 hit
 *  hit: no range check                  -> geometry off the square; depth 2
 *
 * THE SEND RULES (2026-10-03), each broken in a copy and restored:
 *  climbs read as rule A (k = 2 always) -> rules: every k (region and the
 *                                          9 / 81 / 729 / 6,561 counts)
 *  k = 1 sends anywhere outright        -> depth 2: B' and B are the shipped
 *                                          game (the clause that keeps them so)
 *  k = 1 read as k = 2 (A's formula)    -> rules: k = 1 anywhere, and its count
 *  B read as B'                         -> rules: k = 4, 3, 2 regions, counts
 *  B keeps the wrong digit              -> rules: k = 4, 3, 2; B relax cases
 *  no relaxation above the bottom level -> rules relax: B's 9 x 9 decided;
 *                                          depth 5 rule B never ends
 *  k is the deepest decided, not the    -> rules: k = 3, k = 2
 *          largest
 *  init_rule ignores the rule           -> rules: init_rule sets the rule;
 *                                          depth 5 B'/B adopt reproduces
 *  init_rule takes any rule             -> rules: not one is refused
 *  undo clears the rule                 -> rules: undo under B'; depth 5
 *                                          B'/B play then undo, every ply
 *
 * The depth-2 lockstep goes red on many names at once for a status or
 * region mutation: one wrong ply and the two games part.
 */
#include "../src/uttt_big.h"
#include "../src/uttt_draw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static float frnd(void) { return (float)(rnd() & 0xffffff) / (float)0x1000000; }

/* Big enough for every legal move at depth 5, so off the stack. */
static int32_t LIST[UTB_LEAVES_MAX];
static UtbGame G, H, K;

/* Elapsed seconds for the timings this test prints: clock() is standard C,
 * where clock_gettime is POSIX and glibc hides it under -std=c11. */
static double now(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

static int ipow9(int e) { int r = 1; while (e-- > 0) r *= 9; return r; }

static int in_list(const int32_t *l, int n, int mv)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (l[m] < mv) lo = m + 1; else hi = m;
    }
    return lo < n && l[lo] == mv;
}

/* ------------------------------------------------------------ the tree */

static void test_tree(void)
{
    OK(utb_leaves(2) == 81 && utb_leaves(5) == 59049, "tree: leaves are 9^depth");
    OK(utb_nodes(2) == 10 && utb_nodes(3) == 91 && utb_nodes(5) == 7381, "tree: nodes are (9^depth-1)/8");
    OK(utb_leaves(1) == 0 && utb_leaves(6) == 0 && utb_nodes(1) == 0 && utb_nodes(6) == 0,
       "tree: no depth outside 2..5");

    memset(&H, 0xAB, sizeof H);
    K = H;
    OK(!utb_init(&H, 1) && !utb_init(&H, 6) && !memcmp(&H, &K, sizeof H),
       "init: a depth outside 2..5 is refused and the struct untouched");

    utb_init(&G, 5);
    LIST[5] = -99;
    OK(utb_legal(&G, LIST, 5) == UTB_LEAVES_MAX && LIST[4] == 4 && LIST[5] == -99,
       "legal: the count is every move, only cap of them written");
    int ok = 1;
    for (int id = 0; id < utb_nodes(5); id++) {
        int L = utb_node_level(&G, id), p = utb_node_prefix(&G, id);
        int first = utb_node_first(&G, id), span = utb_node_span(&G, id);
        if (utb_node_id(&G, L, p) != id) ok = 0;
        if (span != utb_leaves(5) / ipow9(L)) ok = 0;
        if (first != p * span) ok = 0;
        /* every leaf it spans names it as its level-L ancestor */
        if (utb_ancestor(&G, first, L) != id || utb_ancestor(&G, first + span - 1, L) != id) ok = 0;
        if (L > 0 && first > 0 && utb_ancestor(&G, first - 1, L) == id) ok = 0;
    }
    OK(ok, "tree: id <-> (level, prefix) <-> span round-trips for all 7381 nodes");
    OK(utb_node_level(&G, 0) == 0 && utb_node_level(&G, 1) == 1 && utb_node_level(&G, 9) == 1
       && utb_node_level(&G, 10) == 2 && utb_node_level(&G, 820) == 4 && utb_node_level(&G, 7380) == 4,
       "tree: level boundaries are 1, 10, 91, 820");
    OK(utb_node_level(&G, -1) == -1 && utb_node_level(&G, 7381) == -1 && utb_node_span(&G, 7381) == 0
       && utb_node_first(&G, -1) == -1 && utb_node(&G, 7381) == -1 && utb_node_id(&G, 5, 0) == -1
       && utb_node_id(&G, 1, 9) == -1 && utb_ancestor(&G, 59049, 1) == -1 && utb_ancestor(&G, 0, 5) == -1,
       "tree: off the tree is -1");
    OK(utb_ancestor(&G, 59048, 0) == 0 && utb_ancestor(&G, 59048, 4) == 7380, "tree: ancestor of the last leaf");
    UtbGame *d2 = &H;
    utb_init(d2, 2);
    OK(utb_node_level(d2, 10) == -1 && utb_ancestor(d2, 40, 1) == 1 + 4, "tree: depth 2 has ten nodes, block b is 1+b");
}

/* --------------------------------------------- depth 2 IS the shipped game */

static int region_as_active(int r)
{
    if (r < 0) return -1;
    if (r == UTB_ROOT) return 9;
    return r - 1;
}

static void same_as_uttt(const UtttGame *u, const UtbGame *b, int *bad)
{
    uint8_t ul[81];
    int un = uttt_legal(u, ul);
    int bn = utb_legal(b, LIST, UTB_LEAVES_MAX);
    int same = un == bn;
    for (int i = 0; same && i < un; i++) same = ul[i] == LIST[i];
    if (!same) bad[0]++;
    if (u->turn != b->turn) bad[1]++;
    if (u->over != b->over) bad[2]++;
    if (uttt_active(u) != region_as_active(utb_region(b))) bad[3]++;
    for (int i = 0; i < 81; i++) if (uttt_cell(u, i) != b->cell[i]) { bad[4]++; break; }
    for (int k = 0; k < 9; k++) if (uttt_block(u, k) != b->node[1 + k]) { bad[5]++; break; }
    if (u->n_plies != b->n_plies) bad[6]++;
    for (int mv = -1; mv <= 81; mv++)
        if (utb_legal_at(b, mv) != (mv >= 0 && mv < 81 && in_list(LIST, bn, mv))) { bad[7]++; break; }
}

/* The climbing rules, played beside G in the same random games: at depth 2 a
 * completed 3 x 3 is a child of the root, with no level to climb to, so the
 * cell's ordinary send stands and both ARE the shipped game (uttt_big.h). */
static UtbGame CL[2];

static void test_depth2(int games)
{
    int bad[10] = { 0 }, bad_cl[2][10] = { { 0 } };
    int won_x = 0, won_o = 0, drawn = 0, climbed = 0;
    for (int n = 0; n < games; n++) {
        UtttGame u;
        uttt_init(&u);
        utb_init(&G, 2);
        utb_init_rule(&CL[0], 2, UTB_RULE_CLIMB);
        utb_init_rule(&CL[1], 2, UTB_RULE_CLIMB_FREE);
        same_as_uttt(&u, &G, bad);
        while (!u.over) {
            uint8_t ul[81];
            int m = uttt_legal(&u, ul);
            int mv = ul[rnd() % (uint32_t)m];
            /* a move nobody may make is refused by both, and changes nothing */
            int no = (int)(rnd() % 81);
            if (!utb_legal_at(&G, no)) {
                H = G;
                UtttGame v = u;
                if (utb_play(&G, no) || uttt_play(&v, (uint8_t)no) || memcmp(&H, &G, sizeof G)) bad[8]++;
            }
            /* play, take back, play again: the take-back is exact, and the
             * replay sets prev from `last` again, so nothing is lost */
            H = G;
            if (!utb_play(&G, mv)) bad[9]++;
            if (G.turn == H.turn) bad[1]++;       /* each direction flips it */
            if (!utb_undo(&G)) bad[9]++;
            H.prev = UTB_UNKNOWN;
            if (memcmp(&H, &G, sizeof G)) bad[9]++;
            uttt_play(&u, (uint8_t)mv);
            if (!utb_play(&G, mv)) bad[9]++;
            same_as_uttt(&u, &G, bad);
            for (int r = 0; r < 2; r++) {
                if (!utb_play(&CL[r], mv)) bad_cl[r][9]++;
                same_as_uttt(&u, &CL[r], bad_cl[r]);
            }
            /* a move that decided its 3 x 3 while the game runs: the case
             * where a climb would have had somewhere to go */
            if (!u.over && G.node[1 + mv / 9] != UTTT_OPEN) climbed++;
        }
        if (u.over == UTTT_X) won_x++;
        else if (u.over == UTTT_O) won_o++;
        else drawn++;
    }
    OK(!bad[0], "depth 2: the legal list is uttt_legal's, same order, same count");
    OK(!bad[1], "depth 2: turn is uttt's");
    OK(!bad[2], "depth 2: over is uttt's");
    OK(!bad[3], "depth 2: utb_region is uttt_active (block b -> 1+b, anywhere -> UTB_ROOT)");
    OK(!bad[4], "depth 2: every cell is uttt_cell");
    OK(!bad[5], "depth 2: every block status is uttt_block");
    OK(!bad[6], "depth 2: n_plies is uttt's");
    OK(!bad[7], "depth 2: utb_legal_at is membership of utb_legal, for every leaf");
    OK(!bad[8], "depth 2: an illegal move is refused by both and changes nothing");
    OK(!bad[9], "depth 2: play then undo is the struct before, prev aside");
    OK(won_x && won_o && drawn, "depth 2: the random games include X wins, O wins and draws");
    printf("  depth 2: %d games (X %d, O %d, drawn %d)\n", games, won_x, won_o, drawn);
    for (int r = 0; r < 2; r++) {
        int any = 0;
        for (int i = 0; i < 10; i++) any |= bad_cl[r][i];
        OK(!any, r ? "depth 2: rule B (CLIMB_FREE) is the shipped game move for move: legal list, region, turn, cells, blocks"
                   : "depth 2: rule B' (CLIMB) is the shipped game move for move: legal list, region, turn, cells, blocks");
    }
    OK(climbed > games, "depth 2: the games complete 3 x 3s mid-game, so the rules had the chance to part");

    /* the geometry, against the shipped board's */
    utb_init(&G, 2);
    int geo = 1, hit = 1;
    for (int mv = 0; mv < 81; mv++) {
        float a[4], b[4];
        utb_cell_rect(&G, mv, a);
        uttt_cell_rect(mv, b);
        for (int k = 0; k < 4; k++) if (fabsf(a[k] - b[k]) > 1e-6f) geo = 0;
        if (utb_hit(&G, a[0] + a[2] / 2, a[1] + a[3] / 2) != mv) hit = 0;
    }
    OK(geo, "depth 2: utb_cell_rect is uttt_cell_rect for every cell");
    for (int i = 0; i < 20000; i++) {
        float x = frnd() * 1.2f - 0.1f, y = frnd() * 1.2f - 0.1f;
        if (utb_hit(&G, x, y) != uttt_hit(x, y)) hit = 0;
    }
    float edges[] = { 0.f, 1.f / 3.f, 1.f / 9.f, 2.f / 3.f, 1.f, -0.f };
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++)
            if (utb_hit(&G, edges[i], edges[j]) != uttt_hit(edges[i], edges[j])) hit = 0;
    OK(hit, "depth 2: utb_hit is uttt_hit at every cell centre, 20,000 random points and the grid lines");
}

/* ------------------------------------------- depth 5, played to the end */

static int same_position(const UtbGame *a, const UtbGame *b)
{
    return !memcmp(a->node, b->node, sizeof a->node) && !memcmp(a->cell, b->cell, sizeof a->cell)
        && a->turn == b->turn && a->over == b->over && a->n_plies == b->n_plies
        && a->last == b->last && utb_region(a) == utb_region(b);
}

/* Under rule A (every 97th ply taken back, as it always was) or a climbing
 * rule (every ply taken back: the climbs read the board for k, so undo must
 * restore exactly the state the region is read from). */
static void test_depth5(int games, int rule)
{
    static const char *const NAME[3] = { "A", "B'", "B" };
    const int every = rule == UTB_RULE_SHIFT ? 97 : 1;
    char what[160];
#define OKR(c, msg) do { if (rule == UTB_RULE_SHIFT) OK(c, msg); \
        else { snprintf(what, sizeof what, "%s (rule %s)", msg, NAME[rule]); OK(c, what); } } while (0)
    int bad[8] = { 0 };
    double total = 0;
    for (int n = 0; n < games; n++) {
        double t0 = now();
        utb_init_rule(&G, 5, rule);
        int plies = 0;
        for (;;) {
            int m = utb_legal(&G, LIST, UTB_LEAVES_MAX);
            if (G.over) { if (m) bad[0]++; break; }
            if (m <= 0) { bad[0]++; break; }
            for (int i = 1; i < m; i++) if (LIST[i] <= LIST[i - 1]) { bad[1]++; break; }
            for (int i = 0; i < 20; i++) {
                int leaf = (int)(rnd() % UTB_LEAVES_MAX);
                if (utb_legal_at(&G, leaf) != in_list(LIST, m, leaf)) bad[2]++;
            }
            int mv = LIST[rnd() % (uint32_t)m];
            int r = utb_region(&G);
            int first = utb_node_first(&G, r), span = utb_node_span(&G, r);
            if (mv < first || mv >= first + span) bad[3]++;
            if (utb_closed(&G, utb_ancestor(&G, mv, 4))) bad[4]++;
            if (plies % every == 0) {
                H = G;
                if (!utb_play(&G, mv) || !utb_undo(&G)) bad[6]++;
                H.prev = UTB_UNKNOWN;
                if (memcmp(&H, &G, sizeof G)) bad[6]++;
            }
            if (!utb_play(&G, mv)) { bad[0]++; break; }
            plies++;
            if (plies % 97 == 0 || G.over) {
                /* a picture carries no rule: the adopter sets it */
                int took = utb_adopt(&K, 5, G.cell, G.last);
                K.rule = (uint8_t)rule;
                if (!took || !same_position(&K, &G)) bad[5]++;
            }
        }
        double dt = now() - t0;
        total += dt;
        printf("  depth 5 rule %s: game %d, %d plies, result %c, %.3f s\n", NAME[rule], n, plies,
               G.over == UTTT_X ? 'X' : G.over == UTTT_O ? 'O' : 'D', dt);
        OKR(G.over != 0 && G.n_plies == plies, "depth 5: the game ends and counts its plies");
        int c[3];
        utb_count(&G, c);
        OKR(c[1] + c[2] == plies && c[0] + c[1] + c[2] == UTB_LEAVES_MAX && (c[1] == c[2] || c[1] == c[2] + 1),
           "depth 5: utb_count is the plies, X first");
    }
    OKR(!bad[0], "depth 5: a running game always has a legal move and a finished one none");
    OKR(!bad[1], "depth 5: the legal list is ascending");
    OKR(!bad[2], "depth 5: utb_legal_at is membership of utb_legal for 20 random leaves a ply");
    OKR(!bad[3], "depth 5: every legal move lies in utb_region's span");
    OKR(!bad[4], "depth 5: no legal move has a decided ancestor");
    OKR(!bad[5], "depth 5: utb_adopt of the cells and last reproduces the position every 97th ply and at the end");
    OKR(!bad[6], rule == UTB_RULE_SHIFT ? "depth 5: play then undo is the struct before, prev aside"
                             : "depth 5: play then undo is the struct before, prev aside, at every ply");
    if (games) printf("  depth 5 rule %s: %.3f s a game on average\n", NAME[rule], total / games);
#undef OKR
}

/* -------------------------------------------- hand-built positions */

static uint8_t CELLS[UTB_LEAVES_MAX];

/* Give `mark` the node (level L, prefix p) by winning its children in row
 * `row` (0 top, 1 middle), each of those by its top row, down to the cells.
 * Returns how many marks it laid. */
static int win(int depth, int L, int p, int row, uint8_t mark)
{
    if (L == depth) { CELLS[p] = mark; return 1; }
    int n = 0;
    for (int k = 0; k < 3; k++) n += win(depth, L + 1, p * 9 + row * 3 + k, 0, mark);
    return n;
}

/* Lay `n` O marks, one in the centre of each bottom block of top-level
 * subtree 8, which nothing else in these positions touches: one mark never
 * decides a block. */
static void sprinkle_o(int depth, int n)
{
    int first_block = 8 * ipow9(depth - 2);
    for (int i = 0; i < n; i++) CELLS[(first_block + i) * 9 + 4] = UTTT_O;
}

/* The position: the X structures already laid, then O's `last` at the leaf
 * under top-level subtree 7 whose remaining digits are `target` (so the raw
 * target is the bottom block `target`), and O's to balance. */
static int build(UtbGame *g, int depth, int nx, int target)
{
    int last = 7 * ipow9(depth - 1) + target;
    CELLS[last] = UTTT_O;
    sprinkle_o(depth, nx - 1);
    return utb_adopt(g, depth, CELLS, last);
}

static void test_relaxation(void)
{
    /* DEPTH 3: the raw target is level-2 block (4,0), prefix 36, id 10+36. */
    int t = 4 * 9 + 0;
    memset(CELLS, 0, sizeof CELLS);
    CELLS[6 * 81] = UTTT_X;            /* one X, in a subtree nothing reads */
    OK(build(&G, 3, 1, t) && utb_region(&G) == 10 + t, "relax d3: nothing decided -> the block itself");

    memset(CELLS, 0, sizeof CELLS);
    int nx = win(3, 2, t, 0, UTTT_X);
    OK(build(&G, 3, nx, t) && utb_node(&G, 10 + t) == UTTT_X, "relax d3: the block is won");
    OK(utb_region(&G) == 1 + 4, "relax d3: target decided -> its parent");

    memset(CELLS, 0, sizeof CELLS);
    nx = win(3, 1, 4, 0, UTTT_X);
    OK(build(&G, 3, nx, t) && utb_node(&G, 1 + 4) == UTTT_X, "relax d3: level-1 node 4 is won");
    OK(utb_region(&G) == UTB_ROOT, "relax d3: target and parent decided -> the root");

    memset(CELLS, 0, sizeof CELLS);
    nx = win(3, 1, 4, 1, UTTT_X);     /* by its middle row: block (4,0) stays open */
    OK(build(&G, 3, nx, t) && utb_node(&G, 10 + t) == UTTT_OPEN && utb_node(&G, 5) == UTTT_X,
       "relax d3: block open under a won level-1 node");
    OK(utb_region(&G) == UTB_ROOT, "relax d3: an open block under a decided ancestor -> that ancestor's parent");

    /* DEPTH 5: the raw target is (4,0,0,0), prefix 2916, id 820+2916. */
    const int T = 4 * 729, B = 820 + T, P3 = 91 + T / 9, P2 = 10 + T / 81, P1 = 1 + 4;
    memset(CELLS, 0, sizeof CELLS);
    CELLS[6 * 6561] = UTTT_X;
    OK(build(&G, 5, 1, T) && utb_region(&G) == B, "relax d5: nothing decided -> the block itself");

    memset(CELLS, 0, sizeof CELLS);
    nx = win(5, 4, T, 0, UTTT_X);
    OK(build(&G, 5, nx, T) && utb_region(&G) == P3, "relax d5: block decided -> its parent");

    memset(CELLS, 0, sizeof CELLS);
    nx = win(5, 3, T / 9, 0, UTTT_X);
    OK(build(&G, 5, nx, T) && utb_node(&G, P3) == UTTT_X && utb_region(&G) == P2,
       "relax d5: block and parent decided -> the grandparent");

    memset(CELLS, 0, sizeof CELLS);
    nx = win(5, 2, T / 81, 0, UTTT_X);
    OK(build(&G, 5, nx, T) && utb_region(&G) == P1, "relax d5: decided up to level 2 -> level 1");

    memset(CELLS, 0, sizeof CELLS);
    nx = win(5, 1, 4, 0, UTTT_X);
    OK(build(&G, 5, nx, T) && utb_node(&G, P1) == UTTT_X && utb_region(&G) == UTB_ROOT,
       "relax d5: decided up to level 1 -> UTB_ROOT");

    memset(CELLS, 0, sizeof CELLS);
    nx = win(5, 2, T / 81, 1, UTTT_X);   /* (4,0) won by its middle row */
    OK(build(&G, 5, nx, T) && utb_node(&G, B) == UTTT_OPEN && utb_node(&G, P3) == UTTT_OPEN
       && utb_node(&G, P2) == UTTT_X, "relax d5: (4,0) won while (4,0,0) and its block are open");
    OK(utb_region(&G) == P1, "relax d5: an open block under a decided level-2 ancestor -> level 1");

    /* CLOSED FOREVER, in that last position: region level-1 node 4 spans the
     * empty leaves of (4,0,0,0), and none of them may be played. */
    int closed_leaf = T * 9 + 4;
    OK(G.cell[closed_leaf] == UTTT_OPEN && !utb_legal_at(&G, closed_leaf),
       "closed: an empty leaf under a decided node is not legal though the region spans it");
    H = G;
    OK(!utb_play(&G, closed_leaf) && !memcmp(&H, &G, sizeof G), "closed: utb_play refuses it, nothing changes");
    int m = utb_legal(&G, LIST, UTB_LEAVES_MAX);
    int any_closed = 0, any_open_sibling = 0;
    for (int i = 0; i < m; i++) {
        if (LIST[i] / 729 == 4 * 9 + 0) any_closed = 1;   /* under (4,0) */
        if (LIST[i] / 729 == 4 * 9 + 1) any_open_sibling = 1;
    }
    OK(!any_closed && !in_list(LIST, m, closed_leaf), "closed: utb_legal lists nothing under a decided node");
    OK(any_open_sibling && m == 8 * 729, "closed: utb_legal lists all of the region's open subtrees");
    OK(utb_closed(&G, B) && utb_closed(&G, P3) && utb_closed(&G, P2) && !utb_closed(&G, P1)
       && !utb_closed(&G, 0) && utb_closed(&G, -1), "closed: utb_closed is the node or any node above it");

    /* and at depth 2 through play: a won block is closed even when anywhere */
    utb_init(&G, 2);
    /* X takes block 4's top row while O answers in the centres of 0 and 1 */
    const int moves[] = { 4 * 9 + 0, 0 * 9 + 4, 4 * 9 + 1, 1 * 9 + 4, 4 * 9 + 2 };
    int played = 1;
    for (int i = 0; i < 5; i++) played &= utb_play(&G, moves[i]);
    OK(played && utb_node(&G, 1 + 4) == UTTT_X, "closed d2: X takes block 4 by its top row");
    /* O must play block 2; then X is sent to block 4 by O's cell 4 */
    OK(utb_play(&G, 2 * 9 + 4) && utb_region(&G) == UTB_ROOT, "closed d2: sent to a won block -> anywhere");
    OK(!utb_legal_at(&G, 4 * 9 + 8) && !utb_play(&G, 4 * 9 + 8) && G.cell[4 * 9 + 8] == UTTT_OPEN,
       "closed d2: an empty cell of the won block is refused though the region is anywhere");
}

/* ------------------------------------------------- the climbing rules */

/* The leaf (or, with fewer digits, the node prefix) d1 d2 ... */
static int digits(const int *d, int n)
{
    int p = 0;
    for (int i = 0; i < n; i++) p = p * 9 + d[i];
    return p;
}

/* Give `mark` the level-L ancestor of leaf `last` (depth 5) THROUGH last's
 * cell: at each level the row holding last's digit, the off-path children won
 * by win(). So the last cell is one of the marks that decided it. */
static void win_via(int L, int p, int last, uint8_t mark)
{
    if (L == 5) { CELLS[p] = mark; return; }
    int path = last / ipow9(5 - L - 1), row = (path % 9) / 3;
    for (int k = 0; k < 3; k++) {
        int c = p * 9 + row * 3 + k;
        if (c == path) win_via(L + 1, c, last, mark);
        else win(5, L + 1, c, 0, mark);
    }
}

/* The board in CELLS, O's `last` on it, X and O balanced by single marks in
 * top-level subtree 8 (X in block centres, O in block corners: two marks
 * never decide a block), adopted, under `rule`. */
static int position(int last, int rule)
{
    int n[3] = { 0, 0, 0 };
    CELLS[last] = UTTT_O;
    for (int i = 0; i < UTB_LEAVES_MAX; i++) n[CELLS[i]]++;
    const int first_block = 8 * 729;
    for (int i = 0; n[1] < n[2]; i++, n[1]++) CELLS[(first_block + i) * 9 + 4] = UTTT_X;
    for (int i = 0; n[2] < n[1]; i++, n[2]++) CELLS[(first_block + i) * 9 + 0] = UTTT_O;
    if (!utb_adopt(&G, 5, CELLS, last)) return 0;
    G.rule = (uint8_t)rule;
    return 1;
}

static void test_rules(void)
{
    /* the last move, O's: digits 6 1 3 5 7, all different, so a wrong digit
     * dropped names another block */
    static const int D[5] = { 6, 1, 3, 5, 7 };
    const int last = digits(D, 5);
    const int A_target = 820 + digits((const int[]){ 1, 3, 5, 7 }, 4);
    const int OFFS[5] = { 0, 1, 10, 91, 820 };
    #define NODE(L, ...) (OFFS[L] + digits((const int[]){ __VA_ARGS__ }, L))

    OK(utb_init_rule(&H, 5, UTB_RULE_CLIMB) && H.rule == UTB_RULE_CLIMB && utb_region(&H) == UTB_ROOT,
       "rules: init_rule sets the rule; the first move is anywhere");
    K = H;
    OK(!utb_init_rule(&H, 5, 3) && !utb_init_rule(&H, 5, -1) && !memcmp(&H, &K, sizeof H),
       "rules: a rule that is not one is refused, the struct untouched");
    utb_init(&H, 5);
    OK(H.rule == UTB_RULE_SHIFT, "rules: utb_init is rule A");

    /* each k: the unit laid, then the region under A, B' and B, and B's
     * free choice against B''s 3 x 3 */
    struct { int k; int climb, free_; int free_n; const char *what; } C[] = {
        { 5, NODE(4, 6, 1, 3, 7), NODE(4, 6, 1, 3, 7), 9,
          "k = 5, an ordinary move: B' and B stay in the 9 x 9, the 3 x 3 named by d5 (6 1 3 7)" },
        { 4, NODE(4, 6, 1, 5, 7), NODE(3, 6, 1, 5), 81,
          "k = 4, a 3 x 3 completed: B' to the 9 x 9 named by d4 in the same 27 x 27, its 3 x 3 d5 (6 1 5 7); B the whole 9 x 9 (6 1 5)" },
        { 3, NODE(4, 6, 3, 5, 7), NODE(2, 6, 3), 729,
          "k = 3, a 9 x 9 completed: B' to (6 3 5 7); B the whole 27 x 27 (6 3)" },
        { 2, NODE(4, 1, 3, 5, 7), NODE(1, 1), 6561,
          "k = 2, a 27 x 27 completed: B' is rule A's (1 3 5 7); B the whole 81 x 81 (1)" },
        { 1, UTB_ROOT, UTB_ROOT, -1,
          "k = 1, an 81 x 81 completed: B' and B anywhere (no level to climb to; the cell's send lies in the decided 81 x 81)" },
    };
    for (int i = 0; i < 5; i++) {
        memset(CELLS, 0, sizeof CELLS);
        if (C[i].k < 5) win_via(C[i].k, last / ipow9(5 - C[i].k), last, UTTT_O);
        int ok = position(last, UTB_RULE_SHIFT);
        /* the move completed exactly levels k..4 */
        for (int L = 1; L < 5; L++)
            ok &= (utb_node(&G, utb_ancestor(&G, last, L)) != UTTT_OPEN) == (L >= C[i].k);
        int a = utb_region(&G);
        G.rule = UTB_RULE_CLIMB;
        int b1 = utb_region(&G), n1 = utb_legal(&G, LIST, UTB_LEAVES_MAX);
        G.rule = UTB_RULE_CLIMB_FREE;
        int b = utb_region(&G), n = utb_legal(&G, LIST, UTB_LEAVES_MAX);
        char what[256];
        snprintf(what, sizeof what, "rules: %s", C[i].what);
        OK(ok && a == A_target && b1 == C[i].climb && b == C[i].free_, what);
        if (C[i].free_n > 0) {
            snprintf(what, sizeof what, "rules k = %d: B' offers 9 moves, B %d", C[i].k, C[i].free_n);
            OK(n1 == 9 && n == C[i].free_n, what);
        } else {
            OK(n1 == n && n1 == UTB_LEAVES_MAX - 6561 - 81, "rules k = 1: anywhere outside the decided 81 x 81 and the balancing marks");
        }
    }

    /* RELAXATION under the climbs: a 3 x 3 completed (k = 4) whose B' target
     * (6 1 5 7) is already X's -> its 9 x 9 (6 1 5); that 9 x 9 X's as well
     * -> the 27 x 27 (6 1), for both rules (B's target IS that 9 x 9). */
    memset(CELLS, 0, sizeof CELLS);
    win(5, 4, digits((const int[]){ 6, 1, 5, 7 }, 4), 0, UTTT_X);
    win_via(4, last / 9, last, UTTT_O);
    OK(position(last, UTB_RULE_CLIMB) && utb_region(&G) == NODE(3, 6, 1, 5),
       "rules relax: B''s target 3 x 3 decided -> its 9 x 9");
    G.rule = UTB_RULE_CLIMB_FREE;
    OK(utb_region(&G) == NODE(3, 6, 1, 5), "rules relax: the same 9 x 9 under B, whose target it is");
    memset(CELLS, 0, sizeof CELLS);
    win(5, 3, digits((const int[]){ 6, 1, 5 }, 3), 0, UTTT_X);
    win_via(4, last / 9, last, UTTT_O);
    OK(position(last, UTB_RULE_CLIMB) && utb_region(&G) == NODE(2, 6, 1),
       "rules relax: B''s target and its 9 x 9 decided -> the 27 x 27");
    G.rule = UTB_RULE_CLIMB_FREE;
    OK(utb_region(&G) == NODE(2, 6, 1), "rules relax: B's target 9 x 9 decided -> the 27 x 27");
    /* and the region is derived after undo: play an ordinary O move under
     * B', take it back, and the 3 x 3-completing region returns */
    memset(CELLS, 0, sizeof CELLS);
    win_via(4, last / 9, last, UTTT_O);
    position(last, UTB_RULE_CLIMB);
    int before = utb_region(&G);
    utb_legal(&G, LIST, UTB_LEAVES_MAX);
    OK(before == NODE(4, 6, 1, 5, 7) && utb_play(&G, LIST[0]) && utb_region(&G) != before
       && utb_undo(&G) && utb_region(&G) == before && G.rule == UTB_RULE_CLIMB,
       "rules: undo under B' brings back the region the completion set, rule kept");
    #undef NODE
}

/* ------------------------------------------------------------ undo */

static void test_undo(void)
{
    utb_init(&G, 3);
    H = G;
    OK(!utb_undo(&G) && !memcmp(&H, &G, sizeof G), "undo: an empty board is refused");

    OK(utb_play(&G, 100), "undo: first move");
    UtbGame one = G;
    OK(utb_play(&G, 100 % 81 * 9 + 3), "undo: second move");
    UtbGame two = G;
    OK(utb_undo(&G), "undo: the last move comes back");
    one.prev = UTB_UNKNOWN;
    OK(!memcmp(&one, &G, sizeof G), "undo: the struct is the one before, prev unknown");
    H = G;
    OK(!utb_undo(&G) && !memcmp(&H, &G, sizeof G), "undo: a second undo in a row is refused (prev unknown)");
    OK(utb_play(&G, 100 % 81 * 9 + 3), "undo: replay the move");
    OK(!memcmp(&two, &G, sizeof G) && G.prev == 100, "undo: play, undo, play is the first play's struct, prev too");

    /* a move that decides nodes all the way up, taken back */
    memset(CELLS, 0, sizeof CELLS);
    int nx = win(3, 0, 0, 0, UTTT_X);         /* X wins the whole game */
    int lastx = 2 * 81 + 2 * 9 + 2;           /* (2,2,2): the last cell of the line */
    CELLS[lastx] = UTTT_OPEN;
    nx--;
    /* as many O's, in subtree 8, at cells 0, 1, 5 of a block: no line */
    const int no_line[3] = { 0, 1, 5 };
    for (int i = 0; i < nx; i++) CELLS[(8 * 9 + i % 9) * 9 + no_line[i / 9]] = UTTT_O;
    int lasto = (8 * 9 + 0) * 9 + 0;
    OK(utb_adopt(&G, 3, CELLS, lasto), "undo: adopt a position one move from X's win");
    /* O's last (8,0,0) sends X to block (0,0), under the won level-1 node 0: anywhere */
    OK(utb_region(&G) == UTB_ROOT && utb_legal_at(&G, lastx), "undo: the winning cell is legal");
    OK(!utb_undo(&G), "undo: refused straight after adopt");
    H = G;
    OK(utb_play(&G, lastx) && G.over == UTTT_X && utb_node(&G, 1 + 2) == UTTT_X && utb_node(&G, 10 + 20) == UTTT_X,
       "undo: X's move decides block, level 1 and root");
    OK(utb_undo(&G), "undo: the winning move is taken back");
    H.prev = UTB_UNKNOWN;
    OK(!memcmp(&H, &G, sizeof G), "undo: every status on the path is open again, over 0");
}

/* ------------------------------------------------------------ adopt */

static void refused(int depth, int last, const char *what)
{
    /* a real position in the struct, so "untouched" means something */
    utb_init(&G, 5);
    utb_play(&G, 123);
    utb_play(&G, 4567);
    H = G;
    int r = utb_adopt(&G, depth, CELLS, last);
    char name[160];
    snprintf(name, sizeof name, "adopt refuses %s, struct untouched", what);
    OK(!r && !memcmp(&H, &G, sizeof G), name);
}

static void test_adopt(void)
{
    memset(CELLS, 0, sizeof CELLS);
    refused(1, UTB_NONE, "depth 1");
    refused(6, UTB_NONE, "depth 6");
    OK(utb_adopt(&G, 2, CELLS, UTB_NONE) && G.n_plies == 0 && G.turn == UTTT_X && G.last == UTB_NONE
       && G.prev == UTB_UNKNOWN && utb_region(&G) == UTB_ROOT, "adopt: the empty board");
    refused(2, 0, "a last move on an empty board");

    CELLS[10] = 3;
    refused(2, UTB_NONE, "a cell value over 2");
    CELLS[10] = 0;

    /* Counts no turn order gives, each with `last` holding the mark the
     * parity says just moved, so only the count rule can refuse them. */
    CELLS[10] = UTTT_O; CELLS[11] = UTTT_O; CELLS[12] = UTTT_X;
    refused(2, 12, "O ahead of X");
    CELLS[10] = UTTT_X; CELLS[11] = 0; CELLS[12] = 0;
    CELLS[20] = UTTT_X; CELLS[30] = UTTT_X; CELLS[40] = UTTT_O;
    refused(2, 40, "X two ahead of O");
    CELLS[20] = CELLS[30] = CELLS[40] = 0;
    /* one X at 10 */
    refused(2, UTB_NONE, "no last move on a non-empty board");
    CELLS[81] = UTTT_X;   /* past the board, holding the mover's mark */
    refused(2, 81, "a last move off the board (81)");
    CELLS[81] = 0;
    refused(2, UTB_UNKNOWN, "a last move of UTB_UNKNOWN");
    refused(2, -7, "a negative last move");
    refused(2, 11, "a last move on an empty cell");
    CELLS[11] = UTTT_O;
    refused(2, 10, "a last move holding the mark that did not just move");
    OK(utb_adopt(&G, 2, CELLS, 11) && G.turn == UTTT_X && G.n_plies == 2, "adopt: X then O, X to move");
    CELLS[11] = 0;

    /* both lines in block 0: X 0,1,2 and O 3,4,5 */
    memset(CELLS, 0, sizeof CELLS);
    CELLS[0] = CELLS[1] = CELLS[2] = UTTT_X;
    CELLS[3] = CELLS[4] = CELLS[5] = UTTT_O;
    refused(2, 5, "a block with an X line and an O line");
    /* and a node above the leaves: level-1 node 0 at depth 3 with both */
    memset(CELLS, 0, sizeof CELLS);
    int n = win(3, 1, 0, 0, UTTT_X);
    n -= win(3, 1, 0, 1, UTTT_O);
    OK(n == 0, "adopt: (nine X, nine O built)");
    refused(3, 27, "a level-1 node with an X line and an O line");
}

/* ------------------------------------------------------- win and draw */

static void test_draw(void)
{
    /* DEPTH-3 ROOT DRAW: the nine level-1 nodes  X O X / X O O / O X X  - no
     * line for either - each won by its top row of blocks. */
    const uint8_t who[9] = { UTTT_X, UTTT_O, UTTT_X, UTTT_X, UTTT_O, UTTT_O, UTTT_O, UTTT_X, UTTT_X };
    memset(CELLS, 0, sizeof CELLS);
    int nx = 0, no = 0;
    for (int k = 0; k < 9; k++) {
        int laid = win(3, 1, k, 0, who[k]);
        if (who[k] == UTTT_X) nx += laid; else no += laid;
    }
    /* nine more O's, one in block 8 of each level-1 node: one mark decides nothing */
    for (int k = 0; k < 9; k++) CELLS[k * 81 + 8 * 9 + 4] = UTTT_O;
    no += 9;
    OK(nx == 45 && no == 45, "draw: 45 X, 45 O");
    OK(utb_adopt(&G, 3, CELLS, 8 * 81 + 8 * 9 + 4), "draw: adopted");
    int all = 1;
    for (int k = 0; k < 9; k++) all &= utb_node(&G, 1 + k) == who[k];
    OK(all, "draw: every level-1 node is decided as built");
    OK(G.over == UTTT_DRAW && utb_node(&G, 0) == UTTT_DRAW, "draw: nine decided children and no line is a draw");
    OK(utb_region(&G) == -1 && utb_legal(&G, LIST, UTB_LEAVES_MAX) == 0 && !utb_play(&G, 9 * 0 + 5),
       "draw: a finished game has no region and no move");

    /* and a root win by a line of level-1 nodes, which play reaches by the same rule */
    memset(CELLS, 0, sizeof CELLS);
    nx = win(3, 0, 0, 2, UTTT_X);   /* bottom row of level-1 nodes */
    for (int i = 0; i < nx; i++) CELLS[i * 9 + 4] = UTTT_O;   /* blocks 0..26, one O each */
    OK(utb_adopt(&G, 3, CELLS, 0 * 9 + 4) && G.over == UTTT_X, "win: a line of level-1 nodes is X's game");
}

/* ------------------------------------------------------------ geometry */

static void test_geometry(void)
{
    utb_init(&G, 5);
    float r[4], c[4];
    OK(utb_node_rect(&G, 0, r) && r[0] == 0.f && r[1] == 0.f && r[2] == 1.f && r[3] == 1.f,
       "geometry: the root is the whole square");
    int third = 1;
    for (int id = 1; id < utb_nodes(5); id++) {
        int L = utb_node_level(&G, id), p = utb_node_prefix(&G, id);
        utb_node_rect(&G, utb_node_id(&G, L - 1, p / 9), c);
        utb_node_rect(&G, id, r);
        /* digit d is column d % 3, row d / 3 - integer arithmetic on purpose,
         * then floats */
        int d = p % 9;
        float col = (float)(d % 3), row = (float)(d / 3);
        if (fabsf(r[2] - c[2] / 3) > 1e-6f || fabsf(r[3] - c[3] / 3) > 1e-6f) third = 0;
        if (fabsf(r[0] - (c[0] + col * c[2] / 3)) > 1e-6f) third = 0;
        if (fabsf(r[1] - (c[1] + row * c[3] / 3)) > 1e-6f) third = 0;
    }
    OK(third, "geometry: a child's rect is its parent's third, row-major");
    double area = 0;
    int centre = 1, inside = 1, tile = 1;
    for (int mv = 0; mv < UTB_LEAVES_MAX; mv++) {
        utb_cell_rect(&G, mv, r);
        area += (double)r[2] * r[3];
        if (utb_hit(&G, r[0] + r[2] / 2, r[1] + r[3] / 2) != mv) centre = 0;
        utb_node_rect(&G, utb_ancestor(&G, mv, 4), c);
        if (r[0] < c[0] - 1e-6f || r[0] + r[2] > c[0] + c[2] + 1e-6f) inside = 0;
        /* the neighbours to the right and below start exactly where this ends */
        float e[4];
        if (r[0] + r[2] < 1.f && (!utb_cell_rect(&G, utb_hit(&G, r[0] + r[2], r[1]), e) || e[0] != r[0] + r[2]
                                  || e[1] != r[1])) tile = 0;
        if (r[1] + r[3] < 1.f && (!utb_cell_rect(&G, utb_hit(&G, r[0], r[1] + r[3]), e) || e[1] != r[1] + r[3]
                                  || e[0] != r[0])) tile = 0;
    }
    OK(fabs(area - 1.0) < 1e-4, "geometry: the 59,049 cell rects tile the square");
    OK(inside, "geometry: every cell lies in its block");
    OK(tile, "geometry: neighbouring cells share an edge exactly, no float gap or overlap");
    OK(centre, "geometry: utb_hit of every cell's centre is that cell");
    int pts = 1;
    for (int i = 0; i < 10000; i++) {
        float u = frnd(), v = frnd();
        int mv = utb_hit(&G, u, v);
        if (!utb_cell_rect(&G, mv, r) || u < r[0] || u >= r[0] + r[2] || v < r[1] || v >= r[1] + r[3]) pts = 0;
    }
    OK(pts, "geometry: 10,000 random points hit the cell whose rect holds them");
    OK(utb_hit(&G, 1.f, 1.f) == UTB_LEAVES_MAX - 1 && utb_hit(&G, 0.f, 0.f) == 0
       && utb_hit(&G, 1.f, 0.f) == 2 * (1 + 9 + 81 + 729 + 6561),
       "geometry: the corners, the far edge belonging to the last cell");
    /* THE GRID LINES: a point exactly on line k / 243 is the left (top) edge
     * of column k, so it is that column's; the float just below it is the
     * column before. This is where u * 243 rounds the wrong way. */
    int lines = 1;
    for (int k = 1; k < 243; k++) {
        float e = (float)k / 243.f, below = nextafterf(e, 0.f);
        int on = utb_hit(&G, e, e), under = utb_hit(&G, below, below);
        if (!utb_cell_rect(&G, on, r) || r[0] != e || r[1] != e) lines = 0;
        if (!utb_cell_rect(&G, under, c) || !(below >= c[0] && below < c[0] + c[2]) || c[0] >= e) lines = 0;
    }
    OK(lines, "geometry: on a grid line is the cell after it, a float below is the cell before");
    OK(utb_hit(&G, -0.001f, .5f) == -1 && utb_hit(&G, .5f, 1.001f) == -1 && utb_hit(&G, NAN, .5f) == -1,
       "geometry: off the square is -1");
    OK(!utb_cell_rect(&G, -1, r) && r[2] == 0.f && !utb_cell_rect(&G, UTB_LEAVES_MAX, r)
       && !utb_node_rect(&G, utb_nodes(5), r), "geometry: off the board is 0 and zeros");
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 2000;
    int big = argc > 2 ? atoi(argv[2]) : 3;
    printf("uttt_big: sizeof(UtbGame) = %zu bytes\n", sizeof(UtbGame));
    test_tree();
    test_depth2(games);
    test_relaxation();
    test_rules();
    test_undo();
    test_adopt();
    test_draw();
    test_geometry();
    test_depth5(big, UTB_RULE_SHIFT);
    test_depth5(big, UTB_RULE_CLIMB);
    test_depth5(big, UTB_RULE_CLIMB_FREE);
    printf("uttt_big: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

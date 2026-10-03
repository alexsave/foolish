/* The 243 page's kernel: uttt.live/243, two greedy bots (src/uttt_big_bot.h)
 * playing the depth-5 game in the visitor's browser.
 *
 * ONE GAME, ONE SEED. The page draws a 64-bit seed once (the browser's secure
 * random) and hands it here in two halves; both seats' bots are derived from
 * it (utb_bot_seat), so the seed IS the game: the same seed plays the same
 * moves on any machine, because nothing here reads a clock or a float.
 *
 * A GAME IS ITS SEED AND ITS SETTINGS, FROM THE FIRST MOVE. A change to the
 * bots or the send rule (ua_settings) starts the game again from move 0 on
 * the same seed, so no game is ever played at two settings and the three
 * settings on the page name the game that is on it.
 *
 * WHAT GOES OUT (uttt_243_web.h): the board as bytes - ua_grid is the board
 * as a picture, one byte a cell in row-major order, and ua_nodes every
 * internal node's status in uttt_big.h's numbering - and three structs the
 * page reads through structgen's generated readers: the game now
 * (ua_status), its shape and the bots' settings (ua_config), and where a
 * node sits (ua_box). The page holds no geometry and no layout of its own.
 *
 * NO LIBC, like uttt_web.c: -nostdlib -ffreestanding (Makefile, wasm-243),
 * the few calls from shared/c/wasm. Nothing allocates.
 */
#include "uttt_243_web.h"
#include "../src/uttt_big.h"
#include "../src/uttt_big_bot.h"

#define EXPORT(name) __attribute__((export_name(#name)))

#ifndef SG_LAYOUT_HASH
#define SG_LAYOUT_HASH 0u            /* not stamped: matches no generated module */
#endif

static UtbGame  g;
static UtbBot   bot[2];                      /* [0] plays X, [1] plays O     */
static uint8_t  grid[UTB_LEAVES_MAX];        /* the cells in picture order   */
static int32_t  at[UTB_LEAVES_MAX];          /* leaf -> its index in grid    */
static int      side;                        /* 3^depth: the grid's width    */
static int      depth_now;                   /* the game's depth (0: none)   */
static uint64_t seed_now;                    /* ...and its seed              */
static UaStatus status;
static UaConfig config;
static UaBox    box;

static UtbBot *seat(int s) { return &bot[s == UTTT_O]; }

_Static_assert(UA_PLIES_MAX == UTB_BOT_PLIES_MAX, "the page offers the plies the bot takes");
_Static_assert(UA_BUDGET_HUGE == UTB_BOT_BUDGET_MAX, "the largest budget is the bot's bound");
_Static_assert(UA_DEFAULT_PLIES == UTB_BOT_PLIES && UA_DEFAULT_BUDGET == UTB_BOT_BUDGET,
               "the page opens at the bot's defaults");
_Static_assert(UA_RULE_SHIFT == UTB_RULE_SHIFT && UA_RULE_CLIMB == UTB_RULE_CLIMB
               && UA_RULE_CLIMB_FREE == UTB_RULE_CLIMB_FREE, "the page's rules are the kernel's");

/* A node's square (or with leaf set, a cell's), from the kernel's geometry:
 * a rect's corner times the side is an exact grid line (utb_cell_rect divides
 * integers), so rounding only removes the float. Zeros off the board. */
static UaBox square(int id, int leaf)
{
    float r[4];
    if (!(leaf ? utb_cell_rect(&g, id, r) : utb_node_rect(&g, id, r))) return (UaBox){ 0, 0, 0 };
    const float n = (float)side;
    return (UaBox){ (int32_t)(r[0] * n + .5f), (int32_t)(r[1] * n + .5f), (int32_t)(r[2] * n + .5f) };
}

EXPORT(ua_layout_hash) uint32_t ua_layout_hash(void) { return (uint32_t)SG_LAYOUT_HASH; }

/* The empty board at `depth` under send rule `rule`, and both seats' bots
 * fresh from `seed` (their dice back at the start of their streams), at the
 * bot's defaults. */
static int begin(int depth, uint64_t seed, int rule)
{
    if (!utb_init_rule(&g, depth, rule)) return 0;
    depth_now = depth;
    seed_now  = seed;
    utb_bot_seat(&bot[0], seed, UTTT_X);
    utb_bot_seat(&bot[1], seed, UTTT_O);
    side = 1;
    for (int i = 0; i < depth; i++) side *= 3;
    for (int mv = 0; mv < utb_leaves(depth); mv++) {
        UaBox b = square(mv, 1);
        at[mv] = b.y * side + b.x;
        grid[mv] = UTTT_OPEN;
    }
    return 1;
}

/* A new game at `depth` (5 on the page; 2..5) from the seed hi:lo, at the
 * bot's defaults and rule A. 1, or 0 for a depth the kernel does not play. */
EXPORT(ua_start) int ua_start(int depth, uint32_t hi, uint32_t lo)
{
    return begin(depth, ((uint64_t)hi << 32) | lo, UA_DEFAULT_RULE);
}

/* THE CONTROL: both bots look `plies` ahead with `budget` work units a move
 * (clamped by utb_bot_set, which derives the candidate caps from the two),
 * the game plays send rule `rule` (one that is not a UA_RULE_ is rule A),
 * and THE GAME STARTS AGAIN: the board empty, move 0, the same seed, both
 * bots' dice back at the start. So the game on the page is exactly its seed
 * and these settings, and the same four play the same game whenever they
 * are set. Before any ua_start there is no game to restart, and nothing
 * happens. */
EXPORT(ua_settings) void ua_settings(int plies, int budget, int rule)
{
    if (rule < UA_RULE_SHIFT || rule > UA_RULE_CLIMB_FREE) rule = UA_DEFAULT_RULE;
    if (!depth_now || !begin(depth_now, seed_now, rule)) return;
    utb_bot_set(&bot[0], plies, budget);
    utb_bot_set(&bot[1], plies, budget);
}

/* The bot to move's choice, not played: -1 when the game is over. */
EXPORT(ua_think) int ua_think(void) { return utb_bot_move(seat(g.turn), &g); }

/* Play `mv` for the side to move: 1, or 0 when it is not legal. */
EXPORT(ua_play) int ua_play(int mv)
{
    if (!utb_play(&g, mv)) return 0;
    grid[at[mv]] = g.cell[mv];
    return 1;
}

/* Up to `n` moves, each the mover's bot's; returns how many were played. */
EXPORT(ua_step) int ua_step(int n)
{
    int k = 0;
    while (k < n && !g.over) {
        int mv = ua_think();
        if (mv < 0 || !ua_play(mv)) break;
        k++;
    }
    return k;
}

EXPORT(ua_legal_at) int ua_legal_at(int mv) { return utb_legal_at(&g, mv); }

/* The picture: side x side bytes, row-major, UA_OPEN / UA_X / UA_O. */
EXPORT(ua_grid)  const uint8_t *ua_grid(void)  { return grid; }
/* Every internal node's status (UA_OPEN, UA_X, UA_O, UA_DRAW), by node id. */
EXPORT(ua_nodes) const uint8_t *ua_nodes(void) { return g.node; }
/* The cells by leaf index, for a test that reads a move back. */
EXPORT(ua_cells) const uint8_t *ua_cells(void) { return g.cell; }

static UaSeat seat_now(int s)
{
    const UtbBot *b = seat(s);
    return (UaSeat){ b->value, b->depth, b->work, utb_bot_material(&g, s) };
}

/* The game now, as UaStatus. Filled on every call. */
EXPORT(ua_status) const UaStatus *ua_status(void)
{
    int r = utb_region(&g);
    status.plies      = g.n_plies;
    status.turn       = g.turn;
    status.over       = g.over;
    status.last       = g.last;
    status.region     = r;
    status.last_box   = g.last >= 0 ? square(g.last, 1) : (UaBox){ 0, 0, 0 };
    status.region_box = r >= 0 ? square(r, 0) : (UaBox){ 0, 0, 0 };
    status.x          = seat_now(UTTT_X);
    status.o          = seat_now(UTTT_O);
    return &status;
}

/* The game's shape and the bots' settings, as UaConfig. */
EXPORT(ua_config) const UaConfig *ua_config(void)
{
    config.depth    = g.depth;
    config.rule     = g.rule;
    config.side     = side;
    config.leaves   = utb_leaves(g.depth);
    config.nodes    = utb_nodes(g.depth);
    config.plies    = bot[0].plies;
    config.budget   = bot[0].budget;
    config.cap_root = bot[0].cap_root;
    config.cap_node = bot[0].cap_node;
    config.weight[0] = UTB_BOT_W3;
    config.weight[1] = UTB_BOT_W9;
    config.weight[2] = UTB_BOT_W27;
    config.weight[3] = UTB_BOT_W81;
    config.weight[4] = UTB_BOT_WGAME;
    /* a threat in each size of grid, read off the bot's own weights: the
     * node ids of the bottom 3 x 3 up to the root, one per level */
    for (int i = 0; i < 5; i++) {
        int level = g.depth - 1 - i;
        config.threat[i] = level >= 0 ? utb_bot_threat_weight(&g, utb_node_id(&g, level, 0)) : 0;
    }
    return &config;
}

/* Where node `id` sits, as a UaBox in cells (zeros off the tree), and its
 * level (0 the root, depth - 1 a 3 x 3; -1 off the tree). */
EXPORT(ua_box) const UaBox *ua_box(int id)
{
    box = square(id, 0);
    return &box;
}
EXPORT(ua_level) int ua_level(int id) { return utb_node_level(&g, id); }

// gd_bot_moscow - Moscow, the boss: foolish's cordite at two players, cut to
// fit the Game Boy (README.md "Moscow"). Once the stock and the face-up trump
// are gone it solves the endgame exactly (gd_solve): it takes a proven win,
// and drops the moves proven to lose when some move is proven not to. Then
// Monte Carlo: cordite's candidates (the most cards first among attacks,
// the cheapest covers and transfers, done, pick up) on worlds sampled from
// what it can see, played out by the rollout engine with common random
// numbers, in two stages (every candidate on a few worlds, then the best two
// on more). Draws from the one RNG; the game's stream goes on as if Moscow had
// drawn only its world seeds. Portable C; its own bank in the ROM.
#ifndef GD_BOT_MOSCOW_H
#define GD_BOT_MOSCOW_H

#include "gd_rules.h"

#ifndef MOSCOW_W1
#define MOSCOW_W1        6      // worlds every candidate plays
#endif
#ifndef MOSCOW_W2
#define MOSCOW_W2        12     // more worlds for the best two
#endif
#ifndef MOSCOW_MAX_CANDS
#define MOSCOW_MAX_CANDS 6
#endif
#define MOSCOW_MAX_TURNS 400    // a playout past this counts as lost
#ifndef MOSCOW_PLY_BUDGET
#define MOSCOW_PLY_BUDGET 1500u // no new world after this many plies, solver nodes counted at 2.5 (the worst case bound)
#endif
#ifndef MOSCOW_WIN_NODES
#define MOSCOW_WIN_NODES   400u    // the solver's win hunt, all root moves together (3.3 s on the Game Boy)
#endif
#ifndef MOSCOW_SOLVE_CARDS
#define MOSCOW_SOLVE_CARDS 8       // solve only endgames with at most this many cards in hand
#endif
#define MOSCOW_ROOT_MAX  64     // a root menu longer than this is not solved

// Moscow's move for `seat` (type 0xFF when it has none).
void gd_moscow_choose(const gd_game *g, uint8_t seat, gd_move *out) GD_BANKED;

// The last decision: candidates, playouts, plies in them, solver nodes, and
// what the solver did (MOSCOW_SOLVE_*).
#define MOSCOW_SOLVE_NONE   0   // not an endgame it solves
#define MOSCOW_SOLVE_WIN    1   // a proven win was played
#define MOSCOW_SOLVE_LOST   2   // every move proven to lose (Monte Carlo picks)
#define MOSCOW_SOLVE_ABORT  3   // ran out of nodes
extern uint8_t gd_moscow_cands, gd_moscow_solve;
extern uint16_t gd_moscow_playouts;
extern uint32_t gd_moscow_plies, gd_moscow_nodes;

#endif

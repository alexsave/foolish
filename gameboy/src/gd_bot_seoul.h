// gd_bot_seoul - Seoul: the robusta idea (foolish/c/src/robusta_strategy.c)
// on the Game Boy. For a few candidate moves it samples worlds consistent with
// what it can see (the unseen cards dealt at random to the opponent's hidden
// cards and the stock), plays each candidate on each world, plays the game out
// with the rollout engine (gd_sim), and keeps the candidate that won most.
// Portable C; its own bank in the ROM.
#ifndef GD_BOT_SEOUL_H
#define GD_BOT_SEOUL_H

#include "gd_rules.h"

// The playouts one decision may run, shared by its candidates (two to
// SEOUL_SAMPLES_MAX worlds each): sized for about 3 s a decision on the Game
// Boy (README.md "Seoul").
#ifndef SEOUL_BUDGET
#define SEOUL_BUDGET 20
#endif
#define SEOUL_SAMPLES_MAX 8
// The most candidates a decision weighs.
#ifndef SEOUL_MAX_CANDS
#define SEOUL_MAX_CANDS 6
#endif
// A playout longer than this counts as lost (robusta's rule for an unfinished one).
#define SEOUL_MAX_TURNS 400

// The move Seoul makes for `seat`; out->type is 0xFF when it has none.
void gd_seoul_choose(const gd_game *g, uint8_t seat, gd_move *out) GD_BANKED;

// What the last decision did (tests and the README's sizing): candidates
// weighed and playouts run.
extern uint8_t gd_seoul_cands;
extern uint16_t gd_seoul_playouts;
extern uint32_t gd_seoul_plies;

#endif

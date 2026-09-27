// Cordite — successor to blackpowder (smokeless powder replaced black powder).
// Belief-constrained determinized Monte Carlo with exact endgame play on both
// sides (take wins AND avoid losses), exact leaf endgames inside rollouts,
// rank-floor inference, and per-player constraint trust. Public info only.
#ifndef CNITRO_CORDITE_STRATEGY_H
#define CNITRO_CORDITE_STRATEGY_H

#include "game.h"
#include "legal.h"

int cordite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

// ---- fulminate: per-seat rollout-policy override ---------------------------
// Rollout-policy basis (cordite_core.ts POL_* / ARCH_POLICIES, lines
// 1356-1358). Order is load-bearing: the profiler's posterior vectors index
// this basis and the per-world sampler stores the drawn id per seat.
#define CORDITE_POL_HANDWRITTEN 0
#define CORDITE_POL_ESPRESSO    1
#define CORDITE_POL_RANDOM      2
#define CORDITE_POL_SIMPLE      3
#define CORDITE_POL_GREEDY      4
#define CORDITE_POL_PASSIVE     5
#define CORDITE_POL_HUMAN       6
#define CORDITE_NUM_POLICIES    7

// Install / clear a per-seat posterior WEIGHT vector over the policy basis
// (cordite_core.ts setSeatWeights, lines 1596-1600). While installed, the MC
// world loop samples a concrete per-seat policy table once per sampled world
// (shared by all candidates in that world — preserves CRN) and seats whose
// sampled policy differs from CORDITE_POL_HANDWRITTEN play their archetype
// chooser in the rollout. While cleared (the default), cordite's play is
// bit-for-bit unchanged. Thread-local, like all cordite deliberation state:
// set + clear around a single synchronous choose call (fulminate_strategy.c).
void cordite_set_seat_weights(const double w[MAX_PLAYERS][CORDITE_NUM_POLICIES],
                              int num_players);
void cordite_clear_seat_weights(void);

#endif

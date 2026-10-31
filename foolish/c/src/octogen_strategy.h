// Octogen — semtex + stage-3 opponent reply tournament.
#ifndef CNITRO_OCTOGEN_STRATEGY_H
#define CNITRO_OCTOGEN_STRATEGY_H

#include "game.h"
#include "legal.h"

int octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int octogen_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// Fusion: octogen whose pick a noisy second race may overrule (c/FUSION.md).
// Defined only where the rollout noise is compiled in (cordite_sim.h
// CD_ROLLOUT_NOISE): not in the shipped bots module.
int fusion_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// Fusion's shadow rule (octogen_strategy.c og_fusion_shadow), exposed for its
// test. The noisy race's pick f against octogen's own pick o over n shared
// worlds: dsum and d2sum are the sum and the sum of squares of f's score minus
// o's per world, pen_gap is f's trump-keep tax minus o's. 1 when f beats o by
// more than k10 tenths of a standard error of that paired difference.
int og_fusion_overrules(double dsum, double d2sum, int n, double pen_gap, int k10);
// What fusion's shadow did on this thread since the last reset (a test seam and
// a trace): races run, races whose noisy pick overruled octogen's, and races in
// which octogen's pinned pick missed a world (must stay 0: it is never culled).
typedef struct { long races, overruled, pin_short; } OgFusionStats;
OgFusionStats og_fusion_stats(void);
void          og_fusion_stats_reset(void);

#ifdef FOOLISH_ORACLE_BUILD
// Infinite-oracle only (docs/INFINITE_ORACLE_DESIGN.md §6.2): un-latch the
// OG_* env cache so the client bridge can adapt the per-batch world budget.
void og_reload_flags(void);
#endif

#endif

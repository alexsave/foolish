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

#ifndef FOOLISH_SEEDED_BOTS_ONLY
#include "bot_common.h"
#include <stdbool.h>
// The belief probe: a measurement seam for tools/belief_sizing, NULL in every
// build and set by nothing that plays. When set, og_build_belief hands it the
// belief twice per build: OG_PROBE_RAW after the log scan (every void, floor
// and tell the scan inferred, before a tell drops that seat's voids and floors)
// and OG_PROBE_FINAL as the worlds will apply it. It is only read, never
// written back: a probe cannot change a decision, and at NULL not one
// instruction of the belief differs but the test. Compiled out of the shipped
// bots module (FOOLISH_SEEDED_BOTS_ONLY), like the rollout noise.
#define OG_PROBE_RAW   0
#define OG_PROBE_FINAL 1
typedef void (*og_belief_probe_fn)(int stage, const Game *g, int bot_idx, const BotBelief *b,
                                   const bool mc_tell[MAX_PLAYERS]);
extern og_belief_probe_fn og_belief_probe;
#endif

#ifdef FOOLISH_ORACLE_BUILD
// Infinite-oracle only (docs/INFINITE_ORACLE_DESIGN.md §6.2): un-latch the
// OG_* env cache so the client bridge can adapt the per-batch world budget.
void og_reload_flags(void);
#endif

#endif

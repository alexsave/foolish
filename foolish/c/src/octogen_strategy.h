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
// Fusion's default bar (the FUSION_SHADOW knob), in tenths of a standard error:
// 2 SE, measured better than 1 and 3 at 4 and 6 players (c/FUSION.md). Here and
// not in octogen_strategy.c because the Infinite Oracle holds its own Best pick
// to the same bar (src/oracle/accumulator.ts fusionBest), and reads it through
// structgen (tools/structgen/specs/oracle_answer.args) rather than a copy.
#ifndef FUSION_SHADOW
#define FUSION_SHADOW 20
#endif
// What fusion's shadow did on this thread since the last reset (a test seam and
// a trace): races run, races whose noisy pick overruled octogen's, and races in
// which octogen's pinned pick missed a world (must stay 0: it is never culled).
typedef struct { long races, overruled, pin_short; } OgFusionStats;
OgFusionStats og_fusion_stats(void);
void          og_fusion_stats_reset(void);
// fusion_x (research): fusion with its knobs read from the FUSIONX_ prefix
// instead of FUSION_, so a paired run can set one arm's knobs (FUSION.md).
int fusion_x_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// Fusion's belief at a decision, under the FUSION_* knobs, and what `worlds` of
// its sampled worlds applied and dealt (a test seam; octogen_strategy.c
// og_fusion_probe). Per seat: the voids, floor and tell the scan ended with,
// the D constraints (value masks: d1v no card of the value, d2v / d4v no
// non-trump of it) and the D tell; per world counts: worlds applying each void,
// the floor, the D constraints, and worlds whose deal gave the seat an unknown
// card one of its voids (void_broken) or D constraints (d_broken) forbids.
#define OG_PROBE_VOIDS 6
typedef struct {
    int  void_n[MAX_PLAYERS];
    Card voids[MAX_PLAYERS][OG_PROBE_VOIDS];
    int  floor_v[MAX_PLAYERS];
    bool told[MAX_PLAYERS];
    uint16_t d1v[MAX_PLAYERS], d2v[MAX_PLAYERS], d4v[MAX_PLAYERS];
    bool d_distrust[MAX_PLAYERS];
    int  worlds;
    int  void_on[MAX_PLAYERS][OG_PROBE_VOIDS];
    int  floor_on[MAX_PLAYERS], d_on[MAX_PLAYERS];
    int  void_broken[MAX_PLAYERS], d_broken[MAX_PLAYERS];
} OgFusionProbe;
void og_fusion_probe(const Game *g, int bot_idx, int worlds, OgFusionProbe *out);

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

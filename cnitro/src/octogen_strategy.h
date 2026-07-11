// Octogen — semtex + stage-3 opponent reply tournament.
#ifndef CNITRO_OCTOGEN_STRATEGY_H
#define CNITRO_OCTOGEN_STRATEGY_H

#include "game.h"
#include "legal.h"

int octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int octogen_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

// Hexogen mode: octogen's brain with an iso-latency search-budget raise (the
// S4 spend in docs/L1_SPEND_PLAN.md). The hexogen wrapper (hexogen_strategy.c)
// brackets an octogen_strategy_choose call with this. It is a BEHAVIOR change
// (more worlds resolve more lines), so it is a NEW strategy id, never an edit
// to octogen (iron rule R1). Off by default; octogen plays byte-identically.
void octogen_set_hexogen(int on);

#endif

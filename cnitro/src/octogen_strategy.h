// Octogen — semtex + stage-3 opponent reply tournament.
#ifndef CNITRO_OCTOGEN_STRATEGY_H
#define CNITRO_OCTOGEN_STRATEGY_H

#include "game.h"
#include "legal.h"

int octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int octogen_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#ifdef FOOLISH_ORACLE_BUILD
// Infinite-oracle only (docs/INFINITE_ORACLE_DESIGN.md §6.2): un-latch the
// OG_* env cache so the client bridge can adapt the per-batch world budget.
void og_reload_flags(void);
#endif

#endif

// Semtex — cordite-derived bot tuned to beat cordite itself.
#ifndef CNITRO_SEMTEX_STRATEGY_H
#define CNITRO_SEMTEX_STRATEGY_H

#include "game.h"
#include "legal.h"

int semtex_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int semtex_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

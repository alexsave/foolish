// Torpex — semtex + learned value net replacing rollouts.
#ifndef CNITRO_TORPEX_STRATEGY_H
#define CNITRO_TORPEX_STRATEGY_H

#include "game.h"
#include "legal.h"

int torpex_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int torpex_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

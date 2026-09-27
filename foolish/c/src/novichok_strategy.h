// Novichok — the cheating apex bot (research/eval only).
#ifndef CNITRO_NOVICHOK_STRATEGY_H
#define CNITRO_NOVICHOK_STRATEGY_H

#include "game.h"
#include "legal.h"

int novichok_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int novichok_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

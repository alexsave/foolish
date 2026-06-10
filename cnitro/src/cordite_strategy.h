// Cordite — successor to blackpowder (smokeless powder replaced black powder).
// Belief-constrained determinized Monte Carlo with exact endgame play on both
// sides (take wins AND avoid losses), exact leaf endgames inside rollouts,
// rank-floor inference, and per-player constraint trust. Public info only.
#ifndef CNITRO_CORDITE_STRATEGY_H
#define CNITRO_CORDITE_STRATEGY_H

#include "game.h"
#include "legal.h"

int cordite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

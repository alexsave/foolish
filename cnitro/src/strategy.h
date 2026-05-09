// Strategy interface and registration. Mirrors BotStrategy.chooseMove —
// given a game and the precomputed legal moves, pick one. Index returned.
#ifndef CNITRO_STRATEGY_H
#define CNITRO_STRATEGY_H

#include "game.h"
#include "legal.h"

#define STRAT_RANDOM   0
#define STRAT_ESPRESSO 1
#define STRAT_NITRO    2

// Returns chosen move index in moves->moves[] (0..moves->n-1).
typedef int (*StrategyFn)(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

int random_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int espresso_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

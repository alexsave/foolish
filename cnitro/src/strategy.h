// Strategy interface and registration. Mirrors BotStrategy.chooseMove —
// given a game and the precomputed legal moves, pick one. Index returned.
#ifndef CNITRO_STRATEGY_H
#define CNITRO_STRATEGY_H

#include "game.h"
#include "legal.h"

#define STRAT_RANDOM      0
#define STRAT_ESPRESSO    1
#define STRAT_NITRO       2
#define STRAT_HANDWRITTEN 3
#define STRAT_DYNAMITE    4
#define STRAT_ROBUSTA     5
#define STRAT_FIRECRACKER 6
#define STRAT_GUNPOWDER   7
#define STRAT_BLACKPOWDER 8
#define STRAT_CORDITE     9

// Returns chosen move index in moves->moves[] (0..moves->n-1).
typedef int (*StrategyFn)(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

int random_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int espresso_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int handwritten_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int robusta_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int firecracker_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int gunpowder_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int blackpowder_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int cordite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

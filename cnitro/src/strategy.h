// Strategy interface and registration. Mirrors BotStrategy.chooseMove —
// given a game and the precomputed legal moves, pick one. Index returned.
#ifndef CNITRO_STRATEGY_H
#define CNITRO_STRATEGY_H

#include "game.h"
#include "legal.h"
#include <string.h>

#define STRAT_RANDOM      0
#define STRAT_ESPRESSO    1
#define STRAT_HANDWRITTEN 2
#define STRAT_ROBUSTA     3
#define STRAT_FIRECRACKER 4
#define STRAT_GUNPOWDER   5
#define STRAT_BLACKPOWDER 6
#define STRAT_CORDITE     7
#define STRAT_ASTROLITE   8

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
int astrolite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

// Map a strategy name (or short alias) to its STRAT_* id; -1 if unknown.
// Single source of truth for the name<->id mapping the main programs share.
static inline int parse_strategy(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "random")      || !strcmp(s, "rand")) return STRAT_RANDOM;
    if (!strcmp(s, "espresso")    || !strcmp(s, "esp"))  return STRAT_ESPRESSO;
    if (!strcmp(s, "handwritten") || !strcmp(s, "hw"))   return STRAT_HANDWRITTEN;
    if (!strcmp(s, "robusta")     || !strcmp(s, "rob"))  return STRAT_ROBUSTA;
    if (!strcmp(s, "firecracker") || !strcmp(s, "fc"))   return STRAT_FIRECRACKER;
    if (!strcmp(s, "gunpowder")   || !strcmp(s, "gp"))   return STRAT_GUNPOWDER;
    if (!strcmp(s, "blackpowder") || !strcmp(s, "bp"))   return STRAT_BLACKPOWDER;
    if (!strcmp(s, "cordite")     || !strcmp(s, "cd"))   return STRAT_CORDITE;
    if (!strcmp(s, "astrolite")   || !strcmp(s, "as"))   return STRAT_ASTROLITE;
    return -1;
}

#endif

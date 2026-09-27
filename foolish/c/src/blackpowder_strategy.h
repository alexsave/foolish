// Blackpowder — belief-constrained determinized Monte Carlo with an exact
// endgame solver. Public-info only (never reads live opponent hands or the
// real deck order). See blackpowder_strategy.c for the full design notes.
#ifndef CNITRO_BLACKPOWDER_STRATEGY_H
#define CNITRO_BLACKPOWDER_STRATEGY_H

#include "game.h"
#include "legal.h"

int blackpowder_strategy_choose(const Game *g, int bot_idx,
                                const LegalMoves *moves, void *ctx);

#endif

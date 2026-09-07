// CL-20 — octogen's successor (the next rung up the explosives ladder:
// HNIW sits above HMX). Octogen's engine, forked verbatim, plus the levers
// documented in CL20.md. C-only research/arena bot.
#ifndef CNITRO_CL20_STRATEGY_H
#define CNITRO_CL20_STRATEGY_H

#include "game.h"
#include "legal.h"

int cl20_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int cl20_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

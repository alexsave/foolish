#ifndef CNITRO_ROBUSTA_STRATEGY_H
#define CNITRO_ROBUSTA_STRATEGY_H

#include "strategy.h"

int robusta_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

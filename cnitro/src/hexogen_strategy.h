// Hexogen — octogen's chemical sibling (HMX -> RDX): the same brain with an
// iso-latency search-budget raise. See docs/L1_SPEND_PLAN.md §5/§7.
#ifndef CNITRO_HEXOGEN_STRATEGY_H
#define CNITRO_HEXOGEN_STRATEGY_H

#include "game.h"
#include "legal.h"

int hexogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

// Nitro strategy: NN-driven move selection. Mirrors the autoregressive
// "atomic action" decomposition that nitro_collect.ts uses for training:
// at each step, the model picks a single card / pickup / stop, and we keep
// extending the partial move until stop becomes the chosen action (or the
// resulting partial move is the only legal completion).
#ifndef CNITRO_NITRO_STRATEGY_H
#define CNITRO_NITRO_STRATEGY_H

#include "game.h"
#include "legal.h"
#include "nn.h"

// Stash NN params via a global pointer so the strategy fn signature stays
// uniform with random/espresso. Only one set of weights at a time.
void nitro_strategy_set_params(const NNParams *p);

int nitro_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

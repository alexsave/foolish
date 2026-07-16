#ifndef CNITRO_ROBUSTA_STRATEGY_H
#define CNITRO_ROBUSTA_STRATEGY_H

#include "strategy.h"

int robusta_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

// Generic Monte Carlo move selector that powers robusta. Exported so other
// strategies (e.g. firecracker) can reuse the same MC infrastructure with a
// different rollout policy. `rollout_fn` is invoked for every player in the
// simulated game state — typically handwritten_strategy_choose (robusta) or
// espresso_strategy_choose (firecracker, "fictional perfect-info" rollout).
int robusta_mc_choose(const Game *g, int bot_idx, const LegalMoves *moves,
                      StrategyFn rollout_fn);

#endif

#ifndef CNITRO_GUNPOWDER_STRATEGY_H
#define CNITRO_GUNPOWDER_STRATEGY_H

#include "strategy.h"

// Gunpowder — picks the rollout policy by starting player count:
//   PC=2:  handwritten (robusta-style, matches a real handwritten opponent)
//   PC=3+: espresso    (firecracker-style, sharper 1v1 endgame predictions
//                       while still using handwritten during the multi-player
//                       phase — espresso defers to handwritten internally
//                       when in_count > 2)
// Aim: combine robusta's PC=2 win with firecracker's PC=3-4 wins.
int gunpowder_strategy_choose(const Game *g, int bot_idx,
                              const LegalMoves *moves, void *ctx);

#endif

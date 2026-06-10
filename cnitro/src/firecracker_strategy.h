#ifndef CNITRO_FIRECRACKER_STRATEGY_H
#define CNITRO_FIRECRACKER_STRATEGY_H

#include "strategy.h"

// Firecracker — same MC architecture as robusta, but the rollout policy is
// espresso (a perfect-info "cheater") instead of handwritten. The trick: in
// the simulated game state, opponent hands are drawn from robusta's public-
// info-derived unseen pool, NOT real cards. So espresso "cheats" against
// hands robusta itself made up — i.e. it plays optimally given the
// information robusta already has. Like asking "what's my best move if my
// opponents had oracle vision of the fictional hands I'm sampling?".
int firecracker_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx);

#endif

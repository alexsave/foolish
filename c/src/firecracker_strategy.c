// Firecracker — same MC architecture as robusta, but using espresso as the
// rollout policy instead of handwritten.
//
// The key insight: espresso "cheats" by reading opponents' hands directly.
// Inside robusta's MC simulation, the opp hands are fictional — they came
// from sampling the unseen pool that robusta constructed using purely
// public info. So when espresso reads those hands during simulation, it
// isn't reading the real game's hidden information — it's reading robusta's
// own guess. That gives every simulated player oracle-quality decisions
// against the same fictional state, producing a sharper, more strategic
// rollout than handwritten provides.
//
// Public-info only at the OUTER level (robusta's MC) — same legitimacy as
// the base robusta strategy.

#include "firecracker_strategy.h"
#include "robusta_strategy.h"
#include "strategy.h"

int firecracker_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    return robusta_mc_choose(g, bot_idx, moves, espresso_strategy_choose);
}

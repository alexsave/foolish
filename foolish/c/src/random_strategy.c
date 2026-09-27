// Random strategy — picks a uniformly random legal move using the
// dedicated random_strategy RNG (separate from game-loop Math.random).
#include "strategy.h"

int random_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx) {
    (void)g; (void)bot_idx; (void)ctx;
    if (moves->n == 0) return -1;
    int idx = (int)(random_strategy_random() * moves->n);
    if (idx < 0) idx = 0;
    if (idx >= moves->n) idx = moves->n - 1;
    return idx;
}

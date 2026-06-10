// Gunpowder — picks rollout per decision, by both player count *and* the
// stage of the game (deck depletion). Two switch axes:
//
//   1. in_count: if 2 active players (1v1 now) → handwritten matches the
//      real opponent.
//   2. game stage: while the deck is still active (early/mid-game) we use
//      handwritten (simpler, faster, matches what an actual handwritten
//      opp would do); once the deck dries up (endgame, where finishing
//      hand size drives the outcome) we use espresso for sharper
//      endgame predictions.
//
// Combined rule:
//   in_count == 2                 → handwritten
//   else, deck still active       → handwritten
//   else (3+ IN, deck empty)      → espresso
//
// This is the "card-based" variant — switches mid-game when the deck
// empties. To test the player-based variant set GUNPOWDER_MODE = 1.

#include "gunpowder_strategy.h"
#include "robusta_strategy.h"
#include "strategy.h"

#ifndef GUNPOWDER_MODE
#define GUNPOWDER_MODE 2  // 1 = in_count only, 2 = in_count + deck stage
#endif

int gunpowder_strategy_choose(const Game *g, int bot_idx,
                              const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;

    // PC=5+: too many unknown opp hands for MC sampling to help — robusta
    // matches handwritten exactly at these counts (5000-game data), and
    // espresso rollout regresses. Skip MC entirely; play handwritten.
    if (g->num_players >= 5) {
        return handwritten_strategy_choose(g, bot_idx, moves, NULL);
    }

    int in_count = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) in_count++;
    }
    StrategyFn rollout;
#if GUNPOWDER_MODE == 1
    rollout = (in_count == 2) ? handwritten_strategy_choose
                              : espresso_strategy_choose;
#else
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (in_count == 2 || deck_active) {
        rollout = handwritten_strategy_choose;
    } else {
        rollout = espresso_strategy_choose;
    }
#endif
    return robusta_mc_choose(g, bot_idx, moves, rollout);
}

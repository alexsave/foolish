// difftest_step.h - the struct side of the two simulator difftests
// (sim_difftest, solver_difftest): one step of the real kernel's handwritten
// bots on a Game, the reference the flat SimState step is compared with.
#ifndef DIFFTEST_STEP_H
#define DIFFTEST_STEP_H

#include <stdbool.h>
#include <stddef.h>
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"

// One struct step (mirrors cd_simulate's inner loop, handwritten only).
static inline int struct_step(Game *g) {
    for (int pi = 0; pi < g->num_players; pi++) {
        if (!should_bot_act(g, pi)) continue;
        LegalMoves moves;
        calculate_legal_moves_lite(g, pi, &moves);
        if (moves.n == 0) continue;
        int idx = handwritten_strategy_choose(g, pi, &moves, NULL);
        if (idx < 0 || idx >= moves.n) continue;
        bool ok;
        switch (moves.moves[idx].type) {
            case MOVE_ATTACK: ok = handle_attack(g, pi, moves.moves[idx].cards, moves.moves[idx].n_cards); break;
            case MOVE_COVER:  ok = handle_cover(g, pi, moves.moves[idx].cards, moves.moves[idx].attack_cards, moves.moves[idx].n_cards); break;
            case MOVE_PASS:   ok = handle_pass(g, pi, moves.moves[idx].cards, moves.moves[idx].n_cards); break;
            case MOVE_PICKUP: ok = handle_pickup(g, pi); break;
            case MOVE_GOOD:   ok = handle_good(g, pi); break;
            default: ok = false;
        }
        if (ok) return pi;
    }
    return -1;
}

#endif

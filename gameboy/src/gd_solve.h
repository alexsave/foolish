// gd_solve - the endgame solver: foolish's exact two-player solver
// (cordite_sim.c cd_sim_solve_d) on the rollout state. Portable C; its own
// bank in the ROM, with its stack and transposition table in the cart's RAM.
#ifndef GD_SOLVE_H
#define GD_SOLVE_H

#include "gd_sim.h"

// THE ENDGAME SOLVER (foolish cd_sim_solve_d): the exact value of `root`, which
// must have no stock and no face-up trump left, from `me`'s side: 1000 - d for
// a win d plies from depth 0, -(1000 - d) for a loss, 0 a draw; fail-soft in
// (alpha, beta); `depth0` is the root's depth. `*budget` is the node budget,
// decremented by the nodes this call expanded; *aborted is set when it ran out
// or the search met its depth or move cap, and the value is then meaningless.
// Its stack and transposition table are in cartridge RAM on the Game Boy.
int16_t gd_solve(const gd_sim *root, uint8_t me, int16_t alpha, int16_t beta,
                 uint32_t *budget, uint8_t depth0, uint8_t *aborted) GD_BANKED;
// Empty the transposition table (once a decision: its values hold for any
// window and either seat, as foolish's do).
void gd_solve_reset(void) GD_BANKED;
extern uint32_t gd_solve_nodes;

#endif

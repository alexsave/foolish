// gd_sim - the rollout engine: foolish's two-player Monte Carlo playout
// (foolish/c/src/cordite_sim.c, cd_sim_playout) on a 32-byte state. Each ply
// the lowest eligible seat asks the rollout policy (foolish's arena
// handwritten, computed directly: sim_handwritten_move) for its one move, no
// menu, and plays it; the stock is drawn at a random index; there is no
// repetition rule. Its draws come from the one RNG (gd_rng) as 16-bit values
// u = r / 65536 where foolish draws game_random(). Portable C; its own bank
// in the ROM.
#ifndef GD_SIM_H
#define GD_SIM_H

#include "gd_rules.h"

// flags
#define SIM_FLIPPED  0x01u     // the face-up trump has not been drawn
#define SIM_DEF1     0x02u     // seat 1 defends (else seat 0)
#define SIM_FA1      0x04u     // seat 1 is the first attacker
#define SIM_GOOD0    0x08u     // seat 0 has said good
#define SIM_GOOD1    0x10u
#define SIM_OUT0     0x20u     // seat 0 is out
#define SIM_OUT1     0x40u

typedef struct {
    gd_mask  hand[2];
    gd_mask  stock;            // the stock, not counting the face-up trump
    gd_mask  unc;              // the uncovered attacks
    gd_mask  tbl;              // every card on the table
    uint16_t tranks;           // a bit per rank on the table
    uint8_t  nb;               // battles (attacks) on the table
    uint8_t  flipped;          // the face-up trump card
    uint8_t  trump;
    uint8_t  discard;
    uint8_t  flags;
    uint8_t  first_out;        // the seat that went out first, 0xFF none (foolish's elimination order)
} gd_sim;

// The board as a rollout state (the game must be playing).
void gd_sim_from_game(gd_sim *s, const gd_game *g) GD_BANKED;

// One ply: the lowest eligible seat whose policy has a move plays it. Returns
// that seat, or -1 when nobody acts (the game is over, or stuck).
int8_t gd_sim_step(gd_sim *s) GD_BANKED;

// A whole playout: plies until one seat is left in or `max_turns` pass.
// Returns `me`'s finish: 1 out first (won), 2 the fool, 0 unfinished.
uint8_t gd_sim_playout(gd_sim *s, uint8_t me, uint16_t max_turns) GD_BANKED;
// The plies the last playout played.
extern uint16_t gd_sim_plies;

// Is the game over (one seat left in)?
uint8_t gd_sim_done(const gd_sim *s);

// A menu move (gd_rules) played on the state, as foolish's
// cd_sim_apply_root_move does (any draws come from the one RNG).
void gd_sim_apply_move(gd_sim *s, uint8_t seat, const gd_move *m) GD_BANKED;


#endif

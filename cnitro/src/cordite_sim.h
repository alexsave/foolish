// Compact bitboard rollout engine for cordite.
//
// The root MC (corditeChoose) keeps running on the heavyweight `Game` struct;
// only each fully-determinized sampled WORLD is converted once into a SimState
// here, then all the fast playouts run on bitboards. A SimState is ~100 bytes
// and clones with a single memcpy, vs the 44 KB Game.
//
// Card id encoding: id = suit*13 + (value-1), range 0..51. Each player's hand
// is a uint64 bitmask over ids. Hand ops are O(1) bit twiddles; "cards of value
// v" / "cards of suit s" / trumps are precomputed masks.
//
// The rollout reimplements the same rules (handle_attack/cover/pass/pickup/good
// + refill + elimination) and the same rollout POLICY (handwritten — see
// cordite_sim.c for why espresso is never actually reached through
// cd_rollout_for) directly on the bitboard state.
#ifndef CNITRO_CORDITE_SIM_H
#define CNITRO_CORDITE_SIM_H

#include "game.h"
#include <stdint.h>

#define SIM_MAX_BATTLES 40   // >= MAX_BATTLES (32) with slack

typedef struct {
    uint8_t  num_players;
    uint8_t  power_suit;
    int8_t   defender;
    int8_t   first_attacker;
    uint8_t  status;          // GAME_STATUS_*
    uint8_t  num_battles;
    int16_t  deck_count;
    int16_t  discard_pile_length;
    uint8_t  has_flipped;
    uint8_t  flipped_id;       // card id of the flipped trump (when has_flipped)
    uint32_t good_mask;        // players that have said good
    uint8_t  num_eliminated;

    uint64_t hand[MAX_PLAYERS];     // card-id bitmask per player
    uint8_t  status_p[MAX_PLAYERS]; // PLAYER_STATUS_*

    int8_t   elim_order[MAX_PLAYERS];

    uint8_t  atk[SIM_MAX_BATTLES];      // attack card ids
    uint8_t  def[SIM_MAX_BATTLES];      // defense card ids (valid iff covered bit)
    uint64_t covered_mask;              // bit i set => battle i is covered

    int16_t  deck_n;                    // mirror of deck_count for the array
    uint8_t  deck[MAX_DECK];            // remaining deck card ids, in draw order
} SimState;

#include "legal.h"

// Build a SimState from a fully-determinized Game world (all hands known).
void cd_sim_from_game(SimState *s, const Game *g);

// Apply a root LegalMove (the bot's candidate move) to a SimState. Returns 1 on
// success, 0 on validation failure (mirrors cd_apply/handle_*). Used to run the
// candidate move on the bitboard state so the whole world can be converted once
// and each candidate just clones the SimState.
int cd_sim_apply_root_move(SimState *s, int p_idx, const LegalMove *m);

// Roll the world forward under the handwritten rollout policy; returns
// my_idx's finish position (1..N), or 0 if it didn't terminate. Mirrors
// cd_simulate's early-exit-on-elimination semantics. `early_exit` matches
// !cd_no_earlyexit.
int  cd_sim_playout(SimState *s, int my_idx, int max_turns, int early_exit);

#endif

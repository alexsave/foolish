// State and per-LegalMove feature encoders for the GRPO policy.
//
// All card-set features use a 52-dim space indexed by
//     CARD_IDX(c) = (c.value - 1) * 4 + c.suit
// where value is the engine's value (1..13). For 2-5p games (36-card deck,
// values 5..13), the bottom 16 slots stay zero. For 6-8p games (52-card
// deck, values 1..13), every slot is reachable.
//
// State encoding is from `self_idx`'s point of view — only information a
// legitimate player would observe. Per-opponent slots are walked clockwise
// starting at seat (self+1) mod N, skipping OUT players, up to 7 opponents.
// Slots past the live-opponent count are zero-padded.
#ifndef CNITRO_GRPO_ENCODE_H
#define CNITRO_GRPO_ENCODE_H

#include "game.h"
#include "legal.h"

#define MAX_DECK_INDEX 52
#define CARD_IDX(c)    (((c).value - 1) * 4 + (c).suit)

#define MAX_OPPONENTS 7

#define N_ROLES          4
#define ROLE_ATTACKER    0
#define ROLE_DEFENDER    1
#define ROLE_CO_ATTACKER 2
#define ROLE_IDLE        3

// --- per-opponent block ----------------------------------------------------
#define OPP_HANDSIZE_DIM 1
#define OPP_ROLE_DIM     N_ROLES
#define OPP_STILL_IN_DIM 1
#define OPP_TAKEN_DIM    MAX_DECK_INDEX
#define OPP_FEAT_DIM \
    (OPP_HANDSIZE_DIM + OPP_ROLE_DIM + OPP_STILL_IN_DIM + OPP_TAKEN_DIM)

// --- state vector layout ---------------------------------------------------
#define STATE_OWN_HAND_DIM       MAX_DECK_INDEX
#define STATE_TRUMP_SUIT_DIM     NUM_SUITS
#define STATE_TRUMP_CARD_DIM     MAX_DECK_INDEX
#define STATE_DISCARD_DIM        MAX_DECK_INDEX
#define STATE_ATTACKS_DIM        MAX_DECK_INDEX
#define STATE_DEFENSES_DIM       MAX_DECK_INDEX
#define STATE_OPPS_DIM           (MAX_OPPONENTS * OPP_FEAT_DIM)
#define STATE_NUM_LIVE_OPPS_DIM  1
#define STATE_SELF_ROLE_DIM      N_ROLES
#define STATE_DECK_REMAINING_DIM 1
#define STATE_PLAYER_COUNT_DIM   7

// New (v2) features for high-PC and endgame regimes:
//   hidden_trumps: count of trump cards we haven't observed (not in our
//     hand, not in discard, not in any publicly-known opp pile). Normalized
//     by 13 (max trumps in a suit). Tells the model how dangerous the
//     remaining trump pool is — crucial in late-game.
//   round_phase: 3-way one-hot — early (deck > 16), mid (1 < deck <= 16),
//     late (deck <= 1). Sharper signal than deck_remaining scalar alone.
//   distance_to_defender: one-hot over 8 — clockwise hops from self to the
//     current defender, skipping OUT players. 0 = self IS defender. Helps
//     the model situate itself in the multi-player attack/defense cycle.
#define STATE_HIDDEN_TRUMPS_DIM  1
#define STATE_ROUND_PHASE_DIM    3
#define STATE_DIST_TO_DEF_DIM    8

#define STATE_DIM ( \
    STATE_OWN_HAND_DIM + STATE_TRUMP_SUIT_DIM + STATE_TRUMP_CARD_DIM + \
    STATE_DISCARD_DIM + STATE_ATTACKS_DIM + STATE_DEFENSES_DIM + \
    STATE_OPPS_DIM + STATE_NUM_LIVE_OPPS_DIM + STATE_SELF_ROLE_DIM + \
    STATE_DECK_REMAINING_DIM + STATE_PLAYER_COUNT_DIM + \
    STATE_HIDDEN_TRUMPS_DIM + STATE_ROUND_PHASE_DIM + STATE_DIST_TO_DEF_DIM)

// --- per-move vector layout ------------------------------------------------
#define MOVE_TYPE_DIM       5  // ATTACK, COVER, PASS, PICKUP, GOOD
#define MOVE_CARDS_DIM      MAX_DECK_INDEX
#define MOVE_TARGETS_DIM    MAX_DECK_INDEX
#define MOVE_NCARDS_DIM     1
#define MOVE_MAXVAL_DIM     1
#define MOVE_USES_TRUMP_DIM 1

#define MOVE_FEAT_DIM ( \
    MOVE_TYPE_DIM + MOVE_CARDS_DIM + MOVE_TARGETS_DIM + \
    MOVE_NCARDS_DIM + MOVE_MAXVAL_DIM + MOVE_USES_TRUMP_DIM)

// Encode the game state from `self_idx`'s POV into `out` (length STATE_DIM).
// Only uses publicly-observable information for opponents.
void grpo_encode_state(const Game *g, int self_idx, float *out);

// Encode a single legal move into `out` (length MOVE_FEAT_DIM).
void grpo_encode_move(const Game *g, const LegalMove *m, float *out);

// Encode the full legal-move set into a row-major matrix `out` of shape
// [moves->n, MOVE_FEAT_DIM]. Convenience wrapper around grpo_encode_move.
void grpo_encode_moves(const Game *g, const LegalMoves *moves, float *out);

#endif

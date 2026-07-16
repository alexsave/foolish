// Legal-move enumeration. Mirrors calculateLegalMoves in
// supabase/functions/_shared/common/bot_strategy.ts.
#ifndef CNITRO_LEGAL_H
#define CNITRO_LEGAL_H

#include "card.h"
#include "game.h"
#include <stdbool.h>

#define MOVE_ATTACK 0
#define MOVE_COVER  1
#define MOVE_PASS   2
#define MOVE_PICKUP 3
#define MOVE_GOOD   4
#define MOVE_WAIT   5

// Capacities are build parameters (like MAX_LOG_PAIRS): the native arena
// keeps the compact bot-vs-bot sizes; the WASM production build widens them
// (-DMAX_MOVE_CARDS=40 -DMAX_LEGAL_MOVES=65536) because human games reach
// states (huge post-pickup hands) the arena never does. Moves past the caps
// are dropped in enumeration order — a deliberate, documented bound that
// replaces the old TS enumerator's unbounded combinatorial blow-up.
#ifndef MAX_MOVE_CARDS
#define MAX_MOVE_CARDS 8         // hand size 6 + slack
#endif
typedef struct {
    int8_t type;
    int8_t n_cards;
    Card   cards[MAX_MOVE_CARDS];
    Card   attack_cards[MAX_MOVE_CARDS]; // cover only
} LegalMove;

#ifndef MAX_LEGAL_MOVES
#define MAX_LEGAL_MOVES 4096
#endif

typedef struct {
    int      n;
    LegalMove moves[MAX_LEGAL_MOVES];
} LegalMoves;

void calculate_legal_moves(const Game *g, int bot_idx, LegalMoves *out);

// Scoped output cap: generation appends (and the combinatorial recursions
// prune) at `cap` moves instead of MAX_LEGAL_MOVES, so callers may enumerate
// into buffers with fewer than MAX_LEGAL_MOVES slots (the solver scratch).
// 0 or out-of-range resets to MAX_LEGAL_MOVES. Thread-local; set immediately
// around the calculate_legal_moves call and reset after.
void legal_set_move_cap(int cap);

// Faster variant for use inside Monte Carlo simulations where every player
// plays a deterministic policy (handwritten). Skips the combinatorial cover
// enumeration — emits one greedy lowest-cost full-cover move instead, which
// matches handwritten's pick. Attack/pass enumerations are unchanged.
void calculate_legal_moves_lite(const Game *g, int bot_idx, LegalMoves *out);

// One-tap cover resolution (F9). Given `n_cover` selected cover cards and the
// current table, decide whether they cover the uncovered attacks in exactly ONE
// unambiguous way — every valid full pairing of cover cards to distinct
// uncovered attacks covers the SAME set of attacks. On success writes, for each
// cover card, the attack it covers (index-aligned with cover_cards) into
// out_attacks[] and returns 1. Otherwise returns 0 and the caller lets the
// player place cards manually. The one implementation the web drag, phone
// tap-commit, watch chooser and iMessage all call.
int unambiguous_cover(const Card *cover_cards, int n_cover,
                      const Battle *battles, int n_battles, int power_suit,
                      Card *out_attacks);

#endif

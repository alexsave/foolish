// Legal-move enumeration. Mirrors calculateLegalMoves in
// supabase/functions/_shared/bot_strategy.ts.
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

#define MAX_MOVE_CARDS 8         // hand size 6 + slack
typedef struct {
    int8_t type;
    int8_t n_cards;
    Card   cards[MAX_MOVE_CARDS];
    Card   attack_cards[MAX_MOVE_CARDS]; // cover only
} LegalMove;

// Combinatorial blow-up cap. Espresso-vs-random in TS rarely produces more
// than a few hundred moves at a time; this is a safety bound.
#define MAX_LEGAL_MOVES 4096

typedef struct {
    int      n;
    LegalMove moves[MAX_LEGAL_MOVES];
} LegalMoves;

void calculate_legal_moves(const Game *g, int bot_idx, LegalMoves *out);

// Faster variant for use inside Monte Carlo simulations where every player
// plays a deterministic policy (handwritten). Skips the combinatorial cover
// enumeration — emits one greedy lowest-cost full-cover move instead, which
// matches handwritten's pick. Attack/pass enumerations are unchanged.
void calculate_legal_moves_lite(const Game *g, int bot_idx, LegalMoves *out);

#endif

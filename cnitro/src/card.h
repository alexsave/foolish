// Card representation and constants. Mirrors the TS layout in
// supabase/functions/_shared/types.ts and constants.ts.
#ifndef CNITRO_CARD_H
#define CNITRO_CARD_H

#include <stdbool.h>
#include <stdint.h>

#define SUIT_SPADES   0
#define SUIT_HEARTS   1
#define SUIT_CLUBS    2
#define SUIT_DIAMONDS 3
#define NUM_SUITS     4

// Game uses values 5..13 (2-player) — see refill_deck in common_utils.ts.
#define ACE_VALUE        13
#define MIN_VALUE_2P     5
#define CARDS_PER_PLAYER 6

typedef struct {
    int8_t suit;
    int8_t value;
} Card;

static inline bool card_eq(Card a, Card b) {
    return a.suit == b.suit && a.value == b.value;
}

#endif

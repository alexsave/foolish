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

// Card values: 1..13 (Ace=13). Small games use the 36-card deck (values
// 5..A); large games use the full 52-card deck (values 1..A). Where the
// boundary sits is configurable because the two deployments disagree ON
// PURPOSE: production TS (`refill_deck` in common_utils.ts) and the FROZEN
// replay wire format both use `players > 4` (5+ → 52 cards), while the
// cnitro research tools were tightened to 6+ per project owner. Native
// builds keep the research default (6); the WASM production build sets 5
// at init so the kernel is byte-exact with the live server and with every
// historical replay.
#define ACE_VALUE         13
#define MIN_VALUE_SMALL   5  // small-deck Durak (36 cards)
#define MIN_VALUE_LARGE   1  // full-deck Durak (52 cards)
#define CARDS_PER_PLAYER  6

// Minimum player count that deals the full 52-card deck. Defined in game.c.
extern int g_large_deck_min_players;

static inline int min_value_for(int num_players) {
    return num_players >= g_large_deck_min_players ? MIN_VALUE_LARGE : MIN_VALUE_SMALL;
}

// Back-compat alias: existing call sites used MIN_VALUE_2P.
#define MIN_VALUE_2P MIN_VALUE_SMALL

typedef struct {
    int8_t suit;
    int8_t value;
} Card;

static inline bool card_eq(Card a, Card b) {
    return a.suit == b.suit && a.value == b.value;
}

#endif

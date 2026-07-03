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

// Card values: 1..13 (Ace=13). THE deck rule, settled: 2..5 players use the
// 36-card deck (values 5..A); 6..8 players use the full 52-card deck
// (values 1..A). This is the single source of truth for every deployment —
// the WASM production server, the cnitro arena, and the replay projection
// (runReplay in _shared/replay/core.ts, a living spec that mirrors this).
// Historical 5-player replays encoded under the old 5+ → 52 rule no longer
// decode; accepted when the rule was settled.
#define ACE_VALUE         13
#define MIN_VALUE_SMALL   5  // small-deck Durak (36 cards)
#define MIN_VALUE_LARGE   1  // full-deck Durak (52 cards)
#define CARDS_PER_PLAYER  6

static inline int min_value_for(int num_players) {
    return num_players >= 6 ? MIN_VALUE_LARGE : MIN_VALUE_SMALL;
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

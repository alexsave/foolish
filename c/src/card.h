// Card representation and constants. Mirrors the TS layout in
// server/api/core/types.ts and constants.ts.
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
// (replay.c uses min_value_for directly; the TS mirror is minValueFor in
// _shared/core/constants.ts). Historical 5-player replays encoded under the old
// 5+ → 52 rule no longer decode; accepted when the rule was settled.
#define ACE_VALUE         13
#define MIN_VALUE_SMALL   5  // small-deck Durak (36 cards)
#define MIN_VALUE_LARGE   1  // full-deck Durak (52 cards)
#define CARDS_PER_PLAYER  6

static inline int min_value_for(int num_players) {
    return num_players >= 6 ? MIN_VALUE_LARGE : MIN_VALUE_SMALL;
}

// Back-compat alias: existing call sites used MIN_VALUE_2P.
#define MIN_VALUE_2P MIN_VALUE_SMALL

// ONE byte, not two: suit lives in 3 signed bits (-4..3 — real suits 0..3
// plus the -1 hidden-card sentinel), value in 5 signed bits (-16..15 — real
// values 1..13 plus -1). Bitfields keep the `.suit`/`.value` access syntax
// unchanged across ~490 call sites while halving every Card array (hands,
// battles, log pairs, legal moves — the bulk of the wasm modules' memory).
// The wasm IO marshal reads/writes the FIELDS, so the wire layout stays
// independent of this in-memory representation.
typedef struct {
    int8_t suit  : 3;
    int8_t value : 5;
} Card;
_Static_assert(sizeof(Card) == 1, "Card must pack into one byte");

static inline bool card_eq(Card a, Card b) {
    return a.suit == b.suit && a.value == b.value;
}

// Card <-> dense id (0..51): id = suit*13 + (value-1). The replay codec's
// alphabets and the wire card byte are both this numbering.
static inline Card card_of_id(int id) {
    Card c;
    c.suit  = (int8_t)(id / 13);
    c.value = (int8_t)(id % 13 + 1);
    return c;
}
static inline int card_to_id(Card c) { return c.suit * 13 + (c.value - 1); }

// Several bots mark which VALUES are on the table in a small bool table indexed
// by card value (`bool table_v[CARD_VALUE_MARKS]`). Values are 1..13 on a
// well-formed board, so `marks[c.value] = true` reads as obviously safe — but a
// BOT is handed state it never validated, and a malformed board carries
// anything. Under bots.wasm's `--stack-first` these tables sit near address 0,
// so a NEGATIVE value indexes below linear memory and TRAPS the whole module:
// the fuzz's 40-battle mutation (values 88 and -3) took firecracker down
// through exactly this, in two separate copies of the idiom. Go through these.
#define CARD_VALUE_MARKS 16
static inline void card_mark_value(bool *marks, int value) {
    if (value >= 0 && value < CARD_VALUE_MARKS) marks[value] = true;
}
static inline bool card_has_value(const bool *marks, int value) {
    return value >= 0 && value < CARD_VALUE_MARKS && marks[value];
}

// "No card" sentinel: replaces the has_defense/has_target booleans (an
// uncovered battle stores CARD_NONE as its defense; a single-card log pair
// stores CARD_NONE as its target). Distinct from the -1/-1 hidden card.
#define CARD_NONE ((Card){ .suit = -2, .value = -2 })
static inline bool card_is_none(Card c) { return c.suit == -2 && c.value == -2; }

#endif

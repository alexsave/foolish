// 1-byte wire cards for the TS<->kernel IO buffer.
//   0..51 = suit*13 + (value-1)
//   0xFE  = the hidden card ({-1,-1}: DRAW logs for other players' picks)
//   0xFF  = no card (uncovered battle defense, single-card log pair)
// Halves every card crossing the boundary and removes the has_defense /
// has_target wire bytes (in-band sentinels instead). Mirrored by
// supabase/functions/_shared/sdk/ts/wasm/engine.ts (wireStateCard/cardFromWire*).
#ifndef CNITRO_WASM_WIRE_H
#define CNITRO_WASM_WIRE_H

#include "card.h"

#define WIRE_CARD_HIDDEN 0xFEu
#define WIRE_CARD_NONE   0xFFu

static inline unsigned char wire_from_card(Card c) {
    if (card_is_none(c)) return (unsigned char)WIRE_CARD_NONE;
    if (c.suit < 0 || c.value < 1) return (unsigned char)WIRE_CARD_HIDDEN;
    return (unsigned char)(c.suit * 13 + (c.value - 1));
}

// State/move/action cards are always real cards; hostile ids clamp into the
// representable space (the job clamp_card did on the old 2-byte wire — the
// bot bitboards do `1ull << card_id`, undefined for out-of-range ids).
static inline Card card_from_wire_state(unsigned char b) {
    if (b > 51) b = 51;
    Card c; c.suit = (int8_t)(b / 13); c.value = (int8_t)(b % 13 + 1);
    return c;
}

// Log pairs carry the sentinels.
static inline Card card_from_wire_pair(unsigned char b) {
    if (b == WIRE_CARD_NONE) return CARD_NONE;
    if (b == WIRE_CARD_HIDDEN) { Card c; c.suit = -1; c.value = -1; return c; }
    return card_from_wire_state(b);
}

#endif

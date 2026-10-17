// What a card's face says: the label of each rank, by card value, and the glyph
// and ink of each suit, by suit (card.h's numbering for both).
//
// THIS FILE IS SOURCE FOR A GENERATOR, like c/errtext: tools/datagen reads the
// tables below at build time and writes sdk/ts/gen/card_rank.ts and card_suit.ts
// (tools/structgen/gen.sh). A kernel that deals a value has no use for the letter
// on it, so no kernel object includes it; the host CLIs that print cards
// (src/main_replay.c) include it instead of keeping a table of their own.
//
// WHY IT IS C. The web kept the rank labels as a table of its own beside a copy
// of the suit numbers, and a card value is a rank INDEX, not a face number
// (value 1 is the two, 5 the six the small deck starts at, ACE_VALUE the ace),
// so a table that slips by one draws a deck nobody dealt. Here every row is
// written under its own value or card.h's own constant, and datagen keys it by that.
#ifndef FOOLISH_CARD_TEXT_H
#define FOOLISH_CARD_TEXT_H

#include "card.h"

// By card value: the index beside each label IS the value, so a row cannot slip.
// A value with no rank (0, which no card has) is the empty label, 1 is the two
// and ACE_VALUE the ace. datagen refuses a value left without a label.
static const char *const CARD_RANK_LABEL[] = {
    [0] = "",
    [1] = "2",  [2] = "3",  [3] = "4",  [4] = "5",
    [5] = "6",  [6] = "7",  [7] = "8",  [8] = "9",  [9] = "10",
    [10] = "J", [11] = "Q", [12] = "K", [ACE_VALUE] = "A",
};
_Static_assert(sizeof(CARD_RANK_LABEL) / sizeof(CARD_RANK_LABEL[0]) == ACE_VALUE + 1, "one label per card value, the ace last");

typedef struct { const char *glyph; int red; } SuitFace;

// By suit, each row under card.h's own name for it.
static const SuitFace CARD_SUIT_FACE[] = {
    [SUIT_SPADES]   = { "\u2660", 0 },
    [SUIT_HEARTS]   = { "\u2665", 1 },
    [SUIT_CLUBS]    = { "\u2663", 0 },
    [SUIT_DIAMONDS] = { "\u2666", 1 },
};
_Static_assert(sizeof(CARD_SUIT_FACE) / sizeof(CARD_SUIT_FACE[0]) == NUM_SUITS, "one face per suit");

#endif

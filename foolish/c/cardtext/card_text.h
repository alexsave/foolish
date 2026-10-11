// What a card's face says: the label of each rank, by card value, and the glyph
// and ink of each suit, by suit (card.h's numbering for both).
//
// THIS FILE IS SOURCE FOR A GENERATOR AND IS LINKED BY NOTHING, like c/errtext:
// tools/datagen reads the tables below at build time and writes
// sdk/ts/gen/card_rank.ts and card_suit.ts (tools/structgen/gen.sh). A kernel that deals a value
// has no use for the letter on it.
//
// WHY IT IS C. The web kept the rank labels as a table of its own beside a copy
// of the suit numbers, and a card value is a rank INDEX, not a face number
// (value 1 is the two, 5 the six the small deck starts at, ACE_VALUE the ace),
// so a table that slips by one draws a deck nobody dealt. Here the tables are held
// to the kernel's own constants by the compiler.
#ifndef FOOLISH_CARD_TEXT_H
#define FOOLISH_CARD_TEXT_H

#include "card.h"

// By card value, in order: a value with no rank (0, which no card has) is the
// empty label, 1 is the two and ACE_VALUE the ace. Positional, so the asserts
// are what hold the rows to card.h's numbering.
static const char *const CARD_RANK_LABEL[] = {
    "", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A",
};
_Static_assert(sizeof(CARD_RANK_LABEL) / sizeof(CARD_RANK_LABEL[0]) == ACE_VALUE + 1, "one label per card value, the ace last");

typedef struct { const char *glyph; int red; } SuitFace;

// By suit, in order: spades, hearts, clubs, diamonds.
static const SuitFace CARD_SUIT_FACE[] = {
    { "\u2660", 0 },
    { "\u2665", 1 },
    { "\u2663", 0 },
    { "\u2666", 1 },
};
_Static_assert(sizeof(CARD_SUIT_FACE) / sizeof(CARD_SUIT_FACE[0]) == NUM_SUITS
               && SUIT_SPADES == 0 && SUIT_HEARTS == 1 && SUIT_CLUBS == 2 && SUIT_DIAMONDS == 3,
               "one face per suit, in card.h's order");

#endif

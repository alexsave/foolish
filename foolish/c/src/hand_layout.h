// hand_layout - the shape of the local player's hand: given N cards and the
// width a host can give them, how many rows, how many cards in each, how wide
// a card is, how tall the whole hand stands and where every slot sits.
//
// It is a SHAPE rule, not a Durak rule, and it lives here because two hosts
// need the same answer: the iMessage fan (ios/FoolishKit/DesignSystem/
// FHandFan.swift, round-5 M6 and round-16) worked it out first, and the web
// hand is to split into two rows exactly the way that one does. One derivation
// is the only way the two cannot drift, which is the same argument
// anim_plan.h makes for the hand's ORDER (anim_hand_laid_out) - this file is
// the geometry that order is laid into.
//
// PURE. No Game, no allocation, no state between calls. The host passes its
// own units in HandMetrics (iOS points, web CSS px), so the same function
// serves both and no number in here is a screen size.
//
// FLOATING POINT IS EXACT, AND HAS TO BE. FHandFan's own Swift math was the
// rule before this file was, and the move was proved bit for bit against a
// verbatim copy of that math at every hand count and every half point of
// width (PR #266) before the copy was deleted. Swift neither fuses a multiply-add nor
// reassociates, while every build tree that compiles c/src does one or both by
// default (c/Makefile and the native server at -ffast-math, clang arm64 at
// -ffp-contract=on). hand_layout.c pins IEEE semantics for itself with
// pragmas, so no build tree needs a per-file flag, and keeps the Swift
// operation order step for step.
#ifndef CNITRO_HAND_LAYOUT_H
#define CNITRO_HAND_LAYOUT_H

#include <stdint.h>

#define HAND_EOK      0
#define HAND_ECAP    -1   // `out` holds fewer slots than the hand has cards
#define HAND_EBADARG -2   // a NULL pointer, a negative count or a negative cap

// A hand is never cut into more than two rows. Past the point where even two
// rows thin a card to card_w_min, the card holds the floor and the row runs
// wider than the width it was given; there is no third row.
#define HAND_MAX_ROWS 2

// The host's units. Every field is a length in the host's own unit.
typedef struct HandMetrics {
    double card_w_max;   // a card never gets wider than this (iOS 52: never "superwide")
    double card_w_min;   // ...nor narrower (iOS 22)
    double card_h;       // a card's CONSTANT height, however many share the row (iOS 72)
    double gap;          // between neighbouring cards, and at both row ends (iOS 4)
    double row_gap;      // between the two rows once the hand splits (iOS 6)
    double row_pad;      // a row stands this much taller than its cards (iOS 8)
    double split_below;  // a one-row card narrower than this splits the hand (iOS 34)
} HandMetrics;

// The rows a hand is cut into.
typedef struct HandRows {
    int32_t rows;                   // 1 or 2
    int32_t row_n[HAND_MAX_ROWS];   // cards per row, top first; row_n[1] is 0 for one row
    double  card_w;                 // ONE width for every card, from the fuller row
    double  height;                 // the whole hand's height, rows plus pads plus gap
} HandRows;

// One card's resting slot, in the hand's own space (origin top-leading).
typedef struct HandRect {
    double x, y, w, h;
} HandRect;

// How `count` cards sit in `width`. HAND_EOK, or HAND_EBADARG for a NULL or a
// negative count. A width of zero or less is still an answer (two rows for two
// or more cards, the floor width), because a host asks for a height before it
// has measured anything; a host that has not measured should ask at DBL_MAX,
// which is one row, exactly as FHandFan does on its first paint.
int hand_rows(const HandMetrics *m, int count, double width, HandRows *out);

// Every slot, flat: the top row left to right, then the bottom row. Returns the
// number of slots written - 0 when width <= 0 or count == 0, since there is no
// space to place anything in - or HAND_ECAP when `cap` is short, HAND_EBADARG
// for a NULL, a negative count or a negative cap. Nothing is written on an
// error.
int hand_slots(const HandMetrics *m, int count, double width, HandRect *out, int cap);

// THE WHOLE ANSWER IN ONE VALUE, for a host that reads it out of a struct
// rather than handing over its own array: the web reads it out of bots.wasm
// through a generated snapshot reader (c/wasm/wasm_table_api.c,
// sdk/ts/table/client_table.ts handLayout), so the slots are a fixed array with
// their count beside them. The cap is game.h's MAX_HAND_SIZE, the most cards a
// hand can hold (wasm_table_api.c asserts the two agree).
#define HAND_LAYOUT_CAP 64

typedef struct HandLayout {
    HandRows rows;
    int32_t  n;                        // slots written: hand_slots' count
    HandRect slot[HAND_LAYOUT_CAP];
} HandLayout;

// hand_rows and hand_slots for the same question, into one HandLayout.
// HAND_EOK, HAND_EBADARG for a NULL or a negative count, HAND_ECAP for a count
// over HAND_LAYOUT_CAP. Nothing is written on an error.
int hand_layout(const HandMetrics *m, int count, double width, HandLayout *out);

#endif

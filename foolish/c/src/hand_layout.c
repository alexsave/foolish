// hand_layout.c - see hand_layout.h. The rule was FHandFan.swift's (round-5
// M6, round-16); the reasons below are the owner's, kept with the arithmetic
// they explain.

// IEEE-EXACT. Every line below must produce the bits Swift produces for the
// same expression, and Swift neither contracts a*b+c into a fused multiply-add
// nor reassociates nor turns a divide into a reciprocal multiply. No build
// compiles with -ffast-math any more, so the reassociation and the reciprocal
// cannot happen. The one that can is contraction: clang on arm64 contracts by
// default, and a fused multiply-add is rounded once where Swift rounds twice.
// So the owner of this is the build's flag, -ffp-contract=off, and every build
// that compiles this file carries it: c/Makefile CFLAGS (host, overlay,
// analyse, fuzz), ASAN_CFLAGS, COV_FLAGS, TABLE_HOST_CFLAGS, IOS_CFLAGS (the
// device and simulator slices Xcode runs through ios_xcframework.mk), and
// c_base's KERNEL_CFLAGS. wasm32 is strict IEEE and has no fused multiply-add
// instruction, so it needs nothing. A new build tree that compiles this file
// takes the flag too.

#include "hand_layout.h"

#include <stddef.h>

// Swift.min / Swift.max, operand for operand: min(x, y) is `y < x ? y : x` and
// max(x, y) is `y >= x ? y : x`. fmin/fmax are not the same function - they
// differ on a NaN - and this file keeps Swift's.
static double swift_min(double x, double y) { return y < x ? y : x; }
static double swift_max(double x, double y) { return y >= x ? y : x; }

// The per-card width if `count` cards shared `width` in ONE row:
// min(card_w_max, max(card_w_min, avail / n)), with a gap between neighbours and
// at both ends. A 0-card hand is sized as a 1-card one.
static double one_row_card_w(const HandMetrics *m, int count, double width) {
    const int n = count > 1 ? count : 1;
    // (double)n + 1.0 rather than (double)(n + 1): the same value for every n
    // a hand can be, without an int overflow at INT_MAX.
    const double avail = width - m->gap * ((double)n + 1.0);
    return swift_min(m->card_w_max, swift_max(m->card_w_min, avail / (double)n));
}

int hand_rows(const HandMetrics *m, int count, double width, HandRows *out) {
    if (!m || !out || count < 0) return HAND_EBADARG;

    // Round-5 M6: below split_below per card, split into two rows rather than
    // keep thinning. Durak routinely leaves a defender holding 15-20 cards after
    // two pickups, which fell under Apple's 44pt hit-target minimum in a single
    // row. ~34pt still reads as a proper card, just a narrow one; the web's own
    // answer to the same problem ("that's what we do if we have a lot of cards
    // in the replay on the website") is exactly a second row, not a hard floor.
    //
    // A 0- or 1-card hand is ALWAYS one row whatever the width says, so a
    // degenerate (zero or negative) width can never report a split there is
    // nothing to split.
    const int two = count > 1 && one_row_card_w(m, count, width) < m->split_below;

    // WHICH ROW GETS THE ODD CARD is the owner's call, and they reversed it:
    // "If we have 11 cards, do 5 up top and 6 below". It used to be ceil - six
    // up top, five below - which stands the hand on its point. A hand fans out
    // from the hand that holds it, so the wider row belongs at the BOTTOM,
    // nearest the player; the narrow row reads as sitting behind it. Under the
    // iMessage collapsed drawer's crop it matters more still, because the bottom
    // row is the one whose faces are least occluded.
    //
    // Rows are DERIVED from a flat order by cutting it at floor(n/2); they are
    // not storage. Moving a card across the boundary is an ordinary splice into
    // the flat array and the cut then falls in a different place: sliding a
    // bottom card up to slot 1 pushes everything from 1 onward right by one, and
    // the card that was last in the top row lands first in the bottom row. The
    // "bump" the owner asked for is not a special case; it is what a fixed cut
    // through a shifted array already does.
    if (two) {
        out->rows = 2;
        out->row_n[0] = count / 2;
        out->row_n[1] = count - count / 2;
    } else {
        out->rows = 1;
        out->row_n[0] = count;
        out->row_n[1] = 0;
    }

    // ONE card width for BOTH rows, sized by the FULLER row. Sizing each row by
    // its own count made the shorter row's cards visibly WIDER than the other's,
    // which read as two different decks rather than one hand that wrapped. The
    // fuller row is the bottom one now that the odd card goes below; sizing off
    // the top row was right only while the cut was a ceil, and would size an
    // 11-card hand off 5 and overflow the row of 6.
    const int fuller = out->row_n[0] > out->row_n[1] ? out->row_n[0] : out->row_n[1];
    out->card_w = one_row_card_w(m, fuller, width);

    // One row stands card_h + row_pad; two such rows stack with row_gap between.
    const double one_row = m->card_h + m->row_pad;
    out->height = two ? one_row * 2.0 + m->row_gap : one_row;
    return HAND_EOK;
}

int hand_slots(const HandMetrics *m, int count, double width, HandRect *out, int cap) {
    if (!m || count < 0 || cap < 0) return HAND_EBADARG;
    // `!(width > 0)` rather than `width <= 0`, so a NaN width places nothing,
    // as Swift's `guard width > 0` does.
    if (!(width > 0) || count == 0) return 0;
    if (count > cap) return HAND_ECAP;
    if (!out) return HAND_EBADARG;

    HandRows r;
    hand_rows(m, count, width, &r);

    // Round-16: these rects are the layout ITSELF, not a mirror of it. The fan
    // places every card absolutely from them, so a card that changes row is an
    // ordinary change of offset and animates like every other slide, and a
    // flight into the hand lands on the slot the card will actually rest in.
    //
    // The block of rows is centred vertically in the hand's height, and each
    // row is centred horizontally in the width.
    const double stack_h = (double)r.rows * m->card_h
                         + (double)(r.rows - 1) * m->row_gap;
    const double v_top = (r.height - stack_h) / 2.0;
    int k = 0;
    for (int row = 0; row < r.rows; row++) {
        const int n = r.row_n[row];
        const double row_w = (double)n * r.card_w
                             + (double)(n > 1 ? n - 1 : 0) * m->gap;
        const double row_left = (width - row_w) / 2.0;
        const double y = v_top + (double)row * (m->card_h + m->row_gap);
        for (int c = 0; c < n; c++) {
            out[k].x = row_left + (double)c * (r.card_w + m->gap);
            out[k].y = y;
            out[k].w = r.card_w;
            out[k].h = m->card_h;
            k++;
        }
    }
    return k;
}

int hand_layout(const HandMetrics *m, int count, double width, HandLayout *out) {
    if (!m || !out || count < 0) return HAND_EBADARG;
    if (count > HAND_LAYOUT_CAP) return HAND_ECAP;
    HandRows r;
    hand_rows(m, count, width, &r);
    out->n = hand_slots(m, count, width, out->slot, HAND_LAYOUT_CAP);
    out->rows = r;
    return HAND_EOK;
}

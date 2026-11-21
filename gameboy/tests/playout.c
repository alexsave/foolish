// playout - see playout.h.
#include "playout.h"
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"

// The ROM build times each phase of a move (PO_MARK); elsewhere it is nothing.
#ifndef PO_MARK
#define PO_MARK(i) ((void)0)
#endif

static gd_game g;
static gd_move m;

uint16_t playout_one(void) {
    uint8_t deck[GD_NCARDS], i, j, t;
    uint16_t moves = 0;
    for (i = 0; i < GD_NCARDS; i++) deck[i] = i;
    for (i = GD_NCARDS - 1u; i > 0; i--) {
        j = (uint8_t)gd_rng_below((uint32_t)i + 1u);
        t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    gd_deal(&g, 0, deck, (uint8_t)gd_rng_below(2));
    while (g.status == GD_PLAYING && moves < PLAYOUT_CAP) {
        uint32_t n0, n1, pick;
        uint8_t seat;
        PO_MARK(0);
        n0 = gd_menu_count_lite(&g, 0);
        n1 = gd_menu_count_lite(&g, 1);
        PO_MARK(1);
        pick = gd_rng_below(n0 + n1);
        seat = pick < n0 ? 0 : 1;
        PO_MARK(2);
        gd_menu_move_lite(&g, seat, seat ? pick - n0 : pick, &m);
        PO_MARK(3);
        gd_apply(&g, 0, seat, &m);
        PO_MARK(4);
        moves++;
    }
    return moves;
}

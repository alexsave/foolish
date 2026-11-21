// gd_botlib - small pieces the bots share, in the fixed bank 0 so a brain in
// any bank can call them.
#ifndef GD_BOTLIB_H
#define GD_BOTLIB_H

#include <stdint.h>

// foolish's getTrumpAttackProbability as an integer: an attack with trumps
// goes ahead when the 16-bit draw r is below this. Exactly `r / 65536.0 < p`
// for foolish's double p (tests/difftest.c checks every case). `early` is
// "0.02 while the deck lives" (handwritten_prod reads only the face-up trump,
// the arena handwritten the whole stock); else p = 0.65 + 0.35 x discard /
// total cards, held to [0.5, 0.95].
uint16_t gd_trump_threshold(uint8_t early, uint8_t discard, uint8_t total);

#endif

// gd_bot_hw - New York: foolish's handwritten_prod bot
// (foolish/c/src/handwritten_prod_strategy.c) ported to the Game Boy with
// integers in place of its doubles. Portable C, like gd_rules.
//
// Where foolish breaks a tie by its menu's enumeration order, this port breaks
// it the same way for a hand held in card id order (the order gd_rules keeps).
// Its three random draws come from the one RNG (gd_rng) as 16-bit values u =
// r / 65536 in place of foolish's double in [0, 1).
#ifndef GD_BOT_HW_H
#define GD_BOT_HW_H

#include "gd_rules.h"

// The move New York makes for `seat`; out->type is 0xFF when it has none.
void gd_hw_choose(const gd_game *g, uint8_t seat, gd_move *out) GD_BANKED;
// How many draws (gd_rng_below(65536)) the last gd_hw_choose made.
extern uint8_t gd_hw_draws;

// The trump-attack gate as an integer: an attack with trumps goes ahead when
// the 16-bit draw r is below this. Exactly `r / 65536.0 < p` for foolish's
// double p (tests/hwtest.c checks every case). Exposed for that check.
uint16_t gd_hw_trump_threshold(uint8_t has_flipped, uint8_t discard, uint8_t total);

// The soft double: JS / IEEE double multiplication of positive integers,
// rounded to 53 bits (round half to even) after every multiply, as foolish's
// cover choice computes its products. Exposed for its check.
// While the product fits 32 bits it is kept exact in x (exact in a double too,
// being under 2^53); past that, as m * 2^e with 2^52 <= m < 2^53.
typedef struct { uint32_t x; uint8_t big; uint8_t m[8]; int16_t e; } gd_dbl;
void    gd_dbl_one(gd_dbl *d);
void    gd_dbl_mul(gd_dbl *d, uint16_t f);
int8_t  gd_dbl_cmp(const gd_dbl *a, const gd_dbl *b); // -1, 0, 1

#endif

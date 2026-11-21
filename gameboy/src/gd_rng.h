// gd_rng - THE one random source of the game.
//
// Every random draw (the shuffle, the no-trump opening roll, the bot's move)
// goes through gd_rng_below, from one 16-bit xorshift state seeded by
// gd_rng_seed. There is no other generator and no other hidden state, and
// tests/rng_ban.sh fails the build on any rand(), random() or time based seed.
//
// Portable C: no GBDK headers. 16-bit state, so it is cheap on the SM83.
#ifndef GD_RNG_H
#define GD_RNG_H

#include <stdint.h>

// Seed the generator. Any value is accepted; 0 (a dead state for xorshift) is
// mapped to a fixed nonzero state.
void gd_rng_seed(uint16_t seed);

// The raw next output, 1..65535.
uint16_t gd_rng_next(void);

// The whole state, to save and restore (a test replays a stretch of draws).
uint16_t gd_rng_get(void);
void gd_rng_set(uint16_t state);

// A uniform 16-bit value: exactly gd_rng_below(65536), cheaper.
uint16_t gd_rng_u16(void);

// A uniform value in [0, n), n <= 255, cheaper than gd_rng_below (its own
// stream of values: not the same numbers gd_rng_below(n) would give).
uint8_t gd_rng_small(uint8_t n);
// Its step: 1 and *out when the 16-bit draw r is accepted for n (a test checks
// every r gives each value equally often).
uint8_t gd_rng_small_try(uint16_t r, uint8_t n, uint8_t *out);

// A uniform value in [0, n), n >= 1, by rejection (no modulo bias). Values of n
// above 65535 take two outputs per try.
uint32_t gd_rng_below(uint32_t n);

#endif

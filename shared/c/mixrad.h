// A MIXED-RADIX NUMBER in a byte buffer: the arithmetic under a code whose
// model is the rules.
//
// A game coder stores, at every decision, WHICH of the options the rules list
// there was chosen - an index `i` into a menu of `n` - and the whole history is
// one number in a mixed radix, digit k having base n_k:
//
//     encode:  v = v * n + i        (walked backwards, mixrad_mul_add)
//     decode:  i = v % n;  v /= n   (walked forwards, mixrad_div_mod)
//
// which is exact: it reaches the entropy of the model to within the final
// rounding to a byte, with no renormalisation, no state to flush and no edge
// (an rANS coder was tried first by one of this directory's users and walked
// apart once in every few thousand games because its bound is exact only for
// power-of-two alphabets).
//
// THE NUMBER is little-endian base 256 in `v[0 .. *len)`, and `cap` is how many
// bytes `v` has. The coders that use this start from a sentinel of 1 so the
// number's length is its own and top zero digits cannot be lost; that choice is
// theirs, not this file's.
//
// A digit's base must be at least 1 and below 2^23, so `byte * base + carry`
// never leaves 32 bits. Freestanding: no allocation, no libc.
#ifndef SHARED_MIXRAD_H
#define SHARED_MIXRAD_H

#include <stdint.h>

// v = v * base + digit. 1, or 0 if the result would not fit in `cap` bytes
// (v is then partly updated and must be thrown away).
int mixrad_mul_add(uint8_t *v, int *len, int cap, uint32_t base, uint32_t digit);

// v /= base, returning the remainder. Leading (most significant) zero bytes
// are dropped, so *len shrinks as the number does.
uint32_t mixrad_div_mod(uint8_t *v, int *len, uint32_t base);

#endif

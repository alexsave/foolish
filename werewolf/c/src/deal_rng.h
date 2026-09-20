// deal_rng — a crypto-grade, deterministic CSPRNG for dealing the deck.
//
// WHY THIS EXISTS. The game LCG (game_random in game.c) has 32 bits of state,
// so a whole deal is a function of one 32-bit seed: at most 2^32 (~4.3e9)
// distinct deals. A 52-card deck has 52! ~ 8.1e67 arrangements (225.6 bits),
// a 36-card deck 36! ~ 3.7e41 (138 bits) — the LCG can address a ~5.3e-59
// sliver of the 52-card space. This generator lifts the deal to the full
// universe AND makes it reproducible from a stored seed.
//
// THE SEED IS TWO 128-BIT VALUES. deal_rng takes 32 bytes = 256 bits = two
// 128-bit lanes, which is exactly a ChaCha key. 256 bits clears the 226 a
// 52-card deck needs with margin. Same bytes in -> same deal out, on any
// platform (integer-only, no float — unlike the LCG's game_random()).
//
// WHY CHACHA (not a fast non-crypto PRNG). A player legitimately sees SOME
// outputs of this stream — their own dealt cards are draws from it. A
// reversible generator (LCG, xoshiro) can be run backwards from a few outputs
// to recover state and compute every other hand. A crypto stream makes that
// worthless. ChaCha is also what backs the OS CSPRNGs the seed comes from.
//
// Freestanding: uint32 arithmetic only, no libc, links into the wasm kernel.

#ifndef DEAL_RNG_H
#define DEAL_RNG_H

#include <stdint.h>

typedef struct {
    uint32_t state[16];  // ChaCha state: 4 const | 8 key | 2 counter | 2 nonce
    uint32_t block[16];  // current 64-byte keystream block
    int      used;       // words consumed from block (16 => regenerate)
} DealRng;

// Seed from 32 bytes (two 128-bit lanes, little-endian into the key words).
// Counter and nonce start at zero, so a given seed always yields the same
// keystream — that is the reproducibility contract.
void deal_rng_seed(DealRng *r, const uint8_t seed[32]);

// Next 32 bits of keystream.
uint32_t deal_rng_u32(DealRng *r);

// Uniform in [0, n) with NO modulo bias (rejection sampling). n>=1; n<=1 -> 0.
// This is the unbiased replacement for `(int)(game_random() * n)`.
uint32_t deal_rng_bounded(DealRng *r, uint32_t n);

// Fill the 16-word block from a fully-populated state[] (const|key|ctr|nonce).
// Exposed for the RFC 8439 known-answer test; normal callers use the API above.
void deal_rng_block(const uint32_t state_in[16], uint32_t out[16]);

#endif

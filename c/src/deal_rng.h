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

// Seed at an ARBITRARY point in the same keystream: identical to deal_rng_seed
// followed by throwing away `block` 64-byte blocks (16 u32 draws each), but
// O(1) instead of O(block), because ChaCha's block counter is an INPUT to the
// block function and not a state that has to be walked to.
//
// WHY THIS EXISTS. deal_rng_seed gives a SEQUENTIAL stream, which is exactly
// the shape a deal wants - the deck is drawn once, in order, by whoever deals
// it. Indexing the stream is the other shape: it makes a draw a pure function
// of (seed, index), so a device that has never computed draws 1..N-1 can still
// agree with everyone else about draw N. That is what lets a state carry an
// index instead of a walked generator, and what makes a random value
// recomputable from a replay without replaying the randomness.
//
// The two are one keystream, not two: deal_rng_seed_at(r, s, 0) is exactly
// deal_rng_seed(r, s), and block B's words are the ones a sequential reader
// reaches after B*16 draws. The index is 64 bits, the full width of ChaCha's
// counter.
void deal_rng_seed_at(DealRng *r, const uint8_t seed[32], uint64_t block);

// Next 32 bits of keystream.
uint32_t deal_rng_u32(DealRng *r);

// Uniform in [0, n) with NO modulo bias (rejection sampling). n>=1; n<=1 -> 0.
// This is the unbiased replacement for `(int)(game_random() * n)`.
uint32_t deal_rng_bounded(DealRng *r, uint32_t n);

// Fill the 16-word block from a fully-populated state[] (const|key|ctr|nonce).
// Exposed for the RFC 8439 known-answer test; normal callers use the API above.
void deal_rng_block(const uint32_t state_in[16], uint32_t out[16]);

#endif

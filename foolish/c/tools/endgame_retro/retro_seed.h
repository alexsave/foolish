// retro_seed.h - the 32 deal-seed bytes a crawl index names, for every
// endgame_retro tool that takes an index (find_crawl, narrow_path, trace_path),
// so an index one tool prints is the game another one replays.
//
// The index is spread over 64 bits, then each byte is the low byte of one
// xorshift64 step (rng.h). The derivation is what makes a printed index mean
// a game, so it does not change.
#ifndef RETRO_SEED_H
#define RETRO_SEED_H

#include <stdint.h>
#include "../../../../shared/c/rng.h"

static inline void retro_seed(long idx, uint8_t seed[32])
{
    uint64_t x = (uint64_t)idx * RNG_GOLDEN + 0x1234567;
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(rng_xorshift64(&x) & 0xff);
}

#endif

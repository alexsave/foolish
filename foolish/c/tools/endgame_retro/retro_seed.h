// retro_seed.h - the 32 deal-seed bytes a crawl index names, for every
// endgame_retro tool that takes an index (find_crawl, narrow_path, trace_path),
// so an index one tool prints is the game another one replays.
//
// Each byte is the top byte of one rng.h draw from a stream keyed by the
// index. The derivation is what makes a printed index mean a game, so every
// tool takes it from here.
#ifndef RETRO_SEED_H
#define RETRO_SEED_H

#include <stdint.h>
#include "../../../../shared/c/rng.h"

static inline void retro_seed(long idx, uint8_t seed[32])
{
    uint64_t x = rng_seed64((uint64_t)idx);
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(rng_splitmix64(&x) >> 56);
}

#endif

#ifndef FOOLYARD_RNG_H
#define FOOLYARD_RNG_H

#include "types.h"

// xorshift64, tiltyard's generator. Every holder of a stream keeps its own u64
// so a client's decisions never shift because the network drew a number.
u64 rng_seed(u64 seed);          // never returns 0, and scrambles a small seed
u32 rng_next(u64 *state);
u32 rng_below(u64 *state, u32 n);
u32 rng_pct(u64 *state, u32 pct); // 1 with probability pct/100

#endif

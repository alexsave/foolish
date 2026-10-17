#ifndef FOOLYARD_RNG_H
#define FOOLYARD_RNG_H

#include "types.h"
#include "../../../shared/c/rng.h"

// xorshift64 (rng.h), tiltyard's generator. Every holder of a stream keeps its
// own u64 so a client's decisions never shift because the network drew a
// number. A stream starts at rng_seed64(seed): never 0, and a small seed is
// scrambled. What is foolyard's is how a draw becomes a number:
u32 rng_next(u64 *state);         // the top 32 bits of one step
u32 rng_below(u64 *state, u32 n);
u32 rng_pct(u64 *state, u32 pct); // 1 with probability pct/100

#endif

#include "rng.h"

u64 rng_seed(u64 seed) {
    // splitmix64 once, so seeds 1, 2, 3 give unrelated streams
    seed += 0x9E3779B97F4A7C15ull;
    seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
    seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
    seed ^= seed >> 31;
    return seed ? seed : 0x2545F4914F6CDD1Dull;
}

u32 rng_next(u64 *state) {
    u64 x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return (u32)(x >> 32);
}

u32 rng_below(u64 *state, u32 n) {
    return n ? rng_next(state) % n : 0;
}

u32 rng_pct(u64 *state, u32 pct) {
    if (pct == 0) return 0;
    if (pct >= 100) return 1;
    return rng_below(state, 100) < pct;
}

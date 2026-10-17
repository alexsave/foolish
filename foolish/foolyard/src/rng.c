#include "rng.h"

u32 rng_next(u64 *state) {
    return (u32)(rng_xorshift64(state) >> 32);
}

u32 rng_below(u64 *state, u32 n) {
    return n ? rng_next(state) % n : 0;
}

u32 rng_pct(u64 *state, u32 pct) {
    if (pct == 0) return 0;
    if (pct >= 100) return 1;
    return rng_below(state, 100) < pct;
}

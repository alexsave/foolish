// gd_rng - see gd_rng.h. xorshift16 (7, 9, 8): period 65535 over nonzero states.
#include "gd_rng.h"

static uint16_t gd_rng_state = 0xACE1u;

void gd_rng_seed(uint16_t seed) {
    gd_rng_state = seed ? seed : 0xACE1u;
}

uint16_t gd_rng_get(void) { return gd_rng_state; }
void gd_rng_set(uint16_t state) { gd_rng_state = state; }

uint16_t gd_rng_next(void) {
    uint16_t x = gd_rng_state;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    gd_rng_state = x;
    return x;
}

// One output, 0..65534 (65535 equally likely values).
#define GD_RNG_SPAN 65535u

uint32_t gd_rng_below(uint32_t n) {
    if (n <= 1) return 0;
    if (n <= GD_RNG_SPAN) {
        const uint16_t lim = (uint16_t)(GD_RNG_SPAN - (GD_RNG_SPAN % (uint16_t)n));
        uint16_t r;
        do { r = (uint16_t)(gd_rng_next() - 1u); } while (r >= lim);
        return (uint32_t)(r % (uint16_t)n);
    } else {
        // Two outputs: 65535^2 = 4294836225 equally likely values.
        const uint32_t span = (uint32_t)GD_RNG_SPAN * GD_RNG_SPAN;
        const uint32_t lim = span - (span % n);
        uint32_t r;
        do {
            r = (uint32_t)(gd_rng_next() - 1u) * GD_RNG_SPAN;
            r += (uint32_t)(gd_rng_next() - 1u);
        } while (r >= lim);
        return r % n;
    }
}

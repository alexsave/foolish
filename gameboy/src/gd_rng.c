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

// The bots' 16-bit draw: what the two-output path of gd_rng_below gives for
// n = 65536, made cheap and kept exact. r = a * 65535 + b, and 65535 = -1
// (mod 65536), so r mod 65536 = (b - a) mod 65536; the general path rejects
// only r = 65535^2 - 1 (since 65535^2 mod 65536 = 1), which is a = b = 65534.
// The two xorshift steps are inline: this is the rollouts' hottest draw.
uint16_t gd_rng_u16(void) {
    uint16_t x = gd_rng_state, a, b;
    do {
        x ^= (uint16_t)(x << 7); x ^= (uint16_t)(x >> 9); x ^= (uint16_t)(x << 8);
        a = (uint16_t)(x - 1u);
        x ^= (uint16_t)(x << 7); x ^= (uint16_t)(x >> 9); x ^= (uint16_t)(x << 8);
        b = (uint16_t)(x - 1u);
    } while (a == 65534u && b == 65534u);
    gd_rng_state = x;
    return (uint16_t)(b - a);
}

// A uniform value in [0, n) for a small n, without the two 16-bit modulos of
// gd_rng_below's path (Lemire's method: the high part of r * n, rejecting the
// few r whose low part falls under (65536 - n) mod n; that modulo is taken
// only when the low part is under n, so almost never).
uint8_t gd_rng_small_try(uint16_t r, uint8_t n, uint8_t *out) {
    const uint16_t hi = (uint16_t)((uint8_t)(r >> 8) * (uint16_t)n);     // r * n = hi * 256 + lo
    const uint16_t lo = (uint16_t)((uint8_t)r * (uint16_t)n);
    const uint16_t mid = (uint16_t)(hi + (lo >> 8));
    const uint16_t low16 = (uint16_t)((uint16_t)(mid << 8) | (uint8_t)lo);   // r * n mod 65536
    *out = (uint8_t)(mid >> 8);                                              // r * n / 65536
    return low16 >= n || low16 >= (uint16_t)((uint16_t)(0u - n) % n);
}

uint8_t gd_rng_small(uint8_t n) {
    uint8_t v;
    if (n <= 1) return 0;
    while (!gd_rng_small_try(gd_rng_u16(), n, &v)) {}
    return v;
}

uint32_t gd_rng_below(uint32_t n) {
    if (n <= 1) return 0;
    if (n <= GD_RNG_SPAN) {
        const uint16_t lim = (uint16_t)(GD_RNG_SPAN - (GD_RNG_SPAN % (uint16_t)n));
        uint16_t r;
        do { r = (uint16_t)(gd_rng_next() - 1u); } while (r >= lim);
        return (uint32_t)(r % (uint16_t)n);
    } else if (n == 65536ul) {
        return gd_rng_u16();
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

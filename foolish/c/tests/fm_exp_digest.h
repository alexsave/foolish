// The fm_exp bit digest, shared by the native suite (tests.c) and the wasm
// module `make wasm-fm-exp-test` builds (tests/fm_exp_wasm_test.c), so both
// builds hash the same inputs the same way and are held to one committed value.
//
// The inputs are made from bits, not arithmetic: a splitmix64 stream picks a
// sign, a biased exponent in [963, 1032] (|x| from 2^-60 to just under 1024,
// log-spread, so tiny arguments, the softmax's x <= 0, the positive side and
// both clamps are all hit) and a mantissa. The first eight are fixed edge
// cases.
#ifndef CNITRO_FM_EXP_DIGEST_H
#define CNITRO_FM_EXP_DIGEST_H

#include <stdint.h>
#include <string.h>
#include "fm_exp.h"
#include "../../../shared/c/rng.h"

// fm_exp_digest(FM_EXP_DIGEST_N) on every build. Changing fm_exp changes this
// on purpose: the new value is the one the native and the wasm build agree on.
#define FM_EXP_DIGEST_N 100000
#define FM_EXP_DIGEST   UINT64_C(0xBDE7190BAF003B4B)

static inline uint64_t fmd_bits(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }
static inline double fmd_double(uint64_t u) { double d; memcpy(&d, &u, sizeof d); return d; }

static inline double fm_exp_digest_input(int i) {
    static const uint64_t edge[8] = {
        UINT64_C(0x0000000000000000),   // +0
        UINT64_C(0x8000000000000000),   // -0
        UINT64_C(0xC086200000000000),   // -708, the low clamp
        UINT64_C(0x4086280000000000),   // 709, the high clamp
        UINT64_C(0xFFF0000000000000),   // -inf
        UINT64_C(0x7FF0000000000000),   // +inf
        UINT64_C(0xBFE62E42FEFA39EF),   // -ln2
        UINT64_C(0xBFD62E42FEFA39EF),   // -ln2/2, the reduction's edge
    };
    if (i < 8) return fmd_double(edge[i]);
    uint64_t s = UINT64_C(0xF00115E) + (uint64_t)i;
    const uint64_t a = rng_splitmix64(&s), b = rng_splitmix64(&s);
    const uint64_t sign = (a & 3) ? UINT64_C(1) << 63 : 0;    // 3 in 4 negative
    const uint64_t bexp = 963 + (b % 70);
    return fmd_double(sign | (bexp << 52) | (a >> 12));
}

static inline uint64_t fm_exp_digest(int n) {
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    for (int i = 0; i < n; i++) {
        const uint64_t u = fmd_bits(fm_exp(fm_exp_digest_input(i)));
        for (int k = 0; k < 8; k++) { h ^= (u >> (8 * k)) & 0xFF; h *= UINT64_C(0x100000001B3); }
    }
    return h;
}

#endif

// fm_exp: exp in 64-bit integer fixed point. See fm_exp.h for why.
//
// x = n*ln2 + r, |r| <= ln2/2, so exp(x) = 2^n * exp(r):
//   1. x's bits become X = x * 2^53, an exact-or-rounded int64 (|x| < 709).
//   2. n = nearest(X / ln2), r = X - n*ln2 with ln2 carried to 106 bits, so the
//      reduction loses nothing that matters even at |n| = 1023. r is kept in
//      Q62 (2^62 is 1.0).
//   3. exp(r) by a 16-term Horner in Q62: |r|^17/17! < 2^-66, below the
//      fixed point's own resolution.
//   4. The Q62 value in [0.70, 1.42] is rounded to 53 bits (half to even) and
//      2^n goes straight into the exponent field.
// Every step is an integer operation with one defined answer, so a clang
// native build, a gcc build, a build with FP contraction on and the wasm build agree to
// the bit. No double arithmetic happens anywhere in this file.
#include "fm_exp.h"
#include <string.h>

// ln2 * 2^53 = LN2_A + LN2_B * 2^-53 (LN2_B rounded): 106 bits of ln2.
#define LN2_A INT64_C(6243314768165359)
#define LN2_B INT64_C(1881434294738960)
#define Q62_ONE (UINT64_C(1) << 62)

// Round v / 2^s to nearest (half away from zero), 1 <= s <= 62. Written on
// the magnitude so no right shift of a negative value is ever taken.
static int64_t fm_shr_round(int64_t v, int s) {
    const uint64_t half = UINT64_C(1) << (s - 1);
    if (v >= 0) return (int64_t)(((uint64_t)v + half) >> s);
    return -(int64_t)(((uint64_t)(-v) + half) >> s);
}

// (a * b) / 2^62 rounded to nearest, for a, b < 2^63, by 32-bit limbs (no
// __int128: the freestanding wasm build has no compiler-rt to lower it).
static uint64_t fm_mul_q62(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32;
    const uint64_t b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    uint64_t lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    // + 2^61, the rounding half, carried into hi.
    const uint64_t lo2 = lo + (UINT64_C(1) << 61);
    if (lo2 < lo) hi++;
    lo = lo2;
    return (hi << 2) | (lo >> 62);
}

static double fm_from_bits(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

double fm_exp(double x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    const int neg = (int)(bits >> 63);
    const uint64_t mag = bits & ~(UINT64_C(1) << 63);
    const int bexp = (int)(mag >> 52);              // biased exponent field

    if (mag > UINT64_C(0x7FF0000000000000)) return x;          // NaN
    // |x| < 2^-54: exp(x) rounds to 1.0.
    if (bexp < 1023 - 54) return fm_from_bits(UINT64_C(0x3FF0000000000000));
    // |x| >= 708 (exponent >= 9 and the mantissa past 708 or 709). 708 is
    // 0x4086200000000000 and 709 is 0x4086280000000000.
    if (neg && mag >= UINT64_C(0x4086200000000000)) return fm_from_bits(0);
    if (!neg && mag >= UINT64_C(0x4086280000000000))
        return fm_from_bits(UINT64_C(0x7FF0000000000000));

    // 1. X = |x| * 2^53, rounded. |x| = m * 2^(bexp - 1075), m 53 bits with
    //    the implicit one (x here is normal: bexp >= 969).
    const uint64_t m = (mag & ((UINT64_C(1) << 52) - 1)) | (UINT64_C(1) << 52);
    const int sh = bexp - 1075 + 53;                // X = m * 2^sh
    int64_t X;
    if (sh >= 0) X = (int64_t)(m << sh);            // sh <= 10: X < 709 * 2^53 < 2^63
    else if (sh > -64) X = fm_shr_round((int64_t)m, -sh);
    else X = 0;
    if (neg) X = -X;

    // 2. n = nearest(X / ln2); r = X - n*ln2, in Q62.
    const int64_t n = X >= 0 ? (X + LN2_A / 2) / LN2_A
                             : -((-X + LN2_A / 2) / LN2_A);
    const int64_t r53 = X - n * LN2_A;              // |r53| <= LN2_A / 2 + 1
    const int64_t r62 = r53 * 512 - fm_shr_round(n * LN2_B, 44);

    // 3. exp(r) = 1 + r(1 + r/2(1 + r/3(...))), Horner from k = 16 down.
    const int rneg = r62 < 0;
    const uint64_t ra = (uint64_t)(rneg ? -r62 : r62);
    uint64_t s = Q62_ONE;
    for (uint64_t k = 16; k >= 1; k--) {
        const uint64_t t = (fm_mul_q62(ra, s) + k / 2) / k;
        s = rneg ? Q62_ONE - t : Q62_ONE + t;
    }

    // 4. s * 2^(n - 62) as a double. s is in [0.70, 1.42] * 2^62, so its top
    //    bit is bit 62 or bit 61; keep 53 bits, round half to even.
    const int top = (s >> 62) ? 62 : 61;
    const int drop = top - 52;                      // 9 or 10 low bits go
    uint64_t mant = s >> drop;
    const uint64_t rest = s & ((UINT64_C(1) << drop) - 1);
    const uint64_t halfway = UINT64_C(1) << (drop - 1);
    if (rest > halfway || (rest == halfway && (mant & 1))) mant++;
    int e2 = top + (int)n - 62;                     // value = mant * 2^(e2 - 52)
    if (mant >> 53) { mant >>= 1; e2++; }           // rounding carried out
    // x in (-708, 709) keeps e2 in [-1022, 1023]: always a normal double.
    const uint64_t out = ((uint64_t)(e2 + 1023) << 52) | (mant & ((UINT64_C(1) << 52) - 1));
    return fm_from_bits(out);
}

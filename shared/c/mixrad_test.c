/* mixrad_test.c - mixed-radix arithmetic (mixrad.h): known values, round
 * trips from the sentinel to a full buffer and back, hostile bytes under
 * division, and the refusals at entry. Deterministic and bounded.
 *
 *   cc -std=c11 -Wall -Wextra -Werror mixrad_test.c mixrad.c -o mixrad_test && ./mixrad_test
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "test/check.h"
#include "mixrad.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static uint32_t pick_base(void)
{
    switch (next() % 6) {
    case 0: return 1;
    case 1: return 2;
    case 2: return MIXRAD_BASE_LIMIT - 1;
    case 3: return 255;
    case 4: return 256;
    default: return 1 + next() % (MIXRAD_BASE_LIMIT - 1);
    }
}

int main(void)
{
    TEST("mixrad: known values");
    uint8_t v[8] = { 1 };
    int len = 1;
    CHECK(mixrad_mul_add(v, &len, 8, 10, 3) && len == 1 && v[0] == 13, "1*10+3");
    CHECK(mixrad_mul_add(v, &len, 8, 256, 7) && len == 2 && v[0] == 7 && v[1] == 13, "13*256+7 = 3335");
    CHECK(mixrad_mul_add(v, &len, 8, 1, 0) && len == 2 && v[0] == 7 && v[1] == 13, "base 1 is a no-op");
    CHECK(mixrad_div_mod(v, &len, 256) == 7 && len == 1 && v[0] == 13, "3335 / 256");
    CHECK(mixrad_div_mod(v, &len, 10) == 3 && len == 1 && v[0] == 1, "13 / 10");
    CHECK(mixrad_div_mod(v, &len, 1) == 0 && len == 1 && v[0] == 1, "base 1 divides to itself");
    uint8_t f[2] = { 0xff, 0xff };
    int fl = 2;
    CHECK(!mixrad_mul_add(f, &fl, 2, 2, 0), "a carry past cap is refused");

    TEST("mixrad: refused at entry, v untouched");
    uint8_t g[4] = { 5, 1, 0x5a, 0x5a };
    int gl = 2;
    CHECK(mixrad_div_mod(g, &gl, 0) == MIXRAD_REFUSED && gl == 2 && g[0] == 5 && g[1] == 1,
          "div by base 0");
    CHECK(mixrad_div_mod(g, &gl, MIXRAD_BASE_LIMIT) == MIXRAD_REFUSED && gl == 2 && g[0] == 5,
          "div by base 2^23");
    CHECK(!mixrad_mul_add(g, &gl, 4, 0, 9) && gl == 2 && g[0] == 5 && g[1] == 1, "mul by base 0");
    CHECK(!mixrad_mul_add(g, &gl, 4, 1u << 25, 1) && gl == 2 && g[0] == 5, "mul by base 2^25");
    int neg = -1;
    CHECK(!mixrad_mul_add(g + 1, &neg, 3, 256, 7) && neg == -1 && g[0] == 5, "mul with *len -1 (no write before v)");
    CHECK(mixrad_div_mod(g, &neg, 7) == MIXRAD_REFUSED && neg == -1, "div with *len -1");
    int over = 5;
    CHECK(!mixrad_mul_add(g, &over, 4, 3, 1) && over == 5 && g[2] == 0x5a, "mul with *len past cap");

    TEST("mixrad: round trips from the sentinel to a full buffer");
    for (int it = 0; it < 3000; it++) {
        uint8_t w[64 + 1];
        int cap = 1 + (int)(next() % 64), wl = 1;
        w[0] = 1;
        w[cap] = 0x5a;
        uint32_t bases[512], digs[512];
        int k = 0;
        while (k < 512) {
            uint32_t b0 = pick_base(), d = next() % b0;
            uint8_t save[64];
            int sl = wl;
            memcpy(save, w, (size_t)wl);
            if (!mixrad_mul_add(w, &wl, cap, b0, d)) { memcpy(w, save, (size_t)sl); wl = sl; break; }
            bases[k] = b0; digs[k] = d; k++;
        }
        CHECK(wl <= cap && w[cap] == 0x5a, "iteration %d: wrote past cap %d", it, cap);
        int ok = 1;
        for (int j = k - 1; j >= 0 && ok; j--) ok = mixrad_div_mod(w, &wl, bases[j]) == digs[j];
        CHECK(ok, "iteration %d: a digit came back wrong", it);
        CHECK(wl == 1 && w[0] == 1, "iteration %d: the sentinel came back as len %d", it, wl);
    }

    TEST("mixrad: division walks over hostile bytes");
    for (int it = 0; it < 3000; it++) {
        uint8_t h[64];
        int hl = (int)(next() % 65), L = hl;
        for (int i = 0; i < hl; i++) h[i] = (uint8_t)next();
        for (int q = 0; q < 50 && L > 0; q++) {
            uint32_t b0 = (next() & 1) ? pick_base() : (next() | 1);
            uint32_t rem = mixrad_div_mod(h, &L, b0);
            if (b0 < MIXRAD_BASE_LIMIT) CHECK(rem < b0, "remainder %u for base %u", rem, b0);
            else CHECK(rem == MIXRAD_REFUSED, "base %u past the limit was not refused", b0);
            CHECK(L >= 0 && L <= hl, "len %d out of 0..%d", L, hl);
        }
    }
    return report("mixrad_test");
}

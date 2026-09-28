/* stats_test.c - the arena statistics (stats.h) and the per-game seed hash
 * (seed_hash.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror stats.c stats_test.c -lm -o stats_test && ./stats_test
 *
 * No -I: the headers are beside this file. Exits 1 on any failure. Every
 * expected value is worked out by hand (or in 40-digit decimal) and written
 * down, never computed by the code under test; the seed bytes are the bytes
 * the arenas' own hashes produced before they were replaced by this one. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "stats.h"
#include "seed_hash.h"

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", what); } } while (0)
#define NEAR(a, b, what) do { double a_ = (a), b_ = (b); checks++; \
    if (!(fabs(a_ - b_) <= 1e-12)) { fails++; printf("FAIL %s: %.17g, want %.17g\n", what, a_, b_); } } while (0)

static void test_sums(void)
{
    /* {2, 4, 4, 5, 10}: sum 25, mean 5, squares 161, so the sum of squared
     * deviations is 161 - 125 = 36; / (n - 1) = 9 exactly (the population
     * variance would be 7.2), and the standard error is sqrt(9 / 5). */
    const double x[5] = { 2, 4, 4, 5, 10 };
    StatSums s = { 0, 0, 0 };
    for (int i = 0; i < 5; i++) stat_add(&s, x[i]);
    OK(s.n == 5 && s.sum == 25 && s.sum2 == 161, "the running sums");
    NEAR(stat_mean(&s), 5, "mean");
    NEAR(stat_variance(&s), 9, "Bessel-corrected variance");
    NEAR(stat_stderr(&s), 1.341640786499873817845504, "standard error sqrt(1.8)");

    StatSums none = { 0, 0, 0 };
    OK(stat_mean(&none) == 0 && stat_variance(&none) == 0 && stat_stderr(&none) == 0, "no observations");
    StatSums one = { 0, 0, 0 };
    stat_add(&one, 3.5);
    OK(stat_mean(&one) == 3.5 && stat_variance(&one) == 0 && stat_stderr(&one) == 0, "one observation");
    /* a constant sample the raw formula takes a hair below zero (five 0.01s
     * give -2.7e-20): clamped to exactly zero, so the error is not NaN */
    StatSums flat = { 0, 0, 0 };
    for (int i = 0; i < 5; i++) stat_add(&flat, 0.01);
    OK((flat.sum2 - flat.sum * flat.sum / flat.n) < 0, "the raw formula does go negative here");
    OK(stat_variance(&flat) == 0 && stat_stderr(&flat) == 0, "a constant sample has no spread");
}

static void test_wilson(void)
{
    double lo, hi;
    const double z = STAT_Z95, z2 = 3.841458881296;   /* 1.959964 squared, by hand */
    stat_wilson(0, 0, z, &lo, &hi);
    OK(lo == 0 && hi == 1, "no trials is [0, 1]");
    /* k = 0: the interval is [0, z^2 / (n + z^2)]; k = n: [n / (n + z^2), 1] */
    stat_wilson(0, 1, z, &lo, &hi);
    NEAR(lo, 0, "n=1 k=0 lo");
    NEAR(hi, 0.7934506882081972589390610, "n=1 k=0 hi = z^2 / (1 + z^2)");
    stat_wilson(1, 1, z, &lo, &hi);
    NEAR(lo, 0.2065493117918027410609390, "n=1 k=1 lo = 1 / (1 + z^2)");
    NEAR(hi, 1, "n=1 k=1 hi");
    stat_wilson(0, 40, z, &lo, &hi);
    NEAR(lo, 0, "k=0 lo");
    NEAR(hi, z2 / (40 + z2), "k=0 hi");
    stat_wilson(40, 40, z, &lo, &hi);
    NEAR(lo, 40 / (40 + z2), "k=n lo");
    NEAR(hi, 1, "k=n hi");
    /* k = 5 of 20 at z = 2: (k + z^2/2 -+ z sqrt(k(n-k)/n + z^2/4)) / (n + z^2)
     * = (7 -+ 2 sqrt(4.75)) / 24 */
    stat_wilson(5, 20, 2, &lo, &hi);
    NEAR(lo, 0.1100458773524719353234591, "5 of 20 at z=2, lo");
    NEAR(hi, 0.4732874559808613980098742, "5 of 20 at z=2, hi");
    /* 37 of 100 at the 95% z, the same closed form in 40-digit decimal */
    stat_wilson(37, 100, z, &lo, &hi);
    NEAR(lo, 0.2818236047080924008909132, "37 of 100 at 95%, lo");
    NEAR(hi, 0.4677947049718466283964881, "37 of 100 at 95%, hi");
    /* a half-point score is allowed and lands between its neighbours */
    double lo_a, hi_a, lo_b, hi_b;
    stat_wilson(37, 100, z, &lo_a, &hi_a);
    stat_wilson(38, 100, z, &lo_b, &hi_b);
    stat_wilson(37.5, 100, z, &lo, &hi);
    OK(lo > lo_a && lo < lo_b && hi > hi_a && hi < hi_b, "a fractional score");
}

static int hex_is(const uint8_t b[32], const char *want)
{
    char got[65];
    for (int i = 0; i < 32; i++) snprintf(got + 2 * i, 3, "%02x", b[i]);
    return !strcmp(got, want);
}

static void test_seed_hash(void)
{
    uint8_t s[32];
    /* The key form, as the test helpers spelled it (key 0 and 999) ... */
    seed_hash32(0, s);
    OK(hex_is(s, "f465b9a16a9e786e4f450980185dc406ec814c72a8b88bf89b74a8516a89391b"), "key 0");
    seed_hash32(999, s);
    OK(hex_is(s, "e1295342232ffa2c2d941f213c51addc758bc1355c829b7b88a122c088501dad"), "key 999");
    /* ... and as a line-up arena folds (players, line-up, game) into one key:
     * n * 1000003 + l * 7919 + i * 104729 */
    seed_hash32(2ull * 1000003ull, s);
    OK(hex_is(s, "f4b019e51e767de888f72223ee6c1ccecb3c1bcfca719240f9986f65a42dc772"), "key (2, 0, 0)");
    seed_hash32(8ull * 1000003ull + 3ull * 7919ull + 999ull * 104729ull, s);
    OK(hex_is(s, "8d18d0e5d7d05444717887008dd87079cc42133051f268bf6408315c7b9e2b87"), "key (8, 3, 999)");
    /* The state form, from seed * FNV prime + game, and the stream carries on */
    uint64_t x = 1ull * 0x100000001B3ull + 0;
    seed_hash32_from(&x, s);
    OK(hex_is(s, "a8132d59893bee7fa40100948fc6ec706b8c0817de03ee5d63abaaf9d780de65"), "state (1, 0)");
    uint64_t r = seed_splitmix64(&x);
    OK((r ^ x) == 0xebb9962540942d15ull, "state (1, 0): the fifth draw");
    x = 2ull * 0x100000001B3ull + 999;
    seed_hash32_from(&x, s);
    OK(hex_is(s, "c09b0e8876ece21a70e34b5693981c66b9dfa32e42a1fabbc1238b02e4b374a6"), "state (2, 999)");
    r = seed_splitmix64(&x);
    OK((r ^ x) == 0xa71d47a0b17ea375ull, "state (2, 999): the fifth draw");
    /* splitmix64 from state 0, the published first output */
    x = 0;
    OK(seed_splitmix64(&x) == 0xe220a8397b1dcdafull, "splitmix64(0) first output");
}

int main(void)
{
    test_sums();
    test_wilson();
    test_seed_hash();
    printf("stats: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

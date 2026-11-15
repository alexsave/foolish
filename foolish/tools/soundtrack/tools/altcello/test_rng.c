/* rng: first outputs match an independent reference implementation (python,
 * xoshiro256** seeded by splitmix64 from ALTCELLO_SEED), gauss has mean 0 std 1,
 * uniform stays in [0,1). */
#include "rng.h"
#include "test_common.h"
#include <math.h>

int main(void)
{
    Rng r;
    rng_seed(&r, 0x5eedce110ULL);
    const uint64_t ref[4] = {0x397fdf80d5af1fd3ULL, 0x85814d5ab8a9a5bcULL, 0x8496bc0cb90e2af1ULL, 0xe1fd7d5fe2aba6d4ULL};
    for (int i = 0; i < 4; i++) {
        uint64_t v = rng_next(&r);
        CHECK(v == ref[i], "output %d = %llx, want %llx", i, (unsigned long long)v, (unsigned long long)ref[i]);
    }
    rng_seed(&r, ALTCELLO_SEED);
    double s = 0, s2 = 0;
    int n = 200000;
    for (int i = 0; i < n; i++) {
        double g = rng_gauss(&r);
        s += g;
        s2 += g * g;
    }
    double mean = s / n, var = s2 / n - mean * mean;
    printf("gauss mean %.4f var %.4f\n", mean, var);
    CHECK(fabs(mean) < 0.01, "gauss mean %g", mean);
    CHECK(fabs(var - 1.0) < 0.02, "gauss var %g", var);
    for (int i = 0; i < 10000; i++) {
        double u = rng_uniform(&r);
        CHECK(u >= 0.0 && u < 1.0, "uniform %g", u);
    }
    TEST_DONE("test_rng");
}

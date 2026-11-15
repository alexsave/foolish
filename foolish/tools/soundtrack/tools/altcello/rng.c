#include "rng.h"
#include <math.h>

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void rng_seed(Rng *r, uint64_t seed)
{
    uint64_t x = seed;
    for (int i = 0; i < 4; i++) r->s[i] = splitmix64(&x);
    r->have_spare = 0;
    r->spare = 0.0;
}

uint64_t rng_next(Rng *r)
{
    uint64_t *s = r->s;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

double rng_uniform(Rng *r)
{
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

double rng_gauss(Rng *r)
{
    if (r->have_spare) {
        r->have_spare = 0;
        return r->spare;
    }
    double u1 = rng_uniform(r);
    double u2 = rng_uniform(r);
    if (u1 < 1e-300) u1 = 1e-300;
    double m = sqrt(-2.0 * log(u1));
    r->spare = m * sin(2.0 * M_PI * u2);
    r->have_spare = 1;
    return m * cos(2.0 * M_PI * u2);
}

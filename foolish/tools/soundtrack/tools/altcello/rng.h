/* The only random source of ALTCELLO: xoshiro256** seeded through splitmix64.
 * Every draw takes an explicit Rng*. The seed is one named value: change the
 * constant below to fix or vary every random draw in the program. */
#ifndef ALTCELLO_RNG_H
#define ALTCELLO_RNG_H
#include <stdint.h>

#define ALTCELLO_SEED 0x5eedce110ULL
/* the one instrument body: its modes are drawn once from this fixed sub-seed */
#define ALTCELLO_BODY_SEED (ALTCELLO_SEED ^ 0xb0d1e5ULL)

typedef struct {
    uint64_t s[4];
    int have_spare;
    double spare;
} Rng;

void rng_seed(Rng *r, uint64_t seed);
uint64_t rng_next(Rng *r);
double rng_uniform(Rng *r); /* [0,1) */
double rng_gauss(Rng *r);   /* mean 0, std 1 (Box-Muller, two draws per pair) */

#endif

// seeded_random.h - the seed and the reseed of core/test/seeded_random.c, the stream a UNIT_SEEDED
// program (c_base/Makefile) draws every random byte from.
#ifndef CB_SEEDED_RANDOM_H
#define CB_SEEDED_RANDOM_H

#include "../../../shared/c/rng.h"

#include <stdint.h>

// THE SEED, the one value to change for a different (still fixed) run.
#define CB_TEST_SEED UINT64_C(0x5eed0f001154)

// Puts the stream at word 0 of `seed`'s sequence. unit_app's setup calls it with
// CB_TEST_SEED + (t_tests + (again << 16)) * RNG_GOLDEN: t.h's test index, and how many times this test
// has set up before (a loop of games, each its own deal). So every test starts from a fixed place,
// whatever ran before it and however many words a threaded test drew.
void cb_test_reseed(uint64_t seed);

#endif

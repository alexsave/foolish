// seeded_random.c - the process's one source of random bytes (uuid.h cb_random_bytes) as a
// function of one seed, for a unit test whose runs must be the same run: a program links it IN
// PLACE OF core/src/util/random.c (c_base/Makefile UNIT_SEEDED), the way the fuzz build's sequence
// target replaces it. Every id, deal seed and bot pick the code under test draws then comes from
// this stream, so the same seed is the same games, the same moves and the same number of checks.
//
// THE SEED is CB_TEST_SEED, the one value to change for a different (still fixed) run. The stream
// is splitmix64 over a counter of 8-byte words: a request takes whole words, so the bytes a request
// gets are a function of the seed and of the requests before it, nothing else. A lock keeps the
// counter whole when threads draw; a test whose threads draw at once gets the same words in an
// order its threads decide, so such a test is fixed only as far as its threads are.
#include "../src/util/uuid.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define CB_TEST_SEED UINT64_C(0x5eed0f001154)

static uint64_t g_next;   // the count of 8-byte words drawn so far
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t splitmix64(uint64_t x) {
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

void cb_random_bytes(void *out, size_t n) {
    unsigned char *p = out;
    pthread_mutex_lock(&g_lock);
    while (n > 0) {
        const uint64_t w = splitmix64(CB_TEST_SEED + g_next++);
        const size_t k = n < sizeof w ? n : sizeof w;
        memcpy(p, &w, k);
        p += k;
        n -= k;
    }
    pthread_mutex_unlock(&g_lock);
}

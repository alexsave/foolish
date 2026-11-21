// playout_host - the clang side: the moves of each playout, and their total,
// which the ROM must match (tests/playout.sh).
#include "playout.h"
#include "../src/gd_rng.h"
#include <stdio.h>
int main(void) {
    unsigned long total = 0, mx = 0, cut = 0;
    gd_rng_seed(PLAYOUT_SEED);
    for (unsigned r = 0; r < PLAYOUT_RUNS; r++) {
        unsigned m = playout_one();
        total += m;
        if (m > mx) mx = m;
        if (m >= PLAYOUT_CAP) cut++;
    }
    printf("%02lx %02lx\n", total & 0xFF, (total >> 8) & 0xFF);
    fprintf(stderr, "clang: %u playouts, %lu moves (%.1f a playout, %lu at most), %lu cut at %u\n",
            PLAYOUT_RUNS, total, (double)total / PLAYOUT_RUNS, mx, cut, PLAYOUT_CAP);
    return 0;
}

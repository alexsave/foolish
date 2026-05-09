// Examine the corpus to understand the loss floor:
//   - how many legal options on average
//   - target distribution
//   - duplicate-state-different-target rate (label noise)
#include "../src/tokenize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    int n_tokens;
    int target;
    int n_legal;
    int tokens[MAX_SEQ_LEN];
    uint8_t legal[NUM_ACTIONS];
} Sample;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/test_corpus.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    char magic[4];
    fread(magic, 1, 4, f);
    uint32_t version;
    fread(&version, sizeof(version), 1, f);

    long n = 0;
    long target_hist[NUM_ACTIONS] = {0};
    long n_stop = 0, n_pickup = 0;
    long sum_legal = 0;
    long min_legal = 99, max_legal = 0;

    Sample s;
    while (1) {
        uint16_t nt, tg; uint8_t nl;
        if (fread(&nt, 2, 1, f) != 1) break;
        fread(&tg, 2, 1, f);
        fread(&nl, 1, 1, f);
        s.n_tokens = nt; s.target = tg; s.n_legal = nl;
        for (int i = 0; i < nt; i++) { int32_t t; fread(&t, 4, 1, f); s.tokens[i] = t; }
        for (int i = 0; i < nl; i++) fread(&s.legal[i], 1, 1, f);

        n++;
        target_hist[tg]++;
        if (tg == ACTION_STOP) n_stop++;
        if (tg == ACTION_PICKUP) n_pickup++;
        sum_legal += nl;
        if (nl < min_legal) min_legal = nl;
        if (nl > max_legal) max_legal = nl;
    }
    fclose(f);

    printf("samples=%ld\n", n);
    printf("avg_legal=%.2f  min=%ld  max=%ld\n",
           (double)sum_legal / n, min_legal, max_legal);
    printf("STOP=%ld (%.1f%%)  PICKUP=%ld (%.1f%%)  cards=%ld (%.1f%%)\n",
           n_stop, 100.0 * n_stop / n,
           n_pickup, 100.0 * n_pickup / n,
           n - n_stop - n_pickup, 100.0 * (n - n_stop - n_pickup) / n);

    // Lower bound on loss assuming uniform over avg_legal:
    double uniform_loss = log((double)sum_legal / n);
    printf("uniform-over-legal loss = ln(avg_legal) = %.4f\n", uniform_loss);

    // Best-case loss if model picks target with prob 1 in every call: 0.
    // Noisy-floor estimate: if target is a uniform pick within a value-group
    // of size k, achievable loss is ln(k) for those samples.

    return 0;
}

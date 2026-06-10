// Microbenchmark: forward and backward throughput on synthetic data so we
// can see clearly where the trainer's time goes.
#include "../src/nn.h"
#include "../src/tokenize.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static double secs(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    int L = argc > 1 ? atoi(argv[1]) : 100;
    int N = argc > 2 ? atoi(argv[2]) : 1000;

    NNParams *p = malloc(sizeof(NNParams));
    NNGrads *g  = malloc(sizeof(NNGrads));
    ForwardCache *fc = malloc(sizeof(ForwardCache));
    nn_init_random(p, 1);
    nn_zero_grads(g);

    int tokens[MAX_SEQ_LEN];
    for (int i = 0; i < L; i++) tokens[i] = (i % VOCAB_SIZE);

    bool legal[NUM_ACTIONS] = { false };
    for (int i = 0; i < 6; i++) legal[i] = true;
    legal[ACTION_STOP] = true;

    // Forward only.
    double t0 = secs();
    for (int n = 0; n < N; n++) nn_forward(p, tokens, L, fc);
    double t1 = secs();
    double fwd_ms = (t1 - t0) * 1000.0 / N;

    // Forward + backward.
    t0 = secs();
    for (int n = 0; n < N; n++) {
        nn_forward(p, tokens, L, fc);
        nn_accumulate_grads(p, fc, legal, 0, g);
    }
    t1 = secs();
    double fwdbwd_ms = (t1 - t0) * 1000.0 / N;

    printf("L=%d N=%d  forward=%.2f ms/sample  fwd+bwd=%.2f ms/sample  bwd=%.2f ms\n",
           L, N, fwd_ms, fwdbwd_ms, fwdbwd_ms - fwd_ms);
    printf("  throughput fwd: %.0f samples/s, fwd+bwd: %.0f samples/s\n",
           1000.0 / fwd_ms, 1000.0 / fwdbwd_ms);
    free(fc); free(g); free(p);
    return 0;
}

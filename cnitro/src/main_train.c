// SGD trainer. Reads the binary corpus produced by main_collect.c, runs
// `epochs` passes with batched SGD, prints loss/top-1 each epoch, and
// saves weights to `out`.
//
// Usage:
//   cnitro_train --corpus=/tmp/cnitro_corpus.bin --out=weights.bin \
//                --epochs=5 --batch=32 --lr=0.05

#include "../src/nn.h"
#include "../src/tokenize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Wall time in seconds since some monotonic epoch. Unlike clock(), this
// keeps ticking when the laptop sleeps — critical for overnight runs.
static double wall_secs(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static const char *get_arg(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0 && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') return argv[i] + 2 + kl + 1;
    }
    return def;
}
static int  parse_int(const char *s, int def) { return s ? atoi(s) : def; }
static float parse_float(const char *s, float def) { return s ? (float)atof(s) : def; }

typedef struct {
    int   n_tokens;
    int   target;
    int   n_legal;
    int   tokens[MAX_SEQ_LEN];
    uint8_t legal[NUM_ACTIONS];
} Sample;

static Sample *load_corpus(const char *path, size_t *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "NCOR", 4) != 0) {
        fprintf(stderr, "bad corpus magic\n"); exit(1);
    }
    uint32_t version;
    if (fread(&version, sizeof(version), 1, f) != 1 || version != 1) {
        fprintf(stderr, "bad corpus version\n"); exit(1);
    }
    size_t cap = 1 << 16, n = 0;
    Sample *arr = malloc(cap * sizeof(Sample));
    if (!arr) { perror("malloc"); exit(1); }
    while (true) {
        uint16_t nt, tg; uint8_t nl;
        size_t r = fread(&nt, sizeof(nt), 1, f);
        if (r != 1) break;
        if (fread(&tg, sizeof(tg), 1, f) != 1) break;
        if (fread(&nl, sizeof(nl), 1, f) != 1) break;
        if (n == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(Sample)); if (!arr) exit(1); }
        Sample *s = &arr[n++];
        s->n_tokens = nt;
        s->target = tg;
        s->n_legal = nl;
        for (int i = 0; i < nt; i++) {
            int32_t t; if (fread(&t, sizeof(t), 1, f) != 1) { exit(1); }
            s->tokens[i] = t;
        }
        for (int i = 0; i < nl; i++) {
            uint8_t a; if (fread(&a, sizeof(a), 1, f) != 1) { exit(1); }
            s->legal[i] = a;
        }
    }
    fclose(f);
    *n_out = n;
    return arr;
}

static uint32_t shuf_seed = 42;
static uint32_t shuf_next(void) {
    shuf_seed = shuf_seed * 1664525u + 1013904223u;
    return shuf_seed;
}

int main(int argc, char **argv) {
    const char *corpus = get_arg(argc, argv, "corpus", "/tmp/cnitro_corpus.bin");
    const char *in_path  = get_arg(argc, argv, "in", NULL);
    const char *out_path = get_arg(argc, argv, "out", "weights.bin");
    int   epochs = parse_int(get_arg(argc, argv, "epochs", "5"), 5);
    int   batch  = parse_int(get_arg(argc, argv, "batch", "32"), 32);
    float lr     = parse_float(get_arg(argc, argv, "lr", "0.05"), 0.05f);
    uint32_t seed = (uint32_t)parse_int(get_arg(argc, argv, "seed", "42"), 42);
    shuf_seed = seed ? seed : 1;
    setvbuf(stderr, NULL, _IOLBF, 0);

    fprintf(stderr, "# loading corpus from %s\n", corpus);
    size_t n_samples = 0;
    Sample *samples = load_corpus(corpus, &n_samples);
    fprintf(stderr, "# loaded %zu samples\n", n_samples);

    NNParams *p = malloc(sizeof(NNParams));
    NNGrads  *g = malloc(sizeof(NNGrads));
    BatchedForwardCache *fc = malloc(sizeof(BatchedForwardCache));
    if (!p || !g || !fc) { perror("malloc"); return 1; }
    if (in_path && nn_load(in_path, p)) {
        fprintf(stderr, "# resumed from %s\n", in_path);
    } else {
        nn_init_random(p, seed);
        fprintf(stderr, "# init: fresh seed=%u\n", seed);
    }
    nn_zero_grads(g);

    // Bucket samples by L. Within each bucket, all samples have the same L
    // so we can run BATCH_MAX of them through nn_forward_batch in one call.
    fprintf(stderr, "# bucketing by L...\n");
    size_t *bucket_starts = calloc(MAX_SEQ_LEN + 2, sizeof(size_t));
    size_t *bucket_counts = calloc(MAX_SEQ_LEN + 2, sizeof(size_t));
    for (size_t i = 0; i < n_samples; i++) {
        int L = samples[i].n_tokens;
        if (L < 1 || L > MAX_SEQ_LEN) continue;
        bucket_counts[L]++;
    }
    // Prefix-sum to get starts.
    {
        size_t off = 0;
        for (int L = 1; L <= MAX_SEQ_LEN; L++) {
            bucket_starts[L] = off; off += bucket_counts[L];
        }
    }
    size_t *sorted_idx = malloc(n_samples * sizeof(size_t));
    {
        size_t *cursor = calloc(MAX_SEQ_LEN + 2, sizeof(size_t));
        for (int L = 1; L <= MAX_SEQ_LEN; L++) cursor[L] = bucket_starts[L];
        for (size_t i = 0; i < n_samples; i++) {
            int L = samples[i].n_tokens;
            if (L < 1 || L > MAX_SEQ_LEN) continue;
            sorted_idx[cursor[L]++] = i;
        }
        free(cursor);
    }

    // Build the list of (bucket_L, count, start_in_sorted_idx) for buckets
    // with >= batch samples — these are what we'll iterate through. Drop
    // partial last batch within each bucket.
    typedef struct { int L; size_t start; size_t n; } Bucket;
    Bucket *buckets = malloc(MAX_SEQ_LEN * sizeof(Bucket));
    int n_buckets = 0;
    size_t total_full_batched = 0;
    for (int L = 1; L <= MAX_SEQ_LEN; L++) {
        size_t n = bucket_counts[L];
        size_t full = (n / batch) * batch;
        if (full == 0) continue;
        buckets[n_buckets].L = L;
        buckets[n_buckets].start = bucket_starts[L];
        buckets[n_buckets].n = full;
        n_buckets++;
        total_full_batched += full;
    }
    fprintf(stderr, "# %d L-buckets, %zu/%zu samples in full batches "
                    "(dropped %zu partial-batch tail)\n",
            n_buckets, total_full_batched, n_samples, n_samples - total_full_batched);

    // Per-batch iteration order: pre-build a list of (bucket_idx, batch_start_in_bucket).
    typedef struct { int bidx; size_t bs; } BatchSlot;
    size_t total_batches = total_full_batched / batch;
    BatchSlot *slots = malloc(total_batches * sizeof(BatchSlot));
    {
        size_t k = 0;
        for (int bi = 0; bi < n_buckets; bi++) {
            size_t nb = buckets[bi].n / batch;
            for (size_t j = 0; j < nb; j++) {
                slots[k].bidx = bi;
                slots[k].bs = buckets[bi].start + j * batch;
                k++;
            }
        }
    }

    // Scratch for one batch.
    int   *batch_tokens = malloc(BATCH_MAX * MAX_SEQ_LEN * sizeof(int));
    bool  *batch_legal  = malloc(BATCH_MAX * NUM_ACTIONS * sizeof(bool));
    int   *batch_target = malloc(BATCH_MAX * sizeof(int));

    double start = wall_secs();
    for (int epoch = 1; epoch <= epochs; epoch++) {
        // Shuffle batch order each epoch (samples within a batch keep their
        // bucket — same L is required for the batched path).
        for (size_t i = total_batches - 1; i > 0; i--) {
            size_t j = shuf_next() % (i + 1);
            BatchSlot t = slots[i]; slots[i] = slots[j]; slots[j] = t;
        }

        double total_loss = 0;
        size_t correct = 0, processed = 0;
        double epoch_loss_for_log = 0;
        size_t since_log = 0;
        for (size_t k = 0; k < total_batches; k++) {
            int L = buckets[slots[k].bidx].L;
            size_t bs = slots[k].bs;
            for (int b = 0; b < (int)batch; b++) {
                Sample *s = &samples[sorted_idx[bs + b]];
                memcpy(batch_tokens + b * L, s->tokens, sizeof(int) * L);
                for (int j = 0; j < NUM_ACTIONS; j++) batch_legal[b * NUM_ACTIONS + j] = false;
                for (int j = 0; j < s->n_legal; j++) batch_legal[b * NUM_ACTIONS + s->legal[j]] = true;
                batch_target[b] = s->target;
            }
            nn_forward_batch(p, batch_tokens, batch, L, fc);
            int batch_correct = 0;
            float batch_loss = nn_accumulate_grads_batch(p, fc, batch_legal,
                                                         batch_target, g, &batch_correct);
            nn_apply_grads(p, g, lr, batch);
            total_loss += batch_loss;
            epoch_loss_for_log += batch_loss;
            correct += batch_correct;
            processed += batch;
            since_log += batch;

            if (since_log >= 5000) {
                double dt = wall_secs() - start;
                fprintf(stderr, "  epoch %d  step %zu/%zu  recentLoss=%.4f  dt=%.1fs\n",
                        epoch, processed, total_full_batched,
                        epoch_loss_for_log / since_log, dt);
                epoch_loss_for_log = 0;
                since_log = 0;
            }
        }
        double avg_loss = total_loss / (processed > 0 ? processed : 1);
        double acc = (double)correct / (processed > 0 ? processed : 1);
        double dt = wall_secs() - start;
        fprintf(stderr, "# epoch %d/%d  avgLoss=%.4f  top1=%.1f%%  dt=%.1fs\n",
                epoch, epochs, avg_loss, acc * 100.0, dt);
        nn_save(out_path, p);
    }

    free(batch_tokens); free(batch_legal); free(batch_target);
    free(slots); free(buckets);
    free(sorted_idx); free(bucket_starts); free(bucket_counts);
    free(samples); free(fc); free(g); free(p);
    return 0;
}

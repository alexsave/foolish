// SGD trainer. Reads the binary corpus produced by main_collect.c, runs
// `epochs` passes with batched SGD, prints loss/top-1 each epoch, and
// saves weights to `out`.
//
// Usage:
//   cnitro_train --corpus=/tmp/cnitro_corpus.bin --out=weights.bin \
//                --epochs=5 --batch=32 --lr=0.05

#include "../src/nn.h"
#include "../src/nn_thread.h"
#include "../src/tokenize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <Accelerate/Accelerate.h>

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

// Bucket / BatchSlot live at file scope so the parallel worker can take them
// by pointer. (Originally locals to main; promoted when we added threading.)
typedef struct { int L; size_t start; size_t n; } Bucket;
typedef struct { int bidx; size_t bs; } BatchSlot;

typedef struct {
    int   *tokens;   // [BATCH_MAX * MAX_SEQ_LEN]
    bool  *legal;    // [BATCH_MAX * NUM_ACTIONS]
    int   *target;   // [BATCH_MAX]
    BatchedForwardCache *fc;
    NNGrads *grads;
    double  loss_sum;
    size_t  correct;
} ThreadScratch;

typedef struct {
    const NNParams *p;
    Sample         *samples;
    size_t         *sorted_idx;
    int             macro_L;          // shared L across all batches in this macro
    size_t          batch0_offset;    // sorted_idx start of first batch in macro
    int             batch_size;
    ThreadScratch  *ts;
} MacroCtx;

// Run by parallel_for: each call processes ONE iteration of the macro and
// accumulates into thread-local grads. With the work-stealing pool the
// same tid can be invoked multiple times in one parallel_for call, so we
// must NOT zero the grads here — master zeros every ts[t].grads once per
// macro-step before dispatching.
static void macro_worker(int start, int end, int tid, void *ctx_) {
    MacroCtx *ctx = (MacroCtx *)ctx_;
    ThreadScratch *t = &ctx->ts[tid];
    int L = ctx->macro_L;
    int B = ctx->batch_size;
    for (int idx = start; idx < end; idx++) {
        size_t bs = ctx->batch0_offset + (size_t)idx * (size_t)B;
        for (int b = 0; b < B; b++) {
            Sample *s = &ctx->samples[ctx->sorted_idx[bs + b]];
            memcpy(t->tokens + b * L, s->tokens, sizeof(int) * L);
            for (int j = 0; j < NUM_ACTIONS; j++) t->legal[b * NUM_ACTIONS + j] = false;
            for (int j = 0; j < s->n_legal; j++) t->legal[b * NUM_ACTIONS + s->legal[j]] = true;
            t->target[b] = s->target;
        }
        nn_forward_batch(ctx->p, t->tokens, B, L, t->fc);
        int batch_correct = 0;
        float batch_loss = nn_accumulate_grads_batch(ctx->p, t->fc, t->legal,
                                                     t->target, t->grads, &batch_correct);
        t->loss_sum += batch_loss;
        t->correct  += (size_t)batch_correct;
    }
}

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
    if (!p || !g) { perror("malloc"); return 1; }
    if (in_path && nn_load(in_path, p)) {
        fprintf(stderr, "# resumed from %s\n", in_path);
    } else {
        nn_init_random(p, seed);
        fprintf(stderr, "# init: fresh seed=%u\n", seed);
    }
    nn_zero_grads(g);

    // Spin up the pthread pool, one worker per online CPU. Each worker runs
    // forward+backward on its own batch with its own scratch state; the
    // master sums per-thread gradients into g and applies a single SGD step
    // per macro-step. Effective batch = n_workers * batch.
    nn_thread_init(0);
    int n_workers = nn_thread_count();
    fprintf(stderr, "# pthread pool: %d workers\n", n_workers);

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

    // Macro-steps: each macro is K_PER_WORKER * n_workers consecutive same-L
    // batches. K>1 means each worker processes K batches per macro before
    // the next sync, so we pay the parallel_for / cond_var sync cost K
    // times less often. Cost: effective batch grows K×, slightly larger
    // gradient noise per SGD step.
    #define K_PER_WORKER 4
    int macro_batches = n_workers * K_PER_WORKER;
    typedef struct { int bidx; size_t batch0_offset; } MacroSlot;
    size_t total_macros = 0;
    for (int bi = 0; bi < n_buckets; bi++) {
        size_t nb = buckets[bi].n / batch;
        total_macros += nb / macro_batches;
    }
    MacroSlot *macros = malloc(total_macros * sizeof(MacroSlot));
    {
        size_t k = 0;
        for (int bi = 0; bi < n_buckets; bi++) {
            size_t nb = buckets[bi].n / batch;
            size_t nm = nb / macro_batches;
            for (size_t j = 0; j < nm; j++) {
                macros[k].bidx = bi;
                macros[k].batch0_offset = buckets[bi].start + j * macro_batches * batch;
                k++;
            }
        }
    }
    size_t macro_total_samples = total_macros * macro_batches * batch;
    fprintf(stderr, "# %zu macro-steps × %d workers × %d batches × %d/batch = %zu samples/epoch "
                    "(macro-quantization dropped %zu vs batches)\n",
            total_macros, n_workers, K_PER_WORKER, (int)batch, macro_total_samples,
            (size_t)(total_full_batched > macro_total_samples ?
                     total_full_batched - macro_total_samples : 0));

    // Per-thread scratch: input buffers, forward cache, gradient buffer, and
    // running stats for the macro-step. Each worker writes to its own slot,
    // no shared mutation during the parallel phase.
    ThreadScratch *ts = calloc(n_workers, sizeof(ThreadScratch));
    for (int t = 0; t < n_workers; t++) {
        ts[t].tokens = malloc(BATCH_MAX * MAX_SEQ_LEN * sizeof(int));
        ts[t].legal  = malloc(BATCH_MAX * NUM_ACTIONS * sizeof(bool));
        ts[t].target = malloc(BATCH_MAX * sizeof(int));
        ts[t].fc     = malloc(sizeof(BatchedForwardCache));
        ts[t].grads  = malloc(sizeof(NNGrads));
        if (!ts[t].tokens || !ts[t].legal || !ts[t].target || !ts[t].fc || !ts[t].grads) {
            perror("malloc"); return 1;
        }
        nn_zero_grads(ts[t].grads);
    }

    // NNGrads is a flat tower of inline float arrays — no pointers, no
    // padding inside the struct — so we can sum it as a contiguous float
    // buffer via vDSP.
    const size_t n_grad_floats = sizeof(NNGrads) / sizeof(float);

    // Tell Accelerate's BLAS not to spawn its own threads — we already split
    // work K-way across our pool, so internal BLAS threads would over-
    // subscribe the cores. (The matmuls in this model are small enough that
    // single-thread BLAS is faster anyway.)
    setenv("VECLIB_MAXIMUM_THREADS", "1", 1);

    double start = wall_secs();
    for (int epoch = 1; epoch <= epochs; epoch++) {
        // Shuffle macro-step order each epoch (within-bucket order is fixed
        // since all batches in a bucket are interchangeable for SGD; the
        // sample ordering within sorted_idx is fixed at load time so two
        // macros that point at the same bucket address different samples).
        for (size_t i = total_macros - 1; i > 0; i--) {
            size_t j = shuf_next() % (i + 1);
            MacroSlot t = macros[i]; macros[i] = macros[j]; macros[j] = t;
        }

        double total_loss = 0;
        size_t correct = 0, processed = 0;
        double epoch_loss_for_log = 0;
        size_t since_log = 0;

        // Macro-step: each macro is K_PER_WORKER * n_workers same-L batches.
        // We dispatch macro_batches iterations through parallel_for; each of
        // n_workers workers handles K_PER_WORKER iterations sequentially.
        // Each worker accumulates K batches' gradients into its own NNGrads
        // (workers reset their grads inside macro_worker), then a single
        // parallel reduction sums + applies. Sync cost is paid 1/K as often.
        for (size_t mk = 0; mk < total_macros; mk++) {
            // Zero every thread's accumulators ONCE per macro. Work stealing
            // means a single tid may be called many times by parallel_for,
            // so the workers can't safely zero their own grads inside.
            for (int t = 0; t < n_workers; t++) {
                ts[t].loss_sum = 0;
                ts[t].correct  = 0;
                nn_zero_grads(ts[t].grads);
            }
            MacroCtx ctx = {
                .p = p, .samples = samples, .sorted_idx = sorted_idx,
                .macro_L = buckets[macros[mk].bidx].L,
                .batch0_offset = macros[mk].batch0_offset,
                .batch_size = (int)batch, .ts = ts,
            };
            parallel_for(macro_batches, macro_worker, &ctx);

            // Master-serial reduce + apply. Avoids a second parallel_for
            // dispatch (its sync overhead exceeded the work it parallelized
            // for our small NNGrads). Each step is a single tight scan of
            // ~85K floats.
            int eff_batch = macro_batches * (int)batch;
            float *master_f = (float *)g;
            memset(master_f, 0, n_grad_floats * sizeof(float));
            for (int t = 0; t < n_workers; t++) {
                vDSP_vadd(master_f, 1, (const float *)ts[t].grads, 1,
                          master_f, 1, n_grad_floats);
            }
            nn_apply_grads(p, g, lr, eff_batch);

            for (int t = 0; t < n_workers; t++) {
                total_loss += ts[t].loss_sum;
                epoch_loss_for_log += ts[t].loss_sum;
                correct += ts[t].correct;
            }
            processed += eff_batch;
            since_log += eff_batch;

            if (since_log >= 5000) {
                double dt = wall_secs() - start;
                fprintf(stderr, "  epoch %d  step %zu/%zu  recentLoss=%.4f  dt=%.1fs\n",
                        epoch, processed, macro_total_samples,
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

    for (int t = 0; t < n_workers; t++) {
        free(ts[t].tokens); free(ts[t].legal); free(ts[t].target);
        free(ts[t].fc); free(ts[t].grads);
    }
    free(ts);
    nn_thread_shutdown();
    free(slots); free(macros); free(buckets);
    free(sorted_idx); free(bucket_starts); free(bucket_counts);
    free(samples); free(g); free(p);
    return 0;
}

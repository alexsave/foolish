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

    fprintf(stderr, "# loading corpus from %s\n", corpus);
    size_t n_samples = 0;
    Sample *samples = load_corpus(corpus, &n_samples);
    fprintf(stderr, "# loaded %zu samples\n", n_samples);

    NNParams *p = malloc(sizeof(NNParams));
    NNGrads  *g = malloc(sizeof(NNGrads));
    ForwardCache *fc = malloc(sizeof(ForwardCache));
    if (!p || !g || !fc) { perror("malloc"); return 1; }
    if (in_path && nn_load(in_path, p)) {
        fprintf(stderr, "# resumed from %s\n", in_path);
    } else {
        nn_init_random(p, seed);
        fprintf(stderr, "# init: fresh seed=%u\n", seed);
    }
    nn_zero_grads(g);

    // Sample-index permutation buffer.
    size_t *perm = malloc(n_samples * sizeof(size_t));
    for (size_t i = 0; i < n_samples; i++) perm[i] = i;

    clock_t start = clock();
    for (int epoch = 1; epoch <= epochs; epoch++) {
        // Fisher-Yates with shuf_next.
        for (size_t i = n_samples - 1; i > 0; i--) {
            size_t j = shuf_next() % (i + 1);
            size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }

        double total_loss = 0;
        size_t correct = 0, processed = 0;
        for (size_t b_start = 0; b_start < n_samples; b_start += batch) {
            size_t b_end = b_start + batch;
            if (b_end > n_samples) b_end = n_samples;
            for (size_t i = b_start; i < b_end; i++) {
                Sample *s = &samples[perm[i]];
                bool legal_mask[NUM_ACTIONS] = { false };
                for (int j = 0; j < s->n_legal; j++) legal_mask[s->legal[j]] = true;
                nn_forward(p, s->tokens, s->n_tokens, fc);
                float probs[NUM_ACTIONS];
                nn_softmax_masked(fc->logits, legal_mask, probs);
                int best = 0; float bp = -1e30f;
                for (int j = 0; j < NUM_ACTIONS; j++) if (probs[j] > bp) { bp = probs[j]; best = j; }
                if (best == s->target) correct++;
                float loss = nn_accumulate_grads(p, fc, legal_mask, s->target, g);
                total_loss += loss;
                processed++;
            }
            nn_apply_grads(p, g, lr, (int)(b_end - b_start));
        }
        double avg_loss = total_loss / (processed > 0 ? processed : 1);
        double acc = (double)correct / (processed > 0 ? processed : 1);
        double dt = (double)(clock() - start) / CLOCKS_PER_SEC;
        fprintf(stderr, "# epoch %d/%d  avgLoss=%.4f  top1=%.1f%%  dt=%.1fs\n",
                epoch, epochs, avg_loss, acc * 100.0, dt);
        nn_save(out_path, p);
    }

    free(perm); free(samples); free(fc); free(g); free(p);
    return 0;
}

// Tiny pre-LN transformer: 2 layers × 1 head × d_model=32, FFN=64.
// Same architecture and weight layout as supabase/.../nitro_nn.ts.
//
// Weights are flat arrays in row-major order so they can be serialized as a
// JSON list of float32s and (de)serialized either side.
#ifndef CNITRO_NN_H
#define CNITRO_NN_H

#include "tokenize.h"
#include <stddef.h>
#include <stdbool.h>

#define D_MODEL   64
#define FF_DIM    128
#define N_LAYERS  2
#define N_HEADS   2
#define D_HEAD    (D_MODEL / N_HEADS)   // 32

// Batch size for the batched forward/backward path used by training.
// Samples are sorted by L at load time so within-batch L is uniform.
#define BATCH_MAX 32

typedef struct {
    float Wq[D_MODEL * D_MODEL];
    float Wk[D_MODEL * D_MODEL];
    float Wv[D_MODEL * D_MODEL];
    float Wo[D_MODEL * D_MODEL];
    float ln1g[D_MODEL]; float ln1b[D_MODEL];
    float Wff1[FF_DIM * D_MODEL]; float bff1[FF_DIM];
    float Wff2[D_MODEL * FF_DIM]; float bff2[D_MODEL];
    float ln2g[D_MODEL]; float ln2b[D_MODEL];
} LayerParams;

typedef struct {
    float embed[VOCAB_SIZE * D_MODEL];
    float pos_embed[MAX_SEQ_LEN * D_MODEL];
    LayerParams layers[N_LAYERS];
    float lnFg[D_MODEL]; float lnFb[D_MODEL];
    float Wout[NUM_ACTIONS * D_MODEL];
    float bout[NUM_ACTIONS];
} NNParams;

// Mirrors NNGrads in TS — same shape as NNParams.
typedef NNParams NNGrads;

// Per-layer activations needed by backward.
typedef struct {
    float xIn[MAX_SEQ_LEN * D_MODEL];      // input to the layer (pre-LN1)
    float xLn1[MAX_SEQ_LEN * D_MODEL];     // output of LN1
    float ln1Mean[MAX_SEQ_LEN]; float ln1Var[MAX_SEQ_LEN];
    float Q[MAX_SEQ_LEN * D_MODEL];
    float K[MAX_SEQ_LEN * D_MODEL];
    float V[MAX_SEQ_LEN * D_MODEL];
    // Per-head: [N_HEADS][L][L] flat. Index as scores[h*L*L + i*L + j].
    float scores[N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN];
    float attn[N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN];
    float attnOut[MAX_SEQ_LEN * D_MODEL];
    float proj[MAX_SEQ_LEN * D_MODEL];
    float afterAttn[MAX_SEQ_LEN * D_MODEL];
    float xLn2[MAX_SEQ_LEN * D_MODEL];
    float ln2Mean[MAX_SEQ_LEN]; float ln2Var[MAX_SEQ_LEN];
    float ff1pre[MAX_SEQ_LEN * FF_DIM];
    float ff1[MAX_SEQ_LEN * FF_DIM];
    float ff2[MAX_SEQ_LEN * D_MODEL];
    float out[MAX_SEQ_LEN * D_MODEL];
} LayerCache;

typedef struct {
    int   tokens[MAX_SEQ_LEN];
    int   L;
    float embedded[MAX_SEQ_LEN * D_MODEL];
    LayerCache layers[N_LAYERS];
    float finalLnIn[D_MODEL];
    float finalLnMean, finalLnVar;
    float cls[D_MODEL];
    float logits[NUM_ACTIONS];
} ForwardCache;

// ----- Batched cache. Same shape as LayerCache/ForwardCache but the leading
// dim is B rather than 1. All buffers are sized to BATCH_MAX × MAX_SEQ_LEN.

typedef struct {
    // [B, L, D_MODEL] flat (sample b, position i, dim d at b*L*D + i*D + d).
    float xIn[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float xLn1[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float ln1Mean[BATCH_MAX * MAX_SEQ_LEN];
    float ln1Var[BATCH_MAX * MAX_SEQ_LEN];
    float Q[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float K[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float V[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    // [B, N_HEADS, L, L] — attention is per-sample.
    float scores[BATCH_MAX * N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN];
    float attn[BATCH_MAX * N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN];
    float attnOut[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float proj[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float afterAttn[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float xLn2[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float ln2Mean[BATCH_MAX * MAX_SEQ_LEN];
    float ln2Var[BATCH_MAX * MAX_SEQ_LEN];
    float ff1pre[BATCH_MAX * MAX_SEQ_LEN * FF_DIM];
    float ff1[BATCH_MAX * MAX_SEQ_LEN * FF_DIM];
    float ff2[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    float out[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
} BatchedLayerCache;

typedef struct {
    int   B;
    int   L;
    int   tokens[BATCH_MAX * MAX_SEQ_LEN];
    float embedded[BATCH_MAX * MAX_SEQ_LEN * D_MODEL];
    BatchedLayerCache layers[N_LAYERS];
    float finalLnIn[BATCH_MAX * D_MODEL];   // CLS-only of last layer's out
    float finalLnMean[BATCH_MAX];
    float finalLnVar[BATCH_MAX];
    float cls[BATCH_MAX * D_MODEL];
    float logits[BATCH_MAX * NUM_ACTIONS];
} BatchedForwardCache;

void  nn_init_random(NNParams *p, uint32_t seed);
void  nn_zero_grads(NNGrads *g);

// Forward pass. cache is filled for use by accumulate_grads.
void  nn_forward(const NNParams *p, const int *tokens, int L, ForwardCache *cache);

// Masked softmax. legal[NUM_ACTIONS]: true = legal.
void  nn_softmax_masked(const float *logits, const bool *legal, float *out);

// Returns cross-entropy loss; accumulates gradients into g (no division).
float nn_accumulate_grads(const NNParams *p, const ForwardCache *cache,
                          const bool *legal, int target, NNGrads *g);

// SGD step: p -= lr * g / batchSize, then zero g.
void  nn_apply_grads(NNParams *p, NNGrads *g, float lr, int batch_size);

// ----- Batched training path: B samples each of length L (same L), packed
// into the BatchedForwardCache. Used by main_train; eval/inspect still use
// the per-sample path because they handle variable-length game traces.

void nn_forward_batch(const NNParams *p,
                      const int *tokens,    // [B*L]
                      int B, int L,
                      BatchedForwardCache *cache);

// Returns the SUM of cross-entropy losses across the batch and the number
// of correct top-1 predictions. Accumulates gradients into g (no division).
float nn_accumulate_grads_batch(const NNParams *p,
                                const BatchedForwardCache *cache,
                                const bool *legal,    // [B][NUM_ACTIONS]
                                const int *targets,   // [B]
                                NNGrads *g,
                                int *out_correct);

// Binary serialization (4-byte float32 LE; fixed shape).
bool nn_save(const char *path, const NNParams *p);
bool nn_load(const char *path, NNParams *p);

#endif

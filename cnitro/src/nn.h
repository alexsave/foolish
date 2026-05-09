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

#define D_MODEL   32
#define FF_DIM    64
#define N_LAYERS  2

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
    float scores[MAX_SEQ_LEN * MAX_SEQ_LEN];
    float attn[MAX_SEQ_LEN * MAX_SEQ_LEN];
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

// Binary serialization (4-byte float32 LE; fixed shape).
bool nn_save(const char *path, const NNParams *p);
bool nn_load(const char *path, NNParams *p);

#endif

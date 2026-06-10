// Direct port of nitro_nn.ts forward + backward + SGD. The cache layout is
// designed so backward only references buffers we already wrote during
// forward — no recomputation. NNGrads is intentionally the same shape as
// NNParams so we can reuse the SGD update loop.

#include "nn.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <Accelerate/Accelerate.h>

// ---------- RNG (matches the JS-side LCG used during init) -----------

typedef struct { uint32_t s; } LCG;
static LCG init_rng(uint32_t seed) { LCG r; r.s = seed ? seed : 1; return r; }
static float rng_signed(LCG *r) {
    r->s = r->s * 1664525u + 1013904223u;
    return ((float)r->s / 4294967296.0f) * 2.0f - 1.0f;
}

static void xavier_init(LCG *r, float *a, int rows, int cols) {
    int n = rows * cols;
    float std = sqrtf(1.0f / (float)cols);
    for (int i = 0; i < n; i++) a[i] = rng_signed(r) * std;
}
static void he_init(LCG *r, float *a, int rows, int cols) {
    int n = rows * cols;
    float std = sqrtf(2.0f / (float)cols);
    for (int i = 0; i < n; i++) a[i] = rng_signed(r) * std;
}

void nn_init_random(NNParams *p, uint32_t seed) {
    memset(p, 0, sizeof(*p));
    LCG r = init_rng(seed);
    for (int li = 0; li < N_LAYERS; li++) {
        LayerParams *lp = &p->layers[li];
        for (int i = 0; i < D_MODEL; i++) { lp->ln1g[i] = 1.0f; lp->ln2g[i] = 1.0f; }
        xavier_init(&r, lp->Wq, D_MODEL, D_MODEL);
        xavier_init(&r, lp->Wk, D_MODEL, D_MODEL);
        xavier_init(&r, lp->Wv, D_MODEL, D_MODEL);
        xavier_init(&r, lp->Wo, D_MODEL, D_MODEL);
        he_init(&r, lp->Wff1, FF_DIM, D_MODEL);
        he_init(&r, lp->Wff2, D_MODEL, FF_DIM);
        // bff1, bff2, ln1b, ln2b stay zero
    }
    for (int i = 0; i < D_MODEL; i++) p->lnFg[i] = 1.0f;
    xavier_init(&r, p->embed, VOCAB_SIZE, D_MODEL);
    xavier_init(&r, p->pos_embed, MAX_SEQ_LEN, D_MODEL);
    xavier_init(&r, p->Wout, NUM_ACTIONS, D_MODEL);
    // bout stays zero
}

void nn_zero_grads(NNGrads *g) { memset(g, 0, sizeof(*g)); }

// ---------- LayerNorm forward / backward -----------------------------

static void layer_norm(const float *x, int off, const float *g, const float *b,
                       int dim, float *out, float *mean_out, float *var_out) {
    float mean = 0.f;
    for (int i = 0; i < dim; i++) mean += x[off + i];
    mean /= dim;
    float var = 0.f;
    for (int i = 0; i < dim; i++) { float d = x[off + i] - mean; var += d * d; }
    var /= dim;
    float inv = 1.f / sqrtf(var + 1e-5f);
    for (int i = 0; i < dim; i++) out[i] = (x[off + i] - mean) * inv * g[i] + b[i];
    *mean_out = mean; *var_out = var;
}

// dxOut[dim] -> dx[dim], also accumulates into dg/db.
static void layer_norm_backward(const float *xIn, int off, float mean, float var,
                                const float *g, const float *dxOut,
                                float *dg, float *db,
                                int dim, float *dx_out) {
    float inv = 1.f / sqrtf(var + 1e-5f);
    float xhat[D_MODEL];
    for (int i = 0; i < dim; i++) xhat[i] = (xIn[off + i] - mean) * inv;
    for (int i = 0; i < dim; i++) { dg[i] += dxOut[i] * xhat[i]; db[i] += dxOut[i]; }
    float dxhat[D_MODEL];
    for (int i = 0; i < dim; i++) dxhat[i] = dxOut[i] * g[i];
    float sum_dxhat = 0.f, sum_dxhat_xhat = 0.f;
    for (int i = 0; i < dim; i++) { sum_dxhat += dxhat[i]; sum_dxhat_xhat += dxhat[i] * xhat[i]; }
    float invd = 1.f / (float)dim;
    for (int i = 0; i < dim; i++) {
        dx_out[i] = invd * inv * (dim * dxhat[i] - sum_dxhat - xhat[i] * sum_dxhat_xhat);
    }
}

// ---------- Forward --------------------------------------------------

void nn_forward(const NNParams *p, const int *tokens, int L, ForwardCache *cache) {
    if (L > MAX_SEQ_LEN) L = MAX_SEQ_LEN;
    cache->L = L;
    for (int i = 0; i < L; i++) cache->tokens[i] = tokens[i];

    // Embed + positional.
    for (int i = 0; i < L; i++) {
        int tok = tokens[i];
        for (int d = 0; d < D_MODEL; d++) {
            cache->embedded[i * D_MODEL + d] =
                p->embed[tok * D_MODEL + d] + p->pos_embed[i * D_MODEL + d];
        }
    }

    const float *cur = cache->embedded;
    for (int li = 0; li < N_LAYERS; li++) {
        const LayerParams *lp = &p->layers[li];
        LayerCache *lc = &cache->layers[li];

        // Save xIn (input to layer) for backward through the residual.
        memcpy(lc->xIn, cur, L * D_MODEL * sizeof(float));

        // LN1 per token.
        for (int i = 0; i < L; i++) {
            float out_buf[D_MODEL];
            float m, v;
            layer_norm(cur, i * D_MODEL, lp->ln1g, lp->ln1b, D_MODEL, out_buf, &m, &v);
            lc->ln1Mean[i] = m; lc->ln1Var[i] = v;
            for (int d = 0; d < D_MODEL; d++) lc->xLn1[i * D_MODEL + d] = out_buf[d];
        }

        // Q,K,V = xLn1 @ W^T  (W stored as [out_dim][in_dim], so transpose).
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    lc->xLn1, D_MODEL, lp->Wq, D_MODEL, 0.0f, lc->Q, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    lc->xLn1, D_MODEL, lp->Wk, D_MODEL, 0.0f, lc->K, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    lc->xLn1, D_MODEL, lp->Wv, D_MODEL, 0.0f, lc->V, D_MODEL);

        // Per-head: scores_h = Q_h @ K_h^T * scale, where Q_h is L×D_HEAD slice
        // (offset h*D_HEAD, row stride D_MODEL). Softmax row-wise per head.
        float scale = 1.f / sqrtf((float)D_HEAD);
        for (int h = 0; h < N_HEADS; h++) {
            float *scores_h = lc->scores + (size_t)h * L * L;
            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                        L, L, D_HEAD, scale,
                        lc->Q + h * D_HEAD, D_MODEL,
                        lc->K + h * D_HEAD, D_MODEL,
                        0.0f, scores_h, L);
        }
        // Softmax row-wise per head. To amortize vvexpf's per-call overhead,
        // we build the entire shifted matrix in attn_h, then do ONE vvexpf
        // call over L*L elements per head, then sum/scale row-wise.
        for (int h = 0; h < N_HEADS; h++) {
            float *scores_h = lc->scores + (size_t)h * L * L;
            float *attn_h   = lc->attn   + (size_t)h * L * L;
            // Per-row: shift = scores_row - row_max → into attn_h.
            for (int i = 0; i < L; i++) {
                const float *row_in  = scores_h + i * L;
                float *row_out = attn_h + i * L;
                float mx = row_in[0];
                for (int j = 1; j < L; j++) if (row_in[j] > mx) mx = row_in[j];
                for (int j = 0; j < L; j++) row_out[j] = row_in[j] - mx;
            }
            // One big vvexpf on the entire L*L block.
            int n_total = L * L;
            vvexpf(attn_h, attn_h, &n_total);
            // Per-row sum + scale.
            for (int i = 0; i < L; i++) {
                float *row = attn_h + i * L;
                float sum = 0;
                for (int j = 0; j < L; j++) sum += row[j];
                float inv = 1.f / sum;
                for (int j = 0; j < L; j++) row[j] *= inv;
            }
        }
        // attnOut_h = attn_h @ V_h. Each head writes into its own slice.
        for (int h = 0; h < N_HEADS; h++) {
            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                        L, D_HEAD, L, 1.0f,
                        lc->attn + (size_t)h * L * L, L,
                        lc->V + h * D_HEAD, D_MODEL,
                        0.0f, lc->attnOut + h * D_HEAD, D_MODEL);
        }
        // proj = attnOut @ Wo^T.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    lc->attnOut, D_MODEL, lp->Wo, D_MODEL, 0.0f, lc->proj, D_MODEL);
        // afterAttn = xIn + proj (residual).
        vDSP_vadd(lc->xIn, 1, lc->proj, 1, lc->afterAttn, 1, L * D_MODEL);

        // LN2.
        for (int i = 0; i < L; i++) {
            float out_buf[D_MODEL]; float m, v;
            layer_norm(lc->afterAttn, i * D_MODEL, lp->ln2g, lp->ln2b, D_MODEL, out_buf, &m, &v);
            lc->ln2Mean[i] = m; lc->ln2Var[i] = v;
            for (int d = 0; d < D_MODEL; d++) lc->xLn2[i * D_MODEL + d] = out_buf[d];
        }

        // FFN1: ff1pre = xLn2 @ Wff1^T + bff1; ff1 = ReLU(ff1pre).
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    L, FF_DIM, D_MODEL, 1.0f,
                    lc->xLn2, D_MODEL, lp->Wff1, D_MODEL, 0.0f, lc->ff1pre, FF_DIM);
        for (int i = 0; i < L; i++) {
            // Add bias as a contiguous vector op.
            vDSP_vadd(lc->ff1pre + i * FF_DIM, 1, lp->bff1, 1,
                      lc->ff1pre + i * FF_DIM, 1, FF_DIM);
        }
        // Branchless ReLU on the whole [L, FF_DIM] block.
        for (int i = 0, n = L * FF_DIM; i < n; i++) {
            float s = lc->ff1pre[i];
            lc->ff1[i] = s > 0.f ? s : 0.f;
        }
        // FFN2: ff2 = ff1 @ Wff2^T + bff2.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    L, D_MODEL, FF_DIM, 1.0f,
                    lc->ff1, FF_DIM, lp->Wff2, FF_DIM, 0.0f, lc->ff2, D_MODEL);
        for (int i = 0; i < L; i++) {
            vDSP_vadd(lc->ff2 + i * D_MODEL, 1, lp->bff2, 1,
                      lc->ff2 + i * D_MODEL, 1, D_MODEL);
        }
        // out = afterAttn + ff2 (residual).
        vDSP_vadd(lc->afterAttn, 1, lc->ff2, 1, lc->out, 1, L * D_MODEL);

        cur = lc->out;
    }

    // Final LN on CLS only.
    for (int d = 0; d < D_MODEL; d++) cache->finalLnIn[d] = cur[d];
    {
        float out_buf[D_MODEL]; float m, v;
        layer_norm(cache->finalLnIn, 0, p->lnFg, p->lnFb, D_MODEL, out_buf, &m, &v);
        cache->finalLnMean = m; cache->finalLnVar = v;
        for (int d = 0; d < D_MODEL; d++) cache->cls[d] = out_buf[d];
    }
    for (int a = 0; a < NUM_ACTIONS; a++) {
        float s = p->bout[a];
        for (int d = 0; d < D_MODEL; d++) s += p->Wout[a * D_MODEL + d] * cache->cls[d];
        cache->logits[a] = s;
    }
}

// ---------- Masked softmax ------------------------------------------

void nn_softmax_masked(const float *logits, const bool *legal, float *out) {
    // Find max over legal entries.
    float mx = -1e30f;
    for (int i = 0; i < NUM_ACTIONS; i++) if (legal[i] && logits[i] > mx) mx = logits[i];
    // Stage shifted logits with -inf-ish for masked-out entries so exp goes to 0.
    float shifted[NUM_ACTIONS];
    for (int i = 0; i < NUM_ACTIONS; i++) {
        shifted[i] = legal[i] ? (logits[i] - mx) : -80.0f;  // expf(-80) ≈ 0
    }
    int n = NUM_ACTIONS;
    vvexpf(out, shifted, &n);
    // Force masked-out entries to exact 0 (vvexpf(-80) is just very small).
    for (int i = 0; i < NUM_ACTIONS; i++) if (!legal[i]) out[i] = 0;
    float sum = 0;
    vDSP_sve(out, 1, &sum, NUM_ACTIONS);
    if (sum > 0) {
        float inv = 1.f / sum;
        vDSP_vsmul(out, 1, &inv, out, 1, NUM_ACTIONS);
    }
}

// ---------- Backward --------------------------------------------------
//
// Each named bwd_* stage is __attribute__((noinline)) so the macOS sampler
// attributes its time to a distinct frame instead of one giant
// nn_accumulate_grads bucket.

#define NOINLINE __attribute__((noinline))

// Stage 1: cross-entropy → dCls. Tiny (NUM_ACTIONS × D_MODEL).
static NOINLINE void bwd_output_head(
    const NNParams *p, const ForwardCache *cache, NNGrads *g,
    const float *dlogits, float *dCls)
{
    for (int d = 0; d < D_MODEL; d++) dCls[d] = 0.f;
    for (int a = 0; a < NUM_ACTIONS; a++) {
        float dl = dlogits[a];
        if (dl == 0.f) continue;
        int off = a * D_MODEL;
        for (int d = 0; d < D_MODEL; d++) {
            g->Wout[off + d] += dl * cache->cls[d];
            dCls[d] += p->Wout[off + d] * dl;
        }
        g->bout[a] += dl;
    }
}

// Stage 2: residual split — out = afterAttn + ff2. Pure copy.
static NOINLINE void bwd_split_ffn_residual(
    int L, const float *dOutNext, float *dAfterAttn, float *dFf2_buf)
{
    memcpy(dAfterAttn, dOutNext, sizeof(float) * L * D_MODEL);
    memcpy(dFf2_buf,   dOutNext, sizeof(float) * L * D_MODEL);
}

// Stage 3: bias gradient — sum the rows of a (L × D) matrix into (D,).
// Uses cblas_sgemv(no-trans, all-ones) so AMX handles the reduction.
static NOINLINE void bwd_bias_grad(int L, int D, const float *dY_LxD, float *bias_grad) {
    static _Thread_local float ones[MAX_SEQ_LEN];
    static _Thread_local int ones_init = 0;
    if (!ones_init) { for (int i = 0; i < MAX_SEQ_LEN; i++) ones[i] = 1.f; ones_init = 1; }
    cblas_sgemv(CblasRowMajor, CblasTrans, L, D, 1.0f,
                dY_LxD, D, ones, 1, 1.0f, bias_grad, 1);
}

// Stage 4: ReLU back — dFf1pre[i] = (ff1pre[i] > 0) * dFf1_buf[i].
// Branchless + restrict so the compiler can fuse the load/compare/select/store
// into pure SIMD with no scalar fallback path.
static NOINLINE void bwd_relu(int n,
                              const float * __restrict ff1pre,
                              const float * __restrict dFf1_buf,
                              float * __restrict dFf1pre) {
    for (int i = 0; i < n; i++) {
        float mask = ff1pre[i] > 0.f ? 1.f : 0.f;
        dFf1pre[i] = dFf1_buf[i] * mask;
    }
}

// Stage 5: per-token LN backward (L iterations). Adds the gradient to dXOut.
static NOINLINE void bwd_layer_norm_per_token(
    int L, const float *xIn, const float *means, const float *vars,
    const float *g_param, const float *dXIn_per_token, float *dG, float *dB,
    float *dXOut /* += */)
{
    for (int i = 0; i < L; i++) {
        float dy[D_MODEL];
        for (int d = 0; d < D_MODEL; d++) dy[d] = dXIn_per_token[i * D_MODEL + d];
        float dx[D_MODEL];
        layer_norm_backward(xIn, i * D_MODEL, means[i], vars[i],
                            g_param, dy, dG, dB, D_MODEL, dx);
        for (int d = 0; d < D_MODEL; d++) dXOut[i * D_MODEL + d] += dx[d];
    }
}

// Stage 6: residual split — afterAttn = xIn + proj. Initializes dXIn_b
// directly (no memset+= dance).
static NOINLINE void bwd_split_attn_residual(
    int L, const float *dAfterAttn, float *dProj_b, float *dXIn_b)
{
    memcpy(dProj_b, dAfterAttn, sizeof(float) * L * D_MODEL);
    memcpy(dXIn_b,  dAfterAttn, sizeof(float) * L * D_MODEL);
}

// Stage 7: per-head softmax backward.
//   dScores[i][k] = attn[i][k] * (dAttn[i][k] - dot_i)
// __restrict + manual loops let the auto-vectorizer fuse the multiply-subtract
// into a single SIMD pass. vDSP per-row calls were tried and lost — the
// per-call overhead dominates at L=100.
static NOINLINE void bwd_softmax_attention(
    int L, const float * __restrict attn_h,
    const float * __restrict dAttn_h, float * __restrict dScores_h)
{
    for (int i = 0; i < L; i++) {
        const float *attn_row  = attn_h  + i * L;
        const float *dAttn_row = dAttn_h + i * L;
        float *dScores_row     = dScores_h + i * L;
        float dot = 0;
        for (int j = 0; j < L; j++) dot += attn_row[j] * dAttn_row[j];
        for (int k = 0; k < L; k++) dScores_row[k] = attn_row[k] * (dAttn_row[k] - dot);
    }
}

// Stage 8: embedding gradient. Per token, += dy into embed[tok] and pos[i].
static NOINLINE void bwd_embed_grad(
    int L, const int *tokens, const float *dOutNext,
    float *embed_grad, float *pos_grad)
{
    for (int i = 0; i < L; i++) {
        int tok = tokens[i];
        float *e = embed_grad + tok * D_MODEL;
        float *po = pos_grad + i * D_MODEL;
        const float *dy = dOutNext + i * D_MODEL;
        // vDSP_vadd avoids an explicit loop and lets Accelerate vectorize.
        vDSP_vadd(e, 1, dy, 1, e, 1, D_MODEL);
        vDSP_vadd(po, 1, dy, 1, po, 1, D_MODEL);
    }
}

float nn_accumulate_grads(const NNParams *p, const ForwardCache *cache,
                          const bool *legal, int target, NNGrads *g) {
    int L = cache->L;
    float probs[NUM_ACTIONS];
    nn_softmax_masked(cache->logits, legal, probs);

    float dlogits[NUM_ACTIONS];
    for (int i = 0; i < NUM_ACTIONS; i++) dlogits[i] = probs[i];
    dlogits[target] -= 1.0f;

    float dCls[D_MODEL];
    bwd_output_head(p, cache, g, dlogits, dCls);

    float dFinalLnIn[D_MODEL];
    layer_norm_backward(cache->finalLnIn, 0, cache->finalLnMean, cache->finalLnVar,
                        p->lnFg, dCls, g->lnFg, g->lnFb, D_MODEL, dFinalLnIn);

    static _Thread_local float dOutNext[MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dAfterAttn[MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dFf2_buf[MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dFf1_buf[MAX_SEQ_LEN * FF_DIM];
    static _Thread_local float dFf1pre [MAX_SEQ_LEN * FF_DIM];
    static _Thread_local float dXLn2   [MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dProj_b [MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dXIn_b  [MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dAttnOut[MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dAttn   [N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN];
    static _Thread_local float dV_b    [MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dScores [N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN];
    static _Thread_local float dQ_b    [MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dK_b    [MAX_SEQ_LEN * D_MODEL];
    static _Thread_local float dXLn1   [MAX_SEQ_LEN * D_MODEL];

    memset(dOutNext, 0, sizeof(float) * L * D_MODEL);
    for (int d = 0; d < D_MODEL; d++) dOutNext[d] = dFinalLnIn[d];

    for (int li = N_LAYERS - 1; li >= 0; li--) {
        const LayerParams *lp = &p->layers[li];
        const LayerCache  *lc = &cache->layers[li];
        LayerParams       *lg = &g->layers[li];

        bwd_split_ffn_residual(L, dOutNext, dAfterAttn, dFf2_buf);

        // dWff2 += dFf2_buf^T @ ff1.
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, FF_DIM, L, 1.0f,
                    dFf2_buf, D_MODEL, lc->ff1, FF_DIM, 1.0f, lg->Wff2, FF_DIM);
        // dFf1_buf = dFf2_buf @ Wff2.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    L, FF_DIM, D_MODEL, 1.0f,
                    dFf2_buf, D_MODEL, lp->Wff2, FF_DIM, 0.0f, dFf1_buf, FF_DIM);
        bwd_bias_grad(L, D_MODEL, dFf2_buf, lg->bff2);
        bwd_relu(L * FF_DIM, lc->ff1pre, dFf1_buf, dFf1pre);
        // dWff1 += dFf1pre^T @ xLn2.
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    FF_DIM, D_MODEL, L, 1.0f,
                    dFf1pre, FF_DIM, lc->xLn2, D_MODEL, 1.0f, lg->Wff1, D_MODEL);
        // dXLn2 = dFf1pre @ Wff1.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    L, D_MODEL, FF_DIM, 1.0f,
                    dFf1pre, FF_DIM, lp->Wff1, D_MODEL, 0.0f, dXLn2, D_MODEL);
        bwd_bias_grad(L, FF_DIM, dFf1pre, lg->bff1);
        bwd_layer_norm_per_token(L, lc->afterAttn, lc->ln2Mean, lc->ln2Var,
                                 lp->ln2g, dXLn2, lg->ln2g, lg->ln2b, dAfterAttn);

        bwd_split_attn_residual(L, dAfterAttn, dProj_b, dXIn_b);

        // dWo += dProj_b^T @ attnOut.
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, L, 1.0f,
                    dProj_b, D_MODEL, lc->attnOut, D_MODEL, 1.0f, lg->Wo, D_MODEL);
        // dAttnOut = dProj_b @ Wo.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    dProj_b, D_MODEL, lp->Wo, D_MODEL, 0.0f, dAttnOut, D_MODEL);

        // Per-head: dAttn, dV; softmax-back; dQ, dK. Each head writes its own
        // non-overlapping slice with beta=0.0 — no zeroing needed.
        float scale = 1.f / sqrtf((float)D_HEAD);
        for (int h = 0; h < N_HEADS; h++) {
            const float *attn_h = lc->attn + (size_t)h * L * L;
            float *dAttn_h = dAttn + (size_t)h * L * L;
            float *dScores_h = dScores + (size_t)h * L * L;

            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                        L, L, D_HEAD, 1.0f,
                        dAttnOut + h * D_HEAD, D_MODEL,
                        lc->V + h * D_HEAD, D_MODEL,
                        0.0f, dAttn_h, L);
            cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                        L, D_HEAD, L, 1.0f,
                        attn_h, L,
                        dAttnOut + h * D_HEAD, D_MODEL,
                        0.0f, dV_b + h * D_HEAD, D_MODEL);
            bwd_softmax_attention(L, attn_h, dAttn_h, dScores_h);
            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                        L, D_HEAD, L, scale,
                        dScores_h, L,
                        lc->K + h * D_HEAD, D_MODEL,
                        0.0f, dQ_b + h * D_HEAD, D_MODEL);
            cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                        L, D_HEAD, L, scale,
                        dScores_h, L,
                        lc->Q + h * D_HEAD, D_MODEL,
                        0.0f, dK_b + h * D_HEAD, D_MODEL);
        }

        // dWq/dWk/dWv accumulation.
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, L, 1.0f,
                    dQ_b, D_MODEL, lc->xLn1, D_MODEL, 1.0f, lg->Wq, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, L, 1.0f,
                    dK_b, D_MODEL, lc->xLn1, D_MODEL, 1.0f, lg->Wk, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, L, 1.0f,
                    dV_b, D_MODEL, lc->xLn1, D_MODEL, 1.0f, lg->Wv, D_MODEL);
        // dXLn1 = dQ@Wq + dK@Wk + dV@Wv.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    dQ_b, D_MODEL, lp->Wq, D_MODEL, 0.0f, dXLn1, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    dK_b, D_MODEL, lp->Wk, D_MODEL, 1.0f, dXLn1, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    L, D_MODEL, D_MODEL, 1.0f,
                    dV_b, D_MODEL, lp->Wv, D_MODEL, 1.0f, dXLn1, D_MODEL);

        bwd_layer_norm_per_token(L, lc->xIn, lc->ln1Mean, lc->ln1Var,
                                 lp->ln1g, dXLn1, lg->ln1g, lg->ln1b, dXIn_b);

        memcpy(dOutNext, dXIn_b, sizeof(float) * L * D_MODEL);
    }

    bwd_embed_grad(L, cache->tokens, dOutNext, g->embed, g->pos_embed);

    float pt = probs[target];
    if (pt < 1e-9f) pt = 1e-9f;
    return -logf(pt);
}

// ---------- SGD step --------------------------------------------------

void nn_apply_grads(NNParams *p, NNGrads *g, float lr, int batch_size) {
    float inv = 1.f / (float)batch_size;
    float *fp = (float *)p; float *fg = (float *)g;
    size_t n_floats = sizeof(NNParams) / sizeof(float);
    for (size_t i = 0; i < n_floats; i++) {
        fp[i] -= lr * fg[i] * inv;
        fg[i] = 0.0f;
    }
}

// ---------- Save / load (binary float32 LE) ---------------------------

static const uint32_t MAGIC   = 0x4E4E4E4E;  // "NNNN"
static const uint32_t VERSION = 1;

bool nn_save(const char *path, const NNParams *p) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t hdr[8] = { MAGIC, VERSION, VOCAB_SIZE, D_MODEL, FF_DIM, N_LAYERS, MAX_SEQ_LEN, NUM_ACTIONS };
    fwrite(hdr, sizeof(uint32_t), 8, f);
    fwrite(p, sizeof(NNParams), 1, f);
    fclose(f);
    return true;
}

bool nn_load(const char *path, NNParams *p) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t hdr[8];
    if (fread(hdr, sizeof(uint32_t), 8, f) != 8) { fclose(f); return false; }
    if (hdr[0] != MAGIC || hdr[1] != VERSION
        || hdr[2] != VOCAB_SIZE || hdr[3] != D_MODEL
        || hdr[4] != FF_DIM || hdr[5] != N_LAYERS
        || hdr[6] != MAX_SEQ_LEN || hdr[7] != NUM_ACTIONS) { fclose(f); return false; }
    if (fread(p, sizeof(NNParams), 1, f) != 1) { fclose(f); return false; }
    fclose(f);
    return true;
}

// ===================================================================
//                  Batched forward / backward
// -------------------------------------------------------------------
// Layout: tokens[B*L], embedded[B*L*D], LayerCache buffers [B*L*D].
// All token-wise ops collapse the (B, L) dims into a single (B*L) leading
// dim for one big sgemm. Attention is per-sample so we loop over B.
// ===================================================================

void nn_forward_batch(const NNParams *p,
                      const int *tokens, int B, int L,
                      BatchedForwardCache *cache)
{
    int BL = B * L;
    cache->B = B;
    cache->L = L;
    memcpy(cache->tokens, tokens, sizeof(int) * BL);

    // Embed + positional. For each (b, i): embedded[b,i,d] = embed[tok][d] + pos[i][d].
    // Batched: copy embed rows to embedded, then add positional per i.
    for (int b = 0; b < B; b++) {
        for (int i = 0; i < L; i++) {
            int tok = tokens[b * L + i];
            const float *e = p->embed + tok * D_MODEL;
            const float *po = p->pos_embed + i * D_MODEL;
            float *out = cache->embedded + (b * L + i) * D_MODEL;
            vDSP_vadd(e, 1, po, 1, out, 1, D_MODEL);
        }
    }

    const float *cur = cache->embedded;
    for (int li = 0; li < N_LAYERS; li++) {
        const LayerParams *lp = &p->layers[li];
        BatchedLayerCache *lc = &cache->layers[li];

        memcpy(lc->xIn, cur, sizeof(float) * BL * D_MODEL);

        // LN1 per token over BL.
        for (int t = 0; t < BL; t++) {
            float out_buf[D_MODEL]; float m, v;
            layer_norm(cur, t * D_MODEL, lp->ln1g, lp->ln1b, D_MODEL, out_buf, &m, &v);
            lc->ln1Mean[t] = m; lc->ln1Var[t] = v;
            for (int d = 0; d < D_MODEL; d++) lc->xLn1[t * D_MODEL + d] = out_buf[d];
        }

        // QKV: [BL, D] @ [D, D]^T — single big sgemm each.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    lc->xLn1, D_MODEL, lp->Wq, D_MODEL, 0.0f, lc->Q, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    lc->xLn1, D_MODEL, lp->Wk, D_MODEL, 0.0f, lc->K, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    lc->xLn1, D_MODEL, lp->Wv, D_MODEL, 0.0f, lc->V, D_MODEL);

        // Attention is per-sample (each sequence is independent). For each b
        // and each head h, scores_{b,h} = Q_{b,h} @ K_{b,h}^T * scale.
        float scale = 1.f / sqrtf((float)D_HEAD);
        for (int b = 0; b < B; b++) {
            for (int h = 0; h < N_HEADS; h++) {
                float *scores_bh = lc->scores + ((size_t)b * N_HEADS + h) * L * L;
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                            L, L, D_HEAD, scale,
                            lc->Q + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            lc->K + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            0.0f, scores_bh, L);
            }
            // Softmax + attn @ V per head.
            for (int h = 0; h < N_HEADS; h++) {
                float *scores_bh = lc->scores + ((size_t)b * N_HEADS + h) * L * L;
                float *attn_bh   = lc->attn   + ((size_t)b * N_HEADS + h) * L * L;
                for (int i = 0; i < L; i++) {
                    float *row_in  = scores_bh + i * L;
                    float *row_out = attn_bh + i * L;
                    float mx = row_in[0];
                    for (int j = 1; j < L; j++) if (row_in[j] > mx) mx = row_in[j];
                    for (int j = 0; j < L; j++) row_out[j] = row_in[j] - mx;
                }
                int n_total = L * L;
                vvexpf(attn_bh, attn_bh, &n_total);
                for (int i = 0; i < L; i++) {
                    float *row = attn_bh + i * L;
                    float sum = 0;
                    for (int j = 0; j < L; j++) sum += row[j];
                    float inv = 1.f / sum;
                    for (int j = 0; j < L; j++) row[j] *= inv;
                }
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                            L, D_HEAD, L, 1.0f,
                            attn_bh, L,
                            lc->V + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            0.0f, lc->attnOut + b * L * D_MODEL + h * D_HEAD, D_MODEL);
            }
        }

        // Wo: batched [BL, D] @ [D, D]^T.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    lc->attnOut, D_MODEL, lp->Wo, D_MODEL, 0.0f, lc->proj, D_MODEL);
        // Residual.
        vDSP_vadd(lc->xIn, 1, lc->proj, 1, lc->afterAttn, 1, BL * D_MODEL);

        // LN2.
        for (int t = 0; t < BL; t++) {
            float out_buf[D_MODEL]; float m, v;
            layer_norm(lc->afterAttn, t * D_MODEL, lp->ln2g, lp->ln2b, D_MODEL, out_buf, &m, &v);
            lc->ln2Mean[t] = m; lc->ln2Var[t] = v;
            for (int d = 0; d < D_MODEL; d++) lc->xLn2[t * D_MODEL + d] = out_buf[d];
        }

        // FFN1.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    BL, FF_DIM, D_MODEL, 1.0f,
                    lc->xLn2, D_MODEL, lp->Wff1, D_MODEL, 0.0f, lc->ff1pre, FF_DIM);
        for (int t = 0; t < BL; t++) {
            vDSP_vadd(lc->ff1pre + t * FF_DIM, 1, lp->bff1, 1,
                      lc->ff1pre + t * FF_DIM, 1, FF_DIM);
        }
        for (int i = 0, n = BL * FF_DIM; i < n; i++) {
            float s = lc->ff1pre[i];
            lc->ff1[i] = s > 0.f ? s : 0.f;
        }
        // FFN2.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    BL, D_MODEL, FF_DIM, 1.0f,
                    lc->ff1, FF_DIM, lp->Wff2, FF_DIM, 0.0f, lc->ff2, D_MODEL);
        for (int t = 0; t < BL; t++) {
            vDSP_vadd(lc->ff2 + t * D_MODEL, 1, lp->bff2, 1,
                      lc->ff2 + t * D_MODEL, 1, D_MODEL);
        }
        // Residual.
        vDSP_vadd(lc->afterAttn, 1, lc->ff2, 1, lc->out, 1, BL * D_MODEL);

        cur = lc->out;
    }

    // Final LN on CLS (position 0) of each sample, then Wout.
    for (int b = 0; b < B; b++) {
        const float *cls_in = cur + b * L * D_MODEL;
        for (int d = 0; d < D_MODEL; d++) cache->finalLnIn[b * D_MODEL + d] = cls_in[d];
        float out_buf[D_MODEL]; float m, v;
        layer_norm(cache->finalLnIn, b * D_MODEL, p->lnFg, p->lnFb, D_MODEL, out_buf, &m, &v);
        cache->finalLnMean[b] = m; cache->finalLnVar[b] = v;
        for (int d = 0; d < D_MODEL; d++) cache->cls[b * D_MODEL + d] = out_buf[d];
    }
    // Logits = cls @ Wout^T + bout. Batched [B, D] @ [D, NUM_ACTIONS]^T.
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                B, NUM_ACTIONS, D_MODEL, 1.0f,
                cache->cls, D_MODEL, p->Wout, D_MODEL, 0.0f, cache->logits, NUM_ACTIONS);
    for (int b = 0; b < B; b++) {
        vDSP_vadd(cache->logits + b * NUM_ACTIONS, 1, p->bout, 1,
                  cache->logits + b * NUM_ACTIONS, 1, NUM_ACTIONS);
    }
}

float nn_accumulate_grads_batch(const NNParams *p,
                                const BatchedForwardCache *cache,
                                const bool *legal,
                                const int *targets,
                                NNGrads *g,
                                int *out_correct)
{
    int B = cache->B;
    int L = cache->L;
    int BL = B * L;

    // Heap-allocate the big batched scratch buffers lazily per thread. With
    // BATCH_MAX growing past 32, putting these in TLS as static arrays
    // exceeds macOS thread-local storage limits and the worker silently
    // SIGILLs on first use. Lazy malloc keeps the pointer in TLS but the
    // data on the heap.
    #define TLS_BUF_F(name, n) \
        static _Thread_local float *name = NULL; \
        if (!name) name = malloc((size_t)(n) * sizeof(float))

    // Compute per-sample probs, dlogits, top-1 accuracy, sum of losses.
    TLS_BUF_F(dlogits_all, BATCH_MAX * NUM_ACTIONS);
    float total_loss = 0.f;
    int correct = 0;
    for (int b = 0; b < B; b++) {
        float probs[NUM_ACTIONS];
        nn_softmax_masked(cache->logits + b * NUM_ACTIONS, legal + b * NUM_ACTIONS, probs);
        int best = 0; float bp = -1e30f;
        for (int j = 0; j < NUM_ACTIONS; j++) if (probs[j] > bp) { bp = probs[j]; best = j; }
        if (best == targets[b]) correct++;
        for (int i = 0; i < NUM_ACTIONS; i++) dlogits_all[b * NUM_ACTIONS + i] = probs[i];
        dlogits_all[b * NUM_ACTIONS + targets[b]] -= 1.0f;
        float pt = probs[targets[b]];
        if (pt < 1e-9f) pt = 1e-9f;
        total_loss += -logf(pt);
    }
    if (out_correct) *out_correct = correct;

    // bout: sum dlogits over B.
    for (int b = 0; b < B; b++) {
        vDSP_vadd(g->bout, 1, dlogits_all + b * NUM_ACTIONS, 1, g->bout, 1, NUM_ACTIONS);
    }
    // Wout += dlogits^T @ cls.  ([NUM_ACTIONS, B] @ [B, D] = [NUM_ACTIONS, D])
    cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                NUM_ACTIONS, D_MODEL, B, 1.0f,
                dlogits_all, NUM_ACTIONS, cache->cls, D_MODEL, 1.0f, g->Wout, D_MODEL);
    // dCls = dlogits @ Wout.  ([B, NUM_ACTIONS] @ [NUM_ACTIONS, D])
    TLS_BUF_F(dCls, BATCH_MAX * D_MODEL);
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                B, D_MODEL, NUM_ACTIONS, 1.0f,
                dlogits_all, NUM_ACTIONS, p->Wout, D_MODEL, 0.0f, dCls, D_MODEL);

    // Final LN backward per sample → dFinalLnIn (CLS-only, length B*D).
    TLS_BUF_F(dFinalLnIn, BATCH_MAX * D_MODEL);
    for (int b = 0; b < B; b++) {
        float dy[D_MODEL]; for (int d = 0; d < D_MODEL; d++) dy[d] = dCls[b * D_MODEL + d];
        float dx[D_MODEL];
        layer_norm_backward(cache->finalLnIn, b * D_MODEL,
                            cache->finalLnMean[b], cache->finalLnVar[b],
                            p->lnFg, dy, g->lnFg, g->lnFb, D_MODEL, dx);
        for (int d = 0; d < D_MODEL; d++) dFinalLnIn[b * D_MODEL + d] = dx[d];
    }

    // Set up dOutNext = [BL, D]. Only CLS positions get dFinalLnIn; rest 0.
    TLS_BUF_F(dOutNext,   BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dAfterAttn, BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dFf2_buf,   BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dFf1_buf,   BATCH_MAX * MAX_SEQ_LEN * FF_DIM);
    TLS_BUF_F(dFf1pre,    BATCH_MAX * MAX_SEQ_LEN * FF_DIM);
    TLS_BUF_F(dXLn2,      BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dProj_b,    BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dXIn_b,     BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dAttnOut,   BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dAttn,      BATCH_MAX * N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN);
    TLS_BUF_F(dV_b,       BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dScores,    BATCH_MAX * N_HEADS * MAX_SEQ_LEN * MAX_SEQ_LEN);
    TLS_BUF_F(dQ_b,       BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dK_b,       BATCH_MAX * MAX_SEQ_LEN * D_MODEL);
    TLS_BUF_F(dXLn1,      BATCH_MAX * MAX_SEQ_LEN * D_MODEL);

    memset(dOutNext, 0, sizeof(float) * BL * D_MODEL);
    for (int b = 0; b < B; b++) {
        for (int d = 0; d < D_MODEL; d++) {
            dOutNext[b * L * D_MODEL + d] = dFinalLnIn[b * D_MODEL + d];
        }
    }

    for (int li = N_LAYERS - 1; li >= 0; li--) {
        const LayerParams *lp = &p->layers[li];
        const BatchedLayerCache *lc = &cache->layers[li];
        LayerParams *lg = &g->layers[li];

        // Split residual.
        memcpy(dAfterAttn, dOutNext, sizeof(float) * BL * D_MODEL);
        memcpy(dFf2_buf,   dOutNext, sizeof(float) * BL * D_MODEL);

        // dWff2 += dFf2_buf^T @ ff1   ([D, BL] @ [BL, FF])
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, FF_DIM, BL, 1.0f,
                    dFf2_buf, D_MODEL, lc->ff1, FF_DIM, 1.0f, lg->Wff2, FF_DIM);
        // dFf1_buf = dFf2_buf @ Wff2
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    BL, FF_DIM, D_MODEL, 1.0f,
                    dFf2_buf, D_MODEL, lp->Wff2, FF_DIM, 0.0f, dFf1_buf, FF_DIM);
        // bff2 grad: sum over BL rows.
        bwd_bias_grad(BL, D_MODEL, dFf2_buf, lg->bff2);
        // ReLU back.
        bwd_relu(BL * FF_DIM, lc->ff1pre, dFf1_buf, dFf1pre);
        // dWff1 += dFf1pre^T @ xLn2
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    FF_DIM, D_MODEL, BL, 1.0f,
                    dFf1pre, FF_DIM, lc->xLn2, D_MODEL, 1.0f, lg->Wff1, D_MODEL);
        // dXLn2 = dFf1pre @ Wff1
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    BL, D_MODEL, FF_DIM, 1.0f,
                    dFf1pre, FF_DIM, lp->Wff1, D_MODEL, 0.0f, dXLn2, D_MODEL);
        // bff1 grad: sum over BL rows.
        bwd_bias_grad(BL, FF_DIM, dFf1pre, lg->bff1);

        // LN2 backward per token over BL.
        bwd_layer_norm_per_token(BL, lc->afterAttn, lc->ln2Mean, lc->ln2Var,
                                 lp->ln2g, dXLn2, lg->ln2g, lg->ln2b, dAfterAttn);

        // Split attn residual.
        memcpy(dProj_b, dAfterAttn, sizeof(float) * BL * D_MODEL);
        memcpy(dXIn_b,  dAfterAttn, sizeof(float) * BL * D_MODEL);

        // dWo += dProj_b^T @ attnOut
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, BL, 1.0f,
                    dProj_b, D_MODEL, lc->attnOut, D_MODEL, 1.0f, lg->Wo, D_MODEL);
        // dAttnOut = dProj_b @ Wo
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    dProj_b, D_MODEL, lp->Wo, D_MODEL, 0.0f, dAttnOut, D_MODEL);

        // Per-sample, per-head attention backward.
        float scale = 1.f / sqrtf((float)D_HEAD);
        for (int b = 0; b < B; b++) {
            for (int h = 0; h < N_HEADS; h++) {
                const float *attn_bh = lc->attn + ((size_t)b * N_HEADS + h) * L * L;
                float *dAttn_bh = dAttn + ((size_t)b * N_HEADS + h) * L * L;
                float *dScores_bh = dScores + ((size_t)b * N_HEADS + h) * L * L;
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                            L, L, D_HEAD, 1.0f,
                            dAttnOut + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            lc->V + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            0.0f, dAttn_bh, L);
                cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                            L, D_HEAD, L, 1.0f,
                            attn_bh, L,
                            dAttnOut + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            0.0f, dV_b + b * L * D_MODEL + h * D_HEAD, D_MODEL);
                bwd_softmax_attention(L, attn_bh, dAttn_bh, dScores_bh);
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                            L, D_HEAD, L, scale,
                            dScores_bh, L,
                            lc->K + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            0.0f, dQ_b + b * L * D_MODEL + h * D_HEAD, D_MODEL);
                cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                            L, D_HEAD, L, scale,
                            dScores_bh, L,
                            lc->Q + b * L * D_MODEL + h * D_HEAD, D_MODEL,
                            0.0f, dK_b + b * L * D_MODEL + h * D_HEAD, D_MODEL);
            }
        }

        // dW{q,k,v} += d{Q,K,V}^T @ xLn1 — single big sgemm each.
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, BL, 1.0f,
                    dQ_b, D_MODEL, lc->xLn1, D_MODEL, 1.0f, lg->Wq, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, BL, 1.0f,
                    dK_b, D_MODEL, lc->xLn1, D_MODEL, 1.0f, lg->Wk, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    D_MODEL, D_MODEL, BL, 1.0f,
                    dV_b, D_MODEL, lc->xLn1, D_MODEL, 1.0f, lg->Wv, D_MODEL);
        // dXLn1 = dQ@Wq + dK@Wk + dV@Wv.
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    dQ_b, D_MODEL, lp->Wq, D_MODEL, 0.0f, dXLn1, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    dK_b, D_MODEL, lp->Wk, D_MODEL, 1.0f, dXLn1, D_MODEL);
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    BL, D_MODEL, D_MODEL, 1.0f,
                    dV_b, D_MODEL, lp->Wv, D_MODEL, 1.0f, dXLn1, D_MODEL);

        // LN1 backward per token over BL.
        bwd_layer_norm_per_token(BL, lc->xIn, lc->ln1Mean, lc->ln1Var,
                                 lp->ln1g, dXLn1, lg->ln1g, lg->ln1b, dXIn_b);

        memcpy(dOutNext, dXIn_b, sizeof(float) * BL * D_MODEL);
    }

    // Embedding gradient: per (b, i), += dOutNext[b*L+i] into embed[tok] and
    // pos[i]. We can't trivially batch because of the lookup; just loop.
    for (int b = 0; b < B; b++) {
        for (int i = 0; i < L; i++) {
            int tok = cache->tokens[b * L + i];
            float *e = g->embed + tok * D_MODEL;
            float *po = g->pos_embed + i * D_MODEL;
            const float *dy = dOutNext + (b * L + i) * D_MODEL;
            vDSP_vadd(e, 1, dy, 1, e, 1, D_MODEL);
            vDSP_vadd(po, 1, dy, 1, po, 1, D_MODEL);
        }
    }

    return total_loss;
}

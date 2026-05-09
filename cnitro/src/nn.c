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

        // Q,K,V.
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float q = 0, k = 0, v = 0;
                for (int dd = 0; dd < D_MODEL; dd++) {
                    float x = lc->xLn1[i * D_MODEL + dd];
                    q += lp->Wq[d * D_MODEL + dd] * x;
                    k += lp->Wk[d * D_MODEL + dd] * x;
                    v += lp->Wv[d * D_MODEL + dd] * x;
                }
                lc->Q[i * D_MODEL + d] = q;
                lc->K[i * D_MODEL + d] = k;
                lc->V[i * D_MODEL + d] = v;
            }
        }

        // Scores + softmax.
        float scale = 1.f / sqrtf((float)D_MODEL);
        for (int i = 0; i < L; i++) {
            for (int j = 0; j < L; j++) {
                float s = 0;
                for (int d = 0; d < D_MODEL; d++) {
                    s += lc->Q[i * D_MODEL + d] * lc->K[j * D_MODEL + d];
                }
                lc->scores[i * L + j] = s * scale;
            }
            float mx = -1e30f;
            for (int j = 0; j < L; j++) if (lc->scores[i * L + j] > mx) mx = lc->scores[i * L + j];
            float sum = 0;
            for (int j = 0; j < L; j++) {
                float e = expf(lc->scores[i * L + j] - mx);
                lc->attn[i * L + j] = e;
                sum += e;
            }
            float inv = 1.f / sum;
            for (int j = 0; j < L; j++) lc->attn[i * L + j] *= inv;
        }

        // attnOut = attn @ V.
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float s = 0;
                for (int j = 0; j < L; j++) s += lc->attn[i * L + j] * lc->V[j * D_MODEL + d];
                lc->attnOut[i * D_MODEL + d] = s;
            }
        }
        // proj = attnOut @ Wo (Wo[d][dd]: proj[i][d] = sum_dd Wo[d][dd] * attnOut[i][dd]).
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float s = 0;
                for (int dd = 0; dd < D_MODEL; dd++) {
                    s += lp->Wo[d * D_MODEL + dd] * lc->attnOut[i * D_MODEL + dd];
                }
                lc->proj[i * D_MODEL + d] = s;
            }
        }
        // afterAttn = xIn + proj (residual).
        for (int i = 0; i < L * D_MODEL; i++) lc->afterAttn[i] = lc->xIn[i] + lc->proj[i];

        // LN2.
        for (int i = 0; i < L; i++) {
            float out_buf[D_MODEL]; float m, v;
            layer_norm(lc->afterAttn, i * D_MODEL, lp->ln2g, lp->ln2b, D_MODEL, out_buf, &m, &v);
            lc->ln2Mean[i] = m; lc->ln2Var[i] = v;
            for (int d = 0; d < D_MODEL; d++) lc->xLn2[i * D_MODEL + d] = out_buf[d];
        }

        // FFN.
        for (int i = 0; i < L; i++) {
            for (int h = 0; h < FF_DIM; h++) {
                float s = lp->bff1[h];
                for (int d = 0; d < D_MODEL; d++) s += lp->Wff1[h * D_MODEL + d] * lc->xLn2[i * D_MODEL + d];
                lc->ff1pre[i * FF_DIM + h] = s;
                lc->ff1[i * FF_DIM + h] = s > 0 ? s : 0;
            }
        }
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float s = lp->bff2[d];
                for (int h = 0; h < FF_DIM; h++) s += lp->Wff2[d * FF_DIM + h] * lc->ff1[i * FF_DIM + h];
                lc->ff2[i * D_MODEL + d] = s;
            }
        }
        for (int i = 0; i < L * D_MODEL; i++) lc->out[i] = lc->afterAttn[i] + lc->ff2[i];

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
    float mx = -1e30f;
    for (int i = 0; i < NUM_ACTIONS; i++) if (legal[i] && logits[i] > mx) mx = logits[i];
    float sum = 0;
    for (int i = 0; i < NUM_ACTIONS; i++) {
        if (!legal[i]) { out[i] = 0; continue; }
        float e = expf(logits[i] - mx);
        out[i] = e; sum += e;
    }
    if (sum > 0) for (int i = 0; i < NUM_ACTIONS; i++) out[i] /= sum;
}

// ---------- Backward --------------------------------------------------

float nn_accumulate_grads(const NNParams *p, const ForwardCache *cache,
                          const bool *legal, int target, NNGrads *g) {
    int L = cache->L;
    float probs[NUM_ACTIONS];
    nn_softmax_masked(cache->logits, legal, probs);

    float dlogits[NUM_ACTIONS];
    for (int i = 0; i < NUM_ACTIONS; i++) dlogits[i] = probs[i];
    dlogits[target] -= 1.0f;

    // Wout, bout, dCls.
    float dCls[D_MODEL] = {0};
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

    float dFinalLnIn[D_MODEL];
    layer_norm_backward(cache->finalLnIn, 0, cache->finalLnMean, cache->finalLnVar,
                        p->lnFg, dCls, g->lnFg, g->lnFb, D_MODEL, dFinalLnIn);

    // Backward scratch — single-threaded trainer, so file-static is fine.
    static float dOutNext[MAX_SEQ_LEN * D_MODEL];
    static float dAfterAttn[MAX_SEQ_LEN * D_MODEL];
    static float dFf2_buf[MAX_SEQ_LEN * D_MODEL];
    static float dFf1_buf[MAX_SEQ_LEN * FF_DIM];
    static float dFf1pre [MAX_SEQ_LEN * FF_DIM];
    static float dXLn2   [MAX_SEQ_LEN * D_MODEL];
    static float dProj_b [MAX_SEQ_LEN * D_MODEL];
    static float dXIn_b  [MAX_SEQ_LEN * D_MODEL];
    static float dAttnOut[MAX_SEQ_LEN * D_MODEL];
    static float dAttn   [MAX_SEQ_LEN * MAX_SEQ_LEN];
    static float dV_b    [MAX_SEQ_LEN * D_MODEL];
    static float dScores [MAX_SEQ_LEN * MAX_SEQ_LEN];
    static float dQ_b    [MAX_SEQ_LEN * D_MODEL];
    static float dK_b    [MAX_SEQ_LEN * D_MODEL];
    static float dXLn1   [MAX_SEQ_LEN * D_MODEL];

    memset(dOutNext, 0, sizeof(float) * L * D_MODEL);
    for (int d = 0; d < D_MODEL; d++) dOutNext[d] = dFinalLnIn[d];

    for (int li = N_LAYERS - 1; li >= 0; li--) {
        const LayerParams *lp = &p->layers[li];
        const LayerCache  *lc = &cache->layers[li];
        LayerParams       *lg = &g->layers[li];

        // out = afterAttn + ff2 → split.
        for (int i = 0; i < L * D_MODEL; i++) {
            dAfterAttn[i] = dOutNext[i];
            dFf2_buf[i]   = dOutNext[i];
        }

        // ff2[i][d] = sum_h Wff2[d][h] * ff1[i][h] + bff2[d]
        memset(dFf1_buf, 0, sizeof(float) * L * FF_DIM);
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float dy = dFf2_buf[i * D_MODEL + d];
                for (int h = 0; h < FF_DIM; h++) {
                    lg->Wff2[d * FF_DIM + h] += dy * lc->ff1[i * FF_DIM + h];
                    dFf1_buf[i * FF_DIM + h] += lp->Wff2[d * FF_DIM + h] * dy;
                }
                lg->bff2[d] += dy;
            }
        }
        // ReLU back.
        for (int i = 0; i < L * FF_DIM; i++) {
            dFf1pre[i] = lc->ff1pre[i] > 0 ? dFf1_buf[i] : 0;
        }
        // ff1pre[i][h] = sum_d Wff1[h][d] * xLn2[i][d] + bff1[h]
        memset(dXLn2, 0, sizeof(float) * L * D_MODEL);
        for (int i = 0; i < L; i++) {
            for (int h = 0; h < FF_DIM; h++) {
                float dy = dFf1pre[i * FF_DIM + h];
                if (dy == 0.f) continue;
                for (int d = 0; d < D_MODEL; d++) {
                    lg->Wff1[h * D_MODEL + d] += dy * lc->xLn2[i * D_MODEL + d];
                    dXLn2[i * D_MODEL + d] += lp->Wff1[h * D_MODEL + d] * dy;
                }
                lg->bff1[h] += dy;
            }
        }
        // Back through LN2 (per token) → adds to dAfterAttn.
        for (int i = 0; i < L; i++) {
            float dy[D_MODEL];
            for (int d = 0; d < D_MODEL; d++) dy[d] = dXLn2[i * D_MODEL + d];
            float dx[D_MODEL];
            layer_norm_backward(lc->afterAttn, i * D_MODEL, lc->ln2Mean[i], lc->ln2Var[i],
                                lp->ln2g, dy, lg->ln2g, lg->ln2b, D_MODEL, dx);
            for (int d = 0; d < D_MODEL; d++) dAfterAttn[i * D_MODEL + d] += dx[d];
        }

        // afterAttn = xIn + proj → split.
        memset(dXIn_b, 0, sizeof(float) * L * D_MODEL);
        for (int i = 0; i < L * D_MODEL; i++) {
            dProj_b[i] = dAfterAttn[i];
            dXIn_b[i] += dAfterAttn[i];
        }

        // proj = attnOut · Wo. dAttnOut, lg->Wo.
        memset(dAttnOut, 0, sizeof(float) * L * D_MODEL);
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float dy = dProj_b[i * D_MODEL + d];
                for (int dd = 0; dd < D_MODEL; dd++) {
                    lg->Wo[d * D_MODEL + dd] += dy * lc->attnOut[i * D_MODEL + dd];
                    dAttnOut[i * D_MODEL + dd] += lp->Wo[d * D_MODEL + dd] * dy;
                }
            }
        }

        // attnOut[i][d] = sum_j attn[i][j] * V[j][d]
        memset(dAttn, 0, sizeof(float) * L * L);
        memset(dV_b, 0, sizeof(float) * L * D_MODEL);
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float dy = dAttnOut[i * D_MODEL + d];
                if (dy == 0.f) continue;
                for (int j = 0; j < L; j++) {
                    dAttn[i * L + j] += dy * lc->V[j * D_MODEL + d];
                    dV_b[j * D_MODEL + d] += lc->attn[i * L + j] * dy;
                }
            }
        }

        // Softmax back row-wise.
        for (int i = 0; i < L; i++) {
            float dot = 0;
            for (int j = 0; j < L; j++) dot += lc->attn[i * L + j] * dAttn[i * L + j];
            for (int k = 0; k < L; k++) {
                dScores[i * L + k] = lc->attn[i * L + k] * (dAttn[i * L + k] - dot);
            }
        }

        // scores[i][j] = (Q[i] · K[j]) / sqrt(d). dQ, dK.
        float scale = 1.f / sqrtf((float)D_MODEL);
        memset(dQ_b, 0, sizeof(float) * L * D_MODEL);
        memset(dK_b, 0, sizeof(float) * L * D_MODEL);
        for (int i = 0; i < L; i++) {
            for (int j = 0; j < L; j++) {
                float ds = dScores[i * L + j] * scale;
                if (ds == 0.f) continue;
                for (int d = 0; d < D_MODEL; d++) {
                    dQ_b[i * D_MODEL + d] += ds * lc->K[j * D_MODEL + d];
                    dK_b[j * D_MODEL + d] += ds * lc->Q[i * D_MODEL + d];
                }
            }
        }

        // Q,K,V back through their linear maps.
        memset(dXLn1, 0, sizeof(float) * L * D_MODEL);
        for (int i = 0; i < L; i++) {
            for (int d = 0; d < D_MODEL; d++) {
                float dy = dQ_b[i * D_MODEL + d];
                if (dy != 0.f) {
                    for (int dd = 0; dd < D_MODEL; dd++) {
                        lg->Wq[d * D_MODEL + dd] += dy * lc->xLn1[i * D_MODEL + dd];
                        dXLn1[i * D_MODEL + dd] += lp->Wq[d * D_MODEL + dd] * dy;
                    }
                }
                float dyk = dK_b[i * D_MODEL + d];
                if (dyk != 0.f) {
                    for (int dd = 0; dd < D_MODEL; dd++) {
                        lg->Wk[d * D_MODEL + dd] += dyk * lc->xLn1[i * D_MODEL + dd];
                        dXLn1[i * D_MODEL + dd] += lp->Wk[d * D_MODEL + dd] * dyk;
                    }
                }
                float dyv = dV_b[i * D_MODEL + d];
                if (dyv != 0.f) {
                    for (int dd = 0; dd < D_MODEL; dd++) {
                        lg->Wv[d * D_MODEL + dd] += dyv * lc->xLn1[i * D_MODEL + dd];
                        dXLn1[i * D_MODEL + dd] += lp->Wv[d * D_MODEL + dd] * dyv;
                    }
                }
            }
        }
        // Back through LN1, adds to dXIn.
        for (int i = 0; i < L; i++) {
            float dy[D_MODEL];
            for (int d = 0; d < D_MODEL; d++) dy[d] = dXLn1[i * D_MODEL + d];
            float dx[D_MODEL];
            layer_norm_backward(lc->xIn, i * D_MODEL, lc->ln1Mean[i], lc->ln1Var[i],
                                lp->ln1g, dy, lg->ln1g, lg->ln1b, D_MODEL, dx);
            for (int d = 0; d < D_MODEL; d++) dXIn_b[i * D_MODEL + d] += dx[d];
        }

        memcpy(dOutNext, dXIn_b, sizeof(float) * L * D_MODEL);
    }

    // Embedding gradient: each position routes its dOutNext to embed[tok] + pos[i].
    for (int i = 0; i < L; i++) {
        int tok = cache->tokens[i];
        int tokOff = tok * D_MODEL;
        int posOff = i * D_MODEL;
        for (int d = 0; d < D_MODEL; d++) {
            float dy = dOutNext[i * D_MODEL + d];
            g->embed[tokOff + d] += dy;
            g->pos_embed[posOff + d] += dy;
        }
    }

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

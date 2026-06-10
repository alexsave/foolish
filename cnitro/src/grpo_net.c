// Policy network forward pass. See grpo_net.h for layout.

#include "grpo_net.h"
#include "grpo_encode.h"

#include <Accelerate/Accelerate.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- tiny PRNG -------------------------------------------------------------

static inline uint64_t xs_next(uint64_t *s) {
    uint64_t x = *s ? *s : 0x9E3779B97F4A7C15ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}
static inline float xs_uniform_open(uint64_t *s) {
    // (0, 1), exclusive — safe for logf in Box-Muller.
    uint64_t r = xs_next(s);
    return (float)(((r >> 40) + 1) * (1.0 / 16777217.0));
}
static inline float xs_normal(uint64_t *s) {
    float u1 = xs_uniform_open(s);
    float u2 = xs_uniform_open(s);
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

// --- alloc / free ----------------------------------------------------------

static float *xalloc_f(size_t n) {
    float *p = (float *)malloc(n * sizeof(float));
    if (!p) { fprintf(stderr, "grpo_net: alloc %zu floats failed\n", n); abort(); }
    memset(p, 0, n * sizeof(float));
    return p;
}

void grpo_net_alloc(GrpoNet *n) {
    n->W1  = xalloc_f((size_t)GRPO_H1   * STATE_DIM);
    n->b1  = xalloc_f(GRPO_H1);
    n->W2  = xalloc_f((size_t)GRPO_H2   * GRPO_H1);
    n->b2  = xalloc_f(GRPO_H2);
    n->W3  = xalloc_f((size_t)GRPO_EMBED * GRPO_H2);
    n->b3  = xalloc_f(GRPO_EMBED);
    n->Wh1 = xalloc_f((size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN);
    n->bh1 = xalloc_f(GRPO_HEAD_HIDDEN);
    n->Wh2 = xalloc_f((size_t)GRPO_HEAD_HIDDEN);
    n->bh2 = xalloc_f(1);
}

void grpo_net_free(GrpoNet *n) {
    free(n->W1);  free(n->b1);
    free(n->W2);  free(n->b2);
    free(n->W3);  free(n->b3);
    free(n->Wh1); free(n->bh1);
    free(n->Wh2); free(n->bh2);
    memset(n, 0, sizeof(*n));
}

size_t grpo_net_param_count(void) {
    return (size_t)GRPO_H1 * STATE_DIM + GRPO_H1
         + (size_t)GRPO_H2 * GRPO_H1   + GRPO_H2
         + (size_t)GRPO_EMBED * GRPO_H2 + GRPO_EMBED
         + (size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN + GRPO_HEAD_HIDDEN
         + (size_t)GRPO_HEAD_HIDDEN + 1;
}

// --- init ------------------------------------------------------------------

static void he_init(float *W, size_t out_dim, size_t in_dim, uint64_t *s) {
    float scale = sqrtf(2.0f / (float)in_dim);
    for (size_t i = 0; i < out_dim * in_dim; i++) W[i] = xs_normal(s) * scale;
}

void grpo_net_init_he(GrpoNet *n, uint64_t seed) {
    uint64_t s = seed ? seed : 0xC2B2AE3D27D4EB4FULL;
    he_init(n->W1,  GRPO_H1,          STATE_DIM,      &s);
    he_init(n->W2,  GRPO_H2,          GRPO_H1,        &s);
    he_init(n->W3,  GRPO_EMBED,       GRPO_H2,        &s);
    he_init(n->Wh1, GRPO_HEAD_HIDDEN, GRPO_HEAD_IN,   &s);
    // Output layer: small, near-uniform-logit init.
    float scale_out = 1.0f / sqrtf((float)GRPO_HEAD_HIDDEN);
    for (int i = 0; i < GRPO_HEAD_HIDDEN; i++) n->Wh2[i] = xs_normal(&s) * scale_out;
    // biases stay zero.
}

// --- workspace -------------------------------------------------------------

void grpo_workspace_alloc(GrpoWorkspace *ws, int max_moves) {
    ws->max_moves        = max_moves;
    ws->state_vec        = xalloc_f(STATE_DIM);
    ws->h1_vec           = xalloc_f(GRPO_H1);
    ws->h2_vec           = xalloc_f(GRPO_H2);
    ws->embed_vec        = xalloc_f(GRPO_EMBED);
    ws->moves_mat        = xalloc_f((size_t)max_moves * MOVE_FEAT_DIM);
    ws->head_in_mat      = xalloc_f((size_t)max_moves * GRPO_HEAD_IN);
    ws->head_hidden_mat  = xalloc_f((size_t)max_moves * GRPO_HEAD_HIDDEN);
    ws->logits           = xalloc_f(max_moves);
    ws->log_probs        = xalloc_f(max_moves);
    ws->dlogits          = xalloc_f(max_moves);
    ws->dhead_pre_mat    = xalloc_f((size_t)max_moves * GRPO_HEAD_HIDDEN);
    ws->dembed           = xalloc_f(GRPO_EMBED);
    ws->dh2              = xalloc_f(GRPO_H2);
    ws->dh1              = xalloc_f(GRPO_H1);
}

void grpo_workspace_free(GrpoWorkspace *ws) {
    free(ws->state_vec);  free(ws->h1_vec); free(ws->h2_vec); free(ws->embed_vec);
    free(ws->moves_mat);  free(ws->head_in_mat); free(ws->head_hidden_mat);
    free(ws->logits);     free(ws->log_probs);
    free(ws->dlogits);    free(ws->dhead_pre_mat);
    free(ws->dembed);     free(ws->dh2); free(ws->dh1);
    memset(ws, 0, sizeof(*ws));
}

// --- forward ---------------------------------------------------------------

static inline void relu_inplace(float *x, int n) {
    for (int i = 0; i < n; i++) if (x[i] < 0.0f) x[i] = 0.0f;
}

static inline void add_bias(float *x, const float *b, int n) {
    for (int i = 0; i < n; i++) x[i] += b[i];
}

// y[1,out] = x[1,in] @ W[out,in]^T + b[out].
static void linear_vec(const float *W, const float *b, int out_dim, int in_dim,
                       const float *x, float *y) {
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                1, out_dim, in_dim,
                1.0f, x, in_dim,
                W, in_dim,
                0.0f, y, out_dim);
    add_bias(y, b, out_dim);
}

// Y[M,out] = X[M,in] @ W[out,in]^T + b[out] (broadcast).
static void linear_mat(const float *W, const float *b, int M, int out_dim, int in_dim,
                       const float *X, float *Y) {
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                M, out_dim, in_dim,
                1.0f, X, in_dim,
                W, in_dim,
                0.0f, Y, out_dim);
    for (int i = 0; i < M; i++) add_bias(Y + (size_t)i * out_dim, b, out_dim);
}

void grpo_net_forward(const GrpoNet *n, GrpoWorkspace *ws,
                      const Game *g, int self_idx,
                      const LegalMoves *moves) {
    int M = moves->n;
    if (M <= 0) return;

    // 1. Encode state and trunk-forward once.
    grpo_encode_state(g, self_idx, ws->state_vec);

    linear_vec(n->W1, n->b1, GRPO_H1,    STATE_DIM, ws->state_vec, ws->h1_vec);
    relu_inplace(ws->h1_vec, GRPO_H1);
    linear_vec(n->W2, n->b2, GRPO_H2,    GRPO_H1,   ws->h1_vec,    ws->h2_vec);
    relu_inplace(ws->h2_vec, GRPO_H2);
    linear_vec(n->W3, n->b3, GRPO_EMBED, GRPO_H2,   ws->h2_vec,    ws->embed_vec);
    relu_inplace(ws->embed_vec, GRPO_EMBED);

    // 2. Encode each legal move, build the [M, EMBED+MOVE_FEAT] input matrix.
    grpo_encode_moves(g, moves, ws->moves_mat);
    for (int i = 0; i < M; i++) {
        float *row = ws->head_in_mat + (size_t)i * GRPO_HEAD_IN;
        memcpy(row, ws->embed_vec, GRPO_EMBED * sizeof(float));
        memcpy(row + GRPO_EMBED,
               ws->moves_mat + (size_t)i * MOVE_FEAT_DIM,
               MOVE_FEAT_DIM * sizeof(float));
    }

    // 3. Head: fc_h1 → ReLU → fc_h2.
    linear_mat(n->Wh1, n->bh1, M, GRPO_HEAD_HIDDEN, GRPO_HEAD_IN,
               ws->head_in_mat, ws->head_hidden_mat);
    relu_inplace(ws->head_hidden_mat, M * GRPO_HEAD_HIDDEN);

    // y[M,1] = head_hidden[M,HEAD_HIDDEN] @ Wh2[1,HEAD_HIDDEN]^T + bh2
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                M, 1, GRPO_HEAD_HIDDEN,
                1.0f, ws->head_hidden_mat, GRPO_HEAD_HIDDEN,
                n->Wh2, GRPO_HEAD_HIDDEN,
                0.0f, ws->logits, 1);
    for (int i = 0; i < M; i++) ws->logits[i] += n->bh2[0];

    // 4. Softmax → log_probs.
    float maxv = ws->logits[0];
    for (int i = 1; i < M; i++) if (ws->logits[i] > maxv) maxv = ws->logits[i];
    float sumexp = 0.0f;
    for (int i = 0; i < M; i++) {
        ws->log_probs[i] = ws->logits[i] - maxv;
        sumexp += expf(ws->log_probs[i]);
    }
    float log_sumexp = logf(sumexp);
    for (int i = 0; i < M; i++) ws->log_probs[i] -= log_sumexp;
}

// --- backward --------------------------------------------------------------
//
// Activations from the most recent forward live in ws:
//   state_vec, h1_vec, h2_vec, embed_vec      (post-ReLU; > 0 iff unit fired)
//   moves_mat, head_in_mat, head_hidden_mat   (head_hidden is post-ReLU)
//   logits, log_probs                         (log_probs is normalized)
//
// We never backprop into the move-feature encoder — those features come
// from a fixed, non-parametric mapping. Only the trunk + head weights
// receive gradients. `grads` is accumulated into (zero it before the
// minibatch).

float grpo_net_backward(const GrpoNet *n, GrpoWorkspace *ws,
                        int M, int chosen_idx, GrpoGrads *grads) {
    // Softmax-CE preamble: dlogit[i] = p[i] - 1_{i == chosen}
    float loss = -ws->log_probs[chosen_idx];
    for (int i = 0; i < M; i++) ws->dlogits[i] = expf(ws->log_probs[i]);
    ws->dlogits[chosen_idx] -= 1.0f;
    grpo_net_backward_from_dlogits(n, ws, M, grads);
    return loss;
}

void grpo_net_backward_from_dlogits(const GrpoNet *n, GrpoWorkspace *ws,
                                    int M, GrpoGrads *grads) {
    // 2. bh2 grad += sum dlogit[i]
    float dbh2 = 0.f;
    for (int i = 0; i < M; i++) dbh2 += ws->dlogits[i];
    grads->bh2[0] += dbh2;

    // 3. Wh2[HH] grad += head_hidden_mat^T[HH, M] @ dlogit[M]
    cblas_sgemv(CblasRowMajor, CblasTrans,
                M, GRPO_HEAD_HIDDEN, 1.0f,
                ws->head_hidden_mat, GRPO_HEAD_HIDDEN,
                ws->dlogits, 1,
                1.0f, grads->Wh2, 1);

    // 4. dhead_pre[i, h] = dlogit[i] * Wh2[h], gated by ReLU mask
    //    (head_hidden post-ReLU > 0 ⇒ unit fired)
    for (int i = 0; i < M; i++) {
        float dl = ws->dlogits[i];
        const float *hh = ws->head_hidden_mat + (size_t)i * GRPO_HEAD_HIDDEN;
        float *dpre     = ws->dhead_pre_mat   + (size_t)i * GRPO_HEAD_HIDDEN;
        for (int h = 0; h < GRPO_HEAD_HIDDEN; h++) {
            dpre[h] = (hh[h] > 0.0f) ? (dl * n->Wh2[h]) : 0.0f;
        }
    }

    // 5. Wh1[HH, HEAD_IN] grad += dhead_pre^T[HH, M] @ head_in_mat[M, HEAD_IN]
    cblas_sgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                GRPO_HEAD_HIDDEN, GRPO_HEAD_IN, M,
                1.0f,
                ws->dhead_pre_mat, GRPO_HEAD_HIDDEN,
                ws->head_in_mat,   GRPO_HEAD_IN,
                1.0f, grads->Wh1, GRPO_HEAD_IN);

    // 6. bh1[HH] grad += sum_i dhead_pre[i, :]
    for (int i = 0; i < M; i++) {
        const float *dpre = ws->dhead_pre_mat + (size_t)i * GRPO_HEAD_HIDDEN;
        for (int h = 0; h < GRPO_HEAD_HIDDEN; h++) grads->bh1[h] += dpre[h];
    }

    // 7. dhead_in[M, HEAD_IN] = dhead_pre[M, HH] @ Wh1[HH, HEAD_IN]
    //    (overwrite head_in_mat — we won't need the original after this)
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                M, GRPO_HEAD_IN, GRPO_HEAD_HIDDEN,
                1.0f,
                ws->dhead_pre_mat, GRPO_HEAD_HIDDEN,
                n->Wh1, GRPO_HEAD_IN,
                0.0f, ws->head_in_mat, GRPO_HEAD_IN);

    // 8. dembed = sum_i dhead_in[i, :EMBED]  (first EMBED dims; the last
    //    MOVE_FEAT_DIM dims correspond to per-move features that have no
    //    learnable upstream)
    memset(ws->dembed, 0, GRPO_EMBED * sizeof(float));
    for (int i = 0; i < M; i++) {
        const float *row = ws->head_in_mat + (size_t)i * GRPO_HEAD_IN;
        for (int e = 0; e < GRPO_EMBED; e++) ws->dembed[e] += row[e];
    }

    // 9. ReLU backward on embed.
    for (int e = 0; e < GRPO_EMBED; e++) {
        if (ws->embed_vec[e] <= 0.0f) ws->dembed[e] = 0.0f;
    }

    // 10. b3 grad += dembed
    for (int e = 0; e < GRPO_EMBED; e++) grads->b3[e] += ws->dembed[e];

    // 11. W3[EMBED, H2] grad += outer(dembed, h2_vec)
    cblas_sger(CblasRowMajor,
               GRPO_EMBED, GRPO_H2, 1.0f,
               ws->dembed, 1, ws->h2_vec, 1,
               grads->W3, GRPO_H2);

    // 12. dh2 = W3^T @ dembed   (W3 is [EMBED, H2] row-major)
    cblas_sgemv(CblasRowMajor, CblasTrans,
                GRPO_EMBED, GRPO_H2, 1.0f,
                n->W3, GRPO_H2,
                ws->dembed, 1,
                0.0f, ws->dh2, 1);

    // 13. ReLU backward on h2.
    for (int h = 0; h < GRPO_H2; h++) {
        if (ws->h2_vec[h] <= 0.0f) ws->dh2[h] = 0.0f;
    }

    // 14. b2 grad += dh2
    for (int h = 0; h < GRPO_H2; h++) grads->b2[h] += ws->dh2[h];

    // 15. W2[H2, H1] grad += outer(dh2, h1_vec)
    cblas_sger(CblasRowMajor,
               GRPO_H2, GRPO_H1, 1.0f,
               ws->dh2, 1, ws->h1_vec, 1,
               grads->W2, GRPO_H1);

    // 16. dh1 = W2^T @ dh2
    cblas_sgemv(CblasRowMajor, CblasTrans,
                GRPO_H2, GRPO_H1, 1.0f,
                n->W2, GRPO_H1,
                ws->dh2, 1,
                0.0f, ws->dh1, 1);

    // 17. ReLU backward on h1.
    for (int h = 0; h < GRPO_H1; h++) {
        if (ws->h1_vec[h] <= 0.0f) ws->dh1[h] = 0.0f;
    }

    // 18. b1 grad += dh1
    for (int h = 0; h < GRPO_H1; h++) grads->b1[h] += ws->dh1[h];

    // 19. W1[H1, STATE_DIM] grad += outer(dh1, state_vec)
    cblas_sger(CblasRowMajor,
               GRPO_H1, STATE_DIM, 1.0f,
               ws->dh1, 1, ws->state_vec, 1,
               grads->W1, STATE_DIM);
}

// --- Grad / Adam lifecycle -------------------------------------------------

void grpo_grads_alloc(GrpoGrads *g) {
    g->W1  = xalloc_f((size_t)GRPO_H1   * STATE_DIM);
    g->b1  = xalloc_f(GRPO_H1);
    g->W2  = xalloc_f((size_t)GRPO_H2   * GRPO_H1);
    g->b2  = xalloc_f(GRPO_H2);
    g->W3  = xalloc_f((size_t)GRPO_EMBED * GRPO_H2);
    g->b3  = xalloc_f(GRPO_EMBED);
    g->Wh1 = xalloc_f((size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN);
    g->bh1 = xalloc_f(GRPO_HEAD_HIDDEN);
    g->Wh2 = xalloc_f((size_t)GRPO_HEAD_HIDDEN);
    g->bh2 = xalloc_f(1);
}
void grpo_grads_free(GrpoGrads *g) {
    free(g->W1); free(g->b1); free(g->W2); free(g->b2);
    free(g->W3); free(g->b3); free(g->Wh1); free(g->bh1);
    free(g->Wh2); free(g->bh2);
    memset(g, 0, sizeof(*g));
}
void grpo_grads_zero(GrpoGrads *g) {
    memset(g->W1,  0, (size_t)GRPO_H1   * STATE_DIM * sizeof(float));
    memset(g->b1,  0, GRPO_H1 * sizeof(float));
    memset(g->W2,  0, (size_t)GRPO_H2   * GRPO_H1 * sizeof(float));
    memset(g->b2,  0, GRPO_H2 * sizeof(float));
    memset(g->W3,  0, (size_t)GRPO_EMBED * GRPO_H2 * sizeof(float));
    memset(g->b3,  0, GRPO_EMBED * sizeof(float));
    memset(g->Wh1, 0, (size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN * sizeof(float));
    memset(g->bh1, 0, GRPO_HEAD_HIDDEN * sizeof(float));
    memset(g->Wh2, 0, GRPO_HEAD_HIDDEN * sizeof(float));
    memset(g->bh2, 0, sizeof(float));
}

typedef struct { float *ptr; size_t n; } SliceF;
static int grpo_grads_slices(const GrpoGrads *g, SliceF s[10]) {
    s[0] = (SliceF){g->W1,  (size_t)GRPO_H1   * STATE_DIM};
    s[1] = (SliceF){g->b1,  GRPO_H1};
    s[2] = (SliceF){g->W2,  (size_t)GRPO_H2   * GRPO_H1};
    s[3] = (SliceF){g->b2,  GRPO_H2};
    s[4] = (SliceF){g->W3,  (size_t)GRPO_EMBED * GRPO_H2};
    s[5] = (SliceF){g->b3,  GRPO_EMBED};
    s[6] = (SliceF){g->Wh1, (size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN};
    s[7] = (SliceF){g->bh1, GRPO_HEAD_HIDDEN};
    s[8] = (SliceF){g->Wh2, GRPO_HEAD_HIDDEN};
    s[9] = (SliceF){g->bh2, 1};
    return 10;
}
static int grpo_net_slices(GrpoNet *n, SliceF s[10]) {
    s[0] = (SliceF){n->W1,  (size_t)GRPO_H1   * STATE_DIM};
    s[1] = (SliceF){n->b1,  GRPO_H1};
    s[2] = (SliceF){n->W2,  (size_t)GRPO_H2   * GRPO_H1};
    s[3] = (SliceF){n->b2,  GRPO_H2};
    s[4] = (SliceF){n->W3,  (size_t)GRPO_EMBED * GRPO_H2};
    s[5] = (SliceF){n->b3,  GRPO_EMBED};
    s[6] = (SliceF){n->Wh1, (size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN};
    s[7] = (SliceF){n->bh1, GRPO_HEAD_HIDDEN};
    s[8] = (SliceF){n->Wh2, GRPO_HEAD_HIDDEN};
    s[9] = (SliceF){n->bh2, 1};
    return 10;
}

double grpo_grads_l2_norm(const GrpoGrads *g) {
    SliceF s[10]; int ns = grpo_grads_slices(g, s);
    double sq = 0.0;
    for (int i = 0; i < ns; i++) {
        for (size_t k = 0; k < s[i].n; k++) sq += (double)s[i].ptr[k] * (double)s[i].ptr[k];
    }
    return sqrt(sq);
}

void grpo_grads_clip(GrpoGrads *g, float clip_norm) {
    if (clip_norm <= 0.0f) return;
    double norm = grpo_grads_l2_norm(g);
    if (norm <= (double)clip_norm) return;
    float scale = (float)((double)clip_norm / norm);
    SliceF s[10]; int ns = grpo_grads_slices(g, s);
    for (int i = 0; i < ns; i++) {
        for (size_t k = 0; k < s[i].n; k++) s[i].ptr[k] *= scale;
    }
}

void grpo_grads_scale(GrpoGrads *g, float s) {
    SliceF sl[10]; int ns = grpo_grads_slices(g, sl);
    for (int i = 0; i < ns; i++) {
        for (size_t k = 0; k < sl[i].n; k++) sl[i].ptr[k] *= s;
    }
}

void grpo_adam_init(GrpoAdam *opt, float lr, float beta1, float beta2,
                    float eps, float clip_norm) {
    grpo_grads_alloc(&opt->m);
    grpo_grads_alloc(&opt->v);
    grpo_grads_zero(&opt->m);
    grpo_grads_zero(&opt->v);
    opt->t         = 0;
    opt->lr        = lr;
    opt->beta1     = beta1;
    opt->beta2     = beta2;
    opt->eps       = eps;
    opt->clip_norm = clip_norm;
}
void grpo_adam_free(GrpoAdam *opt) {
    grpo_grads_free(&opt->m);
    grpo_grads_free(&opt->v);
    memset(opt, 0, sizeof(*opt));
}

void grpo_adam_step(GrpoAdam *opt, GrpoNet *n, GrpoGrads *grads) {
    if (opt->clip_norm > 0.0f) grpo_grads_clip(grads, opt->clip_norm);
    opt->t++;
    double bc1 = 1.0 - pow((double)opt->beta1, (double)opt->t);
    double bc2 = 1.0 - pow((double)opt->beta2, (double)opt->t);
    float lr_t = (float)((double)opt->lr * sqrt(bc2) / bc1);

    SliceF gp[10], mp[10], vp[10], np[10];
    grpo_grads_slices(grads,  gp);
    grpo_grads_slices(&opt->m, mp);
    grpo_grads_slices(&opt->v, vp);
    grpo_net_slices(n, np);

    for (int i = 0; i < 10; i++) {
        size_t N = gp[i].n;
        float *g_ = gp[i].ptr;
        float *m_ = mp[i].ptr;
        float *v_ = vp[i].ptr;
        float *p_ = np[i].ptr;
        for (size_t k = 0; k < N; k++) {
            float gk = g_[k];
            m_[k] = opt->beta1 * m_[k] + (1.0f - opt->beta1) * gk;
            v_[k] = opt->beta2 * v_[k] + (1.0f - opt->beta2) * gk * gk;
            p_[k] -= lr_t * m_[k] / (sqrtf(v_[k]) + opt->eps);
        }
    }
}

// --- Checkpoint I/O --------------------------------------------------------

#define GRPO_CKPT_MAGIC   0x47525043u   // "GRPC" on disk = "CPRG"
#define GRPO_CKPT_VERSION 1u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t state_dim;
    uint32_t move_feat_dim;
    uint32_t h1;
    uint32_t h2;
    uint32_t embed;
    uint32_t head_hidden;
    uint32_t reserved0;
    uint32_t reserved1;
} GrpoCkptHeader;
#pragma pack(pop)

bool grpo_net_save(const GrpoNet *n, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    GrpoCkptHeader h = {
        .magic = GRPO_CKPT_MAGIC,
        .version = GRPO_CKPT_VERSION,
        .state_dim = STATE_DIM,
        .move_feat_dim = MOVE_FEAT_DIM,
        .h1 = GRPO_H1, .h2 = GRPO_H2,
        .embed = GRPO_EMBED, .head_hidden = GRPO_HEAD_HIDDEN,
        .reserved0 = 0, .reserved1 = 0,
    };
    if (fwrite(&h, sizeof(h), 1, fp) != 1) { fclose(fp); return false; }
    SliceF s[10]; grpo_net_slices((GrpoNet *)n, s);
    for (int i = 0; i < 10; i++) {
        if (fwrite(s[i].ptr, sizeof(float), s[i].n, fp) != s[i].n) { fclose(fp); return false; }
    }
    fclose(fp);
    return true;
}

bool grpo_net_load(GrpoNet *n, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    GrpoCkptHeader h;
    if (fread(&h, sizeof(h), 1, fp) != 1) { fclose(fp); return false; }
    if (h.magic != GRPO_CKPT_MAGIC || h.version != GRPO_CKPT_VERSION
        || h.state_dim != STATE_DIM || h.move_feat_dim != MOVE_FEAT_DIM
        || h.h1 != GRPO_H1 || h.h2 != GRPO_H2 || h.embed != GRPO_EMBED
        || h.head_hidden != GRPO_HEAD_HIDDEN) {
        fclose(fp); return false;
    }
    SliceF s[10]; grpo_net_slices(n, s);
    for (int i = 0; i < 10; i++) {
        if (fread(s[i].ptr, sizeof(float), s[i].n, fp) != s[i].n) { fclose(fp); return false; }
    }
    fclose(fp);
    return true;
}

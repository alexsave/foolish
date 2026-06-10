// Policy network for the GRPO Durak bot.
//
// Architecture (~2.4M params at the default config):
//   trunk:  state[STATE_DIM]
//           → fc1[STATE_DIM → GRPO_H1]   ReLU
//           → fc2[GRPO_H1  → GRPO_H2]    ReLU
//           → fc3[GRPO_H2  → GRPO_EMBED] ReLU       = state_embed
//   head (per legal move):
//           concat(state_embed, move_feat[MOVE_FEAT_DIM])
//           → fc_h1[EMBED+MOVE_FEAT → HEAD_HIDDEN]  ReLU
//           → fc_h2[HEAD_HIDDEN → 1]                logit
//   softmax over legal moves → π(a | s).
//
// Weight matrices are stored as [out_dim][in_dim] row-major, matching the
// existing nn.c convention (we GEMM with CblasTrans on B). Bias vectors
// have length out_dim.
#ifndef CNITRO_GRPO_NET_H
#define CNITRO_GRPO_NET_H

#include "grpo_encode.h"
#include "legal.h"
#include <stddef.h>
#include <stdint.h>

// v2 arch: ~1.5x widths vs v1 (which capped at overall=3.118 on grpo_run4).
// Wider trunk + head with the new encoder features (hidden trumps, round
// phase, distance-to-defender) should give more capacity for high-PC and
// endgame regimes where v1 stalled.
#define GRPO_H1          1536
#define GRPO_H2          1536
#define GRPO_EMBED       768
#define GRPO_HEAD_HIDDEN 384
#define GRPO_HEAD_IN     (GRPO_EMBED + MOVE_FEAT_DIM)

typedef struct {
    // trunk
    float *W1;   // [GRPO_H1   * STATE_DIM]
    float *b1;   // [GRPO_H1]
    float *W2;   // [GRPO_H2   * GRPO_H1]
    float *b2;   // [GRPO_H2]
    float *W3;   // [GRPO_EMBED * GRPO_H2]
    float *b3;   // [GRPO_EMBED]
    // head
    float *Wh1;  // [GRPO_HEAD_HIDDEN * GRPO_HEAD_IN]
    float *bh1;  // [GRPO_HEAD_HIDDEN]
    float *Wh2;  // [1 * GRPO_HEAD_HIDDEN]
    float *bh2;  // [1]
} GrpoNet;

typedef struct {
    int   max_moves;
    // Forward — post-ReLU activations (also serve as ReLU mask during
    // backward: post > 0 iff the unit fired).
    float *state_vec;       // [STATE_DIM]
    float *h1_vec;          // [GRPO_H1]
    float *h2_vec;          // [GRPO_H2]
    float *embed_vec;       // [GRPO_EMBED]
    float *moves_mat;       // [max_moves * MOVE_FEAT_DIM]
    float *head_in_mat;     // [max_moves * GRPO_HEAD_IN]
    float *head_hidden_mat; // [max_moves * GRPO_HEAD_HIDDEN]
    float *logits;          // [max_moves]
    float *log_probs;       // [max_moves]
    // Backward — gradient scratch.
    float *dlogits;         // [max_moves]
    float *dhead_pre_mat;   // [max_moves * GRPO_HEAD_HIDDEN]
    float *dembed;          // [GRPO_EMBED]
    float *dh2;             // [GRPO_H2]
    float *dh1;             // [GRPO_H1]
} GrpoWorkspace;

typedef struct {
    float *W1, *b1;
    float *W2, *b2;
    float *W3, *b3;
    float *Wh1, *bh1;
    float *Wh2, *bh2;
} GrpoGrads;

typedef struct {
    GrpoGrads m;   // 1st-moment
    GrpoGrads v;   // 2nd-moment
    int       t;   // step counter (incremented before each update)
    float     lr;
    float     beta1;
    float     beta2;
    float     eps;
    float     clip_norm;   // 0 disables grad clipping
} GrpoAdam;

void   grpo_net_alloc(GrpoNet *n);
void   grpo_net_free(GrpoNet *n);
void   grpo_net_init_he(GrpoNet *n, uint64_t seed);
size_t grpo_net_param_count(void);

void grpo_workspace_alloc(GrpoWorkspace *ws, int max_moves);
void grpo_workspace_free(GrpoWorkspace *ws);

// Encode state + legal moves, run trunk + head, fill ws->logits and
// ws->log_probs with the softmax distribution over moves->moves[0..n-1].
// Caller's responsibility: moves->n <= ws->max_moves.
void grpo_net_forward(const GrpoNet *n, GrpoWorkspace *ws,
                      const Game *g, int self_idx,
                      const LegalMoves *moves);

// Backward + accumulate gradients for one sample. Assumes a forward pass
// was just run for the same (state, moves) and that ws still holds the
// activations. Adds gradients into `grads`. Returns the cross-entropy loss
// -log π(chosen | state).
//
// Caller controls when to zero `grads` (typically before a minibatch) and
// when to apply the optimizer step (typically after a minibatch).
float grpo_net_backward(const GrpoNet *n, GrpoWorkspace *ws,
                        int n_moves, int chosen_idx,
                        GrpoGrads *grads);

// Variant: backward starting from caller-supplied dlogits.
// `ws->dlogits[0..n_moves-1]` MUST be filled before this is called.
// Activations from the most recent forward must still be in `ws`. Used by
// the GRPO update where dlogits combines the clipped-surrogate gradient
// with the KL-to-π_ref gradient — both expressed in logit space.
void grpo_net_backward_from_dlogits(const GrpoNet *n, GrpoWorkspace *ws,
                                    int n_moves, GrpoGrads *grads);

// --- Grad / Adam lifecycle -------------------------------------------------

void grpo_grads_alloc(GrpoGrads *g);
void grpo_grads_free(GrpoGrads *g);
void grpo_grads_zero(GrpoGrads *g);
// L2 norm of all parameter gradients (used for clipping diagnostic).
double grpo_grads_l2_norm(const GrpoGrads *g);
// In-place clip: if ||g||₂ > clip_norm, scale all gradients by clip_norm/||g||₂.
void grpo_grads_clip(GrpoGrads *g, float clip_norm);
// Scale every gradient entry by `s`.
void grpo_grads_scale(GrpoGrads *g, float s);

void grpo_adam_init(GrpoAdam *opt, float lr, float beta1, float beta2,
                    float eps, float clip_norm);
void grpo_adam_free(GrpoAdam *opt);
void grpo_adam_step(GrpoAdam *opt, GrpoNet *n, GrpoGrads *grads);

// --- Checkpoint I/O --------------------------------------------------------

bool grpo_net_save(const GrpoNet *n, const char *path);
bool grpo_net_load(GrpoNet *n, const char *path);

#endif

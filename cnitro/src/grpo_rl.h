// GRPO RL trajectory collection + clipped-surrogate update.
//
// One iteration of GRPO:
//   1. collect — play B games with seat 0 = π_θ (sampling) and other seats
//      drawn from the opponent pool. Record seat-0's (state, action,
//      log_prob_old) tuples. After all games, compute group-relative
//      advantages over the batch of rewards.
//   2. update — K epochs of minibatched gradient steps on the clipped
//      surrogate objective + β·KL(π_θ || π_ref). The π_ref network is a
//      frozen copy (typically the SFT warm-start).
//   3. evaluate — every M iterations, run a fixed eval suite (cnitro_eval
//      vs handwritten across player counts) and decide whether to promote
//      the current π_θ to the opponent pool.
#ifndef CNITRO_GRPO_RL_H
#define CNITRO_GRPO_RL_H

#include "grpo_format.h"
#include "grpo_net.h"
#include "grpo_pool.h"
#include "legal.h"
#include <stddef.h>
#include <stdint.h>

// --- Trajectory storage ----------------------------------------------------

typedef struct {
    ObservableState state;
    LegalMove       chosen_move;
    int             chosen_idx;          // index in legal moves at collection
    int             n_legal_at_collection;
    float           log_prob_old;        // log π_θ_old(chosen|s)
} GrpoRlTuple;

typedef struct {
    int          num_players;
    int          seat0_finish;           // 1..num_players
    float        reward;
    float        advantage;              // (r - mean) / (std + eps); filled after batch
    int          n_tuples;
    int          tuple_capacity;
    GrpoRlTuple *tuples;
} GrpoTrajectory;

void grpo_trajectory_init(GrpoTrajectory *t);
void grpo_trajectory_free(GrpoTrajectory *t);
void grpo_trajectory_push(GrpoTrajectory *t, const GrpoRlTuple *tup);

// --- Batch collection ------------------------------------------------------

typedef struct {
    GrpoTrajectory *trajs;
    int             n_trajs;
} GrpoBatch;

void grpo_batch_init(GrpoBatch *b, int n);
void grpo_batch_free(GrpoBatch *b);

// Configuration for one collection pass.
typedef struct {
    int        n_games;               // total games this batch
    int        n_threads;
    uint32_t   base_seed;             // worker derives per-game seed from this
    int        min_players;           // sample pc in [min..max]
    int        max_players;
    bool       sample_actions;        // true = sample from softmax; false = argmax
} GrpoCollectRlConfig;

// Collect one batch. `pi_theta` is the current policy (used at seat 0).
// `pool` provides opponents (other seats). On return, `out_batch` holds B
// trajectories with their rewards filled; advantages are NOT yet assigned
// (call grpo_compute_advantages next).
int grpo_collect_batch(const GrpoCollectRlConfig *cfg,
                       const GrpoNet *pi_theta,
                       const GrpoPool *pool,
                       GrpoBatch *out_batch);

// Compute group-relative advantage A = (r - mean) / (std + eps) and assign
// it to every trajectory in the batch. After this, every tuple inherits
// its trajectory's advantage.
void grpo_compute_advantages(GrpoBatch *batch);

// --- Loss / update ---------------------------------------------------------

typedef struct {
    float clip_eps;     // surrogate clip (default 0.2)
    float kl_beta;      // KL coefficient (default 0.02; 0 disables KL)
    int   k_epochs;     // (default 2)
    int   minibatch_size;
} GrpoUpdateConfig;

typedef struct {
    double mean_policy_loss;
    double mean_kl;
    double mean_reward;
    double mean_advantage_abs;
    double mean_ratio;
    int    n_updated;
    int    n_clipped;
} GrpoUpdateStats;

// One full update pass: K epochs over the batch's tuples, minibatched.
// Updates `pi_theta` in place via Adam. `pi_ref` is read-only.
// Returns 0 on success.
int grpo_update(GrpoNet *pi_theta, GrpoAdam *opt,
                const GrpoNet *pi_ref,
                GrpoBatch *batch,
                const GrpoUpdateConfig *ucfg,
                GrpoUpdateStats *out_stats,
                uint32_t *rng);

#endif

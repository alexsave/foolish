// GRPO RL collector + update. See grpo_rl.h.

#include "grpo_rl.h"
#include "grpo_encode.h"
#include "grpo_format.h"
#include "grpo_pool.h"
#include "card.h"
#include "game.h"
#include "legal.h"
#include "strategy.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// --- xorshift32 -----------------------------------------------------------
static inline uint32_t xs32_local(uint32_t *s) {
    uint32_t x = *s ? *s : 0xA5A5A5A5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// --- Trajectory plumbing --------------------------------------------------

void grpo_trajectory_init(GrpoTrajectory *t) {
    memset(t, 0, sizeof(*t));
    t->tuple_capacity = 64;
    t->tuples = (GrpoRlTuple *)malloc(t->tuple_capacity * sizeof(GrpoRlTuple));
}

void grpo_trajectory_free(GrpoTrajectory *t) {
    free(t->tuples);
    memset(t, 0, sizeof(*t));
}

void grpo_trajectory_push(GrpoTrajectory *t, const GrpoRlTuple *tup) {
    if (t->n_tuples >= t->tuple_capacity) {
        t->tuple_capacity *= 2;
        t->tuples = (GrpoRlTuple *)realloc(t->tuples,
                                           t->tuple_capacity * sizeof(GrpoRlTuple));
    }
    t->tuples[t->n_tuples++] = *tup;
}

void grpo_batch_init(GrpoBatch *b, int n) {
    b->trajs = (GrpoTrajectory *)calloc((size_t)n, sizeof(GrpoTrajectory));
    b->n_trajs = n;
    for (int i = 0; i < n; i++) grpo_trajectory_init(&b->trajs[i]);
}

void grpo_batch_free(GrpoBatch *b) {
    for (int i = 0; i < b->n_trajs; i++) grpo_trajectory_free(&b->trajs[i]);
    free(b->trajs);
    memset(b, 0, sizeof(*b));
}

// --- One-game playback for collection -------------------------------------

// Sample categorical from softmax log_probs. Returns chosen index in [0, n).
static int sample_categorical(const float *log_probs, int n, uint32_t *rng) {
    uint32_t r = xs32_local(rng);
    float u = (float)(r & 0xFFFFFF) / (float)(1 << 24);  // [0,1)
    float acc = 0.0f;
    for (int i = 0; i < n - 1; i++) {
        acc += expf(log_probs[i]);
        if (u < acc) return i;
    }
    return n - 1;
}

// Drive one game. Seat 0 = pi_theta (sampling), other seats = pool members
// (argmax). Records seat-0's tuples in `traj`.
static void play_one_grpo(uint32_t seed, int num_players,
                          const GrpoNet *pi_theta,
                          const GrpoPool *pool,
                          GrpoWorkspace *ws_theta,
                          GrpoWorkspace *ws_pool,
                          bool sample_actions,
                          uint32_t *sampling_rng,
                          GrpoTrajectory *traj) {
    game_set_seed(seed);
    random_strategy_set_seed(seed);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)num_players;
    // Decide which pool member serves each non-seat-0 seat.
    int seat_member_idx[MAX_PLAYERS];
    seat_member_idx[0] = -1;
    for (int i = 1; i < num_players; i++) {
        seat_member_idx[i] = grpo_pool_sample(pool, sampling_rng);
    }
    for (int i = 0; i < num_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (i == 0) ? STRAT_DYNAMITE : STRAT_HANDWRITTEN;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    traj->num_players = num_players;
    traj->n_tuples    = 0;

    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 4000) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(game_random() * (i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int pi = elig[k];
            LegalMoves moves;
            calculate_legal_moves(&g, pi, &moves);
            if (moves.n == 0) continue;
            int chosen;
            if (pi == 0) {
                grpo_net_forward(pi_theta, ws_theta, &g, 0, &moves);
                if (sample_actions) chosen = sample_categorical(ws_theta->log_probs, moves.n, sampling_rng);
                else {
                    chosen = 0;
                    for (int m = 1; m < moves.n; m++) if (ws_theta->logits[m] > ws_theta->logits[chosen]) chosen = m;
                }
                GrpoRlTuple t;
                memset(&t, 0, sizeof(t));
                grpo_observable_state_build(&g, 0, &t.state);
                t.chosen_move           = moves.moves[chosen];
                t.chosen_idx            = chosen;
                t.n_legal_at_collection = moves.n;
                t.log_prob_old          = ws_theta->log_probs[chosen];
                grpo_trajectory_push(traj, &t);
            } else {
                int mi = seat_member_idx[pi];
                if (mi < 0 || mi >= pool->n_members) {
                    chosen = handwritten_strategy_choose(&g, pi, &moves, NULL);
                } else {
                    chosen = grpo_pool_member_choose(&pool->members[mi], &g, pi, &moves, ws_pool);
                }
            }
            if (chosen < 0 || chosen >= moves.n) continue;
            const LegalMove *m = &moves.moves[chosen];
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(&g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (&g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (&g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(&g, pi); break;
                case MOVE_GOOD:   ok = handle_good  (&g, pi); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }

    // Seat-0 finish position.
    int finish = num_players;  // default = durak
    for (int i = 0; i < g.num_eliminated; i++) {
        if (g.elimination_order[i] == 0) { finish = i + 1; break; }
    }
    traj->seat0_finish = finish;
    traj->reward = (num_players > 1)
                 ? (float)(num_players - finish) / (float)(num_players - 1)
                 : 0.0f;
    traj->advantage = 0.0f;
}

// --- Parallel batch collection --------------------------------------------

typedef struct {
    int                          worker_id;
    const GrpoCollectRlConfig   *cfg;
    const GrpoNet               *pi_theta;
    const GrpoPool              *pool;
    atomic_int                  *task_idx;
    GrpoBatch                   *batch;
} CollectWorkerCtx;

static void *collect_worker(void *arg) {
    CollectWorkerCtx *ctx = (CollectWorkerCtx *)arg;
    const GrpoCollectRlConfig *cfg = ctx->cfg;
    GrpoWorkspace ws_theta; grpo_workspace_alloc(&ws_theta, MAX_LEGAL_MOVES);
    GrpoWorkspace ws_pool;  grpo_workspace_alloc(&ws_pool,  MAX_LEGAL_MOVES);
    uint32_t rng = (uint32_t)((cfg->base_seed * 0xDEADBEEF) ^ ((uint32_t)ctx->worker_id * 0x1F2E3D4Cu));

    for (;;) {
        int gi = atomic_fetch_add(ctx->task_idx, 1);
        if (gi >= cfg->n_games) break;

        int pc_range = cfg->max_players - cfg->min_players + 1;
        int pc = cfg->min_players + (int)(xs32_local(&rng) % (uint32_t)pc_range);
        uint32_t game_seed = cfg->base_seed + (uint32_t)gi * 1664525u + 1013904223u;

        play_one_grpo(game_seed, pc,
                      ctx->pi_theta, ctx->pool,
                      &ws_theta, &ws_pool,
                      cfg->sample_actions,
                      &rng,
                      &ctx->batch->trajs[gi]);
    }

    grpo_workspace_free(&ws_theta);
    grpo_workspace_free(&ws_pool);
    return NULL;
}

int grpo_collect_batch(const GrpoCollectRlConfig *cfg,
                       const GrpoNet *pi_theta,
                       const GrpoPool *pool,
                       GrpoBatch *out_batch) {
    if (cfg->n_games < 1 || cfg->n_threads < 1) return 1;
    if (out_batch->n_trajs != cfg->n_games) {
        grpo_batch_free(out_batch);
        grpo_batch_init(out_batch, cfg->n_games);
    } else {
        for (int i = 0; i < out_batch->n_trajs; i++) out_batch->trajs[i].n_tuples = 0;
    }

    atomic_int task_idx = 0;
    pthread_t *threads = (pthread_t *)calloc((size_t)cfg->n_threads, sizeof(pthread_t));
    CollectWorkerCtx *ctxs = (CollectWorkerCtx *)calloc((size_t)cfg->n_threads, sizeof(CollectWorkerCtx));
    for (int i = 0; i < cfg->n_threads; i++) {
        ctxs[i].worker_id = i;
        ctxs[i].cfg = cfg;
        ctxs[i].pi_theta = pi_theta;
        ctxs[i].pool = pool;
        ctxs[i].task_idx = &task_idx;
        ctxs[i].batch = out_batch;
        pthread_create(&threads[i], NULL, collect_worker, &ctxs[i]);
    }
    for (int i = 0; i < cfg->n_threads; i++) pthread_join(threads[i], NULL);
    free(threads); free(ctxs);
    return 0;
}

void grpo_compute_advantages(GrpoBatch *batch) {
    if (batch->n_trajs == 0) return;
    double mean = 0.0;
    for (int i = 0; i < batch->n_trajs; i++) mean += batch->trajs[i].reward;
    mean /= batch->n_trajs;
    double var = 0.0;
    for (int i = 0; i < batch->n_trajs; i++) {
        double d = batch->trajs[i].reward - mean;
        var += d * d;
    }
    var /= batch->n_trajs;
    double sd = sqrt(var);
    float denom = (float)(sd + 1e-8);
    for (int i = 0; i < batch->n_trajs; i++) {
        batch->trajs[i].advantage = (float)((batch->trajs[i].reward - mean) / denom);
    }
}

// --- Update ---------------------------------------------------------------
//
// Per-tuple loss:
//   p_new   = softmax(z_new)
//   r       = exp(log p_new[chosen] - log_prob_old)
//   L_pol   = -min(r * A, clip(r, 1-eps, 1+eps) * A)
//   L_kl    = sum_a p_new[a] * (log p_new[a] - log p_ref[a])
//   loss    = L_pol + beta * L_kl
//
// dL_pol/dz[c]   = -ratio * A * (1 - p_new[c])      if not clipped, else 0
// dL_pol/dz[i!=c] = +ratio * A * p_new[i]           if not clipped, else 0
// dL_kl/dz[b]    = p_new[b] * (log p_new[b] - log p_ref[b] - L_kl)
//
// dlogits[i] = dL_pol/dz[i] + beta * dL_kl/dz[i]
//
// We feed the precomputed dlogits into the existing trunk/head backward
// by reusing grpo_net_backward's machinery — but since that function
// computes dlogits internally from chosen_idx, we inject our custom
// dlogits via ws->dlogits and then invoke a re-entrant helper.

// Helper: run only the head+trunk backward, assuming dlogits already in ws.
// This is grpo_net_backward minus the softmax-CE preamble at the top.
extern void grpo_net_backward_from_dlogits(const GrpoNet *n, GrpoWorkspace *ws,
                                           int M, GrpoGrads *grads);

int grpo_update(GrpoNet *pi_theta, GrpoAdam *opt,
                const GrpoNet *pi_ref,
                GrpoBatch *batch,
                const GrpoUpdateConfig *ucfg,
                GrpoUpdateStats *stats,
                uint32_t *rng) {
    if (stats) memset(stats, 0, sizeof(*stats));

    // Flatten tuples → indices for shuffling.
    int total_tuples = 0;
    for (int t = 0; t < batch->n_trajs; t++) total_tuples += batch->trajs[t].n_tuples;
    if (total_tuples == 0) return 0;
    typedef struct { int traj; int tup; } TupRef;
    TupRef *refs = (TupRef *)malloc((size_t)total_tuples * sizeof(TupRef));
    int k = 0;
    for (int t = 0; t < batch->n_trajs; t++) {
        for (int u = 0; u < batch->trajs[t].n_tuples; u++) {
            refs[k].traj = t; refs[k].tup = u; k++;
        }
    }

    GrpoWorkspace ws_theta; grpo_workspace_alloc(&ws_theta, MAX_LEGAL_MOVES);
    GrpoWorkspace ws_ref;   grpo_workspace_alloc(&ws_ref,   MAX_LEGAL_MOVES);
    GrpoGrads grads; grpo_grads_alloc(&grads);

    double sum_pol = 0.0, sum_kl = 0.0, sum_ratio = 0.0, sum_advabs = 0.0;
    int n_clipped = 0, n_updated = 0;

    for (int epoch = 0; epoch < ucfg->k_epochs; epoch++) {
        // Fisher-Yates shuffle.
        for (int i = total_tuples - 1; i > 0; i--) {
            uint32_t r = xs32_local(rng);
            int j = (int)(r % (uint32_t)(i + 1));
            TupRef tmp = refs[i]; refs[i] = refs[j]; refs[j] = tmp;
        }

        for (int batch_start = 0; batch_start < total_tuples; batch_start += ucfg->minibatch_size) {
            int batch_end = batch_start + ucfg->minibatch_size;
            if (batch_end > total_tuples) batch_end = total_tuples;
            grpo_grads_zero(&grads);
            int n_in_minibatch = 0;

            for (int bi = batch_start; bi < batch_end; bi++) {
                const TupRef *r = &refs[bi];
                const GrpoTrajectory *tr = &batch->trajs[r->traj];
                const GrpoRlTuple    *tu = &tr->tuples[r->tup];
                float A = tr->advantage;

                Game g; grpo_state_to_game(&tu->state, &g);
                LegalMoves moves;
                calculate_legal_moves(&g, tu->state.self_idx, &moves);
                if (moves.n == 0) continue;
                int chosen = grpo_legal_move_match(&moves, &tu->chosen_move);
                if (chosen < 0) continue;

                // Forward π_θ on this state.
                grpo_net_forward(pi_theta, &ws_theta, &g, tu->state.self_idx, &moves);
                // Forward π_ref for KL (optional).
                if (ucfg->kl_beta != 0.0f && pi_ref) {
                    grpo_net_forward(pi_ref, &ws_ref, &g, tu->state.self_idx, &moves);
                }

                // Ratio.
                float logp_new_c = ws_theta.log_probs[chosen];
                float logp_old_c = tu->log_prob_old;
                float dlog = logp_new_c - logp_old_c;
                // Clamp for numerical safety (prevents inf from blowing up).
                if (dlog > 20.0f)  dlog = 20.0f;
                if (dlog < -20.0f) dlog = -20.0f;
                float ratio = expf(dlog);

                // Clip status.
                float lo = 1.0f - ucfg->clip_eps;
                float hi = 1.0f + ucfg->clip_eps;
                float surr1 = ratio * A;
                float clipped_ratio = ratio < lo ? lo : (ratio > hi ? hi : ratio);
                float surr2 = clipped_ratio * A;
                bool  clipped = (ratio < lo) || (ratio > hi);
                // Policy gradient effective factor:
                //   if min picks surr1 (i.e., surr1 <= surr2) — gradient flows through ratio
                //   else (surr2 binding) — gradient is 0 from the clipped branch
                // We use the standard PPO rule: gradient flows iff the unclipped
                // surr1 is the binding term.
                bool ratio_dominates = (surr1 <= surr2);
                float policy_factor = ratio_dominates ? (-ratio * A) : 0.0f;

                // Build dlogits.
                int M = moves.n;
                float *p_new = ws_theta.logits;   // borrow buffer for probs
                for (int i = 0; i < M; i++) p_new[i] = expf(ws_theta.log_probs[i]);

                // KL on this sample (for stats and for grad).
                double kl_sample = 0.0;
                if (ucfg->kl_beta != 0.0f && pi_ref) {
                    for (int i = 0; i < M; i++) {
                        kl_sample += (double)p_new[i]
                                   * ((double)ws_theta.log_probs[i] - (double)ws_ref.log_probs[i]);
                    }
                }

                float *dl = ws_theta.dlogits;
                for (int i = 0; i < M; i++) {
                    float term_pol = policy_factor * ((i == chosen ? 1.0f : 0.0f) - p_new[i]);
                    float term_kl  = 0.0f;
                    if (ucfg->kl_beta != 0.0f && pi_ref) {
                        term_kl = (float)((double)p_new[i]
                                * ((double)ws_theta.log_probs[i]
                                   - (double)ws_ref.log_probs[i]
                                   - kl_sample));
                    }
                    dl[i] = term_pol + ucfg->kl_beta * term_kl;
                }

                grpo_net_backward_from_dlogits(pi_theta, &ws_theta, M, &grads);
                n_in_minibatch++;
                n_updated++;

                float L_pol = -fminf(surr1, surr2);
                sum_pol     += (double)L_pol;
                sum_kl      += kl_sample;
                sum_ratio   += (double)ratio;
                sum_advabs  += fabs((double)A);
                if (clipped) n_clipped++;
            }

            if (n_in_minibatch > 0) {
                grpo_grads_scale(&grads, 1.0f / (float)n_in_minibatch);
                grpo_adam_step(opt, pi_theta, &grads);
            }
        }
    }

    if (stats && n_updated > 0) {
        stats->mean_policy_loss   = sum_pol / n_updated;
        stats->mean_kl            = sum_kl  / n_updated;
        stats->mean_ratio         = sum_ratio / n_updated;
        stats->mean_advantage_abs = sum_advabs / n_updated;
        stats->n_updated          = n_updated;
        stats->n_clipped          = n_clipped;
        double rsum = 0.0;
        for (int t = 0; t < batch->n_trajs; t++) rsum += batch->trajs[t].reward;
        stats->mean_reward = rsum / batch->n_trajs;
    }

    grpo_grads_free(&grads);
    grpo_workspace_free(&ws_theta);
    grpo_workspace_free(&ws_ref);
    free(refs);
    return 0;
}

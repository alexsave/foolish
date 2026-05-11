// SFT dataset + training loop. See grpo_train.h.

#include "grpo_train.h"
#include "grpo_encode.h"
#include "grpo_format.h"
#include "grpo_net.h"
#include "legal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- small RNG (xorshift64) ------------------------------------------------

static inline uint64_t xs64(uint64_t *s) {
    uint64_t x = *s ? *s : 0x9E3779B97F4A7C15ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}

static double wall_secs(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- Dataset loader --------------------------------------------------------

static bool load_shard_file(const char *path, TupleRecord **all, size_t *n, size_t *cap) {
    GrpoShardReader r;
    if (!grpo_shard_reader_open(&r, path)) return true;  // missing overflow shard is fine
    TupleRecord t;
    while (grpo_shard_reader_next(&r, &t)) {
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 65536;
            *all = (TupleRecord *)realloc(*all, *cap * sizeof(TupleRecord));
            if (!*all) { fprintf(stderr, "dataset: oom growing to %zu\n", *cap); return false; }
        }
        (*all)[(*n)++] = t;
    }
    if (!grpo_shard_reader_close(&r)) {
        fprintf(stderr, "dataset: CRC/count mismatch in %s\n", path);
        return false;
    }
    return true;
}

bool grpo_dataset_load(GrpoDataset *d, const char *dir) {
    memset(d, 0, sizeof(*d));
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", dir);
    FILE *mf = fopen(manifest_path, "r");
    if (!mf) { fprintf(stderr, "dataset: cannot open %s\n", manifest_path); return false; }
    char line[512];
    int  shard_ids[1024];
    int  n_shards = 0;
    while (fgets(line, sizeof(line), mf)) {
        int sid;
        if (sscanf(line, "shard %d ", &sid) == 1 && n_shards < 1024) {
            shard_ids[n_shards++] = sid;
        }
    }
    fclose(mf);

    size_t cap = 0;
    for (int i = 0; i < n_shards; i++) {
        char p1[512], p2[512];
        snprintf(p1, sizeof(p1), "%s/shard_%03d.bin",    dir, shard_ids[i]);
        snprintf(p2, sizeof(p2), "%s/overflow_%03d.bin", dir, shard_ids[i]);
        if (!load_shard_file(p1, &d->all, &d->n_all, &cap)) { grpo_dataset_free(d); return false; }
        if (!load_shard_file(p2, &d->all, &d->n_all, &cap)) { grpo_dataset_free(d); return false; }
    }

    // Count per bucket, then fill index lists.
    for (size_t i = 0; i < d->n_all; i++) {
        const TupleRecord *t = &d->all[i];
        int pc = t->state.num_players - GRPO_MIN_PLAYERS;
        int r  = (int)t->role;
        if (pc < 0 || pc >= GRPO_PC_BUCKETS || r < 0 || r >= GRPO_ROLE_COUNT) continue;
        d->bucket_n[pc][r]++;
    }
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        for (int r = 0; r < GRPO_ROLE_COUNT; r++) {
            if (d->bucket_n[pc][r] == 0) continue;
            d->bucket_idx[pc][r] = (uint32_t *)malloc(d->bucket_n[pc][r] * sizeof(uint32_t));
            d->bucket_n[pc][r] = 0;   // re-zero so we can use as a fill cursor
        }
    }
    for (size_t i = 0; i < d->n_all; i++) {
        const TupleRecord *t = &d->all[i];
        int pc = t->state.num_players - GRPO_MIN_PLAYERS;
        int r  = (int)t->role;
        if (pc < 0 || pc >= GRPO_PC_BUCKETS || r < 0 || r >= GRPO_ROLE_COUNT) continue;
        d->bucket_idx[pc][r][d->bucket_n[pc][r]++] = (uint32_t)i;
    }
    // Build active-cells list.
    d->n_active = 0;
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        for (int r = 0; r < GRPO_ROLE_COUNT; r++) {
            if (d->bucket_n[pc][r] > 0) {
                d->active_cells[d->n_active].pc   = pc;
                d->active_cells[d->n_active].role = r;
                d->n_active++;
            }
        }
    }
    return true;
}

void grpo_dataset_free(GrpoDataset *d) {
    if (d->all) free(d->all);
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        for (int r = 0; r < GRPO_ROLE_COUNT; r++) {
            if (d->bucket_idx[pc][r]) free(d->bucket_idx[pc][r]);
        }
    }
    memset(d, 0, sizeof(*d));
}

const TupleRecord *grpo_dataset_sample(const GrpoDataset *d, uint32_t *rng_lo) {
    if (d->n_active == 0 || d->n_all == 0) return NULL;
    uint64_t s = *rng_lo ? (uint64_t)*rng_lo : 0xDEADBEEFCAFEBABEULL;
    uint64_t r1 = xs64(&s);
    int ci = (int)(r1 % (uint64_t)d->n_active);
    int pc = d->active_cells[ci].pc;
    int rl = d->active_cells[ci].role;
    uint64_t r2 = xs64(&s);
    size_t pi = (size_t)(r2 % d->bucket_n[pc][rl]);
    *rng_lo = (uint32_t)s;
    return &d->all[d->bucket_idx[pc][rl][pi]];
}

// --- Metrics ---------------------------------------------------------------

void grpo_metrics_zero(GrpoMetrics *m) { memset(m, 0, sizeof(*m)); }

void grpo_metrics_update(GrpoMetrics *m, const GrpoWorkspace *ws,
                         int n_moves, int chosen,
                         int pc_bucket, int role_bucket) {
    float lp_chosen = ws->log_probs[chosen];
    double ce = -(double)lp_chosen;

    // top-1 / top-3 by logits (logits and log_probs have the same ordering).
    int best_idx = 0;
    for (int i = 1; i < n_moves; i++) if (ws->logits[i] > ws->logits[best_idx]) best_idx = i;
    int top1 = (best_idx == chosen) ? 1 : 0;

    // top-3: count how many move logits exceed the chosen move's logit.
    float chosen_logit = ws->logits[chosen];
    int n_above = 0;
    for (int i = 0; i < n_moves; i++) if (i != chosen && ws->logits[i] > chosen_logit) n_above++;
    int top3 = (n_above < 3) ? 1 : 0;

    double H = 0.0;
    for (int i = 0; i < n_moves; i++) {
        float lp = ws->log_probs[i];
        double p = exp((double)lp);
        H += -p * (double)lp;
    }

    m->sum_ce      += ce;
    m->sum_top1    += top1;
    m->sum_top3    += top3;
    m->sum_entropy += H;
    m->count++;

    if (pc_bucket >= 0 && pc_bucket < GRPO_PC_BUCKETS
        && role_bucket >= 0 && role_bucket < GRPO_ROLE_COUNT) {
        m->bucket_ce  [pc_bucket][role_bucket] += ce;
        m->bucket_top1[pc_bucket][role_bucket] += top1;
        m->bucket_top3[pc_bucket][role_bucket] += top3;
        m->bucket_count[pc_bucket][role_bucket]++;
    }
}

static void metrics_print(const GrpoMetrics *m, const char *tag) {
    double ce = m->count ? m->sum_ce / m->count : 0.0;
    double t1 = m->count ? m->sum_top1 / m->count : 0.0;
    double t3 = m->count ? m->sum_top3 / m->count : 0.0;
    double H  = m->count ? m->sum_entropy / m->count : 0.0;
    fprintf(stderr, "  %s: n=%llu CE=%.4f top1=%.3f top3=%.3f H=%.3f\n",
            tag, (unsigned long long)m->count, ce, t1, t3, H);
    fprintf(stderr, "       %12s %12s %12s %12s\n", "atk", "def", "coa", "idl");
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        char row[320]; int off = 0;
        off += snprintf(row + off, sizeof(row) - off, "  pc=%d", pc + GRPO_MIN_PLAYERS);
        for (int r = 0; r < GRPO_ROLE_COUNT; r++) {
            uint64_t c = m->bucket_count[pc][r];
            if (c == 0) {
                off += snprintf(row + off, sizeof(row) - off, " %12s", "—");
            } else {
                double bt1 = m->bucket_top1[pc][r] / c;
                off += snprintf(row + off, sizeof(row) - off,
                                "  %.2f(%4llu)", bt1, (unsigned long long)c);
            }
        }
        fprintf(stderr, "%s\n", row);
    }
}

// --- Per-sample forward + backward ----------------------------------------

// Returns: cross-entropy loss for this sample. Updates `metrics` if non-NULL.
// `accumulate_grads` controls whether backward runs (false for eval).
static float sft_one_step(const TupleRecord *t,
                          const GrpoNet *n, GrpoWorkspace *ws, GrpoGrads *grads,
                          GrpoMetrics *metrics,
                          bool accumulate_grads) {
    Game g; grpo_state_to_game(&t->state, &g);
    LegalMoves moves;
    calculate_legal_moves(&g, t->state.self_idx, &moves);
    if (moves.n == 0) return 0.0f;

    int chosen = grpo_legal_move_match(&moves, &t->chosen_move);
    if (chosen < 0) {
        fprintf(stderr, "sft_one_step: chosen move not found in recomputed legal moves "
                "(num_players=%d self=%d move_type=%d n_cards=%d)\n",
                t->state.num_players, t->state.self_idx,
                t->chosen_move.type, t->chosen_move.n_cards);
        return 0.0f;
    }

    grpo_net_forward(n, ws, &g, t->state.self_idx, &moves);

    int pc = t->state.num_players - GRPO_MIN_PLAYERS;
    int rl = (int)t->role;
    if (metrics) grpo_metrics_update(metrics, ws, moves.n, chosen, pc, rl);

    float loss;
    if (accumulate_grads) {
        loss = grpo_net_backward(n, ws, moves.n, chosen, grads);
    } else {
        loss = -ws->log_probs[chosen];
    }
    return loss;
}

// --- Training driver -------------------------------------------------------

int grpo_sft_run(const GrpoSftConfig *cfg) {
    GrpoDataset train, val;
    fprintf(stderr, "loading train corpus: %s\n", cfg->train_dir);
    if (!grpo_dataset_load(&train, cfg->train_dir)) return 1;
    fprintf(stderr, "  train: %llu tuples, %d active cells\n",
            (unsigned long long)train.n_all, train.n_active);
    fprintf(stderr, "loading val corpus: %s\n", cfg->val_dir);
    if (!grpo_dataset_load(&val, cfg->val_dir)) { grpo_dataset_free(&train); return 1; }
    fprintf(stderr, "  val:   %llu tuples, %d active cells\n",
            (unsigned long long)val.n_all, val.n_active);

    GrpoNet net; grpo_net_alloc(&net); grpo_net_init_he(&net, cfg->seed ^ 0xA5A5);
    GrpoWorkspace ws; grpo_workspace_alloc(&ws, MAX_LEGAL_MOVES);
    GrpoGrads grads; grpo_grads_alloc(&grads); grpo_grads_zero(&grads);
    GrpoAdam opt; grpo_adam_init(&opt, cfg->lr, cfg->adam_beta1, cfg->adam_beta2,
                                 cfg->adam_eps, cfg->clip_norm);

    uint32_t rng = (uint32_t)(cfg->seed ? cfg->seed : 1);
    uint32_t rng_val = (uint32_t)(cfg->seed ^ 0xBEEFu);

    float best_val_top1 = 0.0f;
    bool  saved_ckpt    = false;
    double prev_val_ce  = 1e30;
    int   plateau_run   = 0;
    double t0 = wall_secs();

    // Running training-CE estimate, smoothed over the last `heartbeat_every`
    // steps. Printed as a heartbeat so the user sees motion between full
    // eval cycles (which only fire every `eval_every` steps).
    int heartbeat_every = 50;
    double recent_ce_sum = 0.0;
    uint64_t recent_n = 0;

    for (int step = 1; step <= cfg->max_steps; step++) {
        grpo_grads_zero(&grads);
        double batch_loss = 0.0;
        int batch_n = 0;
        for (int i = 0; i < cfg->batch_size; i++) {
            const TupleRecord *t = grpo_dataset_sample(&train, &rng);
            if (!t) continue;
            float L = sft_one_step(t, &net, &ws, &grads, NULL, true);
            batch_loss += (double)L;
            batch_n++;
        }
        if (batch_n > 0) {
            grpo_grads_scale(&grads, 1.0f / (float)batch_n);
            grpo_adam_step(&opt, &net, &grads);
            recent_ce_sum += batch_loss;
            recent_n      += batch_n;
        }

        if (step % heartbeat_every == 0 && step % cfg->eval_every != 0) {
            double recent = recent_n ? recent_ce_sum / recent_n : 0.0;
            fprintf(stderr, "  step %5d  t=%.1fs  train_CE(last %d batches)=%.4f\n",
                    step, wall_secs() - t0, heartbeat_every, recent);
            recent_ce_sum = 0.0; recent_n = 0;
        }

        if (step % cfg->eval_every == 0 || step == cfg->max_steps) {
            GrpoMetrics M; grpo_metrics_zero(&M);
            for (int i = 0; i < cfg->eval_samples; i++) {
                const TupleRecord *t = grpo_dataset_sample(&val, &rng_val);
                if (!t) continue;
                sft_one_step(t, &net, &ws, &grads, &M, false);
            }
            double train_avg = batch_n ? batch_loss / batch_n : 0.0;
            double val_ce = M.count ? M.sum_ce / M.count : 0.0;
            double val_t1 = M.count ? M.sum_top1 / M.count : 0.0;
            double val_t3 = M.count ? M.sum_top3 / M.count : 0.0;
            double val_H  = M.count ? M.sum_entropy / M.count : 0.0;
            double elapsed = wall_secs() - t0;
            fprintf(stderr,
                    "[step %5d] t=%.1fs  train_CE=%.4f  val_CE=%.4f  top1=%.3f  top3=%.3f  H=%.3f\n",
                    step, elapsed, train_avg, val_ce, val_t1, val_t3, val_H);
            metrics_print(&M, "val");

            if (!saved_ckpt && val_t1 >= cfg->target_top1) {
                if (grpo_net_save(&net, cfg->ckpt_out)) {
                    fprintf(stderr, "  target top1 reached; saved %s\n", cfg->ckpt_out);
                    saved_ckpt = true;
                }
            }
            if (val_t1 > best_val_top1) best_val_top1 = (float)val_t1;

            if (fabs(val_ce - prev_val_ce) < cfg->plateau_tol) plateau_run++;
            else plateau_run = 0;
            prev_val_ce = val_ce;

            if (saved_ckpt) {
                fprintf(stderr, "  stopping: target top1 hit\n");
                break;
            }
            if (plateau_run >= cfg->plateau_window) {
                fprintf(stderr, "  stopping: val CE plateau (%d consecutive evals)\n", plateau_run);
                break;
            }
        }
    }

    if (!saved_ckpt) {
        if (grpo_net_save(&net, cfg->ckpt_out)) {
            fprintf(stderr, "saved final ckpt at %s (best val top1 was %.3f)\n",
                    cfg->ckpt_out, best_val_top1);
        }
    }

    grpo_adam_free(&opt);
    grpo_grads_free(&grads);
    grpo_workspace_free(&ws);
    grpo_net_free(&net);
    grpo_dataset_free(&train);
    grpo_dataset_free(&val);
    return 0;
}

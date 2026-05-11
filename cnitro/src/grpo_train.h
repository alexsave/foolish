// SFT dataset loader + training loop for the GRPO policy.
//
// Dataset (in-memory): walks a corpus directory's manifest, deserializes
// every tuple from every shard + overflow, indexes them per (pc, role)
// bucket. Sampling is stratified — each minibatch sample picks uniformly
// from populated (pc, role) cells, then uniformly within the cell.
//
// Training loop: per-sample forward/backward into accumulating grads, Adam
// step per minibatch, eval every N steps. Stops at target val top-1 or
// when val cross-entropy plateaus.
#ifndef CNITRO_GRPO_TRAIN_H
#define CNITRO_GRPO_TRAIN_H

#include "grpo_format.h"
#include "grpo_net.h"
#include "grpo_collect.h"   // GRPO_PC_BUCKETS / GRPO_MIN_PLAYERS
#include <stddef.h>
#include <stdint.h>

// --- Dataset ---------------------------------------------------------------

typedef struct {
    int pc;        // 0..GRPO_PC_BUCKETS-1
    int role;      // GrpoRole as int
} GrpoCell;

typedef struct {
    TupleRecord *all;
    size_t       n_all;

    uint32_t    *bucket_idx[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    size_t       bucket_n  [GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];

    GrpoCell     active_cells[GRPO_PC_BUCKETS * GRPO_ROLE_COUNT];
    int          n_active;
} GrpoDataset;

// Walk `dir`/manifest.txt, open every main + overflow shard, deserialize
// all tuples into `d`. Returns false on any I/O or CRC error.
bool grpo_dataset_load(GrpoDataset *d, const char *dir);
void grpo_dataset_free(GrpoDataset *d);

// One stratified sample: uniform over populated (pc, role) cells, then
// uniform within the cell.
const TupleRecord *grpo_dataset_sample(const GrpoDataset *d, uint32_t *rng);

// --- Training metrics ------------------------------------------------------

typedef struct {
    double sum_ce;
    double sum_top1;
    double sum_top3;
    double sum_entropy;
    uint64_t count;
    // Per-bucket
    double   bucket_ce[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    double   bucket_top1[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    double   bucket_top3[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    uint64_t bucket_count[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
} GrpoMetrics;

void grpo_metrics_zero(GrpoMetrics *m);
// Update one sample. logits/log_probs already computed in the workspace.
void grpo_metrics_update(GrpoMetrics *m, const GrpoWorkspace *ws,
                         int n_moves, int chosen,
                         int pc_bucket, int role_bucket);

// --- Training driver -------------------------------------------------------

typedef struct {
    const char *train_dir;
    const char *val_dir;
    const char *ckpt_out;        // where to dump the SFT checkpoint
    int   batch_size;            // minibatch size
    int   max_steps;             // hard cap
    int   eval_every;            // steps between eval cycles
    int   eval_samples;          // tuples sampled per eval cycle
    float lr;
    float adam_beta1;
    float adam_beta2;
    float adam_eps;
    float clip_norm;
    float target_top1;           // stop when val top-1 reaches this
    int   plateau_window;        // num evals with CE Δ < plateau_tol
    float plateau_tol;
    uint64_t seed;               // RNG for dataset sampling
} GrpoSftConfig;

int grpo_sft_run(const GrpoSftConfig *cfg);

#endif

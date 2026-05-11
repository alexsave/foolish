// SFT corpus collector — drives parallel handwritten-vs-handwritten games
// across player counts 2..8 and dumps per-decision tuples to shard files.
//
// Threading model: pthread work-stealing on a shared atomic task counter.
// Each worker plays whole games end-to-end. Per-(player_count, role) bucket
// counts are shared atomics; workers consult them to bias new-game player-
// count selection toward under-filled buckets. Tuples in already-full
// buckets go to an overflow shard rather than being dropped.
//
// Each worker writes to its own main shard + (optional) overflow shard;
// no cross-worker locks during writing. The manifest is written by the
// main thread after all workers exit.
#ifndef CNITRO_GRPO_COLLECT_H
#define CNITRO_GRPO_COLLECT_H

#include "grpo_format.h"
#include <stdint.h>

#define GRPO_MIN_PLAYERS 2
#define GRPO_MAX_PLAYERS 8
#define GRPO_PC_BUCKETS  (GRPO_MAX_PLAYERS - GRPO_MIN_PLAYERS + 1)  // 7

typedef struct {
    int        num_games;          // total games to play across all workers
    int        num_threads;
    uint32_t   base_seed;
    int        target_per_bucket;  // 0 = no cap, everything to main shard
    const char *out_dir;
    int        verbose;
} GrpoCollectConfig;

typedef struct {
    uint64_t bucket_counts[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    uint64_t overflow_counts[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    uint64_t total_games;
    uint64_t total_tuples;
    uint64_t total_overflow;
    double   wall_secs;
} GrpoCollectStats;

// Run the collector. Returns 0 on success.
int grpo_collect_run(const GrpoCollectConfig *cfg, GrpoCollectStats *stats);

#endif

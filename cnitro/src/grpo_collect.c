// SFT collector — see grpo_collect.h.

#include "grpo_collect.h"
#include "grpo_format.h"
#include "game.h"
#include "legal.h"
#include "strategy.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---- Per-worker RNG (used only for sampling player counts / shuffles) ----
//
// Independent from the game's TLS LCGs so player-count picks don't disturb
// the game's reproducible RNG sequence.

static inline uint32_t worker_rng_next(uint32_t *s) {
    uint32_t x = *s ? *s : 0xA5A5A5A5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// ---- Worker context shared across threads --------------------------------

typedef struct {
    int worker_id;
    const GrpoCollectConfig *cfg;
    atomic_ullong *task_idx;
    atomic_ullong (*bucket_counts)[GRPO_ROLE_COUNT];
    atomic_ullong (*overflow_counts)[GRPO_ROLE_COUNT];
    // worker-local results
    uint64_t games_played;
    uint64_t tuples_main;
    uint64_t tuples_overflow;
} WorkerCtx;

// ---- Sampling --------------------------------------------------------------

static int sample_player_count(uint32_t *rng,
                               atomic_ullong (*bc)[GRPO_ROLE_COUNT],
                               int target) {
    if (target <= 0) {
        // No bias — uniform.
        return GRPO_MIN_PLAYERS + (int)(worker_rng_next(rng) % GRPO_PC_BUCKETS);
    }
    uint64_t slack[GRPO_PC_BUCKETS];
    uint64_t total = 0;
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        uint64_t s = 0;
        for (int r = 0; r < GRPO_ROLE_COUNT; r++) {
            uint64_t c = atomic_load(&bc[pc][r]);
            if (c < (uint64_t)target) s += (uint64_t)target - c;
        }
        // +1 ensures every PC has a nonzero weight even when fully satisfied.
        slack[pc] = s + 1;
        total += slack[pc];
    }
    uint64_t pick = worker_rng_next(rng) % total;
    uint64_t acc = 0;
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        acc += slack[pc];
        if (pick < acc) return GRPO_MIN_PLAYERS + pc;
    }
    return GRPO_MAX_PLAYERS;
}

// ---- Game loop -------------------------------------------------------------

static int count_in_players(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].status != PLAYER_STATUS_OUT) n++;
    }
    return n;
}

// Plays one all-handwritten game at `num_players` seats. Emits a tuple per
// decision-point. Returns the number of decisions emitted (across all seats).
static int play_handwritten_game(
        uint32_t game_seed, int num_players,
        GrpoShardWriter *w_main, GrpoShardWriter *w_overflow,
        atomic_ullong (*bucket_counts)[GRPO_ROLE_COUNT],
        atomic_ullong (*overflow_counts)[GRPO_ROLE_COUNT],
        int target_per_bucket,
        uint64_t *out_main, uint64_t *out_overflow) {
    // Two TLS LCGs are independent. We seed game_seed into both so the
    // entire game (engine RNG + any future random-strategy ties) is fully
    // determined by the single per-game seed.
    game_set_seed(game_seed);
    random_strategy_set_seed(game_seed);

    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)num_players;
    for (int i = 0; i < num_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = STRAT_HANDWRITTEN;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    int n_decisions = 0;
    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 4000) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        // Fisher-Yates shuffle the eligible set (matches main_eval pattern).
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(game_random() * (i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            int tmp = elig[i]; elig[i] = elig[j]; elig[j] = tmp;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int p = elig[k];
            LegalMoves moves;
            calculate_legal_moves(&g, p, &moves);
            if (moves.n == 0) continue;
            int chosen = handwritten_strategy_choose(&g, p, &moves, NULL);
            if (chosen < 0 || chosen >= moves.n) continue;
            const LegalMove *m = &moves.moves[chosen];

            // Emit tuple from this seat's POV BEFORE the move is applied.
            TupleRecord t;
            int n_live = count_in_players(&g);
            grpo_tuple_build(&g, p, m, game_seed, (uint16_t)n_decisions, n_live, &t);

            int pc_bucket   = g.num_players - GRPO_MIN_PLAYERS;
            int role_bucket = (int)t.role;
            bool to_overflow = false;
            if (target_per_bucket > 0 && w_overflow) {
                uint64_t cur = atomic_load(&bucket_counts[pc_bucket][role_bucket]);
                if (cur >= (uint64_t)target_per_bucket) to_overflow = true;
            }
            if (to_overflow) {
                grpo_shard_append(w_overflow, &t);
                atomic_fetch_add(&overflow_counts[pc_bucket][role_bucket], 1);
                (*out_overflow)++;
            } else {
                grpo_shard_append(w_main, &t);
                atomic_fetch_add(&bucket_counts[pc_bucket][role_bucket], 1);
                (*out_main)++;
            }
            n_decisions++;

            // Apply the move.
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(&g, p, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (&g, p, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (&g, p, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(&g, p); break;
                case MOVE_GOOD:   ok = handle_good  (&g, p); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }
    return n_decisions;
}

// ---- Worker thread ---------------------------------------------------------

static void *worker_main(void *arg) {
    WorkerCtx *ctx = (WorkerCtx *)arg;
    const GrpoCollectConfig *cfg = ctx->cfg;

    char path_main[512], path_overflow[512];
    snprintf(path_main, sizeof(path_main), "%s/shard_%03d.bin", cfg->out_dir, ctx->worker_id);
    snprintf(path_overflow, sizeof(path_overflow), "%s/overflow_%03d.bin", cfg->out_dir, ctx->worker_id);

    GrpoShardWriter w_main; memset(&w_main, 0, sizeof(w_main));
    GrpoShardWriter w_over; memset(&w_over, 0, sizeof(w_over));
    if (!grpo_shard_open(&w_main, path_main, (uint32_t)ctx->worker_id, cfg->base_seed)) {
        fprintf(stderr, "worker %d: cannot open %s\n", ctx->worker_id, path_main);
        return NULL;
    }
    bool overflow_open = false;
    if (cfg->target_per_bucket > 0) {
        if (!grpo_shard_open(&w_over, path_overflow, (uint32_t)ctx->worker_id, cfg->base_seed)) {
            fprintf(stderr, "worker %d: cannot open %s\n", ctx->worker_id, path_overflow);
            grpo_shard_close(&w_main);
            return NULL;
        }
        overflow_open = true;
    }

    uint32_t rng = (uint32_t)(0x9E3779B9u ^ (ctx->worker_id * 2654435761u));

    for (;;) {
        uint64_t gi = atomic_fetch_add(ctx->task_idx, 1);
        if (gi >= (uint64_t)cfg->num_games) break;

        int pc = sample_player_count(&rng, ctx->bucket_counts, cfg->target_per_bucket);
        uint32_t game_seed = cfg->base_seed + (uint32_t)gi;
        play_handwritten_game(game_seed, pc,
                              &w_main, overflow_open ? &w_over : NULL,
                              ctx->bucket_counts, ctx->overflow_counts,
                              cfg->target_per_bucket,
                              &ctx->tuples_main, &ctx->tuples_overflow);
        ctx->games_played++;
    }

    grpo_shard_close(&w_main);
    if (overflow_open) grpo_shard_close(&w_over);
    return NULL;
}

// ---- Manifest -------------------------------------------------------------

static double wall_secs(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void write_manifest(const GrpoCollectConfig *cfg,
                           const WorkerCtx *ctxs,
                           atomic_ullong (*bc)[GRPO_ROLE_COUNT],
                           atomic_ullong (*oc)[GRPO_ROLE_COUNT],
                           double wall) {
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.txt", cfg->out_dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write manifest %s\n", path); return; }
    fprintf(f, "# cnitro grpo SFT corpus manifest\n");
    fprintf(f, "# generated %ld\n", (long)time(NULL));
    fprintf(f, "version 1\n");
    fprintf(f, "base_seed %u\n", cfg->base_seed);
    fprintf(f, "num_games %d\n", cfg->num_games);
    fprintf(f, "num_threads %d\n", cfg->num_threads);
    fprintf(f, "target_per_bucket %d\n", cfg->target_per_bucket);
    fprintf(f, "wall_secs %.3f\n", wall);
    fprintf(f, "\n# per-thread shards\n");
    for (int i = 0; i < cfg->num_threads; i++) {
        fprintf(f, "shard %d games=%llu tuples_main=%llu tuples_overflow=%llu\n",
                i,
                (unsigned long long)ctxs[i].games_played,
                (unsigned long long)ctxs[i].tuples_main,
                (unsigned long long)ctxs[i].tuples_overflow);
    }
    fprintf(f, "\n# bucket histogram (player_count, role) -> main_count overflow_count\n");
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        for (int r = 0; r < GRPO_ROLE_COUNT; r++) {
            fprintf(f, "bucket pc=%d role=%s main=%llu overflow=%llu\n",
                    pc + GRPO_MIN_PLAYERS,
                    grpo_role_name((GrpoRole)r),
                    (unsigned long long)atomic_load(&bc[pc][r]),
                    (unsigned long long)atomic_load(&oc[pc][r]));
        }
    }
    fclose(f);
}

// ---- Public entry point ---------------------------------------------------

int grpo_collect_run(const GrpoCollectConfig *cfg, GrpoCollectStats *stats) {
    if (cfg->num_threads < 1 || cfg->num_games < 1) return 1;

    atomic_ullong task_idx = 0;
    // 7 × 4 atomics each.
    atomic_ullong bucket_counts[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    atomic_ullong overflow_counts[GRPO_PC_BUCKETS][GRPO_ROLE_COUNT];
    for (int i = 0; i < GRPO_PC_BUCKETS; i++) {
        for (int j = 0; j < GRPO_ROLE_COUNT; j++) {
            atomic_init(&bucket_counts[i][j], 0);
            atomic_init(&overflow_counts[i][j], 0);
        }
    }

    pthread_t *threads = (pthread_t *)calloc((size_t)cfg->num_threads, sizeof(pthread_t));
    WorkerCtx *ctxs = (WorkerCtx *)calloc((size_t)cfg->num_threads, sizeof(WorkerCtx));
    if (!threads || !ctxs) { free(threads); free(ctxs); return 2; }

    double t0 = wall_secs();
    for (int i = 0; i < cfg->num_threads; i++) {
        ctxs[i].worker_id      = i;
        ctxs[i].cfg            = cfg;
        ctxs[i].task_idx       = &task_idx;
        ctxs[i].bucket_counts  = bucket_counts;
        ctxs[i].overflow_counts = overflow_counts;
        pthread_create(&threads[i], NULL, worker_main, &ctxs[i]);
    }
    for (int i = 0; i < cfg->num_threads; i++) pthread_join(threads[i], NULL);
    double wall = wall_secs() - t0;

    write_manifest(cfg, ctxs, bucket_counts, overflow_counts, wall);

    if (stats) {
        memset(stats, 0, sizeof(*stats));
        for (int i = 0; i < cfg->num_threads; i++) {
            stats->total_games    += ctxs[i].games_played;
            stats->total_tuples   += ctxs[i].tuples_main;
            stats->total_overflow += ctxs[i].tuples_overflow;
        }
        for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
            for (int r = 0; r < GRPO_ROLE_COUNT; r++) {
                stats->bucket_counts[pc][r]   = atomic_load(&bucket_counts[pc][r]);
                stats->overflow_counts[pc][r] = atomic_load(&overflow_counts[pc][r]);
            }
        }
        stats->wall_secs = wall;
    }

    free(threads); free(ctxs);
    return 0;
}

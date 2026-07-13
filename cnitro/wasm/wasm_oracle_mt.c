/* =============================================================================
 * Infinite Oracle — Mode B: shared-memory wasm threads, coordination in C
 * (docs/INFINITE_ORACLE_DESIGN.md §8b). Compiled ONLY into oracle-mt.wasm
 * (-DFOOLISH_ORACLE_MT), which restores REAL TLS (no -D_Thread_local=) so every
 * RNG and all cordite scratch is genuinely per-thread again — the proven native
 * OMP model. The control instance (main thread) marshals g_game once via the
 * normal bridge, then N worker threads deliberate in parallel over the shared
 * state, coordinated entirely by C atomics. No per-batch JSON, no postMessage.
 *
 * This build measures raw parallel octogen-choose throughput (the latency
 * question). It does not emit the per-candidate dump (that is Mode A's job); the
 * full per-candidate C accumulator (MT4/MT5) is a follow-up once the throughput
 * win is confirmed.
 * ========================================================================== */

#ifdef FOOLISH_ORACLE_MT
#include <stdint.h>
#include "game.h"
#include "legal.h"

/* kernel + strategy entry points (real TLS in this build) */
void calculate_legal_moves(const Game *g, int bot_idx, LegalMoves *moves);
int  octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
void random_strategy_set_seed(uint32_t s);
void og_reload_flags(void);                 /* FOOLISH_ORACLE_BUILD hook, reused */
Game *wasm_game_ptr_internal(void);         /* the shared resident game */
extern void (*engine_snap_hook)(const Game *g, int tag, int aux);  /* game.c */

/* ---- MT4: the shared job-control block (control writes, threads read) ---- */
typedef struct {
    uint32_t generation;        /* bumped by setup; threads re-arm on change */
    uint32_t stop;              /* 1 = park */
    uint32_t seed_base;
    int32_t  seat;
    uint32_t nthreads;
    uint64_t total_choose;      /* Σ choose calls across all threads */
    uint64_t checksum;          /* Σ (chosen idx + 1) — defeats dead-code elim */
    uint32_t active;            /* # threads currently deliberating */
} JobControl;
static JobControl g_jc;

/* per-thread move list (MT2): threads never touch the shared g_moves. TLS in
 * this build, so each thread gets its own ~large LegalMoves. */
static _Thread_local LegalMoves t_moves;

static inline uint32_t mix3(uint32_t a, uint32_t b, uint32_t c) {
    a += 0x9e3779b9; a ^= b; a *= 0x85ebca6b; a ^= a >> 13;
    a ^= c; a *= 0xc2b2ae35; a ^= a >> 16;
    return a ? a : 1u;
}

void *malloc(unsigned long n);              /* bump allocator (wasm_bots_api.c) */

/* ---- MT8: control-instance exports (main thread only) ------------------- */

/* Reserve one contiguous region from the heap for N thread stacks + TLS blocks
 * BEFORE any transposition table is allocated, so the heap grows ABOVE it and
 * never collides with a live stack (§8b.4/§8b.6). Returns the region base. */
__attribute__((export_name("wasm_mt_reserve")))
double wasm_mt_reserve(uint32_t nthreads, uint32_t stack_bytes, uint32_t tls_bytes) {
    unsigned long per = (unsigned long)stack_bytes + ((tls_bytes + 15ul) & ~15ul);
    void *p = malloc((unsigned long)nthreads * per);
    return (double)(unsigned long)p;
}

/* Force the lazily-first-touch bitboard masks to initialise on the CONTROL
 * thread, and detach the snapshot hook (MT3 items 1-2), before any worker runs.
 * A single octogen choose on the resident game does both: cd_sim_from_game ->
 * ensure_masks(), and we then NULL the hook. */
__attribute__((export_name("wasm_mt_warmup")))
void wasm_mt_warmup(int seat) {
    engine_snap_hook = 0;                   /* MT3 item 2: no SNAP under threads */
    Game *g = wasm_game_ptr_internal();
    og_reload_flags();
    random_strategy_set_seed(1u);
    calculate_legal_moves(g, seat, &t_moves);   /* MT3 item 1: masks + TT on control */
    if (t_moves.n > 0) octogen_strategy_choose(g, seat, &t_moves, 0);
}

__attribute__((export_name("wasm_mt_setup")))
void wasm_mt_setup(int seat, uint32_t seed_base, uint32_t nthreads) {
    g_jc.seat = seat;
    g_jc.seed_base = seed_base;
    g_jc.nthreads = nthreads;
    __atomic_store_n(&g_jc.stop, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jc.total_choose, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jc.checksum, 0ull, __ATOMIC_RELAXED);
    /* publish the job, then release-bump the generation and wake parked threads */
    __atomic_add_fetch(&g_jc.generation, 1u, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify((int32_t *)&g_jc.generation, 0x7fffffff);
}

__attribute__((export_name("wasm_mt_stop")))
void wasm_mt_stop(void) { __atomic_store_n(&g_jc.stop, 1u, __ATOMIC_RELEASE); }

__attribute__((export_name("wasm_mt_total")))
double wasm_mt_total(void) {
    return (double)__atomic_load_n(&g_jc.total_choose, __ATOMIC_RELAXED);
}
__attribute__((export_name("wasm_mt_checksum")))
double wasm_mt_checksum(void) {
    return (double)__atomic_load_n(&g_jc.checksum, __ATOMIC_RELAXED);
}
__attribute__((export_name("wasm_mt_active")))
int wasm_mt_active(void) { return (int)__atomic_load_n(&g_jc.active, __ATOMIC_RELAXED); }

/* ---- MT7: the worker thread loop (never returns) ------------------------ */
__attribute__((export_name("wasm_mt_thread_main")))
void wasm_mt_thread_main(int tid) {
    uint32_t seen_gen = 0u;
    for (;;) {
        uint32_t g = __atomic_load_n(&g_jc.generation, __ATOMIC_ACQUIRE);
        if (g == seen_gen) {
            /* park until setup bumps the generation (worker threads may block) */
            __builtin_wasm_memory_atomic_wait32((int32_t *)&g_jc.generation,
                                                (int32_t)seen_gen, (int64_t)-1);
            continue;
        }
        seen_gen = g;
        og_reload_flags();                  /* re-read OG_* env for this generation */
        const Game *game = wasm_game_ptr_internal();
        int seat = g_jc.seat;
        uint32_t seed_base = g_jc.seed_base;
        __atomic_add_fetch(&g_jc.active, 1u, __ATOMIC_RELAXED);
        for (uint32_t b = 0; !__atomic_load_n(&g_jc.stop, __ATOMIC_ACQUIRE) &&
                 __atomic_load_n(&g_jc.generation, __ATOMIC_ACQUIRE) == g; b++) {
            random_strategy_set_seed(mix3(seed_base, (uint32_t)tid, b));
            calculate_legal_moves(game, seat, &t_moves);
            if (t_moves.n > 0) {
                int idx = octogen_strategy_choose(game, seat, &t_moves, 0);
                __atomic_add_fetch(&g_jc.total_choose, 1ull, __ATOMIC_RELAXED);
                __atomic_add_fetch(&g_jc.checksum, (uint64_t)(idx + 1), __ATOMIC_RELAXED);
            }
        }
        __atomic_sub_fetch(&g_jc.active, 1u, __ATOMIC_RELAXED);
    }
}
#endif  /* FOOLISH_ORACLE_MT */

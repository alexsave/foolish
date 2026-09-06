/* =============================================================================
 * Infinite Oracle - Mode B: shared-memory wasm threads, coordination in C
 * (docs/INFINITE_ORACLE_DESIGN.md §8b). Compiled ONLY into oracle-mt.wasm
 * (-DFOOLISH_ORACLE_MT), which restores REAL TLS (no -D_Thread_local=) so every
 * RNG, the transposition table pointer and all cordite scratch is genuinely
 * per-thread again - the proven native OMP model. The control instance (main
 * thread) marshals g_game once via the normal bridge; N worker threads
 * deliberate in parallel over the shared state and fold their per-candidate
 * rollout scores into g_ogmt with C atomics (the MT5 hook lives in
 * octogen_strategy.c). No per-batch marshalling, no postMessage.
 *
 * What is NOT shared, deliberately: the endgame solver's transposition table.
 * cd_tt is _Thread_local, so each thread grows its own 8 MiB table out of the
 * shared bump heap. That matters more than it looks. A TT entry holds a value in
 * ONE seat's perspective, and a table read by a seat it was not solved for
 * returns the opposite verdict - the bug main fixed in "a solved endgame now
 * carries the seat it was solved for", by storing the canonical value and
 * flipping it on probe. Per-thread tables keep that fix a single-threaded
 * property: no thread ever reads another's entries, so concurrency cannot
 * reintroduce it. Sharing one table would need the canonicalisation to hold
 * under concurrent torn stores, which this build does not attempt.
 * ========================================================================== */

#ifdef FOOLISH_ORACLE_MT
#include <stdint.h>
#include "game.h"
#include "legal.h"
#include "oracle_mt.h"

/* kernel + strategy entry points (real TLS in this build) */
void calculate_legal_moves(const Game *g, int bot_idx, LegalMoves *moves);
int  octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
void random_strategy_set_seed(uint32_t s);
void og_reload_flags(void);                 /* FOOLISH_ORACLE_BUILD hook, reused */
Game *wasm_game_ptr_internal(void);         /* the shared resident game */
extern unsigned char *wasm_io_ptr(void);
/* engine_snap_hook is declared by game.h, which is _Thread_local since the
   kernel's shared globals moved per-thread; re-declaring it here without the
   qualifier is a hard error, and the include already supplies it. */

/* The shared control block the MT5 accumulate hook (octogen_strategy.c) feeds. */
OgMtControl g_ogmt;

/* per-thread move list (MT2): threads never touch the shared g_moves. */
static _Thread_local LegalMoves t_moves;

/* §8b.6: this thread's stack canary word, latched by thread_main. */
static _Thread_local volatile uint32_t *t_canary = 0;
#define OG_MT_CANARY 0xC0DEFACEu

static inline uint32_t mix3(uint32_t a, uint32_t b, uint32_t c) {
    a += 0x9e3779b9; a ^= b; a *= 0x85ebca6b; a ^= a >> 13;
    a ^= c; a *= 0xc2b2ae35; a ^= a >> 16;
    return a ? a : 1u;
}

void *malloc(unsigned long n);              /* bump allocator (wasm_bots_api.c) */

/* ---- MT8: control-instance exports (main thread only) ------------------- */

/* Reserve one contiguous region from the heap for N thread stacks + TLS blocks
 * BEFORE any transposition table is allocated, so the heap grows ABOVE it and
 * never collides with a live stack (§8b.4/§8b.6). Stacks come FIRST so thread
 * k's overflow walks into thread k's own TLS block, never a neighbour's stack.
 * Each stack's lowest word is stamped with a canary. Returns the region base. */
__attribute__((export_name("wasm_mt_reserve")))
double wasm_mt_reserve(uint32_t nthreads, uint32_t stack_bytes, uint32_t tls_bytes) {
    unsigned long per = (unsigned long)stack_bytes + ((tls_bytes + 15ul) & ~15ul);
    unsigned char *p = (unsigned char *)malloc((unsigned long)nthreads * per);
    if (p) {
        for (uint32_t t = 0; t < nthreads; t++)
            *(volatile uint32_t *)(p + (unsigned long)t * stack_bytes) = OG_MT_CANARY;
    }
    return (double)(unsigned long)p;
}

/* Force the lazily-first-touch bitboard masks to initialise on the CONTROL
 * thread, and detach the snapshot hook (MT3 items 1-2), before any worker runs. */
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
    g_ogmt.seat = seat;
    g_ogmt.seed_base = seed_base;
    g_ogmt.nthreads = nthreads;
    __atomic_store_n(&g_ogmt.stop, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ogmt.total_choose, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ogmt.checksum, 0ull, __ATOMIC_RELAXED);
    /* reset the per-candidate accumulator for this generation */
    __atomic_store_n(&g_ogmt.cand_state, 0u, __ATOMIC_RELAXED);
    g_ogmt.n_candidates = 0;
    g_ogmt.chosen = -1;
    __atomic_store_n(&g_ogmt.batches, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ogmt.desc_mismatch, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ogmt.solver_applied, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ogmt.defuse_probe, 0u, __ATOMIC_RELAXED);
    for (int i = 0; i < OG_MT_MAX_CANDS; i++) {
        __atomic_store_n(&g_ogmt.sum_fp[i], 0ull, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ogmt.nsim[i], 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ogmt.forced_loss[i], 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ogmt.verdict[i], 0, __ATOMIC_RELAXED);
    }
    /* publish, then release-bump the generation and wake parked threads */
    __atomic_add_fetch(&g_ogmt.generation, 1u, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify((int32_t *)&g_ogmt.generation, 0x7fffffff);
}

__attribute__((export_name("wasm_mt_stop")))
void wasm_mt_stop(void) { __atomic_store_n(&g_ogmt.stop, 1u, __ATOMIC_RELEASE); }

/* ---- snapshot readers (control instance; relaxed loads, approximate-while-
 * running is fine for the UI, §8b.8) ------------------------------------- */
__attribute__((export_name("wasm_mt_total")))
double wasm_mt_total(void) { return (double)__atomic_load_n(&g_ogmt.total_choose, __ATOMIC_RELAXED); }
__attribute__((export_name("wasm_mt_checksum")))
double wasm_mt_checksum(void) { return (double)__atomic_load_n(&g_ogmt.checksum, __ATOMIC_RELAXED); }
__attribute__((export_name("wasm_mt_active")))
int wasm_mt_active(void) { return (int)__atomic_load_n(&g_ogmt.active, __ATOMIC_RELAXED); }
__attribute__((export_name("wasm_mt_ncand")))
int wasm_mt_ncand(void) { return g_ogmt.n_candidates; }
__attribute__((export_name("wasm_mt_ready")))
int wasm_mt_ready(void) { return __atomic_load_n(&g_ogmt.cand_state, __ATOMIC_ACQUIRE) == 2u; }
__attribute__((export_name("wasm_mt_batches")))
double wasm_mt_batches(void) { return (double)__atomic_load_n(&g_ogmt.batches, __ATOMIC_RELAXED); }
/* Batches DROPPED because this thread's candidate set disagreed with the
 * published descriptor table (see og_mt_desc_matches). Non-zero is not a
 * correctness failure - it is the guard doing its job - but the UI surfaces it
 * because a high rate means the endgame gate is flapping between threads. */
__attribute__((export_name("wasm_mt_mismatch")))
double wasm_mt_mismatch(void) { return (double)__atomic_load_n(&g_ogmt.desc_mismatch, __ATOMIC_RELAXED); }
__attribute__((export_name("wasm_mt_canary")))
int wasm_mt_canary(void) { return (int)__atomic_load_n(&g_ogmt.canary_trips, __ATOMIC_RELAXED); }
__attribute__((export_name("wasm_mt_chosen")))
int wasm_mt_chosen(void) { return __atomic_load_n(&g_ogmt.chosen, __ATOMIC_RELAXED); }
__attribute__((export_name("wasm_mt_sumfp")))
double wasm_mt_sumfp(int i) {
    if (i < 0 || i >= OG_MT_MAX_CANDS) return 0;
    return (double)__atomic_load_n(&g_ogmt.sum_fp[i], __ATOMIC_RELAXED);
}
__attribute__((export_name("wasm_mt_nsim")))
double wasm_mt_nsim(int i) {
    if (i < 0 || i >= OG_MT_MAX_CANDS) return 0;
    return (double)__atomic_load_n(&g_ogmt.nsim[i], __ATOMIC_RELAXED);
}
__attribute__((export_name("wasm_mt_forced")))
int wasm_mt_forced(int i) {
    if (i < 0 || i >= OG_MT_MAX_CANDS) return 0;
    return (int)__atomic_load_n(&g_ogmt.forced_loss[i], __ATOMIC_RELAXED);
}
__attribute__((export_name("wasm_mt_solver")))
int wasm_mt_solver(void) { return (int)__atomic_load_n(&g_ogmt.solver_applied, __ATOMIC_RELAXED); }
__attribute__((export_name("wasm_mt_verdict")))
int wasm_mt_verdict(int i) {
    if (i < 0 || i >= OG_MT_MAX_CANDS) return 2000001;   /* OG_EX_NONE_V */
    return __atomic_load_n(&g_ogmt.verdict[i], __ATOMIC_RELAXED);
}
/* Give up the (expensive) endgame verdict probe: the controller calls this once
 * it sees the solver fired but proved nothing at budget, so later batches are
 * pure-MC-priced (§8b.5 / §5.4). */
__attribute__((export_name("wasm_mt_defuse")))
void wasm_mt_defuse(void) { __atomic_store_n(&g_ogmt.defuse_probe, 1u, __ATOMIC_RELAXED); }

/* Dump the candidate descriptor table into the io buffer for the TS overlay.
 * Per candidate: type, n_cards, n_cards x cardByte, n_targets, n_targets x
 * cardByte (cardByte = (suit<<4)|value). Returns the candidate count, or -1 if
 * the descriptors are not yet published. */
__attribute__((export_name("wasm_mt_candidates")))
int wasm_mt_candidates(void) {
    if (__atomic_load_n(&g_ogmt.cand_state, __ATOMIC_ACQUIRE) != 2u) return -1;
    unsigned char *q = wasm_io_ptr();
    int n = g_ogmt.n_candidates;
    for (int i = 0; i < n && i < OG_MT_MAX_CANDS; i++) {
        const OgMtCand *d = &g_ogmt.cand[i];
        *q++ = d->type;
        *q++ = d->n_cards;
        for (int k = 0; k < d->n_cards; k++) *q++ = d->cards[k];
        *q++ = d->n_targets;
        for (int k = 0; k < d->n_targets; k++) *q++ = d->targets[k];
    }
    return n;
}

/* ---- MT7: the worker thread loop (never returns) ------------------------ */
/* stack_low is this thread's canary address (the LOW end of its stack region);
 * 0 disables the check. The trampoline passes what wasm_mt_reserve stamped. */
__attribute__((export_name("wasm_mt_thread_main")))
void wasm_mt_thread_main(int tid, double stack_low) {
    t_canary = (volatile uint32_t *)(unsigned long)stack_low;
    uint32_t seen_gen = 0u;
    for (;;) {
        uint32_t g = __atomic_load_n(&g_ogmt.generation, __ATOMIC_ACQUIRE);
        if (g == seen_gen) {
            __builtin_wasm_memory_atomic_wait32((int32_t *)&g_ogmt.generation,
                                                (int32_t)seen_gen, (int64_t)-1);
            continue;
        }
        seen_gen = g;
        og_reload_flags();                  /* re-read OG_* env for this generation */
        const Game *game = wasm_game_ptr_internal();
        int seat = g_ogmt.seat;
        uint32_t seed_base = g_ogmt.seed_base;
        __atomic_add_fetch(&g_ogmt.active, 1u, __ATOMIC_RELAXED);
        for (uint32_t b = 0; !__atomic_load_n(&g_ogmt.stop, __ATOMIC_ACQUIRE) &&
                 __atomic_load_n(&g_ogmt.generation, __ATOMIC_ACQUIRE) == g; b++) {
            random_strategy_set_seed(mix3(seed_base, (uint32_t)tid, b));
            calculate_legal_moves(game, seat, &t_moves);
            if (t_moves.n > 0) {
                int idx = octogen_strategy_choose(game, seat, &t_moves, 0);  /* MT5 accumulates */
                __atomic_add_fetch(&g_ogmt.total_choose, 1ull, __ATOMIC_RELAXED);
                __atomic_add_fetch(&g_ogmt.checksum, (uint64_t)(idx + 1), __ATOMIC_RELAXED);
            }
            if (t_canary && *t_canary != OG_MT_CANARY) {
                __atomic_add_fetch(&g_ogmt.canary_trips, 1u, __ATOMIC_RELAXED);
                *t_canary = OG_MT_CANARY;   /* re-arm so the counter counts batches */
            }
        }
        __atomic_sub_fetch(&g_ogmt.active, 1u, __ATOMIC_RELAXED);
    }
}
#endif  /* FOOLISH_ORACLE_MT */

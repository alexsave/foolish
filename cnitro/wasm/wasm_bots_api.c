// WebAssembly bridge for the cnitro BOT STRATEGIES (compiled together with
// the rules kernel into bots.wasm — a separate module from rules.wasm, with
// the arena's compact struct sizes, loaded only by server/offline bot paths).
//
// Adds on top of wasm_api.c:
//   - a growing bump allocator + getenv backed by a small table, because the
//     Monte-Carlo bots allocate one-time scratch (solver stacks, transposition
//     table) and read CD_* tuning knobs;
//   - log import (cordite's belief tracking reads the session log);
//   - wasm_choose_move: enumerate legal moves in-kernel and let a strategy
//     pick — only the CHOSEN move crosses back to JS, never the full list.

#include "game.h"
#include "wire.h"
#include "legal.h"
#include "strategy.h"
#include <string.h>
#include <stdlib.h>

// ---------- allocator -----------------------------------------------------
// Bump-only: the bots allocate a handful of one-time buffers and never free
// in steady state (free is a no-op, matching their usage).

extern unsigned char __heap_base;
static unsigned char *g_brk = 0;

static unsigned long align16(unsigned long v) { return (v + 15ul) & ~15ul; }

#ifdef FOOLISH_ORACLE_MT
// Mode B (docs/INFINITE_ORACLE_DESIGN.md §8b, MT1): under shared-memory threads
// each thread bump-allocates its own transposition table, so g_brk (and the
// memory.grow it drives) must be serialized. A one-byte spinlock; memory.grow
// itself is atomic on shared memory, only the bump pointer needs guarding.
static unsigned char g_brk_lock = 0;
static void brk_lock(void)   { while (__atomic_exchange_n(&g_brk_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void brk_unlock(void) { __atomic_store_n(&g_brk_lock, 0, __ATOMIC_RELEASE); }
#else
static void brk_lock(void)   { }
static void brk_unlock(void) { }
#endif

void *malloc(size_t n) {
    brk_lock();
    if (!g_brk) g_brk = &__heap_base;
    unsigned long need = align16(n);
    unsigned char *cur_end = (unsigned char *)(__builtin_wasm_memory_size(0) * 65536ul);
    while (g_brk + need > cur_end) {
        // +2 pages of slack, not more: the production bots make exactly ONE
        // allocation (the 1MB transposition table) — a fat headroom just
        // inflates every worker's linear memory for nothing, and the edge
        // budget charges buffer SIZE, not touched pages.
        unsigned long grow_pages = (need + 65535ul) / 65536ul + 2ul;
        if (__builtin_wasm_memory_grow(0, grow_pages) == (unsigned long)-1) { brk_unlock(); return 0; }
        cur_end = (unsigned char *)(__builtin_wasm_memory_size(0) * 65536ul);
    }
    void *p = g_brk;
    g_brk += need;
    brk_unlock();
    return p;
}

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void free(void *p) { (void)p; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int atoi(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

// Diagnostics the bots only emit under CD_VERIFY/CD_DIFFTEST-style flags;
// referenced unconditionally, so they must link. No-ops in wasm.
struct FILE;
struct FILE *stderr = 0;
int fprintf(struct FILE *stream, const char *fmt, ...) {
    (void)stream; (void)fmt;
    return 0;
}
int atexit(void (*fn)(void)) { (void)fn; return 0; }

// ---------- getenv --------------------------------------------------------
// Tiny key/value table the TS adapter fills through the IO buffer (e.g.
// CD_W1..CD_W3 world overrides that distinguish cordite_max from cordite).

#define ENV_SLOTS 16
#define ENV_KEY   32
#define ENV_VAL   32
static char g_env_keys[ENV_SLOTS][ENV_KEY];
static char g_env_vals[ENV_SLOTS][ENV_VAL];
static int g_env_n = 0;

char *getenv(const char *name) {
    for (int i = 0; i < g_env_n; i++) {
        const char *a = g_env_keys[i], *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return g_env_vals[i][0] ? g_env_vals[i] : 0;
    }
    return 0;
}

// Key and value are NUL-terminated strings at the start of the IO buffer
// (key first, value right after its NUL). Setting an existing key replaces it.
extern unsigned char *wasm_io_ptr(void);

void wasm_setenv_from_io(void) {
    const char *key = (const char *)wasm_io_ptr();
    unsigned long klen = 0;
    while (key[klen]) klen++;
    const char *val = key + klen + 1;
    if (klen == 0 || klen >= ENV_KEY) return;
    int slot = -1;
    for (int i = 0; i < g_env_n; i++) {
        const char *a = g_env_keys[i], *b = key;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_env_n >= ENV_SLOTS) return;
        slot = g_env_n++;
    }
    unsigned long i = 0;
    for (; key[i] && i < ENV_KEY - 1; i++) g_env_keys[slot][i] = key[i];
    g_env_keys[slot][i] = 0;
    for (i = 0; val[i] && i < ENV_VAL - 1; i++) g_env_vals[slot][i] = val[i];
    g_env_vals[slot][i] = 0;
}

void wasm_clearenv(void) { g_env_n = 0; }

// ---------- log import -----------------------------------------------------
// Same wire layout wasm_api.c exports: u16 count, then per log
// (i8 type, i8 player_idx, i8 defender_index, u8 num_pairs,
//  num_pairs x (u8 primary, u8 target) — 1-byte wire cards).
// Cordite's belief reads these; hidden cards arrive as 0xFE ({-1,-1}).

extern Game *wasm_game_ptr_internal(void);

void wasm_import_logs(void) {
    Game *g = wasm_game_ptr_internal();
    const unsigned char *q = wasm_io_ptr();
    int n = q[0] | (q[1] << 8);
    q += 2;
    if (n > MAX_LOGS) n = MAX_LOGS;
    g->num_logs = 0;
    for (int i = 0; i < n; i++) {
        GameLog *l = &g->logs[g->num_logs++];
        l->log_type = (int8_t)*q++;
        l->player_idx = (int8_t)*q++;
        l->defender_index = (int8_t)*q++;
        int np = *q++;
        l->num_pairs = 0;
        for (int j = 0; j < np; j++) {
            if (l->num_pairs < MAX_LOG_PAIRS) {
                LogPair *p = &l->pairs[l->num_pairs++];
                p->primary = card_from_wire_pair(q[0]);
                p->target  = card_from_wire_pair(q[1]);
            }
            q += 2;
        }
    }
}

// Per-seat application strategy keys. Not part of the rules state (the rules
// module never reads them), but espresso branches on whether an opponent is
// the 'random' bot, so the bot bridge imports them separately: one i8 per
// seat (a STRAT_* id, or -1 for unknown/human/LLM), written AFTER
// wasm_import_state has rebuilt the players.
// Drop the session logs imported for a decision. The TS bridge calls this
// after a choose so the SAME resident kernel state can then run the chosen
// action: the action appends its own logs from zero, exactly like a fresh
// marshal (wasm_import_state also starts from zero), so the post-action log
// export contains only the new entries.
void wasm_clear_logs(void) { wasm_game_ptr_internal()->num_logs = 0; }

// Game identity for per-game bot memory (espresso's discard memory was a
// per-game-id map in TS). The bridge sends a hash of game.id per decision.
// espresso_prod was the only consumer of a per-game key (for its discard
// memory) and is no longer linked into the wasm build (dropped from the shipped
// ladder). The remaining bots keep their per-game state keyed internally
// (e.g. cordite's transposition table), so this bridge hook is now a no-op. The
// TS side still calls it once per decision, so the export stays.
void wasm_set_game_key(unsigned int key) { (void)key; }

void wasm_import_strategy_keys(void) {
    Game *g = wasm_game_ptr_internal();
    const unsigned char *q = wasm_io_ptr();
    for (int i = 0; i < g->num_players; i++)
        g->players[i].strategy_key = (int8_t)q[i];
}

// ---------- strategy dispatch ----------------------------------------------

void random_strategy_set_seed(uint32_t s);

void wasm_set_strategy_seed(unsigned int s) { random_strategy_set_seed(s); }

// cordite caches its CD_* env knobs on first use; one bots.wasm instance
// serves both cordite and cordite_max (different CD_W*/CD_KEEP*), so the TS
// adapter rewrites the env table and forces a re-read before each decision.
void cordite_reload_flags(void);
void wasm_reload_bot_flags(void) { cordite_reload_flags(); }

#ifdef FOOLISH_ORACLE_BUILD
// Infinite-oracle only (docs/INFINITE_ORACLE_DESIGN.md §6.2): the replay
// analyzer rewrites OG_* env between deliberation batches (adaptive world
// budget) and forces octogen to re-read it. Compiled ONLY into oracle.wasm.
void og_reload_flags(void);
void wasm_og_reload_flags(void) { og_reload_flags(); }
#endif

extern LegalMoves *wasm_moves_ptr_internal(void);

// Runs the full bot turn in-kernel: enumerate legal moves for `bot_idx`,
// dispatch strategy `strat` (a STRAT_* id from strategy.h), write the chosen
// move into the IO buffer (u8 type, u8 n_cards, cards, attack_cards — same
// per-move layout as wasm_export_moves) and return the chosen index, or -1
// when there are no legal moves.
int wasm_choose_move(int strat, int bot_idx) {
    Game *g = wasm_game_ptr_internal();
    LegalMoves *lm = wasm_moves_ptr_internal();
    calculate_legal_moves(g, bot_idx, lm);
    if (lm->n == 0) return -1;

    // Shipped ladder only (docs/BOTS_WASM_MEMORY_PLAN.md, "Durak Bot Ordnance
    // Chart"): the wasm module dispatches the deployed difficulty rungs that are
    // ported — random, simple_heuristic, handwritten_prod, firecracker,
    // blackpowder, cordite, octogen (the full seven-rung ladder).
    // espresso_strategy_choose and handwritten_strategy_choose stay LINKED
    // (cordite/octogen/firecracker/blackpowder call them as rollout policies) but
    // are no longer reachable as a top-level bot. champion, ultimate_champion,
    // hacker, fulminate, espresso_prod and semtex left the build entirely. Any
    // dropped/unported strat id falls back to random.
    StrategyFn fn = 0;
    switch (strat) {
        case STRAT_RANDOM:            fn = random_strategy_choose; break;
        case STRAT_SIMPLE_HEURISTIC:  fn = simple_heuristic_strategy_choose; break;
        case STRAT_HANDWRITTEN_PROD:  fn = handwritten_prod_strategy_choose; break;
        case STRAT_FIRECRACKER:       fn = firecracker_strategy_choose; break;
        case STRAT_BLACKPOWDER:       fn = blackpowder_strategy_choose; break;
        case STRAT_CORDITE:           fn = cordite_strategy_choose; break;
        case STRAT_OCTOGEN:           fn = octogen_strategy_choose; break;
        default:                      fn = random_strategy_choose; break;
    }
    int idx = fn(g, bot_idx, lm, 0);
    if (idx < 0 || idx >= lm->n) idx = 0;

    const LegalMove *m = &lm->moves[idx];
    unsigned char *out = wasm_io_ptr();
    *out++ = (unsigned char)m->type;
    *out++ = (unsigned char)m->n_cards;
    for (int i = 0; i < m->n_cards; i++) *out++ = wire_from_card(m->cards[i]);
    for (int i = 0; i < m->n_cards; i++) *out++ = wire_from_card(m->attack_cards[i]);
    return idx;
}

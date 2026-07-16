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
#include "bot_drive.h"
#include "bot_roster.h"
#include "replay_steps.h"
#include "view.h"
#include <string.h>
#include <stdlib.h>

// ---------- allocator -----------------------------------------------------
// Bump-only: the bots allocate a handful of one-time buffers and never free
// in steady state (free is a no-op, matching their usage).

extern unsigned char __heap_base;
static unsigned char *g_brk = 0;

static unsigned long align16(unsigned long v) { return (v + 15ul) & ~15ul; }

void *malloc(size_t n) {
    if (!g_brk) g_brk = &__heap_base;
    unsigned long need = align16(n);
    unsigned char *cur_end = (unsigned char *)(__builtin_wasm_memory_size(0) * 65536ul);
    while (g_brk + need > cur_end) {
        // +2 pages of slack, not more: the production bots make exactly ONE
        // allocation (the 1MB transposition table) — a fat headroom just
        // inflates every worker's linear memory for nothing, and the edge
        // budget charges buffer SIZE, not touched pages.
        unsigned long grow_pages = (need + 65535ul) / 65536ul + 2ul;
        if (__builtin_wasm_memory_grow(0, grow_pages) == (unsigned long)-1) return 0;
        cur_end = (unsigned char *)(__builtin_wasm_memory_size(0) * 65536ul);
    }
    void *p = g_brk;
    g_brk += need;
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

    // ONE roster (docs/C_CORE_CONSOLIDATION.md F1/A1). This was a switch that
    // dispatched the brain at its C defaults and left the KNOBS to whatever env
    // the caller had installed — so a host choosing move-by-move could play a
    // measurably different bot than bot_drive's cycle, which has resolved brain
    // AND knobs through the table since A2. Same lookup here, same answer.
    //
    // An unknown or unlinked strat returns -1 (the caller declines to act)
    // rather than quietly falling back to random: shipping `cordite` that plays
    // like `random` is the exact failure seed.sql warns about, and it is
    // invisible until someone measures ELO.
    int ridx = bot_roster_find_by_strat(strat);
    int idx = bot_roster_choose(ridx, g, bot_idx, lm);
    if (idx < 0 || idx >= lm->n) return -1;

    const LegalMove *m = &lm->moves[idx];
    unsigned char *out = wasm_io_ptr();
    *out++ = (unsigned char)m->type;
    *out++ = (unsigned char)m->n_cards;
    for (int i = 0; i < m->n_cards; i++) *out++ = wire_from_card(m->cards[i]);
    for (int i = 0; i < m->n_cards; i++) *out++ = wire_from_card(m->attack_cards[i]);
    return idx;
}

// ---------- the bot drive cycle (docs/C_CORE_CONSOLIDATION.md F2/F3) --------

// Which bot seats could act right now (bitmask), ignoring `human_mask` seats.
// The server calls this BEFORE driving, to decide an I/O the kernel cannot do:
// hydrating the DRAW-masked session log, but only when a belief bot is about to
// choose (strategyUsesLogs today; the roster's uses_logs flag once the registry
// is gone).
int wasm_bot_eligible_mask(int human_mask) {
    return (int)bot_drive_eligible_mask(wasm_game_ptr_internal(), (uint32_t)human_mask);
}

// class -> milliseconds, from the ONE pacing table. Exported rather than
// mirrored in TS: the whole point of F3 is that the site and the phone cannot
// answer "how long is this worth watching" differently.
int wasm_bot_pacing_ms(int pacing_class, int humans_present) {
    return bot_pacing_ms(pacing_class, humans_present);
}

// Opens ONE action scope for the whole cycle: resets the snapshot buffer and
// captures the pre-action flip the DRAW-privacy log mask reads. See wasm_api.c.
extern void wasm_begin_action_internal(void);

// A cycle's per-seat preferred moves, read from the IO buffer (same per-move
// layout as the output below, minus the pacing byte):
//
//   per pref: u8 seat, u8 type, u8 n_cards,
//             n_cards x u8 wire card, n_cards x u8 wire attack card
//
// Only the server's CAS-retry path sends these — see BotDrivePref. The kernel
// re-checks each against the CURRENT menu, so a stale one costs nothing but a
// re-choose. Decoded up front because the IO buffer is the output slot too.
static BotDrivePref g_pref[MAX_PLAYERS];

static int decode_prefs(const unsigned char *in, int n_pref) {
    if (n_pref < 0 || n_pref > MAX_PLAYERS) return -1;
    for (int p = 0; p < n_pref; p++) {
        g_pref[p].seat = (int8_t)*in++;
        LegalMove *m = &g_pref[p].move;
        m->type = (int8_t)*in++;
        int n = (int)*in++;
        if (n < 0 || n > MAX_MOVE_CARDS) return -1;
        m->n_cards = (int8_t)n;
        for (int c = 0; c < n; c++) m->cards[c] = card_from_wire_state(*in++);
        for (int c = 0; c < n; c++) m->attack_cards[c] = card_from_wire_state(*in++);
    }
    return n_pref;
}

// Run one bot cycle against the resident game. Applies 0..n actions, bundling
// silent ones, and lays the result out in the IO buffer:
//
//   u8 stop        BOT_STOP_*
//   i8 ended       loser seat, or -1
//   u8 n           actions applied
//   per action:
//     u8 seat, u8 pacing_class, u8 type, u8 n_cards,
//     n_cards x u8 wire card, n_cards x u8 wire attack card
//
// `n_pref` preferred moves are read from the IO buffer first (see above).
// Returns n, or -1 on error. The caller exports state/logs/events with the
// existing exports afterwards — the products of the WHOLE cycle, which is
// exactly what the server commits.
// Where the cycle's own log records begin. Non-zero whenever a belief bot was
// eligible: its session log is resident beneath them (wasm_import_logs), and
// the commit must see only what the cycle wrote — see
// wasm_export_logs_masked_from / wasm_events_serialize_from, which the caller
// passes this to. Mirrors ios_api.c's g_last_event_log_start.
static int g_drive_log_start;

int wasm_bot_drive_log_start(void) { return g_drive_log_start; }

// Re-seed from the CURRENT board at each phase, exactly where the
// one-move-per-call path does: the strategy LCG as a decision starts
// (wasmChooseMove) and the draw LCG as the move is applied (packedActionCore).
// This is the whole per-decision seeding policy, and it lives here rather than
// in the TS bridge: the caller only ever hands the kernel the secret it cannot
// derive (wasm_set_rng_base from the deal seed), and the kernel decides what
// every decision draws.
//
// Both read the resident g_game — the game bot_drive is mutating — so each
// decision seeds off the state in front of it.
extern void wasm_set_strategy_seed_deterministic(void);
extern void wasm_seed_rng_deterministic(void);

// ---------- belief probe (observability) ----------------------------------
//
// "Did the bot actually SEE the session log?" — answered by the kernel rather
// than inferred from outside it.
//
// The loop hands the belief bots the persisted session log as packed bytes
// (importLogsPacked). A host-side spy can only prove the bytes were HANDED
// OVER; it cannot prove the importer spliced them into the Game the strategy
// then read. That gap is exactly where the octogen-blind and cordite
// stale-belief regressions lived. Since the choose step moved in-kernel
// (docs/C_CORE_CONSOLIDATION.md F2/A2) there is no TS seam left to spy on
// anyway, so the observation belongs where the read happens.
//
// Records, per SEARCH, the log the strategy was about to read. A reused
// preferred move never fires the CHOOSE phase (no search, no belief read), so
// it correctly records nothing.
//
// Behavior-neutral and OFF until a harness calls reset(): production drives
// never pay the log pass.
#define BELIEF_PROBE_CAP 64

typedef struct {
    uint8_t  seat;
    uint16_t n_logs;
    uint64_t cards;   // bit (suit*16 + value) per real card visible in the log
} BeliefProbe;

static BeliefProbe g_probe[BELIEF_PROBE_CAP];
static int g_n_probe = 0;
static int g_probe_on = 0;

static void probe_capture(const Game *g, int seat) {
    if (!g_probe_on || g_n_probe >= BELIEF_PROBE_CAP) return;
    BeliefProbe *p = &g_probe[g_n_probe++];
    p->seat   = (uint8_t)seat;
    p->n_logs = (uint16_t)g->num_logs;
    p->cards  = 0;
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        for (int k = 0; k < l->num_pairs; k++) {
            const Card cs[2] = { l->pairs[k].primary, l->pairs[k].target };
            for (int c = 0; c < 2; c++) {
                // Card backs (WIRE_CARD_HIDDEN -> suit/value < 0) are not cards.
                if (cs[c].suit >= 0 && cs[c].value > 0)
                    p->cards |= 1ull << ((unsigned)cs[c].suit * 16u + (unsigned)cs[c].value);
            }
        }
    }
}

// Clear + arm. Records accumulate across drives until the next reset, so a
// harness can read the decisions of several cycles in order.
void wasm_belief_probe_reset(void) { g_n_probe = 0; g_probe_on = 1; }

// Dump the records into the IO buffer; returns the count. 11 bytes each:
// u8 seat, u16 n_logs (LE), u64 card mask (LE).
int wasm_belief_probe_dump(void) {
    unsigned char *out = wasm_io_ptr();
    int w = 0;
    for (int i = 0; i < g_n_probe; i++) {
        out[w++] = g_probe[i].seat;
        out[w++] = (unsigned char)(g_probe[i].n_logs & 0xFF);
        out[w++] = (unsigned char)(g_probe[i].n_logs >> 8);
        for (int b = 0; b < 8; b++) out[w++] = (unsigned char)(g_probe[i].cards >> (8 * b));
    }
    return g_n_probe;
}

static void drive_seed_hook(const Game *g, int seat, int phase) {
    if (phase == BOT_DRIVE_PHASE_CHOOSE) {
        probe_capture(g, seat);
        wasm_set_strategy_seed_deterministic();
    } else {
        wasm_seed_rng_deterministic();
    }
}

int wasm_bot_drive(int human_mask, int max_actions, int n_pref) {
    static BotDriveOut drv;
    Game *g = wasm_game_ptr_internal();
    if (decode_prefs(wasm_io_ptr(), n_pref) < 0) return -1;

    g_drive_log_start = g->num_logs;
    wasm_begin_action_internal();
    // Scoped to the cycle: a *_strategy_choose called on its own afterwards
    // (wasm_choose_move) seeds itself through the TS bridge, as it always has.
    bot_drive_pre_action_hook = drive_seed_hook;
    int n = bot_drive(g, (uint32_t)human_mask, max_actions,
                      n_pref > 0 ? g_pref : 0, n_pref, &drv);
    bot_drive_pre_action_hook = 0;
    if (n < 0) return -1;

    unsigned char *out = wasm_io_ptr();
    *out++ = (unsigned char)drv.stop;
    *out++ = (unsigned char)(signed char)drv.ended;
    *out++ = (unsigned char)drv.n;
    for (int i = 0; i < drv.n; i++) {
        const BotDriveAction *a = &drv.actions[i];
        *out++ = (unsigned char)a->seat;
        *out++ = a->pacing_class;
        *out++ = (unsigned char)a->move.type;
        *out++ = (unsigned char)a->move.n_cards;
        for (int c = 0; c < a->move.n_cards; c++) *out++ = wire_from_card(a->move.cards[c]);
        for (int c = 0; c < a->move.n_cards; c++) *out++ = wire_from_card(a->move.attack_cards[c]);
    }
    return drv.n;
}

// ---------- replay steps (docs/C_CORE_CONSOLIDATION.md F4.2 / A5) -----------
// A v6 code, replayed through the REAL engine, serialized as the SAME packed
// evwire frames live play broadcasts — so a replay screen decodes and renders
// what it already decodes and renders, with no replay-side projection.
//
// Lives in bots.wasm, not rules.wasm, and not by preference: replay_steps.c's
// decoded-action buffer alone is ~272 KB, while rules.wasm's whole linear
// memory is PINNED at 196,608 B (--initial-memory == --max-memory). It is 1.4x
// the module before anything else. Per the standing steer, the fix is one big
// module everywhere rather than shrinking the replay to fit the small one
// (docs/C_CORE_CONSOLIDATION.md A10 / RULES_GUARDS_WASM_MEMORY_PLAN.md).
//
// The code goes in the REPLAY io buffer (it is a replay input, like every other
// wasm_replay_* call); the frames come back in the MAIN io buffer, so the two
// never alias.
extern unsigned char *wasm_replay_io_ptr(void);
extern int wasm_io_cap(void);

static int g_rs_n_frames, g_rs_next_step;

int wasm_replay_events(int viewer, int from, int code_len) {
    g_rs_n_frames = 0;
    g_rs_next_step = from;
    return replay_steps_frames_v6(wasm_replay_io_ptr(), code_len,
                                  viewer < 0 ? VIEW_SPECTATOR : viewer, from, 0,
                                  wasm_io_ptr(), wasm_io_cap(),
                                  &g_rs_n_frames, &g_rs_next_step);
}
int wasm_replay_events_n(void)    { return g_rs_n_frames; }
int wasm_replay_events_next(void) { return g_rs_next_step; }

// Steps the code replays to (the deal + one per action). Sizes the scrubber.
int wasm_replay_step_count(int code_len) {
    return replay_steps_count_v6(wasm_replay_io_ptr(), code_len, 0);
}

// What each step is: 4 bytes per step into the MAIN io buffer (the code is in
// the replay buffer, as for every wasm_replay_* call, so the two never alias).
// A whole game's index is steps*4 bytes — a couple of KB — so unlike the frames
// this needs no chunking.
int wasm_replay_step_index(int code_len) {
    return replay_steps_index_v6(wasm_replay_io_ptr(), code_len, 0,
                                 wasm_io_ptr(), wasm_io_cap());
}

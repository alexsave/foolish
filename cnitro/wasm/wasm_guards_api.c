// WebAssembly bridge for the CLIENT-GUARDS kernel: game.c only — no legal.c
// move enumeration, no replay.c codec. It exports exactly what the browser
// needs to (a) gate UI actions (validate-only, no mutation) and (b) apply an
// optimistic move (mutate + read back the predicted state, logs and the
// animation snapshots), plus the three pure projections the client used to
// reimplement in TS (can_cover / next-player / game_done).
//
// The wire layout is byte-identical to wasm/wasm_api.c (the rules module) so
// the SAME marshal on the TS side feeds either module; e2e asserts the two
// stay in lockstep. Kept as its own slim bridge (not #include of wasm_api.c,
// which pulls the legal-move + replay exports) so the client ships the
// smallest possible module.
//
// Freestanding: no libc. memcpy/memset are provided here (clang lowers struct
// copies to them on wasm32).

#include "game.h"
#include "wire.h"
#include "view.h"
#include "awire.h"

// ---------- minimal libc ------------------------------------------------
void *memcpy(void *dst, const void *src, size_t n) { __builtin_memcpy(dst, src, n); return dst; }
void *memset(void *dst, int c, size_t n) { __builtin_memset(dst, c, n); return dst; }

// ---------- shared buffers ----------------------------------------------
// g_io holds the game state the TS bridge marshals in (get_state reads it,
// clamping every count to Game capacity). This is a VALIDATE-ONLY build: no
// state/log/snapshot export is in the linker export list, and at MAX_LOGS=1 even
// a revived log export is a few bytes — so the widest live write is the state
// import, <1.1KB. IO_CAP stays at a conservative 8.5KB (it's a fixed static
// buffer with no per-gate cost, unlike the Game clone); it could safely shrink
// to ~2KB now that the old 8,450B log-stream rationale (MAX_LOGS x MAX_LOG_PAIRS
// at 64/64) no longer applies.
#define IO_CAP 8704
// The shipped guards module exports NO snapshot readers (see the Makefile's
// WASM_GUARDS_EXPORTS trim), so the ring is write-only — the build passes
// -DMAX_SNAPS=1 to shrink the dead slots from 48x1,160B to one. Reviving
// snapshots = re-add the exports AND raise this back (16 covers the
// measured worst of 11 fires per action; see tests/l1_measure.c).
#ifndef MAX_SNAPS
#define MAX_SNAPS 48
#endif
#define MAX_IN_CARDS 128

static unsigned char g_io[IO_CAP];
static Game g_game;

// Snapshots store the Game prefix up to num_logs (animation game_states are
// log-stripped downstream) — see wasm_api.c.
#define GAME_PREFIX_SIZE (__builtin_offsetof(Game, num_logs))
typedef struct { _Alignas(8) unsigned char bytes[GAME_PREFIX_SIZE]; } SnapSlot;
static SnapSlot g_snaps[MAX_SNAPS];
static int g_snap_tags[MAX_SNAPS];
static int g_snap_aux[MAX_SNAPS];
static int g_n_snaps;

static unsigned char g_in_raw_a[MAX_IN_CARDS];
static unsigned char g_in_raw_b[MAX_IN_CARDS];
static Card g_in_a[MAX_IN_CARDS];
static Card g_in_b[MAX_IN_CARDS];

static void decode_in_cards(const unsigned char *raw, Card *out, int n) {
    if (n > MAX_IN_CARDS) n = MAX_IN_CARDS;
    for (int i = 0; i < n; i++) out[i] = card_from_wire_state(raw[i]);
}

unsigned char *wasm_io_ptr(void) { return g_io; }
int wasm_io_cap(void) { return IO_CAP; }
unsigned char *wasm_cards_a_ptr(void) { return g_in_raw_a; }
unsigned char *wasm_cards_b_ptr(void) { return g_in_raw_b; }

// ---------- snapshot hook -------------------------------------------------
static void snap_cb(const Game *g, int tag, int aux) {
    if (g_n_snaps >= MAX_SNAPS) return;
    memcpy(g_snaps[g_n_snaps].bytes, g, GAME_PREFIX_SIZE);
    g_snap_tags[g_n_snaps] = tag;
    g_snap_aux[g_n_snaps] = aux;
    g_n_snaps++;
}

void wasm_init(void) { engine_snap_hook = snap_cb; }
void wasm_set_seed(unsigned int s) { game_set_seed(s); }
int wasm_reject_reason(void) { return engine_last_reject; }

// ---------- state (de)serialization ---------------------------------------
// Byte layout MUST match wasm/wasm_api.c and the TS marshalGame/readState in
// engine.ts — guaranteed by construction now: both bridges call the single
// implementation in src/view.c. See wasm_api.c for the field-by-field doc.

static int put_state(const Game *g, unsigned char *p) {
    return state_put(g, VIEW_UNMASKED, p);
}

static void get_state(Game *g, const unsigned char *p) {
    state_get(g, p, 0);
}

void wasm_import_state(void) { get_state(&g_game, g_io); }
int  wasm_export_state(void) { return put_state(&g_game, g_io); }

// Import a server-produced MASKED view blob ([VIEW_FORMAT_VERSION | viewer |
// masked put_state], docs/PACKED_WIRE_CUTOVER.md) as the resident game —
// hidden cards decode to the same {0,1} placeholder the JS marshal always
// wrote for redacted cards, so gates behave identically either way. Returns
// 1 on success, 0 on an unknown version byte (caller must treat the blob as
// unreadable, never as an empty game).
int wasm_import_view(int len) {
    if (len < 2) return 0;
    if (g_io[0] != VIEW_FORMAT_VERSION) return 0;
    state_get(&g_game, g_io + 2, 1);
    return 1;
}

// ---------- logs (for optimistic-apply animation events) --------------------
int wasm_export_logs(void) {
    unsigned char *q = g_io;
    *q++ = (unsigned char)(g_game.num_logs & 0xff);
    *q++ = (unsigned char)((g_game.num_logs >> 8) & 0xff);
    for (int i = 0; i < g_game.num_logs; i++) {
        const GameLog *l = &g_game.logs[i];
        *q++ = (unsigned char)l->log_type;
        *q++ = (unsigned char)l->player_idx;
        *q++ = (unsigned char)l->defender_index;
        *q++ = (unsigned char)l->num_pairs;
        for (int j = 0; j < l->num_pairs; j++) {
            const LogPair *pr = &l->pairs[j];
            *q++ = wire_from_card(pr->primary);
            *q++ = wire_from_card(pr->target);
        }
    }
    return (int)(q - g_io);
}

// ---------- snapshots -------------------------------------------------------
int wasm_snap_count(void) { return g_n_snaps; }
int wasm_snap_tag(int i) { return g_snap_tags[i]; }
int wasm_snap_aux(int i) { return g_snap_aux[i]; }
int wasm_export_snapshot(int i) { return put_state((const Game *)(const void *)g_snaps[i].bytes, g_io); }

// ---------- validate-only (UI gates) ----------------------------------------
// Run the action against a CLONE so the resident g_game is never mutated — the
// browser marshals the authoritative game once per server update, then fires
// many gate calls (one per candidate selection) that all reuse it. Returns 0
// when the move is legal, else the ENGINE_REJECT_* code.

static Card g_clone_hidden; // unused placeholder to keep the struct copy honest
// The clone lives in BSS, not on the C stack: sizeof(Game) is ~9.6KB at the
// guards caps, and keeping it off the stack is what lets this module link
// with a 16KB shadow stack (the whole module then fits ONE wasm page — the
// L1-cache budget, docs/WASM_L1_BUDGET.md). Single-threaded by design like
// every other static here, and gates never nest.
static Game g_validate_tmp;
static int validate_run(int kind, int player_idx, int n) {
    Game *tmp_p = &g_validate_tmp;
    game_clone(tmp_p, &g_game);
    void (*saved)(const Game *, int, int) = engine_snap_hook;
    engine_snap_hook = 0; // a gate must not perturb the snapshot buffer
    engine_last_reject = ENGINE_REJECT_NONE;
    int ok = 0;
    switch (kind) {
        case 0: ok = handle_attack(tmp_p, player_idx, g_in_a, n); break;
        case 1: ok = handle_cover(tmp_p, player_idx, g_in_a, g_in_b, n); break;
        case 2: ok = handle_pass(tmp_p, player_idx, g_in_a, n); break;
        case 3: ok = handle_pickup(tmp_p, player_idx); break;
        case 4: ok = handle_good(tmp_p, player_idx); break;
        default: break;
    }
    engine_snap_hook = saved;
    (void)g_clone_hidden;
    return ok ? 0 : engine_last_reject;
}

int wasm_validate_attack(int player_idx, int n_cards) {
    decode_in_cards(g_in_raw_a, g_in_a, n_cards);
    return validate_run(0, player_idx, n_cards);
}
int wasm_validate_cover(int player_idx, int n) {
    decode_in_cards(g_in_raw_a, g_in_a, n);
    decode_in_cards(g_in_raw_b, g_in_b, n);
    return validate_run(1, player_idx, n);
}
int wasm_validate_pass(int player_idx, int n_cards) {
    decode_in_cards(g_in_raw_a, g_in_a, n_cards);
    return validate_run(2, player_idx, n_cards);
}
int wasm_validate_pickup(int player_idx) { return validate_run(3, player_idx, 0); }
int wasm_validate_good(int player_idx)   { return validate_run(4, player_idx, 0); }

// ---------- action wire (the packed request body) ----------------------------
// The browser builds ONE awire buffer per move (src/awire.h) and uses it for
// the local gate, the optimistic apply, AND the POST body — the bytes the
// server kernel applies are bit-identical to the bytes validated here.
// AWIRE_* kinds deliberately match validate_run's kind codes.

// Returns 0 legal, ENGINE_REJECT_* code, or -1 malformed wire.
int wasm_validate_action(int player_idx, int wire_len) {
    AwireAction a;
    if (wire_len < 0 || wire_len > MAX_IN_CARDS) return -1;
    if (!awire_decode(g_in_raw_a, wire_len, &a)) return -1;
    memcpy(g_in_a, a.cards, sizeof(Card) * (size_t)a.n);
    memcpy(g_in_b, a.attacks, sizeof(Card) * (size_t)a.n);
    return validate_run(a.kind, player_idx, a.n);
}

// The optimistic apply from the same wire lives after begin_action below
// (wasm_apply_action).

// ---------- apply (optimistic prediction) -----------------------------------
// Mutate the resident game; the caller then reads wasm_export_state (predicted
// state), wasm_export_logs and the snapshots (animation events). Returns 1 on
// success, 0 on rejection (state untouched on a clean early reject).

static void begin_action(void) { g_n_snaps = 0; }

int wasm_attack(int player_idx, int n_cards) {
    begin_action();
    decode_in_cards(g_in_raw_a, g_in_a, n_cards);
    return handle_attack(&g_game, player_idx, g_in_a, n_cards) ? 1 : 0;
}
int wasm_cover(int player_idx, int n) {
    begin_action();
    decode_in_cards(g_in_raw_a, g_in_a, n);
    decode_in_cards(g_in_raw_b, g_in_b, n);
    return handle_cover(&g_game, player_idx, g_in_a, g_in_b, n) ? 1 : 0;
}
int wasm_pass(int player_idx, int n_cards) {
    begin_action();
    decode_in_cards(g_in_raw_a, g_in_a, n_cards);
    return handle_pass(&g_game, player_idx, g_in_a, n_cards) ? 1 : 0;
}
int wasm_pickup(int player_idx) { begin_action(); return handle_pickup(&g_game, player_idx) ? 1 : 0; }
int wasm_good(int player_idx)   { begin_action(); return handle_good(&g_game, player_idx) ? 1 : 0; }

// Optimistic apply from an awire buffer (input buffer A) — the same bytes
// wasm_validate_action gated and the POST body carries. Returns 1 applied,
// 0 rejected, -1 malformed (resident state untouched on a clean early
// reject).
int wasm_apply_action(int player_idx, int wire_len) {
    AwireAction a;
    if (wire_len < 0 || wire_len > MAX_IN_CARDS) return -1;
    if (!awire_decode(g_in_raw_a, wire_len, &a)) return -1;
    begin_action();
    switch (a.kind) {
        case AWIRE_ATTACK: return handle_attack(&g_game, player_idx, a.cards, a.n) ? 1 : 0;
        case AWIRE_COVER:  return handle_cover(&g_game, player_idx, a.cards, a.attacks, a.n) ? 1 : 0;
        case AWIRE_PASS:   return handle_pass(&g_game, player_idx, a.cards, a.n) ? 1 : 0;
        case AWIRE_PICKUP: return handle_pickup(&g_game, player_idx) ? 1 : 0;
        case AWIRE_GOOD:   return handle_good(&g_game, player_idx) ? 1 : 0;
        default:           return -1;
    }
}

// ---------- pure projections (the ex-TS duplicates) -------------------------
int wasm_game_done(void)   { return game_done(&g_game); }
int wasm_next_player(int cur) { return get_next_player_index(&g_game, cur); }
int wasm_can_cover(int as, int av, int ds, int dv, int power_suit) {
    Card a = { (int8_t)as, (int8_t)av }, d = { (int8_t)ds, (int8_t)dv };
    return can_cover(a, d, power_suit) ? 1 : 0;
}

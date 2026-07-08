// WebAssembly bridge for the cnitro rules kernel (game.c + legal.c).
//
// The TS side (supabase/functions/_shared/wasm/engine.ts) marshals the Game
// through ONE compact byte layout in a shared IO buffer — deliberately
// independent of the C struct's padding, so TS never touches struct offsets.
// Every state field crosses as explicit little-endian bytes.
//
// Snapshots: game.c fires engine_snap_hook at exactly the points where the
// production TS handlers captured an intermediate game state for an
// animation event; we copy the whole Game into a slot per hook. The TS
// bridge reads them back to synthesize the identical AnimationEvent stream.
//
// Freestanding: no libc. memcpy/memset are provided here (clang lowers
// struct copies to them on wasm32).

#include "game.h"
#include "wire.h"
#include "legal.h"
#include "replay.h"
#include "view.h"
#include "awire.h"
#include "evwire.h"

// ---------- minimal libc ------------------------------------------------

// The build enables -mbulk-memory, so these __builtin calls (and every
// clang-lowered struct copy across the module) compile to the single wasm
// memory.copy / memory.fill instruction — native memmove in the runtime.
// These out-of-line definitions only back the calls clang chooses not to
// lower inline.
void *memcpy(void *dst, const void *src, size_t n) {
    __builtin_memcpy(dst, src, n);
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    __builtin_memset(dst, c, n);
    return dst;
}

// ---------- shared buffers ----------------------------------------------

// Sized by the widest export, which is the LOG export worst case:
// 2 + MAX_LOGS x (4 + MAX_LOG_PAIRS x 2) = 67,586B at the production
// 512/64 — written unchecked by wasm_export_logs, so IO_CAP must clear it.
// 72KB leaves ~6KB of headroom over that; everything else is far smaller
// (state export <1KB, env strings, the chosen move). The legal-move export
// is CHUNKED (wasm_export_moves) and clamps its chunk to the buffer, so it
// no longer sizes IO_CAP — the TS side derives its chunk from wasm_io_cap.
#define IO_CAP (72 * 1024)
#define MAX_SNAPS 48
#define MAX_IN_CARDS 128

static unsigned char g_io[IO_CAP];
static Game g_game;

// Snapshots never carry logs (animation game_states are log-stripped
// downstream), so each slot stores only the Game prefix up to num_logs —
// ~2.5 KB instead of the full log-laden struct.
#define GAME_PREFIX_SIZE (__builtin_offsetof(Game, num_logs))
typedef struct { _Alignas(8) unsigned char bytes[GAME_PREFIX_SIZE]; } SnapSlot;
static SnapSlot g_snaps[MAX_SNAPS];
static int g_snap_tags[MAX_SNAPS];
static int g_snap_aux[MAX_SNAPS];
static int g_n_snaps;
static LegalMoves g_moves;
// TS writes 1-byte wire cards into the raw buffers; decode_in_cards
// converts (and clamps) them to the in-memory Card.
static unsigned char g_in_raw_a[MAX_IN_CARDS];
static unsigned char g_in_raw_b[MAX_IN_CARDS];
static Card g_in_a[MAX_IN_CARDS];   // action cards (attack/pass/cover covers)
static Card g_in_b[MAX_IN_CARDS];   // cover: the attack cards being covered

static void decode_in_cards(const unsigned char *raw, Card *out, int n) {
    if (n > MAX_IN_CARDS) n = MAX_IN_CARDS;
    for (int i = 0; i < n; i++) out[i] = card_from_wire_state(raw[i]);
}

unsigned char *wasm_io_ptr(void) { return g_io; }
int wasm_io_cap(void) { return IO_CAP; }

// For sibling bridge units (wasm_bots_api.c) that operate on the same
// working game and scratch move list (LegalMoves is ~330KB at the wasm
// build's MAX_LEGAL_MOVES=4096 with 1-byte cards — still not worth a
// second copy).
Game *wasm_game_ptr_internal(void) { return &g_game; }
LegalMoves *wasm_moves_ptr_internal(void) { return &g_moves; }

// Card input buffers: TS writes 1-byte wire cards.
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

// Production configuration: install the snapshot hook. (The deck-size rule
// is hardcoded in card.h — one rule for every deployment.)
void wasm_init(void) {
    engine_snap_hook = snap_cb;
}

void wasm_set_seed(unsigned int s) { game_set_seed(s); }

// Wide, reproducible deal seed: the caller writes 32 bytes (two 128-bit lanes)
// to the front of the io buffer, then calls this before wasm_start_game(). The
// deal then reaches the whole 52!/36! space and replays exactly from the same
// bytes. A following wasm_set_seed() reverts to the legacy 32-bit LCG (so the
// per-move reseed the engine already does keeps mid-game behavior unchanged).
void wasm_set_deal_seed_bytes(void) { game_set_deal_seed_bytes(g_io, 32); }

int wasm_reject_reason(void) { return engine_last_reject; }

// ---------- state (de)serialization ---------------------------------------
//
// Layout (little-endian, byte-packed):
//   u8  status            u8  num_players     i8 power_suit
//   i8  first_attacker    i8  defender
//   u16 discard_pile_length
//   u8  has_flipped       u8 flipped wire-card
//   u32 good_players_mask u8 has_good_timestamp
//   u16 deck_count,   deck_count x u8 wire-card
//   u8  num_battles,  num_battles x (u8 attack, u8 defense; 0xFF = uncovered)
//   num_players x (u8 status, u8 awaiting, u8 hand_count, hand x u8 wire-card)
//   u8  num_eliminated, num_eliminated x i8
//
// The implementation is single-sourced in src/view.c (state_put/state_get),
// shared with the guards bridge and with the per-viewer MASKED serialization
// (deck + other hands as WIRE_CARD_HIDDEN) the packed wire pipeline uses.

static int put_state(const Game *g, unsigned char *p) {
    return state_put(g, VIEW_UNMASKED, p);
}

static void get_state(Game *g, const unsigned char *p) {
    state_get(g, p, 0);
}

// TS -> C: parse the IO buffer into the working game. The ephemeral IO format
// carries no deterministic_deck flag (only the durable blob does), so reset it:
// a fresh deal has start_game set it, and a legacy game draws at random. This
// also stops a reused engine instance inheriting a prior game's flag.
void wasm_import_state(void) { get_state(&g_game, g_io); g_game.deterministic_deck = false; }

// C -> TS: serialize the working game into the IO buffer; returns length.
int wasm_export_state(void) { return put_state(&g_game, g_io); }

// ---------- durable state codec (versioned) -------------------------------
//
// put_state/get_state above are the TRANSIENT request-scoped IO format: they
// never outlive one edge-function call, so they carry no version. This pair is
// the ONLY state format written to durable storage (games.state bytea). It is
// put_state's exact byte layout with a leading 1-byte format version, so a
// future kernel-layout change becomes an explicit decode branch here instead
// of silently misreading every persisted game — the same discipline the
// replay codec (replay.h v2..v5) already applies to its persisted integers.
//
// It carries the VOLATILE game state only (positions, deck, battles, per-seat
// hands/status, good-mask, elimination). Seat identity (player_id/name/
// strategy_key/is_ai) is stable across a game and lives in a separate roster
// column, reattached TS-side — exactly the split parseState/stateToGame
// already assume (KernelState + template).
// Layout: [version][deterministic_deck flag][put_state...]. The flag byte
// (added with the seed-dealt deck; see the Game field) is what bumped this from
// the old v1 [version][put_state...]. There is no v1 read path — a data
// migration rewrites every stored v1 blob to v2 (flag 0), so no v1 blob ever
// reaches this kernel; anything that isn't v2 is treated as unreadable.
#define STATE_FORMAT_VERSION 2

// Serialize the working game into g_io as a versioned durable blob; returns
// the byte length (>=2).
int wasm_state_serialize(void) {
    g_io[0] = (unsigned char)STATE_FORMAT_VERSION;
    g_io[1] = (unsigned char)(g_game.deterministic_deck ? 1 : 0);
    return 2 + put_state(&g_game, g_io + 2);
}

// Load a durable blob (already written into g_io) back into the working game.
// Returns 1 on success, 0 if the leading version byte is one this kernel does
// not understand (caller must treat as unreadable, never as an empty game).
int wasm_state_deserialize(int len) {
    if (len < 2) return 0;
    if (g_io[0] != STATE_FORMAT_VERSION) return 0;
    g_game.deterministic_deck = g_io[1] != 0;
    get_state(&g_game, g_io + 2);
    return 1;
}

// The version this kernel writes — lets the TS bridge assert the embed it
// loaded matches the format it expects without hardcoding the number twice.
int wasm_state_format_version(void) { return STATE_FORMAT_VERSION; }

// ---------- logs -----------------------------------------------------------
// u16 num_logs, then per log: i8 type, i8 player_idx, i8 defender_index,
// u8 num_pairs, num_pairs x (u8 primary, u8 target) — wire cards, target
// 0xFF when the pair has none, 0xFE for the hidden card

// Pre-action flip state, captured by begin_action for the DRAW-privacy rule.
static Card g_pre_flip;
static int g_pre_has_flip;

static int export_logs(int mask_draws) {
    // The DRAW-privacy rule (the TS appendLogs convention, now kernel-side):
    // drawn-card identities are hidden EXCEPT the flipped trump, whose draw
    // is public. "The flip was drawn during this action" is the pre-action
    // has_flipped (captured by begin_action) going false.
    const int flip_drawn = g_pre_has_flip && !g_game.has_flipped;
    unsigned char *q = g_io;
    *q++ = (unsigned char)(g_game.num_logs & 0xff);
    *q++ = (unsigned char)((g_game.num_logs >> 8) & 0xff);
    for (int i = 0; i < g_game.num_logs; i++) {
        const GameLog *l = &g_game.logs[i];
        const int hide = mask_draws && l->log_type == LOG_DRAW;
        *q++ = (unsigned char)l->log_type;
        *q++ = (unsigned char)l->player_idx;
        *q++ = (unsigned char)l->defender_index;
        *q++ = (unsigned char)l->num_pairs;
        for (int j = 0; j < l->num_pairs; j++) {
            const LogPair *pr = &l->pairs[j];
            if (hide && !(flip_drawn && card_eq(pr->primary, g_pre_flip))) {
                *q++ = (unsigned char)WIRE_CARD_HIDDEN;
            } else {
                *q++ = wire_from_card(pr->primary);
            }
            *q++ = wire_from_card(pr->target);
        }
    }
    return (int)(q - g_io);
}

int wasm_export_logs(void) { return export_logs(0); }

// The durable/session variant: what leaves the kernel for storage and (via
// the packed session log) other players' belief imports — draw identities
// masked per the rule above. See docs/PACKED_WIRE_CUTOVER.md.
int wasm_export_logs_masked(void) { return export_logs(1); }

// ---------- snapshots -------------------------------------------------------

int wasm_snap_count(void) { return g_n_snaps; }
int wasm_snap_tag(int i) { return g_snap_tags[i]; }
int wasm_snap_aux(int i) { return g_snap_aux[i]; }
int wasm_export_snapshot(int i) {
    // put_state only reads prefix fields, which is exactly what a slot holds.
    return put_state((const Game *)(const void *)g_snaps[i].bytes, g_io);
}

// ---------- actions ---------------------------------------------------------
// Cards are read from the input buffers (byte pairs). Each action clears the
// snapshot buffer first; on success the caller reads state/logs/snapshots.

static void begin_action(void) {
    g_n_snaps = 0;
    g_pre_flip = g_game.flipped;
    g_pre_has_flip = g_game.has_flipped ? 1 : 0;
}

int wasm_start_game(void) {
    begin_action();
    start_game(&g_game);
    return 1;
}

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

int wasm_pickup(int player_idx) {
    begin_action();
    return handle_pickup(&g_game, player_idx) ? 1 : 0;
}

int wasm_good(int player_idx) {
    begin_action();
    return handle_good(&g_game, player_idx) ? 1 : 0;
}

int wasm_transition(void) {
    begin_action();
    engine_run_round_transition(&g_game);
    return 1;
}

int wasm_refill(void) {
    begin_action();
    engine_run_refill(&g_game);
    return 1;
}

// ---------- packed wire pipeline (docs/PACKED_WIRE_CUTOVER.md) --------------
//
// One call per client move: the action-wire bytes the browser validated with
// guards.wasm are applied verbatim. Wire is read from input buffer A (it is
// at most 2 + 2*AWIRE_MAX_CARDS = 58 bytes, well under MAX_IN_CARDS).
// Returns 1 applied, 0 rejected (wasm_reject_reason), -1 malformed wire.
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
        default:           return -1; // unreachable: awire_decode bounds kind
    }
}

// The TS check_win_sync, kernel-side: if the game is done, set GAME_OVER and
// park every seat (bots READY, humans IDLE — the ai seat bitmask is the one
// fact the kernel doesn't model). Returns the fool's seat, or -1 if the game
// is not over (state untouched).
int wasm_finalize_win(unsigned int ai_mask) {
    const int fool = game_done(&g_game);
    if (fool < 0) return -1;
    g_game.status = GAME_STATUS_GAME_OVER;
    for (int i = 0; i < g_game.num_players; i++) {
        g_game.players[i].status = (ai_mask >> i) & 1u
            ? PLAYER_STATUS_READY : PLAYER_STATUS_IDLE;
    }
    return fool;
}

// Per-viewer masked view blob: [VIEW_FORMAT_VERSION | viewer | masked
// put_state]. viewer < 0 = spectator. This is what get_game returns and what
// the client imports — other hands and the deck never leave as real bytes.
int wasm_view_serialize(int viewer) {
    const int v = viewer < 0 ? VIEW_SPECTATOR : viewer;
    g_io[0] = (unsigned char)VIEW_FORMAT_VERSION;
    g_io[1] = viewer < 0 ? 0xFFu : (unsigned char)viewer;
    return 2 + state_put(&g_game, v, g_io + 2);
}

// Per-viewer packed animation sequence (evwire.h) for the resident game's
// last action: hook snapshots + fresh logs + the current (post-action,
// post-finalize) state as the trailer. Call once per recipient BEFORE any
// other kernel call disturbs the snapshots. Returns length, or -1 on
// overflow/corrupt input.
int wasm_events_serialize(int viewer, int actor, int append_final_transition) {
    EvSnap refs[MAX_SNAPS];
    for (int i = 0; i < g_n_snaps; i++) {
        // put_state/state_put only read prefix fields, which is exactly what
        // a snapshot slot holds.
        refs[i].g = (const Game *)(const void *)g_snaps[i].bytes;
        refs[i].tag = g_snap_tags[i];
        refs[i].aux = g_snap_aux[i];
    }
    return evwire_serialize(refs, g_n_snaps, g_game.logs, g_game.num_logs,
                            &g_game, viewer < 0 ? VIEW_SPECTATOR : viewer,
                            actor, append_final_transition, g_io, IO_CAP);
}

// Reorder a seat's own hand to the given index order — the rearrange-hand
// meta action, validated in the kernel. Indices are single bytes in input
// buffer A. The permutation check is load-bearing (see actions/rearrange.ts
// history): n must equal the hand count, every index in range, and each used
// EXACTLY once — otherwise a hostile payload mints duplicate cards. Returns
// 1 applied, 0 invalid (state untouched).
int wasm_rearrange_hand(int seat, int n) {
    if (seat < 0 || seat >= g_game.num_players) return 0;
    Player *pl = &g_game.players[seat];
    if (n != pl->hand_count || n < 0 || n > MAX_HAND_SIZE) return 0;
    unsigned char seen[MAX_HAND_SIZE];
    Card out[MAX_HAND_SIZE];
    for (int i = 0; i < n; i++) seen[i] = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char idx = g_in_raw_a[i];
        if (idx >= (unsigned char)n || seen[idx]) return 0;
        seen[idx] = 1;
        out[i] = pl->hand[idx];
    }
    for (int i = 0; i < n; i++) pl->hand[i] = out[i];
    return 1;
}

// ---------- queries ----------------------------------------------------------

int wasm_game_done(void) { return game_done(&g_game); }
int wasm_should_act(int idx) { return should_bot_act(&g_game, idx) ? 1 : 0; }
int wasm_next_player(int cur) { return get_next_player_index(&g_game, cur); }
int wasm_can_cover(int as, int av, int ds, int dv, int power_suit) {
    Card a = { (int8_t)as, (int8_t)av }, d = { (int8_t)ds, (int8_t)dv };
    return can_cover(a, d, power_suit) ? 1 : 0;
}

// ---------- replay codec ----------------------------------------------------
// One call per direction, through a dedicated buffer (the shared g_io is
// sized for single-action exports; a whole decoded game stream is bigger).
// Formats are documented in replay.h. Both entries return bytes written or
// a negative REPLAY_E* code; wasm_replay_error_detail carries the message
// parameter (unsupported version, menu size).
//
// 2MB: decode's worst CONFORMING stream is far smaller (~50KB for a monster
// game); the ceiling only matters for hostile integers, which fail with a
// clean REPLAY_ECAP instead of unbounded growth (the TS reference grew an
// unbounded array there).

#define REPLAY_IO_CAP (2 * 1024 * 1024)
static unsigned char g_replay_io[REPLAY_IO_CAP];

unsigned char *wasm_replay_io_ptr(void) { return g_replay_io; }
int wasm_replay_io_cap(void) { return REPLAY_IO_CAP; }

// In-place: input is fully consumed before any output byte is written.
// Defense in depth: the TS bridge already checks lengths against
// wasm_replay_io_cap, but a hostile/stale caller must get a clean error,
// never reads past the replay buffer.
int wasm_replay_encode(int in_len) {
    if (in_len < 0 || in_len > REPLAY_IO_CAP) return -REPLAY_ECAP;
    return replay_encode(g_replay_io, in_len, g_replay_io, REPLAY_IO_CAP);
}
int wasm_replay_decode(int in_len) {
    if (in_len < 0 || in_len > REPLAY_IO_CAP) return -REPLAY_ECAP;
    return replay_decode(g_replay_io, in_len, g_replay_io, REPLAY_IO_CAP);
}
int wasm_replay_error_detail(void) { return replay_last_error_detail(); }

// ---------- legal moves --------------------------------------------------------
// u32 n, then per move: u8 type, u8 n_cards, n_cards x u8 wire-card cards,
// n_cards x u8 wire-card attack_cards (zeroed for non-cover moves).

int wasm_legal_moves(int bot_idx) {
    calculate_legal_moves(&g_game, bot_idx, &g_moves);
    return g_moves.n;
}

// Chunked export (the full list can exceed the IO buffer): serializes up to
// `max_moves` moves starting at `start`. Header: u32 moves written; the
// caller loops until it has wasm_legal_moves() total.
int wasm_export_moves(int start, int max_moves) {
    // Defensive clamp to the buffer: a caller with a stale chunk size gets a
    // short (but well-formed) chunk instead of an overflow into g_game. The
    // worst-case wire move is 2 + 2 x MAX_MOVE_CARDS bytes.
    int fit = (IO_CAP - 4) / (2 + 2 * MAX_MOVE_CARDS);
    if (max_moves > fit) max_moves = fit;
    unsigned char *q = g_io;
    int end = start + max_moves;
    if (end > g_moves.n) end = g_moves.n;
    if (start > end) start = end;
    unsigned int n = (unsigned int)(end - start);
    *q++ = (unsigned char)(n & 0xff);
    *q++ = (unsigned char)((n >> 8) & 0xff);
    *q++ = (unsigned char)((n >> 16) & 0xff);
    *q++ = (unsigned char)((n >> 24) & 0xff);
    for (int i = start; i < end; i++) {
        const LegalMove *m = &g_moves.moves[i];
        *q++ = (unsigned char)m->type;
        *q++ = (unsigned char)m->n_cards;
        for (int j = 0; j < m->n_cards; j++) *q++ = wire_from_card(m->cards[j]);
        for (int j = 0; j < m->n_cards; j++) *q++ = wire_from_card(m->attack_cards[j]);
    }
    return (int)(q - g_io);
}

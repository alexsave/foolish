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

// ---------- minimal libc ------------------------------------------------
void *memcpy(void *dst, const void *src, size_t n) { __builtin_memcpy(dst, src, n); return dst; }
void *memset(void *dst, int c, size_t n) { __builtin_memset(dst, c, n); return dst; }

// ---------- shared buffers ----------------------------------------------
// State export is <1KB; the widest export here is the per-action log stream
// (2 + MAX_LOGS x (4 + MAX_LOG_PAIRS x 2)). At the guards build's 64/64 that
// is ~8KB — 16KB clears it with room to spare.
#define IO_CAP (16 * 1024)
#define MAX_SNAPS 48
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
// Byte layout MUST match wasm/wasm_api.c put_state/get_state and the TS
// marshalGame/readState in engine.ts. See wasm_api.c for the field-by-field
// documentation.

static int put_state(const Game *g, unsigned char *p) {
    unsigned char *q = p;
    *q++ = (unsigned char)g->status;
    *q++ = (unsigned char)g->num_players;
    *q++ = (unsigned char)g->power_suit;
    *q++ = (unsigned char)g->first_attacker;
    *q++ = (unsigned char)g->defender;
    *q++ = (unsigned char)(g->discard_pile_length & 0xff);
    *q++ = (unsigned char)((g->discard_pile_length >> 8) & 0xff);
    *q++ = (unsigned char)(g->has_flipped ? 1 : 0);
    *q++ = wire_from_card(g->flipped);
    *q++ = (unsigned char)(g->good_players_mask & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 8) & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 16) & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 24) & 0xff);
    *q++ = (unsigned char)(g->has_good_timestamp ? 1 : 0);
    *q++ = (unsigned char)(g->deck_count & 0xff);
    *q++ = (unsigned char)((g->deck_count >> 8) & 0xff);
    for (int i = 0; i < g->deck_count; i++) *q++ = wire_from_card(g->deck[i]);
    *q++ = (unsigned char)g->num_battles;
    for (int i = 0; i < g->num_battles; i++) {
        const Battle *b = &g->table_battles[i];
        *q++ = wire_from_card(b->attack);
        *q++ = wire_from_card(b->defense);
    }
    for (int i = 0; i < g->num_players; i++) {
        const Player *pl = &g->players[i];
        *q++ = (unsigned char)pl->status;
        *q++ = (unsigned char)(pl->awaiting_attack ? 1 : 0);
        *q++ = (unsigned char)pl->hand_count;
        for (int j = 0; j < pl->hand_count; j++) *q++ = wire_from_card(pl->hand[j]);
    }
    *q++ = (unsigned char)g->num_eliminated;
    for (int i = 0; i < g->num_eliminated; i++) *q++ = (unsigned char)g->elimination_order[i];
    return (int)(q - p);
}

static void get_state(Game *g, const unsigned char *p) {
    const unsigned char *q = p;
    g->status = (int8_t)*q++;
    g->num_players = (int8_t)*q++;
    if (g->num_players < 0) g->num_players = 0;
    if (g->num_players > MAX_PLAYERS) g->num_players = MAX_PLAYERS;
    g->power_suit = (int8_t)*q++;
    g->first_attacker = (int8_t)*q++;
    g->defender = (int8_t)*q++;
    g->discard_pile_length = (int16_t)(q[0] | (q[1] << 8)); q += 2;
    g->has_flipped = (*q++ != 0);
    {
        unsigned char fw = *q++;
        if (g->has_flipped) g->flipped = card_from_wire_state(fw);
        else { g->flipped.suit = 0; g->flipped.value = 0; }
    }
    g->good_players_mask = (uint32_t)q[0] | ((uint32_t)q[1] << 8)
        | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    q += 4;
    g->has_good_timestamp = (*q++ != 0);
    g->deck_count = (int16_t)(q[0] | (q[1] << 8)); q += 2;
    if (g->deck_count < 0) g->deck_count = 0;
    if (g->deck_count > MAX_DECK) g->deck_count = MAX_DECK;
    for (int i = 0; i < g->deck_count; i++) g->deck[i] = card_from_wire_state(*q++);
    g->num_battles = (int8_t)*q++;
    if (g->num_battles < 0) g->num_battles = 0;
    if (g->num_battles > MAX_BATTLES) g->num_battles = MAX_BATTLES;
    for (int i = 0; i < g->num_battles; i++) {
        Battle *b = &g->table_battles[i];
        b->attack = card_from_wire_state(*q++);
        unsigned char db = *q++;
        b->defense = (db == WIRE_CARD_NONE) ? CARD_NONE : card_from_wire_state(db);
    }
    for (int i = 0; i < g->num_players; i++) {
        Player *pl = &g->players[i];
        pl->status = (int8_t)*q++;
        pl->awaiting_attack = (*q++ != 0);
        pl->hand_count = (int8_t)*q++;
        if (pl->hand_count < 0) pl->hand_count = 0;
        if (pl->hand_count > MAX_HAND_SIZE) pl->hand_count = MAX_HAND_SIZE;
        for (int j = 0; j < pl->hand_count; j++) pl->hand[j] = card_from_wire_state(*q++);
    }
    g->num_eliminated = (int8_t)*q++;
    if (g->num_eliminated < 0) g->num_eliminated = 0;
    if (g->num_eliminated > MAX_PLAYERS) g->num_eliminated = MAX_PLAYERS;
    for (int i = 0; i < g->num_eliminated; i++) g->elimination_order[i] = (int8_t)*q++;
    g->num_logs = 0;
    g->log_cap = 0;
    g->log_virt = 0;
}

void wasm_import_state(void) { get_state(&g_game, g_io); }
int  wasm_export_state(void) { return put_state(&g_game, g_io); }

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
static int validate_run(int kind, int player_idx, int n) {
    Game tmp;
    game_clone(&tmp, &g_game);
    void (*saved)(const Game *, int, int) = engine_snap_hook;
    engine_snap_hook = 0; // a gate must not perturb the snapshot buffer
    engine_last_reject = ENGINE_REJECT_NONE;
    int ok = 0;
    switch (kind) {
        case 0: ok = handle_attack(&tmp, player_idx, g_in_a, n); break;
        case 1: ok = handle_cover(&tmp, player_idx, g_in_a, g_in_b, n); break;
        case 2: ok = handle_pass(&tmp, player_idx, g_in_a, n); break;
        case 3: ok = handle_pickup(&tmp, player_idx); break;
        case 4: ok = handle_good(&tmp, player_idx); break;
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

// ---------- pure projections (the ex-TS duplicates) -------------------------
int wasm_game_done(void)   { return game_done(&g_game); }
int wasm_next_player(int cur) { return get_next_player_index(&g_game, cur); }
int wasm_can_cover(int as, int av, int ds, int dv, int power_suit) {
    Card a = { (int8_t)as, (int8_t)av }, d = { (int8_t)ds, (int8_t)dv };
    return can_cover(a, d, power_suit) ? 1 : 0;
}

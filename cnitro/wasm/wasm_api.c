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
#include "legal.h"

// ---------- minimal libc ------------------------------------------------

// Word-wise copies: these run on multi-KB structs in the per-action hot path
// (snapshot prefix copies, LegalMove clears), so a byte loop is not enough.
typedef unsigned long long u64;

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n >= 8 && ((size_t)d & 7) == 0 && ((size_t)s & 7) == 0) {
        *(u64 *)d = *(const u64 *)s;
        d += 8; s += 8; n -= 8;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    u64 word = 0x0101010101010101ull * (unsigned char)c;
    while (n >= 8 && ((size_t)d & 7) == 0) {
        *(u64 *)d = word;
        d += 8; n -= 8;
    }
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

// ---------- shared buffers ----------------------------------------------

#define IO_CAP (768 * 1024)
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
static Card g_in_a[MAX_IN_CARDS];   // action cards (attack/pass/cover covers)
static Card g_in_b[MAX_IN_CARDS];   // cover: the attack cards being covered

unsigned char *wasm_io_ptr(void) { return g_io; }
int wasm_io_cap(void) { return IO_CAP; }

// For sibling bridge units (wasm_bots_api.c) that operate on the same
// working game and scratch move list (LegalMoves is ~20MB at the wasm build's
// MAX_LEGAL_MOVES — not worth a second copy).
Game *wasm_game_ptr_internal(void) { return &g_game; }
LegalMoves *wasm_moves_ptr_internal(void) { return &g_moves; }

// Card input buffers: TS writes (suit,value) byte pairs.
unsigned char *wasm_cards_a_ptr(void) { return (unsigned char *)g_in_a; }
unsigned char *wasm_cards_b_ptr(void) { return (unsigned char *)g_in_b; }

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
int wasm_reject_reason(void) { return engine_last_reject; }

// ---------- state (de)serialization ---------------------------------------
//
// Layout (little-endian, byte-packed):
//   u8  status            u8  num_players     i8 power_suit
//   i8  first_attacker    i8  defender
//   u16 discard_pile_length
//   u8  has_flipped       i8 flipped_suit     i8 flipped_value
//   u32 good_players_mask u8 has_good_timestamp
//   u16 deck_count,   deck_count x (i8 suit, i8 value)
//   u8  num_battles,  num_battles x (i8 as, i8 av, i8 ds, i8 dv, u8 has_def)
//   num_players x (u8 status, u8 awaiting, u8 hand_count, hand x (i8,i8))
//   u8  num_eliminated, num_eliminated x i8

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
    *q++ = (unsigned char)g->flipped.suit;
    *q++ = (unsigned char)g->flipped.value;
    *q++ = (unsigned char)(g->good_players_mask & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 8) & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 16) & 0xff);
    *q++ = (unsigned char)((g->good_players_mask >> 24) & 0xff);
    *q++ = (unsigned char)(g->has_good_timestamp ? 1 : 0);
    *q++ = (unsigned char)(g->deck_count & 0xff);
    *q++ = (unsigned char)((g->deck_count >> 8) & 0xff);
    for (int i = 0; i < g->deck_count; i++) {
        *q++ = (unsigned char)g->deck[i].suit;
        *q++ = (unsigned char)g->deck[i].value;
    }
    *q++ = (unsigned char)g->num_battles;
    for (int i = 0; i < g->num_battles; i++) {
        const Battle *b = &g->table_battles[i];
        *q++ = (unsigned char)b->attack.suit;
        *q++ = (unsigned char)b->attack.value;
        *q++ = (unsigned char)b->defense.suit;
        *q++ = (unsigned char)b->defense.value;
        *q++ = (unsigned char)(b->has_defense ? 1 : 0);
    }
    for (int i = 0; i < g->num_players; i++) {
        const Player *pl = &g->players[i];
        *q++ = (unsigned char)pl->status;
        *q++ = (unsigned char)(pl->awaiting_attack ? 1 : 0);
        *q++ = (unsigned char)pl->hand_count;
        for (int j = 0; j < pl->hand_count; j++) {
            *q++ = (unsigned char)pl->hand[j].suit;
            *q++ = (unsigned char)pl->hand[j].value;
        }
    }
    *q++ = (unsigned char)g->num_eliminated;
    for (int i = 0; i < g->num_eliminated; i++) *q++ = (unsigned char)g->elimination_order[i];
    return (int)(q - p);
}

static void get_state(Game *g, const unsigned char *p) {
    const unsigned char *q = p;
    // No full-struct memset: the Game is ~200 KB (mostly log capacity) and
    // every read in the kernel is bounded by the counts set below, so
    // clearing the unused array tails would only burn time. num_logs is
    // reset at the end.
    g->status = (int8_t)*q++;
    g->num_players = (int8_t)*q++;
    g->power_suit = (int8_t)*q++;
    g->first_attacker = (int8_t)*q++;
    g->defender = (int8_t)*q++;
    g->discard_pile_length = (int16_t)(q[0] | (q[1] << 8)); q += 2;
    g->has_flipped = (*q++ != 0);
    g->flipped.suit = (int8_t)*q++;
    g->flipped.value = (int8_t)*q++;
    g->good_players_mask = (uint32_t)q[0] | ((uint32_t)q[1] << 8)
        | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    q += 4;
    g->has_good_timestamp = (*q++ != 0);
    g->deck_count = (int16_t)(q[0] | (q[1] << 8)); q += 2;
    for (int i = 0; i < g->deck_count; i++) {
        g->deck[i].suit = (int8_t)*q++;
        g->deck[i].value = (int8_t)*q++;
    }
    g->num_battles = (int8_t)*q++;
    for (int i = 0; i < g->num_battles; i++) {
        Battle *b = &g->table_battles[i];
        b->attack.suit = (int8_t)*q++;
        b->attack.value = (int8_t)*q++;
        b->defense.suit = (int8_t)*q++;
        b->defense.value = (int8_t)*q++;
        b->has_defense = (*q++ != 0);
    }
    for (int i = 0; i < g->num_players; i++) {
        Player *pl = &g->players[i];
        pl->status = (int8_t)*q++;
        pl->awaiting_attack = (*q++ != 0);
        pl->hand_count = (int8_t)*q++;
        for (int j = 0; j < pl->hand_count; j++) {
            pl->hand[j].suit = (int8_t)*q++;
            pl->hand[j].value = (int8_t)*q++;
        }
    }
    g->num_eliminated = (int8_t)*q++;
    for (int i = 0; i < g->num_eliminated; i++) g->elimination_order[i] = (int8_t)*q++;
    g->num_logs = 0;
}

// TS -> C: parse the IO buffer into the working game.
void wasm_import_state(void) { get_state(&g_game, g_io); }

// C -> TS: serialize the working game into the IO buffer; returns length.
int wasm_export_state(void) { return put_state(&g_game, g_io); }

// ---------- logs -----------------------------------------------------------
// u16 num_logs, then per log: i8 type, i8 player_idx, i8 defender_index,
// u8 num_pairs, num_pairs x (i8 ps, i8 pv, i8 ts, i8 tv, u8 has_target)

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
            *q++ = (unsigned char)pr->primary.suit;
            *q++ = (unsigned char)pr->primary.value;
            *q++ = (unsigned char)pr->target.suit;
            *q++ = (unsigned char)pr->target.value;
            *q++ = (unsigned char)(pr->has_target ? 1 : 0);
        }
    }
    return (int)(q - g_io);
}

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

static void begin_action(void) { g_n_snaps = 0; }

int wasm_start_game(void) {
    begin_action();
    start_game(&g_game);
    return 1;
}

int wasm_attack(int player_idx, int n_cards) {
    begin_action();
    return handle_attack(&g_game, player_idx, g_in_a, n_cards) ? 1 : 0;
}

int wasm_cover(int player_idx, int n) {
    begin_action();
    return handle_cover(&g_game, player_idx, g_in_a, g_in_b, n) ? 1 : 0;
}

int wasm_pass(int player_idx, int n_cards) {
    begin_action();
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

// ---------- queries ----------------------------------------------------------

int wasm_game_done(void) { return game_done(&g_game); }
int wasm_should_act(int idx) { return should_bot_act(&g_game, idx) ? 1 : 0; }
int wasm_next_player(int cur) { return get_next_player_index(&g_game, cur); }
int wasm_can_cover(int as, int av, int ds, int dv, int power_suit) {
    Card a = { (int8_t)as, (int8_t)av }, d = { (int8_t)ds, (int8_t)dv };
    return can_cover(a, d, power_suit) ? 1 : 0;
}

// ---------- legal moves --------------------------------------------------------
// u32 n, then per move: u8 type, u8 n_cards, n_cards x (i8,i8) cards,
// n_cards x (i8,i8) attack_cards (zeroed for non-cover moves).

int wasm_legal_moves(int bot_idx) {
    calculate_legal_moves(&g_game, bot_idx, &g_moves);
    return g_moves.n;
}

// Chunked export (the full list can exceed the IO buffer): serializes up to
// `max_moves` moves starting at `start`. Header: u32 moves written; the
// caller loops until it has wasm_legal_moves() total.
int wasm_export_moves(int start, int max_moves) {
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
        for (int j = 0; j < m->n_cards; j++) {
            *q++ = (unsigned char)m->cards[j].suit;
            *q++ = (unsigned char)m->cards[j].value;
        }
        for (int j = 0; j < m->n_cards; j++) {
            *q++ = (unsigned char)m->attack_cards[j].suit;
            *q++ = (unsigned char)m->attack_cards[j].value;
        }
    }
    return (int)(q - g_io);
}

// wasm_table_api.c - the C Table (src/table.h) as bots.wasm exports it.
//
// Linked into bots.wasm only. The table's board is the module's one resident
// Game (wasm_api.c g_game), so every table call is part of the host's single
// synchronous kernel section: load, operate, copy the products out.
//
// How things cross:
//   - Byte inputs (a state blob, a durable roster, an actor id, an action wire,
//     a game id) are written into the IO buffer back to back, with their lengths
//     as call arguments. They are opaque to the host: it moves them, it never
//     reads inside them.
//   - Structured outputs (the commit's spans and scalars, the loaded roster, a
//     decoded request, the finish order and rating changes) are C structs the
//     host reads through sdk/ts/gen/game_layout.bots.ts, generated from these
//     very headers (tools/structgen/specs/game_layout.args) and checked against
//     wasm_layout_hash at load.
//   - Byte outputs (the commit arena, a push, an envelope, a response) are
//     written into the IO buffer; a span or the return value says where.
//   - Versions, epochs and clocks cross as doubles: a JS number, no BigInt, and
//     exact for every integer a version or a millisecond clock reaches.
#include "../src/table.h"
#include <string.h>

extern Game *wasm_game_ptr_internal(void);
extern unsigned char *wasm_io_ptr(void);
extern int wasm_io_cap(void);
extern uint32_t wasm_rng_base_internal(void);

static TableSnaps   g_table_snaps;
static Table        g_table;
static TableCommit  g_table_commit;
static TableRequest g_table_request;
static TableElo     g_table_elo;
static int          g_table_ready;

static Table *table(void) {
    if (!g_table_ready) {
        table_init(&g_table, wasm_game_ptr_internal(), &g_table_snaps);
        g_table_ready = 1;
    }
    return &g_table;
}

// A game id out of the IO buffer, before the call overwrites the buffer with
// its output. 0, or TABLE_E_WIRE for an id over the trailer's cap.
static int take_gid(int gid_len, char *gid) {
    if (gid_len < 0 || gid_len > ROSTER_GAME_ID_MAX) return TABLE_E_WIRE;
    memcpy(gid, wasm_io_ptr(), (size_t)gid_len);
    return 0;
}

static uint32_t u32_of(double v) { return v < 0 ? 0u : (uint32_t)(uint64_t)v; }

Roster       *wasm_table_roster_ptr(void)  { return &table()->r; }
TableCommit  *wasm_table_commit_ptr(void)  { return &g_table_commit; }
TableRequest *wasm_table_request_ptr(void) { return &g_table_request; }
TableElo     *wasm_table_elo_ptr(void)     { return &g_table_elo; }
int wasm_table_detail(void) { return table()->detail; }
int wasm_table_reject(void) { return table()->reject; }

// io = the request body.
int wasm_table_request_decode(int len) {
    if (len < 0 || len > wasm_io_cap()) return TABLE_E_WIRE;
    return table_request_decode(wasm_io_ptr(), len, &g_table_request);
}

// io = [state blob][durable roster].
int wasm_table_load(int state_len, int roster_len) {
    if (state_len < 0 || roster_len < 0 || state_len + roster_len > wasm_io_cap()) return TABLE_E_WIRE;
    Table *t = table();
    t->rng_base = wasm_rng_base_internal();
    const unsigned char *io = wasm_io_ptr();
    return table_load(t, io, state_len, io + state_len, roster_len);
}

// io = [actor id][action wire]. intent < 0: the request carried no intent.
int wasm_table_act(int id_len, int wire_len, double intent, double round_epoch) {
    if (id_len < 0 || wire_len < 0 || id_len + wire_len > wasm_io_cap()) return TABLE_E_WIRE;
    const unsigned char *io = wasm_io_ptr();
    return table_act(table(), (const char *)io, id_len, io + id_len, wire_len,
                     intent < 0 ? -1 : (int64_t)intent, (int64_t)round_epoch);
}

int wasm_table_needs_bots(void) { return table_needs_bots(table()) ? 1 : 0; }
int wasm_table_bots_need_logs(void) { return table_bots_need_logs(table()) ? 1 : 0; }

// io = [game id] -> io = the arena wasm_table_commit_ptr's spans point into.
int wasm_table_commit_products(int gid_len, double next_version, double now_ms) {
    char gid[ROSTER_GAME_ID_MAX];
    if (take_gid(gid_len, gid) != 0) return TABLE_E_WIRE;
    return table_commit_products(table(), gid, gid_len, u32_of(next_version), (int64_t)now_ms,
                                 &g_table_commit, wasm_io_ptr(), wasm_io_cap());
}

// io = [game id] -> io = one viewer's as3 push.
int wasm_table_push(int gid_len, int viewer) {
    char gid[ROSTER_GAME_ID_MAX];
    if (take_gid(gid_len, gid) != 0) return TABLE_E_WIRE;
    return table_push(table(), gid, gid_len, viewer, wasm_io_ptr(), wasm_io_cap());
}

// io = [game id] -> io = one viewer's response envelope.
int wasm_table_envelope(int gid_len, int viewer, double version) {
    char gid[ROSTER_GAME_ID_MAX];
    if (take_gid(gid_len, gid) != 0) return TABLE_E_WIRE;
    return table_envelope(table(), gid, gid_len, viewer, u32_of(version), wasm_io_ptr(), wasm_io_cap());
}

// -> io = the action response body.
int wasm_table_action_response(int result, int reject, double version) {
    return table_action_response(result, reject, u32_of(version), wasm_io_ptr(), wasm_io_cap());
}

// -> TableElo.order, best first.
int wasm_table_rankings(void) {
    return table_rankings(table(), g_table_elo.order);
}

// TableElo.ratings (by seat) and .order (best first) -> TableElo.deltas (by seat).
int wasm_elo_deltas(int n) {
    return elo_deltas(g_table_elo.ratings, g_table_elo.order, n, g_table_elo.deltas);
}

// wasm_local_api.c - the browser's local table (src/local_table.h) as web.wasm
// exports it.
//
// How things cross, as in wasm_table_api.c: byte inputs (a game id, the deal
// seed, a display name, a request or ask body) are written into the IO buffer
// back to back with their lengths as arguments, and byte outputs (an envelope,
// a response body, a cast) come back in the IO buffer with the length returned.
// Clocks cross as doubles. Every output is masked for the human's seat; nothing
// here can hand out the board or the bot's hand.
//
// The table's storage is allocated on the first open, so a visitor who never
// plays a local game carries none of it; its board is the module's one resident
// Game, adopted afresh by every operation (local_table.h).
#include "../src/local_table.h"
#include <stdlib.h>
#include <string.h>

extern Game *wasm_game_ptr_internal(void);
extern unsigned char *wasm_io_ptr(void);
extern int wasm_io_cap(void);

static LocalTable *g_local;
static TableSnaps *g_local_snaps;

// io = [game id][seed: FOOLISH_SEED_LEN][the human's name]. A new table; the
// human's envelope at io, its length, or a refusal.
int wasm_local_open(int gid_len, int you_len, double now_ms) {
    if (gid_len <= 0 || gid_len > ROSTER_GAME_ID_MAX || you_len < 0 || you_len > ROSTER_NAME_MAX) return TABLE_E_WIRE;
    if (!g_local) g_local = calloc(1, sizeof *g_local);
    if (!g_local_snaps) g_local_snaps = calloc(1, sizeof *g_local_snaps);
    if (!g_local || !g_local_snaps) return TABLE_E_CAP;
    char gid[ROSTER_GAME_ID_MAX], you[ROSTER_NAME_MAX];
    uint8_t seed[FOOLISH_SEED_LEN];
    const unsigned char *io = wasm_io_ptr();
    memcpy(gid, io, (size_t)gid_len);
    memcpy(seed, io + gid_len, FOOLISH_SEED_LEN);
    memcpy(you, io + gid_len + FOOLISH_SEED_LEN, (size_t)you_len);
    const int rc = local_table_open(g_local, wasm_game_ptr_internal(), g_local_snaps, gid, gid_len, seed, you, you_len,
                                    (int64_t)now_ms);
    if (rc != TABLE_OK) return rc;
    return local_table_envelope(g_local, wasm_io_ptr(), wasm_io_cap());
}

// The human's envelope at io: its length, or a refusal.
int wasm_local_view(void) { return local_table_envelope(g_local, wasm_io_ptr(), wasm_io_cap()); }

int wasm_local_start(double now_ms) { return local_table_start(g_local, (int64_t)now_ms); }

// io = [the action request body]. The response body at io: its length, or a
// refusal. In place: the request is read whole before the response is written.
int wasm_local_action(int len, double now_ms) {
    if (len <= 0 || len > wasm_io_cap()) return TABLE_E_WIRE;
    return local_table_action(g_local, wasm_io_ptr(), len, (int64_t)now_ms, wasm_io_ptr(), wasm_io_cap());
}

// io = [an ask body]. The table's result.
int wasm_local_ask(int len, double now_ms) {
    if (len <= 0 || len > wasm_io_cap()) return TABLE_E_WIRE;
    return local_table_ask(g_local, wasm_io_ptr(), len, (int64_t)now_ms);
}

int wasm_local_bot_wait(double now_ms) { return local_table_bot_wait_ms(g_local, (int64_t)now_ms); }

int wasm_local_bot(double now_ms) { return local_table_bot(g_local, (int64_t)now_ms); }

// The last committed operation's cast at io, once: its length, 0 for none.
int wasm_local_cast(void) { return local_table_cast(g_local, wasm_io_ptr(), wasm_io_cap()); }

// The finished game's replay (local_table_replay), made on the first ask: the
// moves code at io (`which` 0) or the extras blob (1), its length, or a refusal.
// The decode gate's workspace is allocated the first time, like the table's.
#define LOCAL_REPLAY_SCRATCH (256 * 1024)
static uint8_t *g_local_scratch;
int wasm_local_replay(int which) {
    if (!g_local) return TABLE_E_NOT_LOADED;
    if (!g_local_scratch) g_local_scratch = malloc(LOCAL_REPLAY_SCRATCH);
    if (!g_local_scratch) return TABLE_E_CAP;
    const int rc = local_table_replay(g_local, g_local_scratch, LOCAL_REPLAY_SCRATCH);
    if (rc != TABLE_OK) return rc;
    return which == 0 ? local_table_replay_moves(g_local, wasm_io_ptr(), wasm_io_cap())
                      : local_table_replay_extras(g_local, wasm_io_ptr(), wasm_io_cap());
}

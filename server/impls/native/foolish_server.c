// foolish_server.c — a DEDICATED, in-memory Foolish server, in C.
//
// A second backend under server/impls (sibling to supabase/), to prove the
// server API is language-agnostic: same game, no TypeScript, no edge runtime,
// no Postgres. A long-lived process holds every game as a `Game` struct in RAM
// (the "in-memory authoritative state" of docs/ARCHITECTURE_AS_A_PATTERN.md),
// guarded by per-game locks (see "Locking" below), and the C KERNEL drives all
// of it — this file only starts a socket, routes requests, and hands them to
// the kernel. Every rule (deal, legality, apply, refill, who-is-the-fool, the
// masked per-seat view) is c/src/*.c, exactly as the wasm/edge build uses it.
// Swap Postgres for a hash table and the edge runtime for a thread pool and
// the game is unchanged.
//
// POC scope: HTTP/1.1 (hand-rolled — a real deployment would drop in mongoose
// or civetweb), token auth in a memory map (no JWT). Endpoints mirror the
// contract:
//   POST /auth/signup {username}         -> {token,user_id}
//   POST /auth/signin {username}         -> {token,user_id}
//   POST /create            (bearer)     -> {game_id}
//   POST /meta {type,game_id[,strategy]} (bearer)  type: join|start|add-bot
//   POST /action {game_id,move:{...}}    (bearer)  applies + runs bots
//   GET  /state?game_id=..&seat=..       -> the kernel's masked view JSON
//   GET  /health
//   GET  /stats                          -> Stage 4 bot-decision counters (JSON)
//   GET  /ws?game_id=..&seat=..  (bearer, Upgrade: websocket)
//                                         -> RFC 6455 WebSocket. ONE persistent
//     connection per (authenticated, seated) client replaces the HTTP
//     action+state round trip for the hot loop: every binary frame the
//     client sends is applied as an awire move (or, if empty, treated as a
//     "give me current state" poll) and answered with one binary frame,
//     [ok:u8][state_put masked view bytes]. See ws.h/ws.c and the "T1b"
//     section of PROFILE_HOTPATH.md for why: thread-per-CONNECTION instead
//     of thread-per-REQUEST amortizes pthread_create's cost (a fresh
//     thread's zeroed stack/TLS was 85.8% of instructions under load,
//     T1) over a client's entire session instead of paying it per move.
//   GET  /ws?game_id=..&spectator=1  (bearer, Upgrade: websocket)
//                                         -> Stage 4: the same WebSocket, but
//     for a read-only watcher that owns no seat — masked with VIEW_SPECTATOR
//     (every hand AND the deck hidden), and any frame it sends is ignored,
//     never applied. See ws_conn_thread's own comment for the full design.
//
// TLS (Stage 3, TLS.md): pass `--tls --cert=PATH --key=PATH` to serve every
// endpoint above over TLS instead — https:// for the one-shot endpoints,
// wss:// for /ws — off ONE shared, read-only-after-setup SSL_CTX with a
// fresh per-connection SSL* (see conn.h/conn.c). Without --tls the listener
// is plain HTTP/WS, byte-for-byte unchanged from every earlier stage.
//
// Concurrency ("T2a", PROFILE_HOTPATH.md / SERVER_SCALING.md): a dispatcher
// (the accept loop) reads + parses each request and routes it either to a
// dedicated per-connection thread (a /ws upgrade — still long-lived, see
// ws_conn_thread) or onto a small typed work-queue pool sharded by game_id
// (every other endpoint — see "Work-queue routing" below). Per-game state is
// guarded by that GameSlot's own lock, not one process-wide mutex — see
// "Locking" below for the two-tier scheme and its lock-order invariant.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>       // Stage 6: O_NONBLOCK
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp — the hand-rolled header scan below
#include <sys/epoll.h>   // Stage 6: epoll-per-shard
#include <sys/eventfd.h> // Stage 6: cross-thread worker wakeup (dispatcher handoff + bot_thread push)
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "legal.h"
#include "view.h"
#include "awire.h"
#include "bot_drive.h"
#include "bot_roster.h"
#include "strategy.h"   // STRAT_RANDOM — see h_meta's add-bot branch (Stage 4 strategy_key fix)
#include "ws.h"
#include "persist.h"
#include "conn.h"   // Stage 3: TLS — see conn.h and the "STAGE 3: OpenSSL TLS" block below
#ifdef FOOLISH_QUIC
#include "quic_wt.h"      // QUIC/HTTP3/WebTransport front-end (foolish_server_quic build)
#include "game_bridge.h"  // wrappers quic_wt.c uses to reach the shared game
#endif

// --------------------------------------------------------------------------
// In-memory store (the "fake DB"): games + users, per-game locks.
// --------------------------------------------------------------------------

// Games and users are APPEND-ONLY in this in-memory server (a slot's `used`
// flag is created true and never cleared). They live in lazily-allocated,
// fixed-size CHUNKS rather than one giant static array, so RSS tracks the number
// of live games (each GameSlot is ~48 KB) instead of a compile-time ceiling, and
// a chunk's address never moves — every GameSlot*/User* handed out (the hash
// tables, EConn->slot, bot_thread) stays valid for the process lifetime.
// MAX_GAMES/MAX_USERS are now just the (very high) ceilings; memory is spent per
// chunk actually touched. See g_game_chunks / game_slot_ensure below.
#define GAMES_PER_CHUNK   512
#define USERS_PER_CHUNK   4096
#define MAX_GAME_CHUNKS   512                       // ceiling 262,144 live games
#define MAX_USER_CHUNKS   128                       // ceiling 524,288 users (kept == the token hash's half-load point)
#define MAX_GAMES (GAMES_PER_CHUNK * MAX_GAME_CHUNKS)
#define MAX_USERS (USERS_PER_CHUNK * MAX_USER_CHUNKS)
#define ID_LEN 12

typedef struct {
    bool used;
    char token[33];
    char user_id[ID_LEN + 1];
    char username[24];
    int  slot_idx;   // this user's append index, for persist_mark_dirty (was `u - g_users`); runtime-only, not serialized
} User;

// Worst-case state_put(...) output for this build's caps: 16-byte header +
// MAX_DECK (64) card bytes + 1 + MAX_BATTLES*2 (32*2) + MAX_PLAYERS*(3 +
// MAX_HAND_SIZE) (8*(3+64)) + 1 + MAX_PLAYERS (8) = 690 bytes. Rounded up
// with real margin for VIEW_CACHE below (h_ws/h_state's own wire buffers use
// a much larger 65536 "don't think about it" cap; this one is sized because
// it's paid MAX_PLAYERS+1 times per game, see GameSlot.view_cache). The
// spectator viewer (VIEW_SPECTATOR, Stage 4) masks EVERY hand and the deck —
// its output is never bigger than any per-seat view (same field counts, just
// more of them hidden), so it fits this same cap with no change.
#define VIEW_CACHE_CAP 1024

// Stage 4 (SERVER_SCALING.md "Stage 4 — spectators"): the shared cache slot
// for the one masked view every spectator of a game sees (VIEW_SPECTATOR —
// all hands + the deck hidden, the SAME bytes for every spectator of this
// game at a given version, unlike a per-seat view). One extra slot past the
// MAX_PLAYERS per-seat ones in GameSlot.view_cache*, keyed the same way
// (recompute iff view_cache_version[SPECTATOR_CACHE_IDX] != s->version).
#define SPECTATOR_CACHE_IDX MAX_PLAYERS

typedef struct {
    bool used;
    char id[ID_LEN + 1];
    Game game;                      // THE kernel state — incl. its own lifecycle status
    // Lobby roster (identity lives beside the state blob, never in it — game.h).
    char owner[ID_LEN + 1];
    char seat_user[MAX_PLAYERS][ID_LEN + 1];  // "" for a bot
    char seat_name[MAX_PLAYERS][24];
    bool seat_ready[MAX_PLAYERS];             // lobby "hit ready" — host state; kind (human/bot) lives in the kernel's strategy_key
    // Per-game lock (T2a). Guards EVERYTHING below this point plus the
    // `game` and lobby-roster fields above: this slot's whole game state,
    // once it is reachable via g_game_ht. See the "Locking" block below
    // g_registry_lock's declaration for the two-tier scheme and the lock
    // order invariant every handler in this file follows.
    pthread_mutex_t lock;
    // One per-game trampoline thread paces the bots (see bot_thread). It waits on
    // `cond` when a human is owed; /action signals it. `bot_running` guards
    // against spawning a second driver. Both now pair with `lock` (above),
    // not a process-wide mutex.
    pthread_cond_t cond;
    bool bot_running;
    int  slot_idx;   // this game's append index, for persist_mark_dirty (was `s - g_games`); runtime-only, BELOW `lock` so it is outside the serialized prefix
    // PROFILE_HOTPATH.md "T1c": under WS+legal load, view.c:state_put was the
    // single hottest function (~21-28% of instructions, inclusive) because
    // /ws's hot loop called it on EVERY round trip, including a pure "poll"
    // that changed nothing. `version` is bumped every time this slot's Game
    // could have changed (a human's move applied via /action or /ws, a lobby
    // transition, or a bot_drive cycle that applied >=1 action) — never on a
    // no-op poll. `view_cache`/`view_cache_len` hold the last state_put(...)
    // bytes computed for each seat, and `view_cache_version` records which
    // `version` they were computed at; state_put_cached (below) recomputes
    // only when the two disagree, else memcpy's the cached bytes. Same wire
    // bytes either way — this is a pure CPU-cost cut, not a protocol change,
    // so the client needs no changes and always sees fresh-as-of-version
    // state on every round trip.
    uint32_t version;
    // +1: index SPECTATOR_CACHE_IDX (== MAX_PLAYERS) is the shared spectator
    // cache slot (Stage 4) — see that macro's doc above.
    unsigned char view_cache[MAX_PLAYERS + 1][VIEW_CACHE_CAP];
    int      view_cache_len[MAX_PLAYERS + 1];
    uint32_t view_cache_version[MAX_PLAYERS + 1];
} GameSlot;

// Stage 6 (epoll-per-shard, SERVER_SCALING.md "Stage 6"): forward-declared so
// bot_thread (below) can call it at its existing version-bump site. Full
// definition lives with the rest of the epoll worker machinery, near the end
// of this file — it needs g_n_game_workers and the epoll worker array, both
// declared later. A no-op whenever the server is running in --tls mode
// (which keeps the pre-Stage-6 thread-per-connection /ws design — see that
// section's own doc for why) — see the definition's `g_epoll_active` guard.
static void epoll_notify_game_changed(GameSlot *s);

// Chunk directories: g_*_chunks[c] is NULL until slot c*PER_CHUNK is first
// touched, then a calloc'd block that never moves or frees. g_*_count is the
// append cursor (== number ever created; slots are never reclaimed).
static GameSlot *g_game_chunks[MAX_GAME_CHUNKS];
static User     *g_user_chunks[MAX_USER_CHUNKS];
static int       g_games_count = 0;
static int       g_users_count = 0;

// idx -> slot, allocating the backing chunk on first touch (this is where RAM
// grows with live games). Returns NULL past the ceiling or on OOM.
static GameSlot *game_slot_ensure(int idx) {
    if (idx < 0 || idx >= MAX_GAMES) return NULL;
    GameSlot **chunk = &g_game_chunks[idx / GAMES_PER_CHUNK];
    if (!*chunk) { *chunk = calloc(GAMES_PER_CHUNK, sizeof(GameSlot)); if (!*chunk) return NULL; }
    return &(*chunk)[idx % GAMES_PER_CHUNK];
}
static User *user_slot_ensure(int idx) {
    if (idx < 0 || idx >= MAX_USERS) return NULL;
    User **chunk = &g_user_chunks[idx / USERS_PER_CHUNK];
    if (!*chunk) { *chunk = calloc(USERS_PER_CHUNK, sizeof(User)); if (!*chunk) return NULL; }
    return &(*chunk)[idx % USERS_PER_CHUNK];
}
// Read-only accessors (no allocation) — valid for any idx a slot was created at.
static inline GameSlot *game_slot_at(int idx) {
    if (idx < 0 || idx >= g_games_count) return NULL;
    GameSlot *c = g_game_chunks[idx / GAMES_PER_CHUNK];
    return c ? &c[idx % GAMES_PER_CHUNK] : NULL;
}
static inline User *user_slot_at(int idx) {
    if (idx < 0 || idx >= g_users_count) return NULL;
    User *c = g_user_chunks[idx / USERS_PER_CHUNK];
    return c ? &c[idx % USERS_PER_CHUNK] : NULL;
}

// Registered in main() before persist_start (see "STAGE 2: persistence"
// below) — NULL until then, which is fine: nothing calls game_mark_dirty or
// marks a user dirty before main() finishes setup and starts the worker
// pools / accept loop.
static PersistTable *g_game_table = NULL;
static PersistTable *g_user_table = NULL;

// --------------------------------------------------------------------------
// STAGE 2: SQLite WAL write-behind persistence + crash recovery. Called
// under `s->lock` (see GameSlot.lock) at every point this slot's Game or
// lobby roster could have changed — the exact same events that bump
// `s->version` (a human move via /action or /ws, a lobby transition in
// /meta, a bot_drive cycle that applied >=1 action or ended the game, or a
// fresh /create). O(1) and never touches disk: just flips one bool in
// persist.c's dirty bitmap for this slot's index and signals the
// persistence thread — see persist.h's top comment for the full write-
// behind model and DURABILITY.md for the design writeup + measurements.
// `s - g_games` is safe pointer arithmetic: every GameSlot this function is
// ever called with lives inside the g_games[MAX_GAMES] array (there is no
// other way to obtain a GameSlot*), so the difference is always in range.
static void game_mark_dirty(GameSlot *s) {
    if (g_game_table) persist_mark_dirty(g_game_table, s->slot_idx);
}

// --------------------------------------------------------------------------
// GameSlot <-> durable blob. serialize_slot/deserialize_slot are the ONLY
// place that knows this on-disk layout; everything above (state_put/
// state_get) is the kernel's own exact round-trip codec for `Game` — this
// just wraps it with the lobby/identity fields view.c's codec deliberately
// never carries (game.h: identity lives with the host, never in the state
// blob). Versioned with a leading byte so a future layout change can detect
// (and refuse, rather than misinterpret) an old row.
//
// Layout (see DURABILITY.md for the worked-out byte budget):
//   [0]                                  version (PERSIST_GAME_BLOB_VERSION)
//   [1..2]                               state_len, uint16 LE
//   [3 .. 3+state_len)                   state_put(&game, VIEW_UNMASKED, .)
//   next ID_LEN+1 bytes                  id
//   next ID_LEN+1 bytes                  owner
//   next MAX_PLAYERS*(ID_LEN+1) bytes    seat_user[]
//   next MAX_PLAYERS*24 bytes            seat_name[]
//   next MAX_PLAYERS bytes               seat_ready[] (1 byte each, 0/1)
// Worst case: 3 + 690 (state_put's documented worst case — see
// VIEW_CACHE_CAP above) + 13*2 + 8*13 + 8*24 + 8 = 1023 bytes.
// PERSIST_GAME_BLOB_CAP gives real margin, same discipline as VIEW_CACHE_CAP.
// --------------------------------------------------------------------------
#define PERSIST_GAME_BLOB_VERSION 1
#define PERSIST_GAME_BLOB_CAP 2048

// Returns bytes written, or -1 if it wouldn't fit in `cap` (never happens at
// PERSIST_GAME_BLOB_CAP given the worst case above — defensive, same
// "correctness over the optimization" stance state_put_cached takes).
static int serialize_slot(const GameSlot *s, unsigned char *buf, int cap) {
    unsigned char state[1 + 65536];   // state_put's own documented cap (h_state uses the same 65536)
    int state_len = state_put(&s->game, VIEW_UNMASKED, state);
    int need = 1 + 2 + state_len + (ID_LEN + 1) * 2
             + MAX_PLAYERS * (ID_LEN + 1) + MAX_PLAYERS * 24 + MAX_PLAYERS;
    if (state_len < 0 || need > cap) return -1;
    unsigned char *q = buf;
    *q++ = PERSIST_GAME_BLOB_VERSION;
    *q++ = (unsigned char)(state_len & 0xff);
    *q++ = (unsigned char)((state_len >> 8) & 0xff);
    memcpy(q, state, (size_t)state_len); q += state_len;
    memcpy(q, s->id, ID_LEN + 1); q += ID_LEN + 1;
    memcpy(q, s->owner, ID_LEN + 1); q += ID_LEN + 1;
    for (int i = 0; i < MAX_PLAYERS; i++) { memcpy(q, s->seat_user[i], ID_LEN + 1); q += ID_LEN + 1; }
    for (int i = 0; i < MAX_PLAYERS; i++) { memcpy(q, s->seat_name[i], 24); q += 24; }
    for (int i = 0; i < MAX_PLAYERS; i++) *q++ = (unsigned char)(s->seat_ready[i] ? 1 : 0);
    return (int)(q - buf);
}

// Inverse of serialize_slot. `s` MUST already be zeroed by the caller (same
// contract h_create's fresh-slot memset follows) — this never touches
// s->lock/s->cond/s->bot_running/s->version/s->view_cache* (runtime-only
// fields the caller (re-)initializes separately; see game_persist_load /
// h_create). Rejects (returns false, touches nothing) on a version mismatch
// or a length too short for its own encoded state_len — defense in depth
// against a corrupted DB row, never trusts `len` blindly (same posture
// state_get's own bounds-clamping takes against a hostile/corrupt blob).
static bool deserialize_slot(GameSlot *s, const unsigned char *buf, int len) {
    if (len < 3 || buf[0] != PERSIST_GAME_BLOB_VERSION) return false;
    const unsigned char *q = buf + 1;
    int state_len = q[0] | (q[1] << 8); q += 2;
    int fixed_tail = (ID_LEN + 1) * 2 + MAX_PLAYERS * (ID_LEN + 1) + MAX_PLAYERS * 24 + MAX_PLAYERS;
    if (state_len < 0 || state_len > 65536 || 3 + state_len + fixed_tail > len) return false;
    state_get(&s->game, q, /*masked=*/0);   // exact inverse of state_put(.., VIEW_UNMASKED, ..)
    q += state_len;
    memcpy(s->id, q, ID_LEN + 1); s->id[ID_LEN] = 0; q += ID_LEN + 1;
    memcpy(s->owner, q, ID_LEN + 1); s->owner[ID_LEN] = 0; q += ID_LEN + 1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        memcpy(s->seat_user[i], q, ID_LEN + 1); s->seat_user[i][ID_LEN] = 0; q += ID_LEN + 1;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        memcpy(s->seat_name[i], q, 24); s->seat_name[i][23] = 0; q += 24;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) s->seat_ready[i] = (*q++ != 0);
    s->used = true;
    return true;
}

// User <-> durable blob. Fixed-width (every User field already is), so no
// length field is needed the way serialize_slot needs state_len.
#define PERSIST_USER_BLOB_VERSION 1
#define PERSIST_USER_BLOB_CAP 128

static int serialize_user(const User *u, unsigned char *buf, int cap) {
    int need = 1 + (int)sizeof u->token + (int)sizeof u->user_id + (int)sizeof u->username;
    if (need > cap) return -1;
    unsigned char *q = buf;
    *q++ = PERSIST_USER_BLOB_VERSION;
    memcpy(q, u->token, sizeof u->token); q += sizeof u->token;
    memcpy(q, u->user_id, sizeof u->user_id); q += sizeof u->user_id;
    memcpy(q, u->username, sizeof u->username); q += sizeof u->username;
    return (int)(q - buf);
}
static bool deserialize_user(User *u, const unsigned char *buf, int len) {
    int need = 1 + (int)sizeof u->token + (int)sizeof u->user_id + (int)sizeof u->username;
    if (len != need || buf[0] != PERSIST_USER_BLOB_VERSION) return false;
    const unsigned char *q = buf + 1;
    memcpy(u->token, q, sizeof u->token); q += sizeof u->token; u->token[sizeof u->token - 1] = 0;
    memcpy(u->user_id, q, sizeof u->user_id); q += sizeof u->user_id; u->user_id[sizeof u->user_id - 1] = 0;
    memcpy(u->username, q, sizeof u->username); q += sizeof u->username; u->username[sizeof u->username - 1] = 0;
    u->used = true;
    return true;
}

// Correctness gate: serialize -> deserialize -> serialize again must be
// byte-identical. Runs once at startup on a small synthetic game (populated
// with non-default values in every field family — deck, battles, hands,
// lobby roster — so a field silently dropped from either direction shows up
// as a mismatch, not a coincidental pass) — a real regression test, not
// decoration: a mismatch here means the wire format and the round-trip code
// have drifted, and the server refuses to start rather than silently
// persisting (or recovering) corrupt/lossy snapshots.
static void persist_self_test(void) {
    // static: sizeof(GameSlot) is tens of KB (mostly the Game's MAX_LOGS-
    // sized log array — see game.h) and this runs once, so a static scratch
    // beats a fat stack frame.
    static GameSlot a, b;
    memset(&a, 0, sizeof a);
    a.used = true;
    snprintf(a.id, sizeof a.id, "selftest0001");
    snprintf(a.owner, sizeof a.owner, "selftest0001");
    a.game.status = GAME_STATUS_PLAYING;
    a.game.num_players = 3;
    a.game.power_suit = 2;
    a.game.first_attacker = 0;
    a.game.defender = 1;
    a.game.deck_count = 5;
    for (int i = 0; i < 5; i++) { a.game.deck[i].suit = (int8_t)(i % 4); a.game.deck[i].value = (int8_t)(5 + i); }
    a.game.num_battles = 1;
    a.game.table_battles[0].attack.suit = 1; a.game.table_battles[0].attack.value = 9;
    a.game.table_battles[0].defense = CARD_NONE;
    for (int i = 0; i < 3; i++) {
        snprintf(a.seat_user[i], sizeof a.seat_user[i], "user%07d", i);
        snprintf(a.seat_name[i], sizeof a.seat_name[i], "player-%d", i);
        a.seat_ready[i] = (i % 2) == 0;
        snprintf(a.game.players[i].name, sizeof a.game.players[i].name, "player-%d", i);
        snprintf(a.game.players[i].player_id, sizeof a.game.players[i].player_id, "user%07d", i);
        a.game.players[i].status = PLAYER_STATUS_IN;
        a.game.players[i].strategy_key = (i == 2) ? 0 : STRATEGY_KEY_HUMAN;
        a.game.players[i].hand_count = (int8_t)(2 + i);
        for (int j = 0; j < a.game.players[i].hand_count; j++) {
            a.game.players[i].hand[j].suit = (int8_t)((i + j) % 4);
            a.game.players[i].hand[j].value = (int8_t)(6 + j);
        }
    }

    static unsigned char blob1[PERSIST_GAME_BLOB_CAP], blob2[PERSIST_GAME_BLOB_CAP];
    int n1 = serialize_slot(&a, blob1, sizeof blob1);
    if (n1 < 0) { fprintf(stderr, "persist self-test: FAIL (serialize_slot didn't fit)\n"); exit(1); }
    memset(&b, 0, sizeof b);
    if (!deserialize_slot(&b, blob1, n1)) {
        fprintf(stderr, "persist self-test: FAIL (deserialize_slot rejected a round-trip blob)\n"); exit(1);
    }
    int n2 = serialize_slot(&b, blob2, sizeof blob2);
    if (n2 != n1 || memcmp(blob1, blob2, (size_t)n1) != 0) {
        fprintf(stderr, "persist self-test: FAIL (serialize->deserialize->serialize not byte-identical, "
                         "n1=%d n2=%d)\n", n1, n2);
        exit(1);
    }
    fprintf(stderr, "persist self-test: OK (games: %d-byte round-trip byte-identical)\n", n1);
}

// --------------------------------------------------------------------------
// Locking (T2a introduced the two-tier scheme below, replacing the single
// global g_lock; Stage 5, SERVER_SCALING.md "Stage 5 — parallel bot compute",
// DROPPED the THIRD lock T2a added on top of it — read on for why that is now
// safe). Two tiers remain:
//
//   g_registry_lock — small and SHORT-HELD. Guards ONLY: g_users[] (signup /
//     token lookup) + g_token_ht, game-slot allocation (claiming a free
//     g_games[] entry) + g_game_ht (game_id -> GameSlot*). Never held during
//     game work, bot work, or socket I/O.
//
//   GameSlot.lock (per game) — guards everything else about ONE game: its
//     `Game` struct, lobby roster (seat_user/seat_name/seat_ready/owner),
//     cond/bot_running, and the per-seat view_cache. This is now the ONLY
//     lock taken around a kernel-mutating call (awire_apply, bot_drive,
//     game_seat_and_deal).
//
// T2a's g_kernel_lock (REMOVED, Stage 5): a third, process-wide mutex held
// around every kernel call that mutates a Game or drives bots, because the
// kernel (c/src, read-only to us — see this file's header) used to keep
// process-wide, non-thread-local scratch state across those calls:
// bot_drive.c's `g_scratch` eligibility buffer, game.c's `engine_last_reject`
// (the reject-reason out-param every handle_* writes) and `engine_snap_hook`
// (saved/restored by bot_drive's `choose_move`), plus two more the Stage 5
// kernel audit found beyond that original list — game.c's log-overflow
// scratch `GameLog` inside `log_alloc`, and cordite_sim.c's lazily-built
// card-id lookup masks (`ensure_masks`), which every cordite/octogen decision
// touches via `cd_sim_from_game`. Under the OLD single global g_lock this was
// safe by accident (the whole server was one critical section, so no two
// kernel-mutating calls ever ran concurrently); per-game locks alone
// reintroduced exactly the concurrent-mutation-across-DIFFERENT-games case
// those kernel statics were never built for — confirmed by a Helgrind run on
// an early per-game-lock-only build (a genuine write/write race on
// `engine_last_reject` between two games' threads, SERVER_SCALING.md "T2a").
// g_kernel_lock was the honest fix at the time, but it also serialized every
// game's bot COMPUTE process-wide — Stage 4 measured octogen decisions/s
// plateauing at ~30/s (the single-thread ceiling) no matter how many games
// ran concurrently. Stage 5 (c/src/*, see game.h/game.c/bot_drive.c/
// cordite_sim.c) made every one of those kernel statics `_Thread_local`
// instead of removing the lock outright: each thread now owns its own
// instance, so two DIFFERENT games' kernel calls on two DIFFERENT threads no
// longer share any mutable kernel state — only same-game concurrency needs
// serializing, and GameSlot.lock already does that. Verified by Helgrind on
// this build (0 kernel/server data races — see SERVER_SCALING.md "Stage 5")
// and by the difftest suite (single-threaded, so `_Thread_local` is
// transparent there — byte-identical play, proving the kernel change itself
// carries no behavior difference).
//
// LOCK ORDER (deadlock-freedom): registry, then game — ALWAYS, and never the
// reverse. Every handler below that needs the registry takes
// g_registry_lock, finds the User*/GameSlot*, takes the GameSlot's own lock,
// THEN releases g_registry_lock (never re-acquired while any GameSlot.lock is
// held). No handler here ever holds two GameSlot locks. bot_thread and the
// /ws dedicated connection thread (ws_conn_thread) follow the same rule: each
// only ever holds its own game's lock — neither touches g_registry_lock after
// its initial (game_id -> GameSlot*) lookup.
// --------------------------------------------------------------------------
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
// g_seq: a monotonic counter mixed into gen_id() (registry-guarded — every
// caller holds g_registry_lock) AND read directly by h_meta's "start" branch
// to help season the deal seed (which runs under a GameSlot lock, not the
// registry lock — a Helgrind-caught race in an earlier build of this file).
// Rather than invent a case where the lock order above would need registry-
// while-holding-game (forbidden), it's simplest and correct to make the
// counter itself atomic and drop the lock story entirely — it has no other
// invariant to protect, just "some number that goes up."
static atomic_ulong g_seq = 0;

// --------------------------------------------------------------------------
// Stage 4 (SERVER_SCALING.md "Stage 4 — spectators + octogen stress"):
// process-wide bot-decision counters, read by GET /stats. A "decision" here
// is one BotDriveAction actually applied by bot_thread's bot_drive call
// (drv.actions[0..drv.n)) — the unit bot_stress.sh measures decisions/sec
// against, at 1 game and at N games. Stage 4 used this to quantify
// g_kernel_lock's ceiling on bot compute (see the "Locking" doc above);
// Stage 5 removed that lock and re-ran the same sweep to show the ceiling
// lifting (SERVER_SCALING.md "Stage 5"). `g_bot_decisions` counts every one of them,
// any strategy, any game; `g_octogen_decisions` narrows to actions applied
// by a seat whose strategy_key is octogen's STRAT_* brain id (g_octogen_strat,
// resolved once in main() from the roster entry's OWN `.strat` field — NOT
// bot_roster_find's roster-array index; see h_meta's add-bot branch for why
// those two are different numbers and why only `.strat` is the kernel's
// seat-kind convention — read-only from every thread after main() sets it,
// so no lock is needed to read it from bot_thread). Plain atomics: relaxed
// is enough, same reasoning as g_seq above — nothing else about these
// counters needs ordering with any other memory access.
static atomic_ulong g_bot_decisions = 0;
static atomic_ulong g_octogen_decisions = 0;
// Production hygiene: a live-connection gauge (admission control + observability),
// a moves-applied counter (throughput observability), and the connection ceiling.
static atomic_int   g_live_conns = 0;      // currently-open epoll connections
static atomic_ulong g_moves_applied = 0;   // total client moves the kernel accepted
static int          g_max_conns = 0;       // 0 = unlimited; --max-conns=N sheds NEW connections past N (OOM guard)
static int g_octogen_strat = -1;

// --------------------------------------------------------------------------
// STAGE 3: OpenSSL TLS (WSS/HTTPS) — DONE. Every plain-HTTP socket byte in
// this file funnels through io_read/io_write below, now thin wrappers over
// conn_read/conn_write (conn.c/conn.h), which dispatch to read()/write() or
// SSL_read()/SSL_write() depending on this connection's Conn (a plain `int
// fd` or a per-connection `SSL*` — see conn.h). ws.c's ws_read_full/
// ws_write_full/ws_fill are the equivalent, already-updated seam for the
// WebSocket path (see ws.c's header comment), and every handler in this
// file now takes a `Conn *` instead of a bare `int fd` (respond/
// respond_bin, h_signup/h_create/h_meta/h_action/h_state/h_status, route,
// worker_thread's WorkItem, ws_conn_thread's WsSpawnArg) — nothing above
// this layer inspects a raw fd directly anymore. See TLS.md for the design
// writeup, how to run with certs, and the measured overhead. The one
// non-uniform spot the seam comment (and SERVER_SCALING.md/DURABILITY.md's
// "Seams left" sections) called out ahead of time: ws_send_frame's unmasked
// server path used writev() to send a header+payload in one syscall;
// OpenSSL has no vector write, so ws.c's TLS branch concatenates into one
// buffer instead (see its comment) — everything else was a drop-in swap,
// exactly as predicted.
// --------------------------------------------------------------------------
static ssize_t io_read(Conn *c, void *buf, size_t n)  { return conn_read(c, buf, n); }
static ssize_t io_write(Conn *c, const void *buf, size_t n) { return conn_write(c, buf, n); }

// --------------------------------------------------------------------------
// Small utilities
// --------------------------------------------------------------------------

// POSIX doesn't guarantee libc rand() is thread-safe (glibc's implementation
// shares unlocked global state across callers), and every worker-pool size
// in this file (game/meta/create) is now runtime-configurable — see "Work-
// queue thread routing" below — so no call site here gets to assume it's the
// only thread reaching it. next_rand() gives every thread its own rand_r
// state instead: seeded once per thread from the clock + thread id, no lock
// needed because nothing is shared.
static _Thread_local unsigned int t_rand_seed;
static _Thread_local bool t_rand_seeded = false;
static unsigned int next_rand(void) {
    if (!t_rand_seeded) {
        t_rand_seed = (unsigned int)((uintptr_t)pthread_self() ^ (uintptr_t)time(NULL) ^ (uintptr_t)getpid());
        t_rand_seeded = true;
    }
    return (unsigned int)rand_r(&t_rand_seed);
}

static void gen_id(char *out, int n) {
    static const char hex[] = "0123456789abcdef";
    unsigned long v = (atomic_fetch_add_explicit(&g_seq, 1, memory_order_relaxed) + 1)
                       ^ ((unsigned long)next_rand() << 8) ^ (unsigned long)time(NULL);
    for (int i = 0; i < n; i++) { out[i] = hex[v & 0xf]; v = v * 6364136223846793005UL + 1442695040888963407UL; v >>= 3; }
    out[n] = 0;
}

// Minimal JSON scrapers — enough for our flat request bodies. Not a parser.
static bool json_str(const char *body, const char *key, char *out, int cap) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = body ? strstr(body, pat) : NULL;
    if (!p) return false;
    p = strchr(p + strlen(pat), ':'); if (!p) return false;
    p++; while (*p == ' ' || *p == '"') p++;
    int i = 0; while (*p && *p != '"' && *p != ',' && *p != '}' && i < cap - 1) out[i++] = *p++;
    out[i] = 0; return i > 0;
}

// --------------------------------------------------------------------------
// token -> User* / game_id -> GameSlot* : fixed-size open-addressing hash
// maps (PROFILE_HOTPATH.md T1 report item 2 — these two were O(MAX_USERS)/
// O(MAX_GAMES) linear strcmp scans on EVERY authenticated request, 1.08M
// strcmp calls in one 20s hammer run). Both g_users/g_games are fixed
// static arrays whose elements never move or get freed for the life of the
// process (a POC store — no delete), so raw pointers into them are safe to
// cache here forever; the table only ever grows (insert-only, sized well
// below 1.0 load factor for MAX_USERS/MAX_GAMES, so no growth/tombstone
// logic is needed). A stale slot (e.g. after h_signup mints a fresh token
// for an existing username, orphaning the old token's slot) is harmless: the
// final `strcmp` against the LIVE field still rejects it. Both tables are
// guarded by g_registry_lock (see "Locking" above).
#define TOKEN_HT_SIZE 1048576   // power of two, > 2x MAX_USERS (open-addressing needs load factor < 1 or inserts loop; kept at 2x the ceiling so the table never fills)
#define GAME_HT_SIZE   524288   // power of two, > 2x MAX_GAMES

static User     *g_token_ht[TOKEN_HT_SIZE];
static User     *g_username_ht[TOKEN_HT_SIZE];   // username -> User, so signup dedup is O(1) instead of an O(users) scan
static GameSlot *g_game_ht[GAME_HT_SIZE];

static unsigned long hash_str(const char *s) {
    unsigned long h = 1469598103934665603UL;   // FNV-1a, 64-bit offset basis
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211UL; }
    return h;
}

// Inserts are probe-BOUNDED: the tables are sized to 2x the ceilings so they
// never actually fill, but a bounded loop means a mis-sized table degrades to
// "entry not indexed" (unreachable by id) rather than an infinite loop that
// would hang the whole server. Returns true on insert.
static bool token_ht_insert(User *u) {
    unsigned long h = hash_str(u->token) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        if (!g_token_ht[h]) { g_token_ht[h] = u; return true; }
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    fprintf(stderr, "token hash full (%d) — raise TOKEN_HT_SIZE\n", TOKEN_HT_SIZE);
    return false;
}
static bool game_ht_insert(GameSlot *s) {
    unsigned long h = hash_str(s->id) & (GAME_HT_SIZE - 1);
    for (int probes = 0; probes < GAME_HT_SIZE; probes++) {
        if (!g_game_ht[h]) { g_game_ht[h] = s; return true; }
        h = (h + 1) & (GAME_HT_SIZE - 1);
    }
    fprintf(stderr, "game hash full (%d) — raise GAME_HT_SIZE\n", GAME_HT_SIZE);
    return false;
}

static bool username_ht_insert(User *u) {
    unsigned long h = hash_str(u->username) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        if (!g_username_ht[h]) { g_username_ht[h] = u; return true; }
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    return false;
}
static User *user_by_username(const char *name) {
    if (!name || !*name) return NULL;
    unsigned long h = hash_str(name) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        User *u = g_username_ht[h];
        if (!u) return NULL;
        if (u->used && strcmp(u->username, name) == 0) return u;
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    return NULL;
}

// Both lookups below: caller MUST hold g_registry_lock.
static User *user_by_token(const char *token) {
    if (!token || !*token) return NULL;
    unsigned long h = hash_str(token) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        User *u = g_token_ht[h];
        if (!u) return NULL;   // insert-only table: an empty slot ends the probe chain
        if (u->used && strcmp(u->token, token) == 0) return u;
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    return NULL;
}
static GameSlot *game_by_id(const char *id) {
    if (!id || !*id) return NULL;
    unsigned long h = hash_str(id) & (GAME_HT_SIZE - 1);
    for (int probes = 0; probes < GAME_HT_SIZE; probes++) {
        GameSlot *s = g_game_ht[h];
        if (!s) return NULL;
        if (s->used && strcmp(s->id, id) == 0) return s;
        h = (h + 1) & (GAME_HT_SIZE - 1);
    }
    return NULL;
}
// Whether a seat is a bot: the kernel's own per-seat fact now (strategy_key),
// not a server-side is_ai array. A human seat is STRATEGY_KEY_HUMAN; a bot holds
// its roster index. Caller MUST hold `s`'s own lock (reads s->game).
static bool seat_is_bot(const Game *g, int i) { return g->players[i].strategy_key != STRATEGY_KEY_HUMAN; }

// Caller MUST hold `s`'s own lock (reads s->game and s->seat_user).
static int seat_of(GameSlot *s, const char *user_id) {
    for (int i = 0; i < s->game.num_players; i++)
        if (!seat_is_bot(&s->game, i) && strcmp(s->seat_user[i], user_id) == 0) return i;
    return -1;
}

// The bot game-loop, one thread per game — a TRAMPOLINE, not a blocking hook.
// Each pass drives exactly ONE kernel cycle and RETURNS from the kernel; the
// server then decides how to wait. Same split as supabase (which `await`s a
// setTimeout) and the phone (Task.sleep): the KERNEL owns the cycle and the
// delay value (bot_drive + bot_pacing_ms); the host owns how it waits. The
// `Game` struct IS the continuation, so "resume" is just the next bot_drive.
//
// The lock is `s->lock` — this game's own, not a process-wide one (T2a) — held
// while touching the game and RELEASED during the pacing sleep, so bots think
// + throw in over time while other requests for OTHER games (and, thanks to
// the per-game lock, even other requests for THIS game between cycles) keep
// serving. When no bot can act (a human is owed) the thread waits on `cond`
// until /action or /ws signals it — pairing the wait with `s->lock`, same as
// every other access to this slot.
// glibc (2.39 on this box) brackets every cancellable syscall wrapper this
// server uses in its hot loops — read / write / writev / epoll_wait / accept —
// with an __pthread_enable_asynccancel / __pthread_disable_asynccancel pair, an
// atomic CAS on the thread's cancelhandling word before AND after each call.
// This server NEVER calls pthread_cancel (grep the tree: zero hits — threads
// exit on their own when a game ends / the process stops), so that bracket is
// pure overhead — it was ~4% of instructions on the epoll build and ~11% on the
// thread-per-connection (--tls) build (PROFILE_HOTPATH.md). Setting the cancel
// TYPE to asynchronous makes __pthread_enable_asynccancel find the type bit
// already set and take its no-CAS early break (and __pthread_disable_asynccancel
// early-return); DISABLE-ing the state as well means that even if a
// pthread_cancel were ever introduced it could not asynchronously tear a thread
// down mid-syscall — with nothing cancelling, the async type is inert. Setting
// both collapses the bracket regardless of which mechanism this libc gates it
// on. Call once at each thread's entry (the setting is per-thread).
// INVARIANT: do not introduce pthread_cancel without revisiting this.
static void thread_disable_cancellation(void) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
}

static void *bot_thread(void *arg) {
    thread_disable_cancellation();
    GameSlot *s = arg;
    pthread_mutex_lock(&s->lock);
    while (s->used && s->game.status == GAME_STATUS_PLAYING) {
        uint32_t hmask = game_human_mask(&s->game);   // pure per-Game field read
        BotDriveOut drv;
        // No g_kernel_lock (Stage 5, see "Locking" above): bot_drive's
        // scratch state is now _Thread_local, so this game's bot_thread
        // never shares it with another game's — s->lock (already held for
        // this whole cycle) is the only serialization this needs.
        bot_drive(&s->game, hmask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &drv);   // ONE cycle, then returns
        // A bot's move (or the game ending) changes the board exactly like a
        // human's /action does — the /ws state cache must not stay stale
        // just because no HTTP handler touched this slot this time.
        if (drv.n > 0 || drv.ended >= 0) {
            s->version++; game_mark_dirty(s);
            // Stage 6: the one cross-thread epoll seam — see
            // epoll_notify_game_changed's own doc (near the epoll worker
            // machinery, end of file) for why this specific call site is it.
            epoll_notify_game_changed(s);
        }
        // Stage 4 instrumentation (see g_bot_decisions/g_octogen_decisions'
        // doc above): count every action this cycle actually applied, and —
        // since drv.actions[] names the acting seat — how many of those
        // were an "octogen" seat specifically. Read under s->lock, which we
        // already hold for the whole cycle, so s->game.players[] is stable.
        if (drv.n > 0) {
            atomic_fetch_add_explicit(&g_bot_decisions, (unsigned long)drv.n, memory_order_relaxed);
            if (g_octogen_strat >= 0) {
                unsigned long oct = 0;
                for (int i = 0; i < drv.n; i++) {
                    int aseat = drv.actions[i].seat;
                    if (aseat >= 0 && aseat < s->game.num_players &&
                        s->game.players[aseat].strategy_key == g_octogen_strat)
                        oct++;
                }
                if (oct > 0) atomic_fetch_add_explicit(&g_octogen_decisions, oct, memory_order_relaxed);
            }
        }

        if (drv.ended >= 0) break;   // the kernel already flipped g->status to GAME_OVER
        if (drv.stop == BOT_STOP_NO_ELIGIBLE) {              // a human's move is owed
            pthread_cond_wait(&s->cond, &s->lock);            // sleep until /action or /ws wakes us
            continue;
        }

        // A visible cycle landed — the kernel prices the wait; the host owns the
        // loop and the sleep (the trampoline). Lock released while we wait.
        int delay = bot_cycle_delay_ms(&s->game, hmask, &drv);
        if (delay > 0) {
            pthread_mutex_unlock(&s->lock);
            usleep((useconds_t)delay * 1000);
            pthread_mutex_lock(&s->lock);
        }
    }
    s->bot_running = false;
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

// Spawn the game-loop for a freshly dealt game (idempotent). Caller MUST
// hold s->lock: bot_thread's very first action is to lock it too, so this
// just races the parent's own unlock (harmless — see bot_thread's doc).
static void start_bot_loop(GameSlot *s) {
    if (s->bot_running) return;
    // No bot seats -> no bot thread. An all-human game would otherwise spawn a
    // bot_thread that immediately blocks on `cond` forever with nothing to
    // drive. That's normally harmless, but under fast game churn (push-only
    // completes games in a fraction of a second, so a load run does hundreds of
    // /meta-start rematches) the per-spawn cost dominates: creating a thread
    // zeroes its whole static-TLS block, and THIS build's TLS is large — it
    // embeds the 64 KiB _Thread_local scratch buffers in worker_push_stale and
    // ws_send_frame. That TLS memset (_dl_allocate_tls) was ~18% of all
    // instructions in the assembly profile (PROFILE_HOTPATH.md). Skipping the
    // spawn when every seat is human removes it entirely; real games with a bot
    // still get exactly one bot_thread for their (long) lifetime.
    uint32_t human = game_human_mask(&s->game);
    uint32_t all   = (s->game.num_players >= 32) ? 0xffffffffu
                                                 : ((1u << s->game.num_players) - 1u);
    if ((human & all) == all) return;   // every seated player is human — nothing to drive
    s->bot_running = true;
    pthread_t t;
    if (pthread_create(&t, NULL, bot_thread, s) == 0) pthread_detach(t);
    else s->bot_running = false;
}

// --------------------------------------------------------------------------
// STAGE 2: persist.c callbacks. Snapshot functions run ONLY on the
// persistence thread (see persist.h's PersistSnapshotFn doc) — each takes
// its OWN short-held domain lock just long enough to copy the live data,
// releases it, THEN does the (slower, structured) serialize work with no
// lock held at all: exactly the "briefly take the lock, memcpy, release,
// serialize unlocked" split the design calls for, so a slow disk never
// makes a request thread wait on a game (or the registry) lock.
//
// Load functions run ONLY during persist_start's synchronous crash-recovery
// pass, before any other thread in the process exists (main() calls
// persist_start before spawning the worker pools or starting the accept
// loop) — so they touch g_users[]/g_games[]/the hash maps directly; the
// lock/unlock pairs below are cheap hygiene (uncontended, nothing else is
// running yet), not a correctness requirement at that specific moment.
// --------------------------------------------------------------------------

static int game_persist_snapshot(int idx, char *out_id, int id_cap, unsigned char *buf, int cap) {
    GameSlot *s = game_slot_at(idx);
    if (!s) return -1;
    // static: this whole engine has exactly one persistence thread (see
    // persist.c), so a reused static scratch avoids a ~sizeof(GameSlot)
    // (tens of KB — see game.h's Game size notes) stack frame or a
    // malloc/free every drain cycle.
    static GameSlot snap;
    // Helgrind (run over a --db=... load, per DURABILITY.md's race-check)
    // caught a real bug in an earlier version of this function: copying
    // `sizeof(GameSlot)` bytes — which includes s->lock/s->cond THEMSELVES,
    // not just the game data they guard — races with any other thread's
    // pthread_mutex_lock/unlock on this SAME `s->lock` while we hold it:
    // POSIX does not guarantee a live pthread_mutex_t/pthread_cond_t's raw
    // bytes are safe to read concurrently with normal lock/unlock traffic,
    // even from the thread currently holding it (an implementation is free
    // to touch its own internal bookkeeping via atomics outside the
    // happens-before edge the lock itself provides — glibc's condvar/mutex
    // internals do exactly that). Fix: copy ONLY the fields serialize_slot
    // actually reads — used/id/game/owner/seat_user/seat_name/seat_ready —
    // which the GameSlot layout above (see its definition) keeps
    // contiguous and entirely BEFORE `lock`, so `offsetof(GameSlot, lock)`
    // bytes is exactly that prefix and never touches the mutex/cond
    // themselves. If GameSlot's field order ever changes, this offsetof
    // still compiles but would silently stop covering the right fields —
    // keep any new serialized field ABOVE `lock` in that struct.
    pthread_mutex_lock(&s->lock);
    bool used = s->used;
    if (used) memcpy(&snap, s, offsetof(GameSlot, lock));
    pthread_mutex_unlock(&s->lock);
    if (!used) return -1;
    snprintf(out_id, (size_t)id_cap, "%s", snap.id);
    return serialize_slot(&snap, buf, cap);
}

static void game_persist_load(const char *id, const unsigned char *blob, int len) {
    (void)id;   // deserialize_slot restores s->id from the blob itself — the authoritative copy serialize_slot signed off on
    int idx = g_games_count;                       // append-only: next free slot == count
    GameSlot *s = (idx < MAX_GAMES) ? game_slot_ensure(idx) : NULL;
    if (!s) { fprintf(stderr, "persist: recovery dropped a game row — no free slot\n"); return; }
    g_games_count++;
    memset(s, 0, sizeof *s);
    s->slot_idx = idx;
    if (!deserialize_slot(s, blob, len)) {
        fprintf(stderr, "persist: recovery dropped a corrupt/unreadable game row\n");
        memset(s, 0, sizeof *s);   // leave it fully unused, not half-populated
        return;
    }
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    // Same "must never equal s->version's initial value" reasoning as
    // h_create's identical line — forces the first state_put_cached call
    // for every seat (and the shared spectator slot, index MAX_PLAYERS —
    // Stage 4) to actually recompute instead of serving a bogus zero-length
    // cached view.
    for (int i = 0; i < MAX_PLAYERS + 1; i++) s->view_cache_version[i] = (uint32_t)-1;
    pthread_mutex_lock(&g_registry_lock);
    game_ht_insert(s);
    pthread_mutex_unlock(&g_registry_lock);
    if (s->game.status == GAME_STATUS_PLAYING) {
        // Crash recovery's whole point: a game that was mid-play when the
        // process died resumes paced bot ticks exactly like a freshly
        // dealt one — see start_bot_loop's own doc for the lock contract
        // this follows.
        pthread_mutex_lock(&s->lock);
        start_bot_loop(s);
        pthread_mutex_unlock(&s->lock);
    }
}

static int user_persist_snapshot(int idx, char *out_id, int id_cap, unsigned char *buf, int cap) {
    pthread_mutex_lock(&g_registry_lock);
    User *up = user_slot_at(idx);
    User snap = up ? *up : (User){0};
    pthread_mutex_unlock(&g_registry_lock);
    if (!snap.used) return -1;
    snprintf(out_id, (size_t)id_cap, "%s", snap.user_id);
    return serialize_user(&snap, buf, cap);
}

static void user_persist_load(const char *id, const unsigned char *blob, int len) {
    (void)id;
    int idx = g_users_count;                       // append-only
    User *u = (idx < MAX_USERS) ? user_slot_ensure(idx) : NULL;
    if (!u) { fprintf(stderr, "persist: recovery dropped a user row — no free slot\n"); return; }
    g_users_count++;
    u->slot_idx = idx;
    if (!deserialize_user(u, blob, len)) {
        fprintf(stderr, "persist: recovery dropped a corrupt/unreadable user row\n");
        memset(u, 0, sizeof *u);
        return;
    }
    pthread_mutex_lock(&g_registry_lock);
    token_ht_insert(u);
    username_ht_insert(u);
    pthread_mutex_unlock(&g_registry_lock);
}

// --------------------------------------------------------------------------
// HTTP layer (hand-rolled; swap for mongoose in a real deployment)
// --------------------------------------------------------------------------

typedef struct {
    char method[8];
    char path[256];
    char query[256];
    char token[64];
    char *body; int body_len;
    int  content_length;    // parsed off Content-Length: while walking the headers
    bool is_ws_upgrade;     // GET with Upgrade: websocket + Connection: Upgrade + a key
    char ws_key[64];        // Sec-WebSocket-Key, verbatim (base64, ~24 chars)
} Req;

// Finds the blank line ending the header block ("\r\n\r\n") by a plain byte
// scan instead of strstr (PROFILE_HOTPATH.md T1 report item 3: strstr's
// two-way-search internals showed up as real cost even for this 4-byte
// literal). Returns a pointer just PAST the blank line, or NULL if not seen
// yet in the bytes read so far.
static char *find_headers_end(char *buf, int n) {
    for (int i = 0; i + 3 < n; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i + 4;
    return NULL;
}

// Hand-rolled request-line + header parser — replaces both
// `sscanf(buf, "%7s %255s", method, path)` (2.1% of all instructions under
// load, __vfscanf_internal) and the `strcasestr` header scan (content-length:
// + authorization:, another ~3% combined) with one single pass over the
// header block using strchr-style manual scanning and fixed-width
// `strncasecmp` on each line's already-located header name. Same semantics:
// method/path/query split on the request line, Content-Length, a Bearer
// token, and (new) whatever the WebSocket upgrade needs.
static void parse_request_line_and_headers(char *buf, char *hdr_end, Req *r) {
    memset(r, 0, sizeof *r);
    char *p = buf;

    char *s = p;
    while (p < hdr_end && *p != ' ' && *p != '\r' && *p != '\n') p++;
    int len = (int)(p - s); if (len > (int)sizeof r->method - 1) len = (int)sizeof r->method - 1;
    memcpy(r->method, s, (size_t)len); r->method[len] = 0;
    if (p < hdr_end && *p == ' ') p++;

    s = p;
    while (p < hdr_end && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n') p++;
    len = (int)(p - s); if (len > (int)sizeof r->path - 1) len = (int)sizeof r->path - 1;
    memcpy(r->path, s, (size_t)len); r->path[len] = 0;
    if (p < hdr_end && *p == '?') {
        p++; s = p;
        while (p < hdr_end && *p != ' ' && *p != '\r' && *p != '\n') p++;
        len = (int)(p - s); if (len > (int)sizeof r->query - 1) len = (int)sizeof r->query - 1;
        memcpy(r->query, s, (size_t)len); r->query[len] = 0;
    }
    // Skip the rest of the request line (HTTP version) up to its CRLF.
    while (p < hdr_end && *p != '\n') p++;
    if (p < hdr_end) p++;

    bool saw_upgrade_websocket = false, saw_connection_upgrade = false;
    while (p < hdr_end) {
        char *line_start = p;
        char *colon = NULL;
        while (p < hdr_end && *p != '\r' && *p != '\n') { if (!colon && *p == ':') colon = p; p++; }
        char *line_end = p;
        if (p < hdr_end && *p == '\r') p++;
        if (p < hdr_end && *p == '\n') p++;
        if (!colon || line_start == colon) continue;   // blank/malformed line — skip

        int name_len = (int)(colon - line_start);
        const char *val = colon + 1;
        while (val < line_end && (*val == ' ' || *val == '\t')) val++;
        int val_len = (int)(line_end - val);

        if (name_len == 14 && strncasecmp(line_start, "content-length", 14) == 0) {
            int cl = 0;
            for (const char *q = val; q < line_end && *q >= '0' && *q <= '9'; q++) cl = cl * 10 + (*q - '0');
            r->content_length = cl;
        } else if (name_len == 13 && strncasecmp(line_start, "authorization", 13) == 0) {
            if (val_len > 7 && strncasecmp(val, "Bearer ", 7) == 0) {
                const char *tok = val + 7;
                int tlen = val_len - 7; if (tlen > (int)sizeof r->token - 1) tlen = (int)sizeof r->token - 1;
                memcpy(r->token, tok, (size_t)tlen); r->token[tlen] = 0;
            }
        } else if (name_len == 7 && strncasecmp(line_start, "upgrade", 7) == 0) {
            for (int i = 0; i + 9 <= val_len; i++)
                if (strncasecmp(val + i, "websocket", 9) == 0) { saw_upgrade_websocket = true; break; }
        } else if (name_len == 10 && strncasecmp(line_start, "connection", 10) == 0) {
            for (int i = 0; i + 7 <= val_len; i++)
                if (strncasecmp(val + i, "upgrade", 7) == 0) { saw_connection_upgrade = true; break; }
        } else if (name_len == 17 && strncasecmp(line_start, "sec-websocket-key", 17) == 0) {
            int klen = val_len; if (klen > (int)sizeof r->ws_key - 1) klen = (int)sizeof r->ws_key - 1;
            memcpy(r->ws_key, val, (size_t)klen); r->ws_key[klen] = 0;
        }
    }
    r->is_ws_upgrade = saw_upgrade_websocket && saw_connection_upgrade && r->ws_key[0];
}

// Reads a full HTTP request (headers, then body up to Content-Length) off
// `conn` into caller-owned `buf` (>= cap bytes), and parses it into `r`. `r->
// body`/`r->body_len` end up pointing INTO `buf` — the caller must keep
// `buf` alive for as long as `r` is used (see WorkItem/WsSpawnArg below,
// which both carry the buffer alongside the parsed Req for exactly this
// reason). Returns false on a malformed/empty request (caller should just
// close the conn and free buf).
#define REQ_BUF_CAP (1 << 16)
static bool read_and_parse_request(Conn *conn, char *buf, int cap, Req *r) {
    int total = 0, n;
    char *hdr_end = NULL;
    while ((n = (int)io_read(conn, buf + total, (size_t)(cap - 1 - total))) > 0) {
        total += n; buf[total] = 0;
        hdr_end = find_headers_end(buf, total);
        if (!hdr_end) continue;
        break;
    }
    if (!hdr_end) return false;   // malformed / empty request

    parse_request_line_and_headers(buf, hdr_end, r);
    // Keep reading if the body (by Content-Length) hasn't fully arrived yet.
    int have = total - (int)(hdr_end - buf);
    while (have < r->content_length && (n = (int)io_read(conn, buf + total, (size_t)(cap - 1 - total))) > 0) {
        total += n; buf[total] = 0;
        have = total - (int)(hdr_end - buf);
    }
    r->body = hdr_end;
    r->body_len = total - (int)(hdr_end - buf);   // real byte count (body may be binary awire)
    return true;
}

static void respond(Conn *conn, int code, const char *json) {
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request"
                    : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : "Error";
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        code, msg, strlen(json));
    io_write(conn, hdr, n);
    io_write(conn, json, strlen(json));
}

// Raw bytes (the packed kernel wire) — no JSON. The client decodes with its own
// kernel-wire reader (MaskedView etc.).
static void respond_bin(Conn *conn, int code, const unsigned char *data, int len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d OK\r\nContent-Type: application/octet-stream\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        code, len);
    io_write(conn, hdr, n);
    if (len > 0) io_write(conn, data, (size_t)len);
}

// --------------------------------------------------------------------------
// Route handlers. Each follows the SAME registry->game lock handoff (see
// "Locking" above): take g_registry_lock, find/allocate the User*/GameSlot*
// (copying out any User field it still needs as a local — the User itself
// is only safe to dereference while g_registry_lock is held), take the
// GameSlot's own `lock`, release g_registry_lock, do the game work, release
// the GameSlot lock, THEN respond (I/O never happens with either lock held).
// --------------------------------------------------------------------------

static void h_signup(Req *r, Conn *conn) {
    char uname[24] = {0};
    if (!json_str(r->body, "username", uname, sizeof uname)) { respond(conn, 400, "{\"error\":\"username\"}"); return; }
    pthread_mutex_lock(&g_registry_lock);
    // Dedup by username via the O(1) hash (was an O(users) linear scan).
    User *u = user_by_username(uname);
    if (!u) {
        int idx = g_users_count;
        u = (idx < MAX_USERS) ? user_slot_ensure(idx) : NULL;
        if (u) { g_users_count++; u->slot_idx = idx; u->used = true;
                 snprintf(u->username, sizeof u->username, "%s", uname); gen_id(u->user_id, ID_LEN);
                 username_ht_insert(u); }
    }
    // Fresh session token, indexed, and marked dirty for the persistence
    // thread (Stage 2) — a signup/signin the DB never learns about would
    // strand that user's token past a crash, same durability need as a
    // game's state (see game_mark_dirty's doc for the write-behind model).
    if (u) {
        gen_id(u->token, 32);
        token_ht_insert(u);
        if (g_user_table) persist_mark_dirty(g_user_table, u->slot_idx);
    }
    char out[160];
    if (u) snprintf(out, sizeof out, "{\"token\":\"%s\",\"user_id\":\"%s\",\"username\":\"%s\"}", u->token, u->user_id, u->username);
    pthread_mutex_unlock(&g_registry_lock);
    if (u) respond(conn, 200, out); else respond(conn, 400, "{\"error\":\"full\"}");
}

static void h_create(Req *r, Conn *conn) {
    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    if (!u) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 401, "{\"error\":\"auth\"}"); return; }
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    char username[24]; snprintf(username, sizeof username, "%s", u->username);

    int gidx = g_games_count;                       // append-only: next free slot == count
    GameSlot *s = (gidx < MAX_GAMES) ? game_slot_ensure(gidx) : NULL;
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 400, "{\"error\":\"full\"}"); return; }
    g_games_count++;
    memset(s, 0, sizeof *s);
    s->slot_idx = gidx;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->used = true;
    // s->version starts at 0 (memset); view_cache_version must start at a
    // value that can NEVER equal it, or state_put_cached's very first call
    // for a seat would see version==cache_version (both zeroed) and return
    // the also-zeroed, never-computed view_cache_len (0 bytes) instead of
    // actually serializing — a silent "client gets an empty state" bug. The
    // +1 covers the shared spectator cache slot too (index MAX_PLAYERS —
    // Stage 4, SPECTATOR_CACHE_IDX).
    for (int i = 0; i < MAX_PLAYERS + 1; i++) s->view_cache_version[i] = (uint32_t)-1;
    gen_id(s->id, ID_LEN);
    // LOCK ORDER: registry, then this fresh slot's own lock — never the
    // reverse (see g_registry_lock's declaration). No other thread can find
    // this slot before game_ht_insert runs, so taking s->lock here is
    // uncontended; it's only here for symmetry with every other handler's
    // registry->game handoff, so nothing outside this function ever touches
    // a GameSlot without its lock held.
    pthread_mutex_lock(&s->lock);
    game_ht_insert(s);
    pthread_mutex_unlock(&g_registry_lock);   // registry work done; the rest is s->lock-only

    snprintf(s->owner, sizeof s->owner, "%s", user_id);
    // Seat 0 = creator. Identity lives here; the kernel state is dealt at start.
    Game *g = &s->game; g->num_players = 1; g->status = GAME_STATUS_WAITING;
    snprintf(s->seat_user[0], ID_LEN + 1, "%s", user_id);
    snprintf(s->seat_name[0], 24, "%s", username);
    snprintf(g->players[0].name, 24, "%s", username);
    snprintf(g->players[0].player_id, 24, "%s", user_id);
    g->players[0].status = PLAYER_STATUS_IDLE;
    g->players[0].strategy_key = STRATEGY_KEY_HUMAN;   // the creator is a human seat
    game_mark_dirty(s);
    char out[80]; snprintf(out, sizeof out, "{\"game_id\":\"%s\"}", s->id);
    pthread_mutex_unlock(&s->lock);
    respond(conn, 200, out);
}

static void h_meta(Req *r, Conn *conn) {
    char type[16] = {0}, gid[ID_LEN + 1] = {0};
    json_str(r->body, "type", type, sizeof type);
    json_str(r->body, "game_id", gid, sizeof gid);

    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 401, "{\"error\":\"auth\"}"); return; }
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 404, "{\"error\":\"no game\"}"); return; }
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    char username[24]; snprintf(username, sizeof username, "%s", u->username);
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);

    Game *g = &s->game;
    if (!strcmp(type, "join")) {
        if (seat_of(s, user_id) < 0 && g->num_players < MAX_PLAYERS && g->status == GAME_STATUS_WAITING) {
            int i = g->num_players++;
            snprintf(s->seat_user[i], ID_LEN + 1, "%s", user_id);
            snprintf(s->seat_name[i], 24, "%s", username);
            snprintf(g->players[i].name, 24, "%s", username);
            snprintf(g->players[i].player_id, 24, "%s", user_id);
            g->players[i].status = PLAYER_STATUS_IDLE;
            g->players[i].strategy_key = STRATEGY_KEY_HUMAN;   // a human seat
        }
    } else if (!strcmp(type, "add-bot")) {
        char skey[24] = {0}; if (!json_str(r->body, "strategy", skey, sizeof skey)) snprintf(skey, sizeof skey, "random");
        if (g->num_players < MAX_PLAYERS && g->status == GAME_STATUS_WAITING) {
            int i = g->num_players++;
            s->seat_ready[i] = true;
            int ridx = bot_roster_find(skey);
            if (ridx < 0) ridx = bot_roster_find("random");
            const BotRosterEntry *entry = bot_roster_at(ridx);
            snprintf(s->seat_name[i], 24, "%%%s %d", skey, i);
            snprintf(g->players[i].name, 24, "%s", s->seat_name[i]);
            snprintf(g->players[i].player_id, 24, "bot%d", i);
            g->players[i].status = PLAYER_STATUS_READY;
            // Stage 4 fix: the kernel's own seat-kind convention is a
            // STRAT_* brain id (strategy.h), NOT a bot_roster.h array
            // index — bot_drive.c's choose_move says so directly
            // ("Seats carry a STRAT_* id by kernel-wide convention... the
            // roster entry is resolved back from the brain" via
            // bot_roster_find_by_strat). This used to store `ridx` (the
            // roster INDEX bot_roster_find returns) here directly, which
            // bot_drive.c then read back AS IF it were a STRAT_* id —
            // silently either matching no roster entry at all (that seat's
            // bot_drive_eligible check always fails: BOT_STOP_NO_ELIGIBLE
            // forever, a game with a bot in it that never moves — confirmed
            // by a standalone repro for "octogen": bot_roster_find_by_strat
            // (its roster index, 9) resolves to -1, since no roster entry's
            // own `.strat` happens to be 9) or, for a few names, matching a
            // DIFFERENT roster entry's `.strat` and silently running THAT
            // bot's brain instead (e.g. "gunpowder"'s index (6) equals
            // "blackpowder"'s STRAT_BLACKPOWDER (6) — a gunpowder seat would
            // have silently played blackpowder). The roster entry's OWN
            // `.strat` field is the actual kernel brain id; use it.
            g->players[i].strategy_key = entry ? (int8_t)entry->strat : (int8_t)STRAT_RANDOM;
        }
    } else if (!strcmp(type, "start")) {
        int me = seat_of(s, user_id);
        if (me >= 0) { s->seat_ready[me] = true; g->players[me].status = PLAYER_STATUS_READY; }
        // Deal once every seated human is ready (bots are always ready) and 2+ seated.
        bool all = g->num_players >= 2;
        for (int i = 0; i < g->num_players; i++) if (!seat_is_bot(g, i) && !s->seat_ready[i]) all = false;
        if (all && g->status == GAME_STATUS_WAITING) {
            unsigned char seed[32]; for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(next_rand() ^ (i * 131 + (int)g_seq));
            // No g_kernel_lock (Stage 5, see "Locking" above): the deal RNG
            // state game_set_deal_seed_bytes/game_seat_and_deal touch is
            // _Thread_local, and every other kernel static reachable from
            // game_seat_and_deal's apply/refill path is too — s->lock
            // (already held here) is the only serialization this needs.
            game_set_deal_seed_bytes(seed, 32);
            // Seats were wired in the lobby (strategy_key per seat), so pass NULL:
            // the kernel owns marking them + the deal (+ g->status = PLAYING).
            game_seat_and_deal(g, NULL, g->num_players);
            start_bot_loop(s);             // the game-loop paces bot play from here
        }
    } else if (!strcmp(type, "continue")) {
        // Reset the kernel to the lobby for a rematch (identities kept, re-dealt
        // on start). The host owns only this lobby transition; game-over was the
        // kernel's to declare.
        g->status = GAME_STATUS_WAITING;
        for (int i = 0; i < g->num_players; i++) {
            bool ai = seat_is_bot(g, i);
            s->seat_ready[i] = ai;
            g->players[i].status = ai ? PLAYER_STATUS_READY : PLAYER_STATUS_IDLE;
        }
    }
    // Every branch above either mutates the roster/lobby state or is a no-op
    // (an already-seated join, a re-ready), and bumping on a no-op is
    // harmless (worst case: one extra state_put on the next /ws poll) — see
    // GameSlot.version's doc. Unconditional beats re-deriving "did this
    // branch actually change anything" per-branch for a rarely-called path.
    s->version++;
    game_mark_dirty(s);
    // PROFILE_HOTPATH.md "T1f" (push-only protocol): a lobby transition
    // (join/add-bot/start-and-deal/continue) changes the board exactly like
    // a human move or a bot decision does, and any live /ws connection for
    // this game — most importantly a seat's own connection sitting blocked
    // on its next push right after THIS SAME client issued the /meta
    // continue+start rematch pair over HTTP — needs to hear about it
    // without polling. Same cross-thread wakeup bot_thread already uses at
    // its own version-bump site (see epoll_notify_game_changed's doc); a
    // no-op under --tls (thread-per-connection fallback, no epoll worker to
    // wake — see that doc's g_epoll_active guard). Before T1f this was
    // masked by every /ws client polling every ~1ms regardless, so a lobby
    // transition was noticed within a poll cycle even with no explicit
    // wakeup; a push-only client has no such fallback.
    epoll_notify_game_changed(s);
    char out[80]; snprintf(out, sizeof out, "{\"game_id\":\"%s\",\"status\":%d}", s->id, g->status);
    pthread_mutex_unlock(&s->lock);
    respond(conn, 200, out);
}

static void h_action(Req *r, Conn *conn) {
    // game_id rides the query string (like /state); the body IS the packed awire
    // frame, so it can be binary. /action?game_id=..
    char gid[ID_LEN + 1] = {0};
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }

    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 401, "{\"error\":\"auth\"}"); return; }
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 400, "{\"error\":\"not playing\"}"); return; }
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);

    if (s->game.status != GAME_STATUS_PLAYING) { pthread_mutex_unlock(&s->lock); respond(conn, 400, "{\"error\":\"not playing\"}"); return; }
    int seat = seat_of(s, user_id);
    if (seat < 0) { pthread_mutex_unlock(&s->lock); respond(conn, 400, "{\"error\":\"not seated\"}"); return; }

    // The body is the same packed move the browser validates and the phone
    // sends: [kind, n, cards, (attacks)]. The kernel decodes and dispatches —
    // the server enumerates no move types (awire_apply owns the switch).
    // awire_decode is stateless (no kernel globals — confirmed by inspection
    // of awire.c). No g_kernel_lock around awire_apply either (Stage 5, see
    // "Locking" above): engine_last_reject and every other kernel static
    // handle_* touches are now _Thread_local, so s->lock (held for this whole
    // handler) is the only serialization awire_apply needs.
    AwireAction a;
    bool decoded = r->body && r->body_len > 0
                   && awire_decode((const unsigned char *)r->body, r->body_len, &a);
    bool ok = false;
    if (decoded) {
        ok = awire_apply(&s->game, seat, &a);
    }
    // Human changed the board — wake the game-loop so bots respond (or notice
    // the game ended; awire_apply already settled g->status). If it's mid-sleep
    // it re-reads the state on its own.
    if (ok) { s->version++; game_mark_dirty(s); pthread_cond_signal(&s->cond); }   // real move -> the cached per-seat views are stale
    char out[96]; snprintf(out, sizeof out, "{\"ok\":%s,\"status\":%d}", ok ? "true" : "false", s->game.status);
    pthread_mutex_unlock(&s->lock);
    respond(conn, ok ? 200 : 400, out);
}

static void h_state(Req *r, Conn *conn) {
    char gid[ID_LEN + 1] = {0}; int seat = 0;
    // query: game_id=..&seat=..
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    const char *sp = strstr(r->query, "seat="); if (sp) seat = (int)strtol(sp + 5, NULL, 10);
    // Never let an unauthenticated /state request reach state_put's trusted
    // VIEW_UNMASKED (-2) serialization, which emits every hand and the deck.
    // The only public views are VIEW_SPECTATOR (-1, all hands masked) and a
    // concrete seat (0..num_players-1); reject anything below spectator so the
    // seat= sentinel can't be spoofed into a full-state disclosure.
    if (seat < VIEW_SPECTATOR) { respond(conn, 400, "{\"error\":\"bad seat\"}"); return; }

    pthread_mutex_lock(&g_registry_lock);
    GameSlot *s = game_by_id(gid);
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 404, "{\"error\":\"no game\"}"); return; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);

    // The kernel renders the masked, per-seat view as the PACKED wire (view.c
    // state_put) — no JSON. The client decodes it with its own kernel-wire
    // reader (Swift MaskedView / the web's TS reader). Kernel-to-kernel.
    // Stack-local, not `static`: this handler runs on many worker threads
    // concurrently, and a shared buffer would let two /state requests
    // corrupt each other's bytes.
    unsigned char buf[65536];
    int n = state_put(&s->game, seat, buf);
    pthread_mutex_unlock(&s->lock);
    respond_bin(conn, 200, buf, n);
}

// A plain status int (0 waiting / 1 playing / 2 over), for smoke tests that used
// to read it off the JSON view. Not json_out — just an integer.
static void h_status(Req *r, Conn *conn) {
    char gid[ID_LEN + 1] = {0};
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }

    pthread_mutex_lock(&g_registry_lock);
    GameSlot *s = game_by_id(gid);
    int st = -1;
    if (s) {
        pthread_mutex_lock(&s->lock);
        pthread_mutex_unlock(&g_registry_lock);
        st = s->game.status;
        pthread_mutex_unlock(&s->lock);
    } else {
        pthread_mutex_unlock(&g_registry_lock);
    }
    char out[16]; snprintf(out, sizeof out, "%d", st);
    respond(conn, 200, out);
}

// Stage 4: a tiny JSON counters dump — no game_id, no auth, process-wide —
// so a load tool (foolish_hammer's --mode=ws summary) or an external script
// (bot_stress.sh) can poll it before/after a timed run and compute
// decisions/sec from the delta. See g_bot_decisions/g_octogen_decisions'
// doc above for exactly what each counts. Answered inline off the
// dispatcher (see main()'s accept loop), same as /health — cheap atomic
// loads, no lock, no reason to pay a work-queue round trip.
static void h_stats(Req *r, Conn *conn) {
    (void)r;
    unsigned long bd = atomic_load_explicit(&g_bot_decisions, memory_order_relaxed);
    unsigned long od = atomic_load_explicit(&g_octogen_decisions, memory_order_relaxed);
    int    conns = atomic_load_explicit(&g_live_conns, memory_order_relaxed);
    unsigned long mv = atomic_load_explicit(&g_moves_applied, memory_order_relaxed);
    // g_games_count/g_users_count are written under g_registry_lock; a relaxed
    // read here is fine for a stats gauge (a slightly-stale count never matters).
    char out[256];
    snprintf(out, sizeof out,
        "{\"live_connections\":%d,\"max_connections\":%d,\"games\":%d,\"users\":%d,"
        "\"moves_applied\":%lu,\"bot_decisions\":%lu,\"octogen_decisions\":%lu}",
        conns, g_max_conns, g_games_count, g_users_count, mv, bd, od);
    respond(conn, 200, out);
}

// Serialize a masked view, reusing the cached bytes from GameSlot when
// nothing has changed since they were computed (PROFILE_HOTPATH.md "T1c" —
// see GameSlot's `version`/`view_cache*` fields above for the invariant).
// `cache_idx` selects WHICH cached slot to use/fill: 0 <= cache_idx <
// num_players <= MAX_PLAYERS for a seated client's own cache, or
// SPECTATOR_CACHE_IDX (Stage 4) for the one shared spectator slot every
// spectator of this game reads. `viewer` is state_put's own viewer argument
// (the seat number, or VIEW_SPECTATOR) — kept separate from `cache_idx`
// because the spectator cache slot's INDEX (MAX_PLAYERS) is not a valid
// state_put viewer value (that's VIEW_SPECTATOR, -1). MUST be called with
// s->lock held (same contract as a bare state_put call here) and
// `cache_idx`/`viewer` MUST already be known valid — ws_conn_thread
// validates both at handshake time before ever calling in (a seated
// client's own seat, or the fixed spectator pair); this function does not
// re-check, so it is not safe to point at an unvalidated/attacker-
// controlled index.
// Like state_put_cached, but returns a POINTER to the serialized view bytes
// instead of copying them into an out buffer — on a cache hit that is zero
// copies (the pointer is the cache slot itself), letting the push path land
// the one unavoidable copy directly in its output buffer via ws_send_frame2
// (PROFILE_HOTPATH.md: memcpy was ~18% of the epoll build, the redundant
// cache->scratch->wbuf double copy). `fallback` is caller-owned scratch
// (>= 1 + 65536) used ONLY on the rare path where a view is too big to cache
// (n > VIEW_CACHE_CAP — never at this build's caps, see VIEW_CACHE_CAP): there
// the bytes are serialized into `fallback` and *pp points at it. Same locking
// contract as state_put_cached (s->lock held); the returned pointer is valid
// only while that lock is still held — a later version bump may recompute the
// slot.
static int state_put_cached_ptr(GameSlot *s, int cache_idx, int viewer,
                                const unsigned char **pp, unsigned char *fallback) {
    if (s->view_cache_version[cache_idx] != s->version) {
        // Serialize straight into the scratch buffer (wider than
        // VIEW_CACHE_CAP) first: state_put's real worst case fits well
        // inside VIEW_CACHE_CAP today (see that constant's comment), but if
        // a future kernel change ever grew a cap enough to overflow it,
        // this falls back to "always recompute, never cache" for that slot
        // instead of truncating a state update — correctness over the
        // optimization.
        int n = state_put(&s->game, viewer, fallback);
        if (n < 0) n = 0;
        if (n <= VIEW_CACHE_CAP) {
            memcpy(s->view_cache[cache_idx], fallback, (size_t)n);
            s->view_cache_len[cache_idx] = n;
            s->view_cache_version[cache_idx] = s->version;
        } else {
            *pp = fallback;
            return n;
        }
    }
    *pp = s->view_cache[cache_idx];
    return s->view_cache_len[cache_idx];
}

// Copy-out wrapper kept for the thread-per-connection (--tls) path, whose
// ws_send_frame concatenates a single contiguous payload. The epoll push path
// uses state_put_cached_ptr + ws_send_frame2 to avoid this copy.
static int state_put_cached(GameSlot *s, int cache_idx, int viewer, unsigned char *out) {
    unsigned char scratch[1 + 65536];
    const unsigned char *p = scratch;
    int n = state_put_cached_ptr(s, cache_idx, viewer, &p, scratch);
    memcpy(out, p, (size_t)n);
    return n;
}

// --------------------------------------------------------------------------
// /ws — the WebSocket hot loop (item 1/2 of PROFILE_HOTPATH.md's "where to
// speed up" list). One handshake, then ONE persistent connection carries
// every move + state push for that seat's whole session — no pthread_create
// per action.
//
// T2a design note (see SERVER_SCALING.md "Deliverable 2"): a persistent /ws
// connection is the one endpoint that does NOT go through the typed
// work-queue pools below — a queue worker that blocked for a connection's
// whole lifetime would defeat the point of a small, fixed worker pool. It
// keeps its own dedicated thread (ws_conn_thread, spawned straight off the
// dispatcher's accept loop), same as before T2a, but now takes THIS game's
// `lock` — not a process-wide one — for every access to shared state, and
// releases it around all socket I/O (handshake, ws_recv_message,
// ws_send_frame). This is design (B) from the task brief: simpler and
// easier to keep Helgrind-clean than moving WS service onto a shared
// epoll-driven worker (design A); the tradeoff is still a thread per live
// WS connection (PROFILE_HOTPATH.md T1c's ~0.9MB/conn memory tax), which
// the epoll design would have removed. See SERVER_SCALING.md for the
// measurements and the reasoning behind picking (B) here.
//
// Handshake: GET /ws?game_id=..&seat=.. (Bearer token), validated exactly
// like /action (must be a real user, a real game, and THIS user's own seat —
// no seat spoofing), then the RFC 6455 upgrade (ws_accept_from_key). After
// the 101 response, the server immediately PUSHES the current masked state
// once (ok=0 — nothing was applied yet, this is just "here's where things
// stand"), then loops:
//   client sends a binary frame -> [awire bytes] apply it, or [] (empty) to
//     just poll (a seat with no eligible move yet still needs to notice
//     when OTHER seats' actions make it eligible — see foolish_hammer.c's ws
//     worker) -> server always answers with one binary frame,
//     [ok:u8][state_put(seat) bytes], ok=1 iff a real move decoded AND the
//     kernel accepted it (awire_apply's own validation — same as /action).
// A human's turn wakes the bot game-loop exactly like /action did.
//
// Stage 4 (SERVER_SCALING.md "Stage 4 — spectators"): GET
// /ws?game_id=..&spectator=1 (Bearer, no `seat=`) upgrades the SAME way but
// owns no seat — `seat_of` ownership is never checked, only that the token
// is a real user and the game exists (the seat-membership check below is
// simply skipped, everything else about the handshake is identical). The
// masked view it receives is VIEW_SPECTATOR (view.h — every hand AND the
// deck hidden, not just the other seats' hands), cached in the ONE shared
// SPECTATOR_CACHE_IDX slot (every spectator of a game sees the same bytes at
// a given version, so they share one cache entry instead of paying MAX_
// PLAYERS+1 distinct recomputes). A spectator MAY NOT submit moves: any
// frame it sends — empty or not — is treated purely as "send me the current
// state" (the `!spectator` guard below keeps a spectator's bytes from ever
// reaching awire_decode/awire_apply at all), so `ok` is always
// 0 in a spectator's replies. Same per-game lock, same dedicated-thread-per-
// connection design (B) as a seated client — a spectator is just another
// long-lived /ws connection that happens to skip the seat check and the
// apply branch.
typedef struct {
    Conn conn;
    Req req;
    char *raw_buf;   // owns the bytes r.body/r.query/etc point into until freed
} WsSpawnArg;

// --------------------------------------------------------------------------
// Shared /ws logic (Stage 6, SERVER_SCALING.md "Stage 6 — epoll-per-shard"):
// three helpers factored OUT of ws_conn_thread's body, byte-for-byte the same
// checks/encodings it always ran, so the new epoll worker path (below) and
// this thread-per-connection path (kept as the --tls fallback — see "STAGE 3:
// TLS listener state" below) can never drift on auth, the handshake wire
// bytes, or move-apply semantics. Splitting them out is a pure refactor: a
// single-shard, single-connection run through ws_conn_thread produces
// IDENTICAL bytes before and after this change.
// --------------------------------------------------------------------------

// Validates a /ws (or /ws?spectator=1) upgrade against the registry + this
// game's roster — same checks this function's callers always made. Returns
// NULL on any auth/seat failure (caller responds 401 and closes); on success
// returns the owning GameSlot* with *out_seat/*out_spectator/*out_cache_idx/
// *out_viewer resolved for the rest of the connection's life (see
// state_put_cached's doc for why cache_idx and viewer are two different
// values for a spectator).
static GameSlot *ws_handshake_validate(Req *r, int *out_seat, bool *out_spectator,
                                        int *out_cache_idx, int *out_viewer) {
    char gid[ID_LEN + 1] = {0}; int seat = -1;
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    const char *sp = strstr(r->query, "seat=");
    if (sp) seat = (int)strtol(sp + 5, NULL, 10);
    bool spectator = false;
    const char *spq = strstr(r->query, "spectator=");
    if (spq) { spq += 10; spectator = (*spq == '1'); }

    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u || !s) { pthread_mutex_unlock(&g_registry_lock); return NULL; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    // Spectators: a real Bearer token is still required (same user_by_token
    // check every /ws client passes — this is "no SEAT membership", not "no
    // auth"), but ownership of a specific seat is not — any authenticated
    // user may watch any EXISTING game. Seated clients are exactly as
    // before: must own the seat they asked for.
    bool ok;
    if (spectator) {
        ok = true;
    } else {
        char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
        ok = seat >= 0 && seat < s->game.num_players && seat_of(s, user_id) == seat;
    }
    pthread_mutex_unlock(&s->lock);
    if (!ok) return NULL;

    *out_seat = seat; *out_spectator = spectator;
    *out_cache_idx = spectator ? SPECTATOR_CACHE_IDX : seat;
    *out_viewer    = spectator ? VIEW_SPECTATOR : seat;
    return s;
}

// Encodes the 101 Switching Protocols response, THEN the immediate
// post-handshake state push (current masked view, ok=0 — "here's where
// things stand", not a move confirmation) into `conn` (a real fd, blocking —
// ws_conn_thread; or a Stage 6 buffered Conn — the epoll dispatcher, see
// conn.h). `accept` is the already-computed Sec-WebSocket-Accept value
// (callers check ws_accept_from_key's own failure mode separately, since the
// two paths respond to a bad key differently — see below). On success fills
// *out_wc (mask_outgoing=0: server frames are never masked) and returns
// true; false means "close the connection, nothing more to send."
static bool ws_send_handshake_and_push(WsConn *out_wc, Conn conn, const char *accept,
                                        GameSlot *s, int cache_idx, int viewer) {
    char resp[256];
    int n = snprintf(resp, sizeof resp,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n <= 0 || conn_write(&conn, resp, (size_t)n) != n) return false;

    ws_conn_init(out_wc, conn, /*mask_outgoing=*/0);
    // [ok:u8][state_put bytes] — sized for state_put's documented worst case
    // (h_state uses the same 65536 cap).
    unsigned char msg[1 + 65536];
    pthread_mutex_lock(&s->lock);
    int slen = s->used ? state_put_cached(s, cache_idx, viewer, msg + 1) : 0;
    pthread_mutex_unlock(&s->lock);
    msg[0] = 0;
    return ws_send_frame(out_wc, WS_OP_BIN, msg, slen + 1) >= 0;
}

// Applies one received WS application message (a real awire move, or an
// empty/rejected/spectator-sent poll) and produces the reply payload — the
// exact per-message body ws_conn_thread's loop always ran. `in`/`mlen` is
// the just-received message (mlen==0 is a plain poll); `msg` is
// caller-owned, >= 1+65536 bytes. Returns the total reply length (>= 1),
// [ok:u8][state_put_cached bytes]. MUST be called with NO lock held — takes
// and releases s->lock itself, exactly once. `out_version` (may be NULL) is
// set to `s->version` as observed under that SAME lock acquisition, right
// before it releases — Stage 6's epoll worker uses this to record exactly
// which version this connection's peer was just brought up to date with,
// without a second, unlocked (racy) read of s->version afterward.
// Decode+apply one client frame with s->lock ALREADY HELD. Returns true iff a
// real move was decoded AND the kernel accepted it (awire_apply) — on
// acceptance it bumps s->version, marks the game dirty, and signals the bot
// game-loop, exactly the mutation ws_service_message used to inline. A
// spectator frame, an empty poll, a not-playing game, or a rejected/illegal
// move returns false and mutates nothing. Factored out so the epoll push path
// (econn_push_view) and the --tls thread-per-conn path (ws_service_message)
// share ONE copy of the apply logic and can never drift.
static bool ws_apply_move_locked(GameSlot *s, int seat, bool spectator,
                                 const unsigned char *in, int mlen) {
    // Spectators MAY NOT submit moves (Stage 4): `!spectator` keeps ANY frame
    // a spectator sends — empty or a well-formed move alike — from ever
    // reaching awire_decode/awire_apply.
    if (!s->used || spectator || mlen <= 0 || s->game.status != GAME_STATUS_PLAYING) return false;
    AwireAction a;
    if (!awire_decode(in, mlen, &a)) return false;
    // No g_kernel_lock (Stage 5, see h_action's identical pattern and the
    // "Locking" doc above) — s->lock, held for this whole call, is enough.
    if (!awire_apply(&s->game, seat, &a)) return false;
    s->version++;   // this seat's move can change every seat's view
    atomic_fetch_add_explicit(&g_moves_applied, 1, memory_order_relaxed);   // /stats throughput gauge
    game_mark_dirty(s);
    pthread_cond_signal(&s->cond);   // same wakeup /action gives the bot game-loop
    return true;
}

static int ws_service_message(GameSlot *s, int seat, bool spectator, int cache_idx, int viewer,
                               const unsigned char *in, int mlen, unsigned char *msg, uint32_t *out_version) {
    int slen;
    pthread_mutex_lock(&s->lock);
    bool applied = ws_apply_move_locked(s, seat, spectator, in, mlen);
    // PROFILE_HOTPATH.md "T1c": on a pure poll (mlen==0 or an illegal/rejected
    // move, or ANY frame from a spectator) this view did NOT change, so
    // state_put_cached memcpy's the bytes computed last time instead of
    // re-running the kernel's full masked serialization.
    slen = s->used ? state_put_cached(s, cache_idx, viewer, msg + 1) : 0;
    if (out_version) *out_version = s->version;
    pthread_mutex_unlock(&s->lock);
    msg[0] = applied ? 1 : 0;
    return slen + 1;
}

#ifdef FOOLISH_QUIC
// --------------------------------------------------------------------------
// game_bridge.h implementation — the QUIC/WebTransport transport (quic_wt.c)
// reaches the shared game through these, taking the exact same registry and
// per-game locks the HTTP/WS paths do. So QUIC is just another front-end onto
// the one authoritative in-memory game; no game logic is duplicated here.
// --------------------------------------------------------------------------

int gb_state_for(const char *game_id, int seat, unsigned char *out, int cap) {
    if (seat < VIEW_SPECTATOR) return -1;   // never the trusted VIEW_UNMASKED — same guard as h_state
    if (cap < 65536) return -1;   // require state_put's documented worst-case room (same 65536 buffer h_state uses)
    pthread_mutex_lock(&g_registry_lock);
    GameSlot *s = game_by_id(game_id);
    if (!s) { pthread_mutex_unlock(&g_registry_lock); return -1; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    int n = state_put(&s->game, seat, out);
    pthread_mutex_unlock(&s->lock);
    return n;
}

int gb_apply_move(const char *game_id, const char *token, int seat,
                  const unsigned char *in, int len, unsigned char *out, int cap) {
    if (seat < 0) return -1;                                       // seated players only (spectators use gb_state_for)
    if (cap < 65536) return -1;                                    // room for state_put_cached's worst case (same as h_state)
    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(token);
    GameSlot *s = game_by_id(game_id);
    if (!u || !s) { pthread_mutex_unlock(&g_registry_lock); return -1; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    // Same ownership check GET /ws enforces: this token must actually hold this
    // seat in this game (see ws_handshake_validate).
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    if (!(seat < s->game.num_players && seat_of(s, user_id) == seat)) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    // An empty payload (len==0) is a pure "fetch my current view" — no move.
    if (in && len > 0) ws_apply_move_locked(s, seat, /*spectator=*/false, in, len);
    int n = state_put_cached(s, /*cache_idx=*/seat, /*viewer=*/seat, out);
    pthread_mutex_unlock(&s->lock);
    return n;
}

// Thread wrapper: run the QUIC/HTTP3/WebTransport listener alongside the TCP
// acceptors, sharing this process's game state via the bridge above.
struct QuicArgs { int port; int workers; const char *cert; const char *key; };
static void *quic_thread_main(void *a) {
    thread_disable_cancellation();
    struct QuicArgs *qa = a;
    quic_wt_run(qa->port, qa->workers, qa->cert, qa->key);
    return NULL;
}
#endif   // FOOLISH_QUIC

static void *ws_conn_thread(void *argp) {
    thread_disable_cancellation();
    WsSpawnArg *sa = argp;
    Conn *conn = &sa->conn;
    Req *r = &sa->req;

    int seat; bool spectator; int cache_idx, viewer;
    GameSlot *s = ws_handshake_validate(r, &seat, &spectator, &cache_idx, &viewer);
    if (!s) {
        respond(conn, 401, "{\"error\":\"ws auth\"}");
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }

    char accept[64];
    if (!ws_accept_from_key(r->ws_key, accept, sizeof accept)) {
        respond(conn, 400, "{\"error\":\"ws key\"}");
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }
    WsConn wc;
    if (!ws_send_handshake_and_push(&wc, *conn, accept, s, cache_idx, viewer)) {
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }
    if (r->body_len > 0) ws_conn_prime(&wc, (const unsigned char *)r->body, r->body_len);
    free(sa->raw_buf); sa->raw_buf = NULL;   // primed into wc.pending — the raw request buffer is no longer referenced

    unsigned char in[4096];
    unsigned char msg[1 + 65536];
    int opcode;
    int mlen;
    while ((mlen = ws_recv_message(&wc, in, sizeof in, &opcode)) >= 0) {
        if (opcode != WS_OP_BIN && opcode != WS_OP_TEXT) continue;
        int mtotal = ws_service_message(s, seat, spectator, cache_idx, viewer, in, mlen, msg, NULL);
        if (ws_send_frame(&wc, WS_OP_BIN, msg, mtotal) < 0) break;
    }
    conn_close(&wc.conn);
    free(sa);
    return NULL;
}

// --------------------------------------------------------------------------
// Work-queue thread routing (T2a Deliverable 2). Replaces thread-per-
// connection for every one-shot HTTP endpoint with a dispatcher (the accept
// loop, below) + a small number of typed worker pools, each pulling off its
// own bounded queue:
//   - /auth/*, /create  -> g_auth_create_q (g_n_create_workers threads)
//   - /meta             -> g_meta_q        (g_n_meta_workers threads — see
//                           below for the g_n_meta_workers==0 "fold into the
//                           game pool" mode)
//   - /action,/state,/status -> g_game_q[hash(game_id) % g_n_game_workers]
// Sharding the game queues by game_id means every request for a given game
// lands on the SAME worker thread, so requests for one game are serialized
// by construction; the per-game lock (GameSlot.lock) then only has to
// arbitrate against that game's bot_thread and its /ws dedicated thread(s),
// not against a pile of other HTTP workers for the same game. Multiple
// worker threads may safely share ONE WorkQueue (wq_pop is mutex-guarded,
// any number of consumers), which is how g_n_create_workers/g_n_meta_workers
// > 1 works below — auth/create and meta requests carry no game_id to shard
// by, so widening those pools just adds more consumers of the same queue.
//
// All three counts (game/meta/create) are runtime-configurable
// (--game-workers=N --meta-workers=N --create-workers=N) and were tuned
// EMPIRICALLY on this 4-core box with the WS+legal hammer, not guessed —
// see SERVER_SCALING.md "T2a Deliverable 2 — worker-pool sweep" for the
// sweep table across connection counts and the defaults it settled on.
// g_n_meta_workers may be 0: /meta is low-frequency (lobby join/start/
// continue, not the hot per-move loop) and every branch it runs already
// goes through the same registry->game lock handoff as /action, so folding
// it onto the game pool (sharded by game_id, same as /action) instead of
// paying for a dedicated idle thread is a real point on the sweep, not just
// a degenerate case — the table says whether it actually wins.
// --------------------------------------------------------------------------

#define WQ_CAP 512
#define N_GAME_WORKERS_DEFAULT 4
#define MAX_GAME_WORKERS 64
#define N_META_WORKERS_DEFAULT 0
#define MAX_META_WORKERS 32
#define N_CREATE_WORKERS_DEFAULT 1
#define MAX_CREATE_WORKERS 32

static int g_n_game_workers = N_GAME_WORKERS_DEFAULT;
static int g_n_meta_workers = N_META_WORKERS_DEFAULT;
static int g_n_create_workers = N_CREATE_WORKERS_DEFAULT;

// SO_REUSEPORT multi-acceptor (see SERVER_SCALING.md). Instead of one
// dispatcher thread owning the single listening socket, run N acceptor
// threads, each with its OWN listener bound to the same port with
// SO_REUSEPORT. The kernel spreads inbound connections across the listeners
// by 4-tuple hash, so accept() + the TLS handshake + request parsing all
// parallelize and no single thread is the connection-arrival bottleneck (the
// old single dispatcher serialized every SSL_accept). Game-affinity is
// unchanged: each acceptor still hands the parsed connection to the epoll
// worker that owns its game (worker_handoff_push), exactly as before.
// --accept-threads=N; default 2 (a safe win over 1 without oversubscribing a
// small box, since idle acceptors just block in accept()).
#define N_ACCEPT_THREADS_DEFAULT 2
#define MAX_ACCEPT_THREADS 64
static int g_n_accept_threads = N_ACCEPT_THREADS_DEFAULT;

#ifdef FOOLISH_QUIC
// QUIC/HTTP3/WebTransport listener (foolish_server_quic build): --quic turns it
// on, --quic-port=N sets its UDP port (default = the TCP port; UDP and TCP are
// separate namespaces so they may share the number). Reuses --cert/--key for
// its TLS 1.3 (QUIC has no plaintext mode). See quic_wt.c / game_bridge.h.
static bool g_want_quic = false;
static int  g_quic_port = 0;
static int  g_quic_workers = 2;   // sharded QUIC event loops (--quic-workers=N); each its own SO_REUSEPORT UDP socket
#endif

typedef struct {
    Conn conn;
    Req req;
    char *raw_buf;   // owns the bytes req.body/req.query/etc point into until freed
} WorkItem;

typedef struct {
    WorkItem *buf;    // ring buffer, WQ_CAP entries
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} WorkQueue;

static void wq_init(WorkQueue *q) {
    q->buf = calloc(WQ_CAP, sizeof(WorkItem));
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

// Blocking push (backpressure): if a pool's queue is full, the dispatcher
// waits rather than dropping the connection or growing unboundedly. Fine
// for this POC's bounded load; a production version would size WQ_CAP for
// the target burst or shed load with a 503 instead of blocking the accept
// loop.
static void wq_push(WorkQueue *q, const WorkItem *item) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == WQ_CAP) pthread_cond_wait(&q->not_full, &q->mtx);
    q->buf[q->tail] = *item;
    q->tail = (q->tail + 1) % WQ_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

static void wq_pop(WorkQueue *q, WorkItem *out) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->mtx);
    *out = q->buf[q->head];
    q->head = (q->head + 1) % WQ_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

static WorkQueue g_auth_create_q;
static WorkQueue g_meta_q;
static WorkQueue g_game_q[MAX_GAME_WORKERS];

static void route(Req *r, Conn *conn) {
    if (!strcmp(r->method, "OPTIONS")) { respond(conn, 200, "{}"); return; }
    if (!strcmp(r->path, "/health")) { respond(conn, 200, "{\"ok\":true}"); return; }
    if (!strcmp(r->path, "/auth/signup") || !strcmp(r->path, "/auth/signin")) { h_signup(r, conn); return; }
    if (!strcmp(r->path, "/create")) { h_create(r, conn); return; }
    if (!strcmp(r->path, "/meta"))   { h_meta(r, conn); return; }
    if (!strcmp(r->path, "/action")) { h_action(r, conn); return; }
    if (!strcmp(r->path, "/state"))  { h_state(r, conn); return; }
    if (!strcmp(r->path, "/status")) { h_status(r, conn); return; }
    if (!strcmp(r->path, "/stats"))  { h_stats(r, conn); return; }
    respond(conn, 404, "{\"error\":\"route\"}");
}

// Picks which pool's queue a one-shot request belongs on. Called by the
// dispatcher only for requests it hasn't already answered inline (OPTIONS,
// /health) or handed to a dedicated thread (/ws) — see main()'s accept loop.
static WorkQueue *classify_queue(Req *r) {
    if (!strcmp(r->path, "/auth/signup") || !strcmp(r->path, "/auth/signin") || !strcmp(r->path, "/create"))
        return &g_auth_create_q;
    bool is_meta = !strcmp(r->path, "/meta");
    bool is_game = !strcmp(r->path, "/action") || !strcmp(r->path, "/state") || !strcmp(r->path, "/status");
    if (is_meta && g_n_meta_workers > 0) return &g_meta_q;
    if (is_game || is_meta) {   // is_meta here implies g_n_meta_workers==0 — fold onto the game pool
        char gid[ID_LEN + 1] = {0};
        const char *gp = strstr(r->query, "game_id=");
        if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
        unsigned long h = gid[0] ? hash_str(gid) : 0;
        return &g_game_q[h % (unsigned long)g_n_game_workers];
    }
    // unrecognized route -> route() 404s it; any pool can carry it.
    return g_n_meta_workers > 0 ? &g_meta_q : &g_game_q[0];
}

static void *worker_thread(void *arg) {
    thread_disable_cancellation();
    WorkQueue *q = arg;
    for (;;) {
        WorkItem item;
        wq_pop(q, &item);
        route(&item.req, &item.conn);
        conn_close(&item.conn);
        free(item.raw_buf);
    }
    return NULL;
}

// ==========================================================================
// STAGE 6 — epoll-per-shard connection I/O (SERVER_SCALING.md "Stage 6").
//
// T2a (SERVER_SCALING.md "Deliverable 2 — WS design: (B), not (A)") deferred
// this on purpose: a thread per live /ws connection was the correct,
// Helgrind-provable-clean choice to ship first, with the tradeoff stated
// plainly — ~0.9MB/connection (thread stack) and, at real scale, scheduler
// oversubscription (Stage 5 measured ~300+ runnable OS threads on 4 cores at
// 160 games, capping bot-compute CPU engagement at ~80% even with the
// kernel's own serialization removed). This section is design (A): each of
// the `g_n_game_workers` game-worker threads now runs its OWN epoll loop,
// single-threaded, over exactly the connections whose game_id hashes to it
// (the SAME hash `classify_queue` already used to shard the old typed HTTP
// queues) — no dedicated OS thread per connection, plaintext or `/ws`,
// one-shot or persistent.
//
// SCOPE (stated up front, the same "correct partial beats broken unified"
// posture the task itself allows): this section covers **plaintext**
// connections only. Non-blocking OpenSSL (SSL_read/SSL_write's
// WANT_READ/WANT_WRITE state machine re-armed against epoll's read/write
// readiness) is real, fiddly, per-direction state that this stage did not
// implement — seeSERVER_SCALING.md's "Stage 6" section for the honest
// writeup. A `--tls` server instead keeps the ENTIRE pre-Stage-6 design:
// thread-per-`/ws`-connection (ws_conn_thread) + the typed HTTP work-queue
// pools (g_game_q/g_meta_q/worker_thread, both still fully intact above,
// unchanged) — see main()'s branch on `g_tls_ctx` for exactly where the two
// designs split.
//
// DESIGN
//   - Each epoll worker (`Worker`, below) owns one `epoll_fd`, one `eventfd`
//     (`wake_evfd` — the cross-thread wakeup primitive, used for TWO
//     things: the dispatcher handing off a freshly-accepted fd, and
//     bot_thread signaling a state change — see epoll_notify_game_changed's
//     doc), a bounded mutex-guarded handoff queue (dispatcher produces,
//     this worker alone consumes), and a doubly-linked list of its live
//     `/ws` connections (`ws_head` — this worker's own thread is the ONLY
//     reader/writer of that list, so it needs no lock of its own; see
//     "Threading" below).
//   - The DISPATCHER (main()'s accept loop, unchanged as the single accept()
//     thread) still fully reads+parses each request BLOCKING, exactly as
//     every earlier stage did (read_and_parse_request) — see
//     SERVER_SCALING.md's "Stage 6" section for why this is a deliberate,
//     documented scope decision rather than a deviation: it keeps 100% of
//     the existing request-parsing code (and the /ws handshake's auth+
//     upgrade logic) completely unchanged, and it does not reintroduce
//     thread-per-connection — no thread is spawned either way, the fd is
//     just handed to a shard's epoll loop once the dispatcher is done with
//     it. What's NEW is what happens AFTER the read: instead of spawning a
//     `ws_conn_thread` or pushing a `WorkItem` onto a typed queue, the
//     dispatcher builds the response (or the 101 handshake + initial state
//     push) into a fresh `EConn`'s buffered output (`conn_init_buffered` —
//     see conn.h) by calling the SAME handler code (route()/h_action/
//     h_state/h_meta/ws_handshake_validate/ws_send_handshake_and_push) this
//     file has always used, flips the fd non-blocking, and hands it to
//     `game_worker_index(game_id)`'s `Worker` over the handoff queue. The
//     worker's job from then on is purely non-blocking flush (one-shot) or
//     the ongoing async WS frame loop (`/ws`) — it never re-does any of the
//     auth/business logic the dispatcher already ran.
//   - Non-blocking WS framing: `wsasync_feed` (below) is a byte-for-byte
//     reimplementation of ws_recv_message's (ws.c) RFC 6455 decode —
//     2-byte header, optional 2/8-byte extended length, optional 4-byte
//     mask, payload, control frames answered inline, CONT-frame reassembly
//     — restructured as an explicit, resumable phase machine that consumes
//     bytes fed in from a non-blocking read() instead of blocking on
//     conn_read/ws_fill. See its own doc for the exact contract.
//   - The one cross-thread seam: bot_thread mutates a `GameSlot` a worker
//     also owns connections for. See epoll_notify_game_changed +
//     worker_push_stale's docs — this is the piece Helgrind's job is to
//     prove race-clean (SERVER_SCALING.md "Stage 6").
//
// THREADING
//   Each `Worker`'s epoll loop is fully SINGLE-THREADED over its own shard:
//   only that worker's own thread ever touches its `EConn`s, its `ws_head`
//   list, or calls epoll_ctl on its `epoll_fd` — so none of that needs a
//   lock. The two things that ARE touched cross-thread are (a) the handoff
//   queue (dispatcher produces, the worker consumes — guarded by
//   `handoff_mtx`, the same bounded-queue discipline `WorkQueue` above
//   uses) and (b) `GameSlot.lock` itself (already the existing, proven
//   per-game lock every path in this file goes through — bot_thread and a
//   worker's WS-message handling both take it exactly the way bot_thread
//   and ws_conn_thread always have).
// ==========================================================================

#define EPOLL_MAX_EVENTS   256
#define WS_IN_CAP          4096                  // incoming client frame cap — matches ws_conn_thread's `in[4096]`; client->server messages are always small (a move or a poll)
#define WS_OUT_CAP         4096                  // one reply frame's cap — real worst case is 690B (VIEW_CACHE_CAP's own doc); generous margin, same sizing discipline
#define WS_WBUF_CAP        (2 * WS_OUT_CAP)      // room for one in-flight (unflushed) frame + one freshly queued push, per live /ws connection
#define ONESHOT_WBUF_CAP   (1 + 65536 + 512)     // one-shot HTTP response cap — h_state's raw (uncached) state_put can be up to 65536; +512 header margin

typedef enum { ECONN_ONE_SHOT, ECONN_WS } EConnKind;

// wsasync_feed's return contract (see its own doc).
#define WSF_ERROR     (-1)
#define WSF_NEED_MORE   0
#define WSF_MESSAGE     1

// Incremental, resumable WS frame-parse phase — see wsasync_feed.
typedef enum { WSP_HDR2, WSP_EXTLEN, WSP_MASK, WSP_PAYLOAD } WsParsePhase;

typedef struct EConn {
    int fd;
    EConnKind kind;

    // Non-blocking WRITE side: wbuf[woff..wlen) are the bytes still owed to
    // the peer. Filled by whichever handler produced a reply — a one-shot
    // route() call, ws_send_handshake_and_push, or ws_send_frame appending
    // through `wc_out` (a buffered Conn — conn.h — wrapping THIS SAME
    // wbuf), never a real write() until the epoll loop flushes it. Touched
    // only by this EConn's owning worker thread.
    unsigned char *wbuf;
    int wbuf_cap, wlen, woff;
    bool want_epollout;      // whether EPOLLOUT is currently armed in epoll_ctl for this fd
    bool close_after_flush;  // ECONN_ONE_SHOT: close once wbuf fully drains (every one-shot response ends the connection, same as the old `Connection: close`)
    bool closed;             // econn_close ran: fd shut + unlinked, but free() DEFERRED to end of this epoll batch (see econn_close / econn_drain_dead). Guards against a stale same-batch event re-closing it.

    // ECONN_WS fields (unused for ECONN_ONE_SHOT):
    GameSlot *slot;
    int seat;
    bool spectator;
    int cache_idx, viewer;             // see state_put_cached's doc for why these differ for a spectator
    uint32_t last_pushed_version;      // s->version as of the last reply/push THIS connection's peer actually received
    WsConn wc_out;                     // output-only WsConn wrapping wbuf via a buffered Conn (conn.h) — see econn_reserve_out
    // Incremental parser state (wsasync_feed):
    WsParsePhase phase;
    unsigned char hdrbuf[8];
    int hdr_have, hdr_need;
    int fin, op, masked;
    int64_t frame_len;
    unsigned char mkey[4];
    int64_t payload_got;
    bool frame_validated;               // has THIS frame's header (new-vs-continuation, oversized) already been validated? See wsasync_feed's WSP_PAYLOAD case.
    int msg_opcode;                    // -1 iff not currently assembling a fragmented data message
    int msg_total;                     // bytes assembled so far into msg_buf
    int last_msg_len;                  // set alongside a WSF_MESSAGE return — see wsasync_feed's doc
    unsigned char msg_buf[WS_IN_CAP];
    unsigned char ctrl_buf[125];       // control-frame (PING/PONG/CLOSE) payload scratch — RFC 6455 5.5: control frames are never > 125 bytes

    struct EConn *prev, *next;   // intrusive list — EITHER this worker's handoff queue (via `next` only, before the handoff completes) OR its active ws_head list (both, after) — the two lifetimes never overlap
} EConn;

typedef struct Worker {
    int idx;
    int epfd;
    int wake_evfd;
    pthread_mutex_t handoff_mtx;
    EConn *handoff_head, *handoff_tail;   // dispatcher-produced, this worker alone consumes (drain_handoff_queue)
    EConn *ws_head;                       // this worker's live /ws connections — single-threaded, no lock (see "Threading" above)
    EConn *dead_head;                     // econn_close'd this batch, freed by econn_drain_dead at the end of the epoll loop iteration — see econn_close
} Worker;

static Worker g_workers[MAX_GAME_WORKERS];
// Set true only when main() actually starts the epoll worker pool
// (plaintext mode). epoll_notify_game_changed no-ops while false, so a
// --tls server (which never populates g_workers) never touches it.
static bool g_epoll_active = false;

// Same shard hash classify_queue's game-worker routing has always used
// (hash_str(game_id) % g_n_game_workers) — reused here so a game's HTTP
// requests, its /ws connections, AND its bot_thread's push notifications all
// agree on exactly one owning worker.
static int game_worker_index(const char *game_id) {
    unsigned long h = (game_id && game_id[0]) ? hash_str(game_id) : 0;
    return (int)(h % (unsigned long)g_n_game_workers);
}

// The one cross-thread seam (SERVER_SCALING.md "Stage 6"): bot_thread (its
// own per-game trampoline thread) just mutated `s` — a bot's move landed, or
// the game ended — and bumped s->version under s->lock, same as every other
// mutation site in this file. Under the OLD thread-per-connection /ws
// design that was enough by itself: every live connection was blocked in
// its OWN thread's ws_recv_message, so the NEXT client poll would simply
// see the fresh version. Under epoll, a connection sitting idle is NOT
// blocked in a read — it is just an fd registered in its worker's epoll
// set, and nothing else touches it until either the peer sends a frame or
// something pokes the worker. This function is that poke: a single eventfd
// write (no payload — it doesn't say WHICH game changed, since the scan
// this triggers, worker_push_stale, is cheap and only runs once per actual
// bot decision, not per idle poll) wakes the owning worker's epoll_wait,
// which then re-scans every /ws connection it owns for a stale cached view
// and pushes fresh state to each one that needs it. See worker_push_stale's
// doc for the two call sites (this one, and the fast per-request path used
// when a worker's own handling of a client's frame changed a game its OTHER
// connections are watching).
static void epoll_notify_game_changed(GameSlot *s) {
    if (!g_epoll_active) return;
    Worker *w = &g_workers[game_worker_index(s->id)];
    uint64_t one = 1;
    ssize_t wr = write(w->wake_evfd, &one, sizeof one);
    (void)wr;   // best-effort wake; eventfd only fails to accept a write at counter saturation (~2^63) — unreachable here
}

// Forward declarations: wsasync_feed (below) queues inline control-frame
// replies (PONG, a CLOSE echo) through these two — full definitions are
// with the rest of the EConn output-buffering machinery, just after it.
static bool econn_reserve_out(EConn *ec, int need);
static void econn_commit_out(EConn *ec);

// --------------------------------------------------------------------------
// wsasync_feed — incremental, resumable RFC 6455 frame decode.
//
// Mirrors ws_recv_message's (ws.c) exact per-field logic and control-frame/
// fragmentation semantics — see that function's own comment for the wire
// format this replicates — but consumes bytes fed in from a non-blocking
// read() instead of blocking on conn_read/ws_fill, resuming exactly where
// the previous call left off (all resumable state lives in `ec`). PING is
// answered with PONG inline (queued into ec->wbuf via ec->wc_out, same as
// ws_recv_message's own `ws_send_frame(c, WS_OP_PONG, ...); continue;`);
// PONG is swallowed; CLOSE gets a best-effort echo (matching
// ws_send_close(c, 1000)) and ends the connection — none of the three ever
// surface to the caller as a "message".
//
// Returns:
//   WSF_MESSAGE   — one complete data message (TEXT or BIN, fully
//                   reassembled across any CONT fragments) is ready:
//                   ec->msg_buf[0..ec->last_msg_len) holds it. *consumed
//                   says how many of `in`'s `n` bytes this call used — there
//                   may be bytes for the START of the NEXT frame left over
//                   in `in[*consumed..n)`; the caller must re-feed those
//                   (after handling this message) before reading the socket
//                   again.
//   WSF_NEED_MORE — no complete message yet; *consumed == n (every input
//                   byte was used). Caller should wait for more EPOLLIN.
//   WSF_ERROR     — protocol violation, an oversized message (> WS_IN_CAP),
//                   or a CLOSE frame. Caller must tear the connection down;
//                   *consumed is NOT reliably set on every WSF_ERROR return
//                   path (every caller ignores it in this case — the
//                   connection is going away regardless).
static int wsasync_feed(EConn *ec, const unsigned char *in, int n, int *consumed) {
    int off = 0;
    for (;;) {
        switch (ec->phase) {
        case WSP_HDR2: {
            int take = 2 - ec->hdr_have; if (take > n - off) take = n - off;
            if (take > 0) { memcpy(ec->hdrbuf + ec->hdr_have, in + off, (size_t)take); ec->hdr_have += take; off += take; }
            if (ec->hdr_have < 2) { *consumed = off; return WSF_NEED_MORE; }
            ec->fin    = (ec->hdrbuf[0] >> 7) & 1;
            ec->op     = ec->hdrbuf[0] & 0x0F;
            ec->masked = (ec->hdrbuf[1] >> 7) & 1;
            int64_t len7 = ec->hdrbuf[1] & 0x7F;
            ec->hdr_have = 0;
            ec->frame_validated = false;   // a brand-new frame starts here — see WSP_PAYLOAD's doc
            if (len7 == 126)      { ec->hdr_need = 2; ec->phase = WSP_EXTLEN; }
            else if (len7 == 127) { ec->hdr_need = 8; ec->phase = WSP_EXTLEN; }
            else { ec->frame_len = len7; ec->phase = ec->masked ? WSP_MASK : WSP_PAYLOAD; }
            continue;
        }
        case WSP_EXTLEN: {
            int take = ec->hdr_need - ec->hdr_have; if (take > n - off) take = n - off;
            if (take > 0) { memcpy(ec->hdrbuf + ec->hdr_have, in + off, (size_t)take); ec->hdr_have += take; off += take; }
            if (ec->hdr_have < ec->hdr_need) { *consumed = off; return WSF_NEED_MORE; }
            int64_t len = 0;
            for (int i = 0; i < ec->hdr_need; i++) len = (len << 8) | ec->hdrbuf[i];
            if (len < 0) return WSF_ERROR;   // top bit set on the 8-byte form is a protocol violation per spec
            ec->frame_len = len;
            ec->hdr_have = 0;
            ec->phase = ec->masked ? WSP_MASK : WSP_PAYLOAD;
            continue;
        }
        case WSP_MASK: {
            int take = 4 - ec->hdr_have; if (take > n - off) take = n - off;
            if (take > 0) { memcpy(ec->mkey + ec->hdr_have, in + off, (size_t)take); ec->hdr_have += take; off += take; }
            if (ec->hdr_have < 4) { *consumed = off; return WSF_NEED_MORE; }
            ec->hdr_have = 0;
            ec->phase = WSP_PAYLOAD;
            continue;
        }
        case WSP_PAYLOAD: {
            bool ctrl = (ec->op == WS_OP_PING || ec->op == WS_OP_PONG || ec->op == WS_OP_CLOSE);
            // Frame-header validation (new-vs-continuation bookkeeping, the
            // oversized check) MUST run EXACTLY ONCE per frame. wsasync_feed
            // can be called many times while a single frame's payload
            // trickles in across several non-blocking reads (WSF_NEED_MORE
            // below, resumed on the next EPOLLIN) — including calls that
            // land here with ZERO payload bytes yet available (the header
            // and mask arrived, but the payload hasn't started at all) — so
            // neither "first call" nor "payload_got == 0" reliably means
            // "not yet validated" (a real, caught-live bug: a header+mask
            // that fully arrives with 0 payload bytes in the SAME read, then
            // the payload trickles in on a LATER read, hits this case again
            // with payload_got still 0 — see SERVER_SCALING.md "Stage 6").
            // `ec->frame_validated` is the actual per-FRAME signal: reset to
            // false exactly once, when WSP_HDR2 starts parsing this frame's
            // 2-byte header, and set true here the first time validation
            // actually runs for it — correct regardless of how the
            // header/mask/payload bytes happen to split across reads.
            if (!ec->frame_validated) {
                ec->frame_validated = true;
                if (ctrl && ec->frame_len > 125) return WSF_ERROR;   // control frames are never fragmented/oversized (RFC 6455 5.5)
                if (!ctrl) {
                    if (ec->op != WS_OP_CONT && ec->op != WS_OP_TEXT && ec->op != WS_OP_BIN) return WSF_ERROR;
                    if (ec->op != WS_OP_CONT) {
                        if (ec->msg_opcode != -1) return WSF_ERROR;   // a new message started before the last one finished
                        ec->msg_opcode = ec->op;
                    } else if (ec->msg_opcode == -1) {
                        return WSF_ERROR;   // continuation with nothing to continue
                    }
                    if (ec->msg_total + ec->frame_len > (int64_t)WS_IN_CAP) return WSF_ERROR;   // oversized for this buffer
                }
            }
            unsigned char *dst = ctrl ? ec->ctrl_buf : ec->msg_buf + ec->msg_total;
            int take = (int)(ec->frame_len - ec->payload_got); if (take > n - off) take = n - off;
            if (take > 0) { memcpy(dst + ec->payload_got, in + off, (size_t)take); ec->payload_got += take; off += take; }
            if (ec->payload_got < ec->frame_len) { *consumed = off; return WSF_NEED_MORE; }

            if (ec->masked) for (int64_t i = 0; i < ec->frame_len; i++) dst[i] ^= ec->mkey[i & 3];
            int op = ec->op; int64_t flen = ec->frame_len;
            ec->payload_got = 0; ec->hdr_have = 0; ec->phase = WSP_HDR2;   // ready for the next frame either way

            if (ctrl) {
                if (op == WS_OP_PING) {
                    if (econn_reserve_out(ec, (int)flen + 14)) {
                        ws_send_frame(&ec->wc_out, WS_OP_PONG, ec->ctrl_buf, flen);
                        econn_commit_out(ec);
                    }
                    continue;   // more frames may remain in this same chunk
                }
                if (op == WS_OP_PONG) continue;
                // CLOSE: best-effort echo (matches ws_send_close(c, 1000)), then tear down.
                if (econn_reserve_out(ec, 16)) {
                    unsigned char payload[2] = { 0x03, 0xE8 };   // 1000, network byte order
                    ws_send_frame(&ec->wc_out, WS_OP_CLOSE, payload, 2);
                    econn_commit_out(ec);
                }
                *consumed = off;
                return WSF_ERROR;
            }
            ec->msg_total += (int)flen;
            if (ec->fin) {
                *consumed = off;
                ec->last_msg_len = ec->msg_total;
                ec->msg_total = 0; ec->msg_opcode = -1;
                return WSF_MESSAGE;
            }
            continue;   // more fragments to come
        }
        }
    }
}

// --------------------------------------------------------------------------
// EConn output buffering + epoll bookkeeping — all single-threaded per
// worker (see "Threading" above), so none of this needs a lock.
// --------------------------------------------------------------------------

// Prepares to append `need` more bytes to ec->wbuf: compacts away any
// already-flushed prefix first (slides the unflushed [woff,wlen) tail down
// to offset 0), so backpressure that has only PARTIALLY drained a previous
// write never blocks a new append from reusing that reclaimed space. Fills
// ec->wc_out with a buffered Conn (conn.h) over the (now-compacted) tail —
// callers append through `ec->wc_out` (ws_send_frame et al.), then MUST call
// econn_commit_out to publish the new length. Returns false (appends
// nothing) iff there truly isn't `need` bytes of room even after compacting
// — a real backpressure case (this connection's peer has stopped reading
// entirely) — callers drop that specific send rather than risk writing a
// partial/corrupt frame into the buffer.
static bool econn_reserve_out(EConn *ec, int need) {
    if (ec->woff > 0) {
        int rem = ec->wlen - ec->woff;
        if (rem > 0) memmove(ec->wbuf, ec->wbuf + ec->woff, (size_t)rem);
        ec->wlen = rem; ec->woff = 0;
    }
    if (ec->wlen + need > ec->wbuf_cap) return false;
    Conn buffered; conn_init_buffered(&buffered, ec->wbuf, ec->wbuf_cap);
    buffered.buf_len = ec->wlen;   // resume appending after whatever's already queued
    ws_conn_init(&ec->wc_out, buffered, /*mask_outgoing=*/0);   // server frames are never masked
    return true;
}
static void econn_commit_out(EConn *ec) { ec->wlen = ec->wc_out.conn.buf_len; }

static void econn_set_epollout(Worker *w, EConn *ec, bool want) {
    if (want == ec->want_epollout) return;
    ec->want_epollout = want;
    uint32_t events = (uint32_t)((ec->kind == ECONN_WS ? EPOLLIN : 0) | (want ? EPOLLOUT : 0));
    struct epoll_event ev = { .events = events, .data.ptr = ec };
    epoll_ctl(w->epfd, EPOLL_CTL_MOD, ec->fd, &ev);
}

// Shuts a connection's fd and unlinks it, but DEFERS free() to the end of the
// current epoll batch (econn_drain_dead). Reason: one connection's handler can
// close another (worker_push_stale fanning a move out to a peer that then hits
// a write error), and that peer may already have its OWN event queued later in
// the SAME epoll_wait() batch — freeing it immediately turns that stale event
// into a use-after-free / double-free (memcheck-confirmed; the crash that
// aborted the server at a few hundred connections). Instead we mark it closed,
// unlink it from ws_head, and park it on w->dead_head; the event loop skips any
// event whose ec is already `closed`, and frees the parked set once the whole
// batch is drained. Idempotent: a second close (e.g. a stale HUP event) no-ops.
static void econn_close(Worker *w, EConn *ec) {
    if (ec->closed) return;
    ec->closed = true;
    atomic_fetch_sub_explicit(&g_live_conns, 1, memory_order_relaxed);   // pairs with worker_handoff_push's admit
    epoll_ctl(w->epfd, EPOLL_CTL_DEL, ec->fd, NULL);
    close(ec->fd);
    if (ec->kind == ECONN_WS) {
        if (ec->prev) ec->prev->next = ec->next; else if (w->ws_head == ec) w->ws_head = ec->next;
        if (ec->next) ec->next->prev = ec->prev;
    }
    // Reuse `next` as the dead-list link — ec is now off ws_head, and any live
    // ws_head iterator (worker_push_stale) captured its next pointer before the
    // close, so overwriting it here is safe.
    ec->next = w->dead_head;
    w->dead_head = ec;
}

// Frees every connection econn_close parked this batch. Called once at the end
// of each epoll loop iteration, when no events[] entry can reference them.
static void econn_drain_dead(Worker *w) {
    EConn *d = w->dead_head;
    w->dead_head = NULL;
    while (d) {
        EConn *nx = d->next;
        free(d->wbuf);
        free(d);
        d = nx;
    }
}

// Non-blocking flush of ec->wbuf[woff..wlen). Returns false iff `ec` was
// closed (a real I/O error, or — ECONN_ONE_SHOT with close_after_flush — a
// completed flush): the caller must not touch `ec` again after a false
// return. On EAGAIN, arms EPOLLOUT and returns true (still open, not yet
// fully drained) — the next EPOLLOUT event resumes the flush.
static bool econn_try_flush(Worker *w, EConn *ec) {
    while (ec->woff < ec->wlen) {
        ssize_t wr = write(ec->fd, ec->wbuf + ec->woff, (size_t)(ec->wlen - ec->woff));
        if (wr < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { econn_set_epollout(w, ec, true); return true; }
            if (errno == EINTR) continue;
            econn_close(w, ec); return false;
        }
        if (wr == 0) { econn_close(w, ec); return false; }
        ec->woff += (int)wr;
    }
    ec->woff = ec->wlen = 0;
    econn_set_epollout(w, ec, false);
    if (ec->kind == ECONN_ONE_SHOT && ec->close_after_flush) { econn_close(w, ec); return false; }
    return true;
}

// Pushes fresh state to every active /ws connection THIS worker owns whose
// cached view is stale (ec->last_pushed_version != its game's current
// s->version), skipping `skip` and, when `game` is non-NULL, every OTHER
// game too. TWO call sites:
//   1. The eventfd-triggered path, woken by epoll_notify_game_changed from
//      bot_thread (or h_meta — a lobby transition) on a DIFFERENT thread —
//      since the eventfd carries no payload (which game changed), this is
//      called with game=NULL, skip=NULL and checks every /ws connection
//      this worker owns.
//   2. PROFILE_HOTPATH.md "T1f" (push-only protocol): handle_ws_readable,
//      right after a human move applies, calls this INLINE with
//      game=ec->slot, skip=ec — fanning the new state out to that game's
//      OTHER live connections (the mover already got its own direct reply).
//      No cross-thread wakeup needed here: this runs on the SAME worker
//      thread that already owns every /ws connection for this game (see
//      game_worker_index's doc), so it's just a normal function call, not
//      an eventfd round trip.
// Bounded by how many connections one shard holds, and — thanks to the
// version-stale check above — naturally coalesces: several moves landing in
// quick succession before a given connection is next scanned still cost it
// only ONE push (the latest cached view), not one per move.
//
// History: an earlier revision of this file scoped call site 2 OUT
// (bot-thread pushes only) because the load client of the time POLLED —
// every idle seat sent an empty frame every ~1ms regardless — so fanning
// out on every human move on TOP of that made every OTHER seat's client
// immediately re-check eligibility and often re-submit, racing the others
// for the same now-stale window and measurably hurting the applied ratio
// (SERVER_SCALING.md "Stage 6"). That was a load-tool artifact, not a
// reason to withhold the push: a real client updates its UI and waits for
// its human, it doesn't auto-resubmit. T1f made the reference client
// (`foolish_hammer --mode=ws`) genuinely push-driven — it submits at most
// one legal move per pushed state and otherwise just waits — which removes
// the herd motive; see PROFILE_HOTPATH.md "T1f" for the measured ratio.
// WithOUT this call site, a push-only (non-polling) client would have no
// way to learn about another seat's move at all: it would just sit blocked
// in ws_recv_message forever, stalling the whole game.
// Encode one binary WS frame [ok][state_put_cached(view)] for `ec` STRAIGHT
// into its output buffer under s->lock, copying the view bytes exactly once —
// from the per-seat cache into wbuf — instead of the old two-step (cache ->
// worker scratch in state_put_cached, then scratch -> wbuf in ws_send_frame).
// wbuf is owned by this single-threaded worker (game_worker_index's
// invariant), so appending to it while holding the GAME lock races nothing and
// needs no extra lock; the actual socket flush stays OUTSIDE the lock, in the
// caller. This is the two-copies-to-one push change (PROFILE_HOTPATH.md:
// memcpy ~18% on the epoll build).
//
//   apply_in != NULL  -> apply that client frame first (mover-reply path); the
//                        reply's ok byte is 1 iff the kernel accepted it.
//   only_if_stale     -> skip entirely unless ec->last_pushed_version differs
//                        from the (post-apply) version (fan-out path).
// Returns 1 if a frame was encoded+committed into wbuf (caller should flush),
// 0 if nothing was queued (only_if_stale and not stale), or -1 if the out
// buffer had no room (caller leaves ec untouched). *out_version (may be NULL)
// gets s->version as observed under the lock; *out_applied (may be NULL) gets
// whether apply_in was accepted.
static int econn_push_view(EConn *ec, const unsigned char *apply_in, int apply_len,
                           bool only_if_stale, uint32_t *out_version, bool *out_applied) {
    static _Thread_local unsigned char fallback[1 + 65536];   // this worker's own thread only — never shared; used only on the never-hit overflow path
    GameSlot *s = ec->slot;
    unsigned char ok = 0;
    int rc = 0;
    pthread_mutex_lock(&s->lock);
    if (apply_in) ok = ws_apply_move_locked(s, ec->seat, ec->spectator, apply_in, apply_len) ? 1 : 0;
    uint32_t v = s->version;
    bool stale = ec->last_pushed_version != v;
    if (!only_if_stale || stale) {
        const unsigned char *state = NULL;
        int slen = s->used ? state_put_cached_ptr(s, ec->cache_idx, ec->viewer, &state, fallback) : 0;
        if (econn_reserve_out(ec, slen + 1 + 14) &&
            ws_send_frame2(&ec->wc_out, WS_OP_BIN, &ok, 1, state, slen) >= 0) {
            econn_commit_out(ec);
            rc = 1;
        } else {
            rc = -1;
        }
    }
    if (out_version) *out_version = v;
    pthread_mutex_unlock(&s->lock);
    if (out_applied) *out_applied = (ok != 0);
    return rc;
}

static void worker_push_stale(Worker *w, GameSlot *game, EConn *skip) {
    // `next` MUST be captured before econn_try_flush below: on a real write
    // error that call does econn_close(ec), which frees ec AND unlinks it from
    // ws_head. Advancing the loop with `ec = ec->next` after that is a
    // use-after-free (it reads the freed node's link) — and under load enough
    // connections error out at once that it corrupts the list and cascades into
    // a double-free / abort ("free(): invalid pointer"). econn_close only frees
    // ec itself and re-links its neighbors, so the `next` node captured up front
    // stays valid. (A plain `continue` does NOT avoid this — the for-loop's own
    // `ec = ec->next` increment still runs on the freed pointer.)
    EConn *next;
    for (EConn *ec = w->ws_head; ec; ec = next) {
        next = ec->next;
        if (ec == skip) continue;
        if (game && ec->slot != game) continue;
        uint32_t v;
        if (econn_push_view(ec, NULL, 0, /*only_if_stale=*/true, &v, NULL) == 1) {
            ec->last_pushed_version = v;
            if (!econn_try_flush(w, ec)) continue;   // ec is now freed; `next` was saved above
        }
    }
}

// Drains fds handed off by the dispatcher (epoll_dispatch_ws/
// epoll_dispatch_oneshot) into this worker's own epoll set. Each arrives
// with its FIRST reply/handshake already encoded into wbuf (the dispatcher
// built it via the buffered-Conn trick before handing the fd over) — this
// just registers it for EPOLLOUT (to flush that) and, for a /ws connection,
// EPOLLIN too (to read whatever the client sends next) and links it into
// ws_head.
static void drain_handoff_queue(Worker *w) {
    pthread_mutex_lock(&w->handoff_mtx);
    EConn *head = w->handoff_head;
    w->handoff_head = w->handoff_tail = NULL;
    pthread_mutex_unlock(&w->handoff_mtx);
    while (head) {
        EConn *ec = head; head = head->next; ec->next = NULL;
        if (ec->wlen <= ec->woff) { econn_close(w, ec); continue; }   // nothing to send at all — shouldn't happen, defensive
        uint32_t events = (uint32_t)((ec->kind == ECONN_WS ? EPOLLIN : 0) | EPOLLOUT);
        ec->want_epollout = true;
        struct epoll_event ev = { .events = events, .data.ptr = ec };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, ec->fd, &ev) < 0) { atomic_fetch_sub_explicit(&g_live_conns, 1, memory_order_relaxed); close(ec->fd); free(ec->wbuf); free(ec); continue; }
        if (ec->kind == ECONN_WS) {
            ec->prev = NULL; ec->next = w->ws_head;
            if (w->ws_head) w->ws_head->prev = ec;
            w->ws_head = ec;
        }
    }
}

// EPOLLIN on a live /ws connection: drain what's available non-blockingly,
// feed it through wsasync_feed, and for each complete message, apply it
// (ws_service_message — the SAME function ws_conn_thread uses) and reply.
static void handle_ws_readable(Worker *w, EConn *ec) {
    for (;;) {
        unsigned char tmp[8192];
        ssize_t r = read(ec->fd, tmp, sizeof tmp);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            econn_close(w, ec); return;
        }
        if (r == 0) { econn_close(w, ec); return; }   // peer closed
        int off = 0;
        while (off < (int)r) {
            int consumed = 0;
            int rc = wsasync_feed(ec, tmp + off, (int)r - off, &consumed);
            off += consumed;
            if (rc == WSF_MESSAGE) {
                // Apply the move (if any) and encode the [ok][state] reply
                // straight into wbuf under one lock acquisition — the same
                // service ws_conn_thread does via ws_service_message, but the
                // buffered (epoll) sink lets econn_push_view land the state
                // bytes in wbuf with a single copy (see its doc).
                uint32_t v; bool applied = false;
                if (econn_push_view(ec, ec->msg_buf, ec->last_msg_len, /*only_if_stale=*/false, &v, &applied) == 1) {
                    ec->last_pushed_version = v;
                }
                if (!econn_try_flush(w, ec)) return;   // closed (real write error) — ec is gone
                // PROFILE_HOTPATH.md "T1f" (push-only protocol): fan the new
                // state out to this game's OTHER live /ws connections — the
                // exact same cached-view push worker_push_stale already
                // gives the bot_thread/eventfd path (see its doc), just
                // called straight from THIS worker's own thread instead of
                // via an eventfd wakeup, since this worker already owns
                // every connection for `ec->slot` (game_worker_index's
                // invariant). Gated on `applied`: a rejected move, a
                // spectator's frame, or (pre-T1f-client) a plain poll never
                // changed s->version, so worker_push_stale's own stale-check
                // would no-op every OTHER connection anyway — skipping the
                // call entirely on those avoids even the per-connection lock
                // scan. `skip=ec`: the mover already has its direct reply
                // above; it must not also receive a redundant fan-out frame
                // for the very same version.
                if (applied) worker_push_stale(w, ec->slot, ec);
            } else if (rc == WSF_ERROR) {
                // Best-effort: send any queued CLOSE-echo bytes before
                // tearing down. econn_try_flush's OWN return tells us
                // whether it already closed `ec` (a real write error) —
                // NEVER re-derive that by re-reading `ec` afterward, which
                // would be a use-after-free the moment it already freed it.
                if (econn_try_flush(w, ec)) econn_close(w, ec);
                return;
            } else {
                break;   // WSF_NEED_MORE
            }
        }
        if ((size_t)r < sizeof tmp) return;   // short read: likely drained for now — next EPOLLIN will confirm
    }
}

static void *epoll_worker_main(void *arg) {
    thread_disable_cancellation();
    Worker *w = arg;
    struct epoll_event events[EPOLL_MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(w->epfd, events, EPOLL_MAX_EVENTS, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        bool woke = false;
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {   // the wake_evfd itself — see epoll_notify_game_changed / worker_handoff_push
                uint64_t v; ssize_t rd = read(w->wake_evfd, &v, sizeof v); (void)rd;
                woke = true;
                continue;
            }
            EConn *ec = events[i].data.ptr;
            if (ec->closed) continue;   // a handler earlier in THIS batch already closed it (deferred-freed) — its event is stale, skip it
            uint32_t evb = events[i].events;
            if (evb & (EPOLLHUP | EPOLLERR)) { econn_close(w, ec); continue; }
            if (evb & EPOLLOUT) { if (!econn_try_flush(w, ec)) continue; }
            if (evb & EPOLLIN) { if (ec->kind == ECONN_WS) handle_ws_readable(w, ec); }
        }
        if (woke) {
            drain_handoff_queue(w);
            worker_push_stale(w, NULL, NULL);   // don't know which game(s) a bot_thread notification was for — check them all
        }
        econn_drain_dead(w);   // free everything closed during this batch, now that no events[] entry can still reference it
    }
    return NULL;
}

// Pushes a freshly-prepared EConn (its first reply/handshake already
// encoded into wbuf) onto `w`'s handoff queue and wakes it — the ONE way an
// fd crosses from the dispatcher thread to its owning worker thread.
static void worker_handoff_push(Worker *w, EConn *ec) {
    atomic_fetch_add_explicit(&g_live_conns, 1, memory_order_relaxed);   // admitted: every epoll connection passes through here exactly once
    pthread_mutex_lock(&w->handoff_mtx);
    ec->next = NULL;
    if (w->handoff_tail) w->handoff_tail->next = ec; else w->handoff_head = ec;
    w->handoff_tail = ec;
    pthread_mutex_unlock(&w->handoff_mtx);
    uint64_t one = 1;
    ssize_t wr = write(w->wake_evfd, &one, sizeof one);
    (void)wr;
}

// --------------------------------------------------------------------------
// Dispatcher-side handlers (Stage 6). Called from main()'s accept loop, on
// the dispatcher thread, AFTER read_and_parse_request has already fully
// read this request (headers + body) — exactly the same blocking read every
// earlier stage did here. `conn`'s fd is still in its default BLOCKING mode
// at this point (flipped to non-blocking below, right before the handoff —
// safe because the dispatcher is the fd's sole owner until that instant).
// --------------------------------------------------------------------------

// /action, /state, /status, /meta — game_id-sharded one-shot HTTP. Builds
// the response with the UNCHANGED route()/h_action/h_state/h_status/h_meta
// handlers (a buffered Conn — conn.h — captures their output into memory
// instead of a real write()), then hands the connection to its shard's
// worker purely to flush that response non-blockingly and close.
static void epoll_dispatch_oneshot(Conn *conn, Req *r, char *raw_buf) {
    EConn *ec = calloc(1, sizeof *ec);
    if (!ec) { conn_close(conn); free(raw_buf); return; }
    ec->kind = ECONN_ONE_SHOT;
    ec->fd = conn->fd;
    ec->wbuf_cap = ONESHOT_WBUF_CAP;
    ec->wbuf = malloc((size_t)ec->wbuf_cap);
    if (!ec->wbuf) { free(ec); conn_close(conn); free(raw_buf); return; }

    Conn buffered; conn_init_buffered(&buffered, ec->wbuf, ec->wbuf_cap);
    route(r, &buffered);   // h_action/h_state/h_status/h_meta/h_stats — completely unchanged
    ec->wlen = buffered.buf_len;
    ec->close_after_flush = true;
    free(raw_buf);

    int fl = fcntl(ec->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(ec->fd, F_SETFL, fl | O_NONBLOCK);

    char gid[ID_LEN + 1] = {0};
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    worker_handoff_push(&g_workers[game_worker_index(gid)], ec);
}

// GET /ws (seated or ?spectator=1) — validates + completes the handshake
// HERE (ws_handshake_validate/ws_send_handshake_and_push — the exact checks
// and wire bytes ws_conn_thread has always produced), then hands the
// now-upgraded connection to its shard's worker. No thread is spawned.
static void epoll_dispatch_ws(Conn *conn, Req *r, char *raw_buf) {
    int seat; bool spectator; int cache_idx, viewer;
    GameSlot *s = ws_handshake_validate(r, &seat, &spectator, &cache_idx, &viewer);
    if (!s) { respond(conn, 401, "{\"error\":\"ws auth\"}"); conn_close(conn); free(raw_buf); return; }
    char accept[64];
    if (!ws_accept_from_key(r->ws_key, accept, sizeof accept)) {
        respond(conn, 400, "{\"error\":\"ws key\"}"); conn_close(conn); free(raw_buf); return;
    }

    EConn *ec = calloc(1, sizeof *ec);
    if (!ec) { conn_close(conn); free(raw_buf); return; }
    ec->kind = ECONN_WS;
    ec->fd = conn->fd;
    ec->slot = s; ec->seat = seat; ec->spectator = spectator; ec->cache_idx = cache_idx; ec->viewer = viewer;
    ec->phase = WSP_HDR2; ec->msg_opcode = -1;
    ec->wbuf_cap = WS_WBUF_CAP;
    ec->wbuf = malloc((size_t)ec->wbuf_cap);
    if (!ec->wbuf) { free(ec); conn_close(conn); free(raw_buf); return; }

    Conn buffered; conn_init_buffered(&buffered, ec->wbuf, ec->wbuf_cap);
    WsConn wc;
    if (!ws_send_handshake_and_push(&wc, buffered, accept, s, cache_idx, viewer)) {
        free(ec->wbuf); free(ec); conn_close(conn); free(raw_buf); return;
    }
    ec->wlen = wc.conn.buf_len; ec->woff = 0;
    pthread_mutex_lock(&s->lock);
    ec->last_pushed_version = s->version;
    pthread_mutex_unlock(&s->lock);

    // Any bytes the client pipelined past the upgrade request (the same
    // case ws_conn_prime primes into WsConn.pending for the thread-per-
    // connection path) — feed them straight into this connection's async
    // parser now, before the fd is even handed to its worker, so a
    // pipelined first move/poll is never lost.
    if (r->body_len > 0) {
        int off = 0, n = r->body_len; const unsigned char *body = (const unsigned char *)r->body;
        while (off < n) {
            int consumed = 0;
            int rc = wsasync_feed(ec, body + off, n - off, &consumed);
            off += consumed;
            if (rc == WSF_MESSAGE) {
                unsigned char reply[1 + 65536]; uint32_t v;
                int mtotal = ws_service_message(s, seat, spectator, cache_idx, viewer, ec->msg_buf, ec->last_msg_len, reply, &v);
                if (econn_reserve_out(ec, mtotal + 14) && ws_send_frame(&ec->wc_out, WS_OP_BIN, reply, mtotal) >= 0) {
                    econn_commit_out(ec);
                    ec->last_pushed_version = v;
                }
            } else if (rc != WSF_NEED_MORE) {
                break;   // a malformed pipelined frame — let the worker's own error handling take it from here once connected
            }
        }
    }
    free(raw_buf);

    int fl = fcntl(ec->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(ec->fd, F_SETFL, fl | O_NONBLOCK);

    worker_handoff_push(&g_workers[game_worker_index(s->id)], ec);
}

// --------------------------------------------------------------------------
// STAGE 3: TLS listener state. `g_tls_ctx` is built ONCE in main() — before
// the accept loop, before any worker/connection thread exists — off
// tls_server_ctx_create (conn.c), then only ever READ afterward (every
// accepted connection calls conn_tls_accept, which allocates its OWN fresh
// SSL* off this ctx; the ctx itself is never mutated post-setup), so
// sharing it read-only across every worker/connection thread is safe under
// OpenSSL 3's default library context — see TLS.md's Helgrind section for
// the verification. NULL means plaintext (the default, and the ONLY mode
// when --tls isn't passed) — main()'s accept loop branches on this exactly
// once per accepted connection.
// --------------------------------------------------------------------------
static SSL_CTX *g_tls_ctx = NULL;

// --------------------------------------------------------------------------
// Connection handling — the dispatcher (T2a Deliverable 2)
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// --bench-fanout=N: in-process scaling probe. The container's hard ulimit -n
// (4096) makes N real sockets impossible, but the SERVER's own scaling concerns
// are memory + the per-connection fan-out cost, neither of which needs a real
// fd. This builds N real EConns (2 per real dealt GameSlot) on one worker's
// ws_head and runs the actual push path (econn_push_view — state_put_cached_ptr
// + ws_send_frame2 into each wbuf, the exact code worker_push_stale runs),
// reporting RSS and fan-out throughput. Proves the data structures + hot loop
// hold at N; the socket count itself is an OS ulimit knob, not a server limit.
static long bench_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256]; long kb = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(f); return kb;
}
static void run_bench_fanout(int n_conns) {
    const int seats = 2;
    int n_games = (n_conns + seats - 1) / seats;
    fprintf(stderr, "[bench] target %d connections across %d 2-seat games (fd-free, in-process)\n", n_conns, n_games);
    long rss0 = bench_rss_kb();

    GameSlot **games = malloc(sizeof(GameSlot *) * (size_t)n_games);
    if (!games) { fprintf(stderr, "[bench] OOM games array\n"); return; }
    int gmade = 0;
    for (int gi = 0; gi < n_games; gi++) {
        int idx = g_games_count;
        GameSlot *s = game_slot_ensure(idx);
        if (!s) { fprintf(stderr, "[bench] game slot alloc failed at %d\n", gi); break; }
        g_games_count++;
        memset(s, 0, sizeof *s);
        s->slot_idx = idx;
        pthread_mutex_init(&s->lock, NULL);
        pthread_cond_init(&s->cond, NULL);
        for (int k = 0; k < MAX_PLAYERS + 1; k++) s->view_cache_version[k] = (uint32_t)-1;
        s->used = true;
        s->game.num_players = seats;
        for (int p = 0; p < seats; p++) s->game.players[p].strategy_key = STRATEGY_KEY_HUMAN;
        game_seat_and_deal(&s->game, NULL, seats);
        s->version = 1;
        games[gmade++] = s;
    }
    long rss_g = bench_rss_kb();

    Worker *w = &g_workers[0];
    int cmade = 0;
    for (int gi = 0; gi < gmade && cmade < n_conns; gi++)
        for (int seat = 0; seat < seats && cmade < n_conns; seat++) {
            EConn *ec = calloc(1, sizeof(EConn));
            if (!ec) { fprintf(stderr, "[bench] econn alloc failed at %d\n", cmade); goto built; }
            ec->kind = ECONN_WS; ec->fd = -1; ec->slot = games[gi];
            ec->seat = seat; ec->spectator = false; ec->cache_idx = seat; ec->viewer = seat;
            ec->last_pushed_version = 0;
            ec->wbuf_cap = WS_WBUF_CAP; ec->wbuf = malloc(WS_WBUF_CAP);
            if (!ec->wbuf) { fprintf(stderr, "[bench] wbuf OOM at %d\n", cmade); free(ec); goto built; }
            ec->prev = NULL; ec->next = w->ws_head;
            if (w->ws_head) w->ws_head->prev = ec;
            w->ws_head = ec; cmade++;
        }
built:;
    long rss_c = bench_rss_kb();
    fprintf(stderr, "[bench] built %d connections across %d games\n", cmade, gmade);
    fprintf(stderr, "[bench] RSS  start=%ld MB  +games=%ld MB  +conns=%ld MB  TOTAL=%ld MB\n",
            rss0 / 1024, (rss_g - rss0) / 1024, (rss_c - rss_g) / 1024, rss_c / 1024);
    if (gmade) fprintf(stderr, "[bench]   per-game=%.1f KB   per-conn=%.1f KB\n",
            (double)(rss_g - rss0) / gmade, cmade ? (double)(rss_c - rss_g) / cmade : 0.0);

    // Fan-out sweeps: bump every game's version so all conns are stale, then run
    // the real per-connection push encode over the whole ws_head list.
    int rounds = 5; long pushes = 0;
    for (EConn *ec = w->ws_head; ec; ec = ec->next) { uint32_t v; econn_push_view(ec, NULL, 0, false, &v, NULL); ec->wlen = ec->woff = 0; ec->last_pushed_version = 0; }  // warm caches
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < rounds; r++) {
        for (int gi = 0; gi < gmade; gi++) games[gi]->version++;
        for (EConn *ec = w->ws_head; ec; ec = ec->next) {
            uint32_t v; if (econn_push_view(ec, NULL, 0, true, &v, NULL) == 1) { ec->last_pushed_version = v; ec->wlen = ec->woff = 0; pushes++; }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "[bench] fan-out: %ld pushes in %.3fs = %.0f pushes/s (%.3f us/push); full sweep of %d conns = %.1f ms\n",
            pushes, secs, secs > 0 ? pushes / secs : 0, pushes ? secs / pushes * 1e6 : 0, cmade, rounds ? secs / rounds * 1000 : 0);
    fprintf(stderr, "[bench] DONE — no crash at %d connections / %d games, RSS %ld MB\n", cmade, gmade, rss_c / 1024);
}

static int   make_listener(int port, bool reuseport);   // defined just after main()
static void *acceptor_main(void *arg);                  // defined just after main()

int main(int argc, char **argv) {
    thread_disable_cancellation();   // main is acceptor[0]; runs the accept() loop — same cancel-bracket tax, see thread_disable_cancellation
    int port = 8099;
    // Default: DB ON (see DURABILITY.md — "Stage 2: persistence"). --no-db
    // opts all the way out (pure in-memory, e.g. for tests/benchmarks that
    // don't want a stray file); --db=PATH points at a specific file instead
    // of the default; --persist-interval-ms tunes the write-behind period
    // (50-100ms is the documented sweet spot — see persist.h).
    const char *db_path = "./foolish.db";
    int persist_interval_ms = 75;
    // Stage 3: TLS is OFF by default (plaintext, byte-for-byte the same as
    // every earlier stage) — `--tls --cert=PATH --key=PATH` turns the WHOLE
    // listen socket over to TLS (HTTPS + WSS); there is no mixed
    // plaintext+TLS listener in this design (see TLS.md for why a single
    // `--tls`-flips-the-listener design was chosen over a second
    // `--tls-port`).
    bool want_tls = false;
    const char *cert_path = NULL, *key_path = NULL;
    int bench_fanout = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--bench-fanout=", 15)) { bench_fanout = atoi(argv[i] + 15); continue; }
        if (!strncmp(argv[i], "--max-conns=", 12)) { g_max_conns = atoi(argv[i] + 12); continue; }
        if (!strncmp(argv[i], "--accept-threads=", 17)) {
            int nw = atoi(argv[i] + 17);
            if (nw > 0 && nw <= MAX_ACCEPT_THREADS) g_n_accept_threads = nw;
            continue;
        }
#ifdef FOOLISH_QUIC
        if (!strcmp(argv[i], "--quic")) { g_want_quic = true; continue; }
        if (!strncmp(argv[i], "--quic-port=", 12)) { g_quic_port = atoi(argv[i] + 12); continue; }
        if (!strncmp(argv[i], "--quic-workers=", 15)) { int n = atoi(argv[i] + 15); if (n > 0) g_quic_workers = n; continue; }
#endif
        if (!strncmp(argv[i], "--game-workers=", 15)) {
            int nw = atoi(argv[i] + 15);
            if (nw > 0 && nw <= MAX_GAME_WORKERS) g_n_game_workers = nw;
        } else if (!strncmp(argv[i], "--meta-workers=", 15)) {
            int nw = atoi(argv[i] + 15);
            if (nw >= 0 && nw <= MAX_META_WORKERS) g_n_meta_workers = nw;   // 0 = fold /meta onto the game pool
        } else if (!strncmp(argv[i], "--create-workers=", 17)) {
            int nw = atoi(argv[i] + 17);
            if (nw > 0 && nw <= MAX_CREATE_WORKERS) g_n_create_workers = nw;
        } else if (!strcmp(argv[i], "--no-db")) {
            db_path = NULL;
        } else if (!strncmp(argv[i], "--db=", 5)) {
            db_path = argv[i] + 5;
        } else if (!strncmp(argv[i], "--persist-interval-ms=", 22)) {
            int v = atoi(argv[i] + 22);
            if (v > 0) persist_interval_ms = v;
        } else if (!strcmp(argv[i], "--tls")) {
            want_tls = true;
        } else if (!strncmp(argv[i], "--cert=", 7)) {
            cert_path = argv[i] + 7;
        } else if (!strncmp(argv[i], "--key=", 6)) {
            key_path = argv[i] + 6;
        } else {
            int p = atoi(argv[i]);
            if (p > 0) port = p;
        }
    }
    srand((unsigned)(time(NULL) ^ getpid()));   // only ever called here, before any worker thread exists
    if (bench_fanout > 0) { run_bench_fanout(bench_fanout); return 0; }   // in-process scaling probe, then exit — see run_bench_fanout
    // Stage 4: resolve "octogen"'s STRAT_* brain id ONCE, before any
    // bot_thread can run — g_octogen_strat is read-only from every thread
    // after this line, so no lock is needed (same posture g_tls_ctx's doc
    // takes). Via bot_roster_at(...)->strat, NOT bot_roster_find's own
    // return value directly — that's a roster ARRAY INDEX, a different
    // number from the STRAT_* id seats actually carry (see h_meta's add-bot
    // branch for the full story; this counter needs to compare against the
    // SAME value strategy_key holds). -1 (an unknown key, or a somehow-
    // absent roster entry) just means the per-strategy octogen counter never
    // increments — g_bot_decisions still counts every bot's applied actions
    // regardless.
    {
        int ridx = bot_roster_find("octogen");
        const BotRosterEntry *e = ridx >= 0 ? bot_roster_at(ridx) : NULL;
        g_octogen_strat = e ? e->strat : -1;
    }
    // Long-lived /ws connections mean a peer can vanish (crash, network
    // reset, the load client's own `timeout` cutting it off) between our
    // read and our next write; the default SIGPIPE action is to kill the
    // WHOLE PROCESS on that write. Ignore it — write() already reports the
    // same failure as -1/EPIPE, which every write path here already checks
    // (and, under TLS, conn_read/conn_write (conn.c) translate the
    // equivalent SSL_ERROR_SYSCALL the same way — see their doc).
    signal(SIGPIPE, SIG_IGN);

    // Stage 3: build the shared server SSL_CTX ONCE, before any worker pool
    // or the accept loop starts (same "set up before any other thread can
    // touch it" posture persist_start takes for crash recovery, just for a
    // different piece of startup state) — see g_tls_ctx's own doc above. A
    // REQUESTED (--tls passed) but failed TLS setup is fatal, never a
    // silent downgrade to plaintext: same posture Stage 2 takes for a
    // requested-but-failed --db (see persist_start's doc) — serving
    // plaintext when the operator asked for TLS would be a silent security
    // regression, worse than refusing to start.
    if (want_tls) {
        if (!cert_path || !key_path) {
            fprintf(stderr, "fatal: --tls requires --cert=PATH --key=PATH\n");
            return 1;
        }
        g_tls_ctx = tls_server_ctx_create(cert_path, key_path);
        if (!g_tls_ctx) {
            fprintf(stderr, "fatal: TLS setup failed (--cert=%s --key=%s) — "
                             "check the cert/key are valid PEM and the key matches the cert\n",
                    cert_path, key_path);
            return 1;
        }
    }

    // Stage 2: the round-trip codec gate (always runs, --no-db or not — it's
    // a pure in-memory check of serialize_slot/deserialize_slot, not the DB
    // itself), then registering the two durable tables and starting
    // persistence — BEFORE any worker pool or the accept loop exists, so
    // crash recovery's synchronous load (inside persist_start) runs with no
    // other thread able to touch g_users[]/g_games[] yet. A requested
    // (non-NULL) --db that fails to open/configure is fatal: silently
    // downgrading a requested durability guarantee to "pretend it's fine"
    // would be worse than refusing to start.
    persist_self_test();
    g_game_table = persist_register_table("games", MAX_GAMES, PERSIST_GAME_BLOB_CAP,
                                           game_persist_snapshot, game_persist_load);
    g_user_table = persist_register_table("users", MAX_USERS, PERSIST_USER_BLOB_CAP,
                                           user_persist_snapshot, user_persist_load);
    if (!persist_start(db_path, persist_interval_ms)) {
        fprintf(stderr, "fatal: persistence failed to start (--db=%s)\n", db_path ? db_path : "(null)");
        return 1;
    }

    wq_init(&g_auth_create_q);
    wq_init(&g_meta_q);
    for (int i = 0; i < g_n_game_workers; i++) wq_init(&g_game_q[i]);

    // /auth/* and /create stay on their own threaded worker pool in BOTH
    // modes (task-granted: "the dispatcher and create/auth worker can stay
    // threads") — no game_id to shard by, never the hot path (see
    // SERVER_SCALING.md's Deliverable 2 sweep — neither pool was ever close
    // to contended).
    pthread_t wt;
    for (int i = 0; i < g_n_create_workers; i++)
        if (pthread_create(&wt, NULL, worker_thread, &g_auth_create_q) == 0) pthread_detach(wt);

    if (g_tls_ctx) {
        // Stage 6 TLS fallback: non-blocking OpenSSL's WANT_READ/WANT_WRITE
        // state machine is out of this stage's budget (see
        // SERVER_SCALING.md "Stage 6" for the honest writeup) — a --tls
        // server keeps the ENTIRE pre-Stage-6 design, byte-for-byte:
        // thread-per-/ws-connection + the typed HTTP work-queue pools.
        for (int i = 0; i < g_n_meta_workers; i++)
            if (pthread_create(&wt, NULL, worker_thread, &g_meta_q) == 0) pthread_detach(wt);
        for (int i = 0; i < g_n_game_workers; i++)
            if (pthread_create(&wt, NULL, worker_thread, &g_game_q[i]) == 0) pthread_detach(wt);
    } else {
        // Stage 6: plaintext runs epoll-per-shard instead of the typed game/
        // meta queues + thread-per-/ws-connection — `--game-workers=N` now
        // ALSO sizes the epoll shard count (same knob, see
        // SERVER_SCALING.md). g_game_q/g_meta_q/worker_thread stay fully
        // intact above (unused in this mode) purely for the --tls fallback.
        for (int i = 0; i < g_n_game_workers; i++) {
            Worker *w = &g_workers[i];
            w->idx = i;
            w->epfd = epoll_create1(0);
            w->wake_evfd = eventfd(0, EFD_NONBLOCK);
            if (w->epfd < 0 || w->wake_evfd < 0) {
                fprintf(stderr, "fatal: epoll worker %d setup failed (epoll_create1/eventfd)\n", i);
                return 1;
            }
            pthread_mutex_init(&w->handoff_mtx, NULL);
            w->handoff_head = w->handoff_tail = NULL;
            w->ws_head = NULL;
            struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };   // NULL data.ptr marks the wake_evfd itself — see epoll_worker_main
            if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->wake_evfd, &ev) < 0) {
                fprintf(stderr, "fatal: epoll worker %d setup failed (epoll_ctl)\n", i);
                return 1;
            }
            if (pthread_create(&wt, NULL, epoll_worker_main, w) != 0) {
                fprintf(stderr, "fatal: epoll worker %d thread create failed\n", i);
                return 1;
            }
            pthread_detach(wt);
        }
        g_epoll_active = true;
    }

#ifdef FOOLISH_QUIC
    // QUIC/HTTP3/WebTransport listener on its own thread, sharing this
    // process's game state through game_bridge.h. QUIC carries TLS 1.3 itself,
    // so it needs the same cert/key the TCP --tls path would (there is no
    // plaintext QUIC). Runs beside the TCP acceptors below.
    if (g_want_quic) {
        if (!cert_path || !key_path) {
            fprintf(stderr, "fatal: --quic requires --cert=PATH --key=PATH (QUIC has no plaintext mode)\n");
            return 1;
        }
        static struct QuicArgs qa;
        qa.port = g_quic_port > 0 ? g_quic_port : port;
        qa.workers = g_quic_workers;
        qa.cert = cert_path;
        qa.key  = key_path;
        pthread_t qt;
        if (pthread_create(&qt, NULL, quic_thread_main, &qa) == 0) pthread_detach(qt);
        else fprintf(stderr, "warning: QUIC listener thread failed to start\n");
    }
#endif

    // SO_REUSEPORT multi-acceptor bring-up. Create g_n_accept_threads
    // listeners, each bound to `port` with SO_REUSEPORT so the kernel spreads
    // inbound connections across them; run one acceptor loop per listener.
    // (See make_listener / acceptor_main and the g_n_accept_threads doc.)
    int listeners[MAX_ACCEPT_THREADS];
    int n_listeners = 0;
    for (int i = 0; i < g_n_accept_threads; i++) {
        int fd = make_listener(port, /*reuseport=*/true);
        if (fd < 0) {
            if (n_listeners == 0) return 1;   // couldn't bind even one — fatal
            fprintf(stderr, "warning: bound only %d/%d acceptor listeners\n",
                    n_listeners, g_n_accept_threads);
            break;
        }
        listeners[n_listeners++] = fd;
    }

    fprintf(stderr, "foolish native server (kernel-driven, in-memory + SQLite write-behind%s) on :%d "
            "(accept-threads=%d game-workers=%d meta-workers=%d create-workers=%d db=%s interval=%dms)\n",
            g_tls_ctx ? " + TLS (https/wss)" : "",
            port, n_listeners, g_n_game_workers, g_n_meta_workers, g_n_create_workers,
            db_path ? db_path : "off (--no-db)", persist_interval_ms);

    // Spawn acceptors [1..n_listeners-1] as detached threads; run acceptor
    // [0] on this (main) thread so main() blocks here forever, exactly as the
    // old single accept loop did. The kernel decides which listener each new
    // connection lands on (4-tuple hash).
    for (int i = 1; i < n_listeners; i++) {
        pthread_t at;
        if (pthread_create(&at, NULL, acceptor_main, (void *)(intptr_t)listeners[i]) == 0)
            pthread_detach(at);
        else
            fprintf(stderr, "warning: acceptor thread %d failed to start\n", i);
    }
    acceptor_main((void *)(intptr_t)listeners[0]);   // never returns
    return 0;
}

// Create a listening socket bound to `port`. With reuseport, sets
// SO_REUSEPORT so multiple sockets can share the port and the kernel
// load-balances accepts across them (the SO_REUSEPORT multi-acceptor model);
// it also lets a redeploy bind the port before the old process has fully
// exited. Backlog is deliberately large — a server aiming at tens of
// thousands of connections must not drop SYNs during an arrival burst.
// Returns the fd, or -1 on failure (the caller decides fatal-ness).
static int make_listener(int port, bool reuseport) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt) < 0)
        perror("setsockopt(SO_REUSEPORT)");   // non-fatal: a single listener still works
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 1024) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

// One acceptor thread: owns `srv` (its own SO_REUSEPORT listener, passed as
// an int in arg) and runs the accept -> (TLS handshake) -> read+parse ->
// dispatch loop below. N of these run concurrently, one per listener. The
// loop is exactly what the single dispatcher used to run — replicated across
// threads, not otherwise changed; game-affinity is preserved because each
// dispatch path still hands off to the epoll worker that owns the game
// (worker_handoff_push / classify_queue). See SERVER_SCALING.md.
static void *acceptor_main(void *arg) {
    int srv = (int)(intptr_t)arg;
    thread_disable_cancellation();   // per-thread cancel bracket; each acceptor pays it once
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        // Admission control: at the connection ceiling, shed the new one
        // immediately (a closed fd, not an OOM). The OS listen backlog already
        // absorbs bursts; this is the app-level bound that keeps a connection
        // flood from exhausting memory faster than the fd limit alone would.
        if (g_max_conns > 0 && atomic_load_explicit(&g_live_conns, memory_order_relaxed) >= g_max_conns) {
            close(fd);
            continue;
        }
        // A one-shot HTTP request/response barely notices Nagle's algorithm,
        // but a persistent /ws connection does many small back-and-forth
        // writes+reads — without TCP_NODELAY, Nagle batching interacting
        // with the peer's delayed ACKs turns each round trip into tens of
        // milliseconds instead of tens of microseconds.
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        // Stage 3: SSL_accept happens HERE, on this connection's own
        // servicing path, before any HTTP parsing — a fresh SSL* per
        // connection, off the one shared g_tls_ctx (see its doc above). A
        // failed/abandoned handshake (a port scanner, a plaintext probe
        // against a TLS listener, a client that hangs up mid-handshake) is
        // just a dropped connection, same as a malformed plaintext request
        // below — never fatal to the process.
        Conn conn;
        if (g_tls_ctx) {
            if (!conn_tls_accept(&conn, g_tls_ctx, fd)) { close(fd); continue; }
        } else {
            conn_init_plain(&conn, fd);
        }

        char *buf = malloc(REQ_BUF_CAP);
        if (!buf) { conn_close(&conn); continue; }
        Req r;
        if (!read_and_parse_request(&conn, buf, REQ_BUF_CAP, &r)) { conn_close(&conn); free(buf); continue; }

        if (r.is_ws_upgrade && !strcmp(r.method, "GET") && !strcmp(r.path, "/ws")) {
            if (g_tls_ctx) {
                // Stage 6 TLS fallback: dedicated per-connection thread
                // (design B, see ws_conn_thread's doc) — this thread lives
                // for the client's whole session, so the pthread_create
                // cost is paid once per client instead of once per action.
                // See PROFILE_HOTPATH.md T1b.
                WsSpawnArg *sa = malloc(sizeof *sa);
                if (!sa) { conn_close(&conn); free(buf); continue; }
                sa->conn = conn; sa->req = r; sa->raw_buf = buf;
                pthread_t t;
                if (pthread_create(&t, NULL, ws_conn_thread, sa) == 0) pthread_detach(t);
                else { conn_close(&conn); free(buf); free(sa); }
            } else {
                // Stage 6 plaintext: no thread — hands the now-upgraded
                // connection to its shard's epoll worker. See
                // epoll_dispatch_ws's doc.
                epoll_dispatch_ws(&conn, &r, buf);
            }
            continue;
        }

        // Cheap, store-free routes: answer inline instead of paying a queue
        // round trip for them, in EITHER mode (no game_id, never worth
        // sharding to an epoll worker just to flush a few bytes).
        if (!strcmp(r.method, "OPTIONS")) { respond(&conn, 200, "{}"); conn_close(&conn); free(buf); continue; }
        if (!strcmp(r.path, "/health"))  { respond(&conn, 200, "{\"ok\":true}"); conn_close(&conn); free(buf); continue; }
        if (!strcmp(r.path, "/stats"))   { h_stats(&r, &conn); conn_close(&conn); free(buf); continue; }

        if (g_tls_ctx) {
            // Stage 6 TLS fallback: unchanged typed HTTP work-queue pools.
            WorkItem item; item.conn = conn; item.req = r; item.raw_buf = buf;
            wq_push(classify_queue(&r), &item);
            continue;
        }
        // Stage 6 plaintext: /auth/signup, /auth/signin, /create have no
        // game_id to shard by — stay on the threaded create/auth pool
        // (started above, both modes). Everything else here
        // (/action,/state,/status,/meta) is game_id-sharded — hand it to
        // its shard's epoll worker instead of the (now TLS-only) typed
        // game/meta queues.
        if (!strcmp(r.path, "/auth/signup") || !strcmp(r.path, "/auth/signin") || !strcmp(r.path, "/create")) {
            WorkItem item; item.conn = conn; item.req = r; item.raw_buf = buf;
            wq_push(&g_auth_create_q, &item);
        } else {
            epoll_dispatch_oneshot(&conn, &r, buf);
        }
    }
    return NULL;   // unreachable — the accept loop above never terminates
}

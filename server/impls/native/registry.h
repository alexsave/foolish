// registry.h - the live table of games and users, and the locks that guard it.
//
// This is the "fake DB" of the native server: every game is a `GameSlot`
// holding the kernel's own `Game` struct plus the identity the kernel
// deliberately never carries (game.h: identity lives with the host), every
// account is a `User`, and both are found by id through open-addressing hash
// maps. It owns the two-tier locking scheme the whole server follows (see
// "Locking" below), the id minting, the seat helpers every lobby/play path
// calls, and the reaper that gives slots back when a game goes quiet.
//
// Nothing here decides a game rule. Seating and dealing are the kernel's
// (game_lobby_seat / game_seat_and_deal); this file only decides where the
// state lives and who may touch it when.
#ifndef FOOLISH_REGISTRY_H
#define FOOLISH_REGISTRY_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "game.h"
#include "roster.h"     // the seats' ids and names, beside the Game (game.h Player carries none)

// Games and users are APPEND-ONLY in this in-memory server (a slot's `used`
// flag is created true and never cleared). They live in lazily-allocated,
// fixed-size CHUNKS rather than one giant static array, so RSS tracks the number
// of live games (each GameSlot is ~48 KB) instead of a compile-time ceiling, and
// a chunk's address never moves - every GameSlot*/User* handed out (the hash
// tables, EConn->slot, bot_thread) stays valid for the process lifetime.
// MAX_GAMES/MAX_USERS are now just the (very high) ceilings; memory is spent per
// chunk actually touched. See g_game_chunks / game_slot_ensure in registry.c.
#define GAMES_PER_CHUNK   512
#define USERS_PER_CHUNK   4096
#define MAX_GAME_CHUNKS   512                       // ceiling 262,144 live games
#define MAX_USER_CHUNKS   128                       // ceiling 524,288 users (kept == the token hash's half-load point)
#define MAX_GAMES (GAMES_PER_CHUNK * MAX_GAME_CHUNKS)
#define MAX_USERS (USERS_PER_CHUNK * MAX_USER_CHUNKS)
#define ID_LEN 12

typedef struct {
    bool used;
    // No stored token: session tokens are now stateless signed blobs verified by
    // HMAC (see session.h make_token/verify_token) - a user is looked up by the
    // user_id the token carries, not by a token->user table. Users are indexed by
    // user_id (g_userid_ht) and username (g_username_ht).
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
// spectator viewer (VIEW_SPECTATOR, Stage 4) masks EVERY hand and the deck -
// its output is never bigger than any per-seat view (same field counts, just
// more of them hidden), so it fits this same cap with no change.
#define VIEW_CACHE_CAP 1024

// Stage 4 (SERVER_SCALING.md "Stage 4 - spectators"): the shared cache slot
// for the one masked view every spectator of a game sees (VIEW_SPECTATOR -
// all hands + the deck hidden, the SAME bytes for every spectator of this
// game at a given version, unlike a per-seat view). One extra slot past the
// MAX_PLAYERS per-seat ones in GameSlot.view_cache*, keyed the same way
// (recompute iff view_cache_version[SPECTATOR_CACHE_IDX] != s->version).
#define SPECTATOR_CACHE_IDX MAX_PLAYERS

typedef struct {
    bool used;
    char id[ID_LEN + 1];
    Game game;                      // THE kernel state - incl. its own lifecycle status
    // Lobby roster (identity lives beside the state blob, never in it - game.h).
    // `roster` is the kernel's Roster (roster.h): seat i's id and name, and a
    // brain for a bot seat. Seats are only ever appended, through seat_add, so
    // roster.n == game.num_players always.
    char owner[ID_LEN + 1];
    Roster roster;
    bool seat_ready[MAX_PLAYERS];             // lobby "hit ready" - host state; kind (human/bot) lives in the kernel's strategy_key
    // The secret base every bot decision's streams are seeded from
    // (bot_drive_seed_decision), derived from the deal seed at each deal and
    // persisted with the game, so a bot's move is a function of this game alone.
    uint32_t rng_base;
    // Per-game lock (T2a). Guards EVERYTHING below this point plus the
    // `game` and lobby-roster fields above: this slot's whole game state,
    // once it is reachable via g_game_ht. See the "Locking" block below
    // g_registry_lock's declaration for the two-tier scheme and the lock
    // order invariant every handler in this server follows.
    pthread_mutex_t lock;
    // One per-game trampoline thread paces the bots (see bots.c bot_thread). It
    // waits on `cond` when a human is owed; /action signals it. `bot_running`
    // guards against spawning a second driver. Both now pair with `lock` (above),
    // not a process-wide mutex.
    pthread_cond_t cond;
    bool bot_running;
    int  slot_idx;   // this game's append index, for persist_mark_dirty (was `s - g_games`); runtime-only, BELOW `lock` so it is outside the serialized prefix
    // Game reclamation (bounded memory): the reaper may recycle this slot ONLY
    // when conn_refs==0 AND !bot_running AND it has been idle past
    // GAME_IDLE_TTL_US - so no thread can still hold a stale GameSlot* (an
    // EConn->slot or a --tls ws_conn_thread). conn_refs counts live /ws
    // connections referencing this slot; last_active_us (CLOCK_MONOTONIC) is
    // refreshed on create, every state change (game_mark_dirty), and every
    // connection attempt/open/close. Both runtime-only, below `lock`, outside
    // the serialized prefix (never persisted).
    int      conn_refs;
    uint64_t last_active_us;
    // PROFILE_HOTPATH.md "T1c": under WS+legal load, view.c:state_put was the
    // single hottest function (~21-28% of instructions, inclusive) because
    // /ws's hot loop called it on EVERY round trip, including a pure "poll"
    // that changed nothing. `version` is bumped every time this slot's Game
    // could have changed (a human's move applied via /action or /ws, a lobby
    // transition, or a bot_drive cycle that applied >=1 action) - never on a
    // no-op poll. `view_cache`/`view_cache_len` hold the last state_put(...)
    // bytes computed for each seat, and `view_cache_version` records which
    // `version` they were computed at; state_put_cached (play.h) recomputes
    // only when the two disagree, else memcpy's the cached bytes. Same wire
    // bytes either way - this is a pure CPU-cost cut, not a protocol change,
    // so the client needs no changes and always sees fresh-as-of-version
    // state on every round trip.
    uint32_t version;
    // +1: index SPECTATOR_CACHE_IDX (== MAX_PLAYERS) is the shared spectator
    // cache slot (Stage 4) - see that macro's doc above.
    unsigned char view_cache[MAX_PLAYERS + 1][VIEW_CACHE_CAP];
    int      view_cache_len[MAX_PLAYERS + 1];
    uint32_t view_cache_version[MAX_PLAYERS + 1];
} GameSlot;

// Chunk directories: g_*_chunks[c] is NULL until slot c*PER_CHUNK is first
// touched, then a calloc'd block that never moves or frees. g_*_count is the
// append high-water. Users are append-only; GAME slots are recycled - the
// reaper reclaims quiescent games and game_alloc_slot reuses their indices via
// a free-list, so g_games_count is the high-water, not the live count (see
// SERVER_SCALING.md "Stage 7" - game reclamation).
extern int g_games_count;
extern int g_users_count;
extern int g_free_count;   // reclaimed slot indices waiting to be reused

// idx -> slot, allocating the backing chunk on first touch (this is where RAM
// grows with live games). Returns NULL past the ceiling or on OOM.
GameSlot *game_slot_ensure(int idx);
User     *user_slot_ensure(int idx);
// Read-only accessors (no allocation) - valid for any idx a slot was created at.
GameSlot *game_slot_at(int idx);
User     *user_slot_at(int idx);

uint64_t now_us(void);

// --------------------------------------------------------------------------
// Locking (T2a introduced the two-tier scheme below, replacing the single
// global g_lock; Stage 5, SERVER_SCALING.md "Stage 5 - parallel bot compute",
// DROPPED the THIRD lock T2a added on top of it - read on for why that is now
// safe). Two tiers remain:
//
//   g_registry_lock - small and SHORT-HELD. Guards ONLY: g_users[] (signup /
//     user_id lookup) + g_userid_ht/g_username_ht, game-slot allocation (claiming
//     a free g_games[] entry) + g_game_ht (game_id -> GameSlot*). Never held during
//     game work, bot work, or socket I/O.
//
//   GameSlot.lock (per game) - guards everything else about ONE game: its
//     `Game` struct, lobby roster (roster/seat_ready/owner),
//     cond/bot_running, and the per-seat view_cache. This is now the ONLY
//     lock taken around a kernel-mutating call (awire_apply, bot_drive,
//     game_seat_and_deal).
//
// T2a's g_kernel_lock (REMOVED, Stage 5): a third, process-wide mutex held
// around every kernel call that mutates a Game or drives bots, because the
// kernel (c/src, read-only to us - see foolish_server.c's header) used to keep
// process-wide, non-thread-local scratch state across those calls:
// bot_drive.c's `g_scratch` eligibility buffer, game.c's `engine_last_reject`
// (the reject-reason out-param every handle_* writes) and `engine_snap_hook`
// (saved/restored by bot_drive's `choose_move`), plus two more the Stage 5
// kernel audit found beyond that original list - game.c's log-overflow
// scratch `GameLog` inside `log_alloc`, and cordite_sim.c's lazily-built
// card-id lookup masks (`ensure_masks`), which every cordite/octogen decision
// touches via `cd_sim_from_game`. Under the OLD single global g_lock this was
// safe by accident (the whole server was one critical section, so no two
// kernel-mutating calls ever ran concurrently); per-game locks alone
// reintroduced exactly the concurrent-mutation-across-DIFFERENT-games case
// those kernel statics were never built for - confirmed by a Helgrind run on
// an early per-game-lock-only build (a genuine write/write race on
// `engine_last_reject` between two games' threads, SERVER_SCALING.md "T2a").
// g_kernel_lock was the honest fix at the time, but it also serialized every
// game's bot COMPUTE process-wide - Stage 4 measured octogen decisions/s
// plateauing at ~30/s (the single-thread ceiling) no matter how many games
// ran concurrently. Stage 5 (c/src/*, see game.h/game.c/bot_drive.c/
// cordite_sim.c) made every one of those kernel statics `_Thread_local`
// instead of removing the lock outright: each thread now owns its own
// instance, so two DIFFERENT games' kernel calls on two DIFFERENT threads no
// longer share any mutable kernel state - only same-game concurrency needs
// serializing, and GameSlot.lock already does that. Verified by Helgrind on
// this build (0 kernel/server data races - see SERVER_SCALING.md "Stage 5")
// and by the difftest suite (single-threaded, so `_Thread_local` is
// transparent there - byte-identical play, proving the kernel change itself
// carries no behavior difference).
//
// LOCK ORDER (deadlock-freedom): registry, then game - ALWAYS, and never the
// reverse. Every handler that needs the registry takes g_registry_lock, finds
// the User*/GameSlot*, takes the GameSlot's own lock, THEN releases
// g_registry_lock (never re-acquired while any GameSlot.lock is held). No
// handler here ever holds two GameSlot locks. bot_thread and the /ws dedicated
// connection thread (ws_conn_thread) follow the same rule: each only ever
// holds its own game's lock - neither touches g_registry_lock after its
// initial (game_id -> GameSlot*) lookup.
// --------------------------------------------------------------------------
extern pthread_mutex_t g_registry_lock;
// g_seq: a monotonic counter mixed into gen_id() (registry-guarded - every
// caller holds g_registry_lock) AND read directly by h_meta's "start" branch
// to help season the deal seed (which runs under a GameSlot lock, not the
// registry lock - a Helgrind-caught race in an earlier build of this server).
// Rather than invent a case where the lock order above would need registry-
// while-holding-game (forbidden), it's simplest and correct to make the
// counter itself atomic and drop the lock story entirely - it has no other
// invariant to protect, just "some number that goes up."
extern atomic_ulong g_seq;

void gen_id(char *out, int n);

// FNV-1a over a NUL-terminated key. Shared with the transport, which shards a
// game's connections onto a worker by the same hash its requests use
// (game_worker_index) so both agree on exactly one owning worker.
unsigned long hash_str(const char *s);

// Game reclamation (bounded memory): recycle finished/abandoned slots so total
// memory is bounded by PEAK concurrent games, not cumulative created.
// --game-idle-ttl-s / --reap-interval-s tune the two knobs.
#define GAME_IDLE_TTL_US   (60ULL * 1000000ULL)   // default: reclaim a ref-free, bot-free game after 60s idle
#define GAME_REAP_INTERVAL 10                        // default reaper scan period (seconds)
extern uint64_t g_game_idle_ttl_us;   // --game-idle-ttl-s=N (0 = as soon as quiescent)
extern int      g_reap_interval;      // --reap-interval-s=N
extern atomic_ulong g_games_reclaimed;   // /stats gauge: total slots recycled

// Allocate a game slot for a NEW game: reuse a reclaimed slot if the free-list
// has one (bounded memory), else append a fresh one. Owns each slot's lock/cond
// lifecycle - a fresh slot's are initialized once here; a reused slot's are
// preserved (never re-initialized). The returned slot is fully cleared with its
// lock/cond ready; the caller just fills fields and publishes it. Caller holds
// g_registry_lock. Returns NULL only at hard capacity (MAX_GAMES live at once).
GameSlot *game_alloc_slot(int *out_idx);

// A live /ws connection now references / no longer references slot s (locking
// variants, for callers that don't already hold s->lock - econn_close and the
// --tls ws_conn_thread). Refreshing last_active on ref/unref means a slot with
// an open connection, or one opened/closed within the idle window, is never
// eligible for reclamation. conn_refs is guarded by s->lock.
void game_conn_ref(GameSlot *s);
void game_conn_unref(GameSlot *s);

// Called under `s->lock` at every point this slot's Game or lobby roster could
// have changed - the exact same events that bump `s->version` (a human move via
// /action or /ws, a lobby transition in /meta, a bot_drive cycle that applied
// >=1 action or ended the game, or a fresh /create). O(1) and never touches
// disk: just flips one bool in persist.c's dirty bitmap for this slot's index
// and signals the persistence thread - see persist.h's top comment for the full
// write-behind model and DURABILITY.md for the design writeup + measurements.
void game_mark_dirty(GameSlot *s);

// Hash-map maintenance + lookups. Every one of these requires g_registry_lock.
bool      userid_ht_insert(User *u);
bool      username_ht_insert(User *u);
User     *user_by_id(const char *user_id);
User     *user_by_username(const char *name);
bool      game_ht_insert(GameSlot *s);
void      game_ht_remove(GameSlot *s);
GameSlot *game_by_id(const char *id);

// Whether a seat is a bot: the kernel's own per-seat fact now (strategy_key),
// not a server-side is_ai array. A human seat is STRATEGY_KEY_HUMAN; a bot holds
// its roster index. Caller MUST hold `s`'s own lock (reads s->game).
bool seat_is_bot(const Game *g, int i);

// Caller MUST hold `s`'s own lock (reads s->game and s->roster).
int seat_of(GameSlot *s, const char *user_id);

// Whether the kernel's Roster would seat this display name (UTF-8, and its
// trim). Asked at signup and at create, so a name the roster refuses never
// reaches a lobby.
bool roster_name_ok(const char *name);

// Seats a player in the Roster and the Game together, so roster.n stays
// game.num_players. The roster goes first because it is the side that refuses
// on identity (a name, a duplicate id); when the kernel then refuses the seat
// (a full or dealt table), the roster seat is taken back. Returns the seat, or
// -1. Caller MUST hold `s`'s own lock.
int seat_add(GameSlot *s, int strategy_key, const char *id, const char *name, const char *brain);

// The reaper thread entry (started by main in both TLS and plaintext modes).
void *game_reaper_thread(void *arg);

#endif

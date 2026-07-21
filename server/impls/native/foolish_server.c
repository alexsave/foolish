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
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "legal.h"
#include "view.h"
#include "awire.h"
#include "bot_drive.h"
#include "bot_roster.h"
#include "ws.h"
#include "persist.h"
#include "conn.h"   // Stage 3: TLS — see conn.h and the "STAGE 3: OpenSSL TLS" block below

// --------------------------------------------------------------------------
// In-memory store (the "fake DB"): games + users, per-game locks.
// --------------------------------------------------------------------------

#define MAX_GAMES 256
#define MAX_USERS 512
#define ID_LEN 12

typedef struct {
    bool used;
    char token[33];
    char user_id[ID_LEN + 1];
    char username[24];
} User;

// Worst-case state_put(...) output for this build's caps: 16-byte header +
// MAX_DECK (64) card bytes + 1 + MAX_BATTLES*2 (32*2) + MAX_PLAYERS*(3 +
// MAX_HAND_SIZE) (8*(3+64)) + 1 + MAX_PLAYERS (8) = 690 bytes. Rounded up
// with real margin for VIEW_CACHE below (h_ws/h_state's own wire buffers use
// a much larger 65536 "don't think about it" cap; this one is sized because
// it's paid MAX_PLAYERS times per game, see GameSlot.view_cache).
#define VIEW_CACHE_CAP 1024

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
    unsigned char view_cache[MAX_PLAYERS][VIEW_CACHE_CAP];
    int      view_cache_len[MAX_PLAYERS];
    uint32_t view_cache_version[MAX_PLAYERS];
} GameSlot;

static User     g_users[MAX_USERS];
static GameSlot g_games[MAX_GAMES];

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
    if (g_game_table) persist_mark_dirty(g_game_table, (int)(s - g_games));
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
// Locking (T2a — replaces the single global g_lock). Two tiers:
//
//   g_registry_lock — small and SHORT-HELD. Guards ONLY: g_users[] (signup /
//     token lookup) + g_token_ht, game-slot allocation (claiming a free
//     g_games[] entry) + g_game_ht (game_id -> GameSlot*). Never held during
//     game work, bot work, or socket I/O.
//
//   GameSlot.lock (per game) — guards everything else about ONE game: its
//     `Game` struct, lobby roster (seat_user/seat_name/seat_ready/owner),
//     cond/bot_running, and the per-seat view_cache.
//
//   g_kernel_lock — a THIRD lock, small and narrowly scoped, held ONLY around
//     the specific kernel calls that mutate a Game or drive bots (awire_apply,
//     bot_drive, game_seat_and_deal — see each call site's comment). This one
//     is NOT optional and is not a cautious extra: the kernel (c/src, read-
//     only to us — see this file's header) keeps process-wide, non-thread-
//     local scratch state across calls that only a single external caller was
//     ever assumed to touch at a time. `bot_drive.c` says so directly —
//     "`g_scratch` ... is safe: bot_drive is never re-entered" — and
//     `game.c`'s `engine_last_reject` (the reject-reason out-param every
//     handle_* writes) and `engine_snap_hook` (saved/restored by bot_drive's
//     `choose_move`) are the same story. Under the OLD single global g_lock
//     this was safe by accident (the whole server was one critical section,
//     so no two kernel-mutating calls ever ran concurrently); per-game locks
//     alone reintroduce exactly the concurrent-mutation-across-DIFFERENT-
//     games case those kernel statics were never built for. Confirmed by a
//     Helgrind run on an early per-game-lock-only build: a genuine write/
//     write race on `engine_last_reject` between two games' threads (see
//     SERVER_SCALING.md "T2a" for the report and the source audit that found
//     the rest). g_kernel_lock is the honest fix, not a workaround: it does
//     NOT serialize state reads (state_put/state_put_cached never touch
//     these statics — confirmed by inspection, so h_state/h_status/WS polls
//     stay fully per-game-lock-parallel), only the actual kernel writes.
//
// LOCK ORDER (deadlock-freedom): registry, then game, then kernel — ALWAYS,
// and never the reverse at any step. Every handler below that needs the
// registry takes g_registry_lock, finds the User*/GameSlot*, takes the
// GameSlot's own lock, THEN releases g_registry_lock (never re-acquired
// while any GameSlot.lock is held). g_kernel_lock, when needed, is taken
// LAST — only while already holding the relevant GameSlot.lock — held for
// just the kernel call, and released before anything else; no handler here
// ever holds two GameSlot locks, or acquires g_registry_lock or a GameSlot
// lock while already holding g_kernel_lock. bot_thread and the /ws dedicated
// connection thread (ws_conn_thread) follow the same rule: each only ever
// holds its own game's lock (plus g_kernel_lock, innermost, around its
// kernel call) — neither touches g_registry_lock after its initial
// (game_id -> GameSlot*) lookup.
// --------------------------------------------------------------------------
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_kernel_lock = PTHREAD_MUTEX_INITIALIZER;
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
#define TOKEN_HT_SIZE 1024   // power of two, > 2x MAX_USERS
#define GAME_HT_SIZE   512   // power of two, > 2x MAX_GAMES

static User     *g_token_ht[TOKEN_HT_SIZE];
static GameSlot *g_game_ht[GAME_HT_SIZE];

static unsigned long hash_str(const char *s) {
    unsigned long h = 1469598103934665603UL;   // FNV-1a, 64-bit offset basis
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211UL; }
    return h;
}

static void token_ht_insert(User *u) {
    unsigned long h = hash_str(u->token) & (TOKEN_HT_SIZE - 1);
    while (g_token_ht[h]) h = (h + 1) & (TOKEN_HT_SIZE - 1);
    g_token_ht[h] = u;
}
static void game_ht_insert(GameSlot *s) {
    unsigned long h = hash_str(s->id) & (GAME_HT_SIZE - 1);
    while (g_game_ht[h]) h = (h + 1) & (GAME_HT_SIZE - 1);
    g_game_ht[h] = s;
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
static void *bot_thread(void *arg) {
    GameSlot *s = arg;
    pthread_mutex_lock(&s->lock);
    while (s->used && s->game.status == GAME_STATUS_PLAYING) {
        uint32_t hmask = game_human_mask(&s->game);   // pure per-Game field read — no kernel lock needed
        BotDriveOut drv;
        // g_kernel_lock, innermost (see "Locking" above): bot_drive touches
        // bot_drive.c's process-wide `g_scratch` and, through apply_move/
        // choose_move, engine_last_reject + engine_snap_hook — none of them
        // safe to touch from more than one game's thread at a time.
        pthread_mutex_lock(&g_kernel_lock);
        bot_drive(&s->game, hmask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &drv);   // ONE cycle, then returns
        pthread_mutex_unlock(&g_kernel_lock);
        // A bot's move (or the game ending) changes the board exactly like a
        // human's /action does — the /ws state cache must not stay stale
        // just because no HTTP handler touched this slot this time.
        if (drv.n > 0 || drv.ended >= 0) { s->version++; game_mark_dirty(s); }

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
    GameSlot *s = &g_games[idx];
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
    GameSlot *s = NULL;
    for (int i = 0; i < MAX_GAMES; i++) if (!g_games[i].used) { s = &g_games[i]; break; }
    if (!s) { fprintf(stderr, "persist: recovery dropped a game row — no free slot\n"); return; }
    memset(s, 0, sizeof *s);
    if (!deserialize_slot(s, blob, len)) {
        fprintf(stderr, "persist: recovery dropped a corrupt/unreadable game row\n");
        memset(s, 0, sizeof *s);   // leave it fully unused, not half-populated
        return;
    }
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    // Same "must never equal s->version's initial value" reasoning as
    // h_create's identical line — forces the first state_put_cached call
    // for every seat to actually recompute instead of serving a bogus
    // zero-length cached view.
    for (int i = 0; i < MAX_PLAYERS; i++) s->view_cache_version[i] = (uint32_t)-1;
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
    User snap = g_users[idx];
    pthread_mutex_unlock(&g_registry_lock);
    if (!snap.used) return -1;
    snprintf(out_id, (size_t)id_cap, "%s", snap.user_id);
    return serialize_user(&snap, buf, cap);
}

static void user_persist_load(const char *id, const unsigned char *blob, int len) {
    (void)id;
    User *u = NULL;
    for (int i = 0; i < MAX_USERS; i++) if (!g_users[i].used) { u = &g_users[i]; break; }
    if (!u) { fprintf(stderr, "persist: recovery dropped a user row — no free slot\n"); return; }
    if (!deserialize_user(u, blob, len)) {
        fprintf(stderr, "persist: recovery dropped a corrupt/unreadable user row\n");
        memset(u, 0, sizeof *u);
        return;
    }
    pthread_mutex_lock(&g_registry_lock);
    token_ht_insert(u);
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
    User *u = NULL;
    for (int i = 0; i < MAX_USERS; i++) if (g_users[i].used && !strcmp(g_users[i].username, uname)) { u = &g_users[i]; break; }
    if (!u) for (int i = 0; i < MAX_USERS; i++) if (!g_users[i].used) {
        u = &g_users[i]; u->used = true; snprintf(u->username, sizeof u->username, "%s", uname);
        gen_id(u->user_id, ID_LEN); break;
    }
    // Fresh session token, indexed, and marked dirty for the persistence
    // thread (Stage 2) — a signup/signin the DB never learns about would
    // strand that user's token past a crash, same durability need as a
    // game's state (see game_mark_dirty's doc for the write-behind model).
    if (u) {
        gen_id(u->token, 32);
        token_ht_insert(u);
        if (g_user_table) persist_mark_dirty(g_user_table, (int)(u - g_users));
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

    GameSlot *s = NULL;
    for (int i = 0; i < MAX_GAMES; i++) if (!g_games[i].used) { s = &g_games[i]; break; }
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond(conn, 400, "{\"error\":\"full\"}"); return; }
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->used = true;
    // s->version starts at 0 (memset); view_cache_version must start at a
    // value that can NEVER equal it, or state_put_cached's very first call
    // for a seat would see version==cache_version (both zeroed) and return
    // the also-zeroed, never-computed view_cache_len (0 bytes) instead of
    // actually serializing — a silent "client gets an empty state" bug.
    for (int i = 0; i < MAX_PLAYERS; i++) s->view_cache_version[i] = (uint32_t)-1;
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
            int strat = bot_roster_find(skey);
            if (strat < 0) strat = bot_roster_find("random");
            snprintf(s->seat_name[i], 24, "%%%s %d", skey, i);
            snprintf(g->players[i].name, 24, "%s", s->seat_name[i]);
            snprintf(g->players[i].player_id, 24, "bot%d", i);
            g->players[i].status = PLAYER_STATUS_READY;
            g->players[i].strategy_key = (int8_t)strat;   // the kernel's own seat kind
        }
    } else if (!strcmp(type, "start")) {
        int me = seat_of(s, user_id);
        if (me >= 0) { s->seat_ready[me] = true; g->players[me].status = PLAYER_STATUS_READY; }
        // Deal once every seated human is ready (bots are always ready) and 2+ seated.
        bool all = g->num_players >= 2;
        for (int i = 0; i < g->num_players; i++) if (!seat_is_bot(g, i) && !s->seat_ready[i]) all = false;
        if (all && g->status == GAME_STATUS_WAITING) {
            unsigned char seed[32]; for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(next_rand() ^ (i * 131 + (int)g_seq));
            // g_kernel_lock (see "Locking" above): the deal RNG state
            // game_set_deal_seed_bytes/game_seat_and_deal touch is actually
            // _Thread_local (safe on its own), but they're kernel calls that
            // can reach into game.c's apply/refill paths, so they follow the
            // same "every kernel mutation goes through g_kernel_lock" rule as
            // awire_apply/bot_drive — one rule to audit, not a special case.
            pthread_mutex_lock(&g_kernel_lock);
            game_set_deal_seed_bytes(seed, 32);
            // Seats were wired in the lobby (strategy_key per seat), so pass NULL:
            // the kernel owns marking them + the deal (+ g->status = PLAYING).
            game_seat_and_deal(g, NULL, g->num_players);
            pthread_mutex_unlock(&g_kernel_lock);
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
    // of awire.c), so only awire_apply needs g_kernel_lock (see "Locking"
    // above: it's the one that reaches engine_last_reject via handle_*).
    AwireAction a;
    bool decoded = r->body && r->body_len > 0
                   && awire_decode((const unsigned char *)r->body, r->body_len, &a);
    bool ok = false;
    if (decoded) {
        pthread_mutex_lock(&g_kernel_lock);
        ok = awire_apply(&s->game, seat, &a);
        pthread_mutex_unlock(&g_kernel_lock);
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

// Serialize seat's masked view, reusing the cached bytes from GameSlot when
// nothing has changed since they were computed (PROFILE_HOTPATH.md "T1c" —
// see GameSlot's `version`/`view_cache*` fields above for the invariant).
// MUST be called with s->lock held (same contract as a bare state_put call
// here) and `seat` MUST already be known in-range (0 <= seat < num_players
// <= MAX_PLAYERS — ws_conn_thread validates this at handshake time before
// ever calling in; this function does not re-check, so it is not safe to
// point at an unvalidated/attacker-controlled seat index).
static int state_put_cached(GameSlot *s, int seat, unsigned char *out) {
    if (s->view_cache_version[seat] != s->version) {
        // Serialize straight into a scratch buffer wider than
        // VIEW_CACHE_CAP first: state_put's real worst case fits well
        // inside VIEW_CACHE_CAP today (see that constant's comment), but if
        // a future kernel change ever grew a cap enough to overflow it,
        // this falls back to "always recompute, never cache" for that seat
        // instead of truncating a state update — correctness over the
        // optimization.
        unsigned char scratch[1 + 65536];
        int n = state_put(&s->game, seat, scratch);
        if (n < 0) n = 0;
        if (n <= VIEW_CACHE_CAP) {
            memcpy(s->view_cache[seat], scratch, (size_t)n);
            s->view_cache_len[seat] = n;
            s->view_cache_version[seat] = s->version;
        } else {
            memcpy(out, scratch, (size_t)n);
            return n;
        }
    }
    int n = s->view_cache_len[seat];
    memcpy(out, s->view_cache[seat], (size_t)n);
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
typedef struct {
    Conn conn;
    Req req;
    char *raw_buf;   // owns the bytes r.body/r.query/etc point into until freed
} WsSpawnArg;

static void *ws_conn_thread(void *argp) {
    WsSpawnArg *sa = argp;
    Conn *conn = &sa->conn;
    Req *r = &sa->req;

    char gid[ID_LEN + 1] = {0}; int seat = -1;
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    const char *sp = strstr(r->query, "seat=");
    if (sp) seat = (int)strtol(sp + 5, NULL, 10);

    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u || !s) {
        pthread_mutex_unlock(&g_registry_lock);
        respond(conn, 401, "{\"error\":\"ws auth\"}");
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    bool ok = seat >= 0 && seat < s->game.num_players && seat_of(s, user_id) == seat;
    pthread_mutex_unlock(&s->lock);
    if (!ok) {
        respond(conn, 401, "{\"error\":\"ws auth\"}");
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }

    char accept[64];
    if (!ws_accept_from_key(r->ws_key, accept, sizeof accept)) {
        respond(conn, 400, "{\"error\":\"ws key\"}");
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }
    char resp[256];
    int n = snprintf(resp, sizeof resp,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n <= 0 || ws_write_full(conn, resp, n) != n) { conn_close(conn); free(sa->raw_buf); free(sa); return NULL; }

    WsConn wc; ws_conn_init(&wc, *conn, /*mask_outgoing=*/0);   // server frames: never masked; copies *conn by value — see ws_conn_init's doc
    if (r->body_len > 0) ws_conn_prime(&wc, (const unsigned char *)r->body, r->body_len);
    free(sa->raw_buf); sa->raw_buf = NULL;   // primed into wc.pending — the raw request buffer is no longer referenced

    // [ok:u8][state_put bytes] — sized for state_put's documented worst case
    // (h_state uses the same 65536 cap).
    unsigned char msg[1 + 65536];
    pthread_mutex_lock(&s->lock);
    int slen = s->used ? state_put_cached(s, seat, msg + 1) : 0;
    pthread_mutex_unlock(&s->lock);
    msg[0] = 0;
    if (ws_send_frame(&wc, WS_OP_BIN, msg, slen + 1) < 0) { conn_close(&wc.conn); free(sa); return NULL; }

    unsigned char in[4096];
    int opcode;
    int mlen;
    while ((mlen = ws_recv_message(&wc, in, sizeof in, &opcode)) >= 0) {
        if (opcode != WS_OP_BIN && opcode != WS_OP_TEXT) continue;
        bool applied = false;
        pthread_mutex_lock(&s->lock);
        if (s->used) {
            if (mlen > 0 && s->game.status == GAME_STATUS_PLAYING) {
                AwireAction a;
                if (awire_decode(in, mlen, &a)) {
                    // g_kernel_lock, innermost (see h_action's identical
                    // pattern and the "Locking" doc above).
                    pthread_mutex_lock(&g_kernel_lock);
                    bool applied_now = awire_apply(&s->game, seat, &a);
                    pthread_mutex_unlock(&g_kernel_lock);
                    if (applied_now) {
                        applied = true;
                        s->version++;   // this seat's move can change every seat's view
                        game_mark_dirty(s);
                        pthread_cond_signal(&s->cond);   // same wakeup /action gives the bot game-loop
                    }
                }
            }
            // PROFILE_HOTPATH.md "T1c": on a pure poll (mlen==0 or an
            // illegal/rejected move) this seat's view did NOT change, so
            // state_put_cached memcpy's the bytes computed last time instead
            // of re-running the kernel's full masked serialization — the
            // single biggest measured cost in this loop (~21-31% of
            // instructions under WS+legal load) was paying that on EVERY
            // round trip, including the ~99% that were polls.
            slen = state_put_cached(s, seat, msg + 1);
        } else {
            slen = 0;
        }
        pthread_mutex_unlock(&s->lock);
        msg[0] = applied ? 1 : 0;
        if (ws_send_frame(&wc, WS_OP_BIN, msg, slen + 1) < 0) break;
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

int main(int argc, char **argv) {
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
    for (int i = 1; i < argc; i++) {
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

    pthread_t wt;
    for (int i = 0; i < g_n_create_workers; i++)
        if (pthread_create(&wt, NULL, worker_thread, &g_auth_create_q) == 0) pthread_detach(wt);
    for (int i = 0; i < g_n_meta_workers; i++)
        if (pthread_create(&wt, NULL, worker_thread, &g_meta_q) == 0) pthread_detach(wt);
    for (int i = 0; i < g_n_game_workers; i++)
        if (pthread_create(&wt, NULL, worker_thread, &g_game_q[i]) == 0) pthread_detach(wt);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    listen(srv, 64);
    fprintf(stderr, "foolish native server (kernel-driven, in-memory + SQLite write-behind%s) on :%d "
            "(game-workers=%d meta-workers=%d create-workers=%d db=%s interval=%dms)\n",
            g_tls_ctx ? " + TLS (https/wss)" : "",
            port, g_n_game_workers, g_n_meta_workers, g_n_create_workers,
            db_path ? db_path : "off (--no-db)", persist_interval_ms);

    // The dispatcher: accept, (Stage 3) do the TLS handshake if this
    // listener is running TLS, read+parse the request, then either hand it
    // to a dedicated thread (/ws — a persistent connection, see
    // ws_conn_thread's doc for why it stays off the typed queues) or
    // enqueue it onto the right typed worker pool (classify_queue). This is
    // the accept loop itself acting as dispatcher (no separate
    // reader-thread pool) — see SERVER_SCALING.md for why that's an
    // acceptable, correctness-first choice here rather than a scalability
    // bottleneck in practice.
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
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
            // Dedicated per-connection thread (design B, see ws_conn_thread's
            // doc) — this thread now lives for the client's whole session,
            // so the pthread_create cost — 85.8% of instructions under load
            // in the old thread-per-HTTP-request T1 profile — is paid once
            // per client instead of once per action. See PROFILE_HOTPATH.md T1b.
            WsSpawnArg *sa = malloc(sizeof *sa);
            if (!sa) { conn_close(&conn); free(buf); continue; }
            sa->conn = conn; sa->req = r; sa->raw_buf = buf;
            pthread_t t;
            if (pthread_create(&t, NULL, ws_conn_thread, sa) == 0) pthread_detach(t);
            else { conn_close(&conn); free(buf); free(sa); }
            continue;
        }

        // Cheap, store-free routes: answer inline instead of paying a queue
        // round trip for them.
        if (!strcmp(r.method, "OPTIONS")) { respond(&conn, 200, "{}"); conn_close(&conn); free(buf); continue; }
        if (!strcmp(r.path, "/health"))  { respond(&conn, 200, "{\"ok\":true}"); conn_close(&conn); free(buf); continue; }

        WorkItem item; item.conn = conn; item.req = r; item.raw_buf = buf;
        wq_push(classify_queue(&r), &item);
    }
}

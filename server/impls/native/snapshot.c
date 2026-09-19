// The durable form of a game and a user - see snapshot.h.
#define _GNU_SOURCE
#include "snapshot.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bot_drive.h"
#include "bot_roster.h"
#include "bots.h"       // start_bot_loop: a game recovered mid-play resumes its paced ticks
#include "game.h"
#include "registry.h"
#include "strategy.h"
#include "table.h"      // table_seat_kinds: a restored seat's kind from its roster brain
#include "view.h"

PersistTable *g_game_table = NULL;
PersistTable *g_user_table = NULL;

// --------------------------------------------------------------------------
// GameSlot <-> durable blob. serialize_slot/deserialize_slot are the ONLY
// place that knows this on-disk layout; the kernel's own state_put/state_get
// is its exact round-trip codec for `Game` - this just wraps it with the
// lobby/identity fields view.c's codec deliberately never carries (game.h:
// identity lives with the host, never in the state blob). Versioned with a
// leading byte so a future layout change can detect (and refuse, rather than
// misinterpret) an old row.
//
// Layout (see DURABILITY.md for the worked-out byte budget):
//   [0]                                  version (PERSIST_GAME_BLOB_VERSION)
//   [1..2]                               state_len, uint16 LE
//   [3 .. 3+state_len)                   state_put(&game, VIEW_UNMASKED, .)
//   next ID_LEN+1 bytes                  id
//   next ID_LEN+1 bytes                  owner
//   next ROSTER_BYTES bytes              roster_encode(&roster), the kernel's
//                                        durable roster encoding (roster.h)
//   next MAX_PLAYERS bytes               seat_ready[] (1 byte each, 0/1)
//   next 4 bytes                         rng_base, uint32 LE (the bots' seeding base)
//   next 1 byte                          game.deterministic_deck (0/1)
// Worst case: 3 + 690 (state_put's documented worst case - see
// VIEW_CACHE_CAP in registry.h) + 13*2 + 1227 + 8 + 4 + 1 = 1959 bytes.
// The state codec carries neither the seats' kinds nor the deck mode: the kinds
// come back from the roster's brains (table_seat_kinds, what table_load does), the
// deck mode from its own byte. Before version 3 a recovered game came back with
// every seat a bot of STRAT id 0 (no human could act or read their own seat) and
// its seed-dealt deck drawing at random.
// PERSIST_GAME_BLOB_CAP gives margin, same discipline as VIEW_CACHE_CAP.
// Version 2: the roster replaced version 1's seat_user[]/seat_name[] arrays,
// so a version 1 row is refused rather than misread. Version 3: rng_base, so a
// recovered game's bots keep seeding from the base its deal chose, and the deck mode.
// --------------------------------------------------------------------------
#define PERSIST_GAME_BLOB_VERSION 3
#define PERSIST_GAME_BLOB_CAP 2048

// Returns bytes written, or -1 if it wouldn't fit in `cap` (never happens at
// PERSIST_GAME_BLOB_CAP given the worst case above - defensive, same
// "correctness over the optimization" stance state_put_cached takes).
static int serialize_slot(const GameSlot *s, unsigned char *buf, int cap) {
    unsigned char state[1 + 65536];   // state_put's own documented cap (h_state uses the same 65536)
    int state_len = state_put(&s->game, VIEW_UNMASKED, state);
    int need = 1 + 2 + state_len + (ID_LEN + 1) * 2 + ROSTER_BYTES + MAX_PLAYERS + 4 + 1;
    if (state_len < 0 || need > cap) return -1;
    unsigned char *q = buf;
    *q++ = PERSIST_GAME_BLOB_VERSION;
    *q++ = (unsigned char)(state_len & 0xff);
    *q++ = (unsigned char)((state_len >> 8) & 0xff);
    memcpy(q, state, (size_t)state_len); q += state_len;
    memcpy(q, s->id, ID_LEN + 1); q += ID_LEN + 1;
    memcpy(q, s->owner, ID_LEN + 1); q += ID_LEN + 1;
    if (roster_encode(&s->roster, q, ROSTER_BYTES) != ROSTER_BYTES) return -1;
    q += ROSTER_BYTES;
    for (int i = 0; i < MAX_PLAYERS; i++) *q++ = (unsigned char)(s->seat_ready[i] ? 1 : 0);
    for (int i = 0; i < 4; i++) *q++ = (unsigned char)(s->rng_base >> (8 * i));
    *q++ = s->game.deterministic_deck ? 1 : 0;
    return (int)(q - buf);
}

// Inverse of serialize_slot. `s` MUST already be zeroed by the caller (same
// contract h_create's fresh-slot memset follows) - this never touches
// s->lock/s->cond/s->bot_running/s->version/s->view_cache* (runtime-only
// fields the caller (re-)initializes separately; see game_persist_load /
// h_create). Rejects (returns false) on a version mismatch, a state the kernel refuses,
// or a length too short for its own encoded state_len - defense in depth
// against a corrupted DB row, never trusts `len` blindly (same posture
// state_get's own bounds-clamping takes against a hostile/corrupt blob).
static bool deserialize_slot(GameSlot *s, const unsigned char *buf, int len) {
    if (len < 3 || buf[0] != PERSIST_GAME_BLOB_VERSION) return false;
    const unsigned char *q = buf + 1;
    int state_len = q[0] | (q[1] << 8); q += 2;
    int fixed_tail = (ID_LEN + 1) * 2 + ROSTER_BYTES + MAX_PLAYERS + 4 + 1;
    if (state_len < 0 || state_len > 65536 || 3 + state_len + fixed_tail > len) return false;
    // Exact inverse of state_put(.., VIEW_UNMASKED, ..), and refused whole if
    // the kernel could not have produced the state (game.h game_validate).
    if (state_import(&s->game, q, state_len, /*masked=*/0) != GAME_VALID) return false;
    q += state_len;
    memcpy(s->id, q, ID_LEN + 1); s->id[ID_LEN] = 0; q += ID_LEN + 1;
    memcpy(s->owner, q, ID_LEN + 1); s->owner[ID_LEN] = 0; q += ID_LEN + 1;
    // The roster decoder refuses, never clamps; a roster that does not seat
    // exactly the state's players is refused too.
    if (roster_decode(&s->roster, q, ROSTER_BYTES) != ROSTER_OK) return false;
    if (s->roster.n != s->game.num_players) return false;
    int8_t kinds[MAX_PLAYERS];
    if (table_seat_kinds(&s->roster, kinds) != TABLE_OK) return false;
    for (int i = 0; i < s->roster.n; i++) s->game.players[i].strategy_key = kinds[i];
    q += ROSTER_BYTES;
    for (int i = 0; i < MAX_PLAYERS; i++) s->seat_ready[i] = (*q++ != 0);
    s->rng_base = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    q += 4;
    s->game.deterministic_deck = *q++ != 0;
    s->used = true;
    return true;
}

// User <-> durable blob. Fixed-width (every User field already is), so no
// length field is needed the way serialize_slot needs state_len.
#define PERSIST_USER_BLOB_VERSION 1
#define PERSIST_USER_BLOB_CAP 128

static int serialize_user(const User *u, unsigned char *buf, int cap) {
    int need = 1 + (int)sizeof u->user_id + (int)sizeof u->username;
    if (need > cap) return -1;
    unsigned char *q = buf;
    *q++ = PERSIST_USER_BLOB_VERSION;
    memcpy(q, u->user_id, sizeof u->user_id); q += sizeof u->user_id;
    memcpy(q, u->username, sizeof u->username); q += sizeof u->username;
    return (int)(q - buf);
}
static bool deserialize_user(User *u, const unsigned char *buf, int len) {
    int need = 1 + (int)sizeof u->user_id + (int)sizeof u->username;
    if (len != need || buf[0] != PERSIST_USER_BLOB_VERSION) return false;   // old (token-bearing) rows differ in length -> cleanly rejected
    const unsigned char *q = buf + 1;
    memcpy(u->user_id, q, sizeof u->user_id); q += sizeof u->user_id; u->user_id[sizeof u->user_id - 1] = 0;
    memcpy(u->username, q, sizeof u->username); q += sizeof u->username; u->username[sizeof u->username - 1] = 0;
    u->used = true;
    return true;
}

void persist_self_test(void) {
    // static: sizeof(GameSlot) is tens of KB (mostly the Game's MAX_LOGS-
    // sized log array - see game.h) and this runs once, so a static scratch
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
    // Every card distinct: deserialize_slot imports through game_validate, which
    // refuses one card in two places (the deck ranks 13..9 here, the battle
    // card 9 of another suit, every hand card 8 or lower).
    for (int i = 0; i < 5; i++) { a.game.deck[i].suit = (int8_t)(i % 4); a.game.deck[i].value = (int8_t)(13 - i); }
    a.game.num_battles = 1;
    a.game.table_battles[0].attack.suit = 1; a.game.table_battles[0].attack.value = 9;
    a.game.table_battles[0].defense = CARD_NONE;
    a.game.deterministic_deck = true;
    a.rng_base = 0xA1B2C3D4u;
    for (int i = 0; i < 3; i++) {
        const bool bot = i == 2;
        char id[16], name[16];
        const int id_len = bot ? snprintf(id, sizeof id, "bot%d", i) : snprintf(id, sizeof id, "user%07d", i);
        const int name_len = bot ? snprintf(name, sizeof name, "%%random %d", i) : snprintf(name, sizeof name, "player-%d", i);
        if (roster_seat_add(&a.roster, id, id_len, name, name_len, bot ? "random" : "", bot ? 6 : 0) != i) {
            fprintf(stderr, "persist self-test: FAIL (roster_seat_add)\n"); exit(1);
        }
        a.seat_ready[i] = (i % 2) == 0;
        a.game.players[i].status = PLAYER_STATUS_IN;
        a.game.players[i].strategy_key = (i == 2) ? (int8_t)bot_roster_at(bot_roster_find("random"))->strat : STRATEGY_KEY_HUMAN;
        a.game.players[i].hand_count = (int8_t)(2 + i);
        for (int j = 0; j < a.game.players[i].hand_count; j++) {
            a.game.players[i].hand[j].suit = (int8_t)((i + j) % 4);
            a.game.players[i].hand[j].value = (int8_t)(5 + j);
        }
    }

    static unsigned char blob1[PERSIST_GAME_BLOB_CAP], blob2[PERSIST_GAME_BLOB_CAP];
    int n1 = serialize_slot(&a, blob1, sizeof blob1);
    if (n1 < 0) { fprintf(stderr, "persist self-test: FAIL (serialize_slot didn't fit)\n"); exit(1); }
    memset(&b, 0, sizeof b);
    if (!deserialize_slot(&b, blob1, n1)) {
        fprintf(stderr, "persist self-test: FAIL (deserialize_slot rejected a round-trip blob)\n"); exit(1);
    }
    if (b.rng_base != a.rng_base) { fprintf(stderr, "persist self-test: FAIL (rng_base not restored)\n"); exit(1); }
    // The state blob carries neither the seats' kinds nor the deck mode: a
    // recovered human seat must stay human (else a bot plays for its owner and
    // /state refuses the owner their own seat), and a seed-dealt deck must keep
    // popping in order.
    for (int i = 0; i < 3; i++) {
        if (b.game.players[i].strategy_key != a.game.players[i].strategy_key) {
            fprintf(stderr, "persist self-test: FAIL (seat %d kind %d restored as %d)\n", i,
                    a.game.players[i].strategy_key, b.game.players[i].strategy_key);
            exit(1);
        }
    }
    if (b.game.deterministic_deck != a.game.deterministic_deck) { fprintf(stderr, "persist self-test: FAIL (deterministic_deck not restored)\n"); exit(1); }
    int n2 = serialize_slot(&b, blob2, sizeof blob2);
    if (n2 != n1 || memcmp(blob1, blob2, (size_t)n1) != 0) {
        fprintf(stderr, "persist self-test: FAIL (serialize->deserialize->serialize not byte-identical, "
                         "n1=%d n2=%d)\n", n1, n2);
        exit(1);
    }
    fprintf(stderr, "persist self-test: OK (games: %d-byte round-trip byte-identical)\n", n1);
}

// --------------------------------------------------------------------------
// STAGE 2: persist.c callbacks. Snapshot functions run ONLY on the
// persistence thread (see persist.h's PersistSnapshotFn doc) - each takes
// its OWN short-held domain lock just long enough to copy the live data,
// releases it, THEN does the (slower, structured) serialize work with no
// lock held at all: exactly the "briefly take the lock, memcpy, release,
// serialize unlocked" split the design calls for, so a slow disk never
// makes a request thread wait on a game (or the registry) lock.
//
// Load functions run ONLY during persist_start's synchronous crash-recovery
// pass, before any other thread in the process exists (main() calls
// persist_start before spawning the worker pools or starting the accept
// loop) - so they touch g_users[]/g_games[]/the hash maps directly; the
// lock/unlock pairs below are cheap hygiene (uncontended, nothing else is
// running yet), not a correctness requirement at that specific moment.
// --------------------------------------------------------------------------

static int game_persist_snapshot(int idx, char *out_id, int id_cap, unsigned char *buf, int cap) {
    GameSlot *s = game_slot_at(idx);
    if (!s) return -1;
    // static: this whole engine has exactly one persistence thread (see
    // persist.c), so a reused static scratch avoids a ~sizeof(GameSlot)
    // (tens of KB - see game.h's Game size notes) stack frame or a
    // malloc/free every drain cycle.
    static GameSlot snap;
    // Helgrind (run over a --db=... load, per DURABILITY.md's race-check)
    // caught a real bug in an earlier version of this function: copying
    // `sizeof(GameSlot)` bytes - which includes s->lock/s->cond THEMSELVES,
    // not just the game data they guard - races with any other thread's
    // pthread_mutex_lock/unlock on this SAME `s->lock` while we hold it:
    // POSIX does not guarantee a live pthread_mutex_t/pthread_cond_t's raw
    // bytes are safe to read concurrently with normal lock/unlock traffic,
    // even from the thread currently holding it (an implementation is free
    // to touch its own internal bookkeeping via atomics outside the
    // happens-before edge the lock itself provides - glibc's condvar/mutex
    // internals do exactly that). Fix: copy ONLY the fields serialize_slot
    // actually reads - used/id/game/owner/roster/seat_ready -
    // which the GameSlot layout (see registry.h) keeps contiguous and
    // entirely BEFORE `lock`, so `offsetof(GameSlot, lock)` bytes is exactly
    // that prefix and never touches the mutex/cond themselves. If GameSlot's
    // field order ever changes, this offsetof still compiles but would
    // silently stop covering the right fields - keep any new serialized
    // field ABOVE `lock` in that struct.
    pthread_mutex_lock(&s->lock);
    bool used = s->used;
    if (used) memcpy(&snap, s, offsetof(GameSlot, lock));
    pthread_mutex_unlock(&s->lock);
    if (!used) return -1;
    snprintf(out_id, (size_t)id_cap, "%s", snap.id);
    return serialize_slot(&snap, buf, cap);
}

static void game_persist_load(const char *id, const unsigned char *blob, int len) {
    (void)id;   // deserialize_slot restores s->id from the blob itself - the authoritative copy serialize_slot signed off on
    int idx = g_games_count;                       // append-only: next free slot == count
    GameSlot *s = (idx < MAX_GAMES) ? game_slot_ensure(idx) : NULL;
    if (!s) { fprintf(stderr, "persist: recovery dropped a game row - no free slot\n"); return; }
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
    s->last_active_us = now_us();   // fresh TTL on recovery; a recovered finished game reclaims one interval later
    // Same "must never equal s->version's initial value" reasoning as
    // h_create's identical line - forces the first state_put_cached call
    // for every seat (and the shared spectator slot, index MAX_PLAYERS -
    // Stage 4) to actually recompute instead of serving a bogus zero-length
    // cached view.
    for (int i = 0; i < MAX_PLAYERS + 1; i++) s->view_cache_version[i] = (uint32_t)-1;
    pthread_mutex_lock(&g_registry_lock);
    game_ht_insert(s);
    pthread_mutex_unlock(&g_registry_lock);
    if (s->game.status == GAME_STATUS_PLAYING) {
        // Crash recovery's whole point: a game that was mid-play when the
        // process died resumes paced bot ticks exactly like a freshly
        // dealt one - see start_bot_loop's own doc for the lock contract
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
    if (!u) { fprintf(stderr, "persist: recovery dropped a user row - no free slot\n"); return; }
    g_users_count++;
    u->slot_idx = idx;
    if (!deserialize_user(u, blob, len)) {
        fprintf(stderr, "persist: recovery dropped a corrupt/unreadable user row\n");
        memset(u, 0, sizeof *u);
        return;
    }
    pthread_mutex_lock(&g_registry_lock);
    userid_ht_insert(u);      // stateless tokens: recovered users are found by user_id
    username_ht_insert(u);
    pthread_mutex_unlock(&g_registry_lock);
}

void snapshot_register_tables(void) {
    g_game_table = persist_register_table("games", MAX_GAMES, PERSIST_GAME_BLOB_CAP,
                                           game_persist_snapshot, game_persist_load);
    g_user_table = persist_register_table("users", MAX_USERS, PERSIST_USER_BLOB_CAP,
                                           user_persist_snapshot, user_persist_load);
}

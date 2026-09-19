// The live table of games and users - see registry.h for the store's shape,
// the two-tier locking scheme, and the reclamation contract.
#define _GNU_SOURCE
#include "registry.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "legal.h"
#include "strategy.h"   // STRATEGY_KEY_HUMAN - seat_is_bot's definition of "not a bot"
#include "persist.h"
#include "snapshot.h"   // g_game_table / g_user_table: the durable side of these same rows
#include "srvthread.h"

// --------------------------------------------------------------------------
// In-memory store (the "fake DB"): games + users, per-game locks.
// --------------------------------------------------------------------------

static GameSlot *g_game_chunks[MAX_GAME_CHUNKS];
static User     *g_user_chunks[MAX_USER_CHUNKS];
int              g_games_count = 0;
int              g_users_count = 0;

GameSlot *game_slot_ensure(int idx) {
    if (idx < 0 || idx >= MAX_GAMES) return NULL;
    GameSlot **chunk = &g_game_chunks[idx / GAMES_PER_CHUNK];
    if (!*chunk) { *chunk = calloc(GAMES_PER_CHUNK, sizeof(GameSlot)); if (!*chunk) return NULL; }
    return &(*chunk)[idx % GAMES_PER_CHUNK];
}
User *user_slot_ensure(int idx) {
    if (idx < 0 || idx >= MAX_USERS) return NULL;
    User **chunk = &g_user_chunks[idx / USERS_PER_CHUNK];
    if (!*chunk) { *chunk = calloc(USERS_PER_CHUNK, sizeof(User)); if (!*chunk) return NULL; }
    return &(*chunk)[idx % USERS_PER_CHUNK];
}
GameSlot *game_slot_at(int idx) {
    if (idx < 0 || idx >= g_games_count) return NULL;
    GameSlot *c = g_game_chunks[idx / GAMES_PER_CHUNK];
    return c ? &c[idx % GAMES_PER_CHUNK] : NULL;
}
User *user_slot_at(int idx) {
    if (idx < 0 || idx >= g_users_count) return NULL;
    User *c = g_user_chunks[idx / USERS_PER_CHUNK];
    return c ? &c[idx % USERS_PER_CHUNK] : NULL;
}

uint64_t now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
atomic_ulong    g_seq = 0;

// --------------------------------------------------------------------------
// Game reclamation. A reclaimed slot's index is pushed onto g_free_slots and
// reused by the next create (game_alloc_slot). Guarded by g_registry_lock.
// g_free_slots is demand-paged BSS (only the touched prefix costs RAM). See
// the reaper (game_reaper_thread) at the bottom of this file.
// --------------------------------------------------------------------------
uint64_t g_game_idle_ttl_us = GAME_IDLE_TTL_US;
int      g_reap_interval    = GAME_REAP_INTERVAL;
static int g_free_slots[MAX_GAMES];
int      g_free_count = 0;
atomic_ulong g_games_reclaimed = 0;

// Clear every field of a RECLAIMED slot EXCEPT its already-initialized
// lock/cond. Re-initializing a live pthread_mutex/cond is POSIX undefined
// behavior (and confuses race detectors); a reclaimed slot's lock/cond are
// already initialized and provably have no waiters or holders (the reaper only
// recycles a quiescent slot), so we reuse them in place and zero the game /
// roster / runtime fields on either side of them. Relies on the declared field
// order: lock, cond, then bot_running and the rest.
static void game_slot_reset_preserving_locks(GameSlot *s) {
    memset(s, 0, offsetof(GameSlot, lock));
    memset((char *)s + offsetof(GameSlot, bot_running), 0,
           sizeof(*s) - offsetof(GameSlot, bot_running));
}

GameSlot *game_alloc_slot(int *out_idx) {
    while (g_free_count > 0) {
        int idx = g_free_slots[--g_free_count];
        GameSlot *s = game_slot_at(idx);
        if (s && !s->used) {
            game_slot_reset_preserving_locks(s);   // reuse the slot's live lock/cond in place
            s->slot_idx = idx;
            *out_idx = idx;
            return s;
        }
        // stale free entry (should not happen) - drop it and try the next
    }
    int idx = g_games_count;
    if (idx >= MAX_GAMES) return NULL;
    GameSlot *s = game_slot_ensure(idx);   // calloc'd, already zeroed
    if (!s) return NULL;
    g_games_count++;
    pthread_mutex_init(&s->lock, NULL);    // fresh slot: initialize its lock/cond exactly once
    pthread_cond_init(&s->cond, NULL);
    s->slot_idx = idx;
    *out_idx = idx;
    return s;
}

void game_conn_ref(GameSlot *s) {
    pthread_mutex_lock(&s->lock);
    s->conn_refs++;
    s->last_active_us = now_us();
    pthread_mutex_unlock(&s->lock);
}
void game_conn_unref(GameSlot *s) {
    pthread_mutex_lock(&s->lock);
    if (s->conn_refs > 0) s->conn_refs--;
    s->last_active_us = now_us();
    pthread_mutex_unlock(&s->lock);
}

void game_mark_dirty(GameSlot *s) {
    s->last_active_us = now_us();   // any state change counts as activity (reclamation TTL); called under s->lock
    if (g_game_table) persist_mark_dirty(g_game_table, s->slot_idx);
}

// --------------------------------------------------------------------------
// Small utilities
// --------------------------------------------------------------------------

void gen_id(char *out, int n) {
    static const char hex[] = "0123456789abcdef";
    unsigned long v = (atomic_fetch_add_explicit(&g_seq, 1, memory_order_relaxed) + 1)
                       ^ ((unsigned long)next_rand() << 8) ^ (unsigned long)time(NULL);
    for (int i = 0; i < n; i++) { out[i] = hex[v & 0xf]; v = v * 6364136223846793005UL + 1442695040888963407UL; v >>= 3; }
    out[n] = 0;
}

// --------------------------------------------------------------------------
// token -> User* / game_id -> GameSlot* : fixed-size open-addressing hash
// maps (PROFILE_HOTPATH.md T1 report item 2 - these two were O(MAX_USERS)/
// O(MAX_GAMES) linear strcmp scans on EVERY authenticated request, 1.08M
// strcmp calls in one 20s hammer run). Both g_users/g_games are fixed
// static arrays whose elements never move or get freed for the life of the
// process (a POC store - no delete), so raw pointers into them are safe to
// cache here forever; the table only ever grows (insert-only, sized well
// below 1.0 load factor for MAX_USERS/MAX_GAMES, so no growth/tombstone
// logic is needed). A stale slot (e.g. after h_signup mints a fresh token
// for an existing username, orphaning the old token's slot) is harmless: the
// final `strcmp` against the LIVE field still rejects it. Both tables are
// guarded by g_registry_lock (see "Locking" in registry.h).
#define TOKEN_HT_SIZE 1048576   // power of two, > 2x MAX_USERS (open-addressing needs load factor < 1 or inserts loop; kept at 2x the ceiling so the table never fills)
#define GAME_HT_SIZE   524288   // power of two, > 2x MAX_GAMES

static User     *g_userid_ht[TOKEN_HT_SIZE];     // user_id -> User (a signed token carries the user_id; verify then look up here)
static User     *g_username_ht[TOKEN_HT_SIZE];   // username -> User, so signup dedup is O(1) instead of an O(users) scan
static GameSlot *g_game_ht[GAME_HT_SIZE];

unsigned long hash_str(const char *s) {
    unsigned long h = 1469598103934665603UL;   // FNV-1a, 64-bit offset basis
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211UL; }
    return h;
}

// Inserts are probe-BOUNDED: the tables are sized to 2x the ceilings so they
// never actually fill, but a bounded loop means a mis-sized table degrades to
// "entry not indexed" (unreachable by id) rather than an infinite loop that
// would hang the whole server. Returns true on insert.
bool userid_ht_insert(User *u) {
    unsigned long h = hash_str(u->user_id) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        if (!g_userid_ht[h]) { g_userid_ht[h] = u; return true; }
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    fprintf(stderr, "userid hash full (%d) - raise TOKEN_HT_SIZE\n", TOKEN_HT_SIZE);
    return false;
}
User *user_by_id(const char *user_id) {
    unsigned long h = hash_str(user_id) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        User *u = g_userid_ht[h];
        if (!u) return NULL;
        if (u->used && strcmp(u->user_id, user_id) == 0) return u;
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    return NULL;
}
bool game_ht_insert(GameSlot *s) {
    unsigned long h = hash_str(s->id) & (GAME_HT_SIZE - 1);
    for (int probes = 0; probes < GAME_HT_SIZE; probes++) {
        if (!g_game_ht[h]) { g_game_ht[h] = s; return true; }
        h = (h + 1) & (GAME_HT_SIZE - 1);
    }
    fprintf(stderr, "game hash full (%d) - raise GAME_HT_SIZE\n", GAME_HT_SIZE);
    return false;
}

// Remove s from the open-addressing table using Knuth's backward-shift deletion
// (linear probing): after clearing the slot, shift back any following entry
// whose probe chain ran through the hole, so every remaining key stays findable
// with NO tombstones (which would otherwise accumulate across recycle cycles
// and eventually fill the table). Caller holds g_registry_lock. No-op if absent.
void game_ht_remove(GameSlot *s) {
    unsigned long mask = GAME_HT_SIZE - 1;
    unsigned long i = hash_str(s->id) & mask;
    int probes = 0;
    while (g_game_ht[i] && g_game_ht[i] != s && probes < GAME_HT_SIZE) { i = (i + 1) & mask; probes++; }
    if (g_game_ht[i] != s) return;   // not present
    for (;;) {
        g_game_ht[i] = NULL;
        unsigned long j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (!g_game_ht[j]) return;                        // reached an empty slot - chain closed
            unsigned long k = hash_str(g_game_ht[j]->id) & mask;   // home slot of the entry at j
            // Keep this entry in place (keep scanning) while its home k lies
            // cyclically within (i, j]; otherwise it must shift back into i.
            bool keep = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!keep) break;
        }
        g_game_ht[i] = g_game_ht[j];   // move the entry at j back into the hole at i
        i = j;                          // new hole at j; continue shifting from here
    }
}

bool username_ht_insert(User *u) {
    unsigned long h = hash_str(u->username) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        if (!g_username_ht[h]) { g_username_ht[h] = u; return true; }
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    return false;
}
User *user_by_username(const char *name) {
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

// Caller MUST hold g_registry_lock.
GameSlot *game_by_id(const char *id) {
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

bool seat_is_bot(const Game *g, int i) { return g->players[i].strategy_key != STRATEGY_KEY_HUMAN; }

int seat_of(GameSlot *s, const char *user_id) {
    const int i = roster_seat_of(&s->roster, user_id, (int)strlen(user_id));
    return (i >= 0 && !seat_is_bot(&s->game, i)) ? i : -1;
}

bool roster_name_ok(const char *name) {
    Roster probe;
    memset(&probe, 0, sizeof probe);
    return roster_seat_add(&probe, "x", 1, name, (int)strlen(name), "", 0) == 0;
}

int seat_add(GameSlot *s, int strategy_key, const char *id, const char *name, const char *brain) {
    const int r = roster_seat_add(&s->roster, id, (int)strlen(id), name, (int)strlen(name),
                                  brain, (int)strlen(brain));
    if (r < 0) return -1;
    const int i = game_lobby_seat(&s->game, strategy_key);
    if (i < 0) { roster_seat_remove(&s->roster, r); return -1; }
    return i;
}

// --------------------------------------------------------------------------
// Game reclamation reaper (bounded memory). Every GAME_REAP_INTERVAL seconds,
// recycle any game that is fully quiescent - no /ws connection references it
// (conn_refs==0), no bot thread drives it (!bot_running), and it has been idle
// past GAME_IDLE_TTL_US. That triple guarantees no thread still holds a stale
// GameSlot* (an EConn->slot or a --tls ws_conn_thread), so the slot can be
// unpublished and reused. Without this, finished/abandoned games accumulate
// forever and RAM grows unbounded - the whole point of this pass.
// --------------------------------------------------------------------------
static void reclaim_game_locked(GameSlot *s) {
    // Caller holds g_registry_lock AND s->lock. Unpublish (no new lookup finds
    // it), drop its DB row (so it neither reloads on restart nor grows the DB),
    // mark the slot free, and offer its index for reuse.
    game_ht_remove(s);
    if (g_game_table) persist_delete(g_game_table, s->id);
    s->used = false;
    if (g_free_count < MAX_GAMES) g_free_slots[g_free_count++] = s->slot_idx;
    atomic_fetch_add_explicit(&g_games_reclaimed, 1, memory_order_relaxed);
}

void *game_reaper_thread(void *arg) {
    (void)arg;
    thread_disable_cancellation();
    for (;;) {
        sleep(g_reap_interval);
        uint64_t now = now_us();
        pthread_mutex_lock(&g_registry_lock);
        int count = g_games_count;
        pthread_mutex_unlock(&g_registry_lock);
        for (int i = 0; i < count; i++) {
            pthread_mutex_lock(&g_registry_lock);
            GameSlot *s = game_slot_at(i);
            // trylock: a game busy under its own lock (a bot mid-cycle, a move
            // in flight) is by definition not quiescent - skip it this pass
            // rather than block the whole registry waiting on a slow s->lock.
            if (s && s->used && pthread_mutex_trylock(&s->lock) == 0) {
                if (s->conn_refs == 0 && !s->bot_running &&
                    now - s->last_active_us > g_game_idle_ttl_us) {
                    reclaim_game_locked(s);
                }
                pthread_mutex_unlock(&s->lock);
            }
            pthread_mutex_unlock(&g_registry_lock);
        }
    }
    return NULL;
}

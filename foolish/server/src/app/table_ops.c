// table_ops.c - one game operation per request (table_ops.h), and the section
// and row cache the bot loop shares (app_internal.h).
//
// The port of table_io.ts (runTableOp, commitProducts, ratingsFor), finalize.ts,
// game_cache.ts and the row arithmetic commit_table did in plpgsql, onto the C
// kernel called directly. What a blob, a seat or a push means stays the
// kernel's; what is here is which row to read, which results commit, the CAS
// loop, and the new row's bookkeeping columns.
#include <stddef.h>
#include "app_internal.h"
#include "store/journal.h"
#include "bots.h"
#include "edge.h"
#include "foolish_app.h"
#include "fanout.h"

#include "util/clock.h"
#include "util/err.h"
#include "../../../../shared/c/hex/hex.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"
#include "../../../../shared/c/rng.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A pickup can carry every card in play, and the session log must record it
// whole, or table_replay_code's gate refuses the finished game and it gets no
// replay. The kernel's compact default (16) is its arena's; the Makefile builds
// with the shipped human-game capacities (KERNEL_CAPS), and this holds it there.
#include "game.h"
#include "replay_extras.h"
#include "replay_steps.h"
#include "../../../../shared/c/fnv.h"
_Static_assert(MAX_LOG_PAIRS >= MAX_DECK,
               "the kernel must be built with the human-game capacities (c_base/Makefile KERNEL_CAPS)");

int64_t (*cb_app_now)(void) = cb_now_ms;
void (*cb_app_sleep)(int64_t ms) = cb_sleep_ms;
void cb_game_id_random(char out[CB_GAME_ID_MAX + 1]) {
    uint8_t raw[CB_GAME_ID_HEX / 2];
    cb_random_bytes(raw, sizeof raw);
    hex_encode(raw, sizeof raw, out, CB_GAME_ID_MAX + 1);
}
void (*cb_app_game_id)(char out[CB_GAME_ID_MAX + 1]) = cb_game_id_random;

// ---- byte helpers ------------------------------------------------------------------

static uint8_t *dup_bytes(const void *p, size_t n) {
    uint8_t *d = malloc(n ? n : 1);
    if (d && n) memcpy(d, p, n);
    return d;
}

// A deep copy of a row: every byte field owned by the copy.
static int row_copy(cb_game_row *dst, const cb_game_row *src) {
    *dst = *src;
    dst->state = dup_bytes(src->state, src->state_len);
    dst->roster = dup_bytes(src->roster, src->roster_len);
    dst->logs = src->logs_len ? dup_bytes(src->logs, src->logs_len) : NULL;
    if (!dst->state || !dst->roster || (src->logs_len && !dst->logs)) {
        cb_game_row_free(dst);
        return CB_E_NOMEM;
    }
    return CB_OK;
}

// ---- the row cache (game_cache.ts) ---------------------------------------------------
//
// Rows this process just committed, PLAYING games only, least recently noted
// evicted at CB_ROW_CACHE_CAP. Correctness never depends on it: every commit is
// fenced on the version, so a stale entry costs one conflict and a reload, and
// a refusal computed from an entry is re-checked fresh (cb_table_run). Unlike
// the TypeScript cache it also holds the session log, because the C store
// writes the whole row and the log is appended here, not in SQL.

typedef struct {
    bool        used;
    uint64_t    tick;
    cb_game_row row;
} cache_entry;

static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static cache_entry g_cache[CB_ROW_CACHE_CAP];
static uint64_t g_cache_tick;

static cache_entry *cache_find(const char *id) {
    for (int i = 0; i < CB_ROW_CACHE_CAP; i++)
        if (g_cache[i].used && strcmp(g_cache[i].row.id, id) == 0) return &g_cache[i];
    return NULL;
}

static void cache_evict(cache_entry *e) {
    cb_game_row_free(&e->row);
    memset(e, 0, sizeof *e);
}

void cb_row_cache_drop(const char *id) {
    pthread_mutex_lock(&g_cache_mu);
    cache_entry *e = cache_find(id);
    if (e) cache_evict(e);
    pthread_mutex_unlock(&g_cache_mu);
}

void cb_row_cache_clear(void) {
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < CB_ROW_CACHE_CAP; i++)
        if (g_cache[i].used) cache_evict(&g_cache[i]);
    pthread_mutex_unlock(&g_cache_mu);
}

int cb_row_cache_size(void) {
    int n = 0;
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < CB_ROW_CACHE_CAP; i++) n += g_cache[i].used;
    pthread_mutex_unlock(&g_cache_mu);
    return n;
}

void cb_row_cache_note(const cb_game_row *row) {
    // A lobby or a finished game is edited from everywhere: read it fresh.
    if (row->status != GAME_STATUS_PLAYING) { cb_row_cache_drop(row->id); return; }
    cb_game_row copy;
    if (row_copy(&copy, row) != CB_OK) { cb_row_cache_drop(row->id); return; }
    pthread_mutex_lock(&g_cache_mu);
    cache_entry *e = cache_find(row->id);
    if (!e) {
        cache_entry *oldest = NULL;
        for (int i = 0; i < CB_ROW_CACHE_CAP && !e; i++)
            if (!g_cache[i].used) e = &g_cache[i];
        if (!e) {
            for (int i = 0; i < CB_ROW_CACHE_CAP; i++)
                if (!oldest || g_cache[i].tick < oldest->tick) oldest = &g_cache[i];
            e = oldest;
        }
    }
    if (e->used) cache_evict(e);
    e->used = true;
    e->tick = ++g_cache_tick;
    e->row = copy;
    pthread_mutex_unlock(&g_cache_mu);
}

int cb_row_load(cb_ctx *ctx, const char *id, bool allow_cache, cb_game_row *out, bool *cached) {
    *cached = false;
    if (allow_cache) {
        int rc = CB_E_NOT_FOUND;
        pthread_mutex_lock(&g_cache_mu);
        cache_entry *e = cache_find(id);
        if (e) rc = row_copy(out, &e->row);
        pthread_mutex_unlock(&g_cache_mu);
        if (rc == CB_OK) { *cached = true; return CB_OK; }
    }
    memset(out, 0, sizeof *out);
    return CB_ROWS_CALL(ctx->store, game_get, id, out);
}

// ---- the section -----------------------------------------------------------------------

// ONE OWNER of the section's storage (217 KB: the board, the snapshots, the
// table, the commit and a 128 KB arena). Every operation took a fresh calloc,
// which at that size is an mmap, page faults and a munmap each time. Each
// worker thread keeps one instead. A section is made and freed by the same
// operation on the same thread (no handoff), and a second one wanted while it
// is out (a nested operation) is a private calloc.
//
// What an operation sees is exactly what calloc gave: every byte is zeroed on
// the way out, except the snapshots' slots. Those are written whole
// (table_snap memcpys the entire slot with its tag and aux) before they are
// counted in snaps.n, which is zeroed, and nothing reads past n.
static _Thread_local cb_section *t_section;

// EXCEPTION to "exactly what calloc gave": Game.logs (36 of the Game's 37 KB) is NOT zeroed on the
// way out. Nothing may read it past g.num_logs (table_create and friends reset num_logs to 0 with
// entries still in the array, so zeroing only the first num_logs would not be calloc's state
// either). Where that is the rule to check, the array is filled with LOGS_POISON instead of zeros
// on every reset and on every fresh section, so a read past num_logs shows up as 0xAA garbage in
// the tests and the fuzzers: always under ASan (CB_INSTRUMENTED), and whenever the environment
// has CBASE_POISON_LOGS (the Makefile sets it for every test lane). Shipped builds skip both.
#define LOGS_POISON 0xAA
static bool logs_poisoned(void) {
#ifdef CB_INSTRUMENTED
    return true;
#else
    static atomic_int state;   // 0 unknown, 1 off, 2 on
    int v = atomic_load_explicit(&state, memory_order_relaxed);
    if (!v) {
        v = getenv("CBASE_POISON_LOGS") ? 2 : 1;
        atomic_store_explicit(&state, v, memory_order_relaxed);
    }
    return v == 2;
#endif
}

// A pool thread's kept section goes when the thread does: t_section is also the value of a
// pthread key whose destructor frees it (a _Thread_local has no destructor in C). Without it every
// worker's section leaked at shutdown (LeakSanitizer, 4 req workers x 320 KB).
static pthread_key_t  g_section_key;
static pthread_once_t g_section_once = PTHREAD_ONCE_INIT;
static bool           g_section_key_ok;
static void section_at_exit(void *p) { free(p); }   // a thread ends with nothing out
static void section_key_make(void) { g_section_key_ok = pthread_key_create(&g_section_key, section_at_exit) == 0; }

// Frees the calling thread's kept section (not while one is out). For a leak-counting test.
void cb_section_drain(void) {
    if (t_section && !t_section->in_use) {
        free(t_section);
        t_section = NULL;
        if (g_section_key_ok) pthread_setspecific(g_section_key, NULL);
    }
}

bool cb_section_logs_poisoned(void) { return logs_poisoned(); }

cb_section *cb_section_new(void) {
    cb_section *s = t_section;
    if (s && s->in_use) {
        s = calloc(1, sizeof(cb_section));
        if (s && logs_poisoned()) memset(s->g.logs, LOGS_POISON, sizeof s->g.logs);
        return s;
    }
    if (!s) {
        s = calloc(1, sizeof(cb_section));
        if (!s) return NULL;
        if (logs_poisoned()) memset(s->g.logs, LOGS_POISON, sizeof s->g.logs);
        s->pooled = true;
        t_section = s;
        pthread_once(&g_section_once, section_key_make);
        if (g_section_key_ok) pthread_setspecific(g_section_key, s);
    }
    s->in_use = true;
    return s;
}

// What a products call wrote into the arena: its spans, or all of it when the
// call failed part-way (nothing then says how far it got).
static void arena_note(cb_section *s, int rc) {
    const TableCommit *c = &s->c;
    int hw = rc < 0 ? (int)sizeof s->arena : 0;
    const Span *sp[4 + MAX_PLAYERS];
    int n = 0;
    sp[n++] = &c->state; sp[n++] = &c->roster; sp[n++] = &c->logs; sp[n++] = &c->spectator;
    for (int i = 0; i < MAX_PLAYERS; i++) sp[n++] = &c->views[i];
    for (int i = 0; i < n; i++) {
        int end = sp[i]->len > 0 ? sp[i]->off + sp[i]->len : 0;
        if (end > hw) hw = end;
    }
    if (hw > (int)sizeof s->arena) hw = (int)sizeof s->arena;
    if (hw > s->arena_hw) s->arena_hw = hw;
}

void cb_section_free(cb_section *s) {
    if (!s) return;
    if (!s->pooled) { free(s); return; }
    // calloc's zeros back: the board, the table through the commit, and the
    // arena prefix the products wrote (the rest was never written).
    memset(&s->g, 0, offsetof(Game, logs));   // every field but the log array (see t_section)
    if (logs_poisoned()) memset(s->g.logs, LOGS_POISON, sizeof s->g.logs);
    s->snaps.n = 0;
    memset((char *)s + offsetof(cb_section, t), 0, offsetof(cb_section, arena) - offsetof(cb_section, t));
    memset(s->arena, 0, (size_t)s->arena_hw);
    s->arena_hw = 0;
    s->in_use = false;
}

// Test hook: the calling thread's pooled section, when idle, is entirely zero
// (the calloc state every cb_section_new caller relies on).
bool cb_section_pooled_is_clean(void) {
    const cb_section *s = t_section;
    if (!s || s->in_use) return true;
    const unsigned char *p = (const unsigned char *)&s->g;
    for (size_t i = 0; i < offsetof(Game, logs); i++) if (p[i]) return false;
    // The log array is unspecified when not poisoned; under poison it is all LOGS_POISON.
    if (logs_poisoned())
        for (size_t i = 0; i < sizeof s->g.logs; i++) if (((const unsigned char *)s->g.logs)[i] != LOGS_POISON) return false;
    p = (const unsigned char *)&s->t;
    for (size_t i = 0; i < offsetof(cb_section, arena) - offsetof(cb_section, t); i++) if (p[i]) return false;
    for (int i = 0; i < s->arena_hw; i++) if (s->arena[i]) return false;
    return true;
}

int cb_section_load(cb_section *s, const cb_game_row *row) {
    snprintf(s->gid, sizeof s->gid, "%s", row->id);
    table_init(&s->t, &s->g, &s->snaps);
    int rc = table_load(&s->t, row->state, (int)row->state_len, row->roster, (int)row->roster_len);
    if (rc < 0) return rc;
    // Every mid-game draw and every bot decision is seeded from this game's
    // deal seed, as its hex text exactly as table_io.ts's bareHex wrote it.
    char hex[2 * CB_SEED_LEN + 1] = "";
    if (row->has_seed) hex_encode(row->seed, CB_SEED_LEN, hex, sizeof hex);
    rc = table_set_deal_seed(&s->t, hex, (int)strlen(hex));
    if (rc < 0) return rc;
    s->n_old = s->n_old_bots = 0;
    for (int i = 0; i < s->t.r.n; i++) {
        const RosterSeat *seat = &s->t.r.seats[i];
        // memcpy, not snprintf: the seat and the list live in the same struct,
        // and gcc's -Wrestrict (fortified, under ASan) cannot prove they do not overlap.
        bool human = seat->brain_len == 0;
        int *n_list = human ? &s->n_old : &s->n_old_bots;
        if (*n_list < CB_MAX_SEATS) {
            size_t n = seat->id_len < CB_UUID_LEN ? (size_t)seat->id_len : CB_UUID_LEN;
            cb_strncpy0((human ? s->old_humans : s->old_bots)[(*n_list)++], sizeof(cb_uuid), (const char *)seat->id, n);
        }
    }
    return TABLE_OK;
}

int cb_section_envelope(cb_section *s, int viewer, uint32_t version, uint8_t **out, size_t *out_len) {
    uint8_t buf[CB_ENVELOPE_MAX];
    int n = table_envelope(&s->t, s->gid, (int)strlen(s->gid), viewer, version, buf, sizeof buf);
    if (n < 0) {
        CB_ERROR("table", "game %s: envelope for viewer %d refused (%d)", s->gid, viewer, n);
        return CB_E_STATE;
    }
    *out = dup_bytes(buf, (size_t)n);
    *out_len = (size_t)n;
    return *out ? CB_OK : CB_E_NOMEM;
}

// One viewer's cast for the last operation, malloc'd at its own size. It is
// built in a per-thread 64 KB scratch (generous: a deal's push carries every
// snapshot's events) instead of a 64 KB malloc and free per viewer per move.
static int section_cast(cb_section *s, int viewer, uint32_t version, uint8_t **out, size_t *out_len) {
    enum { CAST_CAP = 64 * 1024 };
    static _Thread_local uint8_t buf[CAST_CAP];
    int n = table_cast(&s->t, s->gid, (int)strlen(s->gid), viewer, version, buf, CAST_CAP);
    if (n < 0) {
        CB_ERROR("table", "game %s: cast for viewer %d refused (%d)", s->gid, viewer, n);
        return CB_E_STATE;
    }
    *out = dup_bytes(buf, (size_t)n);
    *out_len = (size_t)n;
    return *out ? CB_OK : CB_E_NOMEM;
}

static bool seat_is_human(const RosterSeat *seat) { return seat->brain_len == 0; }

static void seat_id(const RosterSeat *seat, cb_uuid out) {
    snprintf(out, sizeof(cb_uuid), "%.*s", (int)seat->id_len, seat->id);
}

// ---- the end of a game (finalize.ts) ------------------------------------------------------
//
// After the winning commit only, so it happens once: the verified v6 replay
// code and the extras, from the committed row's deal seed and session log, as
// one snapshot row. A failure is logged and swallowed: the game has ended
// either way, and the session log stays as the record until the next session
// replaces it. The TypeScript host then cleared logs_packed with a second,
// unfenced UPDATE; the C store has no such write, so the log is left for the
// next commit that leaves the finished table (commit_table's rule: a lobby
// commit empties it, a deal replaces it).
// The history list's line of a replay (cb_snap_summary): who was the fool, the
// order the others went out, and the extras' gaps summed. A code or extras the
// kernel will not read leave the summary absent, and the list shows the row
// without it. One owner: the game end cuts it here for the pointers, and the
// history list cuts it here for a pointer written before they held one.
// The seats' names and the gaps of a STORED extras blob (the version-2 bytes
// behind the dash in a replay link): decoded with the seat and move counts
// the moves give, then read as the struct. The one reader of stored extras in
// the server (the list's names and the summary's duration both come here).
int cb_table_extras_read(ReplayExtras *x, const uint8_t *extras, int n_extras, int n_players, int n_moves) {
    static _Thread_local unsigned char blob[sizeof(ReplayExtras)];
    if (!extras || n_extras <= 0) return -1;
    int nb = replay_extras_decode(extras, n_extras, n_players, n_moves, blob, (int)sizeof blob);
    if (nb < 0 || replay_extras_unpack(blob, nb, x) < 0) return -1;
    return 0;
}

void cb_table_snap_summary(cb_snap_summary *m, const uint8_t *code, int n_code, const uint8_t *extras, int n_extras) {
    ReplaySummary d;
    if (replay_summary_v6(code, n_code, &d) != REPLAY_EOK || d.num_eliminated < 0 || d.num_eliminated > CB_MAX_SEATS) return;
    m->has = true;
    m->fool = d.fool;
    m->n_elim = d.num_eliminated;
    for (int i = 0; i < d.num_eliminated; i++) m->elim[i] = d.elimination[i];
    m->duration_s = -1;
    static _Thread_local ReplayExtras x;
    if (cb_table_extras_read(&x, extras, n_extras, d.num_players, d.moves) == 0 && x.n_gaps > 0) {
        double sum = 0;
        for (int i = 0; i < x.n_gaps; i++) sum += x.gaps[i];
        m->duration_s = sum >= 0 && sum < 2e9 ? (int32_t)(sum + 0.5) : -1;
    }
}

static void finalize_game(cb_ctx *ctx, cb_section *s, const cb_game_row *nrow) {
    enum { CODE_CAP = 2 * 1024 * 1024, SCRATCH_CAP = 1024 * 1024, EXTRAS_CAP = 256 * 1024 };
    uint8_t *code = NULL, *scratch = NULL, *extras = NULL, *logs = NULL;
    size_t logs_len = 0;
    const char *why = NULL;
    int rc = 0;
    if (!nrow->has_seed) { why = "no deal seed: nothing to encode a replay from"; goto out; }
    if (CB_ROWS_CALL(ctx->store, game_logs, nrow->id, nrow->log_from, &logs, &logs_len) != CB_OK || logs_len == 0) {
        free(logs);
        why = "no session log: nothing to encode a replay from";
        goto out;
    }
    code = malloc(CODE_CAP);
    scratch = malloc(SCRATCH_CAP);
    extras = malloc(EXTRAS_CAP);
    if (!code || !scratch || !extras) { why = "out of memory"; goto out; }
    table_init(&s->t, &s->g, &s->snaps);
    if ((rc = table_load(&s->t, nrow->state, (int)nrow->state_len, nrow->roster, (int)nrow->roster_len)) < 0) {
        why = "does not load";
        goto out;
    }
    int n_extras = table_replay_extras(&s->t, logs, (int)logs_len, extras, EXTRAS_CAP);
    if (n_extras < 0) { rc = n_extras; why = "replay extras refused"; goto out; }
    if ((rc = table_load(&s->t, nrow->state, (int)nrow->state_len, nrow->roster, (int)nrow->roster_len)) < 0) {
        why = "does not load";
        goto out;
    }
    int n_code = table_replay_code(&s->t, nrow->seed, CB_SEED_LEN, logs, (int)logs_len,
                                   code, CODE_CAP, scratch, SCRATCH_CAP);
    if (n_code < 0) { rc = n_code; why = "replay code refused"; goto out; }
    CB_INFO("table", "game %s: replay encoded (v6) to %d+%d bytes", nrow->id, n_code, n_extras);

    cb_snapshot_row snap;
    memset(&snap, 0, sizeof snap);
    cb_uuid_v4(snap.id);
    snprintf(snap.game_id, sizeof snap.game_id, "%s", nrow->id);
    // Every seat, bots included, in seat order: what finalize.ts wrote.
    for (int i = 0; i < s->t.r.n && i < CB_MAX_SEATS; i++) seat_id(&s->t.r.seats[i], snap.player_ids[snap.n_players++]);
    snap.moves = code;
    snap.moves_len = (size_t)n_code;
    snap.extras = extras;
    snap.extras_len = (size_t)n_extras;
    snap.created_at = cb_app_now();
    cb_table_snap_summary(&snap.sum, code, n_code, extras, n_extras);
    if ((rc = CB_ROWS_CALL(ctx->store, snapshot_put, &snap)) != CB_OK) why = "snapshot_put failed";
out:
    if (why) CB_ERROR("table", "game %s: snapshot failed, keeping the log: %s (%d)", nrow->id, why, rc);
    free(code);
    free(scratch);
    free(extras);
    free(logs);
}

// ---- the game lock ---------------------------------------------------------------------------

// A game's commits and their pushes happen under the game's own lock: from the row a request reads
// to its last push nothing else in this process moves the row, and a game's pushes go out in its
// commit order (two threads could commit v10 and v11 in that order and publish v11 first, which a
// client gating on version reads as a gap). One mutex per game id that some thread holds or waits
// for: the first taker makes the entry and the last one out frees it, so games never share a lock
// (an unrelated game never waits) and the table holds only the games in flight.
#define LOCK_BUCKETS 1024
struct cb_game_lock {
    struct cb_game_lock *next;
    pthread_mutex_t      mu;
    int                  users;   // holders and waiters, under g_locks_mu
    uint32_t             bucket;
    char                 id[CB_GAME_ID_MAX + 1];
};
static pthread_mutex_t g_locks_mu = PTHREAD_MUTEX_INITIALIZER;
static cb_game_lock *g_locks[LOCK_BUCKETS];

cb_game_lock *cb_table_lock(const char *gid) {
    uint32_t b = fnv1a32_str(FNV32_OFFSET, gid) % LOCK_BUCKETS;
    pthread_mutex_lock(&g_locks_mu);
    cb_game_lock *l = g_locks[b];
    while (l && strcmp(l->id, gid) != 0) l = l->next;
    if (!l && (l = calloc(1, sizeof *l)) != NULL) {
        pthread_mutex_init(&l->mu, NULL);
        l->bucket = b;
        cb_strlcpy(l->id, gid, sizeof l->id);
        l->next = g_locks[b];
        g_locks[b] = l;
    }
    if (l) l->users++;
    pthread_mutex_unlock(&g_locks_mu);
    if (l) pthread_mutex_lock(&l->mu);
    return l;
}

void cb_table_unlock(cb_game_lock *l) {
    if (!l) return;
    pthread_mutex_unlock(&l->mu);
    pthread_mutex_lock(&g_locks_mu);
    if (--l->users == 0) {
        cb_game_lock **pp = &g_locks[l->bucket];
        while (*pp != l) pp = &(*pp)->next;
        *pp = l->next;
        pthread_mutex_destroy(&l->mu);
        free(l);
    }
    pthread_mutex_unlock(&g_locks_mu);
}

// ---- the commit ---------------------------------------------------------------------------

// Rating farming (security finding M6): sign-ups are free, so a player could
// lose games on purpose from fresh accounts to feed one main account. A game
// with a human seat whose account is younger than this ends unrated, for
// everyone at it: the kernel's own path for a host that has no ratings (no
// stamp, no change, table.h table_rate). A deleted account (no row) is no
// reason; a row that cannot be read is, as an unreadable rating is. The age
// is config.h CBASE_RATED_MIN_AGE (72 h; the smoke tests' fresh accounts set 0).
static bool accounts_old_enough(cb_ctx *ctx, const Table *t, int64_t now) {
    const int64_t min_age_ms = (int64_t)(ctx->cfg ? ctx->cfg->rated_min_age_s : 72 * 3600) * 1000;
    if (min_age_ms <= 0) return true;
    for (int i = 0; i < t->r.n; i++) {
        if (!seat_is_human(&t->r.seats[i])) continue;
        cb_uuid id;
        seat_id(&t->r.seats[i], id);
        cb_user_row u;
        int rc = CB_STORE_CALL(ctx->store, user_get, id, &u);
        if (rc == CB_E_NOT_FOUND) continue;
        if (rc != CB_OK) { CB_WARN("table", "account %s unreadable (%s): the game ends unrated", id, cb_strerror(rc)); return false; }
        if (now - u.created_at < min_age_ms) {
            CB_INFO("table", "account %s is under %lld s old: the game ends unrated", id, (long long)(min_age_ms / 1000));
            return false;
        }
    }
    return true;
}

// The seats' ratings into `ratings` (TABLE_RATING_NONE for none). false when
// they cannot be read, or an account is too new to be rated against
// (accounts_old_enough): the game then ends unrated, as ratingsFor's null did.
static bool read_ratings(cb_ctx *ctx, const Table *t, int32_t ratings[CB_MAX_SEATS]) {
    if (!accounts_old_enough(ctx, t, cb_app_now())) return false;
    cb_uuid ids[CB_MAX_SEATS];
    const char *ptrs[CB_MAX_SEATS];
    for (int i = 0; i < t->r.n; i++) { seat_id(&t->r.seats[i], ids[i]); ptrs[i] = ids[i]; }
    int rc = CB_ROWS_CALL(ctx->store, ratings_for, ptrs, t->r.n, ratings);
    if (rc != CB_OK) CB_WARN("table", "seat ratings unreadable (%s): the game ends unrated", cb_strerror(rc));
    return rc == CB_OK;
}

static bool in_list(const cb_uuid *list, int n, const char *id) {
    for (int i = 0; i < n; i++) if (strcmp(list[i], id) == 0) return true;
    return false;
}

// The new row a commit writes, and the commit that writes it, from the section's products:
// commit_table's arithmetic, moved here. The row's byte fields point into the section's
// arena. The commit and the journal's replay (cb_table_replay_move) both come here.
static void section_row(cb_section *s, const cb_game_row *row, const uint8_t *dealt_seed, int64_t now,
                        cb_game_row *nrow_out, cb_game_commit *gp) {
    const Table *t = &s->t;
    const TableCommit *c = &s->c;
    const uint8_t *A = s->arena;
    const bool waiting = c->status == GAME_STATUS_WAITING;
    cb_game_row *const np = nrow_out;
    memset(np, 0, sizeof *np);
    snprintf(np->id, sizeof np->id, "%s", row->id);
    np->status = c->status;
    np->state = (uint8_t *)A + c->state.off;
    np->state_len = (size_t)c->state.len;
    np->roster = (uint8_t *)A + c->roster.off;
    np->roster_len = (size_t)c->roster.len;
    np->needs_bots = c->needs_bots;
    np->version = row->version + 1;
    np->created_at = row->created_at;
    np->updated_at = now;
    np->last_commit_at = now;
    np->seats_exact = true;   // the commit writes the seats the new roster holds
    // The deal seed: none in a lobby; the fresh one when this operation dealt;
    // else the stored one.
    if (!waiting && c->dealt_now && dealt_seed) {
        np->has_seed = true;
        memcpy(np->seed, dealt_seed, CB_SEED_LEN);
    } else if (!waiting && row->has_seed) {
        np->has_seed = true;
        memcpy(np->seed, row->seed, CB_SEED_LEN);
    }
    // The session log: a deal replaces it, a lobby empties it, anything else
    // appends this operation's records. The store appends the delta (a LOG# item
    // at the new version); the row only says where the session's log starts.
    const bool log_clear = c->logs_reset || waiting;
    np->log_from = log_clear ? np->version : row->log_from;
    if (waiting && !c->logs_reset) np->log_from = np->version;
    // The round guard's epoch: 0 on a deal or in a lobby, the new version when
    // this operation closed a round, else unchanged.
    np->round_epoch = (c->logs_reset || waiting) ? 0 : c->closed_round ? row->version + 1 : row->round_epoch;

    memset(gp, 0, sizeof *gp);
    gp->row = np;
    gp->log_clear = log_clear;
    if (!(waiting && !c->logs_reset) && c->logs.len > 0) {
        gp->log_add = (uint8_t *)A + c->logs.off;
        gp->log_add_len = (size_t)c->logs.len;
    }
    gp->expected_version = row->version;
    // The loaded row's seats, when it vouches that they are the stored ones
    // (store.h cb_game_commit prev_known): the store skips reading them.
    gp->prev_known = row->seats_exact;
    gp->prev_needs_bots = row->needs_bots;
    gp->n_prev_humans = s->n_old;
    memcpy(gp->prev_humans, s->old_humans, sizeof gp->prev_humans);
    gp->n_prev_bots = s->n_old_bots;
    memcpy(gp->prev_bots, s->old_bots, sizeof gp->prev_bots);
    for (int i = 0; i < t->r.n; i++) {
        const RosterSeat *seat = &t->r.seats[i];
        if (seat_is_human(seat)) seat_id(seat, gp->humans[gp->n_humans++]);
        else seat_id(seat, gp->bots[gp->n_bots++]);
    }
    if (c->n_rated > 0) {
        gp->n_rated = t->r.n;
        for (int i = 0; i < t->r.n; i++) {
            seat_id(&t->r.seats[i], gp->rated_ids[i]);
            gp->rated_is_bot[i] = !seat_is_human(&t->r.seats[i]);
            gp->rating_delta[i] = c->rating_delta[i];
            snprintf(gp->rated_names[i], sizeof gp->rated_names[i], "%.*s",
                     (int)t->r.seats[i].name_len, t->r.seats[i].name);
        }
    }

}

int cb_section_commit(cb_ctx *ctx, cb_section *s, const cb_game_row *row, const uint8_t *dealt_seed,
                      const char *actor_id, int32_t ratings[CB_MAX_SEATS], bool *have_ratings,
                      cb_commit_out *out) {
    memset(out, 0, sizeof *out);
    Table *t = &s->t;
    TableCommit *c = &s->c;
    const int gid_len = (int)strlen(s->gid);
    const int64_t now = cb_app_now();
    const int64_t board_now = cb_bots_board_now(ctx, row->created_at, now);   // the kernel's (bots.h)
    const uint32_t next = (uint32_t)(row->version + 1);
    int rc = table_commit_products(t, s->gid, gid_len, next, board_now, c, s->arena, sizeof s->arena);
    arena_note(s, rc);
    if (rc >= 0 && c->wants_ratings) {
        // The kernel rates the game inside the section that ends it. Its products
        // are const, so the C host rates and asks for them again: no re-run.
        if (!*have_ratings) *have_ratings = read_ratings(ctx, t, ratings);
        if (*have_ratings) {
            table_rate(t, ratings, t->r.n);
            rc = table_commit_products(t, s->gid, gid_len, next, board_now, c, s->arena, sizeof s->arena);
            arena_note(s, rc);
        }
    }
    if (rc < 0) {
        CB_ERROR("table", "game %s: no commit products (%d)", s->gid, rc);
        return CB_E_STATE;
    }

    cb_game_row nrow;
    cb_game_commit gc;
    section_row(s, row, dealt_seed, now, &nrow, &gc);
    const uint8_t *A = s->arena;
    // A plain human move (journal.h): the store may hold it in its journal. Its menu
    // index was taken at the board before the move (cb_table_run); anything a move does
    // besides moving the board goes to the tables at once.
    if (s->n_mv > 0 && !c->ended && !c->dealt_now && !gc.log_clear && gc.n_rated == 0 &&
        row->status == GAME_STATUS_PLAYING && nrow.status == GAME_STATUS_PLAYING &&
        row->roster_len == nrow.roster_len && memcmp(row->roster, nrow.roster, nrow.roster_len) == 0)
    {
        gc.moves = s->mv;
        gc.n_moves = s->n_mv;
        gc.moves_by_bots = s->mv_bots;
    }

    // ---- everything the aftermath needs, copied out of the section ----------------
    // EVERY COMMIT IS CAST, one per human seat and one for the spectators, at
    // the version it wrote. The version is what a client gates on: a cast that
    // does not follow the one before it lands on a board it does not follow,
    // so the kernel client drops it and fetches the game whole (netclient.c
    // NW_WS_CAST). A commit that cast nothing therefore cost every open page a
    // refetch at its next push.
    //
    // There used to be a rule here, "events, a goods change or a roster
    // change", and the commits it missed were real moves: a good said over an
    // uncovered table (legal, and silent: game_shown_good_mask) and a hand
    // rearranged. Both change the stored board - the raw good mask is in every
    // envelope (view.c), and a good takes the seat out of the attackers - so
    // neither can commit without moving the version the CAS is fenced on.
    // The cast carries the viewer's whole final board after its events, so a
    // cast with none still moves every client to the board the version names;
    // the pages apply one as a board (AnimationContext's empty-stream path).
    cb_fanout f;
    memset(&f, 0, sizeof f);
    f.game_id = row->id;
    f.version = nrow.version;
    f.status = cb_table_status_label(c->status);
    f.updated_at = now;
    f.n_seats = t->r.n;
    rc = CB_OK;
    for (int i = 0; i < t->r.n && rc == CB_OK; i++) {
        const RosterSeat *seat = &t->r.seats[i];
        if (!seat_is_human(seat)) continue;
        seat_id(seat, f.seats[i].user_id);
        f.seats[i].envelope = dup_bytes(A + c->views[i].off, (size_t)c->views[i].len);
        f.seats[i].envelope_len = (size_t)c->views[i].len;
        if (!f.seats[i].envelope) rc = CB_E_NOMEM;
        if (rc == CB_OK) {
            uint8_t *cast = NULL;
            rc = section_cast(s, i, next, &cast, &f.seats[i].cast_len);
            f.seats[i].cast = cast;
        }
    }
    if (rc == CB_OK) {
        uint8_t *cast = NULL;
        rc = section_cast(s, -1, next, &cast, &f.spectator_cast_len);
        f.spectator_cast = cast;
    }
    for (int i = 0; i < f.n_seats; i++) {
        if (!f.seats[i].user_id[0]) continue;
        if (!in_list(s->old_humans, s->n_old, f.seats[i].user_id)) cb_strlcpy(f.joined[f.n_joined++], f.seats[i].user_id, sizeof(cb_uuid));
    }
    for (int i = 0; i < s->n_old; i++) {
        bool still = false;
        for (int j = 0; j < f.n_seats; j++) if (strcmp(f.seats[j].user_id, s->old_humans[i]) == 0) still = true;
        if (!still) snprintf(f.left[f.n_left++], sizeof(cb_uuid), "%s", s->old_humans[i]);
    }
    if (rc == CB_OK && actor_id) {
        int seat = table_seat_of(t, actor_id, (int)strlen(actor_id));
        Span e = seat >= 0 && c->views[seat].len > 0 ? c->views[seat] : c->spectator;
        out->actor_env = dup_bytes(A + e.off, (size_t)e.len);
        out->actor_env_len = (size_t)e.len;
        if (!out->actor_env) rc = CB_E_NOMEM;
    }
    // The committed row survives the section: finalize reloads the table.
    cb_game_row kept;
    memset(&kept, 0, sizeof kept);
    if (rc == CB_OK) rc = row_copy(&kept, &nrow);

    if (rc == CB_OK) {
        // The caller holds the game's lock (cb_table_lock): from the row it read to
        // these pushes nothing else in this process commits the game.
        rc = CB_ROWS_CALL(ctx->store, game_commit, &gc);
        if (rc == CB_E_CONFLICT) {
            // Somebody committed first: nothing was written.
            cb_row_cache_drop(row->id);
            out->lost = true;
            rc = CB_OK;
        } else if (rc == CB_OK) {
            out->version = nrow.version;
            out->needs_bots = c->needs_bots;
            out->ended = c->ended;
            cb_row_cache_note(&kept);
            // A rated game moved the leaderboard: the edge's copy goes (edge.h).
            if (gc.n_rated > 0) cb_edge_stale(CB_EDGE_BOARD);
            // The replay is stored before the push that ends the game, so a
            // client that asks for it on game over finds it.
            if (c->ended) finalize_game(ctx, s, &kept);
            cb_fanout_commit(ctx, &f);
        } else {
            CB_ERROR("table", "game %s: commit failed (%s)", row->id, cb_strerror(rc));
        }
    }

    cb_game_row_free(&kept);
    for (int i = 0; i < CB_MAX_SEATS; i++) {
        free((void *)f.seats[i].envelope);
        free((void *)f.seats[i].cast);
    }
    free((void *)f.spectator_cast);
    if ((rc != CB_OK || out->lost) && out->actor_env) {
        free(out->actor_env);
        out->actor_env = NULL;
        out->actor_env_len = 0;
    }
    return rc;
}

// ---- the journal's replay (journal.h cb_journal_apply_fn) ------------------------------------

// A scratch board per thread for the bot cycle's moves (replay_move_drive_walk plays on it).
static _Thread_local Game *t_walk;
static Game *walk_board(const Game *from) {
    if (!t_walk && !(t_walk = malloc(sizeof *t_walk))) return NULL;
    memcpy(t_walk, from, sizeof *t_walk);
    return t_walk;
}

int cb_bots_journal_moves_before(cb_section *s, Game **board, uint32_t *base, uint32_t *off) {
    table_drive_seeding(&s->t, base, off);
    *board = walk_board(&s->g);
    return *board ? CB_OK : CB_E_NOMEM;
}

void cb_bots_journal_moves_after(cb_section *s, Game *board, uint32_t base, uint32_t off, BotDriveOut *drv) {
    s->n_mv = 0;
    s->mv_bots = true;
    if (drv->n > 0 && replay_move_drive_walk(board, base, off, drv->n, drv->actions, s->mv, true) == 0) s->n_mv = drv->n;
}

int cb_table_replay_move(void *ud, const cb_game_row *row, const uint8_t *session_log, size_t session_log_len,
                         const struct cb_jmove *m, cb_game_row *out, uint8_t **log, size_t *log_len) {
    cb_ctx *ctx = ud;
    memset(out, 0, sizeof *out);
    *log = NULL;
    *log_len = 0;
    cb_section *s = cb_section_new();
    if (!s) return CB_E_NOMEM;
    int rc = CB_E_STATE, kr = cb_section_load(s, row);
    if (kr < 0) {
        CB_ERROR("table", "game %s: the journal's base row at version %lld does not load (%d)", row->id, (long long)row->version, kr);
        goto out;
    }
    if (!m->bots) {
        // The menu at this board must be the one the move was coded on (the check the
        // journal's own bytes cannot make): anything else is a board the move was not
        // played on, and the start is refused.
        uint8_t wire[REPLAY_MOVE_WIRE_MAX];
        const ReplayMove *mv = &m->mv[0];
        int n = replay_move_wire(&s->g, mv, wire, (int)sizeof wire);
        if (n < 0 || mv->seat >= s->t.r.n) {
            CB_ERROR("table", "game %s: the journal's move at version %lld does not fit the board (%d)", row->id,
                     (long long)row->version, n);
            goto out;
        }
        const RosterSeat *seat = &s->t.r.seats[mv->seat];
        kr = table_act(&s->t, (const char *)seat->id, (int)seat->id_len, wire, n, -1, row->round_epoch);
        if (kr != TABLE_APPLIED) {
            CB_ERROR("table", "game %s: the journal's move at version %lld is refused (%d)", row->id, (long long)row->version, kr);
            goto out;
        }
    } else {
        // The cycle again, its choices handed to the drive as the moves it applied: the
        // drive reuses each (no search) under its own seeding, and must apply exactly them.
        kr = table_set_session_log(&s->t, session_log, (int)session_log_len);
        uint32_t base, off;
        table_drive_seeding(&s->t, &base, &off);
        Game *board = kr >= 0 ? walk_board(&s->g) : NULL;
        BotDriveOut want, drv;
        memset(&want, 0, sizeof want);
        uint8_t prefs[1024];
        int pl = -1;
        if (board && replay_move_drive_walk(board, base, off, m->n_mv, want.actions, (ReplayMove *)m->mv, false) == 0) {
            want.n = m->n_mv;
            pl = table_drive_prefs(&s->t, &want, prefs, sizeof prefs);
        }
        kr = pl < 0 ? -1 : table_bot_drive(&s->t, prefs, pl, 0, &drv);
        bool same = kr == m->n_mv;
        for (int i = 0; same && i < m->n_mv; i++) {
            const LegalMove *a = &drv.actions[i].move, *b = &want.actions[i].move;
            same = drv.actions[i].seat == want.actions[i].seat && a->type == b->type && a->n_cards == b->n_cards;
            for (int k = 0; same && k < a->n_cards; k++)
                same = card_eq(a->cards[k], b->cards[k]) && (a->type != MOVE_COVER || card_eq(a->attack_cards[k], b->attack_cards[k]));
        }
        if (!same) {
            CB_ERROR("table", "game %s: the journal's bot cycle at version %lld does not replay (%d of %d moves)", row->id,
                     (long long)row->version, kr, m->n_mv);
            goto out;
        }
    }
    const int64_t board_now = cb_bots_board_now(ctx, row->created_at, m->now_ms);
    kr = table_commit_products(&s->t, s->gid, (int)strlen(s->gid), (uint32_t)(row->version + 1), board_now, &s->c, s->arena,
                               sizeof s->arena);
    arena_note(s, kr);
    if (kr < 0 || s->c.wants_ratings || s->c.ended) {
        CB_ERROR("table", "game %s: the journal's commit at version %lld is not a plain one (%d)", row->id,
                 (long long)row->version, kr);
        goto out;
    }
    cb_game_row nrow;
    cb_game_commit gc;
    section_row(s, row, NULL, m->now_ms, &nrow, &gc);
    rc = row_copy(out, &nrow);
    if (rc == CB_OK && gc.log_add_len) {
        if (!(*log = dup_bytes(gc.log_add, gc.log_add_len))) rc = CB_E_NOMEM;
        else *log_len = gc.log_add_len;
    }
out:
    cb_section_free(s);
    if (rc != CB_OK) cb_game_row_free(out);
    return rc;
}

// ---- one operation (runTableOp) ---------------------------------------------------------

static bool commits_nothing(int rc) {
    return rc == TABLE_MOOT || rc == TABLE_STALE_ROUND || rc == TABLE_REJECTED;
}

// A refusal of the move is only authoritative against a fresh row: an apply
// from a stale cache corrects itself through the CAS, a refusal never reaches it.
static bool fresh_only(int rc) {
    return rc == TABLE_STALE_ROUND || rc == TABLE_REJECTED;
}

int cb_table_run(cb_ctx *ctx, const cb_table_op *op, cb_table_result *out) {
    memset(out, 0, sizeof *out);
    if (!ctx->store) return CB_E_STATE;
    cb_section *s = cb_section_new();
    if (!s) return CB_E_NOMEM;
    const char *actor = op->actor ? op->actor->id : NULL;
    int32_t ratings[CB_MAX_SEATS];
    bool have_ratings = false;
    int ret = CB_E_CONFLICT;

    // ONE LOCK: the game's lock is held from the row read to the last push, so in this
    // process nothing moves the row in between and a commit does not lose its fence. The fence
    // (version = expected) stays as the store's own guarantee; a loss is an error, not a retry.
    // The only second attempt left is the cache's: a refusal or a load failure from a cached
    // row is rechecked against the stored one.
    cb_game_lock *lock = cb_table_lock(op->game_id);
    if (!lock) {
        cb_section_free(s);
        return CB_E_NOMEM;
    }
    for (int attempt = 1; attempt <= 2; attempt++) {
        cb_game_row row;
        bool cached = false;
        int rc = cb_row_load(ctx, op->game_id, !op->fresh && attempt == 1, &row, &cached);
        if (rc != CB_OK) { ret = rc; break; }
        uint8_t seed[CB_SEED_LEN];
        cb_random_bytes(seed, sizeof seed);

        // ---- the kernel section ----
        int lr = cb_section_load(s, &row);
        if (lr < 0) {
            cb_game_row_free(&row);
            if (cached) { cb_row_cache_drop(op->game_id); continue; }
            // A stored row the kernel refuses: never silently repaired or played on.
            CB_ERROR("table", "game %s does not load (%d, detail %d)", op->game_id, lr, s->t.detail);
            ret = CB_E_STATE;
            break;
        }
        // The move as the kernel's menu names it, at the board before it: what the store's
        // journal holds (journal.h). Only a store with a journal asks.
        s->n_mv = 0;
        s->mv_bots = false;
        if (op->wire && actor && cb_journal_of(ctx->store)) {
            int seat = table_seat_of(&s->t, actor, (int)strlen(actor));
            if (seat >= 0 && replay_move_of(&s->g, seat, op->wire, op->wire_len, &s->mv[0]) == 0) s->n_mv = 1;
        }
        rc = op->run(&s->t, &row, seed, op->ud);
        if (rc < 0 || (cached && fresh_only(rc))) {
            if (cached) { cb_row_cache_drop(op->game_id); cb_game_row_free(&row); continue; }
            out->result = rc;
            out->detail = s->t.detail;
            out->version = row.version;
            cb_game_row_free(&row);
            ret = CB_OK;
            break;
        }
        out->result = rc;
        out->reject = rc == TABLE_REJECTED ? s->t.reject : 0;

        if (rc == TABLE_EMPTY) {
            // The last seat left: the row goes, fenced on the version like a
            // commit. The answer carries no envelope: a table with no seats is
            // not a board, and client_adopt_envelope refuses one (the asker's
            // client takes the empty answer as "the game is gone").
            ret = CB_ROWS_CALL(ctx->store, game_delete, op->game_id, row.version);
            cb_row_cache_drop(op->game_id);
            if (ret == CB_OK) {
                out->committed = true;
                out->version = row.version;
                cb_fanout f;
                memset(&f, 0, sizeof f);
                f.game_id = op->game_id;
                f.version = row.version;
                f.status = cb_table_status_label(GAME_STATUS_WAITING);
                f.updated_at = cb_app_now();
                for (int i = 0; i < s->n_old; i++) snprintf(f.left[f.n_left++], sizeof(cb_uuid), "%s", s->old_humans[i]);
                cb_fanout_commit(ctx, &f);
            }
            cb_game_row_free(&row);
            break;
        }

        if (commits_nothing(rc)) {
            int seat = actor ? table_seat_of(&s->t, actor, (int)strlen(actor)) : -1;
            out->needs_bots = table_bots_due(&s->t);
            out->version = row.version;
            ret = cb_section_envelope(s, seat >= 0 ? seat : -1, (uint32_t)row.version, &out->envelope, &out->envelope_len);
            cb_game_row_free(&row);
            break;
        }

        cb_commit_out co;
        ret = cb_section_commit(ctx, s, &row, seed, actor, ratings, &have_ratings, &co);
        cb_game_row_free(&row);
        if (ret != CB_OK) break;
        if (co.lost) { ret = CB_E_CONFLICT; break; }
        out->committed = true;
        out->version = co.version;
        out->needs_bots = co.needs_bots;
        out->envelope = co.actor_env;
        out->envelope_len = co.actor_env_len;
        break;
    }
    if (ret == CB_E_CONFLICT)
        CB_ERROR("table", "could not commit game %s: write contention", op->game_id);
    if (ret != CB_OK) {
        free(out->envelope);
        out->envelope = NULL;
        out->envelope_len = 0;
    }
    cb_table_unlock(lock);
    cb_section_free(s);
    return ret;
}

// ---- create ---------------------------------------------------------------------------------

int cb_table_create(cb_ctx *ctx, const cb_user *u, cb_table_result *out) {
    memset(out, 0, sizeof *out);
    if (!ctx->store) return CB_E_STATE;
    cb_section *s = cb_section_new();
    if (!s) return CB_E_NOMEM;
    int ret = CB_E_CONFLICT;
    for (int draw = 1; draw <= CB_TABLE_ID_DRAWS; draw++) {
        // A fresh random id (app_internal.h CB_GAME_ID_HEX); a draw that names
        // a game that exists is refused by the store, and we draw again.
        cb_app_game_id(s->gid);
        table_init(&s->t, &s->g, &s->snaps);
        int rc = table_create(&s->t, u->id, (int)strlen(u->id), u->username, (int)strlen(u->username));
        if (rc < 0) {
            CB_ERROR("table", "create refused (%d, detail %d)", rc, s->t.detail);
            ret = CB_E_STATE;
            break;
        }
        const int64_t now = cb_app_now();
        rc = table_commit_products(&s->t, s->gid, (int)strlen(s->gid), 0, now, &s->c, s->arena, sizeof s->arena);
        arena_note(s, rc);
        if (rc < 0 || s->c.views[0].len <= 0) {
            CB_ERROR("table", "create: no products (%d)", rc);
            ret = CB_E_STATE;
            break;
        }
        const TableCommit *c = &s->c;
        cb_game_row row;
        memset(&row, 0, sizeof row);
        snprintf(row.id, sizeof row.id, "%s", s->gid);
        row.status = c->status;
        row.state = s->arena + c->state.off;
        row.state_len = (size_t)c->state.len;
        row.roster = s->arena + c->roster.off;
        row.roster_len = (size_t)c->roster.len;
        row.created_at = row.updated_at = row.last_commit_at = now;
        row.seats_exact = true;   // the create writes the creator's seat, the only one
        // Stored BEFORE the answer: the envelope names the game id.
        ret = CB_ROWS_CALL(ctx->store, game_create, &row, u->id);
        if (ret == CB_E_CONFLICT) {
            CB_WARN("table", "game id %s is taken (draw %d/%d), drawing again", s->gid, draw, CB_TABLE_ID_DRAWS);
            continue;
        }
        if (ret != CB_OK) break;
        out->committed = true;
        out->version = 0;
        out->envelope = dup_bytes(s->arena + c->views[0].off, (size_t)c->views[0].len);
        out->envelope_len = (size_t)c->views[0].len;
        if (!out->envelope) { ret = CB_E_NOMEM; break; }
        // The creator's view row appears on their dashboard (pv- INSERT).
        cb_fanout f;
        memset(&f, 0, sizeof f);
        f.game_id = s->gid;
        f.status = cb_table_status_label(c->status);
        f.updated_at = now;
        f.n_seats = 1;
        snprintf(f.seats[0].user_id, sizeof(cb_uuid), "%s", u->id);
        f.seats[0].envelope = out->envelope;
        f.seats[0].envelope_len = out->envelope_len;
        f.n_joined = 1;
        snprintf(f.joined[0], sizeof(cb_uuid), "%s", u->id);
        cb_fanout_commit(ctx, &f);
        break;
    }
    if (ret == CB_E_CONFLICT) CB_ERROR("table", "create: no free game id in %d draws", CB_TABLE_ID_DRAWS);
    cb_section_free(s);
    return ret;
}

// ---- views on demand --------------------------------------------------------------------------

void cb_view_row_free(cb_view_row *v) {
    free(v->view);
    v->view = NULL;
    v->view_len = 0;
}

const char *cb_table_status_label(int status) {
    switch (status) {
    case GAME_STATUS_WAITING:   return "waiting";
    case GAME_STATUS_PLAYING:   return "playing";
    case GAME_STATUS_GAME_OVER: return "game_over";
    default:                    return "unknown";
    }
}

static int view_of_row(cb_section *s, const cb_game_row *row, const char *viewer_id, cb_view_row *out) {
    memset(out, 0, sizeof *out);
    int lr = cb_section_load(s, row);
    if (lr < 0) {
        CB_ERROR("table", "game %s does not load (%d)", row->id, lr);
        return CB_E_STATE;
    }
    int seat = -1;
    if (viewer_id) {
        seat = table_seat_of(&s->t, viewer_id, (int)strlen(viewer_id));
        if (seat < 0) return CB_E_NOT_FOUND;
        snprintf(out->player_id, sizeof out->player_id, "%s", viewer_id);
    }
    snprintf(out->game_id, sizeof out->game_id, "%s", row->id);
    out->version = row->version;
    out->updated_at = row->updated_at;
    snprintf(out->status, sizeof out->status, "%s", cb_table_status_label(row->status));
    return cb_section_envelope(s, seat, (uint32_t)row->version, &out->view, &out->view_len);
}

int cb_table_view(cb_ctx *ctx, const char *game_id, const char *viewer_id, cb_view_row *out) {
    memset(out, 0, sizeof *out);
    if (!ctx->store) return CB_E_STATE;
    cb_game_row row;
    memset(&row, 0, sizeof row);
    int rc = CB_ROWS_CALL(ctx->store, game_get, game_id, &row);
    if (rc != CB_OK) return rc;
    cb_section *s = cb_section_new();
    rc = s ? view_of_row(s, &row, viewer_id, out) : CB_E_NOMEM;
    cb_section_free(s);
    cb_game_row_free(&row);
    if (rc != CB_OK) cb_view_row_free(out);
    return rc;
}

typedef struct {
    int n, cap;
    char (*ids)[CB_GAME_ID_MAX + 1];
} id_list;

// One dashboard row at a time, as the store reads them.
typedef struct {
    cb_section *s;
    const char *user_id;
    cb_view_fn  fn;
    void       *ud;
    int         n, rc;
} views_walk;

static bool view_step(const cb_game_row *row, void *ud) {
    views_walk *w = ud;
    cb_view_row v;
    int g = view_of_row(w->s, row, w->user_id, &v);
    if (g == CB_E_NOT_FOUND) { cb_view_row_free(&v); return true; }   // a membership the roster no longer seats
    if (g != CB_OK) { cb_view_row_free(&v); w->rc = g; return false; }
    w->n++;
    bool more = w->fn(&v, w->ud);
    cb_view_row_free(&v);
    return more;
}

static bool collect_member(const cb_member_row *r, void *ud) {
    id_list *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? 2 * l->cap : 16;
        void *p = realloc(l->ids, (size_t)cap * sizeof *l->ids);
        if (!p) return false;
        l->ids = p;
        l->cap = cap;
    }
    snprintf(l->ids[l->n++], sizeof *l->ids, "%s", r->game_id);
    return true;
}

int cb_table_views_of_user(cb_ctx *ctx, const char *user_id, int limit, cb_view_fn fn, void *ud) {
    if (!ctx->store) return CB_E_STATE;
    id_list l = { 0, 0, NULL };
    int rc = CB_ROWS_CALL(ctx->store, games_of_user, user_id, limit, collect_member, &l);
    if (rc < 0) { free(l.ids); return rc; }
    cb_section *s = cb_section_new();
    const char **ptrs = malloc((size_t)(l.n ? l.n : 1) * sizeof *ptrs);
    if (!s || !ptrs) { cb_section_free(s); free(ptrs); free(l.ids); return CB_E_NOMEM; }
    for (int i = 0; i < l.n; i++) ptrs[i] = l.ids[i];
    // One batch read of the rows (a game deleted between the two reads is
    // simply not among them), each masked for this viewer by the kernel.
    views_walk w = { .s = s, .user_id = user_id, .fn = fn, .ud = ud };
    rc = CB_ROWS_CALL(ctx->store, games_get_views, ptrs, l.n, view_step, &w);
    cb_section_free(s);
    free(ptrs);
    free(l.ids);
    if (rc < 0) return rc;
    return w.rc != CB_OK ? w.rc : w.n;
}

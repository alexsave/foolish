// persist.c — the generic write-behind engine persist.h declares. See that
// header's top comment for the model; this file is deliberately ignorant of
// GameSlot/User (foolish_server.c owns those) so it can drive both the
// `games` and `users` tables with one code path.
#define _GNU_SOURCE
#include "persist.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_TABLES 4
#define MAX_PENDING_DELETES 256
#define PERSIST_ID_CAP 40   // generous over ID_LEN+1(13) — ids here are opaque strings to this file

struct PersistTable {
    char name[32];
    int  capacity;
    int  blob_cap;
    PersistSnapshotFn snapshot_fn;
    PersistLoadFn      load_fn;

    // Dirty bitmap: a plain bool[capacity], guarded by g_wake_mtx (below).
    // Marking the same index dirty any number of times between drains costs
    // the same as marking it once — no growth, no dedup logic needed.
    bool *dirty;
    // Snapshot-under-lock scratch: filled from `dirty` while g_wake_mtx is
    // held (see drain_and_persist), then walked with NO lock held. Only the
    // persistence thread ever reads these two, so plain fields suffice.
    int  *scan_buf;
    int   n_scan;

    // Explicit deletes (see persist_delete) — not indexed by slot, since a
    // deleted slot may not have a stable index concept once gone.
    char pending_deletes[MAX_PENDING_DELETES][PERSIST_ID_CAP];
    int  n_pending_deletes;
    char del_scan[MAX_PENDING_DELETES][PERSIST_ID_CAP];
    int  n_del_scan;

    sqlite3_stmt *upsert_stmt;
    sqlite3_stmt *delete_stmt;
};

static PersistTable g_tables[MAX_TABLES];
static int g_n_tables = 0;

static sqlite3 *g_db = NULL;
static bool g_enabled = false;
static int  g_interval_ms = 75;

// Guards: every table's `dirty` bitmap + pending_deletes list, AND is the
// condvar predicate lock for the persistence thread's wait. One lock, not
// one per table — every critical section under it is a bounded array scan
// or a handful of string copies (microseconds), and this engine only ever
// has two tables (games, users), so a per-table lock would add real
// complexity for no measurable win. This is exactly the "small mutex" the
// design brief calls for around game_mark_dirty's dirty-set write.
static pthread_mutex_t g_wake_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake_cond = PTHREAD_COND_INITIALIZER;

// Scratch blob buffer for one row's serialized bytes, reused across the
// drain loop. Only the persistence thread ever touches it (this whole
// engine has exactly one such thread — see persist_start), so a plain
// static avoids a per-row heap alloc without needing thread-local storage.
static unsigned char *g_scratch = NULL;
static int g_scratch_cap = 0;

static void ensure_scratch(int cap) {
    if (cap > g_scratch_cap) {
        unsigned char *grown = realloc(g_scratch, (size_t)cap);
        if (!grown) return;   // leave the old (smaller) buffer; caller's cap check still protects it
        g_scratch = grown;
        g_scratch_cap = cap;
    }
}

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

PersistTable *persist_register_table(const char *sql_table_name, int capacity,
                                      int blob_cap, PersistSnapshotFn snapshot_fn,
                                      PersistLoadFn load_fn) {
    if (g_n_tables >= MAX_TABLES) {
        fprintf(stderr, "persist: too many tables registered (max %d)\n", MAX_TABLES);
        return NULL;
    }
    PersistTable *t = &g_tables[g_n_tables++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", sql_table_name);
    t->capacity = capacity;
    t->blob_cap = blob_cap;
    t->snapshot_fn = snapshot_fn;
    t->load_fn = load_fn;
    t->dirty = calloc((size_t)capacity, sizeof(bool));
    t->scan_buf = calloc((size_t)capacity, sizeof(int));
    return t;
}

bool persist_enabled(void) { return g_enabled; }

void persist_mark_dirty(PersistTable *table, int idx) {
    if (!g_enabled || !table) return;
    if (idx < 0 || idx >= table->capacity) return;
    pthread_mutex_lock(&g_wake_mtx);
    table->dirty[idx] = true;
    pthread_cond_signal(&g_wake_cond);
    pthread_mutex_unlock(&g_wake_mtx);
}

void persist_delete(PersistTable *table, const char *id) {
    if (!g_enabled || !table || !id) return;
    pthread_mutex_lock(&g_wake_mtx);
    if (table->n_pending_deletes < MAX_PENDING_DELETES) {
        snprintf(table->pending_deletes[table->n_pending_deletes], PERSIST_ID_CAP, "%s", id);
        table->n_pending_deletes++;
    }
    pthread_cond_signal(&g_wake_cond);
    pthread_mutex_unlock(&g_wake_mtx);
}

// One drain pass: snapshot the dirty sets (lock held briefly), then, with NO
// engine lock held, serialize each dirty row (each snapshot_fn takes its
// OWN short-held domain lock — e.g. one GameSlot's mutex — internally) and
// write every row from this pass in ONE transaction. Never holds g_wake_mtx
// during a snapshot_fn call or any sqlite3_* call — those are exactly the
// two things that must never block a request-path thread's own
// persist_mark_dirty call.
static void drain_and_persist(void) {
    if (!g_db) return;

    bool any = false;
    pthread_mutex_lock(&g_wake_mtx);
    for (int ti = 0; ti < g_n_tables; ti++) {
        PersistTable *t = &g_tables[ti];
        t->n_scan = 0;
        for (int i = 0; i < t->capacity; i++) {
            if (t->dirty[i]) { t->dirty[i] = false; t->scan_buf[t->n_scan++] = i; }
        }
        t->n_del_scan = t->n_pending_deletes;
        for (int i = 0; i < t->n_pending_deletes; i++)
            memcpy(t->del_scan[i], t->pending_deletes[i], PERSIST_ID_CAP);
        t->n_pending_deletes = 0;
        if (t->n_scan > 0 || t->n_del_scan > 0) any = true;
    }
    pthread_mutex_unlock(&g_wake_mtx);
    if (!any) return;   // nothing dirty — skip opening a transaction for no reason

    char *errmsg = NULL;
    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "persist: BEGIN failed: %s\n", errmsg ? errmsg : "(no message)");
        sqlite3_free(errmsg);
        return;   // whatever was dirty stays lost from THIS pass only — it's
                  // still marked live in memory, so the NEXT dirty write to
                  // the same slot (or a future drain re-scanning) will retry
                  // it; only a genuinely stuck DB loses data, and that's the
                  // same failure mode any DB-backed store has.
    }
    for (int ti = 0; ti < g_n_tables; ti++) {
        PersistTable *t = &g_tables[ti];
        ensure_scratch(t->blob_cap);
        for (int k = 0; k < t->n_scan; k++) {
            int idx = t->scan_buf[k];
            char id[PERSIST_ID_CAP];
            int n = t->snapshot_fn(idx, id, sizeof id, g_scratch, g_scratch_cap);
            if (n < 0) continue;   // slot no longer live by the time we got to it — nothing to write
            sqlite3_reset(t->upsert_stmt);
            sqlite3_bind_text(t->upsert_stmt, 1, id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(t->upsert_stmt, 2, g_scratch, n, SQLITE_TRANSIENT);
            sqlite3_bind_int64(t->upsert_stmt, 3, now_us());
            if (sqlite3_step(t->upsert_stmt) != SQLITE_DONE)
                fprintf(stderr, "persist: upsert %s/%s failed: %s\n", t->name, id, sqlite3_errmsg(g_db));
        }
        for (int k = 0; k < t->n_del_scan; k++) {
            sqlite3_reset(t->delete_stmt);
            sqlite3_bind_text(t->delete_stmt, 1, t->del_scan[k], -1, SQLITE_TRANSIENT);
            if (sqlite3_step(t->delete_stmt) != SQLITE_DONE)
                fprintf(stderr, "persist: delete %s/%s failed: %s\n", t->name, t->del_scan[k], sqlite3_errmsg(g_db));
        }
    }
    if (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "persist: COMMIT failed: %s\n", errmsg ? errmsg : "(no message)");
        sqlite3_free(errmsg);
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
    }
}

static void *persist_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_wake_mtx);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long ns = ts.tv_nsec + (long)(g_interval_ms % 1000) * 1000000L;
        ts.tv_sec += g_interval_ms / 1000 + ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
        // Times out after ~interval_ms even with no signal (periodic
        // drain), or wakes early on a persist_mark_dirty/persist_delete
        // signal — either way we fall through to drain_and_persist. The
        // return value (timed out vs signaled) doesn't matter: both paths
        // do the same drain.
        pthread_cond_timedwait(&g_wake_cond, &g_wake_mtx, &ts);
        pthread_mutex_unlock(&g_wake_mtx);
        drain_and_persist();
    }
    return NULL;
}

bool persist_start(const char *db_path, int interval_ms) {
    g_interval_ms = interval_ms > 0 ? interval_ms : 75;
    if (!db_path) { g_enabled = false; return true; }   // --no-db: pure in-memory, not a failure

    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "persist: sqlite3_open(%s) failed: %s\n", db_path,
                g_db ? sqlite3_errmsg(g_db) : sqlite3_errstr(rc));
        if (g_db) sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }

    char *errmsg = NULL;
    // WAL + synchronous=NORMAL: the "mostly in memory, persist when we get a
    // chance, still crash-safe" sweet spot — a committed transaction
    // survives a process crash either way; NORMAL just means an fsync isn't
    // forced on every single commit (still forced often enough, per SQLite's
    // own WAL docs, to survive a crash — the tradeoff is OS-level power loss,
    // not a process kill). See DURABILITY.md.
    if (sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "persist: PRAGMA journal_mode=WAL failed: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg); sqlite3_close(g_db); g_db = NULL; return false;
    }
    if (sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "persist: PRAGMA synchronous=NORMAL failed: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg); sqlite3_close(g_db); g_db = NULL; return false;
    }

    for (int i = 0; i < g_n_tables; i++) {
        PersistTable *t = &g_tables[i];
        char sql[256];
        snprintf(sql, sizeof sql,
            "CREATE TABLE IF NOT EXISTS %s(id TEXT PRIMARY KEY, blob BLOB NOT NULL, updated_us INTEGER);",
            t->name);
        if (sqlite3_exec(g_db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
            fprintf(stderr, "persist: schema(%s) failed: %s\n", t->name, errmsg ? errmsg : "?");
            sqlite3_free(errmsg); sqlite3_close(g_db); g_db = NULL; return false;
        }
        snprintf(sql, sizeof sql,
            "INSERT INTO %s(id,blob,updated_us) VALUES(?1,?2,?3) "
            "ON CONFLICT(id) DO UPDATE SET blob=excluded.blob, updated_us=excluded.updated_us;",
            t->name);
        if (sqlite3_prepare_v2(g_db, sql, -1, &t->upsert_stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "persist: prepare upsert(%s) failed: %s\n", t->name, sqlite3_errmsg(g_db));
            sqlite3_close(g_db); g_db = NULL; return false;
        }
        snprintf(sql, sizeof sql, "DELETE FROM %s WHERE id=?1;", t->name);
        if (sqlite3_prepare_v2(g_db, sql, -1, &t->delete_stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "persist: prepare delete(%s) failed: %s\n", t->name, sqlite3_errmsg(g_db));
            sqlite3_close(g_db); g_db = NULL; return false;
        }
    }

    // Crash recovery: load every existing row through its table's
    // PersistLoadFn, synchronously, BEFORE the write-behind thread (or any
    // worker pool / the accept loop — see foolish_server.c's main()) exists.
    // No locking needed: nothing else can touch g_users[]/g_games[] yet.
    for (int i = 0; i < g_n_tables; i++) {
        PersistTable *t = &g_tables[i];
        char sql[128];
        snprintf(sql, sizeof sql, "SELECT id, blob FROM %s;", t->name);
        sqlite3_stmt *sel;
        if (sqlite3_prepare_v2(g_db, sql, -1, &sel, NULL) != SQLITE_OK) {
            fprintf(stderr, "persist: prepare select(%s) failed: %s\n", t->name, sqlite3_errmsg(g_db));
            continue;
        }
        int rows = 0;
        while (sqlite3_step(sel) == SQLITE_ROW) {
            const unsigned char *id = sqlite3_column_text(sel, 0);
            const void *blob = sqlite3_column_blob(sel, 1);
            int blen = sqlite3_column_bytes(sel, 1);
            if (id && t->load_fn) { t->load_fn((const char *)id, (const unsigned char *)blob, blen); rows++; }
        }
        sqlite3_finalize(sel);
        fprintf(stderr, "persist: recovered %d row(s) from %s\n", rows, t->name);
    }

    g_enabled = true;
    pthread_t th;
    if (pthread_create(&th, NULL, persist_thread, NULL) != 0) {
        fprintf(stderr, "persist: failed to start the persistence thread\n");
        g_enabled = false;
        return false;
    }
    pthread_detach(th);
    return true;
}

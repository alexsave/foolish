// persist.h — SQLite WAL write-behind persistence engine (Stage 2 of the
// production-hardening series; Stage 1 was per-game locks + work-queue
// routing, see SERVER_SCALING.md; Stage 3 is TLS). See DURABILITY.md for the
// full design writeup — this header is the API foolish_server.c wires
// domain logic (GameSlot/User layout, serialize_slot/deserialize_slot) into.
//
// This file knows NOTHING about GameSlot or User — it is a small generic
// "durable key/value table, write-behind, one owning thread" engine so the
// same code drives both the `games` and `users` tables. foolish_server.c
// supplies, per table: how big a serialized row can get (a byte cap, same
// style as VIEW_CACHE_CAP's documented worst case), and a callback that
// turns "the Nth slot" into (id, serialized bytes) — see PersistSnapshotFn.
//
// THE MODEL (write-behind, async, batched — never blocks the request path):
//   - `persist_mark_dirty(table, index)` is O(1): lock a small mutex, set
//     one bool in a fixed-size dirty bitmap, unlock, signal a condvar. No
//     disk I/O, no allocation, no blocking on anything the persistence
//     thread might be doing. Safe to call even when persistence is off
//     (--no-db) — it's then a single `if` and a return.
//   - ONE dedicated thread owns the ONE sqlite3* connection (SQLite handles
//     are not safe to share across threads without serializing every call
//     yourself; giving the connection to exactly one thread sidesteps that
//     entirely rather than adding a second lock around every sqlite3_*
//     call). It wakes every `interval_ms` OR when signaled, drains the
//     dirty bitmaps (copies + clears the set indices under the small mutex,
//     which is held only for that scan — microseconds), then for each dirty
//     index calls the table's PersistSnapshotFn — which itself takes the
//     relevant per-GAME lock just long enough to memcpy the slot, releases
//     it, and serializes afterward with no lock held — and finally writes
//     every row from this pass in ONE `BEGIN;...;COMMIT;` transaction.
//   - Crash recovery (persist_start, called once at startup before any
//     other thread exists): opens the DB, ensures the schema, and — if rows
//     already exist — calls each table's PersistLoadFn once per row,
//     synchronously, before spawning the write-behind thread. Single
//     writer/reader at that point (no other thread is running yet), so no
//     locking is needed for the load itself.
//
// Durability guarantee (write-behind, stated plainly — see DURABILITY.md for
// the full argument): everything committed to SQLite survives a crash
// (`kill -9`) intact, because SQLite's WAL + `synchronous=NORMAL` guarantees
// a committed transaction survives a process crash (it does NOT protect
// against an OS-level power-loss/fsync lie, which `synchronous=NORMAL`
// deliberately trades for speed — the documented tradeoff for this "mostly
// in memory, persist when you get a chance" design). Anything mutated in the
// last `interval_ms` (or less, if the crash lands mid-batch) that hadn't yet
// been drained into a transaction is lost — that's the write-behind
// tradeoff, not a bug.
#ifndef FOOLISH_PERSIST_H
#define FOOLISH_PERSIST_H

#include <stdbool.h>

// Turns slot index `idx` of this table into a durable row. Called ONLY from
// the persistence thread, with NO lock held on entry — implementations that
// need to read live, concurrently-mutated data (as foolish_server.c's
// GameSlot/User do) must take their own short-held lock internally, copy
// what they need, release it, and do any real work (serialization) after
// releasing — see game_persist_snapshot/user_persist_snapshot for the
// pattern this exists to support. Fills `out_id` (a NUL-terminated string,
// the row's primary key) and returns bytes written into `buf` (0..cap), or
// -1 if `idx` no longer names a live row (the caller should skip it — this
// engine does not itself infer deletion from -1; see persist_delete for the
// explicit path).
typedef int (*PersistSnapshotFn)(int idx, char *out_id, int id_cap,
                                  unsigned char *buf, int cap);

// Rehydrates one row read back from the DB at startup. Called synchronously
// from persist_start, once per existing row, before any other thread is
// running — implementations write straight into their in-memory arrays
// (g_users[]/g_games[] and their hash-map indexes) with no locking needed.
typedef void (*PersistLoadFn)(const char *id, const unsigned char *blob, int len);

typedef struct PersistTable PersistTable;

// Registers a durable table. `capacity` bounds the largest slot index ever
// passed to persist_mark_dirty for this table (foolish_server.c passes
// MAX_GAMES / MAX_USERS) — the dirty set is a plain bool[capacity], not a
// growable structure, so marking the same index dirty any number of times
// between drains costs the same as marking it once. `blob_cap` bounds the
// largest serialized row this table will ever write (sized with the same
// "documented worst case + margin" discipline VIEW_CACHE_CAP uses — see
// DURABILITY.md for the actual numbers). Must be called BEFORE persist_start.
PersistTable *persist_register_table(const char *sql_table_name, int capacity,
                                      int blob_cap, PersistSnapshotFn snapshot_fn,
                                      PersistLoadFn load_fn);

// Opens `db_path` (WAL + synchronous=NORMAL), ensures every registered
// table's schema exists, loads every existing row through each table's
// PersistLoadFn (crash recovery), then spawns the one persistence thread
// that owns the connection from here on. `db_path == NULL` disables
// persistence entirely (pure in-memory, `--no-db`): every registered
// table's snapshot/load machinery is simply never invoked, and
// persist_mark_dirty/persist_delete become no-ops. Returns false only on a
// real failure to open/configure a requested (non-NULL) db_path — callers
// should treat that as fatal (a requested durability guarantee that can't
// be met should not silently downgrade to "pretend it's fine").
bool persist_start(const char *db_path, int interval_ms);

// True iff persist_start was called with a non-NULL db_path and it
// succeeded — lets callers (e.g. h_signup) skip even the trivial dirty-mark
// cost when persistence is off, though persist_mark_dirty is itself already
// a safe, cheap no-op in that case.
bool persist_enabled(void);

// O(1), lock-only, never touches disk: marks slot `idx` of `table` dirty.
// The next drain (interval or a future explicit signal) will snapshot +
// upsert it. Safe to call at any time, including before persist_start (a
// no-op then) and when persistence is disabled.
void persist_mark_dirty(PersistTable *table, int idx);

// Enqueues an explicit row deletion by id (NOT indexed by slot — a deleted
// slot may not have a stable index concept once gone). Drained the same way
// as dirty marks, in the same transaction. Not currently called by
// foolish_server.c (this POC's game/user stores never free a slot — see
// their own "insert-only, no delete" comments), but implemented because the
// schema and the write path both need to support it honestly rather than
// silently dropping deletes if that ever changes. See DURABILITY.md.
void persist_delete(PersistTable *table, const char *id);

#endif

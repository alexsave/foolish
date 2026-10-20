// journal.h - the move journal: `cbase serve`'s write-behind in front of the SQL tables
// (c_base/DESIGN.md "The move journal").
//
// A plain human move does not write the tables. It is coded (replay_move.h: the kernel menu's
// index on the replay codec's coder) into a record, appended with every other record of
// the same moment straight into an append-only segment file's mapped pages (no system
// call), and acknowledged once the batch's header, stored last, commits it: the durability
// the tables' WAL at synchronous=NORMAL gave (a process crash keeps the page cache, an OS
// crash may not; opts.sync adds an msync per batch). The game's
// new row and its moves are held in memory (the overlay) and the store's reads see them
// first: one game by its id, the dashboard list and the bot scan alike.
//
// THE FLUSH RULE (its one owner: flush_due): a game's waiting moves go to the tables when
// the game is written around the journal (its end, a seat change, its delete: below), when
// its oldest waiting move is opts.game_age_ms old, or when its waiting moves hold
// opts.game_bytes; every game's go when all the waiting moves together hold
// opts.total_bytes, at close, and for a reader of the file behind the store (the admin's
// SQL, an import). A segment is closed once it holds opts.seg_bytes and deleted once
// every move in it is in the tables and (with a backup attached) in an uploaded copy.
//
// Everything else is written at once, as before: a commit that is not one plain human move
// (a deal, a seat change, the end of a game) and a game's delete write that game's waiting
// moves first, in the same transaction.
//
// At start the tables are read and the journal's segments replayed through the kernel
// (the app's apply), in order, before anything is served; a torn or corrupt batch at a
// segment's end is dropped (it was never acknowledged). A record the kernel's menu does not
// confirm refuses the start.
//
// SEGMENT FILE <db>-journal.<seq> (16 digits; a start numbers its first segment from the
// wall clock's ms, so a number is never reused across restarts): "FJNL", u32 LE format (1), u64 LE start ms, then
// batches. A BATCH: u8 'J', u8 format (1), u16 LE payload length, u32 LE FNV-1a 32 over
// those 4 bytes and the payload, then the payload: ONE replay-coder integer (replay.h),
// coded independently of every other batch, holding
//   n           varint, the records
//   base        varint, zigzag(the batch's earliest ms - the segment's start ms)
//   per record: slot    uniform over N + 1, N = the slots the segment named so far; N
//                       names a new game: its code's length uniform over
//                       CB_GAME_ID_MAX + 1, each byte uniform over 256, and its version
//                       before the move as a varint
//               (old)   uniform over 2: 0 the move follows the slot's last one, 1 a varint
//                       version follows (the game was written around the journal between)
//               ms      varint, the move's ms - base
//               kind    uniform over 2: 0 a human's move, 1 a bot cycle, then the cycle's
//                       moves less one, uniform over BOT_DRIVE_MAX_ACTIONS
//               moves   replay_move_code each, on the board the one before it left
#ifndef CB_JOURNAL_H
#define CB_JOURNAL_H

#include "store/rows_foolish.h"
#include "replay_move.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct cb_journal cb_journal;

// Replays one record at start: `row` is the game as it stood before it (a bot cycle also gets
// the game's session log, which seeds its moves), m the record. Fills *out (owned byte fields) with the row the commit wrote and *log with the
// session-log records it appended (malloc'd, may be NULL with len 0). CB_OK, or anything
// else to refuse the start.
struct cb_jmove;
typedef int (*cb_journal_apply_fn)(void *ud, const cb_game_row *row, const uint8_t *session_log, size_t session_log_len,
                                   const struct cb_jmove *m, cb_game_row *out, uint8_t **log, size_t *log_len);

typedef struct {
    const char *path;          // the database's path: segments are <path>-journal.<seq>
    int64_t     game_age_ms;   // a game's oldest waiting move waits at most this long (0: 600000)
    int64_t     game_bytes;    // a game whose waiting moves hold this many bytes is flushed (0: 256 KiB)
    int64_t     total_bytes;   // every game is flushed once all waiting moves hold this many (0: 64 MiB)
    int64_t     seg_bytes;     // a segment is closed once it holds this many bytes (0: 8 MiB)
    bool        sync;          // msync each batch before it is acknowledged
    int         max_games;     // games the overlay keeps resident; past it the least recently used idle ones go (0: 4096)
    cb_journal_apply_fn apply;
    void       *apply_ud;
} cb_journal_opts;

// Recovers the segments into the tables and installs the journal on s (cb_rows_of then
// answers the journal's operations). CB_OK, or why the store must not be served (err).
int cb_journal_open(cb_store *s, const cb_journal_opts *o, char *err, size_t cap);
// Flushes everything, deletes the segments and uninstalls. Nothing may commit meanwhile.
void cb_journal_close(cb_store *s);
cb_journal *cb_journal_of(const cb_store *s);   // NULL: none installed
const cb_rows_ops *cb_journal_rows(void);

// THE GAME'S ONE OWNER under serve: the overlay holds every game an operation touched (its
// acknowledged state; the tables lag it by the waiting moves), so an operation reads its game
// here and nowhere else. fn runs under the overlay's lock with the game's row (borrowed: it
// copies what it keeps) and, when the overlay holds the game decoded, its resident (table.h
// TableResident: the operation adopts a copy, and then must not read the row's state and
// roster bytes, which may be stale until encoded; the columns are always the game's). With
// res NULL the row's bytes are the game's: the caller decodes them and may hand the result
// back (cb_journal_game_decoded). A game the overlay does not hold is read from the tables
// and made resident first. ONLY UNDER THE GAME'S LOCK (app/table_ops.c cb_table_lock): a row read
// from the tables is made resident only because no commit of the game can land between the
// read and the insert. CB_OK or fn's result, CB_E_NOT_FOUND for no such game, or the tables'
// error. A resident game leaves when it is idle (IDLE_EVICT_MS, nothing waiting), when over
// opts.max_games the least recently used idle ones do (at the flush thread's next pass),
// and on an admin's write (THE ADMIN RULE).
struct TableResident;
typedef int (*cb_journal_game_fn)(void *ud, const cb_game_row *row, const struct TableResident *res);
int cb_journal_game_acquire(cb_store *s, const char *id, cb_journal_game_fn fn, void *ud);
// The game decoded from the row acquire gave at `version` (its bytes, res NULL): kept as its
// resident if the game is still at that version, so the next operation adopts instead of
// decoding. Under the game's lock.
void cb_journal_game_decoded(cb_store *s, const char *id, int64_t version, const struct TableResident *res);

// Every acknowledged move into the tables now (store.h cb_store_settle).
int cb_journal_flush(cb_store *s);
// The flush rule's periodic check now (the flush thread's tick): the games due, flushed.
int cb_journal_flush_due(cb_store *s);

typedef struct {
    long batches, writes, records, bytes;   // bytes: every byte written to segments
    long flushes, flushed_games, flushed_moves;
    long recovered;                         // records replayed at the last open
    long waiting, waiting_bytes;            // moves not in the tables yet, and their bytes
    long segments;                          // segment files on disk
    long games;                             // games resident in the overlay
    long table_reads;                       // games read from the tables to be made resident
} cb_journal_stats;
void cb_journal_stats_get(const cb_store *s, cb_journal_stats *out);

// ---- the batch codec (pure; unit_journal) ------------------------------------------------
// One record: one commit of one game, a human's move or a bot cycle's moves.
typedef struct cb_jmove {
    char       game[CB_GAME_ID_MAX + 1];
    int64_t    base_version;   // the game's version before the commit
    int64_t    now_ms;
    bool       bots;           // a bot drive's cycle (bot_drive.h), else one human move
    int        n_mv;           // 1 for a human's; 1..BOT_DRIVE_MAX_ACTIONS for a cycle's
    ReplayMove mv[BOT_DRIVE_MAX_ACTIONS];
} cb_jmove;

// A segment's slot table: the games it named, in order.
typedef struct cb_jslots cb_jslots;
cb_jslots *cb_jslots_new(void);
void cb_jslots_free(cb_jslots *t);
int cb_jslots_count(const cb_jslots *t);

#define CB_JOURNAL_BATCH_HDR 8
// One batch of n moves (n <= CB_JOURNAL_BATCH_MAX) into out, the slot table brought
// forward. Its length, or a negative CB_E_* (nothing is consumed from t then).
#define CB_JOURNAL_BATCH_MAX 64
int cb_journal_batch_encode(cb_jslots *t, int64_t seg_start_ms, const cb_jmove *m, int n, uint8_t *out, int cap);
// The batch at in: its moves into out (cap of them), *used its length. The count, or -1
// for a batch that is torn (runs past len) or corrupt (header, checksum or payload).
int cb_journal_batch_decode(cb_jslots *t, int64_t seg_start_ms, const uint8_t *in, size_t len, size_t *used,
                            cb_jmove *out, int cap);

#endif

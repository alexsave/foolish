// journal.h - the move journal: `cbase serve`'s write-behind in front of the SQL tables
// (c_base/DESIGN.md "The move journal").
//
// A plain human move does not write the tables. It is written as a fixed record (its seat and
// awire bytes: the commit's move list, table.h Table.moves, the delta cast's own) into a batch,
// appended with every other record of the same moment straight into an append-only segment
// file's mapped pages (no system
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
// SQL, an import). A flush is bounded transactions of at most opts.txn_moves waiting moves,
// the oldest games first, the flush lock given back between them. A segment is closed once it holds opts.seg_bytes and deleted once
// every move in it is in the tables and (with a backup attached) in an uploaded copy.
//
// Every other commit (a deal, a seat change, a lobby edit, an end, a rated one too) is journaled
// WHOLE, as a full record (below): the overlay keeps its row and seats, the store's membership
// reads see them, and the flush writes the row, its session-log changes, the seats and a rated
// end's rating rows. A rated end makes its game due at once (the leaderboard and the profiles
// read the tables), and its changes are THE RATINGS' ONE OWNER's (cb_journal_seat_ratings) from
// its commit on; a read of the ratings table writes every waiting rated end first (journal.c THE
// RATINGS READ RULE), so it never answers older ratings than an end a client was told. A game's delete is written at once, around the journal, with that game's
// waiting commits first in the same transaction.
//
// At start the tables are read and the journal's segments replayed through the kernel
// (the app's apply), in order, before anything is served; a torn or corrupt batch at a
// segment's end is dropped (it was never acknowledged). A record the kernel does not apply
// (a move it refuses, a cycle the drive does not play again) refuses the start.
//
// SEGMENT FILE <db>-journal.<seq> (16 digits; a start numbers its first segment from the
// wall clock's ms, so a number is never reused across restarts): "FJNL", u32 LE format (2), u64 LE start ms, then
// batches. A segment of another format refuses the start: a deploy stops the old server, whose
// close flushes every move into the tables and deletes its segments, before the new one opens.
// A BATCH: u8 'J', u8 format (3), u16 LE payload length, u32 LE CRC-32C over those 4 bytes
// and the payload (shared/c/crc32c), then the payload, fixed fields and no coder:
//   u8  n               the records, 1..CB_JOURNAL_BATCH_MAX
//   i32 LE base         the batch's earliest ms - the segment's start ms
//   per record:
//     u8  flags         1 a bot cycle (else one human move), 2 a new game, 4 a version follows
//     new game (2):     u8 id length, the id; it takes the segment's next slot
//     else:             u16 LE slot, a game the segment named before
//     (2 or 4)          u32 LE version before the commit; without it the record follows the
//                       slot's last one (4: the game was written around the journal between)
//     u32 LE ms         the record's ms - base
//     the moves         evwire.h THE MOVE LIST: u8 n, then { u8 seat, u8 wire length, the
//                       awire bytes } per move (one for a human's move, a cycle's in order)
// A FULL RECORD (one whole commit): u8 'R', u8 format (3), u16 0, u32 LE CRC-32C over the
// payload length and the payload, u32 LE payload length, then the payload: u8 id length, the
// id, i64 LE base version, i64 LE ms, u8 flags (1 clears the session log, 2 has a deal seed,
// 4 needs bots, 8 rated), i8 status, i64 LE log_from, round_epoch and created_at, the 32-byte seed when
// flagged, then u32 LE length + bytes each of the state blob, the roster and the move list
// chunk it appended (move_list.h; format 2 held the session-log records), and when rated (flag 8) the rating changes: u8 n, then per seat u8 id
// length, the id, u8 1 for a bot, i32 LE delta, u8 name length, the name. Recovery adopts its row as the game's (no kernel run: the record is
// the commit's result) when the game is at its base version. It names no slot: a move after
// it codes its version (the "old" bit).
#ifndef CB_JOURNAL_H
#define CB_JOURNAL_H

#include "store/rows_foolish.h"
#include "evwire.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct cb_journal cb_journal;

// The segments' and records' format: 3, a whole commit carries its move list chunk (move_list.h;
// 2 carried its session-log records; 1 was the replay coder and FNV-1a). A FORMAT-2 SEGMENT IS
// READ TOO: its batches are format 3's byte for byte, and a whole commit's records are chunked
// into its move list as they are read (table_move_list_chunk, from the record's created_at), so a
// server SIGKILLed during the swap to format 3 leaves segments the new one recovers. A segment of
// any other format is refused at open: the server that wrote it flushes everything when it
// stops cleanly.
#define CB_JOURNAL_FORMAT 3
#define CB_JOURNAL_FORMAT_PREV 2

// Replays one record at start: `row` is the game as it stood before it, m the record (a bot
// cycle is seeded from the row's bot memory alone, as the live cycle is). Fills *out (owned
// byte fields) with the row the commit wrote and *log with the move list chunk it appended
// (move_list.h; malloc'd, may be NULL with len 0). CB_OK, or anything else to refuse the start.
struct cb_jmove;
typedef int (*cb_journal_apply_fn)(void *ud, const cb_game_row *row, const struct cb_jmove *m, cb_game_row *out,
                                   uint8_t **log, size_t *log_len);

typedef struct {
    const char *path;          // the database's path: segments are <path>-journal.<seq>
    int64_t     game_age_ms;   // a game's oldest waiting move waits at most this long (0: 600000)
    int64_t     game_bytes;    // a game whose waiting moves hold this many bytes is flushed (0: 256 KiB)
    // Every game is flushed once all waiting moves hold this many (0: 16 MiB). The default is
    // the production box's: a t4g.nano has 512 MB for the OS, the server and the tables' cache.
    int64_t     total_bytes;
    int64_t     seg_bytes;     // a segment is closed once it holds this many bytes (0: 8 MiB)
    int         txn_moves;     // the waiting moves one flush transaction carries at most; a pass is as
                               // many transactions as it needs, the flush lock given back between them
                               // (0: 2000; one game with more goes alone)
    bool        sync;          // msync each batch before it is acknowledged
    int         max_games;     // games the overlay keeps resident; past it the least recently used idle ones go (0: 4096)
    // The network loop (net/loop.h), or NULL. ON ITS THREAD THE JOURNAL NEVER WAITS: an operation
    // that would (a read or write of the tables, a write around the journal) returns
    // CB_E_WOULD_BLOCK having done nothing, and the request runs again on the pool.
    struct cb_loop *loop;
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
//
// THE HOLD: with `held` non-NULL, a game the overlay holds is also held for the caller (*held, else
// NULL: not resident, or fn failed) until cb_journal_game_release: never evicted meanwhile, and a
// commit that names it (cb_game_commit.held) is fenced and applied in place, with no lookup by id.
// Unlinked while held (a conflict, an admin's write), it is freed by the release.
struct TableResident;
typedef struct cb_jhold cb_jhold;
typedef int (*cb_journal_game_fn)(void *ud, const cb_game_row *row, const struct TableResident *res);
int cb_journal_game_acquire(cb_store *s, const char *id, cb_journal_game_fn fn, void *ud, cb_jhold **held);
void cb_journal_game_release(cb_store *s, cb_jhold *held);   // NULL: nothing
// The game decoded from the row acquire gave at `version` (its bytes, res NULL): kept as its
// resident if the game is still at that version, so the next operation adopts instead of
// decoding. Under the game's lock.
void cb_journal_game_decoded(cb_store *s, const char *id, int64_t version, const struct TableResident *res);
// The game as the overlay holds it, read WITHOUT the game's lock (a bot drive, whose commit is
// fenced): fn as for acquire. CB_E_NOT_FOUND when the overlay does not hold it (nothing is
// read from the tables, nothing made resident).
int cb_journal_game_peek(cb_store *s, const char *id, cb_journal_game_fn fn, void *ud);
// A game the tables just created (row, as written) made resident with its decoded state, so
// its first operation reads no table. Nobody else knows the id yet: no lock is needed.
void cb_journal_game_created(cb_store *s, const cb_game_row *row, const struct TableResident *res);

// THE SEATS' RATINGS under serve (THE RATINGS' ONE OWNER, journal.c jrating): each id's rating
// (TABLE_RATING_NONE: no row) as the tables hold it plus every acknowledged rated end not flushed
// yet, and whether every account behind them is min_age_ms old at now_ms (an id with no account
// - a bot, a deleted user - is no reason). A seat not held yet is read from the tables first, so
// on the network loop's thread that answers CB_E_WOULD_BLOCK having read nothing (a join, a deal,
// any pool operation reads its seats here first, and the end then runs on the loop). CB_OK, or
// the tables' error.
int cb_journal_seat_ratings(cb_store *s, const char *const *ids, int n, int64_t now_ms, int64_t min_age_ms,
                            int32_t *ratings, bool *old_enough);

// Every acknowledged move into the tables now (store.h cb_store_settle).
int cb_journal_flush(cb_store *s);
// The flush rule's periodic check now (the flush thread's tick): the games due, flushed.
int cb_journal_flush_due(cb_store *s);

typedef struct {
    long batches, writes, records, bytes;   // bytes: every byte written to segments
    long flushes, flushed_games, flushed_moves;
    long flush_failures;                    // flush transactions that failed (their moves stay, and the next flush retries)
    long recovered;                         // records replayed at the last open
    long waiting, waiting_bytes;            // moves not in the tables yet, and their bytes
    long segments;                          // segment files on disk
    long games;                             // games resident in the overlay
    long table_reads;                       // games read from the tables to be made resident
    long log_reads;                         // session logs read from the tables (the rest was the overlay's)
    long loop_writes;                       // of the writes, made on the network loop's thread
    long loop_seg_opens;                    // segments opened on it by a wait (none: loop_write rotates at once or not at all)
    long txn_moves_max;                     // the most waiting moves one flush transaction carried
    long txn_hold_us_max;                   // the longest one held the flush lock (a write around the journal's worst wait)
    long flusher_wakes;                     // times the flush thread woke (no tick: an idle box wakes it never)
    long lookups;                           // games found by id in the overlay (a human's move: one, its acquire; THE HOLD)
} cb_journal_stats;
void cb_journal_stats_get(const cb_store *s, cb_journal_stats *out);
// What the overlay holds of a game, for a test: its stamps (THE JOURNAL'S TIME, journal.c j_mono:
// its last operation's, its oldest waiting move's or 0), its decoded game's buffer (compared, never
// read: NULL when it has none) and the holds on it (THE HOLD). CB_OK, or CB_E_NOT_FOUND when the
// overlay does not hold it. Not counted in the stats' lookups.
typedef struct {
    int64_t     touched_ms, first_ms;
    const void *resident;
    int         held;
} cb_journal_game_facts;
int cb_journal_game_info(const cb_store *s, const char *id, cb_journal_game_facts *out);

// ---- the batch codec (pure; unit_journal) ------------------------------------------------
// One record: one commit of one game, a human's move or a bot cycle's moves.
typedef struct cb_jmove {
    char           game[CB_GAME_ID_MAX + 1];
    int64_t        base_version;   // the game's version before the commit
    int64_t        now_ms;
    bool           bots;           // a bot drive's cycle (bot_drive.h), else one human move
    int            n_mv;           // 1 for a human's; 1..EVW_DELTA_MAX_MOVES for a cycle's
    const EvwMove *mv;             // the moves: the commit's own (table.h Table.moves, borrowed
                                   // until the record is written), or a decoder's store
} cb_jmove;

// A segment's slot table: the games it named, in order.
typedef struct cb_jslots cb_jslots;
cb_jslots *cb_jslots_new(void);
void cb_jslots_free(cb_jslots *t);
int cb_jslots_count(const cb_jslots *t);

#define CB_JOURNAL_BATCH_HDR 8
#define CB_JOURNAL_BATCH_MAX 64
// The most one record's payload takes: flags, a new game's id, a version, the ms, a cycle's moves.
#define CB_JOURNAL_RECORD_MAX (1 + 1 + CB_GAME_ID_MAX + 4 + 4 + 1 + EVW_DELTA_MAX_MOVES * (2 + EVW_DELTA_WIRE_MAX))
// The most one batch takes, its header too.
#define CB_JOURNAL_BATCH_BYTES (CB_JOURNAL_BATCH_HDR + 1 + 4 + CB_JOURNAL_BATCH_MAX * CB_JOURNAL_RECORD_MAX)
// One batch of the n records m[0..n) (n <= CB_JOURNAL_BATCH_MAX) into out, the slot table
// brought forward. Its length, or a negative CB_E_* (nothing is consumed from t then).
int cb_journal_batch_encode(cb_jslots *t, int64_t seg_start_ms, const cb_jmove *const *m, int n, uint8_t *out, int cap);
// The batch at in: its records into out (cap of them), each one's moves into mv[i] (out[i].mv
// points there), *used its length. The count, or -1 for a batch that is torn (runs past len)
// or corrupt (header, checksum or payload).
int cb_journal_batch_decode(cb_jslots *t, int64_t seg_start_ms, const uint8_t *in, size_t len, size_t *used,
                            cb_jmove *out, EvwMove (*mv)[EVW_DELTA_MAX_MOVES], int cap);

#endif

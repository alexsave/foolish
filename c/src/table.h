// table.h - a game plus its roster, as the servers and fixtures use it.
//
// A Game (game.h) is the board and a Roster (roster.h) is who sits at it. A
// server never wants one without the other: it loads both from a row, resolves
// the acting seat from the caller's auth id, runs a move or a lobby edit, and
// writes back every product the row and the realtime channels need. This layer
// is where those kernel pieces meet (docs/C_GAME_SHAPE_MIGRATION.md 2.3-2.5,
// 2.9), so that no host restates any of it:
//
//   table_load              state blob + durable roster -> a checked table
//   table_act               a move, the seat resolved HERE from the actor id
//   table_commit_products   the state blob, the roster blob, the session-log
//                           records, one response envelope per human seat, the
//                           spectator envelope, and the scalar columns
//   table_push              one viewer's realtime payload (as3, evwire.h)
//
// ONE KERNEL SECTION. The Game is host storage (the resident slot, in wasm) and
// so are the snapshots an operation captures. A host runs load, operate, and
// copies every product out, synchronously, before anything else may touch the
// kernel: nothing here survives an await (engine.ts "Packed wire pipeline").
//
// Every string crosses as bytes with a length; a length is never a NUL.
#ifndef CNITRO_TABLE_H
#define CNITRO_TABLE_H

#include "game.h"
#include "view.h"
#include "roster.h"
#include "evwire.h"
#include "bot_drive.h"
#include <stddef.h>

// ---- results ----------------------------------------------------------------
//
// Non-negative is an outcome, negative a refusal. A refused load returns the
// state's GAME_INVALID_* (-1..-12) unchanged, or one of the TABLE_E_* below.
// Append-only numbering: hosts log and map these numbers.
#define TABLE_OK              0
#define TABLE_APPLIED         0
#define TABLE_REJECTED        1   // the kernel refused the move; Table.reject holds the ENGINE_REJECT_*
#define TABLE_MOOT            2   // a no-op: a move on a game already over, a ready on a game already dealt
#define TABLE_STALE_ROUND     3   // the move was composed before the current round began
#define TABLE_EMPTY           4   // the last seat left the lobby: the host deletes the row
#define TABLE_E_STATE_VERSION (-101)  // the state blob's format byte is not one this kernel reads
#define TABLE_E_ROSTER        (-102)  // the durable roster was refused; Table.detail holds the ROSTER_E_*
#define TABLE_E_MISMATCH      (-103)  // the state's seat count is not the roster's
#define TABLE_E_UNKNOWN_BRAIN (-104)  // a bot seat names a brain this build cannot play
#define TABLE_E_NOT_SEATED    (-105)  // the actor id is not a seat at this table
#define TABLE_E_NOT_WAITING   (-106)  // a lobby edit on a game that is not in its lobby
#define TABLE_E_FORBIDDEN     (-107)  // the lobby policy refuses this actor this edit
#define TABLE_E_WIRE          (-108)  // the action wire or request is malformed
#define TABLE_E_CAP           (-109)  // an output buffer is too small
#define TABLE_E_NOT_LOADED    (-110)  // no table has been loaded
#define TABLE_E_NOT_OVER      (-111)  // continue on a game that has not ended
#define TABLE_E_REPLAY_VERIFY (-112)  // a replay code that does not decode back to its session log

// The response code for a stale-round refusal. It sits above the kernel's
// ENGINE_REJECT_* space so a client can tell a rules rejection from a server
// policy one by the number alone (sdk/ts/wire/awire.ts REJECT_STALE_ROUND).
#define TABLE_REJECT_STALE_ROUND 100

// The durable state blob: [STATE_BLOB_FORMAT][deterministic deck][state_put].
// The format, and the codec this layer writes and reads it with, are view.h's
// (state_blob_put / state_blob_load) - the wasm bridge persists the same column
// through the same pair, so there is one format byte, not two that can drift.
#define TABLE_STATE_FORMAT STATE_BLOB_FORMAT

// ---- the action request and response (the `action` endpoint's body) --------
//
//   v1  u8 fmt=1, u8 gid_len, gid, wire                    (no round guard)
//   v2  u8 fmt=2, u8 gid_len, gid, u32 intent_version, wire
//   response: u8 fmt=1, u8 status (0 applied, 1 rejected, 2 moot),
//             u8 reject code, u32 version
#define TABLE_REQ_FORMAT_V1   1
#define TABLE_REQ_FORMAT_V2   2
#define TABLE_RESP_FORMAT     1
#define TABLE_RESP_BYTES      7
#define TABLE_STATUS_APPLIED  0
#define TABLE_STATUS_REJECTED 1
#define TABLE_STATUS_MOOT     2

typedef struct {
    int8_t   fmt;
    bool     has_intent;   // v2: the move names the version it was composed against
    uint8_t  gid_len;
    int32_t  gid_off;      // offsets into the request bytes
    int32_t  wire_off;
    int32_t  wire_len;
    uint32_t intent;
} TableRequest;

// Parses a request body; 0, or TABLE_E_WIRE. The wire itself is judged by
// table_act, not here.
int table_request_decode(const uint8_t *p, int len, TableRequest *out);

// The response body for a table_act result: TABLE_RESP_BYTES, or TABLE_E_CAP.
// A refusal of the request as a whole (a negative result) is not a response.
int table_action_response(int result, int reject, uint32_t version, uint8_t *out, int cap);

// ---- the table ---------------------------------------------------------------

// Snapshots an operation's engine hooks captured, for its event pushes. Only
// the log-free Game prefix is kept (state_put and the event walk read nothing
// past it). The wasm builds size MAX_SNAPS for one action's worst case.
#ifndef MAX_SNAPS
#define MAX_SNAPS 48
#endif
#define TABLE_SNAP_BYTES offsetof(Game, num_logs)
typedef struct { _Alignas(8) unsigned char bytes[TABLE_SNAP_BYTES]; } TableSnapSlot;
typedef struct {
    int n;
    int tag[MAX_SNAPS];
    int aux[MAX_SNAPS];
    TableSnapSlot slot[MAX_SNAPS];
} TableSnaps;

typedef struct {
    Game       *g;          // host storage: the board (the resident slot in wasm)
    TableSnaps *snaps;      // host storage: this operation's hook snapshots
    Roster      r;
    uint32_t    rng_base;   // the secret mid-game RNG base (game.h game_state_seed)
    int32_t     detail;     // the ROSTER_E_* behind a TABLE_E_ROSTER
    int32_t     reject;     // the ENGINE_REJECT_* behind a TABLE_REJECTED
    // The scope of the last operation, which its products describe.
    bool        loaded;
    bool        ended;          // this operation finished the game
    bool        dealt_now;      // this operation dealt (the session log restarts)
    bool        roster_changed; // this operation edited the roster
    bool        lobby_event;    // this operation is a lobby edit its pushes announce
    bool        pre_has_flip;   // the face-up trump before the operation (DRAW privacy)
    Card        pre_flip;
    uint32_t    pre_good_mask;  // game_shown_good_mask before the operation (goods_changed)
    int8_t      actor;          // the acting seat, -1 for none
    int32_t     log_start;      // the operation's first log record
    int32_t     log_len;        // records the ROW's session log holds (table_set_session_log), loaded or not
    // The preferred moves the last table_bot_drive was offered (BotDrivePref).
    int8_t      n_prefs;
    BotDrivePref prefs[MAX_PLAYERS];
} Table;

// Points a table at its storage. Nothing is loaded.
void table_init(Table *t, Game *g, TableSnaps *snaps);

// Loads a row. The roster is decoded, every bot seat's brain must be one this
// build links, the seat counts must agree, and the state goes through
// state_import (game_validate), so a WAITING row that holds cards is refused.
// Each seat's kind (Player.strategy_key) is set from the roster, and the
// deterministic-deck flag from the blob. On any refusal nothing is loaded.
int table_load(Table *t, const uint8_t *state, int state_len, const uint8_t *roster, int roster_len);

// Each seat's kind from the roster: STRATEGY_KEY_HUMAN for an empty brain, else
// the linked bot_roster entry's STRAT_* id. The state blob carries no kinds, so a
// host restoring a Game from a stored state and roster sets them from this, as
// table_load does. TABLE_OK, or TABLE_E_UNKNOWN_BRAIN for a brain this build
// does not link (kinds is then partly written).
int table_seat_kinds(const Roster *r, int8_t *kinds);

// A move by the seat whose roster id is EXACTLY actor_id. In order: a finished
// game is TABLE_MOOT; a move composed before the current round (intent_version
// >= 0 and below round_epoch) is TABLE_STALE_ROUND; an actor with no seat is
// TABLE_E_NOT_SEATED; a malformed wire is TABLE_E_WIRE; a move the rules refuse
// is TABLE_REJECTED. An applied move that ends the game finalizes it (GAME_OVER,
// bots parked READY and humans IDLE).
int table_act(Table *t, const char *actor_id, int id_len, const uint8_t *awire, int wire_len,
              int64_t intent_version, int64_t round_epoch);

// The seat whose roster id is EXACTLY these bytes, or -1; TABLE_E_NOT_LOADED
// when no table is loaded. A host picks the caller's response envelope with it.
int table_seat_of(const Table *t, const char *id, int id_len);

// PLAYING, and a bot seat is still IN: the row's needs_bots column.
bool table_needs_bots(const Table *t);

// A bot that could act now uses the session log (a belief brain), so the host
// must hydrate it before driving.
bool table_bots_need_logs(const Table *t);

// ---- lobby edits (docs/C_GAME_SHAPE_MIGRATION.md Q9) --------------------------
//
// The lobby policy: every edit needs a SEATED actor, except join. A seated
// player may remove another human (the web's "remove player"); bots are added
// and removed as bots; a hand is only ever rearranged by its own seat. Each edit
// that changes what a viewer sees marks the table for one MAGIC_TRANSITION push
// on the committed board, and marks the roster changed when it changed.
// Refusals from the Roster come back as TABLE_E_ROSTER with the ROSTER_E_* in
// Table.detail: a full table (E_FULL), a duplicate join (E_DUPLICATE), a bad
// title (E_TITLE), a reseat that is not a permutation (E_PERM).
//
// A deal seed is FOOLISH_SEED_LEN bytes the host draws from crypto; the edits
// that can deal (ready, add-bot) take one whether or not they deal.

// A new lobby with the creator in seat 0, titled "<name>'s Game" (the name cut
// on a scalar boundary if the title would pass ROSTER_TITLE_MAX).
int table_create(Table *t, const char *actor_id, int id_len, const char *name, int name_len);
int table_join(Table *t, const char *actor_id, int id_len, const char *name, int name_len);
// Removes the human `target_id` (the actor themself, or another human). The
// last seat leaving returns TABLE_EMPTY; the host deletes the row.
int table_leave(Table *t, const char *actor_id, int id_len, const char *target_id, int target_len);
// Seats a bot READY; deals when that makes every seat ready.
int table_add_bot(Table *t, const char *actor_id, int id_len, const char *bot_id, int bot_len,
                  const char *nick, int nick_len, const char *brain, int brain_len, const uint8_t *deal_seed);
int table_remove_bot(Table *t, const char *actor_id, int id_len, const char *bot_id, int bot_len);
// Readies the actor; deals when game_lobby_can_deal. TABLE_MOOT once dealt.
int table_ready(Table *t, const char *actor_id, int id_len, const uint8_t *deal_seed);
// The new seat order as the seated ids: n x { u8 len, id bytes }.
int table_reseat(Table *t, const char *actor_id, int id_len, const uint8_t *ids, int ids_len);
// The lobby's title rule: at most 50 characters as the web counts them (UTF-16
// code units, before trimming), then trimmed of JavaScript whitespace, not empty.
int table_retitle(Table *t, const char *actor_id, int id_len, const char *title, int title_len);
// A finished game back to its lobby (game_reset_to_lobby with the roster's bots).
int table_continue(Table *t, const char *actor_id, int id_len);
// The actor's own hand: new card i is old card idx[i]. TABLE_E_WIRE when idx is
// not a permutation of the hand. No push: nobody else can see a hand's order.
int table_rearrange_hand(Table *t, const char *actor_id, int id_len, const uint8_t *idx, int n);
// Account deletion: the seat holding user_id is renamed.
int table_redact(Table *t, const char *user_id, int id_len, const char *name, int name_len);

// ---- fixtures ------------------------------------------------------------------

// A board and a roster a test composed field by field, checked exactly as a
// stored row is. Writes [state blob][durable roster] into `out` and loads them
// into `t` with table_load, so what a fixture hands back is a row the kernel has
// already accepted. `g` may be the table's own board (t->g): it is serialized
// before the load adopts anything. Returns the state blob's length (the roster
// follows it, ROSTER_BYTES long), or the refusal: TABLE_E_ROSTER with the
// ROSTER_E_* in t->detail for a roster that does not encode, TABLE_E_CAP, or
// whatever table_load says of the row.
int table_seal(Table *t, const Game *g, const Roster *r, uint8_t *out, int cap);

// ---- products ----------------------------------------------------------------

typedef struct { int32_t off, len; } Span;

typedef struct {
    int8_t  status;        // GAME_STATUS_*: the row's status column (the blob is authoritative)
    int8_t  fool;          // the fool's seat once the game is over, else -1
    int8_t  num_players;
    // Events each push carries. 0 USED TO MEAN "nothing to broadcast", and that
    // was wrong: see goods_changed below.
    uint8_t n_events;
    bool    needs_bots;
    bool    closed_round;  // the operation's records hold a PICKUP or DISCARD (and it dealt nothing)
    bool    logs_reset;    // the operation dealt: its records start a new session log
    bool    ended;
    bool    dealt_now;
    bool    roster_changed;
    // THE OPERATION MOVED THE GOODS, which a push must carry even when the
    // operation moved no card. A `good` emits no event - the kernel has no card
    // to fly - so for years the adapter's `n_events > 0` test dropped the push
    // entirely and the mark only arrived folded into whatever moved next.
    //
    // That test was never the kernel's. It came from the TypeScript server
    // (`isPassive && moveEvents === 0`) and c/src/bot_drive.c's classify() was
    // then written to MIRROR it, so the kernel ended up mirroring a mirror.
    // Meanwhile the iMessage client has always been built for exactly the push
    // this enables - ios/FoolishKit/Boards/MessageTableView+Sequence.swift seeds
    // its roles "AHEAD OF THE EMPTY-STREAM GUARD, because the stream that needs
    // it most is the empty one: a `good` that does not close the bout emits no
    // step, so the difference between these two role states is the ENTIRE move."
    // It never received one, because the server never sent one.
    //
    // A good being SET is somebody's move (round 21, same file: "A GOOD IS A
    // MOVE, SO IT PLAYS FIRST"), and a move is worth a broadcast.
    //
    // BUT ONLY A GOOD A VIEWER IS SHOWN: this compares game_shown_good_mask
    // (game.h), so a bot's SILENT good - said over an uncovered table, which no
    // human can do - leaves it false and is bundled with the next card instead.
    bool    goods_changed;
    Span    state;         // durable state blob
    Span    roster;        // durable roster (ROSTER_BYTES)
    Span    logs;          // session-log records, u48 LE ms timestamp each; len 0 when none
    Span    views[MAX_PLAYERS];  // the response envelope per HUMAN seat; len 0 for a bot or no seat
    Span    spectator;     // the spectator envelope
} TableCommit;

// Every product of the loaded table and its last operation, written into
// `arena`. `next_version` is the version the commit will produce (the envelope
// carries it); `now_ms` stamps this operation's log records.
int table_commit_products(const Table *t, const char *game_id, int gid_len, uint32_t next_version,
                          int64_t now_ms, TableCommit *out, uint8_t *arena, int cap);

// One viewer's response envelope (seat, or -1 for the spectator): the header
// (view.h ENV_*, written with env_header_write and read back by the client
// through env_header_read), the view blob, then the roster trailer (roster.h).
int table_envelope(const Table *t, const char *game_id, int gid_len, int viewer, uint32_t version,
                   uint8_t *out, int cap);

// One viewer's realtime payload for the last operation (evwire.h as3): the
// event sequence masked for `viewer` (seat, or -1), the flags byte, and the new
// roster when the operation changed it. A bot seat has no viewer.
int table_push(const Table *t, const char *game_id, int gid_len, int viewer, uint8_t *out, int cap);

// ---- the bot cycle (docs/C_GAME_SHAPE_MIGRATION.md 2.7) ------------------------
//
// One cycle as a server runs it, after table_load: the deal seed, the row's
// session log, then the drive. The cycle is ONE table operation, so
// table_commit_products and table_push after it carry exactly what the cycle
// wrote: its records (never the session beneath them), its events, the acting
// seat, and the finalize when it ended the game.

// The game's deal seed (games.game_seed) as its hex text: the secret base every
// mid-game draw and every bot decision is seeded from (game.h game_state_seed).
// FNV-1a 32 over the characters, 0 for none. Call after table_load.
int table_set_deal_seed(Table *t, const char *seed_hex, int len);

// Hands the table the row's session log (games.logs_packed: per record a u48 LE
// ms timestamp, then the log_record_put layout). A bot cycle ALWAYS calls it: the
// log's length is the game's progress, which seeds every bot decision
// (bot_drive.h bot_drive_seed_decision), so a table that returns to an exact
// earlier board still draws a fresh number.
//
// The RECORDS are read onto the board only for a brain that reads them
// (table_bots_need_logs) - otherwise a long game would spend its MAX_LOGS
// capacity on a session nothing consults and drop its own new records. A record
// with more pairs than MAX_LOG_PAIRS keeps its first MAX_LOG_PAIRS; records past
// MAX_LOGS are dropped; a truncated tail ends the log. Returns the records the
// log holds (whether or not they were read onto the board), or TABLE_E_WIRE for
// an unknown record type.
int table_set_session_log(Table *t, const uint8_t *log, int len);

// Drives the bot seats for one cycle (bot_drive.h), seeded per decision from the
// table's deal seed, with every seat the roster names human left to its player.
// `prefs` is a blob table_drive_prefs wrote for an earlier attempt of the same
// cycle that lost its commit (len 0 for none): moves reused while still legal.
// Returns the actions applied (0 means nothing to commit), TABLE_E_WIRE for a
// malformed prefs blob, TABLE_E_NOT_LOADED.
int table_bot_drive(Table *t, const uint8_t *prefs, int prefs_len, int max_actions, BotDriveOut *out);

// The preferred moves to offer a retry of this cycle: the ones the drive was
// offered, overlaid with what it applied (one per seat, the latest winning).
// Returns the blob's length, or TABLE_E_CAP.
int table_drive_prefs(const Table *t, const BotDriveOut *drv, uint8_t *out, int cap);

// Called with the board and the seat just before each bot decision a
// table_bot_drive searches for (a reused preferred move searches nothing). NULL by
// default; bots.wasm points it at its belief probe (wasm_bots_api.c).
extern void (*table_choose_observer)(const Game *g, int seat);

// How long the host waits after the cycle `drv` describes (bot_cycle_delay_ms).
int table_cycle_delay_ms(const Table *t, const BotDriveOut *drv);

// ---- the end of a game -------------------------------------------------------

// The finished table's v6 replay code, from its 32-byte deal seed and its
// session log (the layout table_set_session_log reads, which it counts), and
// then THE ROUND-TRIP GATE: the code is decoded again (into `scratch`) and every
// ATTACK, COVER, PASS and PICKUP of the log's last GAME_START session must come
// back with the same seat and card pairs. Returns the code's length, a negative
// REPLAY_E* from the codec, TABLE_E_WIRE for a log the import refuses, or
// TABLE_E_REPLAY_VERIFY when the gate fails. Only a code this returns may retire
// the log.
int table_replay_code(Table *t, const uint8_t *seed, int seed_len, const uint8_t *log, int log_len,
                      uint8_t *out, int cap, uint8_t *scratch, int scratch_cap);

// The replay extras blob (replay_extras.h) for the loaded roster's names in seat
// order and the move times of the session log: its last GAME_START session's
// GAME_START and ATTACK/COVER/PASS/PICKUP records, each time the record's
// milliseconds / 1000 as a double, the first the start and the rest as gaps
// (never negative). A log with none of those records carries no times. Returns
// the blob's length, TABLE_E_WIRE for a malformed log, or TABLE_E_CAP.
int table_replay_extras(const Table *t, const uint8_t *log, int log_len, uint8_t *out, int cap);

// The finish order, best first: the eliminated seats in the order they went
// out, then the fool (anim_plan.c anim_finish_rows). Returns the count.
int table_rankings(const Table *t, int8_t seats_out[MAX_PLAYERS]);

// Each seat's rating change: the sum over every other seat of the 1v1 change
// (K = 10, rounded like Math.round), scoring 1 against a seat that finished
// below and 0 against one above. `ratings` and `out` by seat, `order` best
// first. Integer ratings only, which every rating in the product is.
typedef struct {
    int32_t ratings[MAX_PLAYERS];
    int8_t  order[MAX_PLAYERS];
    int32_t deltas[MAX_PLAYERS];
} TableElo;
int elo_deltas(const int32_t *ratings, const int8_t *order, int n, int32_t *out);

#endif

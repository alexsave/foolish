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
#include "roster.h"
#include "evwire.h"
#include <stddef.h>

// ---- results ----------------------------------------------------------------
//
// Non-negative is an outcome, negative a refusal. A refused load returns the
// state's GAME_INVALID_* (-1..-12) unchanged, or one of the TABLE_E_* below.
// Append-only numbering: hosts log and map these numbers.
#define TABLE_OK              0
#define TABLE_APPLIED         0
#define TABLE_REJECTED        1   // the kernel refused the move; Table.reject holds the ENGINE_REJECT_*
#define TABLE_MOOT            2   // the game is already over, so the move is a no-op
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

// The response code for a stale-round refusal. It sits above the kernel's
// ENGINE_REJECT_* space so a client can tell a rules rejection from a server
// policy one by the number alone (sdk/ts/wire/awire.ts REJECT_STALE_ROUND).
#define TABLE_REJECT_STALE_ROUND 100

// The durable state blob: [STATE_FORMAT_VERSION][deterministic deck][state_put].
#define TABLE_STATE_FORMAT 2

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
    int8_t      actor;          // the acting seat, -1 for none
    int32_t     log_start;      // the operation's first log record
} Table;

// Points a table at its storage. Nothing is loaded.
void table_init(Table *t, Game *g, TableSnaps *snaps);

// Loads a row. The roster is decoded, every bot seat's brain must be one this
// build links, the seat counts must agree, and the state goes through
// state_import (game_validate), so a WAITING row that holds cards is refused.
// Each seat's kind (Player.strategy_key) is set from the roster, and the
// deterministic-deck flag from the blob. On any refusal nothing is loaded.
int table_load(Table *t, const uint8_t *state, int state_len, const uint8_t *roster, int roster_len);

// A move by the seat whose roster id is EXACTLY actor_id. In order: a finished
// game is TABLE_MOOT; a move composed before the current round (intent_version
// >= 0 and below round_epoch) is TABLE_STALE_ROUND; an actor with no seat is
// TABLE_E_NOT_SEATED; a malformed wire is TABLE_E_WIRE; a move the rules refuse
// is TABLE_REJECTED. An applied move that ends the game finalizes it (GAME_OVER,
// bots parked READY and humans IDLE).
int table_act(Table *t, const char *actor_id, int id_len, const uint8_t *awire, int wire_len,
              int64_t intent_version, int64_t round_epoch);

// PLAYING, and a bot seat is still IN: the row's needs_bots column.
bool table_needs_bots(const Table *t);

// A bot that could act now uses the session log (a belief brain), so the host
// must hydrate it before driving.
bool table_bots_need_logs(const Table *t);

// ---- products ----------------------------------------------------------------

typedef struct { int32_t off, len; } Span;

typedef struct {
    int8_t  status;        // GAME_STATUS_*: the row's status column (the blob is authoritative)
    int8_t  fool;          // the fool's seat once the game is over, else -1
    int8_t  num_players;
    uint8_t n_events;      // events each push carries; 0 means there is nothing to broadcast
    bool    needs_bots;
    bool    closed_round;  // the operation's records hold a PICKUP or DISCARD (and it dealt nothing)
    bool    logs_reset;    // the operation dealt: its records start a new session log
    bool    ended;
    bool    dealt_now;
    bool    roster_changed;
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

// One viewer's response envelope (seat, or -1 for the spectator):
//   u8 fmt=1, u8 flags (bit0 seated viewer, bit1 roster trailer), u8 seat (0xFF),
//   u32 version, u16 0, u16 view_len, [VIEW_FORMAT_VERSION][viewer][masked state],
//   the roster trailer (roster.h)
int table_envelope(const Table *t, const char *game_id, int gid_len, int viewer, uint32_t version,
                   uint8_t *out, int cap);

// One viewer's realtime payload for the last operation (evwire.h as3): the
// event sequence masked for `viewer` (seat, or -1), the flags byte, and the new
// roster when the operation changed it. A bot seat has no viewer.
int table_push(const Table *t, const char *game_id, int gid_len, int viewer, uint8_t *out, int cap);

// ---- the end of a game -------------------------------------------------------

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

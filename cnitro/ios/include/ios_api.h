// ios_api.h — the Swift-visible C API for the native iOS app (FoolishKit/Engine).
//
// This header IS the bridge contract. Per docs/IOS_APP_DESIGN.md §16.0 ("the
// JSON bridge rule"): Swift never parses the kernel's packed binary formats.
// Every piece of state Swift needs is emitted as JSON into a caller-provided
// buffer and decoded in Swift with Codable. Binary crosses the boundary in
// exactly two places (both outside this header): golden-vector fixtures
// (compared as opaque bytes) and the packed action encoder (Net/PackedAction,
// where byte-exactness against the TS implementation is the whole point).
//
// The one hard rule (§3): no Durak rule is ever reimplemented in Swift. Whose
// turn, legal moves, capacity checks, refills — every rules question is
// answered here, through the C kernel (game.c / legal.c / view.c / replay.c).
// Swift renders state and forwards intents.
//
// Threading: there is ONE static Game in ios_api.c and it is NOT reentrant.
// The Swift wrapper (EngineC) serializes every call onto a single private
// dispatch queue; never call these from two threads at once.
//
// Return-value convention: functions returning `int` that fill `out` return
// the number of bytes written to `out` (>= 0, NOT counting a NUL terminator —
// the buffer is always NUL-terminated when there is room) on success, or a
// NEGATIVE error code (see FIO_E* below) on failure. Functions that don't
// fill a buffer return 0 on success or a negative error code.

#ifndef FOOLISH_IOS_API_H
#define FOOLISH_IOS_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------- error codes ----------------------------------------------------
#define FIO_EOK          0
#define FIO_EBADARG     -1   // null pointer / bad player count / bad seat
#define FIO_ENOGAME     -2   // no game created yet
#define FIO_ECAP        -3   // output buffer too small
#define FIO_EPARSE      -4   // could not parse the input JSON move
#define FIO_EREJECT     -5   // move was rejected by the kernel (see fio_last_reject)
#define FIO_ENOSTRAT    -6   // unknown strategy id
#define FIO_EREPLAY     -7   // replay encode/decode failed (see fio_last_replay_error)
#define FIO_ENOSEED     -8   // this game was not dealt from a wide seed, so its
                             // deal cannot be re-derived (v6 needs it; use v5)

// ---------- lifecycle ------------------------------------------------------

// Deal a fresh game from `seed`. When seed_len >= 32 the deal uses the wide
// (ChaCha) deal RNG so the whole 52!/36! space is reachable and the deal is
// reproducible on any platform (deal_rng.h). Fewer than 32 bytes falls back to
// the legacy 32-bit LCG seed (first 4 bytes, little-endian) — used only by the
// golden fixtures that pin the legacy stream. All seats start as human
// (strategy 0); assign bots afterwards with fio_set_seat_strategy.
// n_players must be 2..8. Returns FIO_EOK or a negative error.
int fio_new_game(const uint8_t *seed, int seed_len, int n_players);

// Assign a strategy to a seat (offline bots). strategy_id is a FIO strategy id
// (0..fio_strategy_count()-1, see fio_strategy_name). Seat 0 is conventionally
// the local human but nothing enforces that. Safe to call any time before the
// seat is asked to choose. Returns FIO_EOK or a negative error.
int fio_set_seat_strategy(int seat, int strategy_id);

// True once fio_new_game has succeeded.
int fio_has_game(void);

// ---------- observation (all emit JSON; see FoolishKit/Engine/Models.swift) --

// Per-viewer masked state: viewer_seat's own hand is real cards, every other
// hand is redacted to a count only, the deck is hidden. This is the "you only
// see your own hand" rule, computed in the kernel (view.c semantics).
int fio_state_json(int viewer_seat, char *out, int cap);

// Spectator view: every hand redacted to counts, deck hidden.
int fio_public_state_json(char *out, int cap);

// Decode a SERVER packed-view blob (the player_views / spectator_views wire —
// the same state_put/state_get layout the kernel single-sources) into the app's
// GameView JSON, so online play (§8) renders through the SAME kernel decode as
// offline. `viewer` is the local player's seat (whose hand is real in the blob),
// or -1 (VIEW_SPECTATOR) for the public feed. Does not touch the current game.
int fio_view_from_packed_json(const uint8_t *buf, int len, int viewer, char *out, int cap);

// Legal moves for `seat` from a server packed masked-view blob — online
// enable-states, kernel-computed (§3). `seat` must be the local player's seat.
int fio_legal_from_packed_json(const uint8_t *buf, int len, int seat, char *out, int cap);

// Legal moves available to `seat` right now, as a JSON array. Empty array []
// when the seat has no pending action.
int fio_legal_moves_json(int seat, char *out, int cap);

// Bitmask over seats (bit i => seat i has a pending legal action right now).
// Mirrors should_bot_act across all seats — the single source of "whose turn".
int fio_actor_mask(void);

// Loser (fool) seat when the game is over, or -1 while it is still running.
int fio_game_over(void);

// ---------- intents --------------------------------------------------------

// Apply one move, given as a JSON object (the shape a single legal-move entry
// has: {"type":"attack","cards":[{"s":0,"v":6}]}; cover adds "attackCards").
// `actor_seat` is the seat performing it. Returns FIO_EOK on success,
// FIO_EPARSE if the JSON is malformed, or FIO_EREJECT if the kernel rejected
// the move (fio_last_reject() then carries the ENGINE_REJECT_* code).
int fio_apply_json(int actor_seat, const char *move_json);

// Kernel rejection reason from the last fio_apply_json that returned
// FIO_EREJECT (an ENGINE_REJECT_* value from game.h), else 0.
int fio_last_reject(void);

// The animation events of the LAST fio_apply_json / fio_bot_drive_json, as seen
// by `viewer` (a seat, or -1 for a spectator), e.g.
//   [{"type":5,"seat":3,"msg":4,"from":1,"to":2,"cards":[...],
//     "target":{...},"battle":0}]
// type/msg/from/to are EVW_* codes (cnitro/src/evwire.h) — the SAME event
// stream the website plays, derived once in the kernel (evwire_walk). This is
// why `BoardDiff.swift` is cancelled: a client never works out which card flew
// where (docs/C_CORE_CONSOLIDATION.md F4). `cards` entries are null where the
// kernel redacted them (a card dealt/drawn into someone else's hand).
// Returns bytes written, or a negative error. fio_bot_drive_json already
// includes its cycle's events inline; this is the apply-path companion.
int fio_last_events_json(int viewer, char *out, int cap);

// Drive ONE eligible bot seat (any seat with a pending action other than
// human_seat): choose its move with that seat's assigned strategy, apply it,
// and emit the applied move as JSON: {"seat":i,"type":...,"cards":[...]}.
// Returns bytes written (> 0) when a bot acted, 0 when no bot seat is eligible
// (it is the human's turn, or the game is over), or a negative error.
// Pass human_seat = -1 to let every seat be driven (used by replays/spectate).
//
// DEPRECATED for the app's bot loop — it walks seats and drives the FIRST
// eligible one, which hands low seats a tempo advantage the website does not
// have, and it cannot bundle silent actions. Use fio_bot_drive_json.
// Kept for tests and single-step tooling.
int fio_bot_step_json(int human_seat, char *out, int cap);

// Drive the bot cycle (docs/C_CORE_CONSOLIDATION.md F2/F3): apply 0..n bot
// actions and stop on the same conditions as the website's loop — game over, a
// visible action landed, or no bot can act. Silent actions (a `good` that does
// not end the bout) bundle into one cycle instead of costing a delay each.
// `human_mask` is a bitmask of seats the kernel must NOT drive; a human being
// able to act is NOT a stop condition (bots throw in while you deliberate,
// exactly as they do online).
//
// Emits:
//   {"actions":[{"seat":i,"pace":c,"type":...,"cards":[...]}...],
//    "stop":s,"ended":foolSeatOr-1,"delayMs":ms}
//
// `delayMs` is how long the host should wait before the next cycle — the one
// pacing table (bot_pacing_ms), not a Swift constant. Returns bytes written
// (always > 0; "actions" may be empty), or a negative error.
int fio_bot_drive_json(int human_mask, char *out, int cap);

// ---------- strategies (offline bot roster, §7.2) --------------------------

// Number of exposed offline strategies.
int fio_strategy_count(void);
// Name of strategy `id` (e.g. "espresso"), written to `out`. Bytes written or negative.
int fio_strategy_name(int id, char *out, int cap);
// Choose (but do NOT apply) a move for `seat` using `strategy_id`, emitted as
// a JSON move object. Used by tests and by any caller wanting to preview.
int fio_bot_choose_json(int strategy_id, int seat, char *out, int cap);

// ---------- replays (§7.3) -------------------------------------------------

// Encode the CURRENT game's history into a replay integer, base32-ish encoded
// into `out` as the short shareable code (foolish.cards/<code>). Bytes written
// or negative. (v5 codec; byte-parity with the server's shared C — replay.c.)
int fio_replay_encode_b32(char *out, int cap);

// Same, as a v6 code: the exact game including every hidden card, where v5's
// decoder has to retrodict the hands. Prefer this — it is what the site
// produces, and the Oracle reads real hands instead of guesses.
//
// Takes no seed: the kernel kept the one fio_new_game was given. Returns
// FIO_ENOSEED if this game was dealt without a wide seed (its deal cannot be
// re-derived) — fall back to fio_replay_encode_b32 then, and only then.
int fio_replay_encode_v6_b32(char *out, int cap);

// The one a CLIENT should call: the best code this game can produce (v6 when its
// deal is re-derivable, else v5), so choosing a replay format never becomes app
// code. The two calls above are for tests that must pin a format.
int fio_replay_share_code_b32(char *out, int cap);

// Decode a shareable `code` into the step list as JSON (the DecodedReplay
// shape: header + logs). Does NOT touch the current game. Bytes written or
// negative; on FIO_EREPLAY see fio_last_replay_error().
int fio_replay_decode_json(const char *code, char *out, int cap);

// Play a v6 `code` back and return the animation events, masked for `viewer`
// (a seat, or -1 to spectate) — the SAME GameEvent stream live play emits
// (fio_bot_drive_json / fio_last_events_json), each event carrying its step's
// board in `state`. A shared code therefore renders on the real table with the
// real animations and no replay-specific code (docs/C_CORE_CONSOLIDATION.md
// A5): the kernel rebuilds the game and replays it through the engine.
//
// Does NOT touch the current game. Bytes written or negative; v5 codes fail
// with FIO_EREPLAY / REPLAY_EVERSION — they hide the deal, so there is no game
// to rebuild (use fio_replay_decode_json for those).
int fio_replay_events_json(const char *code, int viewer, char *out, int cap);

// Detail of the last replay error (a REPLAY_E* code from replay.h), else 0.
int fio_last_replay_error(void);

#ifdef __cplusplus
}
#endif

#endif // FOOLISH_IOS_API_H

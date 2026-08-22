// ios_api.h — the Swift-visible C API for the native iOS app (sdk/swift).
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

// Re-deal the RESIDENT game's own locked deal seed at a DIFFERENT player
// count — the iMessage lobby's "Start" action (docs/IMESSAGE_LOBBY_V2.md): a
// group lobby is created OPEN (fio_new_game with the wire's max capacity, 8)
// so seats stay free to fill; this re-derives the SAME seed's deal at the
// actual joined count once the group decides to start (never a new random
// seed — that is the "locked at create" guarantee). Requires a wide (32-byte)
// seed to already be resident (a prior fio_new_game, or an
// fio_msg_decode_packed of an envelope that carried one) — FIO_ENOSEED
// otherwise. n_players must be 2..8 (FIO_EBADARG, see fio_new_game). The seed
// itself never crosses back into Swift; this is the same "the kernel keeps
// the seed" discipline fio_replay_encode_v6_b32 already relies on.
int fio_reseat_game(int n_players);

// Assign a strategy to a seat (offline bots). strategy_id is a FIO strategy id
// (0..fio_strategy_count()-1, see fio_strategy_name). Seat 0 is conventionally
// the local human but nothing enforces that. Safe to call any time before the
// seat is asked to choose. Returns FIO_EOK or a negative error.
int fio_set_seat_strategy(int seat, int strategy_id);

// True once fio_new_game has succeeded.
int fio_has_game(void);

// ---------- observation ----------------------------------------------------

// (Server packed-view blobs decode to a GameView in pure Swift via MaskedView,
// and their legal moves come through the PACKED fio_legal_from_packed — so the
// JSON packed-view bridges and the unused spectator-JSON reader are gone with
// the JSON surface.)

// Legal moves available to `seat` right now, as a JSON array. Empty array []
// when the seat has no pending action.
int fio_legal_moves_json(int seat, char *out, int cap);

// PACKED (no-JSON) twins — the kernel wire Swift decodes directly (MaskedView /
// MoveWire). fio_state_packed: the resident masked view (view.c state_put).
// fio_legal_packed / fio_legal_from_packed: legal moves (wasm_export_moves
// layout) from the resident game or a server packed view. Owner: wipe the JSON.
int fio_state_packed(int viewer, char *out, int cap);
int fio_legal_packed(int seat, char *out, int cap);
int fio_legal_from_packed(const uint8_t *buf, int len, int seat, char *out, int cap);
// Apply an awire action frame ([kind, n, cards, attacks]) — THE apply entry
// (a plain move never crosses as JSON). Returns FIO_EREJECT on an illegal move
// (see fio_last_reject).
int fio_apply_awire(int actor_seat, const uint8_t *buf, int len);

// Drive one bot cycle, result packed (no JSON, no events): u32 n_actions, per
// action {seat, pace, type, n_cards, cards[], attacks[]}, then i32 stop, ended,
// delayMs (LE). The BotDriveWire Swift decoder reads it.
int fio_bot_drive_packed(int human_mask, char *out, int cap);

// Bitmask over seats (bit i => seat i has a pending legal action right now).
// Mirrors should_bot_act across all seats — the single source of "whose turn".
int fio_actor_mask(void);

// Loser (fool) seat when the game is over, or -1 while it is still running.
int fio_game_over(void);

// ---------- intents --------------------------------------------------------

// (A move applies through fio_apply_awire, above — the JSON apply entry is gone.
// The FMSG rebase path still parses a move from JSON, internally, via
// fio_move_to_awire; see fio_msg_rebase.)

// Kernel rejection reason from the last fio_apply_awire that returned
// FIO_EREJECT (an ENGINE_REJECT_* value from game.h), else 0.
int fio_last_reject(void);

// The animation events of the LAST fio_apply_awire / fio_bot_drive_json, as seen
// by `viewer` (a seat, or -1 for a spectator), e.g.
//   [{"type":5,"seat":3,"msg":4,"from":1,"to":2,"cards":[...],
//     "target":{...},"battle":0}]
// type/msg/from/to are EVW_* codes (c/src/evwire.h) — the SAME event
// stream the website plays, derived once in the kernel (evwire_walk). This is
// why `BoardDiff.swift` is cancelled: a client never works out which card flew
// where (docs/C_CORE_CONSOLIDATION.md F4). `cards` entries are null where the
// kernel redacted them (a card dealt/drawn into someone else's hand).
// Returns bytes written, or a negative error. fio_bot_drive_json already
// includes its cycle's events inline; this is the apply-path companion.
//
// TODO(delete): JSON. We don't want JSON crossing this boundary — clients drive
// animations off the PACKED replay/event stream (fio_replay_decode_packed ->
// DecodedReplay.logs, the LOG_* steps). Delete this once the last JSON consumer
// is off it.
int fio_last_events_json(int viewer, char *out, int cap);

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

// ---------- animation core (c/src/anim_plan.h) -----------------------------
//
// The platform-independent animation POLICY, shared with the web (which reaches
// the same C through wasm) and any future client. THE point: MessageTableView's
// runEventStream + preCounts + veil each re-derive this choreography today; this
// hands them the finished plan so Swift only tweens sprites per step and obeys
// the freezes/veils. See docs/ANIMATION_CORE_C.md.
//
// The animation plan for the LAST fio_apply_awire / fio_bot_drive_json, as seen
// by `viewer` (a seat, or -1 spectator). One ordered plan of steps, each with a
// duration and the board counts the display advances to as its flight lands,
// PLUS the pre-sequence count-freeze (the iOS preCounts backward walk, in C now)
// and the veil (the card identities to hide until their step lands, the C twin
// of preHide). Shape:
//   {"durationMs":500,"gapMs":25,"totalMs":1550,"nPlayers":2,
//    "pre":{"deck":24,"discard":4,"hand":[4,4]},
//    "veil":[4,32],                       // dense card ids (suit*13+value-1)
//    "steps":[{"type":7,"seat":-1,"from":2,"to":3,"nCards":4,
//              "durationMs":500,"startMs":0,"deck":24,"discard":8,
//              "inFlightFromDeck":0,"inFlightToFlipped":0,"hand":[4,4]}, ...]}
// type/from/to are EVW_*/ANIM_* codes (identical numbering; c/src/anim_plan.h).
// Returns bytes written, or a negative error.
int fio_anim_plan_json(int viewer, char *out, int cap);

// The live-broadcast version gate (anim_should_drop_stale): should a broadcast
// at `incoming` be dropped as stale given the newest applied `last`? The has_*
// flags model a missing version (a replay sequence, never gated): pass 0 for
// "none". Returns 1 (drop) or 0 (apply). Provided for a future iOS/Steam optimism
// layer; the web already routes its feed gate through the same C.
int fio_anim_should_drop_stale(int has_last, int last, int has_incoming, int incoming);

// ---------- strategies (offline bot roster, §7.2) --------------------------

// Number of exposed offline strategies.
int fio_strategy_count(void);
// Name of strategy `id` (e.g. "espresso"), written to `out`. Bytes written or negative.
int fio_strategy_name(int id, char *out, int cap);

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

// Decode a shareable `code` into the step list as the RAW replay.h DECODE binary
// (20-byte header + n_logs records; Swift parses it with DecodedReplay.decode) —
// no JSON crosses the boundary. Does NOT touch the current game. Bytes written or
// negative; on FIO_EREPLAY see fio_last_replay_error().
int fio_replay_decode_packed(const char *code, unsigned char *out, int cap);

// Play a v6 `code` back and return the animation events, masked for `viewer`
// (a seat, or -1 to spectate) — the SAME GameEvent stream live play emits
// (fio_bot_drive_json / fio_last_events_json), each event carrying its step's
// board in `state`. A shared code therefore renders on the real table with the
// real animations and no replay-specific code (docs/C_CORE_CONSOLIDATION.md
// A5): the kernel rebuilds the game and replays it through the engine.
//
// Does NOT touch the current game. Bytes written or negative; v5 codes fail
// with FIO_EREPLAY / REPLAY_EVERSION — they hide the deal, so there is no game
// to rebuild (use fio_replay_decode_packed for those).
//
// TODO(delete): JSON. Clients drive animations off the PACKED replay stream
// (fio_replay_decode_packed -> DecodedReplay.logs, the LOG_* steps), never JSON.
// Delete once the last JSON consumer is off it.
int fio_replay_events_json(const char *code, int viewer, char *out, int cap);

// The animations of the chain's LAST BUBBLE, as PACKED evwire frames (each
// preceded by a u16 LE length, in play order), masked for `viewer` - what an
// iMessage receiver sees on opening a bubble. THE KERNEL decides the group from
// `atoms_before`: how many atoms sat on the chain BEFORE this bubble, so the
// group is every step after them. A receiver reads it off the envelope (`turn`
// minus the round-16 bubble delta, msg_wire.h's n_new); a sender animating its
// own move passes the turn of the chain it adopted, which is the same boundary
// from the other side and needs no count of what its moves became. A player may
// stage several actions before sending, so a double cover replays BOTH - and a
// cover SENT SEPARATELY from the next one does not. Pass -1 for a chain that
// cannot say (format 2, sealed before round 16) and the kernel falls back to
// its old guess, the trailing run of steps by one acting seat. Never "where I
// last looked": a device's cache is not a property of the bubble, so a wipe or
// a reinstall must not change what animates. The viewer's own
// drawn/picked-up cards carry real
// identities, everyone else's are hidden - the same packed evwire live play
// broadcasts and the website renders, so a reopen animates through the kernel,
// not a client-side view diff. No JSON (§zero-JSON): Swift reads them with
// EvWire.decodeFrames. Bytes written (0 if the turn produced nothing), or
// negative - including when `cap` could not hold the whole turn, which is an
// error rather than a silently truncated animation. v6 only (v5 hides the
// deal); see fio_last_replay_error.
int fio_replay_last_events_packed(const char *code, int viewer, int atoms_before,
                                  unsigned char *out, int cap);

// Detail of the last replay error (a REPLAY_E* code from replay.h), else 0.
int fio_last_replay_error(void);

// ---------- FMSG: the iMessage envelope (src/msg_wire.h) -------------------
//
// An iMessage game has no server. The whole game is one MSMessage URL —
// (32-byte deal seed, v6 replay code) — and every device rebuilds it by
// re-dealing from the seed and replaying the code through this kernel. The
// extension ships inside this app and uses these five calls for all of it.
//
// NOTE WHAT IS NOT HERE: no Rule P, no rebase guard, no "is this move legal
// now" for Swift to answer. Those are in C (msg_wire.c) and exposed below.
// Rule P decides which game every player SEES — a phone disagreeing with a
// browser forks the game — and the rebase guard is a rules question, which
// §17.16 forbids Swift from answering. The M3 plan's Swift port of the
// concurrency model is cancelled: there is nothing to port, and so nothing to
// keep in step. Swift moves bytes and renders.
//
// Errors are the FIO_E* above, plus FIO_EMSG for a payload the envelope layer
// rejected; fio_last_msg_error() carries the MSG_E* that says why.

#define FIO_EMSG        -9   // the FMSG payload was rejected (see fio_last_msg_error)

// Decode + VALIDATE a payload, and ADOPT it: the chain is replayed through the
// kernel into the resident game, so every other call in this header then reads
// the game the payload describes. A corrupt or hand-edited payload fails here,
// loudly — validation IS replay, and there is no partial recovery (§7.3).
//
// The envelope metadata is handed back as a PACKED fixed-layout blob (Swift
// parses it with MessageEnvelope.decode) — no JSON, no embedded state / moves
// (read those via fio_state_packed / fio_legal_packed). Layout:
//   phase(1) n_players(1) last_actor_seat(1) round(1) turn(u16 LE) game_id(u64 LE)
//   parent8(8) digest(32) sent_at(u16 LE) n_joins(1)
//   then n_joins*{seat(1) name_len(1) name[]}.
// ROUND 16: sent_at is the envelope's send clock (unix seconds mod 65536); 0
// when the chain is format 2 and carries none, which means no pickup hold.
// Bytes written or negative (FIO_EMSG → fio_last_msg_error).
int fio_msg_decode_packed(const uint8_t *payload, int len, unsigned char *out, int cap);

// 1.0(6) DIAGNOSTIC: replay codec version (5/6/7) of the body the last
// fio_msg_decode_packed replayed, or -1 for an empty-body message.
int fio_msg_last_body_version(void);

// Seal the RESIDENT game into a payload — the send path, after the local player
// has applied a move. The caller supplies what the PROTOCOL owns; the kernel
// fills in what the BODY owns (turn, round) by decoding the code it just wrote,
// so a device cannot emit a payload it would itself reject.
//
// `joins_json` is [{"seat":0,"name":"Sveta"},...]; names are <=64 UTF-8 bytes
// (round-5 B1, docs/APP_REVIEW_NOTES.md — was 12, too tight for a byte-counted
// Cyrillic name) and are the only identity a payload carries (no participant
// UUID ever goes in — they do not transfer across devices, §6).
//
// `sent_at` is the SEND CLOCK: this device's unix seconds mod 65536, which seals
// a format-3 envelope, or 0 to seal format 2 exactly as before. It is a
// parameter and not a time() call because the kernel must answer the same for
// the same bytes on every device (round 16; see msg_wire.h).
//
// Returns bytes written to `out`, or negative. FIO_ENOSEED if this game was not
// dealt from a wide seed (a serverless game cannot be rebuilt without one).
int fio_msg_encode(int phase, int last_actor_seat, uint64_t game_id,
                   const uint8_t parent8[8], const char *joins_json,
                   int sent_at, uint8_t *out, int cap);

// ROUND 16 — the pickup hold, asked of the RESIDENT game (the one the last
// fio_msg_decode_packed replayed). Seconds `seat` must still wait before it may
// pick up: 0 when it may pick up now. `sent_at` is the clock that came back in
// the packed blob, `now` the caller's own unix seconds mod 65536.
//
// The UI hides the Pickup button while this is non-zero AND the stage path
// refuses the move, so the rule lives in one place (msg_wire.c) and neither
// half can drift. It is deliberately NOT part of the legal-move menu: the v6
// body codes each action as an index into that menu, so a menu that changed
// with the clock would re-point every replay code ever written.
int fio_msg_pickup_hold(int seat, int sent_at, int now);

// ---------- Rule F: the fool's penalty -------------------------------------
//
// A rematch among the SAME players, in the same cycle, opens on the seat to the
// RIGHT of the last game's fool rather than on the lowest trump - the fool is
// the first player attacked. The rule and its guard live in msg_wire.c
// (msg_roster_key / msg_rematch_opening); these two entries are the phone's
// access to them, and neither lets Swift decide anything.

// CREATING the rematch lobby: turn the lobby's roster and the fool's seat WITHIN
// it into the carry a WAITING envelope hands forward. `*key_out` is the roster
// key (never 0) and `*fool_index_out` the fool's index in that key's canonical
// rotation. Seal them as the envelope's carry_key/carry_fool.
int fio_msg_carry(const char *joins_json, int fool_seat,
                  uint32_t *key_out, int *fool_index_out);

// Arm the resident game's carry, so the next fio_msg_encode seals a WAITING
// lobby that carries the question forward. Pass the pair fio_msg_carry
// produced; pass key 0 (or a negative index) to disarm. A fresh fio_new_game
// disarms it too, so an ordinary lobby never carries one by accident.
int fio_msg_set_carry(uint32_t key, int fool_index);

// SHOWING it: the seat a lobby's pending penalty would fall on - the fool, who
// becomes the first defender - or -1 if the rule would not apply to this
// roster. Read-only; it deals nothing and changes nothing, so a lobby can call
// it on every render.
int fio_msg_penalty_fool_seat(const char *joins_json, uint32_t carry_key, int carry_fool);

// STARTING it: deal the resident locked seed at the joins' count, applying the
// penalty if - and only if - the roster still keys equal to the carry. Replaces
// fio_reseat_game on the rematch path and does everything it does.
//
// `*opening_out` receives the seat the game opens on, or -1 when the rule did
// not apply (anyone joined, left or was renamed) and the deal derived its
// opener normally. Either way the resident game is dealt and every later
// fio_msg_encode repeats the term, so the caller does not carry it.
int fio_msg_start_rematch(const char *joins_json, uint32_t carry_key,
                          int carry_fool, int *opening_out);
// This one entry seals every phase. A 0-action game — a WAITING lobby (§5.2) or
// the last-joiner LIVE handoff that "applies nothing" — seals to an empty body:
// msg_seal detects "no opening attack logged" and emits no v6 body, since the v6
// producer is an action-run codec and the deal alone is the state.

// Rule P (§7.2): which of two payloads does EVERY device prefer?
// <0 `a`, >0 `b`, 0 the same chain. Structure only — no replay, and no clocks:
// delivery order is never an input, because two devices can transiently
// disagree about which message is newest.
// Returns FIO_EMSG if either is not an envelope.
int fio_msg_rule_p(const uint8_t *a, int a_len, const uint8_t *b, int b_len);

// Rule R (§7.4): rebase ONE pending move onto the chain fio_msg_decode_packed
// last adopted — the ledger's moves, in order. Returns:
//   0  re-applied, and APPLIED to the resident game (that IS the rebase)
//   1  discarded by the round-boundary guard
//   2  discarded: the kernel refuses it on the new state
// or negative on a bad argument.
//
// The guard is the point: a throw-in composed against round 5's table would,
// after a pickup closed round 5, re-validate as an OPENING ATTACK of round 6 —
// legal, and not what the player chose.
#define FIO_REBASE_REAPPLY         0
#define FIO_REBASE_DISCARD_ROUND   1
#define FIO_REBASE_DISCARD_ILLEGAL 2
int fio_msg_rebase(int pending_round, int seat, const char *move_json);

// Rule R over the AWIRE frame (kind,n,cards[,attacks]) — the JSON-free entry the
// Swift extension uses (its pending ledger holds moves as awire, never JSON).
// Identical verdicts to fio_msg_rebase; `buf`/`len` is the same action frame
// fio_apply_awire takes.
int fio_msg_rebase_awire(int pending_round, int seat, const uint8_t *buf, int len);

// The MSG_E* (src/msg_wire.h) behind the last FIO_EMSG, else 0.
int fio_last_msg_error(void);

#ifdef __cplusplus
}
#endif

#endif // FOOLISH_IOS_API_H

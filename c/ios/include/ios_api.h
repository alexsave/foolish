// ios_api.h — the Swift-visible C API for the native iOS app (sdk/swift).
//
// This header IS the bridge contract.
//
// CORRECTED, 2026-09-05. It used to say, per docs/IOS_APP_DESIGN.md §16.0 "the
// JSON bridge rule", that Swift never parses the kernel's packed binary formats
// and that every piece of state crosses as JSON. Task #17 deliberately made that
// false: production Swift decodes the PACKED wire for the masked view, the legal
// menu, the replay and the message envelope (sdk/swift/MaskedView.swift,
// DecodedReplay.swift, EvWire.swift, MessageEnvelope.swift), and
// fio_state_json / fio_apply_json / fio_msg_decode_json were deleted outright.
//
// The rule that actually survived is narrower, and it is the one that matters:
// NO DURAK RULE IS REIMPLEMENTED IN SWIFT. Swift may read a layout the kernel
// wrote; it may not decide anything the kernel could decide. A new entry point
// here should emit PACKED bytes into a caller-provided buffer by default - JSON
// only where a human reads the output.
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

// THE TABLE'S RULES: `passing` = 1 for perevodnoy (the defender may transfer -
// the default, and what every game before this variant played) and 0 for
// podkidnoy (throw-in, no transfer at all).
//
// Chosen in the iMessage LOBBY and nowhere else, which is why this is a setter
// on the resident game rather than an argument to fio_new_game: a lobby is
// created before anyone has decided anything, and the checkbox that changes it
// re-seals a chain that already exists. Call it after adopting the lobby being
// changed (fio_msg_decode_packed) and before sealing; the seal states it on the
// wire, and the Start that re-deals the locked seed carries it across.
//
// It changes what is LEGAL - a podkidnoy defender's menu has no transfer in it
// (fio_legal_packed) and fio_apply_awire refuses one - so a host must not flip
// it mid-game: it is a term of the table, not a display option.
int fio_set_passing(int passing);

// The resident game's rules, as the same 1/0. 1 when nothing has said otherwise.
int fio_passing_allowed(void);

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

// ---------- what a gesture on a board means --------------------------------
//
// The rules a board applies between a finger and a move (legal.c's play_*):
// which menu entry a drop resolves to, which battles a selection could cover,
// which one the Cover button aims at, which moves a human may make at all.
//
// THEY READ NOTHING BUT THEIR ARGUMENTS - no resident game, no static - which
// is what lets a SwiftUI render pass call them at all: the resident game lives
// behind an actor a view body cannot await. It is also what a board actually
// wants asked. A board renders its own PUBLISHED pair, the menu it was handed
// and the table it was handed, and the iMessage board deliberately publishes an
// EMPTY menu while it holds a bout settlement back; a rule that re-derived the
// menu from the live game would answer about a position nobody is looking at.
//
// `menu` is the seat's packed menu (fio_legal_packed / fio_legal_from_packed
// bytes). `table` is 2 bytes per battle - the attack, then its cover or 0xFE.
// `sel` is the selected cards as card bytes. `target` is the battle index a
// gesture landed on, or FIO_PLAY_TARGET_TABLE / _HAND.
#define FIO_PLAY_TARGET_HAND   (-2)
#define FIO_PLAY_TARGET_TABLE  (-1)

// The fixed head of a probe answer, before the move wire (see below).
#define FIO_PLAY_PROBE_HEAD 10

// ONE ANSWER for a selection, so a board cannot paint a highlight that the
// release then refuses. Layout (LE):
//
//   0   u8    flags: 1 = an attack with this selection is legal,
//                    2 = a pass is, 4 = this seat may say good
//   1   i8    the battle the Cover button aims at, -1 for none
//   2   u64   bitmask of the battles this selection could cover
//   10  ...   the move the gesture resolves to, as a ONE-ENTRY menu wire
//             (count 0 when it resolves to nothing) - so it decodes through
//             MoveWire and no second format is born.
//
// "May say good" is the human rule the kernel menu deliberately does not carry:
// the menu always offers GOOD because that is how an attacker leaves the bot
// loop's eligible set (legal.c), while a player may not end a bout over an
// uncovered attack.
int fio_play_probe(const uint8_t *menu, int menu_len,
                   const uint8_t *table, int n_battles,
                   int power_suit, int is_defender,
                   const uint8_t *sel, int n_sel, int target,
                   char *out, int cap);

// The moves a HUMAN may make on this board, as the same menu wire back out:
// the kernel's menu minus `wait`, minus `good` while any attack is uncovered.
// The set form of the same rule, for the callers that ask "can this seat do
// anything at all" rather than "is this one button live" - a turn handoff that
// reads the raw menu passes the game to a seat whose only offer is a good the
// board will not let it make, and stops with no button on screen.
int fio_play_human_menu(const uint8_t *menu, int menu_len,
                        const uint8_t *table, int n_battles,
                        char *out, int cap);
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
// the same C through wasm) and any future client. THE point: MessageTableView
// re-derived this choreography in Swift; this hands it the finished plan so
// Swift only tweens sprites per step and obeys the freezes/veils. See
// docs/ANIMATION_CORE_C.md.
//
// THE STREAM IS AN INPUT, for the reason fio_beats_packed's is: a board
// animates the stream it was HANDED (often only half a bubble, because a staged
// bout end is cut at its settlement), and it asks from a SwiftUI render pass
// that cannot await the actor the resident game lives behind.
//
// EVERY EVENT CARRIES THE BOARD IT COMMITTED, and that is what the freeze is
// derived from - one undo off the FIRST event's own board, never a walk back
// from the final one. anim_plan.h says why (the flipped trump lies under the
// deck and is dealt without ever being counted).
//
// INPUT (`in`):
//   0  u8  version (FIO_PLAN_VERSION)
//   1  u8  n_players (2..8)
//   2  u8  n_events  (0..ANIM_MAX_STEPS, 128)
//   3  u8  final deck count
//   4  u8  final discard count
//   5  n_players x u8 final hand counts, by seat
//   then per event, 9 + n_players + n_ids bytes:
//     u8 type (EVW_T_*/ANIM_EVT_*), u8 seat (0xFF none), u8 from, u8 to,
//     u8 n_cards (the count the arithmetic reads), u8 n_ids (real identities
//     listed; 0 for viewer-masked backs), u8 has_counts, u8 deck, u8 discard,
//     n_players x u8 hand counts, n_ids x u8 dense card id.
//   A step with has_counts == 0 carries the walk forward instead of anchoring
//   it; a stream whose FIRST event has none falls back to undoing them all.
//
// OUTPUT (`out`):
//   0  u8  version
//   1  u8  n_steps
//   2  u8  n_players
//   3  u8  n_veil
//   4  u32 total wall time, ms (a full-length stream runs past a u16)
//   8  u8  pre deck   (the count-freeze the display opens on)
//   9  u8  pre discard
//  10  FIO_PLAN_SEATS x u8 pre hand counts, by seat
//   then n_steps x FIO_PLAN_STRIDE:
//     0  u8  type
//     1  u8  seat (0xFF none)
//     2  u8  from
//     3  u8  to
//     4  u8  n_cards
//     5  u16 duration ms
//     7  u32 start ms (offset from the sequence's first frame)
//    11  u8  deck as this step lands
//    12  u8  discard as this step lands
//    13  u8  cards of this step that left the deck
//    14  u8  ...of which are bound for the flipped slot (no badge change)
//    15  FIO_PLAN_SEATS x u8 hand counts as this step lands
//   then n_veil x u8 dense card id: identities in transit, hidden until the step
//   that lands them.
// Returns bytes written, or a negative error.
#define FIO_PLAN_VERSION 1
// The seat block is a FIXED width so a step sits at a constant offset whatever
// the table size; the kernel's MAX_PLAYERS is checked against it at build time.
#define FIO_PLAN_SEATS   8
#define FIO_PLAN_HEAD    (10 + FIO_PLAN_SEATS)
#define FIO_PLAN_STRIDE  (15 + FIO_PLAN_SEATS)
int fio_anim_plan_packed(const uint8_t *in, int len, char *out, int cap);

// The live-broadcast version gate (anim_should_drop_stale): should a broadcast
// at `incoming` be dropped as stale given the newest applied `last`? The has_*
// flags model a missing version (a replay sequence, never gated): pass 0 for
// "none". Returns 1 (drop) or 0 (apply). Provided for a future iOS/Steam optimism
// layer; the web already routes its feed gate through the same C.
int fio_anim_should_drop_stale(int has_last, int last, int has_incoming, int incoming);

// ---------- the shape of a sequence (anim_plan.h beats + role beat) ---------
//
// A board hands the kernel a stream and gets back its BEATS: what plays
// together, what waits, what each beat carries with it. See anim_plan.h for the
// rules; the reason they cross with the stream instead of being re-derived from
// the resident game is the reason fio_play_probe takes a menu. The board
// animates a stream it was HANDED - a bubble's events, and often only HALF of
// them, because a staged bout end is cut at its settlement and the second half
// is withheld until Send - and it asks from a SwiftUI render pass, which cannot
// await the actor the resident game lives behind.
//
// INPUT (`in`): u8 version (FIO_BEATS_VERSION), u8 n_events, then per event
//   u8 type (EVW_T_*/ANIM_EVT_*), u8 seat (0xFF none), u8 has_good_mask,
//   u8 good_mask (that step's own board), u8 n_ids, n_ids x u8 dense card id.
// Only REAL identities travel: a masked card back names nothing, so the caller
// simply does not list it.
//
// OUTPUT (`out`), all little-endian:
//   0   u8  version
//   1   u8  n_beats
//   2   u8  the first event's good mask is present
//   3   u8  the first event's good mask
//   4   u64 every card the whole stream puts down on the table, as id bits
//   12  n_beats x FIO_BEATS_STRIDE:
//        0 u8  index of the beat's first event
//        1 u8  how many events it spans
//        2 u8  the lead event's type
//        3 u8  the lead event's seat (0xFF none)
//        4 u8  flags: 1 holds after this beat, 2 it moved a card,
//                     4 it placed one on the table, 8 the acting badge drops
//                       as these cards LEAVE rather than as they land
//        5 u8  seats that go out WITH this beat
//        6 u8  seats that laid cards via ATTACK_PASS in it
//        7 u8  this beat's good mask is present
//        8 u8  this beat's good mask (its LAST event's board)
//        9 u64 the cards it puts on the table, as id bits
// Returns bytes written, or a negative error.
#define FIO_BEATS_VERSION 1
#define FIO_BEATS_HEAD    12
#define FIO_BEATS_STRIDE  17
int fio_beats_packed(const uint8_t *in, int len, char *out, int cap);

// Does a step of this kind take cards out of the acting seat's hand? (The
// flags-bit-8 question for a caller holding one event rather than a beat.)
int fio_badge_drops_as_cards_leave(int type);

// WHICH MARKS CHANGE, AND WHEN. `out` receives {defender, first_attacker,
// good_mask}; the return is 1 when the marks change and 0 when nothing does.
// `shown_*` is what the badges are WEARING, not the live board - a sequence
// freezes the marks and walks them forward - which is why it crosses as an
// argument. A good mask of -1 means "this step carried no board".
//
//   opening    the goods this stream ADDS, played in FRONT of the consequences
//              they caused (a good is a move; the discard behind it is not).
//   cleared    the goods a beat REMOVES, played in parallel with the throw-in
//              that cleared them.
//   hand_off   a PASS: the shield travels with the transfer card. Told from an
//              attack by the rules rather than the wire (they are one event
//              type): a defender may not attack, so a card laid by the seat
//              currently wearing the shield can only be a transfer. The new
//              defender is an argument because a pass is snapshotted BEFORE the
//              hand-over and emits no event for it - it appears only on the
//              bubble's final board.
#define FIO_ROLES_OUT 3
int fio_roles_goods_opening(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int first_good_mask, int *out);
int fio_roles_goods_cleared(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int step_good_mask, int *out);
int fio_roles_pass_hand_off(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int attack_pass_seats,
                            int final_defender, int *out);

// ---------- the pre-bout table (anim_plan.h) -------------------------------
//
// THE TABLE A BOUT END SWEEPS, so the board can lay it out and fly each card
// from where it actually sat. See anim_plan.h for the rule and for why the
// PAIRING is the hard part of it.
//
// The stream is an input for the reason fio_beats_packed's is, and the prior
// board travels with it because a single-action pickup turn carries no earlier
// board of its own - the pickup step's own board is the emptied table.
//
// INPUT (`in`):
//   0  u8 version (FIO_PRETABLE_VERSION)
//   1  u8 n_events
//   2  u8 the prior board's battle count, or FIO_PRETABLE_NONE for no prior
//   3  2 x that many u8: the prior board
//   then per event:
//     u8 type (EVW_T_*/ANIM_EVT_*)
//     u8 this step's own board: its battle count, or FIO_PRETABLE_NONE
//     2 x that many u8: the board's battles
//     u8 n_cards, n_cards x u8 dense card id (a pickup's cards ARE the table)
//   A table is always 2 bytes per battle - the attack, then its cover or
//   FIO_PRETABLE_NONE - which is the layout fio_play_probe's board takes.
//
// OUTPUT (`out`):
//   0  u8 version
//   1  u8 n_battles
//   2  u8 1 when the pairing is a REAL board, 0 when it is the flat reading
//        (one cell per picked-up card - the right cards in a shape nobody
//        vouched for; a caller choosing between two tables must not take it
//        for a table)
//   3  2 x n_battles u8: the table
// Returns bytes written, or a negative error.
#define FIO_PRETABLE_VERSION 1
#define FIO_PRETABLE_HEAD    3
// "no board" and "this attack is uncovered" are the same byte, which is the one
// legal.h's LEGAL_WIRE_NONE and anim_plan.h's ANIM_TABLE_NONE already use.
#define FIO_PRETABLE_NONE    0xFE
int fio_pre_bout_table_packed(const uint8_t *in, int len, char *out, int cap);

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

// THE SETTLEMENT (src/evwire.h's evw_is_settlement): is this event type
// (`GameEvent.type`) one of the consequences a bout-ender runs - the
// transition, the discard, the refill, the trash sweep - rather than the
// acting seat's own play?
//
// The extension cuts a staged turn's animation at the FIRST step this answers
// yes for, and holds everything from there until the human presses Send. That
// is the whole of the "staged good deals me a hand I can look at, then undo"
// hole: the cards a bout end deals are secret, and a staged move is not a move
// until it is sent. Asked of the kernel rather than re-listed per client
// because which steps a bout end owns is a rules fact.
int fio_evw_is_settlement(int type);

// WHERE A STAGED TURN SETTLES: the index, into the flattened event list of the
// frames fio_replay_last_events_packed returned, of the first step that belongs
// to the bout end rather than to the move that caused it. -1 when the turn
// ended no bout; below -1 for a stream that is not whole.
//
// A turn is several frames because a player may stage several actions before
// sending, and both clients flatten them into one list before animating - so
// the cut has to be counted ACROSS frames, in order, which is the part a client
// would get subtly wrong on its own. Everything from the cut onward is withheld
// until Send (see fio_evw_is_settlement for what is being withheld and why).
int fio_evw_frames_settlement_cut(const unsigned char *frames, int len);

// Where the moves THIS DEVICE has staged begin, as an atom count on the
// resident game - the `atoms_before` a board passes to
// fio_replay_last_events_packed to animate its own turn, and the same number
// msg_seal measures the bubble delta with. -1 when no chain has been adopted.
//
// The alternative - the atom count of the chain that was adopted - is wrong for
// the same reason subtracting two atom counts was: the stream is re-derived
// from the whole log every time, so it can shrink under a history that only
// grew. See the implementation.
int fio_msg_staged_atoms_before(void);

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
//   parent8(8) digest(32) sent_at(u16 LE) n_new(1) opening(1) carry_key(u32 LE)
//   carry_fool(1) passing(1) n_joins(1)
//   then n_joins*{seat(1) name_len(1) name[]}.
// `passing` is the table's rules, already resolved against the envelope's
// format: 1 the defender may transfer, 0 podkidnoy (see fio_set_passing).
// ROUND 16: sent_at is the envelope's send clock (unix seconds mod 65536); 0
// when the chain is format 2 and carries none, which means no pickup hold.
// Bytes written or negative (FIO_EMSG → fio_last_msg_error).
int fio_msg_decode_packed(const uint8_t *payload, int len, unsigned char *out, int cap);

// READ the same blob and ADOPT NOTHING: no replay, and not one byte of the
// resident game - nor of the base a later seal measures its bubble against -
// changes. For a caller that only wants the header (the composer reading the
// joins and the summary out of the bubble it has just sealed).
//
// ROUND 16, and the reason it exists: a decode is not a read. The composer's
// "idempotent re-decode" of its own outgoing payload told the kernel that the
// chain up to and including the staged move was history somebody else made, so
// the NEXT action of the same turn measured its delta from the middle of its
// own bubble - a bubble carrying two actions claimed one, and its caption and
// its recipient's animation both lost everything but the last.
//
// Nothing here validates the body, so the fields are the sender's claims: peek
// what you are about to SEND or merely describe, decode what is about to be
// PLAYED (there, validation is the replay and the replay is the point).
// Bytes written or negative (FIO_EMSG → fio_last_msg_error).
int fio_msg_peek_packed(const uint8_t *payload, int len, unsigned char *out, int cap);

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

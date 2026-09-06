// FMSG v1 ("msg wire") — the iMessage envelope. THE serverless game payload:
// an MSMessage URL carries one of these, and it holds the ENTIRE game as
// (32-byte deal seed, ordered action chain). Every device reconstructs the game
// by re-dealing from the seed and replaying the chain through the kernel, so
// there is no server and no authority — only bytes that either replay or don't.
// Spec: docs/IMESSAGE_GAME_DESIGN.md §4/§7, corrected by
// docs/IMESSAGE_IMPLEMENTATION_HANDOFF.md §3.1 (32-byte seed) and by
// docs/IMESSAGE_BODY_CODEC.md (the body is a v6 code, not raw frames — §3.2's
// raw chain was measured and cut). In C, like every other rule in this tree: the
// same bytes must mean the same game on the phone (libfoolish.a) and on the web
// (rules.wasm), and one implementation is the only way to guarantee that.
//
// Wire layout (little-endian):
//
//   off  size  field
//   0    1     magic      0xF7
//   1    1     format     2; 3 adds a send clock + a bubble delta; 4 adds the
//                          fool's penalty; 5 and 6 are 3 and 4 with the variant
//                          byte spent on the RULES (1 was cut before shipping)
//   2    1     flags      bit0 fair_deal, bit1 gzip-body,
//                          bit2 = legacy (was passing_allowed in 1.0(3); tolerated
//                          on decode, never set now), bits3-7 reserved=0
//   3    1     phase      0 WAITING, 1 ACCEPT, 2 LIVE, 3 FINISHED
//   4    8     game_id    random u64, constant for the game
//   12   2     turn       u16, count of kernel actions applied
//   14   1     last_actor_seat
//   15   1     n_players  2..8
//   16   1     variant    FORMAT 5/6: the table's RULES - bit0 MSG_VARIANT_PASS
//                          (0 podkidnoy, 1 perevodnoy). Reserved =0 on 2/3/4,
//                          which are the passing game by definition. See below.
//   17   1     round      completed-round counter (Rule R's guard input)
//   18   8     parent8    first 8 bytes of SHA-256(previous envelope), 0 at creation
//   26   32    seed       -> game_set_deal_seed_bytes(seed, 32)
//   58   2     sent_at    FORMAT 3+ ONLY: unix seconds mod 65536 (0 = none)
//   60   1     n_new      FORMAT 3+ ONLY: atoms THIS bubble added
//                          (0 = unknown, 255 = none - see MSG_NEW_NOTHING)
//   62   1     opening    FORMAT 4 ONLY: the seat this deal opens on (0xFF = derive)
//   63   4     carry_key  FORMAT 4 ONLY: u32 LE roster key of the game before (0 = none)
//   67   1     carry_fool FORMAT 4 ONLY: the fool's canonical index (0xFF = none)
//   58   1     n_joins    (61 on format 3, 68 on format 4)
//   59   var   joins      n_joins x { u8 seat, u8 name_len<=64, name utf8 }
//   var  2     n_actions  u16, the action count the body must yield
//   var  var   body       the v6 replay code — see THE BODY
//
// THE BODY is a v6-family replay code (replay.h) - the codec that already
// ships. "v6" here is the DECODER FAMILY, not the version byte the producer
// writes: today's encoder stamps 10 (REPLAY_FORMAT_VERSION_V10), which is v6
// plus a pass-mode bit, a forced-opening bit and the corrected deal order, and
// every entry point that reads one is still named _v6 because it accepts the
// whole line. What this file counts - atoms - is the same in any of them. It
// entropy-codes each action as an index into that state's legal-move menu, so an
// action costs ~1-2 bits instead of the ~34 a raw frame spends. Measured over
// 240 full games per size: 34 B at 2p, 45 B at 4p, 68 B at 8p — 8x to 18x
// smaller than a raw chain, putting a 4-player envelope at ~208 base32 chars
// against §4.4's 1,000-char budget. A raw body was tried and cut; see
// docs/IMESSAGE_BODY_CODEC.md for the measurements that killed it.
//
// This is the natural fit, not a trick. docs/IMESSAGE_IMPLEMENTATION_HANDOFF
// §3.3 rules v6 out as "the continuation format" because v6 carries NO deal
// seed: alone, it reveals the cards dealt so far but nothing about the undealt
// stock, so two devices cannot draw identically from it. But this envelope's
// header carries the seed. **Seed (the future) + v6 code (the past)** is exactly
// the pair serverless play needs, and it is what §16's "FMSG v2" asked for —
// already built, already proven, already version-dispatched. v6's mid-game cut
// (an explicit atom count) is what lets it encode a TURN and not just a finished
// game, and v6 codes a pending `good` for the same reason (a mid-round good is
// the commonest correspondence turn; without that atom 47% of 4-player mid-game
// states were unrepresentable).
//
// The seed is why a continuation is not a replay. replay_steps.c rebuilds a
// Game from a code's DEAL/DRAW atoms and fills the never-drawn tail in canonical
// order — right for rendering a finished game, wrong to play on from, because a
// continuation draws from that tail and canonical order is not the shuffled
// stock. So msg_replay deals the TRUE deck from the seed and takes only the
// ACTIONS from the code.
//
// TWO LAYERS, deliberately separate:
//
//   msg_decode  — STRUCTURE only. Parses and bounds-checks the bytes. Builds no
//                 Game and answers no rules question. Hostile input is expected
//                 here (the payload arrives from a URL), so every field is
//                 range-checked and every length is proven against the buffer.
//                 The body is opaque to this layer (an entropy-coded integer —
//                 only the codec can say whether it is well-formed), so it is
//                 checked at replay instead. Nothing trusts it in the meantime.
//   msg_replay  — SEMANTICS. Re-deals from the seed and applies each action
//                 through the PUBLIC kernel handlers, exactly as the shim does.
//                 A corrupt or hand-edited chain fails here, loudly, because an
//                 illegal action simply won't apply. There is no partial
//                 recovery and no memcpy into a Game — validation IS replay
//                 (§7.3).
//
// The split is what keeps the hostile-bytes surface small and auditable: decode
// touches only this file's arithmetic; replay touches only public kernel calls.
//
// ZERO-COPY ACTIONS. MsgEnvelope BORROWS the action bytes from the caller's
// buffer instead of decoding them into an array. A decoded AwireAction is ~120
// bytes, so a 512-action chain would be ~60KB — and rules.wasm's linear memory
// is PINNED at 3 pages (c/Makefile: --initial-memory == --max-memory), so
// that array would refuse to link. Frames are self-delimiting (awire_frame_len),
// so walking them on demand costs nothing and stores nothing. Consequence, and
// the one rule for callers: `actions` points INTO the decoded buffer — it must
// outlive the envelope, and an in-place encode must not overwrite the buffer it
// is still reading (msg_encode_into documents the one safe ordering).
#ifndef CNITRO_MSG_WIRE_H
#define CNITRO_MSG_WIRE_H

#include "game.h"
#include "awire.h"
#include "sha256.h"
#include <stdint.h>

#define MSG_MAGIC        0xF7
// Format 1 (a raw chain of seat-prefixed awire frames) was measured, rejected
// and never shipped: it costs ~34 bits for an action worth ~1-2 and missed the
// size budget by 1.33x at 4 players, permanently. The version byte keeps its
// grave so nothing re-uses the number. See docs/IMESSAGE_BODY_CODEC.md.
#define MSG_FORMAT_V6    2   // body = a v6 replay code, no clock
// Format 3 = format 2 plus a two-byte SEND CLOCK and a one-byte BUBBLE DELTA,
// the two things round 16 found this wire could not answer. Both are additions
// to the header, and both are 0 on a format-2 chain, which is exactly what
// "this bubble does not say" means for each of them.
//
// THE CLOCK. Round 16:
// the defender may not pick up within MSG_PICKUP_HOLD_S seconds of an attack,
// so that the attackers get a fair chance to throw more in — and answering
// "how long ago was this attack sent" needs a time this wire never carried.
//
// Nothing on the platform could answer it instead. MSMessage's whole public
// surface is session/pending/senderParticipantIdentifier/layout/URL/
// shouldExpire/accessibilityLabel/summaryText/error: no date. The times a user
// reads in the transcript come from the message database, which an app
// extension cannot see.
//
// A NEW FORMAT was unavoidable rather than chosen. Trailing bytes cannot carry
// it (msg_decode takes the body as everything to the end of the buffer, so an
// appended field lands inside the v6 code and corrupts it), and the header has
// no hole (`variant` is one byte and must be 0). The cost is exact and was
// accepted by the owner: a build that only knows format 2 rejects a format-3
// bubble outright (MSG_EFORMAT). The other direction is preserved on purpose —
// this build reads format 2 fine, as a chain with no clock, which by
// `msg_pickup_hold_remaining`'s contract means no hold at all.
//
// THE BUBBLE DELTA (`n_new`) is how many atoms of this body belong to THIS
// bubble's turn: the last n_new atoms are the move it carries, and everything
// before them is history its recipient has already seen.
//
// It is NOT the difference of two `turn`s, though it was at first. A chain
// appends LOGS, but its atom stream is re-derived from all of them every time
// it is encoded, and a good stops being an atom the moment anything follows it
// (replay.c's log_atom_kind) - so the same history can encode to fewer atoms
// than the parent claimed, and the subtraction loses exactly one per superseded
// good. What that cost is the FRONT of a turn: a defender who covered twice
// into one bubble sealed a delta of 1, and both the caption on the bubble and
// the animation its recipient played dropped the first cover. The seal measures
// from the LOG MARK instead (msg_seal), which only ever grows.
//
// Without the delta at all a receiver can only GUESS the boundary, and the guess it
// used to make (the trailing run of steps by one seat) is wrong in two ways the
// owner hit in play: a defender who covers, sends, covers, sends puts two cover
// atoms on the chain that are indistinguishable from two staged at once, so
// opening the second bubble replays both; and a cover that ends the bout
// without a ROUND_END atom (the defender's last card - handle_cover discards
// inline) sits directly before that same seat's opening attack of the next
// bout, so replaying the attack replays the cover with it.
//
// ONE BYTE, and a DELTA rather than an index. The alternative considered was a
// per-atom boundary marker inside the v6 body, which is cumulative (the body
// carries the whole game, so it grows ~1 bit per atom per bubble, 15-30% on a
// measured body by the end of a game) and would re-point every v6 code the
// website, the share links and the Oracle already read. An absolute parent-turn
// index would need two bytes to match `turn`; the delta fits in one.
//
// WHY A WHOLE BYTE for a number that small, when flags has five reserved bits.
// Because the number is not that small. A staged turn is not bounded by six
// table slots - the attack limit is the DEFENDER'S HAND (MAX_BATTLES 32, 64 on
// wasm: "a defender holding 33+ cards can legally face 33+ simultaneous
// attacks"), so a defender who just picked up a fat pile can legitimately stage
// ten or twenty covers into one bubble. A nibble would truncate real play and
// five bits would sit right against it; the deck's 36 cards are the true
// ceiling and a byte clears them with room. What the byte costs is ~1.6 base32
// chars on a ~240-char bubble against a 1,000-char budget - less than the
// header's remaining reserve is worth (owner's call, round 16).
//
// Note it does not bound the GAME: a 300-atom game is 300 bubbles of delta 1,
// and the total stays in `turn`, a u16. A delta that would not fit the byte
// seals as 0 - "this bubble does not say" - rather than as a clamp, because a
// too-large delta would name a suffix starting INSIDE the bubble, and a
// confident wrong boundary is worse than an honest missing one.
//
// AND A BUBBLE CAN ADD NOTHING (MSG_NEW_NOTHING). Undo-to-empty is a real move
// in this UI: Messages offers no API to REMOVE a staged bubble, so §10 cancels
// one by overwriting it with a re-seal of the state the chain was already in.
// That bubble is sendable, and what it carries is a board every recipient has
// already seen. "0 atoms added" is not 0 on this wire - 0 is "does not say",
// whose fallback GUESS animates the previous player's move again (owner, round
// 16: "if you stage a move then undo, you can still send a message and it will
// look weird for the other players"). So the count and the absence of a count
// are separate values, and the third state gets the one byte value a real delta
// can never take.
#define MSG_FORMAT_CLOCK 3

// Format 4 = format 3 plus THE FOOL'S PENALTY, the durak-ism this wire could
// not express: a rematch played by the same people, in the same cycle, does not
// open on the lowest trump - it opens on the seat to the RIGHT of the last
// game's fool, so the fool is the first player attacked. (Right, not left:
// attacks travel to the attacker's left, so the seat on the fool's right is the
// one whose attack lands on them.)
//
// THE RULE CANNOT BE RE-DERIVED BY A RECEIVER, which is why it needs the wire.
// A device that opens a rematch bubble may never have held the game before it -
// reinstalled, joined the chat late, or simply looking at a chain whose parent
// scrolled away - so "who was the fool" is not a fact every device has. It is
// also not a fact any device may DECIDE alone: the opening seat changes the
// deal's whole shape, and two devices that disagree about it have forked the
// game. So the chain states it.
//
// THREE FIELDS, in two phases that never overlap:
//
//   `opening` is the ANSWER, and it rides every LIVE and FINISHED bubble of a
//   game the rule touched. It is the seat game_open_at_seat pins before the
//   deal (game.h), so every device re-deals the identical board from the seed.
//   0xFF (MSG_NO_OPENING) is an ordinary game: derive from the lowest trump,
//   exactly as before this format existed. It rides EVERY bubble rather than
//   just the first because a chain is replayed from its seed on every open -
//   there is no "first bubble" a later reader can consult - and because the
//   turn-0 LIVE handoff carries no body at all, so the body's own recorded
//   opener (v8, replay.h) cannot answer at the one moment it matters most.
//
//   `carry_key` + `carry_fool` are the QUESTION, and they ride only the WAITING
//   lobby a "New game" creates. carry_key fingerprints the roster that lobby
//   was born with; carry_fool names, within it, who the fool was. Whoever taps
//   Start re-fingerprints the roster it is actually starting and compares: same
//   people in the same cycle, the rule applies; anyone joined, left or was
//   renamed, it does not, and the game opens on the lowest trump like any
//   other. That is the guard the owner specified - "if the players do not
//   change at all between the lobby and the start" - and it lives here, in C,
//   because it decides a deal.
//
// ROTATION-CANONICAL, deliberately. The lobby a rematch creates seats whoever
// tapped New game at 0, so the same table in the same cyclic order comes back
// ROTATED: Alex/Bob/Cindy becomes Bob/Cindy/Alex. That is the same table and
// must fingerprint equal, while Alex/Cindy/Bob - an order no rotation
// produces - must not. msg_roster_key hashes the rotation whose bytes are
// smallest, so every rotation of one table yields one key, and carry_fool is an
// index into THAT rotation rather than into a seating that moves.
//
// SIX BYTES, and only on a game the rule touched. seal_format writes format 3
// (or 2) whenever all three fields are empty, so an ordinary game pays nothing
// at all; a rematch pays ~10 base32 chars on a ~240-char bubble. The
// alternative - splitting the answer and the question into two formats to save
// five bytes on live bubbles - buys less than it costs in a wire that then has
// four live formats instead of three.
#define MSG_FORMAT_REMATCH 4

// The hold itself, in seconds (owner: "you cannot pickup within 15 seconds of
// the attack").
#define MSG_PICKUP_HOLD_S 15

// Formats 5 and 6 = formats 3 and 4 with THE VARIANT BYTE SPENT: it is now the
// table's RULES, and bit 0 is PASSING (perevodnoy, the transfer). 0 is
// podkidnoy, the throw-in game with no transfer at all (game.h
// GAME_RULE_NO_PASS). Every other bit stays reserved and must be 0.
//
// WHY THE HEADER CARRIES THE RULES WHEN THE BODY ALREADY DOES. A v10 code names
// its own pass mode (replay.h), and for a LIVE bubble that would be enough. A
// WAITING lobby has no body at all - the deal alone is the state - so the one
// place a lobby's rules can live is the header, and the lobby is exactly where
// they are chosen. Carrying it on every phase keeps one answer rather than two:
// a receiver reads the rules the same way whether it is looking at a lobby or
// at the twentieth bubble of a game, and msg_replay checks the header against
// the body's bit, so a chain cannot say one thing and play the other.
//
// WHY A NEW FORMAT RATHER THAN A NEW MEANING FOR AN OLD BYTE. Under formats
// 2-4 the variant byte is reserved and must be 0, and 0 meant the only game
// this engine played: with the transfer. The owner's call is that the byte
// reads 0 = podkidnoy / 1 = passing from here on - which is the opposite
// reading of the same byte, so every bubble already sitting in a transcript
// would flip its rules under it. A version number is exactly the instrument for
// that: formats 2-4 keep the old reading (variant 0, passing), formats 5-6
// carry the new one, and a build that predates them refuses a format it does
// not know (MSG_EFORMAT) instead of quietly dealing a different game. Every
// seal this build makes writes 5 or 6, because the rules must never again be a
// byte whose meaning depends on who is reading.
//
// The header does not grow: 5 is 3's 62 bytes and 6 is 4's 69, so an ordinary
// game pays nothing for saying what it is.
#define MSG_FORMAT_RULES         5
#define MSG_FORMAT_RULES_REMATCH 6

#define MSG_VARIANT_PASS  0x01
#define MSG_VARIANT_KNOWN (MSG_VARIANT_PASS)

#define MSG_PHASE_WAITING  0
#define MSG_PHASE_ACCEPT   1
#define MSG_PHASE_LIVE     2
#define MSG_PHASE_FINISHED 3

#define MSG_FLAG_FAIR_DEAL 0x01
#define MSG_FLAG_GZIP      0x02
// bit2 (0x04) was PASSING_ALLOWED, a forward-compat marker 1.0(3) set on every
// seal. REMOVED (1.0(4)): the pass/perevod mode now lives in the replay code
// (the v7 pass-mode bit, replay.h), so the message format no longer needs it.
// This build does not set it, and nothing has a named define for it any more.
// validate_fields still TOLERATES a stray bit2 (0x04) so a bubble sealed by
// 1.0(3) still decodes and re-encodes to itself; no new meaning is attached.

// Was 12: the App Store review's B1 (docs/APP_REVIEW_NOTES.md) found that cap
// too tight for a byte-counted UTF-8 name — "Владимир" (8 letters, 16 bytes)
// silently failed to seal. Owner's round-5 call: allow up to 64 bytes.
#define MSG_MAX_NAME     64
// …and the DISPLAY cap that rides beside it: a name can clear 64 bytes and
// still be a run of 1-byte characters no lobby row or seat badge has room for.
// Not the wire's business, but it is the same decision as the byte cap and the
// two are asked together (msg_nickname_verdict), so they are named together.
#define MSG_MAX_NAME_CHARS 16
#define MSG_MAX_JOINS    MAX_PLAYERS
#define MSG_SEED_LEN     FOOLISH_SEED_LEN   // 32 — the ChaCha key width
#define MSG_PARENT_LEN   8
#define MSG_HEADER_LEN   59                 // format 2: through n_joins
// Format 3 puts its two clock bytes at 58 and its delta byte at 60, AFTER the
// seed and BEFORE n_joins, so every offset a format-2 reader knows is unchanged
// and the two decoders share one prefix. n_joins lands at 61.
#define MSG_CLOCK_OFF    58
#define MSG_NEW_OFF      60
#define MSG_HEADER_LEN_CLOCK 62
// Format 4 appends its three rematch fields after format 3's, on the same
// principle: every earlier offset is untouched and the decoders share one
// prefix. n_joins lands at 68.
#define MSG_OPEN_OFF     62
#define MSG_CARRY_OFF    63
#define MSG_FOOL_OFF     67
#define MSG_HEADER_LEN_REMATCH 69
// "Derive the opening seat from the lowest trump" - an ordinary game.
#define MSG_NO_OPENING   0xFF
// "This lobby carries no fool to punish" - an ordinary lobby.
#define MSG_NO_FOOL      0xFF
// The delta's ceiling; past it a seal writes 0 ("does not say") rather than a
// clamp - see the MSG_FORMAT_CLOCK note. 254 rather than 255 because the top
// value is spoken for: it is the third state, below.
#define MSG_MAX_NEW      254
// "THIS BUBBLE ADDED NOTHING" - the undo-to-empty re-seal (§10). Distinct from
// 0 ("does not say") because the two want opposite things from a reader: 0 asks
// it to guess a boundary, and this one states that there IS no move here, so
// nothing animates. 255 carries it because it is the one byte value no honest
// delta can hold (MSG_MAX_NEW caps a real one at 254), which keeps the field a
// plain count everywhere else.
#define MSG_NEW_NOTHING  255
// msg_seal's `base_turn` for a host that cannot say where the parent chain
// ended - the seal then writes no delta and the receiver guesses, exactly as
// every build before round 16 did. A GENESIS is not this: it passes 0, because
// a chain with no parent really did add all of itself.
#define MSG_NO_BASE      (-1)
// msg_seal's `base_turn` for the re-seal that adds NOTHING: the chain ends
// exactly where this body ends. It is its own value rather than "pass the base
// and let the subtraction come out 0" because at seal time a zero difference is
// AMBIGUOUS - the codec folds a bout's closing goods into the round_end atom
// that replaces them, so a real move can seal to its parent's atom count too
// (measured at 22% of one-action bubbles). The two are told apart by the one
// host that knows, and it knows because nothing was applied to the resident
// game since it adopted the chain - see fio_msg_encode.
#define MSG_BASE_NOTHING (-2)

// A full game is ~60-90 actions at 2 players (spec §4.4); 8-player games run
// longer. 1024 is far above any reachable game and bounds the decode walk.
#define MSG_MAX_ACTIONS  1024

// Body byte cap, sized for the RAW fallback (a v6 body is an order of magnitude
// under it). The size guardrail is P95 < 1,000 base32 chars (~625 bytes) for a
// whole envelope, and MSMessage.url tolerates 5,000 chars; 4096 body bytes is
// ~6.5x the P95 target and still ~1/2 the platform cap, so a payload that trips
// this was never going to fit in a URL anyway.
#define MSG_MAX_ACTION_BYTES 4096

// Errors (all negative; 0 is never an error).
#define MSG_EOK          0
#define MSG_ESHORT      -1   // buffer ends inside a field
#define MSG_EMAGIC      -2   // not an FMSG payload
#define MSG_EFORMAT     -3   // unknown format byte
#define MSG_EFLAGS      -4   // reserved bit set, or a flag this build can't honor
#define MSG_EPHASE      -5   // phase out of range, or inconsistent with the chain
#define MSG_EPLAYERS    -6   // n_players outside 2..8
#define MSG_EVARIANT    -7   // a variant bit this build does not implement
#define MSG_ESEAT       -8   // a seat >= n_players, or a duplicate join
#define MSG_ENAME       -9   // name too long, or non-printable bytes
#define MSG_ESEED      -10   // all-zero seed outside fair-deal
#define MSG_EACTION    -11   // malformed awire frame, or n_actions disagrees
#define MSG_ETURN      -12   // turn != n_actions
#define MSG_EROUND     -13   // round byte disagrees with the replayed chain
#define MSG_ECAP       -14   // output buffer too small
#define MSG_ETRAIL     -15   // trailing bytes after the chain
#define MSG_ECHAIN     -16   // an action was rejected by the kernel: illegal chain
#define MSG_EJOINS     -17   // n_joins is 0 or > n_players
#define MSG_EBODY      -18   // the v6 body did not decode (replay_last_error_detail has why)

typedef struct {
    uint8_t seat;
    uint8_t name_len;
    char    name[MSG_MAX_NAME];
} MsgJoin;

typedef struct {
    uint8_t  format;   // one of MSG_FORMAT_*
    uint8_t  flags;
    uint8_t  phase;
    uint64_t game_id;
    uint16_t turn;
    uint8_t  last_actor_seat;
    uint8_t  n_players;
    // The RULES byte, raw: bit0 MSG_VARIANT_PASS on format 5/6, and 0 (=the
    // passing game, which is all those formats could describe) on 2/3/4. Read
    // it through `msg_pass_allowed` rather than testing the bit, so no caller
    // has to remember which formats predate it.
    uint8_t  variant;
    uint8_t  round;
    uint8_t  parent8[MSG_PARENT_LEN];
    uint8_t  seed[MSG_SEED_LEN];

    // Unix seconds MOD 65536, as the sending device's clock read them when it
    // sealed this envelope. 0 means NO CLOCK — a format-2 chain, or a host that
    // did not stamp one — and every rule that reads it treats 0 as "no hold".
    // (A real stamp lands on 0 one second in every 65,536; the cost of that
    // collision is one skipped 15-second hold, which is why 0 is allowed to be
    // the sentinel.)
    //
    // TWO BYTES, and the wrap is deliberate. Nothing ever needs the absolute
    // time — only `now - sent_at` against 15 — and unsigned modular subtraction
    // gets that right across a rollover with no special case. What the wrap
    // costs is DISTINGUISHABILITY: a chain sent 3 seconds ago and one sent
    // 18h12m+3s ago produce the same delta, so roughly one stale open in 4,400
    // draws a 15-second hold it did not earn. One wait, self-clearing. A u32
    // would buy that back for two more bytes; it was judged the wrong trade
    // against a wire whose whole design is bytes-per-bubble.
    uint16_t sent_at;

    // How many atoms THIS bubble added to the chain - `turn` minus the turn of
    // the envelope it continues. 0 means the bubble does not say: a format-2
    // chain, or a genesis/lobby seal with nothing in it. Readers treat 0 as
    // "guess" (see fio_replay_last_events_packed), which is what every build did
    // before this field existed, so an old chain animates exactly as it always
    // did. MSG_NEW_NOTHING means the opposite of a guess: this bubble added no
    // atoms at all (the undo-to-empty re-seal), so a reader animates NOTHING.
    //
    // Set by msg_seal from the base turn it is handed, never by a caller: like
    // `turn` and `round` it is a claim about the body, and a producer that
    // could write it freely could emit a bubble that animates a move it did not
    // carry. Bounded by `turn` for the same reason (validate_fields).
    uint8_t  n_new;

    // THE FOOL'S PENALTY, format 4 (see MSG_FORMAT_REMATCH for the rule).
    //
    // `opening` is the seat this deal opens on, or MSG_NO_OPENING to derive it
    // from the lowest trump. Unlike turn/round/n_new this IS a caller's field:
    // it is not a claim about the body but a term of the deal, settled at Start
    // by msg_rematch_opening and then simply repeated by every later seal, the
    // way `seed` is. msg_replay checks it against the body's own recorded
    // opener, so a chain that lies about it does not replay.
    uint8_t  opening;

    // The rematch carry, meaningful only on a WAITING lobby. `carry_key` is the
    // roster key (msg_roster_key) of the roster this lobby was created with; 0
    // means "no carry", an ordinary lobby. `carry_fool` is the fool's index
    // within that key's canonical rotation, or MSG_NO_FOOL.
    uint32_t carry_key;
    uint8_t  carry_fool;

    int      n_joins;
    MsgJoin  joins[MSG_MAX_JOINS];

    // The body, borrowed and NOT owned — see the zero-copy note above.
    // `n_actions` is the action count the body must yield; for format 2 that is
    // a claim only the codec can settle, so msg_replay checks it.
    int                  n_actions;
    int                  actions_len;
    const unsigned char *actions;
} MsgEnvelope;

// May the defender transfer, in the game these bytes describe? THE reader for
// the variant byte: it knows that formats 2-4 predate the rules byte and are
// the passing game by definition, so no caller has to remember which formats
// say what. Safe on any decoded envelope.
int msg_pass_allowed(const MsgEnvelope *e);

// THE PRODUCER. Fills in `e`'s body and the four header fields that describe
// it - `n_actions`, `turn`, `round`, `n_new` - for `g`, a game dealt from
// `e->seed` and played. The caller sets the rest (game_id, phase, seed, joins,
// parent8, last_actor_seat). Returns MSG_EOK or a negative MSG_E*; `body`
// (>= 512 B is ample; a full 8p game measures ~68) receives the code and
// `e->actions` is left borrowing it, so it must outlive `e`.
//
// `base_logs` is g->num_logs AT THE MOMENT THIS HOST ADOPTED THE CHAIN it is
// continuing - the log mark - or MSG_NO_BASE for "this host cannot say", which
// seals n_new = 0 and leaves the receiver to guess the boundary as it always
// did, or MSG_BASE_NOTHING for "this bubble adds nothing", which seals
// n_new = MSG_NEW_NOTHING so the receiver animates nothing at all. A genesis
// passes 0: everything on the chain is new. It is an input rather than
// something derived here because it is the one fact the body cannot tell us -
// the body is the whole game, and where the PREVIOUS bubble ended is not in it.
// The delta itself is still derived (the atoms after that mark,
// replay_atoms_before_log), so the same rule holds as for turn/round: a host
// cannot claim a boundary its body does not have.
//
// A LOG MARK and not the parent's atom count, which is what round 16 first
// used: a chain appends logs, but its atom stream is re-derived from all of
// them every time, and a good that was an atom while it was pending stops
// being one as soon as anything follows it. Subtracting the parent's `turn`
// therefore lost one atom per superseded good, and a turn of several actions
// sealed as fewer - so its recipient animated, and its sender captioned, only
// the tail of it.
//
// It derives those fields by DECODING THE BODY IT JUST WROTE and replaying it
// into `scratch`, rather than by counting what the caller thinks it played.
// Two reasons, both load-bearing:
//
//   - `turn` counts ATOMS, and atoms are not moves. The codec folds a bout's
//     closing goods into ONE round_end atom, so a chain of 8 kernel actions can
//     be 5 atoms. Only the codec knows the number, and Rule P orders chains on
//     it — a producer that guessed would order the whole protocol wrong.
//   - a header that disagrees with its body is exactly what msg_replay rejects
//     (MSG_ETURN / MSG_EROUND). Deriving the header FROM the body means a host
//     cannot emit a payload it would itself refuse.
//
// `scratch` is a caller-owned Game (a Game is ~33KB — far too big for a wasm
// stack, and this file must not own static state that rules.wasm's pinned memory
// would have to hold).
//
// MSG_EBODY means the v6 producer refused — it rejects a game whose log buffer
// overflowed (num_logs >= MAX_LOGS), which was measured on ~10% of full 8-player
// games and never at 2-4p. See docs/IMESSAGE_BODY_CODEC.md §4.
int msg_seal(MsgEnvelope *e, const Game *g, int base_logs,
             unsigned char *body, int body_cap, Game *scratch);

// THE BASE A HOST SHOULD SEAL WITH, from the one thing it remembers about the
// chain it adopted: `g->num_logs` AT THE MOMENT IT ADOPTED IT (`base_logs`,
// negative if it did not look). Returns base_logs, MSG_NO_BASE, or
// MSG_BASE_NOTHING.
//
// The question it answers is "has this game moved since I adopted the chain",
// and it answers it by OBSERVING THE GAME rather than by counting applies,
// because the log array is what the body is encoded from: a game whose log
// count is where adoption left it encodes to the same body the parent carried,
// so that bubble demonstrably adds nothing. A counter incremented at each apply
// would have to be found and bumped by every path that can move a game (an
// apply, a rebase, a bot drive), and the one that got missed would seal a real
// move as "nothing" and animate it nowhere.
//
// It lives here rather than in each host so that the two that seal FMSG - the
// phone and the browser twin - cannot disagree about what an empty bubble is.
int msg_seal_base(const Game *g, int base_logs);

// Parse + bounds-check `in` into `out`. Returns MSG_EOK or a negative MSG_E*.
// Never reads past `in_len`, never allocates, never builds a Game. On success
// `out->actions` points into `in`.
int msg_decode(const unsigned char *in, int in_len, MsgEnvelope *out);

// 1.0(6) DIAGNOSTIC: replay codec version (9 or 10) of the last body msg_replay
// decoded, or -1 for an empty-body message. Set by msg_replay.
extern int msg_last_body_version;

// Serialize `e` into `out`. Returns bytes written, or a negative MSG_E*. The
// same field validation decode applies runs here too, so this host can never
// emit a payload it would itself reject.
//
// In-place aliasing (out == e->actions' buffer) is SAFE: the chain is the last
// thing written and the header is a fixed 59+joins bytes, but a caller that
// relies on that must guarantee the output offset never passes the read cursor.
// The wasm bridge sidesteps it entirely by encoding from a separate buffer.
int msg_encode(const MsgEnvelope *e, unsigned char *out, int out_cap);

// Replay `e`'s chain into `g` through the public kernel handlers: re-deal from
// the seed, then apply each seat-prefixed action in order. Returns MSG_EOK, or
// MSG_ECHAIN on the first action the kernel rejects (`g` is then garbage — an
// envelope is all-or-nothing). Also cross-checks the header's `round` against
// the round closures the replay actually produced (MSG_EROUND) and FINISHED
// against game_done (MSG_EPHASE): a header that lies about the chain it carries
// is a tampered header, and Rule P reads those two fields BEFORE anyone replays.
//
// NOTE this touches the process-wide deal RNG (game_set_deal_seed_bytes), like
// every other seeded deal in this tree — see game.h's deal-RNG save/restore note
// if you need to replay inside another game's lifetime.
//
// Costs no scratch and no log buffer: the body is read at the codec's own atom
// level (replay_decode_atoms_v6), not expanded into a log stream.
int msg_replay(const MsgEnvelope *e, Game *g);

// ---------- Rule P: which chain does every device prefer? -----------------
//
// Two chains for the same game_id are ordered by (§7.2):
//
//   0. a STARTED chain beats a pre-game one — phase >= MSG_PHASE_LIVE outranks
//      WAITING/ACCEPT, always
//   1. higher round wins        — a closed bout is settled history
//   2. else higher turn wins    — more accepted actions
//   3. else more JOINS wins     — the fuller roster is strictly later history
//   4. else smaller SHA-256     — arbitrary, but identical everywhere
//
// Rule 0 is not cosmetic, and it is not subsumed by round/turn: a WAITING lobby
// and the LIVE handoff that starts it BOTH sit at round 0 / turn 0 (the handoff
// applies no action — see msg_seal's 0-action path), so without it the two tie
// all the way down to the digest and the winner is a COIN FLIP. Devices that
// cached the lobby then kept it, and `adopt` rendered that phase-0 payload as a
// board — which is dealt at the lobby's CAPACITY (8 for a group), with joins
// only for whoever had joined. That is the "some players see a 5-player game,
// others see an 8-player one with seats named 'Seat N'" fork, and because those
// two deals have different first attackers, the game deadlocks. A started chain
// is never superseded by the invite it grew out of, so it wins outright.
//
// Rule 3 closes the SAME class of fork one layer up, between two STARTED
// chains. Lobby v3 lets ANY joined player tap Start, and Start deals at the
// tapped bubble's join count — so two players starting near-simultaneously
// (or one starting off a stale bubble that predates the last join) seal TWO
// LIVE handoffs, both at round 0 / turn 0, dealt from the same locked seed at
// DIFFERENT player counts. Those are different games: different trump,
// different first attacker. Under the digest tiebreak the 3-player fork beat
// the real 4-player game half the time (measured 1008/2000 seeds), the two
// forks disagreed on the first attacker in 2/3 of deals, and when the full
// game's first attacker was the player stuck on the small fork's board, the
// whole table deadlocked — everyone waiting on a player whose own screen says
// someone else must open. Joins-count ordering makes every such fork resolve
// to the fullest roster, on every device, deterministically. It also orders
// WAITING chains among themselves (a 3-join lobby beats the 2-join lobby it
// grew from), which is what lets a device refresh its roster from an incoming
// join instead of coin-flipping against its own cached invite. It ranks BELOW
// turn on purpose: a chain someone has actually played on must never be
// clobbered by a stale wider Start sealed after the fact.
//
// Rule 4 ranks ABOVE all of that: a chain's own DIRECT CHILD outranks it,
// whatever the other fields say. Between a parent and its descendant the other
// rules can lie about which came later, because `turn` counts ATOMS and the
// atom stream is re-derived from the whole log on every seal - a pending good
// is an atom only until a non-good follows it (replay.c log_atom_kind). So
// "parent + good" and "parent + good + cover" seal to the SAME turn (the old
// digest tiebreak then kept the PARENT half the time), and "parent + good +
// good" seals one turn ABOVE the cover that follows and supersedes both goods
// (the turn rule then kept the parent every time). A device that kept the
// parent silently refused the very move that had just been played on it: the
// 1.0(17) live-drawer report of a board "a bit behind" until the bubble is
// closed and re-tapped (the tapped-bubble path adopts without consulting Rule
// P, which is why re-tapping always recovered). The same flip in the other
// direction adopted a parent DELIVERED AFTER its child, sending the board
// backwards - a cover that flew, landed and then vanished while the attack
// under it stood back upright. The parent link every envelope already carries
// decides this exactly: the child names its parent's digest, the parent cannot
// name its child's, and a child's phase, round and joins are always >= its
// parent's, so ranking descent first can never misorder the rules it
// overrules. Two SIBLINGS (same parent, neither an ancestor of the other) name
// neither and fall through to rules 0..3 and the digest, which for a genuine
// concurrency fork is the designed answer.
//
// Delivery order is never an input. Two devices can transiently disagree about
// which message is "newest", so the rule needs no clocks and no ordering
// guarantee from Messages — that is the whole point.
//
// In C, not in each client: this decides which game every player sees, so a
// phone and a browser disagreeing here forks the game. There is nothing to port.
typedef struct {
    uint8_t  phase;                        // MSG_PHASE_*; only "started or not" is compared
    uint8_t  round;
    uint16_t turn;
    uint8_t  n_joins;                      // rule 3: the fuller roster wins the turn-0 tie
    uint8_t  parent8[MSG_PARENT_LEN];      // rule 4: a child outranks the parent it names
    uint8_t  digest[SHA256_DIGEST_LEN];
} MsgChainKey;

// Rule P's inputs, read off an envelope's bytes. Structure only — no replay, no
// Game: comparing is cheap and happens before a device decides what to adopt.
// Returns MSG_EOK, or a negative MSG_E* if the bytes are not an envelope.
int msg_chain_key(const unsigned char *envelope, int len, MsgChainKey *out);

// <0: `a` is preferred. >0: `b`. 0: the same chain (identical digests).
int msg_rule_p(const MsgChainKey *a, const MsgChainKey *b);

// ---------- Rule R: no legal move is silently lost ------------------------
//
// When a device adopts a chain that does not contain the move it staged, that
// move is rebased onto the adopted state (§7.4). Verdicts:
#define MSG_REBASE_REAPPLY         0  // legal on the new state; APPLIED to `adopted`
#define MSG_REBASE_DISCARD_ROUND   1  // the round-boundary guard fired
#define MSG_REBASE_DISCARD_ILLEGAL 2  // the kernel refused it on the new state

// Decide ONE pending action, in order, against the adopted chain.
//
//   adopted        the game the adopted chain replayed to. On REAPPLY it is
//                  MUTATED — that is the rebase; on either DISCARD it is
//                  untouched, so a caller can keep folding the rest in.
//   adopted_round  the adopted chain's completed-bout count.
//   pending_round  the round the action was composed against.
//
// THE ROUND-BOUNDARY GUARD IS THE POINT. Without it a rebased action can be
// legal but mean something else: a throw-in composed against round 5's table,
// after the defender's pickup closed round 5, re-validates as an OPENING ATTACK
// of round 6 — legal per the kernel, and not what the player chose. Within a
// round, kernel legality is the only arbiter; across one, nothing survives.
//
// Legality is asked the only honest way — by applying it to a clone and letting
// the handler answer. No TS or Swift ever gets to have an opinion about whether
// a move is legal (§17.16: a hand-rolled "is it my turn" is a bug by policy).
int msg_rebase_one(Game *adopted, int adopted_round, int pending_round,
                   int seat, const AwireAction *a);

// SHA-256 of a whole envelope's bytes. `parent8` is the first MSG_PARENT_LEN
// bytes of the parent's digest; Rule P's tiebreak compares full digests
// lexicographically. Thin wrapper, but it names the one hash the protocol means
// so no caller has to re-decide what "the digest of a chain" is.
void msg_digest(const unsigned char *envelope, int len, uint8_t out[SHA256_DIGEST_LEN]);

// ---------- the pickup hold (round 16) -----------------------------------
//
// How many more seconds `seat` must wait before it may pick up: 0 when it may
// pick up now, 1..MSG_PICKUP_HOLD_S while the hold stands. Owner: "make it so
// that you cannot pickup within 15 seconds of the attack ... this is to give
// attackers a fair chance to throw in additional cards".
//
// `g` is the game the chain replayed to, `sent_at` its envelope's clock, `now`
// the reader's own unix seconds mod 65536. Pure: no clock is read here, because
// a kernel that read the time would answer differently on two devices holding
// the same bytes, and every other rule in this tree is a function of the bytes.
//
// THE HOLD STANDS only when all of these are true, and each one is a way the
// answer is already decided by the state:
//
//   - `sent_at` is non-zero. A format-2 chain has no clock, so there is nothing
//     to measure and nothing is held (this is the whole of backward
//     compatibility: an old bubble simply never holds).
//   - `seat` is the defender. Nobody else can pick up.
//   - the last thing in the log is an ATTACK or a PASS — the two actions that
//     leave a defender facing a fresh uncovered card. After a cover or a good,
//     the defender is not sitting in front of anything new, and a hold would
//     just be a delay. (A pass counts because the seat it lands on is in
//     exactly the attacked position, facing throw-ins, and it is a different
//     seat than the one that acted.)
//   - the defender still has SPARE CAPACITY: strictly fewer uncovered cards on
//     the table than cards in hand. Owner: "if the last attack caused the number
//     of uncovered cards to equal the number of cards remaining in the
//     defenders hand, we need no such timer, because if the defender has no more
//     capacity, no one can throw in anything regardless." Holding a defender who
//     cannot be thrown at protects nothing and costs 15 seconds.
//
// The wait itself is `MSG_PICKUP_HOLD_S - (uint16_t)(now - sent_at)`, computed
// in unsigned 16-bit so a clock rollover cancels on both sides. A stamp that
// reads as being in the FUTURE (a sender whose clock runs fast) wraps to a large
// delta and so releases the hold rather than maxing it — the safe direction, and
// the only one that cannot wedge a defender behind a stranger's bad clock.
int msg_pickup_hold_remaining(const Game *g, int seat, uint16_t sent_at, uint16_t now);

// Zero an envelope to its EMPTY state, which is not all-zero: `opening` and
// `carry_fool` mean "seat 0" when zeroed and "absent" at their sentinels, so a
// memset alone would quietly claim seat 0 opens every game.
//
// Producers must use this instead of memset. Forgetting to is caught the moment
// anything is encoded rather than at the table: an all-zero envelope carries a
// rematch claim its format cannot hold, and validate_fields refuses it
// (MSG_EFORMAT). msg_decode fills the sentinels itself, so a decoded envelope
// never needs this.
void msg_envelope_init(MsgEnvelope *e);

// ---------- Rule F: the fool's penalty -----------------------------------
//
// The two calls that decide who opens a rematch. Both are pure functions of a
// join list, and both live here rather than in a client for the reason every
// other rule in this file does: a phone and a browser that disagree have dealt
// two different games.
//
// THE ROSTER KEY. A fingerprint of `joins` as an ORDERED CYCLE - the same
// people in the same rotation hash equal, a different order does not. Seats are
// read in ascending order and the rotation whose (name_len, name) byte sequence
// compares smallest is the one hashed, with the player count folded in.
//
//   hash   receives the key. Never 0 for a valid roster (a computed 0 is bumped
//          to 1), because 0 is the wire's "no carry" sentinel.
//   rot    receives the rotation offset: canonical[k] == joins[(k + *rot) % n].
//          Pass NULL if you only want the key.
//
// Returns MSG_EOK, or MSG_EJOINS if `n` is outside 2..MSG_MAX_JOINS.
//
// A key collision costs one game opened on the wrong seat - a legal game, just
// not the punishment that was due - so 32 bits is ample; nothing here is a
// security boundary, since a device that wanted to lie would simply write the
// `opening` byte it liked.
//
// Duplicate names are the one genuinely ambiguous input: a table with two
// players called "Sam" has rotations that are byte-identical, so which "Sam"
// carry_fool names cannot be recovered. The rule then punishes one of them,
// arbitrarily but deterministically (every device picks the same one). This is
// judged acceptable: the alternative is putting stable player ids on a wire
// that has deliberately never carried them (§4.1).
int msg_roster_key(const MsgJoin *joins, int n, uint32_t *hash, int *rot);

// THE VERDICT. Given a lobby's carry (`carry_key`/`carry_fool`, straight off
// the WAITING envelope) and the roster that is about to start, returns the seat
// that must open the new game, or -1 when the rule does not apply and the deal
// should derive its opener from the lowest trump as usual.
//
// It applies when, and only when, `carry_key` is non-zero, `carry_fool` names a
// real index, and the starting roster keys EQUAL to the carry - which is the
// owner's "if the players do not change at all" guard, read through the
// rotation tolerance above. The answer is then the seat to the RIGHT of the
// fool, `(fool_seat - 1 + n) % n`, where fool_seat is carry_fool mapped through
// the starting roster's own rotation.
int msg_rematch_opening(const MsgJoin *joins, int n,
                        uint32_t carry_key, uint8_t carry_fool);

// The seat the penalty falls ON - the fool, and therefore the new game's first
// DEFENDER. Same guard and same inputs as msg_rematch_opening, and derived from
// its answer so the two can never disagree; -1 when the rule does not apply.
// This is what a lobby shows: "if nobody else joins, <name> is attacked first".
int msg_rematch_fool_seat(const MsgJoin *joins, int n,
                          uint32_t carry_key, uint8_t carry_fool);

// ---------- the chain layer's gates ----------------------------------------
//
// A CHAIN-BASED CLIENT'S FOUR SMALL DECISIONS: is this board a branch off an
// old bubble, is this nickname usable, is it taken, and which seat am I? They
// were Swift (ios/FoolishKit/Messages/{StaleBranchGate,NicknameGate,
// SeatIdentity}.swift), where the extension is the only thing that has ever
// asked them. They are here so the second chain client does not re-derive them;
// what stayed behind is the async orchestration around them, which is the
// host's.
//
// Everything here is a pure function of ints and a roster. None of it reads the
// resident game, and none of it puts seat identity into the state blob - game.h
// says seat identity "lives with the caller" and it still does: the caller
// hands its own cache, its own sender signal and its own chat shape IN.

// DOES `a` SHOW MORE OF THE GAME THAN `b`? Lexicographic over (phase, round,
// turn), STRICTLY - a tie is not ahead.
//
// ROUND is compared above TURN and not below it, because the two do not move
// together: `turn` counts ATOMS and the atom stream is re-derived on every
// seal, so a bout-closing action folds that bout's pending goods into the one
// round_end atom that replaces them, and a chain can complete a round and come
// back with the same atom count as its parent or fewer. Round is monotonic
// where turn is not, so it is asked first.
//
// IT FAILS OPEN ON PURPOSE. That same fold means a chain that is genuinely
// ahead can tie on turn within one round ("parent + good" against "parent +
// good + cover" seal to the same turn), and this answers "not ahead" there. A
// false positive is a game that cannot be played, which is far worse than the
// branch it would prevent - and the chain that is really ahead still wins the
// moment it ARRIVES, because Rule P ranks a child over its parent. A stale
// BRANCH is never in that window: it has strictly fewer atoms and loses on
// turn with nothing folded.
int msg_chain_is_ahead(int a_phase, int a_round, int a_turn,
                       int b_phase, int b_round, int b_turn);

// A NICKNAME, JUDGED. `n_chars` is the trimmed name's character count and
// `n_bytes` its UTF-8 byte count - both counted by the host, because trimming
// and grapheme clustering are Unicode work a C kernel has no business doing;
// the CAPS and their precedence are here so a client cannot drift from the one
// the seal enforces (MSG_MAX_NAME). Rejects rather than truncates, deliberately.
#define MSG_NAME_OK        0
#define MSG_NAME_EMPTY     1
#define MSG_NAME_TOO_LONG  2
int msg_nickname_verdict(int n_chars, int n_bytes);

// Is `name` already held by a seat in this roster? Names are the only identity
// a payload carries, so the seat picker, the disown check and the lobby's
// "(you)" tag all lean on them - and they can only lean as far as names are
// unique WITHIN a chain, which is what the Join button gates on. Exact match on
// the sealed bytes.
int msg_name_taken(const MsgJoin *joins, int n, const char *name, int name_len);

// WHICH SEAT AM I? The three §6 layers, highest priority first, over signals
// the caller supplies. Returns the seat, or -1 for "ambiguous" - which is not
// an error but the honest answer, and means ask the human.
//
//   1. the cache, set with certainty at create/join time. A cached seat outside
//      0..n_players is treated as absent: a stale row from another game must
//      never seat somebody out of range.
//   2. the sender signal - if THIS device sent the bubble I am its last actor,
//      exact for any n. In a 2-player game I am the OTHER seat when I did not
//      send it, but only in a DM: that inference's premise is "only two humans
//      can be holding a phone in this thread", and in a group chat a third
//      member is one tap away from being seated on somebody's face-up hand.
//   3. otherwise ambiguous.
int msg_seat_resolve(int cached_seat, int sender_is_local, int n_players,
                     int last_actor_seat, int chat_is_dm);

// The seat carrying MY OWN recorded claim name in this roster, or -1. Per-chain
// names are unique and only this device seals its own, so when a fork race
// leaves the numeric cache pointing at a claim that LOST, the winning chain
// still carries the name at whichever claim survived. `name_len` <= 0 means the
// caller has no recorded name and the scan is skipped.
int msg_seat_claimed_by_name(const MsgJoin *joins, int n,
                             const char *name, int name_len);

// Does this bubble's roster DISOWN the cached seat - list it under a different
// name than the one this device recorded when it claimed it? True only when
// both sides are present and they differ: either missing is permissive, and the
// range check and the fallbacks above still apply. It happens in exactly one
// situation, a seat-claim race this device lost, where trusting the cache would
// seat it on somebody else's hand face-up.
int msg_seat_cache_disowned(const MsgJoin *joins, int n, int cached_seat,
                            const char *name, int name_len);

// msg_seat_resolve, gated for a LOBBY bubble: a resolved seat only counts as
// mine if this bubble's OWN roster contains it. Correct for a live board, where
// every chain carries every seated player forward, and wrong for a lobby - an
// older WAITING bubble reopened after I have since joined still resolves my
// cached seat although that bubble's joins predate the join, which granted
// Start to somebody the lobby does not list. -1 covers both "ambiguous" and
// "resolved, but not in this bubble's joins".
int msg_seat_resolve_in_lobby(const MsgJoin *joins, int n_joins,
                              int cached_seat, int sender_is_local, int n_players,
                              int last_actor_seat, int chat_is_dm,
                              const char *name, int name_len);

// ---------- the turn controller, as a transition function -------------------
//
// A turn on a chain is not "make a move and pass the phone". Several seats can
// be legal at once, so the model is STAGE A CHAIN: a device establishes a base,
// applies one or more of its own actions locally, and seals the result into a
// bubble the human - never the code - presses Send on. The controller that
// drives that (ios/FoolishKit/Messages/MessageTurnController.swift) is a state
// machine over time, and its suspension points belong to the host: an actor
// hop, a decode, a paint. Those stay where they are.
//
// The DECISIONS ACROSS them are here. Every rule below is a pure function of
// facts the host already holds - is anything staged, has Send been pressed, is
// a board mounted, is a settlement withheld - and answers what to do rather
// than doing it; the host performs the effect. That is the division bot_drive.h
// already draws: the kernel says which action and how long to wait, and the
// caller decides only how to wait.
//
// WHY IT IS WORTH MOVING. Staging is where this extension's worst bugs have
// lived, and each was one rule stated in two places and drifting: a send that
// walked the board backwards, a duplicate delivery red-retracting a move nobody
// had superseded, an arrival adopted underneath the retraction meant to precede
// it. A second chain client re-deriving these from outside meets all of them
// again.

// THE CHAIN STATE, as bits. Every one is a fact about THIS DEVICE's turn, not
// about the game: the game is the state blob, and none of it is read here.
#define MSG_TURN_STAGED         (1 << 0)  // actions applied locally, not yet sent
#define MSG_TURN_SENDING        (1 << 1)  // Send was pressed; the rebase has not resolved
#define MSG_TURN_READY          (1 << 2)  // a base chain has been established once
#define MSG_TURN_SUPERSEDED     (1 << 3)  // this board branches off a chain the table left
#define MSG_TURN_RETRACTING     (1 << 4)  // a conflict retraction is in flight
#define MSG_TURN_BOARD_WATCHING (1 << 5)  // a live board is mounted to fly one
#define MSG_TURN_HELD           (1 << 6)  // a bout settlement is withheld until Send
#define MSG_TURN_GENESIS        (1 << 7)  // a dealt game with no parent chain

// Is there a staged bubble to send? Not "may I play": a send is about bytes in
// the input field, and the send window (MSG_TURN_SENDING) has already claimed
// them - an Undo pill surviving into it offers to retract what cannot be
// retracted.
int msg_turn_can_send(int state);

// MAY THIS SEAT ACT AT ALL - over the HUMAN menu, never the raw one.
//
// `n_human_moves` is the count from play_human_menu: the kernel's menu minus
// `wait` and minus `good` while an attack is still uncovered. The raw menu
// always offers GOOD, because a bot needs to say good over an uncovered attack
// to leave the eligible set (legal.c), and a human may not. A handoff counting
// the RAW menu calls it this seat's move when its only offer is a good the
// board will not let it make, and the run stops with no button on screen.
// legal.h's play_human_menu note is the other half of this sentence.
int msg_turn_can_act(int state, int n_human_moves);

// Is there a bubble to put in the input field? Either I staged one, or this is
// a fresh genesis on which I have no move at all - I dealt the game and am not
// the first attacker, so the only way it progresses is to send the deal on.
int msg_turn_can_stage(int state, int n_human_moves);

// ---- the door every gesture comes through ----------------------------------
//
// Enforced as well as displayed. The board already hides what these refuse, so
// this only fires on a path the UI does not draw - a drop gesture, the dev
// harness, a future shortcut - which is exactly why it is a rule and not a
// button state.
#define MSG_TURN_ADMIT_OK           0
// The chain on screen is being replaced; a move staged now would be composed
// against a base the latched arrival is about to supersede. SILENT: the window
// is one red flight long, and the tap simply does nothing.
#define MSG_TURN_ADMIT_RETRACTING   1
#define MSG_TURN_ADMIT_SUPERSEDED   2
#define MSG_TURN_ADMIT_HELD_PICKUP  3   // the pickup hold has not lapsed
int msg_turn_admit(int state, int move_type, int pickup_hold);

// ---- a chain that ARRIVED --------------------------------------------------
//
// `same_chain` is the host's byte comparison of the arriving payload against
// the one this board is built on.
//
// SKIP KEEPS THE STAGED MOVES. A re-delivery of the chain I am already on is
// not an arrival: those moves were composed against exactly these bytes, and
// retracting them in red because the host handed the same bubble over twice is
// a retraction with nothing to retract for.
//
// LATCH IS CHECKED BEFORE THE STAGED TEST, because a retraction has already
// emptied the staged list: without that order a burst's second arrival falls
// through to a plain adopt UNDERNEATH the retraction, and the finish then
// adopts the older latched chain on top of it - the board walking backwards one
// bubble.
#define MSG_TURN_ARRIVE_SKIP     0  // the chain I am already on - staged moves stand
#define MSG_TURN_ARRIVE_LATCH    1  // a retraction is already flying; newest wins the latch
#define MSG_TURN_ARRIVE_ADOPT    2  // fold it in now
#define MSG_TURN_ARRIVE_RETRACT  3  // fly the staged cards home first, then adopt
int msg_turn_arrival(int state, int same_chain);

// The narrower duplicate guard the ADOPT path keeps for its direct callers. It
// differs from MSG_TURN_ARRIVE_SKIP on purpose: with moves staged the resident
// game is base+pending, and re-adopting rebuilds it, which is the behaviour
// every duplicate got before the routing verdict above existed. Nothing reaches
// adopt in that shape from the routing layer any more.
int msg_turn_adopt_duplicate(int state, int same_chain);

// ---- what a send means -----------------------------------------------------
//
// WHICH BYTES WENT OUT. The host's Send signal can arrive without its payload,
// and it can arrive with a STALE one. Both were reported from a device and each
// was fixed on its own in a different round, as two layers that silently
// disagreed. They are one sentence:
//
//   WITH MOVES STAGED, OUR OWN SEALED CHAIN IS THE BUBBLE.
//   WITH NOTHING STAGED, ONLY THE HOST CAN SAY.
//
// Staged, there is exactly one bubble in the input field and this device sealed
// it, so a send signal can only be that bubble going out, whatever bytes came
// along for the ride. Unstaged there is no such claim, and that is also the
// shape a genuinely foreign signal arrives in.
//
// THE BYTES NEVER CROSS - the kernel has no opinion about two opaque blobs it
// did not write. This is the picker: three facts in, one answer out.
#define MSG_TURN_BYTES_NONE    0
#define MSG_TURN_BYTES_HOST    1
#define MSG_TURN_BYTES_SEALED  2
int msg_turn_sent_source(int staged, int have_host, int have_sealed);

// WHAT THE SEND DOES TO THIS BOARD, from those same facts plus one comparison
// the host makes on the blobs (`host_is_sealed`) and, on a second ask, whether
// the bytes decoded.
//
// Ask once with `decoded` < 0. MSG_TURN_SEND_DECODE means "these are ours to
// adopt - decode them and ask again"; every other answer is final and no decode
// is owed. That two-step keeps the host's one await where it belongs and still
// leaves the branching here.
//
// EVERY ANSWER RELEASES THE HELD SETTLEMENT. Send is the only releaser there
// is: once the host reports it, the move can no longer be undone and its bubble
// can no longer be deleted, so withholding past that point is a board no tap
// can move and no arrival can unstick. A refusal is a refusal to REBASE, never
// a refusal to release. MSG_TURN_SEND_NOOP is the exception that proves it -
// nothing was staged, so nothing is held.
#define MSG_TURN_SEND_FOREIGN     0  // not our bytes: keep the base and the staged moves
#define MSG_TURN_SEND_NOOP        1  // nothing staged and no bytes - no send of ours
#define MSG_TURN_SEND_BLIND       2  // staged, but no chain to rebase onto: keep the moves
#define MSG_TURN_SEND_DECODE      3  // decode the bytes and ask again
#define MSG_TURN_SEND_UNREADABLE  4  // they will not decode: keep the board on its staged move
#define MSG_TURN_SEND_REBASE      5  // adopt them as the new base and drop the staged moves
//
// WHY A REFUSAL KEEPS THE STAGED MOVES. They are what the board is DRAWN from,
// so dropping them while declining to rebase walks the board back by exactly
// the move the player just watched - a smaller version of the complaint the
// refusal exists to prevent. Re-deriving the sent bytes instead was rejected: a
// re-seal stamps a fresh send clock, so it would be a DIFFERENT chain with a
// digest nobody in the thread has.
int msg_turn_send_verdict(int staged, int have_host, int have_sealed,
                          int host_is_sealed, int decoded);

// ---- what is withheld ------------------------------------------------------
//
// A staged move is not a move. It sits in the input field until the human
// presses Send, and until then it can be undone - or the bubble simply deleted,
// which no amount of hiding an Undo button prevents. So a staged move must
// never TELL its player anything they could act on, and all three bout-closing
// moves DEAL: the last good owed, a cover that empties the defender's hand, and
// a pickup that refills the picker. The turn is cut at the kernel's own
// settlement boundary and the second half is withheld until Send. Recipients
// need none of this: their bubble carries the whole turn and they were never in
// a position to take it back.

// THE STEP WHOSE COMMITTED BOARD A HELD SETTLEMENT SHOWS, or -1 when this turn
// has nothing to hold. `cut` is evwire_frames_settlement_cut's answer over the
// same frames these events were decoded from; pass it < 0 for "no cut".
//
// For a `good` the cut is at 0 - a good emits no step of its own - and the
// transition step it lands on carries the PRE-discard board, because game.c
// fires ENGINE_HOOK_MAGIC_TRANSITION before anything moves. That is exactly the
// state being asked for, so index 0 is the answer rather than an error.
// Otherwise it is the state the ACTING step committed: the cover on the table,
// the table taken.
int msg_turn_hold_state(int n_events, int cut);

// ---- what a read publishes -------------------------------------------------
//
// A BOARD PUBLISHES A SNAPSHOT, SOMETIMES A DOCTORED ONE. While a settlement is
// withheld the board shows the state BEFORE it and publishes an EMPTY menu, and
// both halves matter. The view is what keeps the deal out of sight; the empty
// menu is what stops the same player acting on it anyway - a defender whose
// last cover swept the table becomes the next first attacker, and without it
// they could pick that attack out of a hand they have not been shown, with the
// whole turn still retractable. legal.h's PlayBoard note is why every board
// rule takes the published pair as an input rather than re-deriving it.
typedef struct {
    int state;                // MSG_TURN_* bits
    int base_atoms_before;    // the adopted chain's own boundary (-1 = unknown)
    int staged_atoms_before;  // the kernel's log mark for the staged turn
    int n_open_replay;        // events this open will animate (0 = not an open)
    int view_would_change;    // does the board about to be published differ from the one up
} MsgTurnRead;

typedef struct {
    int show_held_view;    // publish the withheld snapshot instead of the live board
    int empty_menu;        // publish an EMPTY menu
    int anim_atoms_before; // where the animation now on screen starts
    int raise_veil;        // hide the cards this open will move, before the first paint
} MsgTurnPublished;

// WHERE THE ANIMATION STARTS is the one arithmetic here, and getting it wrong
// is a re-run rather than a crash: too high a base drops the front of my own
// turn, too low replays the move before it. With nothing staged, what is on
// screen is the bubble I opened and that bubble states its own boundary. Once I
// have staged moves the resident game is that bubble PLUS my actions, so the
// boundary is the KERNEL's mark for them - not the adopted chain's atom count,
// which can exceed the re-derived stream once my move supersedes a pending
// good, and then lands past the end of it.
//
// THE VEIL GOES UP ONLY WHEN THE VIEW IS ABOUT TO CHANGE. The one thing that
// takes it down is the board's own view-change handler, and a host that does
// not fire that for an equal assignment would strand every veiled card - laid
// out in its slot, invisible, on a board that is otherwise correct.
void msg_turn_publish(const MsgTurnRead *in, MsgTurnPublished *out);

#endif

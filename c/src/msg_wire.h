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
//   1    1     format     2 (see THE BODY below; 1 was cut before shipping)
//   2    1     flags      bit0 fair_deal, bit1 gzip-body,
//                          bit2 = legacy (was passing_allowed in 1.0(3); tolerated
//                          on decode, never set now), bits3-7 reserved=0
//   3    1     phase      0 WAITING, 1 ACCEPT, 2 LIVE, 3 FINISHED
//   4    8     game_id    random u64, constant for the game
//   12   2     turn       u16, count of kernel actions applied
//   14   1     last_actor_seat
//   15   1     n_players  2..8
//   16   1     variant    reserved rules-variant byte, =0
//   17   1     round      completed-round counter (Rule R's guard input)
//   18   8     parent8    first 8 bytes of SHA-256(previous envelope), 0 at creation
//   26   32    seed       -> game_set_deal_seed_bytes(seed, 32)
//   58   1     n_joins
//   59   var   joins      n_joins x { u8 seat, u8 name_len<=64, name utf8 }
//   var  2     n_actions  u16, the action count the body must yield
//   var  var   body       the v6 replay code — see THE BODY
//
// THE BODY is a v6 replay code (replay.h) — the codec that already ships. It
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
#define MSG_FORMAT_V6    2   // body = a v6 replay code — the only format

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
// silently failed to seal. Owner's round-5 call: allow up to 64 bytes; the
// Swift UI separately caps at 16 characters (not this layer's job).
#define MSG_MAX_NAME     64
#define MSG_MAX_JOINS    MAX_PLAYERS
#define MSG_SEED_LEN     FOOLISH_SEED_LEN   // 32 — the ChaCha key width
#define MSG_PARENT_LEN   8
#define MSG_HEADER_LEN   59                 // through n_joins

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
#define MSG_EVARIANT    -7   // non-zero variant (no variant is defined yet)
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
    uint8_t  format;   // always MSG_FORMAT_V6
    uint8_t  flags;
    uint8_t  phase;
    uint64_t game_id;
    uint16_t turn;
    uint8_t  last_actor_seat;
    uint8_t  n_players;
    uint8_t  variant;
    uint8_t  round;
    uint8_t  parent8[MSG_PARENT_LEN];
    uint8_t  seed[MSG_SEED_LEN];
    int      n_joins;
    MsgJoin  joins[MSG_MAX_JOINS];

    // The body, borrowed and NOT owned — see the zero-copy note above.
    // `n_actions` is the action count the body must yield; for format 2 that is
    // a claim only the codec can settle, so msg_replay checks it.
    int                  n_actions;
    int                  actions_len;
    const unsigned char *actions;
} MsgEnvelope;

// THE PRODUCER. Fills in `e`'s body and the three header fields that describe
// it — `n_actions`, `turn`, `round` — for `g`, a game dealt from `e->seed` and
// played. The caller sets the rest (game_id, phase, seed, joins, parent8,
// last_actor_seat). Returns MSG_EOK or a negative MSG_E*; `body` (>= 512 B is
// ample; a full 8p game measures ~68) receives the code and `e->actions` is left
// borrowing it, so it must outlive `e`.
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
int msg_seal(MsgEnvelope *e, const Game *g, unsigned char *body, int body_cap,
             Game *scratch);

// Parse + bounds-check `in` into `out`. Returns MSG_EOK or a negative MSG_E*.
// Never reads past `in_len`, never allocates, never builds a Game. On success
// `out->actions` points into `in`.
int msg_decode(const unsigned char *in, int in_len, MsgEnvelope *out);

// 1.0(6) DIAGNOSTIC: replay codec version (5/6/7) of the last body msg_replay
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

#endif

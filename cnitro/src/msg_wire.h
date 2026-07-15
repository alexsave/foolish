// FMSG v1 ("msg wire") — the iMessage envelope. THE serverless game payload:
// an MSMessage URL carries one of these, and it holds the ENTIRE game as
// (32-byte deal seed, ordered action chain). Every device reconstructs the game
// by re-dealing from the seed and replaying the chain through the kernel, so
// there is no server and no authority — only bytes that either replay or don't.
// Spec: docs/IMESSAGE_GAME_DESIGN.md §4/§7, corrected by
// docs/IMESSAGE_IMPLEMENTATION_HANDOFF.md §3.1/§3.2 (32-byte seed,
// seat-prefixed awire frames). In C, like every other rule in this tree: the
// same bytes must mean the same game on the phone (libfoolish.a) and on the web
// (rules.wasm), and one implementation is the only way to guarantee that.
//
// Wire layout (little-endian):
//
//   off  size  field
//   0    1     magic      0xF7
//   1    1     format     1 = raw body, 2 = v6-coded body (see THE BODY below)
//   2    1     flags      bit0 fair_deal, bit1 gzip-body, bits2-7 reserved=0
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
//   59   var   joins      n_joins x { u8 seat, u8 name_len<=12, name utf8 }
//   var  2     n_actions  u16, the action count the body must yield
//   var  var   body       the chain — see THE BODY
//
// THE BODY — why there are two formats.
//
//   format 2 (PREFERRED): the body is a **v6 replay code** (replay.h), the
//   codec that already ships. It entropy-codes each action as an index into
//   that state's legal-move menu, so an action costs ~1-2 bits instead of the
//   ~34 a raw frame spends. Measured over 240 full games per size (msg_wire_
//   test): the body is 34 B at 2p, 45 B at 4p, 68 B at 8p — 8x to 18x smaller
//   than the raw chain, putting a 4-player envelope at ~208 base32 chars
//   against §4.4's 1,000-char budget.
//
//   This is the natural fit, not a trick. docs/IMESSAGE_IMPLEMENTATION_HANDOFF
//   §3.3 rules v6 out as "the continuation format" because v6 carries NO deal
//   seed: alone, it reveals the cards dealt so far but nothing about the
//   undealt stock, so two devices cannot draw identically from it. But this
//   envelope's header carries the seed. Seed (the future) + v6 code (the past)
//   is exactly the pair that makes serverless play work, and it is what §16's
//   "FMSG v2" asked for — already built, already proven at ~787k assertions,
//   already version-dispatched. v6's mid-game cut (explicit atom count) is what
//   lets it encode a turn rather than only a finished game.
//
//   format 1 (FALLBACK): the body is the raw chain, n_actions x
//   { u8 seat, awire frame } (awire.h). Kept because the v6 producer can
//   legitimately REFUSE: replay_encode_v6_from_game reads the actions out of a
//   game's logs and rejects a log buffer that overflowed (num_logs >= MAX_LOGS),
//   which happens in ~10% of full 8-player games. Raw always encodes, so the
//   protocol can never wedge on a game it cannot re-send. v1's UI caps at 4
//   players, where the overflow was never observed.
//
//   Decode dispatches on the format byte — the same shape as replay.c's v5/v6
//   dispatch. Both formats replay to the identical Game; the choice is purely
//   how many bytes the same actions cost.
//
// TWO LAYERS, deliberately separate:
//
//   msg_decode  — STRUCTURE only. Parses and bounds-checks the bytes. Builds no
//                 Game and answers no rules question. Hostile input is expected
//                 here (the payload arrives from a URL), so every field is
//                 range-checked and every length is proven against the buffer.
//                 A format-1 body is walked frame by frame here; a format-2 body
//                 is opaque to this layer (it is an entropy-coded integer — only
//                 the codec can say whether it is well-formed), so it is checked
//                 at replay instead. Nothing downstream trusts it in the meantime.
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
// is PINNED at 3 pages (cnitro/Makefile: --initial-memory == --max-memory), so
// that array would refuse to link. Frames are self-delimiting (awire_frame_len),
// so walking them on demand costs nothing and stores nothing. Consequence, and
// the one rule for callers: `actions` points INTO the decoded buffer — it must
// outlive the envelope, and an in-place encode must not overwrite the buffer it
// is still reading (msg_encode_into documents the one safe ordering).
#ifndef CNITRO_MSG_WIRE_H
#define CNITRO_MSG_WIRE_H

#include "game.h"
#include "sha256.h"
#include <stdint.h>

#define MSG_MAGIC        0xF7
#define MSG_FORMAT_RAW   1   // body = seat-prefixed awire frames (fallback)
#define MSG_FORMAT_V6    2   // body = a v6 replay code (preferred)
#define MSG_FORMAT_MAX   2

#define MSG_PHASE_WAITING  0
#define MSG_PHASE_ACCEPT   1
#define MSG_PHASE_LIVE     2
#define MSG_PHASE_FINISHED 3

#define MSG_FLAG_FAIR_DEAL 0x01
#define MSG_FLAG_GZIP      0x02

#define MSG_MAX_NAME     12
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

// Scratch needed by msg_replay for a format-2 body: replay_decode expands the
// code into a v5-shaped log stream, which is far larger than the code itself.
// Callers in wasm pass g_replay_io (WASM_REPLAY_IO_CAP); native callers can use
// a static buffer of this size.
#define MSG_REPLAY_SCRATCH 32768

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
#define MSG_ESCRATCH   -19   // scratch buffer missing or too small for a v6 body

typedef struct {
    uint8_t seat;
    uint8_t name_len;
    char    name[MSG_MAX_NAME];
} MsgJoin;

typedef struct {
    uint8_t  format;   // MSG_FORMAT_V6 unless the v6 producer refused
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

// Builds a format-2 body: the v6 replay code for `g`, a game dealt from
// `e->seed` and played. Returns bytes written to `body` (>= 0), or a negative
// MSG_E*. On MSG_EBODY the caller should fall back to a format-1 raw body — the
// v6 producer refuses a game whose log buffer overflowed, and that is a real
// (if rare) outcome, not a bug. See replay.h's replay_encode_v6_from_game.
//
// This is a thin wrapper over the ONE v6 producer, not a second encoder: the
// point is that the caller never has to know a v6 code needs a deal seed, and
// that FMSG cannot drift from the codec the rest of the product ships.
int msg_body_from_game(const MsgEnvelope *e, const Game *g,
                       unsigned char *body, int body_cap);

// Parse + bounds-check `in` into `out`. Returns MSG_EOK or a negative MSG_E*.
// Never reads past `in_len`, never allocates, never builds a Game. On success
// `out->actions` points into `in`.
int msg_decode(const unsigned char *in, int in_len, MsgEnvelope *out);

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
// `scratch` (>= MSG_REPLAY_SCRATCH bytes) is only read for a format-2 body, to
// expand the v6 code; pass 0/0 if you only ever handle format 1.
int msg_replay(const MsgEnvelope *e, Game *g, unsigned char *scratch, int scratch_cap);

// SHA-256 of a whole envelope's bytes. `parent8` is the first MSG_PARENT_LEN
// bytes of the parent's digest; Rule P's tiebreak compares full digests
// lexicographically. Thin wrapper, but it names the one hash the protocol means
// so no caller has to re-decide what "the digest of a chain" is.
void msg_digest(const unsigned char *envelope, int len, uint8_t out[SHA256_DIGEST_LEN]);

#endif

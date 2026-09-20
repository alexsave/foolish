// WMSG v1 - the iMessage envelope. THE serverless game payload: an MSMessage
// URL carries one of these and it holds the ENTIRE game as (32-byte role seed,
// ordered record chain). Every device rebuilds the game by re-dealing from the
// seed and replaying the chain through the kernel, so there is no server and no
// authority - only bytes that either replay or do not.
//
// Inherited wholesale from the fork's FMSG (c/src/msg_wire.h), including the
// header offsets where a field means the same thing, because the shape is the
// part that was expensive to get right and none of what made it right was about
// cards.
//
// CHAINS, NOT DIFFS. Every envelope carries the full record chain from the deal.
// A single bubble is therefore sufficient to adopt the game - a device never
// needs to have seen a prior message, and a lost concurrent message loses only
// its own delta. Do not ever optimize this into deltas.
//
// TWO LAYERS, deliberately separate:
//
//   ww_msg_decode - STRUCTURE only. Parses and bounds-checks. Builds no game and
//                   answers no rules question. Hostile input is EXPECTED here,
//                   because the payload arrives from a URL, so every field is
//                   range-checked and every length is proven against the buffer.
//   ww_msg_replay - SEMANTICS. Re-deals from the seed and applies each record
//                   through the public kernel entry points, exactly as a live
//                   device does. A corrupt or hand-edited chain fails here,
//                   loudly, because an illegal record simply will not apply.
//                   There is no partial recovery and no memcpy into a WwGame:
//                   validation IS replay.
//
// SIZE, MEASURED NOT GUESSED. A ten-player first night seals to 375 bytes (58
// header + 140 roster + 2 + 10 records x 4 + 3 wolf lines x 45), about 600
// base32 characters against the ~1000-character URL budget the fork measured.
// By night ten the record chain has grown to ~400 bytes of records and the total
// is ~730, which is over. The answer when that day comes is the one the fork
// already found for its own body - entropy-code the chain against the state's
// own menu of legal choices - and it is deliberately NOT built here: it needs
// real games to calibrate and the first night is what has to work first.
//
// WHAT IS NOT DONE, ON PURPOSE. The body is not padded to a constant size. It
// was considered: a longer bubble on a night when a wolf spoke is in principle
// countable. It was rejected because the hiding here is social, not
// cryptographic - the same trust level as passing the phone around a table -
// and anybody willing to measure bytes has the whole envelope in front of them
// and could read the roles out of it instead of counting. Padding would cost
// every real player URL budget to defend against an attacker who does not exist.
#ifndef WW_WIRE_H
#define WW_WIRE_H

#include "ww_game.h"
#include "sha256.h"
#include <stdint.h>

// Its own magic, not FMSG's 0xF7: a Durak bubble and a werewolf bubble must
// never be mistaken for each other by either product's decoder, and the cheapest
// place to make that impossible is the first byte.
#define WW_MAGIC   0xB6
#define WW_FORMAT  1

#define WW_PARENT_LEN   8
#define WW_MAX_JOINS    WW_MAX_PLAYERS
#define WW_NAME_MAX     16

// Header size for WW_FORMAT. A named constant because the decoder proves the
// buffer against it before reading a single field.
#define WW_HDR_LEN      61

#define WW_MSG_EOK       0
#define WW_MSG_ESHORT  (-1)   // the buffer cannot hold what it claims
#define WW_MSG_EMAGIC  (-2)
#define WW_MSG_EFORMAT (-3)
#define WW_MSG_EFIELD  (-4)   // a field out of range
#define WW_MSG_ESEED   (-5)   // an all-zero seed is never a real deal
#define WW_MSG_EREPLAY (-6)   // decoded fine, would not replay

typedef struct {
    uint8_t seat;
    uint8_t name_len;
    char    name[WW_NAME_MAX];
} WwJoin;

typedef struct {
    uint8_t  format;
    uint8_t  flags;
    uint8_t  phase;                    // WW_PHASE_*
    uint64_t game_id;
    uint16_t turn;                     // accepted records - Rule P clause 2
    uint8_t  last_actor_seat;
    uint8_t  n_players;
    uint8_t  night;
    uint8_t  round;                    // completed nights - Rule P clause 1
    uint8_t  parent8[WW_PARENT_LEN];   // first 8 bytes of SHA-256(parent bubble)
    uint8_t  seed[32];
    uint16_t sent_at;                  // unix seconds mod 65536; 0 = not stated
    uint8_t  n_joins;
    WwJoin   joins[WW_MAX_JOINS];
    uint16_t n_records;
    // THE RECORDS ARE BORROWED, not copied. A WwRecord is 45 bytes and the cap
    // is 110 of them, so decoding into an array would put 5 KB on a stack an
    // iMessage extension is already tight on. The frames are self-delimiting
    // (ww_frame_len), so walking them on demand costs nothing and stores
    // nothing. The one rule for callers: `body` points INTO the decoded buffer,
    // so that buffer must outlive the envelope, and an in-place re-encode must
    // not overwrite bytes it is still reading.
    const unsigned char *body;
    int                  body_len;
} WwEnvelope;

// Wire layout, little-endian:
//
//   off  size  field
//   0    1     magic       WW_MAGIC
//   1    1     format      WW_FORMAT
//   2    1     flags       reserved, 0
//   3    1     phase
//   4    8     game_id
//   12   2     turn
//   14   1     last_actor_seat
//   15   1     n_players
//   16   1     night
//   17   1     round
//   18   8     parent8
//   26   32    seed
//   58   2     sent_at     -- the clock ww_send_floor_remaining reads
//   60   1     n_joins
//   61   var   joins       n x { u8 seat, u8 name_len<=16, name utf8 }
//   var  2     n_records
//   var  var   body        n_records x record frame
//
// Record frame:
//   u8 seat, u8 target, u8 night, u8 flags, [u8 chat_len, chat_len bytes]
// The chat bytes are present only when WW_REC_HAS_CHAT is set, so a record
// without a wolf line costs four bytes.
//
// PAST NIGHTS' CHAT IS PRUNED AT SEAL. The wolf line is a channel for one night
// - it is display, and no kernel rule reads it - so carrying last night's lines
// forever would grow every bubble for the rest of the game to no purpose. The
// prune is deterministic (a function of the sealing game's `night`), so two
// devices sealing the same state seal the same bytes, and replay is unaffected
// because replay never consults the text. It is also a privacy win: a wolf's
// phone stops carrying a transcript of the pack.

void ww_envelope_init(WwEnvelope *e);

// Structure only. Bounds-checks every field and proves every length against
// `in_len`. Returns WW_MSG_EOK or a negative WW_MSG_E*.
int ww_msg_decode(const unsigned char *in, int in_len, WwEnvelope *out);

// Bytes ww_msg_encode would write for `e`, or -1 if it would not fit a u16
// record count. Lets a caller size a buffer without a trial encode.
int ww_msg_measure(const WwEnvelope *e);

// Serialize. Returns bytes written, or a negative WW_MSG_E* (ESHORT when
// out_cap is too small).
int ww_msg_encode(const WwEnvelope *e, unsigned char *out, int out_cap);

// SEMANTICS. Re-deal from e->seed and apply every record in `body`, in order,
// through ww_night_act / ww_day_lynch. Returns WW_MSG_EOK with `g` holding the
// game, or WW_MSG_EREPLAY with `g` left undefined - never half a game a caller
// might use.
//
// A CARRIED PASS IS REPLAYED AS ITSELF, not re-derived. The chain states which
// seats were passed for; re-deriving them from the carrier's `carry` flag would
// make replay depend on the rotation as the replaying build computes it, and
// then a rotation change would silently rewrite every game already in flight.
int ww_msg_replay(const WwEnvelope *e, WwGame *g);

// Build an envelope from a game. `game_id`, `seed` and the roster come from the
// caller because they are the game's identity and the kernel does not own them;
// everything else is read off `g`, so a sealed bubble cannot disagree with the
// state it was sealed from.
int ww_msg_seal(WwEnvelope *e, const WwGame *g, unsigned char *body, int body_cap);

// ---------------------------------------------------------------- Rule P -----
//
// The chain-preference rule, inherited from the fork intact (msg_wire.h §7.2)
// because nothing about it was about cards. Two chains for the same game are
// compared so that EVERY DEVICE COMPUTES THE SAME WINNER regardless of delivery
// order:
//
//   ancestry first: a chain's own DIRECT CHILD beats the parent it names
//   0. a STARTED chain (phase >= NIGHT) beats a lobby
//   1. higher round wins            (a resolved night is settled history)
//   2. else higher turn wins        (MORE ACCEPTED RECORDS)
//   3. else more joins wins         (the fuller roster is strictly later)
//   4. else lexicographically smaller SHA-256(envelope bytes) wins
//
// CLAUSE 2 IS WHY THE NIGHT CANNOT BE STALLED, and it is the clause this
// product leans on hardest. Two players can act for the same seat's turn - one
// really, one carried as an auto-pass by a later player. The player who carried
// a pass AND made their own move has a longer chain, so they win, whichever
// bubble happened to land last. The consequence is the property the night needs:
// the night always moves forward, and racing it cannot stall it.
//
// DELIVERY ORDER IS NEVER AN INPUT. Two devices can transiently disagree about
// which message is newest, so the rule needs no clocks and no ordering guarantee
// from Messages. That is the whole point.
//
// In C, not in each client: this decides which game every player sees, and a
// phone and another phone disagreeing here forks the game.
typedef struct {
    uint8_t  phase;
    uint8_t  round;
    uint16_t turn;
    uint8_t  n_joins;
    uint8_t  parent8[WW_PARENT_LEN];
    uint8_t  digest[SHA256_DIGEST_LEN];
} WwChainKey;

void ww_msg_digest(const unsigned char *envelope, int len, uint8_t out[SHA256_DIGEST_LEN]);

// Rule P's inputs, read off an envelope's bytes. Structure only - no replay, no
// WwGame: comparing is cheap and happens before a device decides what to adopt.
int ww_chain_key(const unsigned char *envelope, int len, WwChainKey *out);

// <0: `a` is preferred. >0: `b`. 0: the same chain (identical digests).
int ww_rule_p(const WwChainKey *a, const WwChainKey *b);

#endif

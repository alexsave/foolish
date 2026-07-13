// Whole-game replay codec — C port of the format-v5 rules projection that
// lived in supabase/functions/_shared/replay/core.ts (runReplay + menus +
// weights) and the rANS coder from codec.ts. The wire format is FROZEN:
// existing v5 integers in game_snapshots.moves must decode byte-identically,
// so every menu ordering, weight, and probability model here is wire format.
// The original TS implementation is kept as the frozen differential oracle
// (e2e/replay_ts_oracle.ts); e2e/replay_codec.test.ts polices byte-exact
// agreement between the two on kernel-played games.
//
// This lives in the kernel so the game rules the replay projection mirrors
// (can_cover, the deck-size rule, refill/rotation semantics) come from ONE
// codebase — the same game.c/card.h the production actions run — instead of
// a parallel TS engine that can drift.
#ifndef CNITRO_REPLAY_H
#define CNITRO_REPLAY_H

#include "game.h"

#define REPLAY_FORMAT_VERSION  5
// Format 6 (docs/IMESSAGE_GAME_DESIGN.md §16, option 3): the partial-game,
// hidden-state-lossless variant. Unlike v5 — which derives DRAW logs publicly
// (identity-hidden) and recovers the losers' cards by complement once the fool
// is known — v6 entropy-codes every hidden card's identity INLINE at the moment
// it is dealt or drawn (uniform over the unseen pool). Consequences: the decoded
// stream carries REAL draw/deal identities (no retrodiction guess is ever
// needed), an explicit atom count replaces "decode until fool known" so a stream
// may terminate MID-GAME (no REPLAY_EINCOMPLETE), and the deal seed is NOT
// carried (that is §16's alternative; the seed lives only in the FMSG envelope).
// v5 stays byte-frozen; v6 is purely additive with its own version byte.
#define REPLAY_FORMAT_VERSION_V6 6
#define REPLAY_VERSION_ALPHABET 16
// Hard guard: a malformed integer must never hang (mirrors core.ts MAX_ATOMS).
#define REPLAY_MAX_ATOMS 20000
// Decode-input cap: real replay integers are a few hundred bytes; anything
// bigger is hostile. Rejected with REPLAY_ECAP before any work happens.
#define REPLAY_MAX_INT_BYTES 8192

// A log's card pairs are all distinct real cards (plus hidden-draw sentinels
// capped at CARDS_PER_PLAYER), so 52 covers every reachable stream.
#define REPLAY_MAX_PAIRS 52

// 1-byte wire cards, identical to wasm/wire.h (kept here so the native
// build doesn't reach into wasm/): 0..51 = suit*13+(value-1),
// 0xFE = hidden card ({-1,-1}), 0xFF = no card.
#define REPLAY_CARD_HIDDEN 0xFEu
#define REPLAY_CARD_NONE   0xFFu

// ---------- error codes ---------------------------------------------------
// Negated return values of replay_encode/replay_decode. The TS bridge maps
// them back to the production error messages; replay_last_error_detail()
// carries the message parameter (version, menu size, ...).
#define REPLAY_EOK           0
#define REPLAY_EVERSION      1  // unsupported format version (detail = version)
#define REPLAY_ELEFTOVER     2  // leftover data after game end
#define REPLAY_ENOFOOL       3  // no single fool
#define REPLAY_EATOMS        4  // too many events
#define REPLAY_ENOMOVES      5  // no legal moves
#define REPLAY_ECONSERVATION 6  // unseen != deck + hidden
#define REPLAY_EKNOWN        7  // known card not in hand
#define REPLAY_EFRESH        8  // fresh card not unseen
#define REPLAY_EHIDDEN       9  // hidden count underflow
#define REPLAY_ENOFRESH     10  // no fresh card feasible
#define REPLAY_ENOTFEAS     11  // logged fresh card not feasible
#define REPLAY_ENOTINMENU   12  // logged action not in menu (detail = type<<16 | menu size)
#define REPLAY_EROUNDEND    13  // round end not in menu
#define REPLAY_EATTCONT     14  // attack continuation desync
#define REPLAY_EPASSCONT    15  // pass continuation desync
#define REPLAY_EINCOMPLETE  16  // logs ended before the fool was known
#define REPLAY_ELOGSAFTER   17  // logs continue after the game ended
#define REPLAY_EEMPTYMENU   18  // coder: empty menu
#define REPLAY_ECHOSEN      19  // coder: chosen index out of range (encode)
#define REPLAY_EHEADER      20  // bad header (trump not in alphabet, ...)
#define REPLAY_EINPUT       21  // malformed encode input bytes
#define REPLAY_ECAP         22  // capacity exceeded (bignum / choices / output)

// ---------- byte formats ----------------------------------------------------
//
// ENCODE input (the seat-mapped info actions; TS keeps the player_id->seat
// mapping and the GOOD+DISCARD -> round_end synthesis in encode.ts):
//   u8 n (2..8), u8 trump_id (0..51), u8 first_attacker (< n)
//   u16 LE n_actions
//   per action:
//     u8 kind   — LOG_ATTACK / LOG_COVER / LOG_PASS / LOG_PICKUP,
//                 or 0xFF = round_end marker
//     u8 seat   — acting seat for logs; 0xFF for round_end
//     u8 n_pairs (<= REPLAY_MAX_PAIRS; 0 for round_end / pickup allowed)
//     n_pairs x (u8 primary, u8 target) wire cards; info-log primaries must
//     be real cards (0..51), target is 0xFF except COVER
//
// ENCODE output: the replay integer, minimal big-endian bytes (a zero
// integer is the single byte 0x00, mirroring bigintToBytes).
//
// DECODE input: the replay integer, big-endian bytes.
//
// DECODE output:
//   u8 version, u8 n, u8 trump_id, u8 first_attacker, u8 fool
//   u16 LE discard_pile_length
//   u8 n_elim, u8 elim[8] (unused slots 0xFF)
//   u32 LE n_logs
//   per log: u8 log_type, u8 seat (0xFF = system), u8 defender_index
//            (0xFF = none), u8 n_pairs, n_pairs x (u8 primary, u8 target)
#define REPLAY_ROUND_END 0xFF
#define REPLAY_DEC_HDR   20

// Returns bytes written to out (>= 0) or -REPLAY_E* on failure. `in` and
// `out` may alias: input is fully consumed before output is written (encode
// reads actions lazily but writes the integer only at the end; decode folds
// the integer into the bignum before the log stream starts writing).
int replay_encode(const unsigned char *in, int in_len,
                  unsigned char *out, int out_cap);
int replay_decode(const unsigned char *in, int in_len,
                  unsigned char *out, int out_cap);

// ---------- Format 6 (partial-game, hidden-state-lossless) ------------------
//
// ENCODE input (v6 — v5's action stream plus the real hidden cards, since the
// caller/server holds the true deck):
//   u8 n (2..8), u8 trump_id (0..51), u8 first_attacker (< n)
//   u16 LE n_actions       — number of top-level atoms to code (may be < the
//                            full game: this is the mid-game cut point)
//   u16 LE n_reveals       — number of real hidden cards supplied, in reveal
//                            order: first n*CARDS_PER_PLAYER = the initial deal
//                            (seat-major: seat 0's cards, then seat 1's, ...),
//                            then one card per stock draw in the exact order the
//                            refill cascade pops them. The flip is never listed
//                            (it is the header trump and is drawn face-up last).
//   n_reveals x u8         — wire card ids (0..51)
//   per action (identical to v5): u8 kind, u8 seat, u8 n_pairs, pairs...
//
// ENCODE output / DECODE input: the replay integer, big-endian (as v5).
//
// DECODE output: the same header+log layout as v5, EXCEPT out[0] = 6, the log
// stream is prefixed by one LOG_DRAW per seat carrying that seat's real initial
// hand, every subsequent LOG_DRAW carries REAL card ids (never REPLAY_CARD_
// HIDDEN), and out[4] (fool) is 0xFF when the stream ends mid-game.
int replay_encode_v6(const unsigned char *in, int in_len,
                     unsigned char *out, int out_cap);

// Parameter of the last error (version for EVERSION, log_type<<16|menu size
// for ENOTINMENU, 0 otherwise).
int replay_last_error_detail(void);

#endif

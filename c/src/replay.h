// Whole-game replay codec. A game is coded as ONE mixed-radix rANS integer over
// the legal-move menus its own rules produce, so every menu ordering, weight and
// probability model in replay.c is WIRE FORMAT: change one and existing codes
// decode as different games. So are the RULES the projection replays, with less
// warning - the deal order was fixed in August 2026 and that alone retired five
// formats (docs/DEAL_ORDER.md).
//
// This lives in the kernel so the game rules the replay projection mirrors
// (can_cover, the deck-size rule, refill/rotation semantics) come from ONE
// codebase — the same game.c/card.h the production actions run — instead of
// a parallel TS engine that can drift. e2e/replay_codec.test.ts and
// c/tests/replay_v6_test.c hold the codec against real engine-played games.
#ifndef CNITRO_REPLAY_H
#define CNITRO_REPLAY_H

#include "game.h"

// THE ONE REPLAY FORMAT (docs/IMESSAGE_GAME_DESIGN.md §16, option 3): a
// partial-game, hidden-state-lossless stream. It entropy-codes every hidden
// card's identity INLINE at the moment it is dealt or drawn (uniform over the
// unseen pool). Consequences: the decoded stream carries REAL draw/deal
// identities, an explicit atom count lets a stream terminate MID-GAME, and the
// deal seed is NOT carried (the seed lives only in the FMSG envelope).
//
// WHAT THE NUMBER 10 REMEMBERS. Nine formats came before it and not one of them
// decodes any more. The history is kept because each retirement was paid for
// once and the reasons still constrain what may change here.
//
//   5  The original. DRAW logs were derived PUBLICLY (identity-hidden) and the
//      losers' cards were recovered by COMPLEMENT once the fool was known -
//      "retrodiction". A decoded hand was therefore a deduction, sometimes a
//      confident wrong one, and the stream could not stop mid-game (it decoded
//      until the fool was known) nor rebuild a playable deck.
//
//   6  Inline reveals, which is what killed retrodiction: no decoder ever
//      guesses a hand again, and the atom count replaced "decode until fool
//      known".
//
//   7  v6 plus a PASS-MODE bit written right after the version symbol -
//      1 = perevodnoy (transfer/pass allowed), 0 = podkidnoy (throw-in, no
//      pass). The bit is needed because the move MENU differs when passing is
//      off: a move is coded as an INDEX into build_top_menu's fixed-order menu
//      and the PASS options sit MID-menu, so removing them shifts every later
//      index and desyncs the whole rANS integer. A code cannot be decoded under
//      the wrong variant, so the variant must be pinned by the code itself,
//      never guessed.
//
//      SPLICED, NOT APPENDED. An earlier plan was to move the PASS block to the
//      BACK of the menu first, so every non-pass index stayed identical across
//      the two modes. That was reversed when the variant was built, and
//      re-examined in 1.0(17) when the owner asked whether the append was the
//      more elegant answer after all. It is not, and the reason is that INDEX
//      IDENTITY IS NOT AN OBSERVABLE PROPERTY OF THIS FORMAT: a code is one
//      mixed-radix rANS integer, and every step of a decode is `x mod M` where M
//      is the menu's TOTAL WEIGHT, so a decoder that had the mode wrong would
//      read a different divisor at the first state that could offer a pass and
//      scramble the remainder of the stream - under an appended menu exactly as
//      under a spliced one. The append cannot make a wrong mode fail more softly
//      OR more loudly; it buys nothing, and nothing in this tree ever compares
//      an index across modes.
//
//      What it costs is real: the FRESH pass option was offered whenever the
//      defender held any unknown card of a matching rank, so the block was
//      non-empty at very nearly every first defender decision of every bout, and
//      moving it re-points pickup, every later seat's attacks and every good in
//      all of those states - a second format renumber inside a week, or worse,
//      old codes decoding silently as different moves. Cutting the block out
//      where it stands costs nothing, and keeps this menu's convention the same
//      as `legal.c`'s, which gates the pass inside calc_pass_moves for the same
//      reason.
//
//      The redundant MSG_FLAG_PASSING_ALLOWED bit was removed from the FMSG
//      message format in 1.0(4) once the mode lived here; the envelope's
//      `variant` byte says the same thing for a LOBBY, which has no body to
//      carry a bit yet, and msg_replay checks the two against each other.
//
//   8  v7 plus a FORCED-OPENING bit, written right after the header's
//      first_attacker symbol. It exists for the fool's penalty (game.h
//      game_open_at_seat): a rematch among the same players opens on the seat to
//      the right of the last game's fool, which is NOT the seat the deal
//      derives.
//
//      Why the code has to say so, rather than the header's first_attacker alone
//      carrying it: replay_steps.c rebuilds the deal from the code's own reveals
//      and then CHECKS that the rebuilt hands derive the recorded opener. That
//      check is what catches a mis-rebuilt deal (A4), and an overridden opener
//      fails it for an entirely innocent reason. The bit says "this opener was
//      imposed, do not derive it" - and when it is set the code ALSO carries the
//      seat the deal derives (uniform over n), so the check keeps every bit of
//      its teeth rather than being switched off for rematch games.
//
//   9  and 10: THE DEAL ORDER CHANGED. Versions 5 through 8 were cut by a kernel
//      that dealt the refill wrong - it gave the defender the top of the talon
//      when a clean cover had emptied their hand, and otherwise took the
//      defender in their natural rotation slot instead of last (game.c
//      refill_player_hands). Real Durak deals the first attacker, then the table
//      clockwise, then the defender.
//
//      That is a RULES change, not a wire change, and the two new versions
//      carried byte-identical wires to the two they replaced. They existed
//      because a replay code is a game, not a document: the same bytes under the
//      fixed rules deal different cards to different seats, so a v5..v8 code
//      re-read by this kernel would silently become a game that never happened.
//      Both lines were therefore renumbered together - 9 = the format-5
//      retrodiction line, 10 = the format-8 inline-reveal line.
//
// And then the retrodiction line itself was dropped. v9 was the last format that
// hid the deal, and everything downstream had already left it behind: the FMSG
// body, replay_steps.c, the replay screen and the Oracle all need a real deck,
// which retrodicted hands are not. It had no production producer left - the web
// share refuses without a deal seed, and the phone only fell back to it for
// games it never deals - so what it actually bought was the RISK that a share
// silently encoded a different, guessed game. An honest failure beats that.
//
// So exactly one version decodes. Every code carrying 5, 6, 7, 8 or 9 - and
// every unknown future number - is REJECTED at decode with REPLAY_EVERSION and
// the version in replay_last_error_detail(), which is the whole point: a loud
// "this was played under older rules" beats a quiet fiction. Nothing bridges
// them. The fix is to play a new game, and for the tutorial's frozen code, to
// re-cut it (npx tsx tests/gen_tutorial_game.ts).
#define REPLAY_FORMAT_VERSION_V10 10
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
// Negated return values of replay_encode_v6/replay_decode. The TS bridge maps
// them back to the production error messages; replay_last_error_detail()
// carries the message parameter (version, menu size, ...).
//
// THE NUMBERS ARE THE WIRE between this file and sdk/ts/wasm/engine.ts
// (replayError) and the iOS bridge, so 3, 8 and 9 are left as a HOLE rather
// than reused. They belonged to the retrodiction line - no single fool, fresh
// card not unseen, hidden count underflow - and every one of them was a fault
// only a hidden hand could have. Renumbering to close the gap would silently
// re-point every remaining code onto a different message.
#define REPLAY_EOK           0
#define REPLAY_EVERSION      1  // unsupported format version (detail = version)
#define REPLAY_ELEFTOVER     2  // leftover data after game end
#define REPLAY_EATOMS        4  // too many events
#define REPLAY_ENOMOVES      5  // no legal moves
#define REPLAY_ECONSERVATION 6  // unseen != the face-down stock
#define REPLAY_EKNOWN        7  // played card not in hand
#define REPLAY_ENOFRESH     10  // the unseen pool is empty - nothing to reveal
#define REPLAY_ENOTFEAS     11  // a supplied reveal is not in the unseen pool
#define REPLAY_ENOTINMENU   12  // logged action not in menu (detail = type<<16 | menu size)
#define REPLAY_EROUNDEND    13  // round end not in menu
#define REPLAY_EATTCONT     14  // attack continuation desync
#define REPLAY_EPASSCONT    15  // pass continuation desync
#define REPLAY_EINCOMPLETE  16  // the action stream ran out before the coded atom count
#define REPLAY_ELOGSAFTER   17  // logs continue after the game ended
#define REPLAY_EEMPTYMENU   18  // coder: empty menu
#define REPLAY_ECHOSEN      19  // coder: chosen index out of range (encode)
#define REPLAY_EHEADER      20  // bad header (trump not in alphabet, ...)
#define REPLAY_EINPUT       21  // malformed encode input bytes
#define REPLAY_ECAP         22  // capacity exceeded (bignum / choices / output)
// A REFUSAL, not a fault: the game outran the log buffer (num_logs hit
// MAX_LOGS), so the recorded stream is truncated and there is no honest code to
// emit. Its own number because every caller wants to SKIP it and fail on the
// rest, and because folding it into EINPUT told the reader the encoder had
// been handed garbage bytes, which is the one thing that had not happened.
#define REPLAY_ETOOLONG     23  // session log overflowed - the game has no code

// ---------- byte formats ----------------------------------------------------
//
// ENCODE input (the seat-mapped info actions plus the real hidden cards, since
// the caller holds the true deck; the player_id->seat mapping and the
// GOOD+DISCARD -> round_end synthesis stay caller-side):
//   u8 n (2..8), u8 trump_id (0..51), u8 first_attacker (< n)
//   u16 LE n_actions       - number of top-level atoms to code (may be < the
//                            full game: this is the mid-game cut point)
//   u16 LE n_reveals       - number of real hidden cards supplied, in reveal
//                            order: first n*CARDS_PER_PLAYER = the initial deal
//                            (seat-major: seat 0's cards, then seat 1's, ...),
//                            then one card per stock draw in the exact order the
//                            refill cascade pops them. The flip is never listed
//                            (it is the header trump and is drawn face-up last).
//   n_reveals x u8         - wire card ids (0..51)
//   per action:
//     u8 kind   — LOG_ATTACK / LOG_COVER / LOG_PASS / LOG_PICKUP / LOG_GOOD,
//                 or 0xFF = round_end marker
//     u8 seat   — acting seat for logs; 0xFF for round_end
//     u8 n_pairs (<= REPLAY_MAX_PAIRS; 0 for round_end / pickup / good allowed)
//     n_pairs x (u8 primary, u8 target) wire cards; info-log primaries must
//     be real cards (0..51), target is 0xFF except COVER
//
// ENCODE output: the replay integer, minimal big-endian bytes (a zero
// integer is the single byte 0x00, mirroring bigintToBytes).
//
// DECODE input: the replay integer, big-endian bytes.
//
// DECODE output:
//   u8 version, u8 n, u8 trump_id, u8 first_attacker, u8 fool (0xFF when the
//                                  stream is a mid-game cut)
//   u16 LE discard_pile_length
//   u8 n_elim, u8 elim[8] (unused slots 0xFF)
//   u32 LE n_logs
//   per log: u8 log_type, u8 seat (0xFF = system), u8 defender_index
//            (0xFF = none), u8 n_pairs, n_pairs x (u8 primary, u8 target)
// The log stream is prefixed by one LOG_DRAW per seat carrying that seat's real
// initial hand, and every later LOG_DRAW carries REAL card ids (never
// REPLAY_CARD_HIDDEN) - the format is hidden-state-lossless.
#define REPLAY_ROUND_END 0xFF
#define REPLAY_DEC_HDR   20

// Returns bytes written to out (>= 0) or -REPLAY_E* on failure. `in` and
// `out` may alias: input is fully consumed before output is written (encode
// reads actions lazily but writes the integer only at the end; decode folds
// the integer into the bignum before the log stream starts writing).
//
// The `_v6` in the name is the DECODER FAMILY, not the version byte: this
// encoder stamps 10 and every entry point that reads one is named _v6 because
// it accepts the whole inline-reveal line (msg_wire.h says the same).
int replay_encode_v6(const unsigned char *in, int in_len,
                     unsigned char *out, int out_cap);
int replay_decode(const unsigned char *in, int in_len,
                  unsigned char *out, int out_cap);

// ---------- Format 6 from a played game (the one v6 producer) ---------------
//
// Encode `g` — a game DEALT FROM `seed` — as a v6 replay. This is the whole of
// what the server's finalize choreography used to do across three TS modules
// and two wasm round-trips (reconstructSeededDeal + collectV6 + marshalInputV6
// + encodeReplayV6), and it is what lets an offline host produce a v6 share at
// all: assembling the reveal stream is the only reason v6 needed a server.
//
// WHY THE SEED IS A PARAMETER, and not read back off `g`. A finished Game
// cannot answer "what was dealt": deal_initial (game.c) emits no per-seat
// LOG_DRAW — only a hook snapshot — and draw_card splices each drawn card out
// of deck[], so by game end the deal is gone from the struct and deck_count is
// 0. The seed reproduces it exactly, and nothing else in the struct can. (The
// seed is deliberately NOT in the Game or the durable blob: it stays in a
// server-only column, away from the client boundary — see game.c's deal notes.)
//
// From `g` come the ACTIONS (its logs, read directly — no marshalling) and the
// player count; from the re-deal come the true initial hands, the stock draw
// order, and the trump.
//
//   g         a game dealt from `seed` and played; its logs are the action
//             stream. Rejected with REPLAY_ETOOLONG if the log buffer
//             overflowed (num_logs >= MAX_LOGS): a truncated stream is
//             untrusted, not encodable. Callers SKIP that one; it is the
//             build's documented ceiling, not an encoder fault.
//   seed      the FOOLISH_SEED_LEN-byte deal seed this game was dealt from. A
//             seed that did not deal `g` does not encode: while the game still
//             holds a trump it is caught up front (REPLAY_EHEADER), and
//             otherwise downstream, where the logged actions do not fit the
//             wrong deal's menus (REPLAY_ENOTINMENU).
//   max_atoms cap on top-level atoms — v6's legal mid-game cut. Pass a huge
//             value (INT_MAX) for the whole game.
//
// Safe to call on a live, in-progress game and on a host with a snapshot hook
// installed: the scratch re-deal restores both the deal RNG and the hook.
//
// Returns bytes written to `out` (>= 0) or -REPLAY_E* on failure.
int replay_encode_v6_from_game(const Game *g, const unsigned char *seed, int seed_len,
                               int max_atoms, unsigned char *out, int out_cap);

// The opening attacker, read off a played game's logs: the seat of the first
// LOG_ATTACK, or -1 if none was logged. NOT g->first_attacker, which is set at
// the deal but REASSIGNED every bout, so a finished game holds the LAST round's
// attacker - which, when a game's last attacker was not its first, put the
// opening ATTACK log outside the menu the decoder builds and killed the encode
// on step 0. Shared so the two producers cannot drift on it.
int replay_first_attacker_from_logs(const GameLog *logs, int num_logs);

// How many ATOMS of this game's encoding come from logs before `cut_log` - the
// boundary a bubble reports as its own start (msg_wire.h's n_new).
//
// It has to be asked of the encoder, because THE ATOM STREAM IS NOT APPEND-ONLY
// while the log is. A GOOD is an atom only while it is still pending at the end
// of the stream (see log_atom_kind): play anything after it and it stops being
// one - it is dead state the decoder reconstructs for free - so the SAME log
// prefix encodes to one atom FEWER than it did before the next action landed.
// Subtracting two atom counts therefore under-measures a turn by exactly the
// goods it superseded, and a bubble that measured itself that way described
// only its own tail.
//
// `num_logs` is the WHOLE log, not `cut_log`: which logs are atoms is a
// question about the finished stream, and answering it from the prefix alone
// would count the very goods this exists to drop.
int replay_atoms_before_log(const GameLog *logs, int num_logs, int cut_log);

// RFC 4648 base32, no padding, the web's codec.ts alphabet: how a replay
// integer travels as text. Decode accepts lower case, ignores characters
// outside the alphabet and stops at '-' (where a share link's extras suffix
// begins); encode writes a NUL-terminated string. Both return the count
// written, or -1 when `cap` is too small. One implementation: the iOS bridge
// and the analyser both read codes through this.
int replay_b32_decode(const char *s, unsigned char *out, int cap);
int replay_b32_encode(const unsigned char *in, int n, char *out, int cap);

// Parameter of the last error (version for EVERSION, log_type<<16|menu size
// for ENOTINMENU, 0 otherwise).
int replay_last_error_detail(void);

// ---------- the v6 atom stream (A5) -----------------------------------------
//
// What a v6 code actually says, as the decoder understands it. This exists
// because replay_decode's OUTPUT cannot be read back as moves: it is a game
// LOG stream, with derived records (DEFENDER_CHANGE, PLAYER_OUT, DISCARD)
// folded in among the real ones, and a round end is genuinely NOT recoverable
// from it —
//   * LOG_DISCARD does not mean "round end": a cover that empties the
//     defender's hand discards too (apply_cover's clean-sweep branch), and
//   * LOG_GOOD does not mean "round end" either: a round whose every attacker
//     is already out emits no GOOD at all.
// The decoder knows each atom's kind for certain, so it reports it rather than
// leaving a reader to infer it. replay_steps.c rebuilds a real Game from this.
//
// DEAL/DRAW carry REAL cards (v6 is hidden-state-lossless); DRAW atoms arrive
// in the exact order the refill cascade pops them, which is what makes the
// stream a deck. The trump appears as the LAST DRAW when the stock runs dry
// and the flip itself is taken (game.c draw_card's has_flipped branch).
#define REPLAY_ATOM_DEAL      0  // `seat`'s initial hand, ascending
#define REPLAY_ATOM_DRAW      1  // one refill's cards, in pop order
#define REPLAY_ATOM_ATTACK    2
#define REPLAY_ATOM_COVER     3  // `target` = the attack card being covered
#define REPLAY_ATOM_PASS      4
#define REPLAY_ATOM_PICKUP    5
#define REPLAY_ATOM_ROUND_END 6  // the good that closed the bout: the rest
                                 // of the IN attackers said good, then discard
#define REPLAY_ATOM_GOOD      7  // `seat` said good and the bout stayed open

typedef struct {
    int  kind;                     // REPLAY_ATOM_*
    int  seat;                     // acting/receiving seat, or -1
    Card cards[REPLAY_MAX_PAIRS];
    int  n_cards;
    Card target;                   // COVER only
} ReplayAtom;

typedef void (*ReplayAtomSink)(void *ctx, const ReplayAtom *a);

// The decode header, as fields instead of the 20 packed bytes.
typedef struct {
    int version, n, trump_id, first_attacker;
    // 1 perevodnoy / 0 podkidnoy — the rules this code was cut under, and the
    // menu it was cut against. A host rebuilding a playable game from a code
    // must stamp it onto the Game (GAME_RULE_NO_PASS) or the board will offer a
    // transfer the chain cannot contain.
    int pass_allowed;
    // The forced opening (the fool's penalty). `forced_opening` = 1 when
    // first_attacker was IMPOSED rather than derived, and then
    // `derived_opening` is the lowest-trump seat the deal produces, kept so a
    // rebuilt deal can still be checked. 0 / -1 on an ordinary game.
    int forced_opening;
    int derived_opening;
    int fool;            // -1 when the stream is a mid-game cut
    int discard_count;
    int num_eliminated;
    int elim[MAX_PLAYERS];   // -1 past num_eliminated
} ReplayHeader;

// Decode a code for its atoms. Same decode as replay_decode (same coder, same
// model - the menus ARE the probability model, so there is no cheaper way in),
// reporting each atom as it resolves. `hdr` optional. Costs no log buffer - see
// decode_impl.
// Returns REPLAY_EOK (0) or -REPLAY_E*.
int replay_decode_atoms_v6(const unsigned char *in, int in_len,
                           ReplayHeader *hdr, ReplayAtomSink sink, void *ctx);

#endif

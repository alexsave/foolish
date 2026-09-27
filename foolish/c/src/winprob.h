// The win-probability strip: who was winning a finished game, and when, for
// every seat at every move.
//
// docs/POST_GAME_ANALYSER.md § The win-probability strip is the document. The
// post-game analyser (analyse.h) answers "was THIS move a mistake" and carries
// a running win probability as a by-product of the analysed seat's own column.
// This answers the other question - "who was winning, and when" - which is a
// different shape of work: one board per STEP rather than one per decision, no
// candidates, and every seat scored from the same playout.
//
// It is built out of the analyser's own pieces rather than a second copy of
// them: the real rebuild (replay_steps.h), analyse_belief, analyse_sample_world
// and analyse_install_world, and analyse_playout_board.
//
// TWO VIEWS, because "chance of winning" is two different questions.
//
//   TRUTH   The position as it really was. A replay code is hidden-state-
//           lossless, so the rebuilt board holds every hidden hand and the real
//           remaining stock, and a playout from it IS the true position handed
//           to bots. One playout scores every seat (exactly one of them is the
//           fool), so the seats' fool probabilities sum to 1 at every step and
//           the curves are commensurable. This is the eval bar.
//
//   BELIEF  The same number computed inside each seat's own information set:
//           what that seat could honestly know at that moment (analyse_belief -
//           own hand, table, discard, flip, watched pickups; no inference from
//           behaviour). Worlds are sampled from that, installed and played out.
//           These sum to nothing, and that is the point: eight seats hold eight
//           different pictures of one board, and the gap between a seat's two
//           curves is what it could not see.
//
// WHAT IT CANNOT SEE is the analyser document's section, in full, plus one
// thing more: this has no proof region at all. It never solves, so every
// number is a frequency against the chosen engine's play - including in the
// endgame, where the analyser would have proved it.
//
// The result crosses as packed bytes, reader beside writer below, in the shape
// of analyse.h and the fio_*_packed entries. Every record is FIXED STRIDE and
// the strides are in the header, so a consumer seeks to step i rather than
// walking to it.
#ifndef CNITRO_WINPROB_H
#define CNITRO_WINPROB_H

#include <stdint.h>

#include "game.h"

// ---------- errors ----------------------------------------------------------
// Negated return values of winprob_packed.
#define WINPROB_EOK      0
#define WINPROB_EBADARG  1   // null pointer, bad roster index, worlds < 1
#define WINPROB_EREPLAY  2   // the code did not decode or rebuild (winprob_last_replay_error)
#define WINPROB_ECAP     3   // output buffer too small
#define WINPROB_ETRUNC   4   // reader: the packed bytes end early
#define WINPROB_ELAYOUT  5   // the writer and the strides the reader seeks by
                             // disagree - a layout bug, never a bad input

// ---------- parameters ------------------------------------------------------
typedef struct {
    int      roster_idx;      // bot_roster index; this brain plays EVERY seat in a playout
    int      worlds;          // truth playouts per step (0 = skip the truth view)
    int      belief_worlds;   // belief worlds per seat per step (0 = skip the belief view)
    uint32_t seed;            // world sampling and playout seed base
    int      threads;         // playouts of one step across threads; 0 or 1 = this thread.
                              // The result is the same at any count.
    // The EXTRAS half of a foolish.cards link - what follows the dash - or
    // NULL. The seat names ride there, not in the moves half, so a caller with
    // only a bare code gets empty names. Nothing here invents "P1": a consumer
    // that wants a fallback knows its own locale's word for it.
    const char *names_b32;
} WinprobParams;

void winprob_params_default(WinprobParams *p);

// ---------- the entry -------------------------------------------------------
//
// Measure the game `code` (a replay integer, big-endian bytes, as replay_decode
// takes it; the current version byte is 10 and `replay_deal_v6` is the decoder
// family that reads it) and write the packed result. Returns bytes written
// (> 0) or -WINPROB_E*. On -WINPROB_EREPLAY, winprob_last_replay_error() is the
// -REPLAY_E* the decode or rebuild returned.
//
// `progress` (may be NULL) is called after each step with (ctx, step index,
// steps in all) so a caller can draw a bar; it must not touch the result.
typedef void (*WinprobProgress)(void *ctx, int done, int total);
int winprob_packed(const unsigned char *code, int code_len, const WinprobParams *p,
                   WinprobProgress progress, void *progress_ctx,
                   unsigned char *out, int out_cap);
int winprob_last_replay_error(void);

// The upper bound on winprob_packed's output for a game of `n_steps` steps and
// `n_players` seats, so a caller can size a buffer without guessing.
int winprob_packed_bound(int n_players, int n_steps);

// ---------- the wire --------------------------------------------------------
//
// All integers little-endian. Probabilities are x10000 and finish positions
// x1000, so nothing floating crosses. A name is UTF-8 and NOT NUL-terminated.
//
//   header (header_bytes long)
//     u8  version          WINPROB_WIRE_VERSION
//     u8  n_players        2..MAX_PLAYERS
//     u8  code_version     the replay format version byte the code was cut under
//     u8  trump_suit       0..3
//     u8  fool             the seat that lost, or 0xFF for a code cut mid-game
//     u8  roster_idx       the engine that played every seat of every playout
//     u8  flags            WINPROB_F_*
//     u8  pad              0
//     u16 n_steps
//     u16 header_bytes     byte offset of step 0
//     u16 step_bytes       stride of one step record
//     u16 worlds           truth playouts per step, as asked for
//     u16 belief_worlds    belief worlds per seat per step, as asked for
//     u16 pad2             0
//     u32 playouts         playouts actually run
//     u32 elapsed_ms
//     u8  elim[n_players]  seats in the order they went out; 0xFF past the end
//     n_players x { u8 len; u8 text[WINPROB_NAME_MAX] }    the seat names
//
//   step record (step_bytes long, n_steps of them)
//     u16 move             the recorded action this position FOLLOWS, or
//                          0xFFFF for the position before the first move
//     u8  seat             who played it, 0xFF for the opening position
//     u8  kind             REPLAY_ATOM_* of that action, 0xFF for the opening
//     u8  n_cards          cards in that action, capped at WINPROB_MOVE_CARDS
//     u8  cards[WINPROB_MOVE_CARDS]   card ids, 0xFF where absent
//     u8  target           COVER only: the card covered; 0xFF otherwise
//     u8  deck             stock cards left, capped at 255
//     u16 n_truth          truth playouts folded here (0 = the view is absent
//                          or the board was already finished)
//     u16 n_belief         belief worlds folded per measured seat
//     n_players x u16 truth_fool     P(this seat is the fool) x10000, or
//                                    WINPROB_NONE where it was not measured
//     n_players x u16 truth_mean     mean finish position x1000, 1 = first out
//     n_players x u16 belief_fool    the same two, from that seat's own belief;
//     n_players x u16 belief_mean    WINPROB_NONE where the seat is out or its
//                                    belief broke conservation
//     n_players x u8  hand           cards in hand, 0xFF once the seat is out
//
// A card id is suit * 13 + (value - 1), which is what main_analyse.c prints.
#define WINPROB_WIRE_VERSION 1
#define WINPROB_NAME_MAX     48   // the replay extras' own name budget
#define WINPROB_MOVE_CARDS   6    // cards of a move kept for a label; a bigger
                                  // throw-in is reported by n_cards and cut here
#define WINPROB_NONE         0xFFFFu   // "not measured", in any x10000 / x1000 field

// header flags
#define WINPROB_F_TRUTH   0x01   // the truth view was measured
#define WINPROB_F_BELIEF  0x02   // the belief view was measured
#define WINPROB_F_BELIEF_FAIL 0x04  // some seat's belief broke conservation; those
                                    // seats carry WINPROB_NONE at those steps

// ---------- the reader ------------------------------------------------------
// Nothing is ever read past `len`; every truncation is -WINPROB_ETRUNC rather
// than a shorter answer.

typedef struct {
    uint8_t  version, n_players, code_version, trump_suit, fool, roster_idx, flags;
    uint16_t n_steps, header_bytes, step_bytes, worlds, belief_worlds;
    uint32_t playouts, elapsed_ms;
    uint8_t  elim[MAX_PLAYERS];
    uint8_t  name_len[MAX_PLAYERS];
    char     name[MAX_PLAYERS][WINPROB_NAME_MAX + 1];   // NUL-terminated for a C caller
} WinprobHeader;

typedef struct {
    uint16_t move;
    uint8_t  seat, kind, n_cards, target, deck;
    uint8_t  cards[WINPROB_MOVE_CARDS];
    uint16_t n_truth, n_belief;
    uint16_t truth_fool[MAX_PLAYERS], truth_mean[MAX_PLAYERS];
    uint16_t belief_fool[MAX_PLAYERS], belief_mean[MAX_PLAYERS];
    uint8_t  hand[MAX_PLAYERS];
} WinprobStep;

// Read the header. Returns header_bytes (> 0) or -WINPROB_E*.
int winprob_read_header(const unsigned char *buf, int len, WinprobHeader *h);

// Read step `i` (0 .. h->n_steps-1). Returns step_bytes (> 0) or -WINPROB_E*.
int winprob_read_step(const unsigned char *buf, int len, const WinprobHeader *h,
                      int i, WinprobStep *out);

#endif

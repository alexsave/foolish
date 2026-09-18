// Per-viewer masked state serialization — the "you only see your own hand"
// rule, computed in the kernel instead of the TS layer (see
// docs/PACKED_WIRE_CUTOVER.md). Also single-sources the plain put_state /
// get_state byte layout that wasm_api.c and wasm_guards_api.c used to
// duplicate: both bridges now call state_put/state_get, so the wire layout
// has exactly one implementation.
#ifndef CNITRO_VIEW_H
#define CNITRO_VIEW_H

#include "game.h"

// `viewer` argument for state_put:
//   VIEW_UNMASKED  — trusted serialization, every card real (the layout the
//                    state codec / transient IO marshal always used)
//   VIEW_SPECTATOR — mask every hand and the deck
//   0..7           — mask everything except this seat's hand
#define VIEW_UNMASKED  (-2)
#define VIEW_SPECTATOR (-1)

// Leading byte of the masked view blob (wasm_view_serialize): bump on any
// layout change, same discipline as STATE_FORMAT_VERSION.
#define VIEW_FORMAT_VERSION 1

// Serialize g into the put_state layout (see wasm_api.c for the field-by-
// field doc). Masked entries (deck cards, non-viewer hands) are emitted as
// WIRE_CARD_HIDDEN with counts preserved; non-viewer awaiting_attack is
// forced to 0 (private turn state — PublicPlayer never carried it).
// Returns bytes written.
int state_put(const Game *g, int viewer, unsigned char *out);

// Parse the layout back into g, WITHOUT judging it - an import goes through
// state_import below. Counts clamp to their array capacity (memory safety on a
// hostile buffer) and a clamp is reported as GAME_INVALID_COUNT; a card byte
// that is not a card decodes to the {-1,-1} not-a-card. masked=1 additionally
// decodes WIRE_CARD_HIDDEN deck and hand cards to the {0,1} placeholder - the
// same placeholder the browser marshal always used for redacted cards, so a
// client importing a masked view gets a kernel state byte-identical to one
// marshaled from the host's own PersonalGame.
int state_get(Game *g, const unsigned char *p, int masked);

// The exact byte length of the state_put payload at p, reading no byte at or
// past p + len, or -1: a count past its capacity (a player count, the deck, the
// battles, a hand, the eliminations) or a payload that runs off the end. For a
// reader of bytes off the network, which state_get (it trusts its caller for the
// length) must never see unmeasured.
int state_measure(const unsigned char *p, int len);

// THE import: decode the layout and adopt it into `g` only if it is valid
// (game.h game_validate). Returns GAME_VALID, or a negative GAME_INVALID_*
// reason with `g` left exactly as it was. Every path that takes a state from
// outside the kernel - the transient IO marshal, the durable blob, a masked
// view on a client - goes through this rather than state_get.
int state_import(Game *g, const unsigned char *p, int masked);

// One kernel log record in the export layout the session log is built from:
//   u8 log_type, u8 player seat (0xFF system), u8 defender_index (0xFF none),
//   u8 num_pairs, num_pairs x (u8 primary, u8 target)   wire cards
// With `mask_draws`, THE DRAW-PRIVACY RULE: a drawn card's identity is written
// as WIRE_CARD_HIDDEN, except the face-up trump when it was drawn by this action
// (`pre_has_flip` and the game no longer has one, `pre_flip` being the trump
// that was up before the action began) - that draw is public. Returns bytes
// written (4 + 2 x num_pairs; the caller sizes the buffer).
int log_record_put(const GameLog *l, int mask_draws, int pre_has_flip, Card pre_flip,
                   int has_flipped_now, unsigned char *out);

#endif

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

// Parse the layout back into g. masked=0 reproduces the legacy get_state
// exactly (hostile bytes clamp to real cards; defense-in-depth count clamps).
// masked=1 additionally decodes WIRE_CARD_HIDDEN state cards to the {0,1}
// placeholder — the same placeholder the browser marshal always used for
// redacted cards, so a client importing a masked view gets a kernel state
// byte-identical to one marshaled from the PersonalGame JSON.
void state_get(Game *g, const unsigned char *p, int masked);

#endif

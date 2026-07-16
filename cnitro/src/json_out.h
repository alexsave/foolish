// JSON emission — for the hosts that want objects, not bytes.
//
// The kernel's native tongue is packed bytes: view.c's state_put/state_get and
// evwire.c's serializer are the formats, and they are the formats exactly once.
// But a host renders objects, and every host that has ever had to turn these
// bytes into objects has done it by hand-writing the layout a second time —
// iOS nearly shipped `BoardDiff.swift`, and the web DID ship
// `@shared/wire/{view,evwire}.ts`, ~215 lines of byte offsets kept in step with
// this directory by a parity test. That is the duplication F7/A8 exists to end.
//
// So: one JSON emitter, in C, next to the codecs it describes. iOS drove it
// first (ios_api.c, §16.A2); the web now decodes through the same functions via
// wasm. A format change lands here, once, and every host follows — which is the
// whole point of the exercise and the thing a parity test can never buy you.
//
// SHAPE CONTRACT. The emitted objects are deliberately *raw*: ints where the
// kernel has ints, seats where the kernel has seats, `null` where a card is
// masked. They are NOT the host's view model. Identity (player_id, name, is_ai),
// good-order, timestamps and message prose are NOT here and never will be —
// game.h is explicit that seat identity "is deliberately not in the state blob;
// it lives with the caller", so the roster join stays host-side. That split is
// not a shortcoming of this file, it is the boundary: the kernel says what the
// board IS, the host says who the seats ARE and how to draw them.
#ifndef CNITRO_JSON_OUT_H
#define CNITRO_JSON_OUT_H

#include "game.h"

// Negative returns. These match ios_api.h's FIO_* values on the nose (checked
// by a _Static_assert in ios_api.c) so the iOS bridge can hand them straight
// back without a translation table that could drift.
#define JSON_EBADARG -1   // null/short buffer, or a viewer seat off the board
#define JSON_ECAP    -3   // output buffer too small; nothing usable was written
#define JSON_EPARSE  -4   // the blob did not decode to a well-formed board

// A bounded JSON string builder. `w` counts bytes written; every append checks
// the cap and, on overflow, clears `ok` so the caller returns JSON_ECAP rather
// than emitting truncated JSON that would parse as something else. The buffer is
// NUL-terminated whenever there is room. No allocation, anywhere.
typedef struct { char *buf; int cap; int w; int ok; } J;

void j_init(J *j, char *buf, int cap);
void j_putc(J *j, char c);
void j_puts(J *j, const char *s);
void j_puti(J *j, long v);
void j_putstr(J *j, const char *s);   // JSON-escaped
void j_card(J *j, Card c);            // {"s":<suit>,"v":<value>}
int  j_finish(J *j);                  // bytes written, or JSON_ECAP

// One viewer-masked board, written into an EXISTING writer so it can nest
// inside a larger document (the event stream carries one per step).
void json_state(J *j, const Game *g, int viewer);

// The standalone-buffer form: one masked board as a whole JSON document.
int json_state_of(const Game *g, int viewer, char *out, int cap);

// Decode a packed masked view blob (view.c's state_get layout — the bytes the
// server's player_views/spectator_views wire carries) and emit it as JSON.
// `viewer` is the seat whose hand is real in this blob, or VIEW_SPECTATOR.
// Reads `buf` only; touches no resident game.
int json_view_from_packed(const unsigned char *buf, int len, int viewer,
                          char *out, int cap);

// Decode a packed evwire sequence (evwire.h's layout — what the server
// broadcasts and what evwire_serialize writes) and emit it as JSON:
//
//   {"viewer":<seat|-1>,"actor":<seat|-1>,
//    "events":[{"type":..,"seat":..,"msg":..,"from":..,"to":..,
//               "cards":[..],["target":..],["battle":..],"state":{..}}, ..],
//    "game":{..}}
//
// The reader for the format evwire.c writes, sitting beside the writer — which
// is the only arrangement in which "the format" is a single thing. Returns bytes
// written, JSON_ECAP, or JSON_EPARSE on a truncated/foreign payload (never a
// partial parse: a corrupt sequence is unreadable, not empty).
int json_events_from_packed(const unsigned char *buf, int len,
                            char *out, int cap);

#endif

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
// layout change, same discipline as STATE_BLOB_FORMAT below.
#define VIEW_FORMAT_VERSION 1

// Serialize g into the put_state layout (see wasm_api.c for the field-by-
// field doc). Masked entries (deck cards, non-viewer hands) are emitted as
// WIRE_CARD_HIDDEN with counts preserved; non-viewer awaiting_attack is
// forced to 0 (private turn state — PublicPlayer never carried it).
// Returns bytes written.
int state_put(const Game *g, int viewer, unsigned char *out);

// Parse the layout back into g, WITHOUT judging it - an import goes through
// state_import below.
// `len` is the payload's BYTE COUNT, never a buffer capacity.
// The bytes at p must be exactly one state and nothing else, and state_measure
// below decides that before a single field is read: a payload that does not
// measure to exactly `len` is refused as GAME_INVALID_COUNT, and no byte at or
// past p + len is ever touched.
// Counts still clamp to their array capacity behind that, belt and braces, and
// a clamp is reported as GAME_INVALID_COUNT; a card byte that is not a card
// decodes to the {-1,-1} not-a-card.
// masked=1 additionally decodes WIRE_CARD_HIDDEN deck and hand cards to the
// {0,1} placeholder - the same placeholder the browser marshal always used for
// redacted cards, so a client importing a masked view gets a kernel state
// byte-identical to one marshaled from the host's own PersonalGame.
int state_get(Game *g, const unsigned char *p, int len, int masked);

// The exact byte length of the state_put payload at p, reading no byte at or
// past p + len, or -1: a count past its capacity (a player count, the deck, the
// battles, a hand, the eliminations) or a payload that runs off the end.
// This is what turns a pointer into a bounded read, and state_get calls it on
// every single decode - so no caller can be the one that forgets, and a new
// caller cannot reintroduce the over-read by not knowing it had to.
int state_measure(const unsigned char *p, int len);

// THE import: decode the layout and adopt it into `g` only if it is valid
// (game.h game_validate). Returns GAME_VALID, or a negative GAME_INVALID_*
// reason with `g` left exactly as it was. Every path that takes a state from
// outside the kernel - the transient IO marshal, the durable blob, a masked
// view on a client - goes through this rather than state_get, and hands over
// `len`: how many bytes it actually holds, not how big its buffer is.
int state_import(Game *g, const unsigned char *p, int len, int masked);

// ---------- the durable state blob ----------------------------------------
//
// state_put/state_get above are the TRANSIENT request-scoped IO format: they
// never outlive one edge-function call, so they carry no version. The pair
// below is the ONLY state format written to durable storage (games.state
// bytea). It is state_put's exact byte layout with a leading 1-byte format
// version, so a future kernel-layout change becomes an explicit decode branch
// here instead of silently misreading every persisted game - the same
// discipline the replay codec (replay.h v2..v5) already applies to its
// persisted integers.
//
// It carries the VOLATILE game state only (positions, deck, battles, per-seat
// hands/status, good-mask, elimination). Seat identity (player_id/name/
// strategy_key/is_ai) is stable across a game and lives in a separate roster
// column, reattached by the table layer.
//
// Layout: [version][deterministic_deck flag][state_put(VIEW_UNMASKED)]. The
// flag byte (added with the seed-dealt deck; see the Game field) is what
// bumped this from the old v1 [version][state_put...]. There is no v1 read
// path - a data migration rewrote every stored v1 blob to v2 (flag 0), so no
// v1 blob ever reaches this kernel; anything that isn't v2 is unreadable.
//
// ONE definition, for every writer and reader of that column: the wasm bridge
// (wasm_state_serialize / wasm_state_deserialize) and the table layer
// (table.h TABLE_STATE_FORMAT, table_seal/table_load/table_commit_products)
// are both these functions, so a format bump cannot land on one side only.
// A richer on-disk layout that WRAPS this blob is a different format with its
// own version (server/impls/native/snapshot.c PERSIST_GAME_BLOB_VERSION).
#define STATE_BLOB_FORMAT 2

// The bytes the blob's header costs, ahead of the state_put payload.
#define STATE_BLOB_HEADER 2

// Write g as a durable blob; returns the byte length (>= STATE_BLOB_HEADER).
int state_blob_put(const Game *g, unsigned char *out);

// Load a durable blob back into g. Returns 1 on success; 0 if the leading
// version byte is not one this kernel reads (the caller must treat that as
// unreadable, never as an empty game); or a negative GAME_INVALID_* reason if
// the state inside is one the kernel refuses (game.h game_validate) - g is
// then left exactly as it was.
// `len` counts the header bytes too.
int state_blob_load(Game *g, const unsigned char *p, int len);

// ---------- the response envelope header ----------------------------------
//
// The bytes ahead of the view blob in a server response envelope, known ONCE.
// table.c table_envelope writes them with env_header_write; client_table.c
// client_adopt_envelope reads them back with env_header_read; shipped iOS
// builds decode the same layout in Swift. It lives here, beside
// VIEW_FORMAT_VERSION, because the envelope is a view blob with a header and
// because both ends link view.c - table.c is the server's alone.
//
//   0   u8  ENV_FORMAT
//   1   u8  flags: ENV_FLAG_SEATED, ENV_FLAG_ROSTER
//   2   u8  the viewer's seat, 0xFF for the spectator
//   3   u32 version, little-endian
//   7   u16 the retired JSON roster island's length (view.ts), then that run
//  ..   u16 view_len, then the view blob, then the roster trailer (roster.h)
//
// All little-endian. The view blob is [VIEW_FORMAT_VERSION][viewer][masked
// state], and view_len counts those two prefix bytes.
//
// THE ISLAND IS A LENGTH-PREFIXED RUN, so the view's offset is DERIVED from it
// and never a constant. This kernel writes the island empty, which makes the
// header exactly ENV_VIEW_AT bytes long - but a reader that hardcoded
// ENV_VIEW_AT would misread the first envelope anyone wrote an island into,
// which is why there is one reader and it derives.
#define ENV_FORMAT       1
#define ENV_FLAG_SEATED  0x01   // bit0: the envelope is for a seated viewer
#define ENV_FLAG_ROSTER  0x02   // bit1: a packed roster trailer follows the view
#define ENV_ISLAND_AT    7      // the island's u16 length, then its bytes
#define ENV_VIEW_AT      11     // the view blob, with the empty island this kernel writes

// What env_header_read found. Offsets are into the envelope it was given.
typedef struct {
    int      seat;        // the viewer's seat, or -1 for the spectator
    uint32_t version;
    int      state_at;    // the masked state inside the view blob
    int      state_len;
    int      trailer_at;  // where the roster trailer starts (state_at + state_len)
} EnvHeader;

// Writes the header for `viewer` (a seat, or -1 for the spectator) at
// `version`, the empty island, and the view blob's own two-byte prefix.
// Returns the view blob's offset (ENV_VIEW_AT), or -1 when `cap` cannot hold
// even the header. The caller writes the masked state at that offset + 2 and
// then calls env_header_set_view_len: the length prefix sits AHEAD of a blob
// whose length is only known once it is written.
int env_header_write(unsigned char *out, int cap, int viewer, uint32_t version);

// The view blob's length (2 + the state's), into the header written above.
void env_header_set_view_len(unsigned char *out, int view_len);

// Reads the header of the `len`-byte envelope at p. true and *h filled, or
// false for a header this kernel does not read (format, flags, seat byte or a
// length that runs off the end). The view blob is bounds-checked inside p; the
// TRAILER is not, so h->trailer_at may be len - the caller judges a missing
// trailer, which is a refusal of its own and not a malformed header.
bool env_header_read(const unsigned char *p, int len, EnvHeader *h);

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

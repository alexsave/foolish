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
// Layout v3: [version][deterministic_deck flag][state_put(VIEW_UNMASKED)]
// [u48 LE shown_ms][u48 LE settles_ms]. The flag byte (added with the
// seed-dealt deck; see the Game field) is what bumped the old v1
// [version][state_put...] to v2, and the trailing BoardClock is what bumped v2
// to v3. There is no v1 read path - a data migration rewrote every stored v1
// blob to v2 (flag 0), so no v1 blob ever reaches this kernel.
//
// Layout v4: v3 with the flag byte a bitfield (STATE_BLOB_FLAG_* below), so
// the board's RULES (game.h Game.rules) survive the row. v2 and v3 read their
// flag byte as "nonzero means a deterministic deck", so no bit could be added
// to it without a new format byte. Bit 1 reads 1 for the classic passing game
// and 0 for podkidnoy, the sense every value that crosses a boundary has
// (docs/PODKIDNOY.md); Game.rules keeps its own sense (zero is the classic
// game) inside the kernel. A v4 flag with a bit this kernel does not know is
// a rule it cannot honour, so that blob is unreadable, never a guess.
//
// v2, v3 AND v4 ARE READ; ONE OF THEM IS WRITTEN (STATE_BLOB_FORMAT below).
// The column is durable, so its format moves the way the repo moves any durable
// store (docs/ARCHITECTURE_AS_A_PATTERN.md): EXPAND, a kernel that reads the new
// format and still writes the old one, deployed first; SWITCH, the same kernel
// with STATE_BLOB_FORMAT flipped to the new one, deployed once the expand kernel
// is live everywhere, so a rollback or a deploy window never puts a new row in
// front of a kernel that refuses it; CONTRACT, the old read path deleted once no
// old row is left. v2 -> v3 has had its switch (#244, #246), and v3 -> v4 has
// had both: its expand (#249) read v4 and still wrote v3, and this kernel is the
// switch, which writes v4. The CONTRACT of v2 and v3 is still to come: rows of
// both stay in the column until a game rewrites them, so both are still read. A
// v2 row reads as a zero clock: a
// board last shown long ago, which is what a zero clock says (bot_drive.h
// bot_wait_ms asks for no wait on it). A v2 or v3 row reads as the classic
// passing game, which is all either could ever hold. Anything else is
// unreadable.
//
// THE RULES HAVE ONE OWNER ON LOAD: state_blob_load sets g->rules from the row
// (zero for v2 and v3), so a Game that held a variant before - an FMSG decode in
// the same module, say - never leaks it into the row's game.
//
// ONE definition, for every writer and reader of that column: the wasm bridge
// (wasm_state_serialize / wasm_state_deserialize) and the table layer
// (table.h TABLE_STATE_FORMAT, table_seal/table_load/table_commit_products)
// are both these functions, so a format bump cannot land on one side only.
// A richer on-disk layout that WRAPS this blob is a different format with its
// own version (server/impls/native/snapshot.c PERSIST_GAME_BLOB_VERSION).
#define STATE_BLOB_FORMAT_V2 2   // [version][flag][state_put]
#define STATE_BLOB_FORMAT_V3 3   // ... then the board's clock (BoardClock below)
#define STATE_BLOB_FORMAT_V4 4   // v3, its flag byte the bits below

// The v4 flag byte. Bit 0 means what the whole v2/v3 flag byte meant; a classic
// game's row reads PASSING with it.
#define STATE_BLOB_FLAG_DETERMINISTIC 0x01   // the seed-dealt deck (Game.deterministic_deck)
#define STATE_BLOB_FLAG_PASSING       0x02   // 1 the classic passing game, 0 podkidnoy (GAME_RULE_NO_PASS)
#define STATE_BLOB_FLAGS_V4 (STATE_BLOB_FLAG_DETERMINISTIC | STATE_BLOB_FLAG_PASSING)

// THE FORMAT THIS KERNEL WRITES, and the one switch of the move above: V4, the
// switch step (it was V3 while the expand kernel was deploying). Everything that
// depends on which one is written - the clock bytes a blob ends with, whether a
// board's rules can be written at all - follows from it.
#define STATE_BLOB_FORMAT STATE_BLOB_FORMAT_V4

// The bytes the blob's header costs, ahead of the state_put payload.
#define STATE_BLOB_HEADER 2

// THE BOARD'S CLOCK, which a v3 or v4 blob carries behind the state: when a viewer was
// last shown an operation on this board, and when that showing finishes playing
// (the operation's animation stream, queued behind whatever was still playing).
// Epoch milliseconds, 0 for never. It is a table fact, not a rule of the game -
// the Game struct never holds it - and the one reader of it is the server's bot
// wait (bot_drive.h bot_wait_ms). table.c table_commit_products advances it.
typedef struct {
    int64_t shown_ms;
    int64_t settles_ms;
} BoardClock;
#define STATE_BLOB_V3_CLOCK_BYTES 12   // v3 and v4 alike
// The clock bytes a blob of format `fmt` ends with, or -1 for a format this
// kernel does not read: the ONE list of the formats read, for state_blob_load
// and for table_load's look at the header before it.
static inline int state_blob_clock_bytes(int fmt) {
    return fmt == STATE_BLOB_FORMAT_V3 || fmt == STATE_BLOB_FORMAT_V4 ? STATE_BLOB_V3_CLOCK_BYTES
         : fmt == STATE_BLOB_FORMAT_V2 ? 0 : -1;
}
// The clock bytes a WRITTEN blob ends with.
#define STATE_BLOB_CLOCK_BYTES (STATE_BLOB_FORMAT == STATE_BLOB_FORMAT_V2 ? 0 : STATE_BLOB_V3_CLOCK_BYTES)

// Refused by the writers below: g's rules are ones the format cannot carry (any
// variant at all, before v4). Writing the row anyway would turn the game into
// the classic one the next time it loads, so nothing is written.
#define STATE_BLOB_E_RULES (-1)

// Write g as a durable blob at STATE_BLOB_FORMAT, with its clock when that
// format carries one; returns the byte length, or STATE_BLOB_E_RULES. `clk`
// NULL writes a zero clock (a board nobody has been shown).
int state_blob_put(const Game *g, const BoardClock *clk, unsigned char *out);

// The same at an explicit format, v2, v3 or v4: for the tests that write the
// rows older kernels wrote, which this one must still read, and that pin the
// refusal of a variant at a format that cannot carry it. Nothing but
// state_blob_put names a format in shipped code.
int state_blob_put_at(const Game *g, const BoardClock *clk, int format, unsigned char *out);

// Load a durable blob back into g, and its clock into `clk` (may be NULL; a v2
// blob reads as a zero clock), and its rules into g->rules (v2 and v3: the
// classic game). Returns 1 on success; 0 if the leading version byte is not one
// this kernel reads, or a v4 flag byte holds a bit it does not know (the caller
// must treat that as unreadable, never as an empty game); or a negative
// GAME_INVALID_* reason if the state inside is one the kernel refuses (game.h
// game_validate) - g and clk are then left exactly as they were.
// `len` counts the header bytes too.
int state_blob_load(Game *g, const unsigned char *p, int len, BoardClock *clk);

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

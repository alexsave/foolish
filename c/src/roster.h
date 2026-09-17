// roster.h - who sits where: the table's identity, beside the board.
//
// A Game (game.h) is the board. A Roster is everything a board deliberately
// does not know: each seat's player id, display name and bot brain, plus the
// table's title. It lives BESIDE the Game rather than inside it, because the
// Monte-Carlo searchers copy offsetof(Game, logs) bytes per node and identity
// must not ride those copies (docs/C_GAME_SHAPE_MIGRATION.md 2.9).
//
// Every string crosses as bytes with a length. A name is arbitrary Unicode and
// NUL is not its terminator; the zero byte after each field in the struct is a
// convenience for C readers, never the length.
//
// Two byte layouts live here, and both are frozen:
//
// 1. THE DURABLE ENCODING (games.roster, ROSTER_FORMAT_VERSION 1). Fixed width,
//    so the one-time SQL conversion from JSONB is a straight concatenation:
//
//      off   size       field
//      0     1          ROSTER_FORMAT_VERSION = 1
//      1     1          n seats (0..8)
//      2     1          title_len (0..200)
//      3     200        title bytes, zero padded
//      203   8 x 128    seat records (all 8 written; unused ones all zero):
//                         +0    1   id_len (1..36)
//                         +1    36  id, zero padded
//                         +37   1   name_len (0..64)
//                         +38   64  name UTF-8, zero padded
//                         +102  1   brain_len (0..23)
//                         +103  23  brain key, zero padded ("" = human)
//                         +126  2   reserved, zero
//      1227             end
//
//    The decoder refuses, never clamps: any other length, an unknown version,
//    a count or length over its cap, invalid UTF-8, a non-zero pad byte, a
//    non-empty unused record, or a duplicate id. So there is exactly one byte
//    string per roster, and decode -> encode is the identity.
//
// 2. THE ENVELOPE TRAILER, which shipped iOS builds decode in Swift
//    (ios/FoolishNet/EnvelopeRoster.swift) and the web decodes in TS
//    (c/src/client_table.c, through roster_trailer_read). Written byte for byte like
//    roster.ts encodePackedRoster:
//
//      u8  format = 1
//      u16 game_id_len, game_id
//      u16 title_len, title
//      u8  status (0 waiting, 1 playing, 2 game_over)
//      u8  n, then n x { u8 seat, u8 name_len, name }   (the kernel's names block)
//      n x { u16 id_len, id, u8 is_ai }
//      u8  n_good, then n_good x { u16 id_len, id }
//      u8  has_ts, then f64 ts when has_ts != 0
//
//    All little-endian. The C writer emits the good ids in SEAT order from the
//    mask and has_ts = 0 (5.4 in the migration doc: no reader uses either).
#ifndef CNITRO_ROSTER_H
#define CNITRO_ROSTER_H

#include "game.h"
#include <stdint.h>

#define ROSTER_FORMAT_VERSION 1
#define ROSTER_ID_MAX      36   // bytes: a canonical UUID, or any shorter host id
#define ROSTER_NAME_MAX    64   // bytes of UTF-8, == MSG_MAX_NAME
#define ROSTER_BRAIN_MAX   23   // bytes: a bot_roster key such as "cordite"; empty = human
#define ROSTER_TITLE_MAX   200  // bytes: the table title (50 characters today)
#define ROSTER_GAME_ID_MAX 64   // bytes: games.id as the trailer carries it
#define ROSTER_BYTES       1227 // the durable encoding is FIXED WIDTH

#define ROSTER_SEAT_BYTES  128
#define ROSTER_TRAILER_FORMAT 1
// The largest trailer roster_trailer_write can produce, for sizing a buffer.
#define ROSTER_TRAILER_MAX (1 + 2 + ROSTER_GAME_ID_MAX + 2 + ROSTER_TITLE_MAX + 1 \
                            + 1 + MAX_PLAYERS * (2 + ROSTER_NAME_MAX)            \
                            + MAX_PLAYERS * (2 + ROSTER_ID_MAX + 1)              \
                            + 1 + MAX_PLAYERS * (2 + ROSTER_ID_MAX) + 1)

// Results. Negative is a refusal; append-only numbering, because a host may
// log or map the number.
#define ROSTER_OK            0
#define ROSTER_E_LENGTH    (-1)   // durable bytes are not exactly ROSTER_BYTES
#define ROSTER_E_VERSION   (-2)   // unknown format byte
#define ROSTER_E_COUNT     (-3)   // n outside 0..MAX_PLAYERS
#define ROSTER_E_ID        (-4)   // id empty, over ROSTER_ID_MAX, NUL inside, or not UTF-8
#define ROSTER_E_NAME      (-5)   // name over ROSTER_NAME_MAX or not UTF-8
#define ROSTER_E_BRAIN     (-6)   // brain over ROSTER_BRAIN_MAX or not printable ASCII
#define ROSTER_E_TITLE     (-7)   // title over ROSTER_TITLE_MAX or not UTF-8
#define ROSTER_E_DUPLICATE (-8)   // two seats share an id
#define ROSTER_E_FULL      (-9)   // every seat is taken
#define ROSTER_E_SEAT      (-10)  // seat index out of range, or the id is not seated
#define ROSTER_E_PERM      (-11)  // not a permutation of the seats
#define ROSTER_E_CAP       (-12)  // output buffer too small
#define ROSTER_E_PADDING   (-13)  // a pad, reserved or unused-record byte is not zero
#define ROSTER_E_STATUS    (-14)  // trailer status outside 0..2
#define ROSTER_E_GAME_ID   (-15)  // game id over ROSTER_GAME_ID_MAX
#define ROSTER_E_SHORT     (-16)  // trailer runs off the end
#define ROSTER_E_GOOD      (-17)  // a good bit names no seat
#define ROSTER_E_FLAG      (-18)  // a trailer flag byte is neither 0 nor 1

typedef struct {
    uint8_t id_len, name_len, brain_len;
    char    id[ROSTER_ID_MAX + 1];
    char    name[ROSTER_NAME_MAX + 1];
    char    brain[ROSTER_BRAIN_MAX + 1];
} RosterSeat;

typedef struct {
    int8_t     n;                       // == Game.num_players, always
    uint8_t    title_len;
    char       title[ROSTER_TITLE_MAX + 1];
    RosterSeat seats[MAX_PLAYERS];
} Roster;

// ---- the test entries' shapes ---------------------------------------------
//
// A table as a TEST states it, and what reading a trailer said. The test-only
// wasm entries (c/wasm/wasm_bots_api.c, WASM_TEST_EXPORTS) cross with these
// structs rather than a packed seat list a harness would have to write byte by
// byte. `RosterSpec` is an INPUT: the title and the seats to roster_set_title
// and roster_seat_add, in order, so every refusal those make is still the
// kernel's (a value wider than a slot is the generated writer's RangeError, and
// nothing drives that path).
// The slots are WIDER than the roster's own budgets on purpose: a test hands
// over RAW input so that trimming a name and refusing an id stay the kernel's
// judgement (roster_seat_add, roster_set_title) and not a writer's range check.
// 4x a name covers 64 characters of 4-byte scalars, the widest a nickname field
// can produce, and the parity suite's worst case is 144 bytes.
#define ROSTER_SPEC_ID_MAX    (2 * ROSTER_ID_MAX)
#define ROSTER_SPEC_NAME_MAX  (4 * ROSTER_NAME_MAX)
#define ROSTER_SPEC_BRAIN_MAX (2 * ROSTER_BRAIN_MAX)
#define ROSTER_SPEC_TITLE_MAX (2 * ROSTER_TITLE_MAX)

typedef struct {
    uint16_t id_len;    char id[ROSTER_SPEC_ID_MAX];
    uint16_t name_len;  char name[ROSTER_SPEC_NAME_MAX];
    uint16_t brain_len; char brain[ROSTER_SPEC_BRAIN_MAX];
} RosterSpecSeat;

typedef struct {
    int32_t        n;                  // seats, 0..MAX_PLAYERS
    uint16_t       title_len;
    char           title[ROSTER_SPEC_TITLE_MAX];
    RosterSpecSeat seats[MAX_PLAYERS];
} RosterSpec;

// What roster_trailer_read found, beside the durable roster it decoded (which
// the entry leaves in the IO buffer as ROSTER_BYTES of opaque bytes).
typedef struct {
    int32_t  status;      // GAME_STATUS_*
    uint32_t ai_mask;     // bot seats
    int32_t  consumed;    // trailer bytes read
    uint16_t gid_len;
    char     gid[ROSTER_GAME_ID_MAX];
} RosterTrailerRead;

// The durable blob. decode zeroes *r first and returns ROSTER_OK or ROSTER_E_*;
// encode refuses an invalid roster and otherwise returns ROSTER_BYTES.
int      roster_decode(Roster *r, const uint8_t *p, int len);
int      roster_encode(const Roster *r, uint8_t *out, int cap);

// Every rule a Roster keeps: n range; each seated id 1..36 bytes of UTF-8 with
// no NUL; each name <= 64 bytes of UTF-8; each brain <= 23 bytes of printable
// ASCII; the title <= 200 bytes of UTF-8; no two seats with one id.
int      roster_validate(const Roster *r);

// The seat whose id is EXACTLY these bytes, or -1. A prefix of a seated id, a
// longer id that starts with one, and the empty id all miss.
int      roster_seat_of(const Roster *r, const char *id, int id_len);

// Seats with a brain (brain_len > 0), as a bit mask by seat.
uint32_t roster_bot_mask(const Roster *r);

// Appends a seat and returns its index. The name is trimmed with
// roster_name_trim (what every envelope has always shown), then must be UTF-8.
// Refusals: E_ID, E_DUPLICATE, E_FULL, E_NAME, E_BRAIN. *r is untouched on one.
int      roster_seat_add(Roster *r, const char *id, int id_len, const char *name, int name_len,
                         const char *brain, int brain_len);

// Removes a seat and compacts the seats above it down by one.
int      roster_seat_remove(Roster *r, int seat);

// New seat i is old seat perm[i]. n must equal r->n and perm must be a
// permutation of 0..n-1, or E_PERM with *r untouched.
int      roster_reorder(Roster *r, const int8_t *perm, int n);

// The title is refused over its cap, never trimmed (no envelope ever trimmed it).
int      roster_set_title(Roster *r, const char *t, int len);

// Replaces the name of the seat holding this id (account deletion). Returns the
// seat, E_SEAT when the id is not seated, or E_NAME.
int      roster_redact(Roster *r, const char *id, int id_len, const char *name, int name_len);

// The byte length of the longest prefix of whole Unicode scalars that fits
// ROSTER_NAME_MAX: the rule of roster.ts rosterNameBytes and RosterWire.swift
// nameBytes (both trim code points, never grapheme clusters). Input is UTF-8.
int      roster_name_trim(const char *utf8, int len);

// The envelope trailer (layout above). Returns bytes written, or E_* (the
// roster is validated, status must be 0..2, good_mask must name seats only).
int      roster_trailer_write(const Roster *r, const char *game_id, int gid_len, int status,
                              uint32_t good_mask, uint8_t *out, int cap);

// The same trailer with the AI seats named by a mask rather than by brains (a
// client's roster, read from a trailer, has none). roster_trailer_write is this
// with roster_bot_mask(r).
int      roster_trailer_write_ai(const Roster *r, const char *game_id, int gid_len, int status,
                                 uint32_t good_mask, uint32_t ai_mask, uint8_t *out, int cap);

// Reads a trailer at p. game_id must hold ROSTER_GAME_ID_MAX + 1 bytes. The
// trailer carries is_ai, not a brain, so the seats come back with no brain and
// the AI seats in *ai_mask. The good ids and the timestamp are checked for
// shape and skipped: the mask lives in the state blob. Returns ROSTER_OK and
// the bytes read in *consumed, or E_*.
int      roster_trailer_read(Roster *r, char *game_id, int *gid_len, int *status,
                             uint32_t *ai_mask, const uint8_t *p, int len, int *consumed);

#endif

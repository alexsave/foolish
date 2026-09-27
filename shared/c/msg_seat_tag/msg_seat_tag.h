// WHO SITS WHERE in a Messages game whose roster rows carry a seat tag: the
// row a roster is made of, the verdict on a typed nickname, and which row is
// this device.
//
// A ROSTER ROW is a seat tag (a value only the device that wrote it can
// recognise, salted by the game so it cannot follow a person between threads)
// and a nickname. Each product derives its own tag with its own salt and puts
// the rows on its own wire; the row's layout and every rule below are the
// same for all of them.
//
// WHICH SEAT AM I: four witnesses, in this order.
//
//   (a) THE RECORD - this device's own note of the tag it sat with, keyed by
//       the game. It names a tag, not a seat number, so a leave that moves
//       rows down still finds the row.
//   (b) THE TAG - my identity hashed with this game's seed, found in the
//       roster.
//   (c) THE SENDER - i_sent 1: I am the seat that sent this bubble; i_sent 0
//       in a two-person chat of two seats: the other seat.
//   (d) THE NAME - my nickname, the first row that carries it.
//
// A RECORD THAT FINDS NO ROW (MSG_SEAT_REC_GONE) IS A WITNESS TOO: this
// device sat in this game under a tag no row carries, so it is not seated
// here, and neither the sender nor the name may say otherwise. Only the tag,
// a direct witness, still counts.
//
// THE LOBBY GATE: before the game starts, a seat found by the sender witness
// counts only if its row carries my name when I have one, because a lobby
// seat is claimed by a named join and an inference cannot overrule "none of
// these rows is me".
//
// Freestanding: no allocation, nothing from libc beyond memcmp. In its own
// directory beside its test so the builds that wildcard shared/c/*.c pick up
// neither.
#ifndef SHARED_MSG_SEAT_TAG_H
#define SHARED_MSG_SEAT_TAG_H

#include <stdint.h>

#define MSG_SEAT_TAG_LEN        9
#define MSG_SEAT_NAME_MAX_BYTES 48   // a row's name, in bytes of UTF-8
#define MSG_SEAT_NAME_MAX_CHARS 16   // and in code points: the seat-name clipping cap

// One roster row. A product's own seat type is this type, so its rows are
// these rows byte for byte.
typedef struct {
    uint8_t tag[MSG_SEAT_TAG_LEN];
    uint8_t name_len;                // 1..MSG_SEAT_NAME_MAX_BYTES
    uint8_t name[MSG_SEAT_NAME_MAX_BYTES];
} MsgSeat;

// A NICKNAME, JUDGED. The host trims; the caps are here. Rejects rather than
// truncates.
#define MSG_SEAT_NAME_OK       0
#define MSG_SEAT_NAME_EMPTY    1
#define MSG_SEAT_NAME_TOO_LONG 2     // over the byte cap or the code point cap
#define MSG_SEAT_NAME_BAD      3     // not strict UTF-8, or a control character
int msg_seat_name_verdict(const uint8_t *name, int len);

// Strict UTF-8: shortest forms only, no surrogates, nothing past U+10FFFF, no
// control character (C0, DEL, C1). The code point count, or -1.
int msg_seat_utf8_chars(const uint8_t *s, int len);

// The row carries exactly these bytes: same length, not empty, same bytes. No
// folding of any kind, so "Alex" and "alex" are two names.
int msg_seat_same_name(const MsgSeat *row, const uint8_t *name, int len);

// The deciding witness.
#define MSG_SEAT_BY_NONE   0
#define MSG_SEAT_BY_RECORD 1
#define MSG_SEAT_BY_TAG    2
#define MSG_SEAT_BY_SENDER 3
#define MSG_SEAT_BY_NAME   4

// A record lookup's answer when this device has a record of the game and no
// row carries its tag.
#define MSG_SEAT_REC_GONE  (-2)
// i_sent when Messages has not said who sent the bubble.
#define MSG_SEAT_SENT_UNKNOWN (-1)

// Which of the `n` rows is this device. `started` is whether the game is past
// its lobby; `sender` the seat that sent this bubble, or -1 (a leave: the
// sender has no seat). `record` is the record lookup's answer (a seat, -1 or
// MSG_SEAT_REC_GONE), `tag_seat` the row holding my tag or -1, `i_sent` 1, 0
// or MSG_SEAT_SENT_UNKNOWN, `name` my nickname (may be NULL). The seat, or -1
// for "not seated here" (a spectator, or a lobby I may join); the deciding
// witness in *by (may be NULL).
int msg_seat_resolve(const MsgSeat *rows, int n, int started, int sender,
                     int record, int tag_seat, int is_dm, int i_sent,
                     const uint8_t *name, int name_len, int *by);

#endif

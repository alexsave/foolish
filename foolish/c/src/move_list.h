// move_list.h - THE SESSION'S MOVE LIST: what the server keeps of a game being
// played so that, when it ends, its replay code and its extras can be cut
// (table.h table_replay_code_list / table_replay_extras_list).
//
// It replaces the session log (games.logs_packed / game_moves: 10 + 2n bytes a
// record, every DRAW and DEFENDER_CHANGE kept, a u48 stamp on each) with only
// what those two need:
//
//   * the v6 code's ATOMS (replay.c log_atom_kind): every ATTACK, COVER, PASS
//     and PICKUP, the ROUND_END (a DISCARD right after a GOOD), and the GOODs,
//     which the code keeps only while they are the list's last items (a good
//     still pending when the list ends). The deal is not here: it is the game's
//     32-byte seed (games.seed) re-dealt at the table's width. The opener is the
//     first ATTACK's seat, as the code itself reads it; an imposed one (the
//     fool's penalty) is told apart from the deal's by the encoder, against the
//     re-deal (replay.c encode_v6_run), exactly as it was from the log.
//   * the TIMES the extras carry (replay_extras.h): the exact millisecond of
//     the GAME_START and of every ATTACK/COVER/PASS/PICKUP, as a signed delta
//     from the last time the list holds. All the records of one commit share
//     one millisecond, so a commit costs one TICK, written before its first
//     timed item, and a commit at the same millisecond as the last costs none:
//     equal times stay equal (the viewer groups covers by a gap of 0). The
//     extras' one-byte log-scale gaps are quantised from these at the end, once.
//
// It is server-only, like the seed: with the seed it is the whole deal, so it
// never goes into a row's serialized fields, a TableResident, an envelope or a
// response.
//
// THE BYTES. Empty (0 bytes) is no session. Otherwise byte 0 is the format,
// MOVE_LIST_FORMAT, or MOVE_LIST_BROKEN (a session this list could not hold:
// past MOVE_LIST_MAX, or a record it has no spelling for; it has no replay and
// stays broken until the next GAME_START). Then items, each one head byte
// h = kind << 5 | low:
//
//   kind 0 MARK    low 0 START (the GAME_START: the list's first item, after
//                  its TICK), 1 ROUND_END, 2 CUT (a record that is not an atom
//                  after a GOOD: the GOOD is superseded). ROUND_END and CUT
//                  only right after a GOOD.
//   kind 1 ATTACK  low = seat | c << 3: n = c + 1 cards for c < 3, c = 3 means
//   kind 3 PASS    a byte n (4..MOVE_LIST_MAX_CARDS) follows; then n cards.
//   kind 2 COVER   low = seat; then the cover and the attack it covers.
//   kind 4 PICKUP  low = seat.
//   kind 5 GOOD    low = seat.
//   kind 6 TICK    the time moves by zigzag(delta ms) = z, never 0: low bits
//                  0-3 are z & 15, bit 4 says a minimal LEB128 of z >> 4
//                  follows. Only right before a timed item (START, ATTACK,
//                  COVER, PASS, PICKUP); the time stays in [0, 2^48).
//
// A card is its wire id, 0..51. A seat is 0..7 (the table checks it against its
// width when it encodes). Every list has exactly one spelling: the reader
// refuses any byte the writer would not have written.
#ifndef MOVE_LIST_H
#define MOVE_LIST_H

#include <stdbool.h>
#include <stdint.h>

#define MOVE_LIST_FORMAT 1
#define MOVE_LIST_BROKEN 0xFF
// The longest list kept. About 64 times a 6-seat game's p99 (531 records,
// ~1 KB), so only a game that should have ended long ago reaches it.
#define MOVE_LIST_MAX (64 * 1024)
#define MOVE_LIST_MAX_CARDS 52   // replay.h REPLAY_MAX_PAIRS

#define MOVE_LIST_E_PARSE (-1)   // not a list this kernel writes
#define MOVE_LIST_E_CAP   (-2)   // the caller's buffer cannot hold even the reset list

// Item kinds a reader sees: the LOG_* of the atom it stands for (game.h), and these.
#define MOVE_LIST_START     0x40
#define MOVE_LIST_ROUND_END 0x41
#define MOVE_LIST_CUT       0x42

typedef struct {
    int     kind;     // LOG_ATTACK, LOG_COVER, LOG_PASS, LOG_PICKUP, LOG_GOOD, or MOVE_LIST_*
    int     seat;     // -1 for START, ROUND_END and CUT
    int     n;        // cards: ATTACK and PASS n, COVER 2 (cover, then the attack covered), else 0
    uint8_t cards[MOVE_LIST_MAX_CARDS];
    bool    timed;    // START and the four info atoms: the extras time it
    int64_t ms;       // the list's time at this item
} MoveListItem;

typedef struct {
    const uint8_t *p;
    int     len, at;
    int64_t ms;
    int     prev;     // the last item's kind, -1 before the first
} MoveListCursor;

// Opens a cursor on `list`. 0 for a list to read (an empty one too), 1 for a
// broken one (no items), MOVE_LIST_E_PARSE for a format this kernel does not write.
int move_list_open(MoveListCursor *c, const uint8_t *list, int len);
// The next item: 1, 0 at the end, MOVE_LIST_E_PARSE.
int move_list_next(MoveListCursor *c, MoveListItem *it);

// What an append needs to know of the list it extends: read once a commit
// (move_list_scan), then carried record to record.
typedef struct {
    int64_t ms;        // the last time the list holds (0 before any)
    bool    last_good; // the last item is a GOOD
    bool    broken;
} MoveListTail;

// Reads the whole list, checking every byte. 0 and the tail, or MOVE_LIST_E_PARSE.
int move_list_scan(const uint8_t *list, int len, MoveListTail *tail);

// Appends one session-log record (game.h LOG_*, its seat, and its pairs as wire
// bytes, primary then target, n_pairs of them) stamped `ms`, to the list of
// `len` bytes in `list` (`cap` bytes of room), whose tail is `tail`. A
// GAME_START starts a new list. A record the list cannot hold (a card past 51
// where an atom needs a card, a seat past 7, a list past MOVE_LIST_MAX or past
// `cap`) leaves it broken. Returns the new length, or MOVE_LIST_E_CAP when
// `cap` cannot hold the one byte a broken list is.
int move_list_append(uint8_t *list, int len, int cap, MoveListTail *tail, int64_t ms,
                     int type, int seat, int n_pairs, const uint8_t *pairs);

#endif

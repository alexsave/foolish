// move_list.h - THE SESSION'S MOVE LIST: what the server keeps of a game being
// played so that, when it ends, its replay code and its extras can be cut
// (table.h table_replay_code_list / table_replay_extras_list).
//
// It replaces the session log (game_moves / logs_packed: 10 + 2n bytes a
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
//   * the TIMES the extras carry (replay_extras.h): the exact millisecond of the
//     GAME_START and of every ATTACK/COVER/PASS/PICKUP. All the records of one
//     commit share one millisecond, so a commit costs one TICK, written before
//     its first timed item: equal times stay exactly equal (the viewer groups
//     covers by a gap of 0) and a clock that steps back is kept as it stepped.
//     The extras' one-byte log-scale gaps are quantised from these at the end,
//     once.
//
// THE LIST IS ITS COMMITS' CHUNKS, CONCATENATED. A commit appends the chunk its
// own records make (table.h table_move_list_chunk) and never reads the list it
// extends, so a store keeps it exactly as it kept the session log: append a
// delta, clear it at a deal or a lobby. That is what fixes two choices below:
//   * a TICK is the commit's millisecond as an offset from a BASE the host
//     holds constant for the game (the server: the row's created_at), not from
//     the commit before, which the chunk cannot see;
//   * a record that is no atom costs nothing, except where the reader must hear
//     of it to decide what a GOOD before it means: right after a GOOD in the same
//     chunk, or as a chunk's first record (the chunk cannot see what came last).
//
// It is server-only, like the seed: with the seed it is the whole deal, so it
// never goes into a row's serialized fields, a TableResident, an envelope or a
// response.
//
// THE BYTES. Empty (0 bytes) is no session. A session's list begins with the
// format byte MOVE_LIST_FORMAT and the START, which the chunk holding the
// session's GAME_START writes. Then items, each one head byte h = kind << 5 | low:
//
//   kind 0 MARK    low 0 START (the GAME_START: the list's first item, after
//                  its TICK), 1 DISCARD (a round's end when right after a GOOD,
//                  else nothing), 2 OTHER (a record that is no atom: a GOOD
//                  before it is superseded).
//   kind 1 ATTACK  low = seat | c << 3: n = c + 1 cards for c < 3, c = 3 means
//   kind 3 PASS    a byte n (4..MOVE_LIST_MAX_CARDS) follows; then n cards.
//   kind 2 COVER   low = seat; then the cover and the attack it covers.
//   kind 4 PICKUP  low = seat.
//   kind 5 GOOD    low = seat.
//   kind 6 TICK    the time is BASE + unzigzag(z): low bits 0-3 are z & 15,
//                  bit 4 says a minimal LEB128 of z >> 4 follows. Only right
//                  before a timed item (START, ATTACK, COVER, PASS, PICKUP).
//                  z < 2^49: a u48 stamp less a u48 base.
//   kind 7 BROKEN  low 0: a record the list has no spelling for (a card past 51
//                  where an atom needs one, a seat past 7). The session has no
//                  replay.
//
// A card is its wire id, 0..51. A seat is 0..7 (the table checks it against its
// width when it encodes). A list longer than MOVE_LIST_MAX has no replay either.
// Each commit's chunk has one spelling, and the reader refuses any byte no chunk
// could have.
#ifndef MOVE_LIST_H
#define MOVE_LIST_H

#include <stdbool.h>
#include <stdint.h>

#define MOVE_LIST_FORMAT 1
// The one-byte chunk a host appends for a commit whose chunk it had no room for:
// the session keeps its commits and loses its replay.
#define MOVE_LIST_BROKEN_ITEM 0xE0
// The longest list a replay is cut from. About 64 times a 6-seat p99 game's
// (531 records), so only a game that should have ended long ago reaches it.
#define MOVE_LIST_MAX (64 * 1024)
#define MOVE_LIST_MAX_CARDS 52   // replay.h REPLAY_MAX_PAIRS
// The most one chunk adds to its records' bytes: the format byte and a TICK.
#define MOVE_LIST_CHUNK_SLACK 10

#define MOVE_LIST_E_PARSE  (-1)   // not a list this kernel writes
#define MOVE_LIST_E_CAP    (-2)   // the chunk does not fit the caller's buffer
#define MOVE_LIST_E_BROKEN (-3)   // a list this kernel wrote, of a session it could not keep

// Item kinds a reader sees: the LOG_* of the atom it stands for (game.h), and these.
#define MOVE_LIST_START     0x40
#define MOVE_LIST_ROUND_END 0x41   // a DISCARD right after a GOOD
#define MOVE_LIST_OTHER     0x42   // any other record the list mentions: no atom

typedef struct {
    int     kind;     // LOG_ATTACK, LOG_COVER, LOG_PASS, LOG_PICKUP, LOG_GOOD, or MOVE_LIST_*
    int     seat;     // -1 for START, ROUND_END and OTHER
    int     n;        // cards: ATTACK and PASS n, COVER 2 (cover, then the attack covered), else 0
    uint8_t cards[MOVE_LIST_MAX_CARDS];
    bool    timed;    // START and the four info atoms: the extras time them
    int64_t ms;       // the time of the list's last TICK (BASE before any)
} MoveListItem;

typedef struct {
    const uint8_t *p;
    int     len, at;
    int64_t base, ms;
    int     prev;     // the last item's kind, -1 before the first
} MoveListCursor;

// Opens a cursor on `list`, whose TICKs are offsets from `base_ms`. 0, or
// MOVE_LIST_E_PARSE for a list that does not begin with this format.
int move_list_open(MoveListCursor *c, const uint8_t *list, int len, int64_t base_ms);
// The next item: 1, 0 at the end, MOVE_LIST_E_PARSE, MOVE_LIST_E_BROKEN.
int move_list_next(MoveListCursor *c, MoveListItem *it);
// Reads the whole list, checking every byte: 0, MOVE_LIST_E_PARSE or MOVE_LIST_E_BROKEN.
int move_list_check(const uint8_t *list, int len);

// ONE COMMIT'S CHUNK, written a record at a time: the writer holds only what its
// own records said.
typedef struct {
    uint8_t *out;
    int      cap, len;
    int64_t  base;
    bool     any;       // a record has been put
    bool     ticked;    // this chunk has written a TICK, for tick_ms
    int64_t  tick_ms;
    int      prev;      // the last record's type, -1 before the first
    bool     broken;
} MoveListChunk;

void move_list_chunk_begin(MoveListChunk *w, uint8_t *out, int cap, int64_t base_ms);
// One session-log record (game.h LOG_*, its seat, and its pairs as wire bytes,
// primary then target, n_pairs of them) stamped `ms`. A GAME_START drops what
// the chunk held before it and begins a list. 0, or MOVE_LIST_E_CAP.
int move_list_chunk_put(MoveListChunk *w, int64_t ms, int type, int seat, int n_pairs, const uint8_t *pairs);

#endif

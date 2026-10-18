// THE WHOLE-GAME ENVELOPE of a Messages game whose every bubble carries the
// whole game: the seed, the roster of seat-tagged rows, and the body, the
// product's code of every move since the deal. An extension is handed exactly
// one message, the tapped one, so nothing is carried between bubbles but this.
//
// WHAT IS HERE is everything about such an envelope that is not the game:
//
//   - the roster half of a product's message (MsgWhole, the first member of a
//     product's message type): phase, two-person chat, the newest lobby bubble
//     being a leave, the lobby revision, the starter, the seed and the rows;
//   - the seat tag and the game id, SHA-256 over the product's salt and the
//     seed;
//   - the lobby kept in step with the rows (join, leave, the start marked),
//     with every verdict msg_lobby_roster's;
//   - the head and the roster on the wire, and the check over head and body;
//   - the link text ("?m=" and base32) and the reader that finds it in a URL;
//   - the race rule's lobby half and its last word, the smaller SHA-256;
//   - the seat witnesses (msg_seat_tag's resolver) and this device's seat
//     records, keyed by the game id.
//
// WHAT STAYS A PRODUCT'S: its salts, magic, format byte, flag bits and group
// capacity (MsgWholeSpec, one constant per product), the counters its head
// carries between the lobby revision and the roster (bubbles and turns, or
// moves), its game and its code, who sent a started game's bubble, and the
// race rule's game rules.
//
// THE LAYOUT (multi-byte fields little-endian; k is the spec's counters):
//
//   off      size  field
//   0        1     magic
//   1        1     format
//   2        1     phase      0 WAITING, 2 LIVE, 3 FINISHED
//   3        1     flags      the spec's DM and LEFT bits, the product's others
//   4        32    seed
//   36       2     lobby_rev  roster changes so far (joins and leaves)
//   38       2k    counters   the product's, 0 in WAITING
//   38+2k    1     n_seats    1..cap in WAITING, 2..cap once started
//   39+2k    1     starter    the seat that started it, 0xFF in WAITING
//   40+2k    var   roster     n_seats x { tag[9], u8 name_len (1..48), name }
//   var      2     check      wire_check over every other byte
//   var      var   body       the product's code, to the end; empty in WAITING
//
// Freestanding: no allocation, nothing from libc beyond memcpy, memset and
// memcmp. In its own directory beside its test so the builds that wildcard
// shared/c/*.c pick up neither.
#ifndef SHARED_MSG_WHOLE_H
#define SHARED_MSG_WHOLE_H

#include <stdint.h>
#include "../msg_seat_tag/msg_seat_tag.h"
#include "../msg_lobby_roster/msg_lobby_roster.h"

#define MSG_WHOLE_MAX_SEATS    MSG_LOBBY_ROSTER_MAX_SEATS
#define MSG_WHOLE_NO_SEAT      MSG_LOBBY_ROSTER_NO_SEAT
#define MSG_WHOLE_SEED_LEN     32
#define MSG_WHOLE_ID_LEN       8      // the game id a seat record is keyed by
#define MSG_WHOLE_CHECK_LEN    2
#define MSG_WHOLE_MAX_COUNTERS 2
#define MSG_WHOLE_ROW_MAX      (MSG_SEAT_TAG_LEN + 1 + MSG_SEAT_NAME_MAX_BYTES)
// The head before the roster, for a product with `k` counters.
#define MSG_WHOLE_HEAD_LEN(k)  (40 + 2 * (k))

#define MSG_WHOLE_WAITING  0
#define MSG_WHOLE_LIVE     2
#define MSG_WHOLE_FINISHED 3

// The refusals, negative. A product's own names are these values.
#define MSG_WHOLE_EOK      0
#define MSG_WHOLE_ESHORT   -1   // the buffer ends inside the header or the roster
#define MSG_WHOLE_EMAGIC   -2   // not one of ours
#define MSG_WHOLE_EFORMAT  -3   // a format this build does not know
#define MSG_WHOLE_EFLAGS   -4   // a reserved flag bit, or a flag the phase forbids
#define MSG_WHOLE_ECHECK   -5   // the check does not match: cut or edited
#define MSG_WHOLE_EGAME    -6   // the body does not replay, or disagrees with the header
#define MSG_WHOLE_EROSTER  -7   // empty or long name, duplicate tag or name, seats out of range
#define MSG_WHOLE_ECAP     -8   // output buffer too small
#define MSG_WHOLE_ETEXT    -9   // no "m=" in the text, or it is not base32
#define MSG_WHOLE_EREFUSED -10  // a lobby or seat verdict said no (not a wire error)

// The roster half of a message. A product's message type has this as its
// first member, `w`.
typedef struct {
    uint8_t  phase;                  // MSG_WHOLE_WAITING, _LIVE or _FINISHED
    uint8_t  dm;                     // a two-person chat: capacity 2
    uint8_t  left;                   // WAITING: the newest lobby bubble was a leave
    uint8_t  n_seats;
    uint16_t lobby_rev;
    uint8_t  starter;                // MSG_WHOLE_NO_SEAT while WAITING
    uint8_t  pad0;
    uint8_t  seed[MSG_WHOLE_SEED_LEN];
    MsgSeat  seat[MSG_WHOLE_MAX_SEATS];
} MsgWhole;

// One product's envelope: its constants, nothing else.
typedef struct {
    const char *seat_salt;           // the tag:     SHA-256(seat_salt || seed || id)[0..9)
    const char *game_salt;           // the game id: SHA-256(game_salt || seed)[0..8)
    uint8_t  magic, format;
    uint8_t  flag_dm, flag_left;     // the two flags every product carries
    uint8_t  flags_known;            // every flag bit this format defines
    uint8_t  flags_live;             // the bits only a started game may carry
    uint8_t  counters;               // u16 fields after lobby_rev: 1..MSG_WHOLE_MAX_COUNTERS
    uint8_t  group_cap;              // seats outside a two-person chat
    int      max_bytes;              // the longest envelope
    int      rec_len;                // a seat record: the id, the tag, then the product's bytes
    int      rec_bytes;              // the records' whole store: rec_len x the most kept
} MsgWholeSpec;

// ---- identity

void msg_whole_tag(const MsgWholeSpec *spec, const uint8_t seed[MSG_WHOLE_SEED_LEN],
                   const uint8_t *id, int id_len, uint8_t out[MSG_SEAT_TAG_LEN]);
void msg_whole_game_id(const MsgWholeSpec *spec, const uint8_t seed[MSG_WHOLE_SEED_LEN],
                       uint8_t out[MSG_WHOLE_ID_LEN]);

// ---- the lobby. Each leaves `w` untouched on refusal.

int  msg_whole_started(const MsgWhole *w);

// A new lobby: the creator in seat 0, who also sent it. EOK or EROSTER.
int  msg_whole_new(MsgWhole *w, const uint8_t seed[MSG_WHOLE_SEED_LEN], int dm,
                   const uint8_t tag[MSG_SEAT_TAG_LEN], const uint8_t *name, int name_len);

// The lobby as msg_lobby_roster sees it; who[s] is s + 1, a handle for the
// verdicts of this one roster, never compared across two.
void msg_whole_lobby(const MsgWholeSpec *spec, const MsgWhole *w, MsgLobbyRoster *l);

// Both lobbies of a roster change, each person known by their tag across the
// two: handles from the union of both rosters, in order of first appearance.
void msg_whole_lobby_pair(const MsgWholeSpec *spec, const MsgWhole *before, const MsgWhole *after,
                          MsgLobbyRoster *lb, MsgLobbyRoster *la);

// What `seat` (-1: not seated) is offered, 0 once started.
int  msg_whole_offered(const MsgWholeSpec *spec, const MsgWhole *w, int seat);

// The seat holding `tag`, or -1.
int  msg_whole_seat_of_tag(const MsgWhole *w, const uint8_t tag[MSG_SEAT_TAG_LEN]);

// Join: the lowest free seat, or EROSTER (a bad or taken name, a tag already
// seated) or EREFUSED (full or started).
int  msg_whole_join(const MsgWholeSpec *spec, MsgWhole *w, const uint8_t tag[MSG_SEAT_TAG_LEN],
                    const uint8_t *name, int name_len);

// Leave: later rows move down one seat. EOK or EREFUSED.
int  msg_whole_leave(const MsgWholeSpec *spec, MsgWhole *w, int seat);

// The start, once the product's own constructor has dealt: LIVE, started by
// `seat`, no longer a leave.
void msg_whole_mark_started(MsgWhole *w, int seat);

// ---- the bytes

// Everything a writer checks before it writes: the seat count for the phase
// and the capacity, every name good with no tag or name twice, the lobby
// revision agreeing with the count, and the starter (none while WAITING, a
// seat once started). EOK or EROSTER.
int  msg_whole_writable(const MsgWholeSpec *spec, const MsgWhole *w);

// The head and the roster at `buf`, which holds max_bytes: `phase` the byte
// to write, `flags` the product's own bits (the DM and LEFT bits are added
// here), `counters` the spec's count of them. The bytes written, which is
// where the check goes; the body goes after the check.
int  msg_whole_put_head(const MsgWholeSpec *spec, const MsgWhole *w, int phase, int flags,
                        const uint16_t *counters, uint8_t *buf);

// The check for a head of `head` bytes and the `body_len` bytes after the
// check. The envelope's whole length.
int  msg_whole_seal(uint8_t *buf, int head, int body_len);

// Read the head, the roster and the check of `n` bytes into `w` (zeroed
// first), the flags into *flags and the counters into counters[]. Every
// refusal a reader of the envelope makes before the body, in the order they
// are made: EOK with the body at in + *body_at for *body_len bytes, which is
// empty in WAITING and not empty once started. Never reads past in[n-1].
int  msg_whole_read(const MsgWholeSpec *spec, const uint8_t *in, int n, MsgWhole *w,
                    int *flags, uint16_t *counters, int *body_at, int *body_len);

// The same roster by everything that is not derived: what a writer was asked
// to write against what it reads back. The game is the product's to compare.
int  msg_whole_same(const MsgWhole *a, const MsgWhole *b);

// ---- the text a link carries: "?m=" + base32 of the bytes

// `n` bytes (a negative n is a refusal, handed back) as text. The length, or
// ECAP.
int  msg_whole_text_put(const uint8_t *bytes, int n, char *out, int cap);

// The value of `m` in a URL's query, after "?m=" or "&m=" up to the next '&'
// or '#', copied into `span` (span_cap bytes) and read as base32 into
// `bytes`. The byte count, or ETEXT.
int  msg_whole_text_get(const char *text, char *span, int span_cap, uint8_t *bytes, int cap);

// ---- two messages

int  msg_whole_same_game(const MsgWhole *a, const MsgWhole *b);

// The race rule's comparison: <0 mine, >0 tapped, 0 equal; the larger wins.
int  msg_whole_more(int mine, int tapped);

// The race rule's lobby step: the higher lobby_rev, then more seats.
int  msg_whole_prefer_lobby(const MsgWhole *mine, const MsgWhole *tapped);

// Its last word: the smaller SHA-256 of the envelope bytes, and a message
// that cannot be written (n < 0) loses.
int  msg_whole_prefer_bytes(const uint8_t *mine, int mine_n, const uint8_t *tapped, int tapped_n);

// ---- the seats

// Who sent a lobby bubble: the newest joiner, or -1 after a leave.
int  msg_whole_lobby_sender(const MsgWhole *w);

// Which row is this device (msg_seat_resolve), `sender` the product's.
int  msg_whole_resolve(const MsgWhole *w, int sender, int record, int tag_seat, int is_dm,
                       int i_sent, const uint8_t *name, int name_len, int *by);

// THE RECORDS: this device's seats, newest first, spec->rec_len bytes each:
// the game id, the tag it sat with, then the product's own bytes. A fixed
// layout, so the host stores the bytes as they are.

// The seat the recorded tag holds in this roster; -1 with no record of the
// game; MSG_SEAT_REC_GONE when there is one and no row carries its tag.
int  msg_whole_rec_find(const MsgWholeSpec *spec, const uint8_t *recs, int n, const MsgWhole *w);
// The offset of this game's record, or -1 (one record a game).
int  msg_whole_rec_at(const MsgWholeSpec *spec, const uint8_t *recs, int n, const MsgWhole *w);
// Drop this game's records. The new byte count.
int  msg_whole_rec_forget(const MsgWholeSpec *spec, uint8_t *recs, int n, const MsgWhole *w);
// Record `seat`'s tag for this game at the front, replacing its record and
// keeping that record's own bytes (zero for a new one); the oldest falls off.
// `recs` holds rec_bytes. The new byte count.
int  msg_whole_rec_put(const MsgWholeSpec *spec, uint8_t *recs, int n, const MsgWhole *w, int seat);
// A byte count held to whole records within the store.
int  msg_whole_rec_n(const MsgWholeSpec *spec, int n);

// ---- a bridge's own state: who this device is, what the host said about
// the bubble it hands over, and the resident roster's names for the words.

#define MSG_WHOLE_ID_MAX   64        // the bytes of a device id that are kept
#define MSG_WHOLE_TEXT_MAX 5000      // MSMessage.url's documented cap

typedef struct {
    uint8_t     id[MSG_WHOLE_ID_MAX];       // Messages' local participant id
    int         id_n;
    uint8_t     nick[MSG_SEAT_NAME_MAX_BYTES];  // the App Group nickname, if a good one
    int         nick_n;
    // THE SENDER FACT, about one message only: the host's word on who sent it
    char        sent_text[MSG_WHOLE_TEXT_MAX];
    int         sent_dm, sent_mine, sent_set;
    // the roster's names, NUL-terminated
    char        name[MSG_WHOLE_MAX_SEATS][MSG_SEAT_NAME_MAX_BYTES + 1];
    const char *names[MSG_WHOLE_MAX_SEATS];
} MsgWholeBridge;

// Who I am: an id (cut to MSG_WHOLE_ID_MAX), and a nickname, kept only when
// msg_seat_name_verdict passes it.
void msg_whole_bridge_id(MsgWholeBridge *b, const uint8_t *id, int n);
void msg_whole_bridge_nick(MsgWholeBridge *b, const uint8_t *name, int n);
// My tag in the game of `seed`.
void msg_whole_bridge_tag(const MsgWholeSpec *spec, const MsgWholeBridge *b,
                          const uint8_t seed[MSG_WHOLE_SEED_LEN], uint8_t tag[MSG_SEAT_TAG_LEN]);
// The longest link of a product: "?m=", base32 of max_bytes, and the NUL.
#define MSG_WHOLE_TEXT_LEN(max_bytes) (3 + ((max_bytes) * 8 + 4) / 5 + 1)

// The host says the bubble whose text is `text` was sent by me (i_sent 1) or
// not (0). Anything else, or a text longer than the product's longest link,
// clears the fact.
void msg_whole_bridge_sent(const MsgWholeSpec *spec, MsgWholeBridge *b, const char *text,
                           int is_dm, int i_sent);
// The fact for `text`: the host's word when it named exactly this text, else
// not a two-person chat and MSG_SEAT_SENT_UNKNOWN.
void msg_whole_bridge_sent_of(const MsgWholeBridge *b, const char *text, int *is_dm, int *i_sent);
// Which row of `w` is me, by the four witnesses: my record in `recs`, my tag,
// the sender fact against `sender` (the product's), my nickname. The seat or
// -1, the witness in *by.
int  msg_whole_bridge_seat(const MsgWholeSpec *spec, const MsgWholeBridge *b, const MsgWhole *w,
                           int sender, const uint8_t *recs, int rec_n, int is_dm, int i_sent, int *by);
// The roster's names for the words: every row of `w`, empty past its seats.
void msg_whole_bridge_names(MsgWholeBridge *b, const MsgWhole *w);

#endif

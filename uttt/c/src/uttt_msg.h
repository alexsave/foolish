/* The message: what a bubble carries, who sits where, and what the person
 * holding it may do. Shaped like foolish's msg_wire (c/src/msg_wire.h) - a
 * magic, a format byte, a versioned refusal, encode/decode with negative error
 * codes, and the seat and lobby verdicts as pure functions - but NOT sharing a
 * byte with it. foolish's formats live in sealed threads and are typed on
 * Durak; this game is two seats, no names, no hidden state and no chain.
 *
 * AN EXTENSION CANNOT ENUMERATE THE TRANSCRIPT. It is handed exactly one
 * message - the one that was tapped - so every bubble carries the whole game
 * (about 22 bytes) and its whole roster, and nothing is reconstructed.
 *
 * ---------------------------------------------------------------- the layout
 *
 *     [0]      UTM_MAGIC
 *     [1]      UTM_FORMAT (1)
 *     [2..5]   seed, int32 big-endian: the send time of the first empty board
 *     [6]      flags: bit 0 = sealed (the X seat is taken). Others reserved.
 *     [7..15]  O's seat tag - the creator's
 *     [16..24] X's seat tag - the joiner's, present only when sealed
 *     [+0..1]  check: the first two bytes of SHA-256 over every other byte
 *     [..end]  the game, as uttt_encode codes it (runs to the end)
 *
 * A SHIPPED BUBBLE LIVES FOREVER in somebody's transcript, so the format is
 * the second byte read and a reader that meets one it does not know refuses
 * rather than misreads. Nothing has shipped at format 1 yet; after TestFlight
 * it is frozen and a change is format 2.
 *
 * The check is there because the game code is a mixed-radix number with no
 * redundancy: a link cut short does not fail to decode, it decodes into a
 * DIFFERENT game. Two bytes turn that into a refusal.
 *
 * ------------------------------------------------------------- the protocol
 *
 * 1. A sends an empty board. The moment it is composed is the seed and the
 *    only thing that gets baked in; A's tag sits in the O seat.
 * 2. THE JOINER IS X AND MOVES FIRST (docs/UI.html, "Lobby and end"). Nobody
 *    rolls for it, so nobody can grind it - and taking the seat and making the
 *    first move are one act, so there is never a bubble whose only content is
 *    "I am here". An unsealed message has no plies and a sealed one has at
 *    least one; decode refuses anything else.
 * 3. Once X is taken the roster is sealed. A third tap in a group chat matches
 *    neither tag and is a spectator.
 */
#ifndef UTTT_MSG_H
#define UTTT_MSG_H

#include "uttt.h"
#include <stdint.h>

#define UTM_MAGIC     0xB7
#define UTM_FORMAT    1
#define UTM_TAG_LEN   9
#define UTM_CHECK_LEN 2
#define UTM_FLAG_SEALED 0x01
#define UTM_FLAGS_KNOWN (UTM_FLAG_SEALED)

#define UTM_HEAD_OPEN   (7 + UTM_TAG_LEN)
#define UTM_HEAD_SEALED (7 + 2 * UTM_TAG_LEN)
#define UTM_MAX_CODE    48                  /* uttt_code.c's CAP */
#define UTM_MAX_BYTES   (UTM_HEAD_SEALED + UTM_CHECK_LEN + UTM_MAX_CODE)
/* "?m=" + base32 + NUL */
#define UTM_MAX_TEXT    (3 + (UTM_MAX_BYTES * 8 + 4) / 5 + 1)

/* An identity is whatever bytes a device says it is (Messages' local
 * participant UUID, 16 bytes). It never reaches the wire - only its tag. */
#define UTM_MAX_ID      64

#define UTM_EOK      0
#define UTM_ESHORT  -1   /* buffer ends inside the header                 */
#define UTM_EMAGIC  -2   /* not one of ours                               */
#define UTM_EFORMAT -3   /* a format this build does not know             */
#define UTM_EFLAGS  -4   /* a reserved flag bit is set                    */
#define UTM_ECHECK  -5   /* the check does not match: cut or edited       */
#define UTM_EGAME   -6   /* the game code does not decode                 */
#define UTM_EROSTER -7   /* roster and game disagree, or X == O           */
#define UTM_ECAP    -8   /* output buffer too small                       */
#define UTM_ETEXT   -9   /* no "m=" in the text, or it is not base32      */

typedef struct {
    int32_t  seed;
    uint8_t  sealed;
    uint8_t  o[UTM_TAG_LEN];      /* the creator                          */
    uint8_t  x[UTM_TAG_LEN];      /* the joiner; meaningful iff sealed    */
    UtttGame game;
} UtmMsg;

/* ------------------------------------------------------------- identity */

/* A SEAT TAG: a value only the device that wrote it can recognise.
 *
 *     first 9 bytes of SHA-256("uttt.seat.1|" || seed as 4 BE bytes || id)
 *
 * Apple's participant UUIDs are per device per conversation, so a UUID on the
 * wire would be a value nobody else can compare with anything. That is
 * enough, because a seat only ever answers "is this me?" and the device
 * asking holds the UUID: it hashes its own and looks for the result. Salted
 * with the seed so one device has a different tag in every game and a tag
 * cannot follow a person from thread to thread.
 *
 * THE COST IS A REINSTALL: a new UUID matches neither seat, and its owner
 * becomes a spectator in their own sealed game (foolish's answer too:
 * exact or spectator, never a guess). */
void utm_tag(int32_t seed, const uint8_t *id, int id_len, uint8_t out[UTM_TAG_LEN]);

/* THE SEED IS THE SEND TIME OF THE FIRST EMPTY BOARD and nothing else - not
 * names, which their owners control. Unix seconds, truncated to 32 bits; 0 is
 * not a seed (the pen reads it as 1), so it becomes 1 here, once. */
int32_t utm_seed_at(int64_t unix_seconds);

/* ----------------------------------------------------------- the bytes */

/* An invitation: the empty board, `me` in the O seat, X open. */
void utm_open(UtmMsg *m, int32_t seed, const uint8_t me[UTM_TAG_LEN]);

/* Bytes written, or a negative UTM_E*. Refuses to write what decode would
 * refuse to read. */
int  utm_encode(const UtmMsg *m, uint8_t *out, int cap);
int  utm_decode(const uint8_t *in, int n, UtmMsg *out);

/* The text a link carries: "?m=" + base32 of the bytes. A bare query because
 * Messages silently DROPS an MSMessage.url whose scheme it will not vouch for
 * (uttt:// came back nil on the next line), and a query promises no
 * destination. The reader takes a whole URL string and finds the value. */
int  utm_text_encode(const UtmMsg *m, char *out, int cap);
int  utm_text_decode(const char *text, UtmMsg *out);

/* ------------------------------------------------------------ the seats */

#define UTM_SEAT_SPECTATOR 0   /* sealed, and neither tag is mine            */
#define UTM_SEAT_X         1   /* == UTTT_X                                  */
#define UTM_SEAT_O         2   /* == UTTT_O                                  */
#define UTM_SEAT_WAITING   3   /* my invitation, nobody has taken it         */
#define UTM_SEAT_OPEN      4   /* somebody's invitation: X is mine to take   */

/* WHICH SEAT AM I, for this message. The whole answer: no cache, no sender
 * signal, no DM inference, because every device can recompute its own tag.
 * A reinstalled creator on an open invitation resolves OPEN - with no tag to
 * match it is indistinguishable from anybody else in the thread. */
int  utm_seat(const UtmMsg *m, const uint8_t me[UTM_TAG_LEN]);

/* The mark a seat plays, or 0. OPEN plays X: taking the seat is the first
 * move. */
int  utm_seat_mark(int seat);

/* May `me` put a mark on this board now. */
int  utm_can_move(const UtmMsg *m, const uint8_t me[UTM_TAG_LEN]);

/* Play `mv` as `me`. On an OPEN invitation this TAKES THE SEAT: X becomes
 * `me`, the roster seals, and the move is played - one message. Returns 1 if
 * it was played; 0 leaves `m` untouched. */
int  utm_play(UtmMsg *m, const uint8_t me[UTM_TAG_LEN], int mv);

/* Take back `me`'s own last move - a staged bubble is a draft. Taking back the
 * joining move gives the seat back: the roster unseals and the invitation is
 * what is left. Returns 1 if a move came back. */
int  utm_undo(UtmMsg *m, const uint8_t me[UTM_TAG_LEN]);

/* A CHANGE OF MIND: may `me` replace their staged last move with `mv` - a
 * different square that is legal in the position the draft was played in.
 * The one legality question a tap on a board with a draft asks; anything
 * else (the same square, an occupied cell, a block the draft was not played
 * in, a gap between cells, a move that is not mine to take back) is a tap
 * that does nothing. Pure: `m` is not touched. */
int  utm_can_replace(const UtmMsg *m, const uint8_t me[UTM_TAG_LEN], int mv);

/* ------------------------------------------------------------ the doors */

#define UTM_DOOR_NONE      0
#define UTM_DOOR_AGAIN     1   /* the game is over: a fresh invitation        */

/* THE ONE DOOR A SCREEN MAY OFFER, and whether it may offer one at all.
 *
 * AGAIN is for anybody holding a finished game, seated or watching: the next
 * thing anybody does is ask for another (docs/UI.html, 06 and 07), and the
 * person who asks proposes, so they will move second. Where on the screen a
 * door may stand (the expanded view only) is layout, not this.
 *
 * THERE IS NO TAKE-BACK DOOR, by the owner's decision (2026-09-22), over
 * UI.html 02: the only way to change a move is to tap another square, which
 * replaces the staged draft, and the only undo is Messages' own X on the
 * draft (utm_undo). */
int  utm_door(const UtmMsg *m);

/* ------------------------------------------------ getting it into the field */

/* THE SEND HINT. A staged bubble nobody has sent stalls the thread, so after
 * this long unsent in the compact drawer an arrow bobs under Messages' own
 * Send button (shared/swift/MessagesKit's SendHint). A new stage restarts the
 * wait; a send, a cancel or the drawer growing hides it. The sister product's
 * number, so the two games feel the same under a thumb. */
#define UTM_SEND_HINT_MS       3000

/* A REFUSED INSERT NEVER ANSWERS. ChatKit drops an insert that arrives before
 * the host counts the drawer as presenting, and calls no completion at all -
 * not with an error, not ever (docs/INSERT_GATING.md). So silence is the
 * refusal: an insert unanswered after UTM_INSERT_SILENCE_MS is asked again,
 * up to UTM_INSERT_ATTEMPTS in all (about five seconds), and then the human
 * is handed a door that inserts on a tap - by which time the drawer is
 * presenting and the gate passes. */
#define UTM_INSERT_SILENCE_MS  500
#define UTM_INSERT_ATTEMPTS    10

#define UTM_INSERT_LISTEN      0   /* keep waiting; this silence is not a refusal */
#define UTM_INSERT_RETRY       1   /* insert the same bubble again               */
#define UTM_INSERT_DOOR        2   /* stop asking; offer the one-tap door        */

/* What an insert that has gone UTM_INSERT_SILENCE_MS without an answer means,
 * on its `attempt`th try (1-based). ONLY THE COMPACT DRAWER'S SILENCE COUNTS:
 * expanded, the host deliberately parks an accepted insert's completion until
 * later, so a watchdog there would take a yes for a no - it listens and
 * counts nothing. */
int  utm_insert_silence(int attempt, int compact);

/* ------------------------------------------------------- two messages */

/* The same game: the same invitation (seed and creator). */
int  utm_same_game(const UtmMsg *a, const UtmMsg *b);

/* WHICH OF TWO BUBBLES TO SHOW - `mine`, the device's own newest (its staged
 * draft), or `tapped`, the one Messages handed over. <0 mine, >0 tapped,
 * 0 identical. Delivery order is never an input, so two devices holding the
 * same pair agree.
 *
 *   - Different games: the tapped one. Tapping an older game has to win.
 *   - A sealed game beats its own invitation.
 *   - More plies wins (foolish's Rule P turn rule).
 *   - Equal plies, ONE ROSTER: `mine`. The only way two same-length siblings
 *     of one roster meet on a device is that device's own change of mind, and
 *     the draft is the newer mind.
 *   - Equal plies, TWO JOINERS - two people replying at once in a group: the
 *     lower join key wins (utm_join_key). Not first by clock, because clocks
 *     disagree and the loser must never see themselves seated. */
int  utm_prefer(const UtmMsg *mine, const UtmMsg *tapped);

/* SHA-256("uttt.join.1|" || seed || X's tag || the first move): the replier
 * and their message, fixed for the life of that fork, so ANY later bubble of
 * one fork compares the same way against any bubble of the other. Only
 * meaningful when sealed. */
void utm_join_key(const UtmMsg *m, uint8_t out[32]);

#endif

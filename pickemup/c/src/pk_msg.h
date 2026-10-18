/* Pick 'Em Up - the envelope: what a bubble carries, who sits where, and which
 * of two bubbles wins (RULES_AND_KERNEL.md section 4).
 *
 * THE SHAPE IS UTTT'S (uttt/c/src/uttt_msg.h) with foolish's lobby and seat
 * rules (foolish/c/src/msg_wire.h): a magic, a format byte, a versioned
 * refusal, encode and decode with negative error codes, and every seat and
 * lobby verdict a pure function. The lobby's arithmetic is pk_lobby.h's
 * (D29); this file keeps the tags and the names and calls it.
 *
 * AN EXTENSION IS HANDED EXACTLY ONE MESSAGE, the tapped one, so every bubble
 * carries the whole game (D25): the seed, the roster, and the body, the
 * mixed-radix code of every choice since the deal (pk_code.h). Decoding is
 * replay through pk_apply, so an illegal or tampered choice simply fails.
 *
 * ---------------------------------------------------------------- the layout
 * (format 1; multi-byte fields little-endian)
 *
 *   off  size  field
 *   0    1     magic       0xB9
 *   1    1     format      1
 *   2    1     phase       0 WAITING, 2 LIVE, 3 FINISHED
 *   3    1     flags       bit0 DM          (lobby capacity 2, else 8)
 *                          bit1 TIP_SAID    (the newest bubble holds "Last card!")
 *                          bit2 LEFT        (the newest lobby bubble was a leave; D41)
 *                          bits 3-7 reserved = 0, refused if set
 *   4    32    seed
 *   36   2     lobby_rev   roster changes so far (joins and leaves)
 *   38   2     bubbles     sealed bubbles since the deal (0 in WAITING)
 *   40   2     turns       completed turns (0 in WAITING)
 *   42   1     n_seats     1..cap in WAITING, 2..cap once started
 *   43   1     starter     the seat that started it, 0xFF in WAITING (D41)
 *   44   var   roster      n_seats x { tag[9], u8 name_len (1..48), name }
 *   var  2     check       first 2 bytes of SHA-256 over every other byte
 *   var  var   body        pk_code's bytes, to the end; empty in WAITING
 *
 * PHASE, BUBBLES, TURNS AND TIP_SAID ARE DERIVED from the game when the
 * envelope is written, and a decode refuses a header that disagrees with its
 * own replay (PK_EGAME), so a host cannot emit a payload it would refuse.
 * The check turns a cut or edited link into a refusal rather than a different
 * game, because a mixed-radix code has no redundancy.
 *
 * A SHIPPED BUBBLE LIVES FOREVER in somebody's transcript: the format is the
 * second byte read, and a reader that meets one it does not know refuses
 * (PK_EFORMAT) rather than misreads. A rules change that would deal or play
 * any existing code differently is a new format, with no migration (4.7). */
#ifndef PK_MSG_H
#define PK_MSG_H

#include "pk.h"
#include "pk_lobby.h"
#include "pk_plan.h"
#include "pk_code.h"
#include "../../../shared/c/msg_whole/msg_whole.h"

#define PK_MSG_MAGIC      0xB9
#define PK_MSG_FORMAT     1

#define PK_PHASE_WAITING  MSG_WHOLE_WAITING
#define PK_PHASE_LIVE     MSG_WHOLE_LIVE
#define PK_PHASE_FINISHED MSG_WHOLE_FINISHED

#define PK_FLAG_DM        0x01
#define PK_FLAG_TIP_SAID  0x02
#define PK_FLAG_LEFT      0x04
#define PK_FLAGS_KNOWN    (PK_FLAG_DM | PK_FLAG_TIP_SAID | PK_FLAG_LEFT)

#define PK_TAG_LEN        MSG_SEAT_TAG_LEN
#define PK_NAME_MAX_BYTES MSG_SEAT_NAME_MAX_BYTES  /* D23 */
#define PK_NAME_MAX_CHARS MSG_SEAT_NAME_MAX_CHARS  /* D23; the seat-name clipping cap */
#define PK_CHECK_LEN      MSG_WHOLE_CHECK_LEN
#define PK_HEAD_LEN       MSG_WHOLE_HEAD_LEN(2)   /* 44 */
#define PK_ROW_MAX        MSG_WHOLE_ROW_MAX
#define PK_MSG_MAX_BYTES  (PK_HEAD_LEN + PK_MAX_SEATS * PK_ROW_MAX + PK_CHECK_LEN + PK_CODE_MAX)
/* "?m=" + base32 + NUL */
#define PK_MSG_MAX_TEXT   MSG_WHOLE_TEXT_LEN(PK_MSG_MAX_BYTES)

/* EVERY GAME FITS ONE MESSAGE (D23, 4.5): the capped worst case, every name
 * at 48 bytes and the body at its bound, is under the 5,000 characters Apple
 * documents for MSMessage.url. Raise a cap past this and the build stops. */
_Static_assert(PK_MSG_MAX_TEXT - 1 < 5000, "the capped worst case fits MSMessage.url");

/* Errors (4.3), each one msg_whole.h says. */
#define PK_EOK      MSG_WHOLE_EOK
#define PK_ESHORT   MSG_WHOLE_ESHORT
#define PK_EMAGIC   MSG_WHOLE_EMAGIC
#define PK_EFORMAT  MSG_WHOLE_EFORMAT
#define PK_EFLAGS   MSG_WHOLE_EFLAGS
#define PK_ECHECK   MSG_WHOLE_ECHECK
#define PK_EGAME    MSG_WHOLE_EGAME
#define PK_EROSTER  MSG_WHOLE_EROSTER
#define PK_ECAP     MSG_WHOLE_ECAP
#define PK_ETEXT    MSG_WHOLE_ETEXT
#define PK_EREFUSED MSG_WHOLE_EREFUSED

/* A roster row, shared/c/msg_seat_tag's: the tag is
 * SHA-256("pickemup.seat.1|" || seed || id)[0..9), then a 1..48-byte UTF-8 name. */
typedef MsgSeat PkSeat;
_Static_assert(PK_MAX_SEATS <= MSG_WHOLE_MAX_SEATS, "the shared roster seats the whole table");
_Static_assert(PK_HEAD_LEN == 44, "the head is format 1's");

typedef struct {
    MsgWhole w;                       /* the roster half: phase, dm, left, lobby_rev,
                                         n_seats, starter, seed, seat[] (msg_whole.h) */
    uint8_t  tip_said;                /* derived: the newest bubble said it          */
    PkGame   game;                    /* the table once started (phase LIVE / FINISHED) */
} PkMsg;

/* This product's envelope: its salts, magic, flags, counters and caps
 * (shared/c/msg_whole). */
extern const MsgWholeSpec PK_MSG_SPEC;

/* ------------------------------------------------------------- identity */

/* A SEAT TAG: a value only the device that wrote it can recognise (UTTT's,
 * utm_tag), salted by the game so it cannot follow a person between threads.
 * `id` is whatever bytes the device says it is (Messages' local participant
 * id); it never reaches the wire. */
void pk_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[PK_TAG_LEN]);

/* The game's identity (D21): SHA-256("pickemup.game.1|" || seed)[0..8). */
void pk_game_id(const uint8_t seed[32], uint8_t out[8]);

/* A NICKNAME, JUDGED (foolish's msg_nickname_verdict) by shared/c/msg_seat_tag's
 * msg_seat_name_verdict. The host trims; the caps are there. Rejects rather
 * than truncates. */
#define PK_NAME_OK       MSG_SEAT_NAME_OK
#define PK_NAME_EMPTY    MSG_SEAT_NAME_EMPTY
#define PK_NAME_TOO_LONG MSG_SEAT_NAME_TOO_LONG
#define PK_NAME_BAD      MSG_SEAT_NAME_BAD

/* ------------------------------------------------------------ the lobby
 *
 * Every verdict is pk_lobby.h's; these keep the roster rows in step. Each
 * returns 0 (PK_EOK) or a negative PK_E* and leaves `m` untouched on refusal. */

/* A new lobby (4.6.1): the creator in seat 0, who also sent it. */
int pk_msg_new(PkMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[PK_TAG_LEN],
               const uint8_t *name, int name_len);

/* The lobby as the verdict functions see it. who[s] is s + 1: a handle for
 * the verdicts of this one roster, never compared across two. */
void pk_msg_lobby(const PkMsg *m, PkLobby *l);

/* What `seat` (-1: not seated) is offered (PK_LOBBY_*), 0 once started. */
int  pk_msg_offered(const PkMsg *m, int seat);

/* Join (4.6.2): the lowest free seat. The seat, or a negative PK_E*: ROSTER
 * for a bad or taken name or a tag already seated, REFUSED when full or
 * started. */
int  pk_msg_join(PkMsg *m, const uint8_t tag[PK_TAG_LEN], const uint8_t *name, int name_len);

/* Leave (4.6.3): later rows move down one seat. */
int  pk_msg_leave(PkMsg *m, int seat);

/* Start (4.6.4): `seat` must be offered START. Deals at n = n_seats. */
int  pk_msg_start(PkMsg *m, int seat);

/* Join and start in one bubble (4.6.5): exactly when the join fills the
 * table. The joiner's seat, or a negative PK_E*. */
int  pk_msg_join_start(PkMsg *m, const uint8_t tag[PK_TAG_LEN], const uint8_t *name, int name_len);

/* The roster's events between two bubbles of one lobby (LOBBY_LEAVE and
 * LOBBY_JOIN), with each person known by their tag across the two. */
int  pk_msg_plan_lobby(const PkMsg *before, const PkMsg *after, PkEvent *out, int cap);

/* ----------------------------------------------------------- the bytes */

/* Bytes written, or a negative PK_E*. The game must have no draft open (a
 * host seals a copy of its draft, pk_seal, before it encodes). The written
 * bytes are decoded again before they are returned. */
int pk_msg_encode(const PkMsg *m, uint8_t *out, int cap);

/* Read `n` bytes. Never reads past in[n-1]. PK_EOK, or a negative PK_E* with
 * `out` untouched. */
int pk_msg_decode(const uint8_t *in, int n, PkMsg *out);

/* The text a link carries: "?m=" + base32 of the bytes (UTTT's, 4.1). The
 * reader takes a whole URL string and finds the value. */
int pk_msg_text_encode(const PkMsg *m, char *out, int cap);
int pk_msg_text_decode(const char *text, PkMsg *out);

/* ------------------------------------------------------- two messages (4.8) */

int pk_msg_same_game(const PkMsg *a, const PkMsg *b);

/* WHICH OF TWO BUBBLES TO SHOW: <0 mine, >0 tapped, 0 identical. Delivery
 * order is never an input, so every phone holding the same pair agrees.
 *   1 different games: the tapped one
 *   2 a started chain beats a WAITING one
 *   3 more turns
 *   4 more bubbles
 *   5 TIP_SAID beats not (D26)
 *   6 higher lobby_rev, then more seats
 *   7 the smaller SHA-256 of the envelope bytes */
int pk_msg_prefer(const PkMsg *mine, const PkMsg *tapped);

/* The number of sealed bubbles two chains of one game share: what a host
 * takes back and what it plays forward after a lost race. 0 for different
 * games or a lobby. */
int pk_common_bubbles(const PkMsg *a, const PkMsg *b);

/* ------------------------------------------------------------ the seats (4.6)
 *
 * WHICH SEAT AM I: UTTT's three witnesses in UTTT's order (utm_resolve), then
 * foolish's nickname as the last one:
 *
 *   (a) THE RECORD - this device's own note of the tag it sat with, keyed by
 *       the game (pk_rec_*). A new participant id cannot miss it, and it names
 *       a tag, not a seat number, so a leave that moves rows down still finds
 *       the row (D42).
 *   (b) THE TAG - my identity hashed with this seed, found in the roster.
 *   (c) THE SENDER - i_sent 1: I am the seat that sent this bubble (the
 *       newest lobby joiner, the starter of a start bubble, or the sender of
 *       the newest sealed bubble); i_sent 0 in a two-person chat of two
 *       seats: the other seat.
 *   (d) THE NAME - my App Group nickname, if exactly one row carries it.
 *
 * A RECORD THAT FINDS NO ROW (PK_REC_GONE) IS A WITNESS TOO: this device sat
 * in this game under a tag no row carries, so it is not seated here, and
 * neither the sender nor the name may say otherwise (D51). Only the tag, a
 * direct witness, still counts.
 *
 * THE LOBBY GATE (foolish msg_seat_resolve_in_lobby): in a WAITING bubble a
 * seat found by the sender witness counts only if the row carries my name
 * when I have one, because a lobby seat is claimed by a named join and an
 * inference cannot overrule "none of these rows is me". */
#define PK_BY_NONE   MSG_SEAT_BY_NONE
#define PK_BY_RECORD MSG_SEAT_BY_RECORD
#define PK_BY_TAG    MSG_SEAT_BY_TAG
#define PK_BY_SENDER MSG_SEAT_BY_SENDER
#define PK_BY_NAME   MSG_SEAT_BY_NAME

#define PK_SENT_UNKNOWN MSG_SEAT_SENT_UNKNOWN

/* The seat holding `tag`, or -1. */
int pk_msg_seat_of_tag(const PkMsg *m, const uint8_t tag[PK_TAG_LEN]);

/* The seat that sent this bubble, or -1 (a leave: the sender has no seat). */
int pk_msg_sender(const PkMsg *m);

/* `record` is pk_rec_find's answer (a seat, -1 or PK_REC_GONE) and
 * `tag_seat` a seat or -1. The seat, or -1 for "not
 * seated here" (a spectator, or a lobby I may join); the deciding witness in
 * *by (may be NULL). */
int pk_msg_resolve(const PkMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by);

/* THE RECORDS: this device's seats, newest first, PK_REC_LEN bytes each - the
 * game id and the tag it sat with. A fixed layout, so the host stores the
 * bytes as they are. */
#define PK_REC_LEN   (8 + PK_TAG_LEN)
#define PK_REC_MAX   256
#define PK_REC_BYTES (PK_REC_LEN * PK_REC_MAX)

/* The seat my recorded tag holds in this roster; -1 when this device has no
 * record of this game; PK_REC_GONE when it has one and no row carries its
 * tag - I sat and I left, or this bubble is from before I joined (D51). */
#define PK_REC_GONE  MSG_SEAT_REC_GONE
int pk_rec_find(const uint8_t *recs, int n, const PkMsg *m);
/* Record `seat`'s tag for this game at the front, replacing any record of
 * it; the oldest fall off. `recs` holds PK_REC_BYTES. The new byte count. */
int pk_rec_put(uint8_t *recs, int n, const PkMsg *m, int seat);
/* Drop this game's records. The new byte count. */
int pk_rec_forget(uint8_t *recs, int n, const PkMsg *m);

#endif

/* Tallybones - the envelope: what a bubble carries, who sits where, and which
 * of two bubbles wins (DECISIONS.md T7).
 *
 * PICK 'EM UP'S ENVELOPE, COPIED AND SHRUNK (pickemup/c/src/pk_msg.h, whose
 * D-numbers below are Pick 'Em Up's): a magic, a format byte, a versioned
 * refusal, encode and decode with negative error codes, and every seat and
 * lobby verdict a pure function. The lobby's arithmetic is tb_lobby.h's.
 *
 * AN EXTENSION IS HANDED EXACTLY ONE MESSAGE, the tapped one, so every bubble
 * carries the whole game: the seed, the roster, and the body, the mixed-radix
 * code of every move since the start (tb_code.h). Decoding is the RESIDENT
 * replay, so an illegal or tampered move simply fails, and every die is
 * derived there and nowhere else (T11). THE WIRE CARRIES NO DICE VALUES.
 *
 * ---------------------------------------------------------------- the layout
 * (format 1; multi-byte fields little-endian)
 *
 *   off  size  field
 *   0    1     magic       0xD7
 *   1    1     format      1
 *   2    1     phase       0 WAITING, 2 LIVE, 3 FINISHED
 *   3    1     flags       bit0 DM          (lobby capacity 2, else 8)
 *                          bit1 LEFT        (the newest lobby bubble was a leave)
 *                          bits 2-7 reserved = 0, refused if set
 *   4    32    seed
 *   36   2     lobby_rev   roster changes so far (joins and leaves)
 *   38   2     bubbles     moves since the start (0 in WAITING)
 *   40   2     turns       completed turns, the SCOREs (0 in WAITING)
 *   42   1     n_seats     1..cap in WAITING, 2..cap once started
 *   43   1     starter     the seat that started it, 0xFF in WAITING
 *   44   var   roster      n_seats x { tag[9], u8 name_len (1..48), name }
 *   var  2     check       first 2 bytes of SHA-256 over every other byte
 *   var  var   body        tb_code's bytes, to the end; empty in WAITING
 *
 * PHASE, BUBBLES AND TURNS ARE DERIVED from the game when the envelope is
 * written, and a decode refuses a header that disagrees with its own replay
 * (TB_EGAME), so a host cannot emit a payload it would refuse.
 *
 * A SHIPPED BUBBLE LIVES FOREVER in somebody's transcript: the format is the
 * second byte read, and a reader that meets one it does not know refuses
 * (TB_EFORMAT) rather than misreads. */
#ifndef TB_MSG_H
#define TB_MSG_H

#include "tb.h"
#include "tb_lobby.h"
#include "tb_plan.h"
#include "tb_code.h"

#define TB_MSG_MAGIC      0xD7
#define TB_MSG_FORMAT     1

#define TB_PHASE_WAITING  0
#define TB_PHASE_LIVE     2
#define TB_PHASE_FINISHED 3

#define TB_FLAG_DM        0x01
#define TB_FLAG_LEFT      0x02
#define TB_FLAGS_KNOWN    (TB_FLAG_DM | TB_FLAG_LEFT)

#define TB_TAG_LEN        9
#define TB_NAME_MAX_BYTES 48          /* D23 */
#define TB_NAME_MAX_CHARS 16          /* D23; the seat-name clipping cap */
#define TB_CHECK_LEN      2
#define TB_HEAD_LEN       44
#define TB_ROW_MAX        (TB_TAG_LEN + 1 + TB_NAME_MAX_BYTES)
#define TB_MSG_MAX_BYTES  (TB_HEAD_LEN + TB_MAX_SEATS * TB_ROW_MAX + TB_CHECK_LEN + TB_CODE_MAX)
/* "?m=" + base32 + NUL */
#define TB_MSG_MAX_TEXT   (3 + (TB_MSG_MAX_BYTES * 8 + 4) / 5 + 1)

/* EVERY GAME FITS ONE MESSAGE (T7): the capped worst case, every name at 48
 * bytes and the body at its bound (319 bubbles of 6 bits), is under the 5,000
 * characters Apple documents for MSMessage.url. Raise a cap past this and the
 * build stops. */
_Static_assert(TB_MSG_MAX_TEXT - 1 < 5000, "the capped worst case fits MSMessage.url");

/* Errors. */
#define TB_EOK      0
#define TB_ESHORT  -1    /* the buffer ends inside the header or the roster  */
#define TB_EMAGIC  -2    /* not one of ours                                  */
#define TB_EFORMAT -3    /* a format this build does not know                */
#define TB_EFLAGS  -4    /* a reserved flag bit, or a flag the phase forbids */
#define TB_ECHECK  -5    /* the check does not match: cut or edited          */
#define TB_EGAME   -6    /* the body does not replay, or disagrees with the header */
#define TB_EROSTER -7    /* empty or long name, duplicate tag or name, seats out of range */
#define TB_ECAP    -8    /* output buffer too small                          */
#define TB_ETEXT   -9    /* no "m=" in the text, or it is not base32         */
#define TB_EREFUSED -10  /* a lobby or seat verdict said no (not a wire error, D41) */
#define TB_ESTAGED  -11  /* the bridge: this is my own staged, unsent bubble (T15) */

typedef struct {
    uint8_t tag[TB_TAG_LEN];          /* SHA-256("tallybones.seat.1|" || seed || id)[0..9) */
    uint8_t name_len;                 /* 1..48 bytes of UTF-8                            */
    uint8_t name[TB_NAME_MAX_BYTES];
} TbSeat;

typedef struct {
    uint8_t  phase;                   /* TB_PHASE_*                                  */
    uint8_t  dm;
    uint8_t  left;                    /* WAITING: the newest lobby bubble was a leave */
    uint16_t lobby_rev;
    uint8_t  n_seats;
    uint8_t  starter;                 /* TB_SEAT_NONE while WAITING                  */
    uint8_t  seed[32];
    TbSeat   seat[TB_MAX_SEATS];
    TbGame   game;                    /* the table once started (LIVE / FINISHED); may be
                                         a draft, which encode writes and decode never is */
} TbMsg;

/* ------------------------------------------------------------- identity */

/* A SEAT TAG: a value only the device that wrote it can recognise (UTTT's,
 * utm_tag), salted by the game so it cannot follow a person between threads.
 * `id` is whatever bytes the device says it is (Messages' local participant
 * id); it never reaches the wire. */
void tb_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[TB_TAG_LEN]);

/* The game's identity (D21): SHA-256("tallybones.game.1|" || seed)[0..8). */
void tb_game_id(const uint8_t seed[32], uint8_t out[8]);

/* A NICKNAME, JUDGED (foolish's msg_nickname_verdict). The host trims; the
 * caps are here. Rejects rather than truncates. */
#define TB_NAME_OK       0
#define TB_NAME_EMPTY    1
#define TB_NAME_TOO_LONG 2
#define TB_NAME_BAD      3     /* not UTF-8, or a control character */
int tb_name_verdict(const uint8_t *name, int len);

/* ------------------------------------------------------------ the lobby
 *
 * Every verdict is tb_lobby.h's; these keep the roster rows in step. Each
 * returns 0 (TB_EOK) or a negative TB_E* and leaves `m` untouched on refusal. */

/* A new lobby: the creator in seat 0, who also sent it. */
int tb_msg_new(TbMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[TB_TAG_LEN],
               const uint8_t *name, int name_len);

/* The lobby as the verdict functions see it. who[s] is s + 1: a handle for
 * the verdicts of this one roster, never compared across two. */
void tb_msg_lobby(const TbMsg *m, TbLobby *l);

/* What `seat` (-1: not seated) is offered (TB_LOBBY_*), 0 once started. */
int  tb_msg_offered(const TbMsg *m, int seat);

/* Join: the lowest free seat. The seat, or a negative TB_E*: ROSTER
 * for a bad or taken name or a tag already seated, REFUSED when full or
 * started. */
int  tb_msg_join(TbMsg *m, const uint8_t tag[TB_TAG_LEN], const uint8_t *name, int name_len);

/* Leave: later rows move down one seat. */
int  tb_msg_leave(TbMsg *m, int seat);

/* Start: `seat` must be offered START. A game at n = n_seats. */
int  tb_msg_start(TbMsg *m, int seat);

/* Join and start in one bubble: exactly when the join fills the
 * table. The joiner's seat, or a negative TB_E*. */
int  tb_msg_join_start(TbMsg *m, const uint8_t tag[TB_TAG_LEN], const uint8_t *name, int name_len);

/* The roster's events between two bubbles of one lobby (LOBBY_LEAVE and
 * LOBBY_JOIN), with each person known by their tag across the two. */
int  tb_msg_plan_lobby(const TbMsg *before, const TbMsg *after, TbEvent *out, int cap);

/* ----------------------------------------------------------- the bytes */

/* Bytes written, or a negative TB_E*. A draft game is written with its
 * pending move as the newest bubble: that is the bubble a host sends. The
 * written bytes are decoded again before they are returned, WITHOUT deriving
 * (the read-back of a draft must not roll it), and must say the same moves. */
int tb_msg_encode(const TbMsg *m, uint8_t *out, int cap);

/* Read `n` bytes: the RESIDENT replay, every roll derived. Never reads past
 * in[n-1]. TB_EOK, or a negative TB_E* with `out` untouched. */
int tb_msg_decode(const uint8_t *in, int n, TbMsg *out);

/* The text a link carries: "?m=" + base32 of the bytes (UTTT's). The
 * reader takes a whole URL string and finds the value. */
int tb_msg_text_encode(const TbMsg *m, char *out, int cap);
int tb_msg_text_decode(const char *text, TbMsg *out);
/* The same read WITHOUT deriving: the roster and the moves, every die 0.
 * For comparing two chains (which is newer, what they share, whether a link
 * is my own staged bubble), never for showing a game. */
int tb_msg_text_peek(const char *text, TbMsg *out);

/* ------------------------------------------------------- two messages */

int tb_msg_same_game(const TbMsg *a, const TbMsg *b);

/* WHICH OF TWO BUBBLES TO SHOW: <0 mine, >0 tapped, 0 identical. Delivery
 * order is never an input, so every phone holding the same pair agrees.
 *   1 different games: the tapped one
 *   2 a started chain beats a WAITING one
 *   3 more turns
 *   4 more bubbles: consecutive bubbles from one seat in a turn (KEEP, KEEP,
 *     SCORE) are one chain growing, never a race with itself (T11)
 *   5 higher lobby_rev, then more seats
 *   6 the smaller SHA-256 of the envelope bytes */
int tb_msg_prefer(const TbMsg *mine, const TbMsg *tapped);

/* The number of bubbles (moves) two chains of one game share: what a host
 * plays forward after a lost race. 0 for different games or a lobby. */
int tb_common_bubbles(const TbMsg *a, const TbMsg *b);

/* ------------------------------------------------------------ the seats
 *
 * WHICH SEAT AM I: UTTT's three witnesses in UTTT's order (utm_resolve), then
 * foolish's nickname as the last one:
 *
 *   (a) THE RECORD - this device's own note of the tag it sat with, keyed by
 *       the game (tb_rec_*). A new participant id cannot miss it, and it names
 *       a tag, not a seat number, so a leave that moves rows down still finds
 *       the row (D42).
 *   (b) THE TAG - my identity hashed with this seed, found in the roster.
 *   (c) THE SENDER - i_sent 1: I am the seat that sent this bubble (the
 *       newest lobby joiner, the starter of a start bubble, or the seat of
 *       the newest move); i_sent 0 in a two-person chat of two
 *       seats: the other seat.
 *   (d) THE NAME - my App Group nickname, if exactly one row carries it.
 *
 * A RECORD THAT FINDS NO ROW (TB_REC_GONE) IS A WITNESS TOO: this device sat
 * in this game under a tag no row carries, so it is not seated here, and
 * neither the sender nor the name may say otherwise (D51). Only the tag, a
 * direct witness, still counts.
 *
 * THE LOBBY GATE (foolish msg_seat_resolve_in_lobby): in a WAITING bubble a
 * seat found by the sender witness counts only if the row carries my name
 * when I have one, because a lobby seat is claimed by a named join and an
 * inference cannot overrule "none of these rows is me". */
#define TB_BY_NONE   0
#define TB_BY_RECORD 1
#define TB_BY_TAG    2
#define TB_BY_SENDER 3
#define TB_BY_NAME   4

#define TB_SENT_UNKNOWN (-1)

/* The seat holding `tag`, or -1. */
int tb_msg_seat_of_tag(const TbMsg *m, const uint8_t tag[TB_TAG_LEN]);

/* The seat that sent this bubble, or -1 (a leave: the sender has no seat). */
int tb_msg_sender(const TbMsg *m);

/* `record` is tb_rec_find's answer (a seat, -1 or TB_REC_GONE) and
 * `tag_seat` a seat or -1. The seat, or -1 for "not
 * seated here" (a spectator, or a lobby I may join); the deciding witness in
 * *by (may be NULL). */
int tb_msg_resolve(const TbMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by);

/* THE RECORDS: this device's seats, newest first, TB_REC_LEN bytes each - the
 * game id and the tag it sat with. A fixed layout, so the host stores the
 * bytes as they are. */
#define TB_REC_LEN   (8 + TB_TAG_LEN)
#define TB_REC_MAX   256
#define TB_REC_BYTES (TB_REC_LEN * TB_REC_MAX)

/* The seat my recorded tag holds in this roster; -1 when this device has no
 * record of this game; TB_REC_GONE when it has one and no row carries its
 * tag - I sat and I left, or this bubble is from before I joined (D51). */
#define TB_REC_GONE  (-2)
int tb_rec_find(const uint8_t *recs, int n, const TbMsg *m);
/* Record `seat`'s tag for this game at the front, replacing any record of
 * it; the oldest fall off. `recs` holds TB_REC_BYTES. The new byte count. */
int tb_rec_put(uint8_t *recs, int n, const TbMsg *m, int seat);
/* Drop this game's records. The new byte count. */
int tb_rec_forget(uint8_t *recs, int n, const TbMsg *m);

#endif

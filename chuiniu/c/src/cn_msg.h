/* Chui Niu - the envelope: what a bubble carries, who sits where, and which
 * of two bubbles wins.
 *
 * PICK 'EM UP'S ENVELOPE (pickemup/c/src/pk_msg.h, its D25) cut to a dice
 * game: an extension is handed exactly one message, the tapped one, so every
 * bubble carries the whole game - the seed, the roster and the body, the
 * mixed-radix code of every move since the start (cn_code.h). Decoding is
 * replay through cn_apply, so an illegal or tampered move simply fails. The
 * dice are never here: they are derived from what is (K2).
 *
 * ---------------------------------------------------------------- the layout
 * (format 1; multi-byte fields little-endian)
 *
 *   off  size  field
 *   0    1     magic       0xC5
 *   1    1     format      1
 *   2    1     phase       0 WAITING, 2 LIVE, 3 FINISHED
 *   3    1     flags       bit0 DM    (lobby capacity 2, else 6)
 *                          bit1 LEFT  (the newest lobby bubble was a leave)
 *                          bits 2-7 reserved = 0, refused if set
 *   4    32    seed
 *   36   2     lobby_rev   roster changes so far (joins and leaves)
 *   38   2     moves       moves since the start (0 in WAITING)
 *   40   1     n_seats     1..cap in WAITING, 2..cap once started
 *   41   1     starter     the seat that pressed Start, 0xFF in WAITING
 *   42   var   roster      n_seats x { tag[9], u8 name_len (1..48), name }
 *   var  2     check       first 2 bytes of SHA-256 over every other byte
 *   var  var   body        cn_code's bytes, to the end; empty in WAITING
 *
 * PHASE AND MOVES ARE DERIVED from the game when the envelope is written,
 * and a decode refuses a header that disagrees with its own replay
 * (CN_EGAME). The check turns a cut or edited link into a refusal rather
 * than a different game, because a mixed-radix code has no redundancy.
 *
 * A reader that meets a format it does not know refuses (CN_EFORMAT) rather
 * than misreads; a rules change that would roll or play any existing code
 * differently is a new format. */
#ifndef CN_MSG_H
#define CN_MSG_H

#include "cn.h"
#include "cn_lobby.h"
#include "cn_code.h"
#include "../../../shared/c/msg_seat_tag/msg_seat_tag.h"

#define CN_MSG_MAGIC      0xC5
#define CN_MSG_FORMAT     1

#define CN_PHASE_WAITING  0
#define CN_PHASE_LIVE     2
#define CN_PHASE_FINISHED 3

#define CN_FLAG_DM        0x01
#define CN_FLAG_LEFT      0x02
#define CN_FLAGS_KNOWN    (CN_FLAG_DM | CN_FLAG_LEFT)

#define CN_TAG_LEN        MSG_SEAT_TAG_LEN
#define CN_NAME_MAX_BYTES MSG_SEAT_NAME_MAX_BYTES
#define CN_NAME_MAX_CHARS MSG_SEAT_NAME_MAX_CHARS
#define CN_CHECK_LEN      2
#define CN_HEAD_LEN       42
#define CN_ROW_MAX        (CN_TAG_LEN + 1 + CN_NAME_MAX_BYTES)
#define CN_MSG_MAX_BYTES  (CN_HEAD_LEN + CN_MAX_SEATS * CN_ROW_MAX + CN_CHECK_LEN + CN_CODE_MAX)
/* "?m=" + base32 + NUL */
#define CN_MSG_MAX_TEXT   (3 + (CN_MSG_MAX_BYTES * 8 + 4) / 5 + 1)

/* EVERY GAME FITS ONE MESSAGE: the longest game there can be (cn.h's
 * CN_MAX_MOVES, every digit at 8 bits) with six 48-byte names is under the
 * 5,000 characters Apple documents for MSMessage.url. Raise a cap past this
 * and the build stops. tests/cn_msg_test.c builds that game and measures it. */
_Static_assert(CN_MSG_MAX_TEXT - 1 < 5000, "the capped worst case fits MSMessage.url");

#define CN_EOK      0
#define CN_ESHORT  -1    /* the buffer ends inside the header or the roster  */
#define CN_EMAGIC  -2    /* not one of ours                                  */
#define CN_EFORMAT -3    /* a format this build does not know                */
#define CN_EFLAGS  -4    /* a reserved flag bit, or a flag the phase forbids */
#define CN_ECHECK  -5    /* the check does not match: cut or edited          */
#define CN_EGAME   -6    /* the body does not replay, or disagrees with the header */
#define CN_EROSTER -7    /* empty or long name, duplicate tag or name, seats out of range */
#define CN_ECAP    -8    /* output buffer too small                          */
#define CN_ETEXT   -9    /* no "m=" in the text, or it is not base32         */
#define CN_EREFUSED -10  /* a lobby or seat verdict said no                  */

/* A roster row, shared/c/msg_seat_tag's: the tag is
 * SHA-256("chuiniu.seat.1|" || seed || id)[0..9), then a 1..48-byte UTF-8 name. */
typedef MsgSeat CnSeat;

typedef struct {
    uint8_t  phase;                   /* CN_PHASE_*                                  */
    uint8_t  dm;
    uint8_t  left;                    /* WAITING: the newest lobby bubble was a leave */
    uint8_t  n_seats;
    uint16_t lobby_rev;
    uint8_t  starter;                 /* CN_SEAT_NONE while WAITING                  */
    uint8_t  pad0;
    uint8_t  seed[32];
    CnSeat   seat[CN_MAX_SEATS];
    CnGame   game;                    /* the table once started                      */
} CnMsg;

/* ------------------------------------------------------------- identity */

void cn_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[CN_TAG_LEN]);
/* SHA-256("chuiniu.game.1|" || seed)[0..8). */
void cn_game_id(const uint8_t seed[32], uint8_t out[8]);

/* A NICKNAME, JUDGED by shared/c/msg_seat_tag's msg_seat_name_verdict. */
#define CN_NAME_OK       MSG_SEAT_NAME_OK
#define CN_NAME_EMPTY    MSG_SEAT_NAME_EMPTY
#define CN_NAME_TOO_LONG MSG_SEAT_NAME_TOO_LONG
#define CN_NAME_BAD      MSG_SEAT_NAME_BAD

/* ------------------------------------------------------------ the lobby
 * Each returns 0 (CN_EOK), a seat, or a negative CN_E*, and leaves `m`
 * untouched on refusal. */
int  cn_msg_new(CnMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[CN_TAG_LEN],
                const uint8_t *name, int name_len);
void cn_msg_lobby(const CnMsg *m, CnLobby *l);
int  cn_msg_offered(const CnMsg *m, int seat);
int  cn_msg_join(CnMsg *m, const uint8_t tag[CN_TAG_LEN], const uint8_t *name, int name_len);
int  cn_msg_leave(CnMsg *m, int seat);
int  cn_msg_start(CnMsg *m, int seat);
int  cn_msg_join_start(CnMsg *m, const uint8_t tag[CN_TAG_LEN], const uint8_t *name, int name_len);
int  cn_msg_plan_lobby(const CnMsg *before, const CnMsg *after, CnEvent *out, int cap);

/* ----------------------------------------------------------- the bytes */

/* Bytes written, or a negative CN_E*. What is written is decoded again
 * before it is returned. */
int cn_msg_encode(const CnMsg *m, uint8_t *out, int cap);
/* Read `n` bytes. Never reads past in[n-1]. CN_EOK, or a negative CN_E*
 * with `out` untouched. */
int cn_msg_decode(const uint8_t *in, int n, CnMsg *out);
/* "?m=" + base32 of the bytes; the reader takes a whole URL string. */
int cn_msg_text_encode(const CnMsg *m, char *out, int cap);
int cn_msg_text_decode(const char *text, CnMsg *out);

/* ------------------------------------------------------- two messages */

int cn_msg_same_game(const CnMsg *a, const CnMsg *b);

/* WHICH OF TWO BUBBLES TO SHOW (pickemup's Rule P cut to one move per
 * bubble): <0 mine, >0 tapped, 0 identical. Delivery order is never an
 * input, so every phone holding the same pair agrees.
 *   1 different games: the tapped one
 *   2 a started chain beats a WAITING one
 *   3 more moves
 *   4 higher lobby_rev, then more seats
 *   5 the smaller SHA-256 of the envelope bytes */
int cn_msg_prefer(const CnMsg *mine, const CnMsg *tapped);

/* The moves two chains of one game share. 0 for different games or a lobby. */
int cn_common_moves(const CnMsg *a, const CnMsg *b);

/* ------------------------------------------------------------ the seats
 *
 * WHICH SEAT AM I (pickemup's resolver, its D42 / D47 / D51): the record,
 * then the tag, then the sender fact, then the nickname; a record whose tag
 * has no row says "not me" and only the tag may overrule it; in a WAITING
 * bubble the sender witness counts only if the row carries my name. */
#define CN_BY_NONE   MSG_SEAT_BY_NONE
#define CN_BY_RECORD MSG_SEAT_BY_RECORD
#define CN_BY_TAG    MSG_SEAT_BY_TAG
#define CN_BY_SENDER MSG_SEAT_BY_SENDER
#define CN_BY_NAME   MSG_SEAT_BY_NAME

#define CN_SENT_UNKNOWN MSG_SEAT_SENT_UNKNOWN

int cn_msg_seat_of_tag(const CnMsg *m, const uint8_t tag[CN_TAG_LEN]);
/* The seat that sent this bubble, or -1 (a leave: the sender has no seat). */
int cn_msg_sender(const CnMsg *m);
int cn_msg_resolve(const CnMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by);

/* THE RECORDS: this device's seats, newest first, CN_REC_LEN bytes each -
 * the game id, the tag it sat with, and the newest round whose throw this
 * device has watched to its end (that round + 1, 0 for none): each round's
 * throw plays on a phone once (docs_pkgX.md). */
#define CN_REC_LEN   (8 + CN_TAG_LEN + 1)
#define CN_REC_SEEN  (8 + CN_TAG_LEN)          /* the seen byte's offset    */
#define CN_REC_MAX   256
#define CN_REC_BYTES (CN_REC_LEN * CN_REC_MAX)
#define CN_REC_GONE  MSG_SEAT_REC_GONE
int cn_rec_find(const uint8_t *recs, int n, const CnMsg *m);
/* Records `seat`'s tag for m's game, newest first; a game already recorded
 * keeps its seen byte (a rejoin is the same phone that watched). */
int cn_rec_put(uint8_t *recs, int n, const CnMsg *m, int seat);
int cn_rec_forget(uint8_t *recs, int n, const CnMsg *m);
/* The seen byte of m's game: 0 with no record. */
int cn_rec_seen(const uint8_t *recs, int n, const CnMsg *m);
/* Raise m's seen byte to `seen` (1..255); never lowers it. 1 if it changed,
 * 0 otherwise (no record of the game, or already as high). */
int cn_rec_see(uint8_t *recs, int n, const CnMsg *m, int seen);

/* THE STORED FORM (what the host keeps and hands back unread): the magic
 * CN_REC_MAGIC, then the records as above. A stored form without the magic
 * is the first one (17-byte records: no seen byte), read as nothing seen.
 * The magic is 8 bytes where the first form held a game id (a SHA-256
 * prefix), so a first-form store is mistaken for this one with chance 2^-64.
 * Either form's trailing partial record is dropped. */
#define CN_REC_MAGIC      "cnrec\x02\x00\xa5"
#define CN_REC_MAGIC_LEN  8
#define CN_REC_LEN_V1     (8 + CN_TAG_LEN)
#define CN_REC_FILE_BYTES (CN_REC_MAGIC_LEN + CN_REC_BYTES)
/* The stored form `bytes` (n of them) into recs (CN_REC_BYTES): the records'
 * length. */
int cn_rec_load(uint8_t *recs, const uint8_t *bytes, int n);
/* recs (n bytes) in the stored form: its length, or -1 when cap is short. */
int cn_rec_save(const uint8_t *recs, int n, uint8_t *out, int cap);

#endif

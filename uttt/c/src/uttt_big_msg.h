/* The big game's message: what a 243 x 243 bubble carries, who sits where,
 * and what the person holding it may do (docs/BIG_BOARD.md).
 *
 * Shaped like uttt_msg.h, under its magic, and NOT sharing a byte of its
 * format: this is FORMAT 3, and every shipped reader refuses it with
 * UTM_EFORMAT (uttt_msg.c, utm_decode), which is what makes it safe to put
 * in a thread next to 9 x 9 bubbles - an App Store build opens it as "a
 * bubble this build cannot read" and nothing else. Formats 1 and 2 are not
 * touched and stay exactly what they are.
 *
 * THE BOARD IS THE PICTURE. 59,049 cells do not fit a URL (about 3 KB), so
 * the bubble's picture carries them (shared/swift/BubbleDataKit: n x (n + 1)
 * cells of three greys with a header and a CRC-32, geometry board243) and
 * the URL carries what the picture cannot: the game's identity and roster,
 * the last move (the forced target is derived from it), the ply count, and a
 * check that ties this URL to this board. An extension cannot enumerate the
 * transcript, so every bubble is the whole position, never a diff.
 *
 * ---------------------------------------------------------------- the layout
 *
 *     [0]      UTM_MAGIC
 *     [1]      UTB_FORMAT (3)
 *     [2..5]   seed, int32 big-endian: the send time of the first empty board
 *     [6]      flags: bit 0 = sealed (the X seat is taken). Others refused.
 *     [7]      look: the drawing byte (the sheet's paper; copied by Again)
 *     [8..16]  O's seat tag - the creator's (utm_tag, the same hash)
 *     [17..25] X's seat tag - the joiner's, present only when sealed
 *     [+0..1]  n_plies, uint16 big-endian
 *     [+2..3]  the last move, uint16 big-endian; 0xFFFF for none
 *     [+4..7]  the board check: CRC-32 (IEEE, bd_crc32's) over the 59,049
 *              cells in leaf order, big-endian
 *     [+8..9]  check: the first two bytes of SHA-256 over every other byte
 *              (shared/c/wire_check, as format 2)
 *
 * Decoding takes the URL bytes AND the cells the picture read back to: the
 * header is checked, the cells' CRC is held against the header's, the board
 * is adopted (utb_adopt: counts, lines, the last move's cell) and the ply
 * count must agree with the marks. Anything else is refused, never guessed.
 * utb_msg_peek reads the header alone, for the questions that need no board:
 * is this a big game, which of two bubbles to show, is it the same game.
 *
 * THE PROTOCOL is uttt_msg.h's: A sends an empty board with their tag in O;
 * the joiner is X and moves first, and taking the seat and making the first
 * move are one act; once X is taken the roster is sealed. The seats, the
 * records and the three witnesses (utm_resolve) are the shipped kernel's own
 * functions, reached through utb_msg_roster - a UtmMsg carrying this
 * message's seed, look, sealed flag and two tags, with game.n_plies set to
 * this game's PARITY, which is all those functions read of the game.
 */
#ifndef UTTT_BIG_MSG_H
#define UTTT_BIG_MSG_H

#include "uttt_big.h"
#include "uttt_msg.h"
#include <stdint.h>

#define UTB_FORMAT      3
#define UTB_DEPTH       5
#define UTB_SIDE        243
#define UTB_CELLS       59049                  /* UTB_SIDE * UTB_SIDE        */
#define UTB_LAST_NONE   0xFFFF
#define UTB_TAIL_LEN    (2 + 2 + 4)            /* n_plies, last, board check */
#define UTB_HEAD_OPEN   (UTM_HEAD_OPEN + UTB_TAIL_LEN)
#define UTB_HEAD_SEALED (UTM_HEAD_SEALED + UTB_TAIL_LEN)
#define UTB_MAX_BYTES   (UTB_HEAD_SEALED + UTM_CHECK_LEN)
/* "?m=" + base32 + NUL */
#define UTB_MAX_TEXT    (3 + (UTB_MAX_BYTES * 8 + 4) / 5 + 1)

/* THE MODE'S DOOR (docs/BIG_BOARD.md): the 243 mode is switched on and off
 * by holding a grid still. The hold is this long, and the finger may drift
 * this many points before it is not a hold any more (a pan, a pinch, a
 * drag). A tap, a double tap and a pinch never come near either number. */
#define UTB_HOLD_MS      4000
#define UTB_HOLD_SLOP_PT 10

/* One more refusal beside uttt_msg.h's UTM_E*: the cells are not a board
 * legal play could reach, or they disagree with the header's count. */
#define UTB_EBOARD   -10

/* The header alone: everything the URL says. */
typedef struct {
    int32_t  seed;
    uint8_t  look;
    uint8_t  sealed;
    uint8_t  o[UTM_TAG_LEN];
    uint8_t  x[UTM_TAG_LEN];
    int32_t  n_plies;
    int32_t  last;                /* UTB_NONE or 0..UTB_CELLS-1 */
    uint32_t board_check;
} UtbHead;

typedef struct {
    int32_t  seed;
    uint8_t  look;
    uint8_t  sealed;
    uint8_t  o[UTM_TAG_LEN];      /* the creator                            */
    uint8_t  x[UTM_TAG_LEN];      /* the joiner; meaningful iff sealed      */
    UtbGame  game;                /* depth UTB_DEPTH                        */
} UtbMsg;

/* CRC-32 (IEEE 802.3) over `n` cells, the kit's bd_crc32 reproduced here so
 * the kernel links nothing outside uttt/c and shared/c; the test holds the
 * two to one check value. */
uint32_t utb_board_check(const uint8_t *cells, int n);

/* ----------------------------------------------------------- the bytes */

/* An invitation: the empty 243 board, `me` in the O seat, X open. */
void utb_msg_open(UtbMsg *m, int32_t seed, uint8_t look, const uint8_t me[UTM_TAG_LEN]);
/* AGAIN after a finished big game, on the same napkin (its look). 1, or 0 and
 * nothing written unless `finished` offers the Again door. */
int  utb_msg_again(UtbMsg *next, const UtbMsg *finished, int32_t seed, const uint8_t me[UTM_TAG_LEN]);
/* (`next` may be `finished` itself: the bridge has one slot, and a second
 * 66 KB message only to copy one byte out of the first would be waste.) */

/* The URL bytes for `m` (never the board). Bytes written, or a negative
 * UTM_E*; refuses what decode would refuse. */
int  utb_msg_encode(const UtbMsg *m, uint8_t *out, int cap);
/* The header of a big-game message, check verified; 0 (UTM_EOK) or a negative
 * UTM_E*. A format-1 or format-2 message is UTM_EFORMAT here, as a format-3
 * one is there. */
int  utb_msg_peek(const uint8_t *in, int n, UtbHead *out);
/* A header's own bytes, exactly as utb_msg_encode writes them for a message
 * with that header (the board check is the header's, so no board is
 * needed): what the bridge holds a sender fact against. Bytes written, or a
 * negative UTM_E* / UTB_EBOARD for a header peek would refuse. */
int  utb_head_encode(const UtbHead *h, uint8_t *out, int cap);
/* Header and board together: peek, the cells' CRC against the header, adopt,
 * the ply count against the marks, the roster rules (unsealed <=> no plies;
 * sealed needs a first move and X != O). `cells` holds UTB_CELLS symbols. */
int  utb_msg_decode(const uint8_t *in, int n, const uint8_t *cells, UtbMsg *out);

/* The text a link carries, "?m=" + base32, and back (the reader takes a
 * whole URL string and finds the value, exactly as utm_text_decode does). */
int  utb_msg_text_encode(const UtbMsg *m, char *out, int cap);
int  utb_msg_text_peek(const char *text, UtbHead *out);
int  utb_msg_text_decode(const char *text, const uint8_t *cells, UtbMsg *out);

/* The bytes a link's "m=" value decodes to, whatever their format: what the
 * diagnostics print the length and format byte of. Count, or UTM_ETEXT.
 * Hidden, as the diagnostics are (uttt_big_diag.h): its one caller is them,
 * so an App Store build, which links none of them, keeps none of it. */
__attribute__((visibility("hidden")))
int  utb_msg_text_bytes(const char *text, uint8_t *out, int cap);

/* Is this text a big-game message by its magic and format byte alone: the
 * router's question, answered before any check. 1 or 0. */
int  utb_msg_text_is(const char *text);

/* ------------------------------------------------------------ the roster */

/* This message's roster as the shipped kernel's functions read it (utm_seat,
 * utm_resolve, utm_rec_find, utm_rec_put, utm_rec_forget, utm_same_game):
 * seed, look, sealed and the two tags copied, game.n_plies = the parity of
 * this game's ply count. Nothing else of `game` is meaningful. */
UtmMsg utb_msg_roster(const UtbMsg *m);
UtmMsg utb_head_roster(const UtbHead *h);

/* The seat `me` holds (UTM_SEAT_*), the mark it plays, and whether it may
 * put a mark on this board now - utm_seat's and utm_can_move's rules. */
int  utb_msg_seat(const UtbMsg *m, const uint8_t me[UTM_TAG_LEN]);
int  utb_msg_can_move(const UtbMsg *m, const uint8_t me[UTM_TAG_LEN]);

/* Play `mv` as `me`; on an OPEN invitation this TAKES THE SEAT. 1 if played,
 * 0 and `m` untouched otherwise. */
int  utb_msg_play(UtbMsg *m, const uint8_t me[UTM_TAG_LEN], int mv);
/* Take back `me`'s own last move (a staged bubble is a draft); taking back
 * the joining move gives the seat back. 1 if a move came back. Refused when
 * the move before it is unknown (utb_undo). */
int  utb_msg_undo(UtbMsg *m, const uint8_t me[UTM_TAG_LEN]);
/* A change of mind: may `me` replace their staged last move with `mv`. Pure. */
int  utb_msg_can_replace(const UtbMsg *m, const uint8_t me[UTM_TAG_LEN], int mv);
/* The one door: UTM_DOOR_AGAIN once the game is over, else UTM_DOOR_NONE. */
int  utb_msg_door(const UtbMsg *m);

/* ------------------------------------------------------- two messages */

/* The same game: seed and creator. */
int  utb_head_same_game(const UtbHead *a, const UtbHead *b);
/* WHICH OF TWO BUBBLES TO SHOW, from their headers: <0 mine, >0 tapped, 0
 * identical. utm_prefer's rules: different games -> tapped; sealed beats its
 * own invitation; more plies wins; equal plies and one roster -> mine unless
 * identical (same last move and same board check); two joiners -> the lower
 * SHA-256("uttt.join.big|" || seed || X's tag). */
int  utb_head_prefer(const UtbHead *mine, const UtbHead *tapped);

/* THE CAPTION, the 9 x 9's words exactly (uttt_caption): "New game?" for an
 * invitation, "X to play" in play, "X won in N moves", a draw. Length, or -1. */
int  utb_msg_caption(const UtbMsg *m, const char *who, char *out, int cap);

#endif

/* Tallybones - the body: every move since the start as one number (T7).
 *
 * THE MODEL IS THE RULES (pickemup/c/src/pk_code.h). At every bubble the
 * kernel lists the moves there are (tb_menu: KEEP 0..30 while a roll is
 * left, SCORE per open category, LEAVE per seat still in), so the only thing
 * stored is which one was taken: one digit per bubble, its base the menu's
 * length (at most 52), none at all for a menu of one. The arithmetic is
 * shared/c/mixrad.
 *
 * The number starts as the sentinel 1, digits are folded in backwards
 * (v = v * base + index), the bytes are its little-endian minimal
 * representation, and the decoder walks forwards (index = v % base; v /=
 * base) and must end at exactly 1. The menus depend on no die, so neither
 * does the body: THE WIRE CARRIES NO DICE VALUES (T7, T11).
 *
 * THE BODY IS ALSO THE ROLL'S INPUT (T11). Every roll is derived from the
 * body of the history through the move that caused it, which is the number
 * a message holding exactly that history would carry. The walker keeps it as
 * it goes: with digits (b_1, i_1) .. (b_k, i_k) the body is S_k + P_k, where
 * P_k = b_1 .. b_k and S_k = i_1 + b_1 i_2 + .. + P_{k-1} i_k, which is the
 * backward fold written forwards, one step a bubble (T14).
 *
 * ONE WALKER, THREE USES: encode (the indices from the history, nothing
 * derived), decode (the indices from the number) and the resident replay
 * (the indices from the history, every roll derived). */
#ifndef TB_CODE_H
#define TB_CODE_H

#include "tb.h"

/* A digit's base is at most TB_MENU_MAX = 52 < 2^6. */
#define TB_CODE_MAX_BITS (TB_MAX_BUBBLES * 6)
#define TB_CODE_MAX      ((TB_CODE_MAX_BITS + 1 + 7) / 8)        /* 240 bytes */

/* Encode the resident moves of `g`, and its pending move when it is a draft
 * (the bubble a host is about to send). The byte count, -1 for a history the
 * menus cannot say, -2 for a `cap` too small. Nothing is derived. */
int tb_code_encode(const TbGame *g, uint8_t *buf, int cap);

/* The body of the first `k` resident moves of `g` (0: the empty history,
 * the one byte 1), by the backward fold: what tb_code_encode writes for a
 * game cut there. The byte count, or a negative as tb_code_encode. */
int tb_code_body(const TbGame *g, int k, uint8_t *buf, int cap);

/* Rebuild the game at `n` seats started by `starter` from exactly `bubbles`
 * moves in `buf[0 .. len)`. With `derive` every roll is derived (the RESIDENT
 * replay: a received or sent message); without, the dice read 0 (the
 * encoder's own read-back of a draft it is writing). 1, or 0 for a body that
 * does not decode (a digit off its menu, a non-minimal number, bytes left
 * over, a game that ends before its last bubble). */
int tb_code_decode(TbGame *out, const uint8_t seed[32], int n, int starter, int bubbles,
                   const uint8_t *buf, int len, int derive);

/* THE RESIDENT REPLAY: `out` rebuilt from the seed through `moves[0 .. k)`,
 * every roll derived. 1, or 0 when a move is not on its menu. */
int tb_replay(TbGame *out, const uint8_t seed[32], int n, int starter, const TbMove *moves, int k);

#endif

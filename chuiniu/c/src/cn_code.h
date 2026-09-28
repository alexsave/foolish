/* Chui Niu - the body: every move since the start as one number.
 *
 * THE MODEL IS THE RULES (pickemup/c/src/pk_code.h, uttt's before it). At
 * every move the kernel lists the options (cn_legal: the call first when it
 * is legal, then every legal bid by rank), so the only thing stored is which
 * one was taken, as a digit whose base is the number of options. A move with
 * one option (only the call is left) is not a digit and costs nothing. The
 * arithmetic is shared/c/mixrad.
 *
 * The number starts as the sentinel 1, digits are folded in backwards
 * (v = v * base + index), the bytes are its little-endian minimal
 * representation (no zero top byte), and the decoder walks forwards
 * (index = v % base; v /= base) and must end at exactly 1. The envelope's
 * header says how many moves to read; the body carries no length.
 *
 * ONE WALKER DOES BOTH DIRECTIONS, so the encoder and the decoder cannot
 * disagree about a menu, and every body that decodes re-encodes to exactly
 * its own bytes. */
#ifndef CN_CODE_H
#define CN_CODE_H

#include "cn.h"

/* THE BOUND. A menu is at most 151 entries (30 dice x 5 faces, and the
 * call), so a digit is under 8 bits; the sentinel adds one bit. */
#define CN_CODE_MAX (CN_MAX_MOVES + 1)                 /* 2,350 bytes */

/* Encode every move of `g`. The byte count, -1 for a history the menus
 * cannot say, -2 for a small `cap`. */
int cn_code_encode(const CnGame *g, uint8_t *buf, int cap);

/* Rebuild the game from `seed` at `n` seats, reading exactly `moves` moves
 * from buf[0 .. len). 1, or 0 for a body that does not decode (a digit off
 * its menu, a non-minimal number, bytes left over, a game that ends before
 * its last move). */
int cn_code_decode(CnGame *out, const uint8_t seed[32], int n, int moves, const uint8_t *buf, int len);

#endif

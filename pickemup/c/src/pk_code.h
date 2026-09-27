/* Pick 'Em Up - the body: every choice since the deal as one number
 * (RULES_AND_KERNEL.md 4.4).
 *
 * THE MODEL IS THE RULES (uttt/c/src/uttt_code.h). At every decision the
 * kernel lists the options, so the only thing stored is which one was chosen,
 * as a digit whose base is the number of options. A decision with one option
 * is not a digit and costs nothing. The arithmetic is shared/c/mixrad.
 *
 * The number starts as the sentinel 1, digits are folded in backwards
 * (v = v * base + index), the bytes are its little-endian minimal
 * representation (no zero top byte), and the decoder walks forwards
 * (index = v % base; v /= base) and must end at exactly 1.
 *
 * PER BUBBLE, on the table as it stands between bubbles:
 *
 *   S   sender       base 2: the turn seat / somebody else - only when some
 *                    other seat has anything to send (a "Last card!" or a
 *                    catch); then base k, index among those k seats in seat
 *                    order (DECISION D40: the candidates, not all n-1)
 *   Y   said         base 2, when the sender may say it - forced to "said"
 *                    for an out-of-turn sender with nobody to catch, since a
 *                    bubble must hold something
 *   C   call         base 2 none / a call - forced to "a call" for an
 *                    out-of-turn sender who did not say it, forced to "none"
 *                    when there is nobody to catch; then base (legal
 *                    targets), index in seat order
 *   T   turn         base 2, only for the turn seat that said or caught; the
 *                    turn seat with neither must take its turn
 *   then, while a turn is present:
 *     A  action      base = pk_legal_turn's count, index into it (its order
 *                    is the format: DRAW, plays by position with a non-final
 *                    wild as four suits, PASS)
 *     after a PLAY or PASS: if the turn has ended (pk_turn_ended) the bubble
 *     ends, else
 *     K  continue    base 2: end the bubble / take the next turn too (D7)
 *
 * ONE WALKER DOES BOTH DIRECTIONS: the encoder and the decoder run the same
 * function over the same menus, the encoder choosing each index from the
 * history and the decoder reading it from the number, so the two cannot
 * disagree about a menu. Any body that decodes re-encodes to exactly its own
 * bytes (7.4.2).
 *
 * The decoder knows how many bubbles to read from the envelope's header; the
 * body carries no length of its own. */
#ifndef PK_CODE_H
#define PK_CODE_H

#include "pk.h"

/* THE BOUND, and why every game fits it. An action menu is at most 128
 * entries (DRAW, PASS, 102 hand positions, 3 more suits for each of 8 wilds),
 * 7 bits; a K digit follows at most one terminal per action (1 bit); a
 * bubble's header is at most S 1 + 3, Y 1, C 1 + 3, T 1 = 10 bits. The number
 * is below 2^(bits + 1) because of the sentinel. */
#define PK_CODE_MAX_BITS   (PK_MAX_ACTIONS * (7 + 1) + PK_MAX_BUBBLES * 10)
#define PK_CODE_MAX        ((PK_CODE_MAX_BITS + 1 + 7) / 8)      /* 2,438 bytes */
#define PK_CODE_MAX_DIGITS (PK_MAX_ACTIONS * 2 + PK_MAX_BUBBLES * 6)

/* Encode every sealed bubble of `g` (which must have no draft open). Returns
 * the byte count, -1 if the history is not one the menus can say (a history
 * pk_apply did not write), or -2 if `cap` is too small. */
int pk_code_encode(const PkGame *g, uint8_t *buf, int cap);

/* Rebuild the game: deal from `seed` at `n` seats with `starter` recorded,
 * then read exactly `bubbles` bubbles from `buf[0 .. len)`. 1, or 0 for a
 * body that does not decode (a digit off its menu, a non-minimal number,
 * bytes left over, a game that ends before its last bubble). */
int pk_code_decode(PkGame *out, const uint8_t seed[32], int n, int starter, int bubbles,
                   const uint8_t *buf, int len);

#endif

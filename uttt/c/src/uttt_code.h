/* A whole game in ~21 bytes.
 *
 * THE MODEL IS THE RULES. At every ply the kernel enumerates the legal moves,
 * so the only thing to store is WHICH of them was chosen - an index into a list
 * both sides rebuild for themselves. A ply with nine legal moves costs
 * log2(9) = 3.17 bits; a ply with two costs one. No frequency table, no
 * probabilities to ship, nothing to tune: the alphabet size is whatever the
 * position says it is.
 *
 * The code is then one big number in a MIXED RADIX - digit k has base n_k:
 *
 *     encode:  v = v * n + i        (walked backwards)
 *     decode:  i = v % n;  v /= n   (walked forwards)
 *
 * which is exact. It reaches the entropy of the model to within the final
 * rounding to a byte, and there is no renormalisation, no state to flush and
 * no window to keep the state inside.
 *
 * WHY NOT rANS. It was rANS first. rANS renormalises against x_max = (L/n)<<8,
 * which is exact only when n divides L - true for the power-of-two alphabets
 * every reference implementation uses, and false here, where n is the number
 * of legal moves and runs 1..81. With L = 2^16 and n = 81, floor(L/n)*n is
 * 65529: seven below the floor the decoder renormalises against, so the two
 * sides silently walked apart about twice in every eight thousand games.
 * Rounding the bound up fixed the floor and broke the ceiling instead. A
 * bignum has neither edge, is shorter, and is about fifteen lines.
 */
#ifndef UTTT_CODE_H
#define UTTT_CODE_H

#include "uttt.h"
#include <stddef.h>

/* Encode g's history. Returns bytes written, or -1 if it did not fit. No
 * header: the decoder stops when the rules say the game is over. */
int uttt_encode(const UtttGame *g, uint8_t *buf, size_t cap);

/* Rebuild a game from n bytes. Returns 1 on success. */
int uttt_decode(UtttGame *out, const uint8_t *buf, size_t n);

/* What the history is worth under the same model, in bits. The coder should
 * land within a byte of this; the gap is everything it wastes. */
double uttt_ideal_bits(const UtttGame *g);

/* THE LOOK OF A GAME THAT HAS NO DRAWING BYTE. A game's sheet is drawn from
 * one byte, its LOOK (uttt_msg.h: chosen at random when a game is made and
 * inherited by its rematches). Three things carry a game seed and no look -
 * a format-1 message, a format-1 replay link and a seeded debug game (the
 * rig's, the preview's) - and they all draw with THIS byte of the seed. One
 * owner, so the same old bubble draws the same on every phone and the same
 * again on uttt.live. */
static inline uint8_t uttt_look_of_seed(int32_t seed) { return (uint8_t)(uint32_t)seed; }

/* THE REPLAY LINK: the finished game as a URL somebody can paste, the end
 * screen's "Copy code" (an iMessage extension can open only its own
 * container's scheme, so the link is copied rather than opened - foolish's
 * replay row, FGameOverList.replayLink). It is UTTT_REPLAY_PREFIX followed by
 * the code: a FORMAT SEGMENT, then base32 (shared/c/b32: letters and digits,
 * the same read back in either case, nothing a URL has to escape):
 *
 *     2/<base32 of>   [0]     the look (uttt_look_seed draws everything from it)
 *                     [1..]   uttt_encode's bytes (the moves)
 *
 * A link with no format segment is FORMAT 1, written by 1.0(6)-1.0(8):
 *
 *        <base32 of>  [0..3]  the game seed, int32 big-endian
 *                     [4..]   the moves
 *
 * and its look is uttt_look_of_seed of that seed - the one branch on the way
 * in; the moves and the drawing go the same way after it. The segment holds
 * the format because base32 is A-Z and 2-7 and a format-1 code cannot have a
 * "/" in it, so the two cannot be confused; a byte inside the code could
 * be, since a format-1 code begins with whatever byte the clock gave.
 *
 * The kernel writes the whole string; a host only puts it on the pasteboard.
 *
 * uttt.live is the game's own site: uttt/web opens the code and replays the
 * game, drawn by this kernel (wasm/uttt_web.c). */
#define UTTT_REPLAY_PREFIX "https://uttt.live/"
#define UTTT_REPLAY_FORMAT "2/"

/* Write g's link, drawn with `look`, into out (NUL-terminated). Returns its
 * length, or -1 if g has no plies or cap is too small. */
int uttt_replay_url(const UtttGame *g, uint8_t look, char *out, int cap);

/* Read a link back (the prefix is optional; anything after the code - a
 * query, a fragment, a slash - is ignored). Returns 1, the game and its
 * look (`look` may be NULL) on success, 0 for a link that is not a game. */
int uttt_replay_read(const char *url, UtttGame *out, uint8_t *look);

#endif

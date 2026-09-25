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

/* THE REPLAY LINK: the finished game as a URL somebody can paste, the end
 * screen's "Copy code" (an iMessage extension can open only its own
 * container's scheme, so the link is copied rather than opened - foolish's
 * replay row, FGameOverList.replayLink). It is UTTT_REPLAY_PREFIX followed by
 * the code in base32 (shared/c/b32: letters and digits, the same read back
 * in either case, nothing a URL has to escape). The code is a fixed layout:
 *
 *     [0..3]  the drawing seed, int32 big-endian (the wire message's seed,
 *             uttt_msg.h), so a replay draws the same napkin - every pen
 *             stroke's wobble is seeded from it
 *     [4..]   uttt_encode's bytes (the moves)
 *
 * The kernel writes the whole string; a host only puts it on the pasteboard.
 *
 * uttt.live is the game's own site: uttt/web opens the code and replays the
 * game, drawn by this kernel (wasm/uttt_web.c). Links copied before it
 * existed carry UTTT_REPLAY_PREFIX_OLD, which foolish.cards redirects here
 * and uttt_replay_read still reads. */
#define UTTT_REPLAY_PREFIX     "https://uttt.live/"
#define UTTT_REPLAY_PREFIX_OLD "https://www.foolish.cards/uttt/"

/* Write g's link, drawn with `seed`, into out (NUL-terminated). Returns its
 * length, or -1 if g has no plies or cap is too small. */
int uttt_replay_url(const UtttGame *g, int32_t seed, char *out, int cap);

/* Read a link back (the prefix, new or old, is optional; anything after the code - a
 * query, a fragment, a slash - is ignored). Returns 1, the game and its
 * drawing seed (`seed` may be NULL) on success, 0 for a link that is not a
 * game. */
int uttt_replay_read(const char *url, UtttGame *out, int32_t *seed);

#endif

/* THE DIAGONAL FIXTURE, the one finished game the tests share: X takes the
 * top-left, centre and bottom-right blocks in 25 plies, and the last move
 * (square 0) wins the game on the top-left to bottom-right diagonal
 * (docs/UI.html 04-07). Several earlier moves take a block without ending
 * the game, which the motion test reads off its prefixes.
 *
 * ONE COPY. It was written out seven times across four tests and the bridge
 * smoke, and a fixture edited in one of them would have had the others
 * asserting things about a different game. */
#ifndef UTTT_FIXTURE_H
#define UTTT_FIXTURE_H

#include <stdint.h>

static const uint8_t diag_fixture[] = { 79, 63, 5, 45, 8, 76, 42, 61, 70, 71, 78, 55, 15,
                                        58, 36, 1, 11, 24, 4, 40, 39, 31, 80, 35, 0 };

#endif

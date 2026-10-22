// tournament.h - THE BRACKET: 48 of the server's bots, single elimination, two-player best-of-five
// matches (wire.h be_w_tour), seeded by the server's ratings. Pure bookkeeping: which match needs a
// game, who sits where, what a game's result does to the match and the bracket. The league engine
// (league.c, BE_MODE=tournament) plays the games, as real rated server games through the dev-only
// bot table, and hands each result here; the bracket is kept in a file of its own fixed layout, so
// the site shows it again after a restart.
//
// The draw: every bot but one, the one left out being the lowest-rated of the random family (the
// weakest by construction), else the lowest-rated; seeds by rating, highest first.
// Seeds 1..16 get byes into the round of 32; seeds 17..48 open, 17 v 48 .. 32 v 33, and the opening
// match of seed s feeds the round-of-32 match whose lower seed is s. From the round of 32 on the
// bracket is the standard one (1 v 32, 16 v 17, ...), so the top two seeds can only meet in the final.
#ifndef BE_TOURNAMENT_H
#define BE_TOURNAMENT_H

#include "wire.h"

#include <stdbool.h>
#include <stdint.h>

#define BE_TOUR_MAX_GAMES 9   // a match drawn this often goes to the higher seed

// A fresh bracket from `n` bots' ratings and strategy families (`is_random[i]`: bot i plays the
// random brain). false when fewer than 49 bots are given (one is always left out).
bool be_tour_draw(be_w_tour *t, const int32_t *rating, const bool *is_random, int n, int64_t now_ms);

// A match that needs a game now (both players known, undecided, no game in play), or -1. The seats
// for that game: the higher seed first in odd games, second in even ones.
int  be_tour_next(const be_w_tour *t, uint8_t seats[2]);
// The game for match `m` is in play at `gid`.
void be_tour_started(be_w_tour *t, int m, const char *gid, int gid_len);
// The game ended with `fool_bot` the fool (a league bot index; BE_TOUR_NONE for a draw), or never
// started (`played` false: the match may ask again). Updates the match; a decided match sends its
// winner on, and the final decides the champion. true when the bracket changed.
bool be_tour_result(be_w_tour *t, int m, bool played, uint8_t fool_bot, int64_t now_ms);

// The match `m` feeds (its winner's next match) and which side, or -1 for the final.
int  be_tour_feeds(int m, int *side);

// The file: be_w_tour behind a head naming the bots by id, in the order the indices mean.
// load: false when there is none or it is not one this build reads; `ids` maps today's bots.
bool be_tour_load(be_w_tour *t, const char *path, const char *const *ids, int n);
bool be_tour_save(const be_w_tour *t, const char *path, const char *const *ids, int n);

#endif

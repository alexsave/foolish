// gauntlet.h - THE GAUNTLET: one human against one bot of each of the server's seven brains, one at a
// time, weakest first (wire.h be_w_gauntlet). Pure bookkeeping, as tournament.h is for the bracket:
// the ladder a run is drawn with, which rung needs a game, who sits where, what a game's result does
// to the rung and the run, and the file the run is kept in. The league engine (league.c,
// BE_MODE=gauntlet) plays the games as real rated server games at a locked two-seat table of the
// human's and hands each result here.
//
// THE LADDER, drawn at the start of every run from the server's ratings then: for each brain (the
// strategy keys of BE_GAUNT_BRAINS) its highest-rated bot, the lower row index on a tie; the seven
// rungs in ascending order of that rating, ties in BE_GAUNT_BRAINS' order.
//
// A RUNG is a best of five: the first to BE_TOUR_WIN games the other was the fool of; a drawn game is
// played again, and a rung drawn BE_GAUNT_GAMES times goes to the one ahead, else to the bot (the
// human has to beat a rung to climb it). The human sits first in the rung's odd games (1, 3, 5...).
// Win it and he is on the next rung; lose it and the run is over where it stood. The best run (the
// highest rung reached, the most rungs won) outlives every run. A new run starts from rung 1.
#ifndef BE_GAUNTLET_H
#define BE_GAUNTLET_H

#include "wire.h"

#include <stdbool.h>
#include <stdint.h>

// The brains, by strategy key, in the order that breaks a rating tie.
#define BE_GAUNT_N_BRAINS BE_GAUNT_RUNGS
extern const char *const BE_GAUNT_BRAINS[BE_GAUNT_N_BRAINS];

// An empty record: no run yet (state IDLE, run 0, no best).
void be_gaunt_init(be_w_gauntlet *g, uint8_t human);

// A new run from rung 1 over `n` league rows (`rating[i]`, strategy key `key[i]`), `human` the
// human's row (never on the ladder). The best run and the run count carry over; everything else is
// new. false, `g` untouched, when a brain has no bot among the rows.
bool be_gaunt_new_run(be_w_gauntlet *g, const int32_t *rating, const char *const *key, int n, uint8_t human, int64_t now_ms);

// The rung that needs a game now (the run is going on and no game of it is in play), or -1; the
// seats for that game, the human first in the rung's odd games.
int  be_gaunt_next(const be_w_gauntlet *g, uint8_t seats[2]);
// The current rung's game is in play at `gid`.
void be_gaunt_started(be_w_gauntlet *g, const char *gid, int gid_len, int64_t now_ms);
// The current rung's game ended with `fool_bot` the fool (a league row; BE_TOUR_NONE a draw), or
// never started (`played` false: the rung asks again). Updates the rung and the run: a rung won
// climbs, a rung lost ends the run. true when the record changed.
bool be_gaunt_result(be_w_gauntlet *g, bool played, uint8_t fool_bot, int64_t now_ms);

// The title of the current rung's next game, into out (NUL-terminated, cut at cap): "Gauntlet <r>/7:
// <name> - Game <n> of 5" (or "- Game <n>" past the fifth), the name left out should the title pass
// BE_GAUNT_TITLE_MAX. `name` is the bot's as shown (without the server's "%"). Its length.
#define BE_GAUNT_TITLE_MAX 50   // the lobby's title rule: at most 50 characters (foolish/c/src/roster.c)
int  be_gaunt_game_title(const be_w_gauntlet *g, const char *name, int name_len, char *out, int cap);

// The file: be_w_gauntlet behind a head naming the rows by id, in the order its indices mean.
// load: false when there is none, it is not one this build reads, or it names a row today's list
// lacks; the indices are mapped to today's rows by id.
bool be_gaunt_load(be_w_gauntlet *g, const char *path, const char *const *ids, int n);
bool be_gaunt_save(const be_w_gauntlet *g, const char *path, const char *const *ids, int n);

#endif

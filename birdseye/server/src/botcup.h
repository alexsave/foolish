// botcup.h - THE BOT CUP: the four contest bots (foolish/c/botcup: kestrel, sapper, sonnet, haiku)
// in a round robin of two-player best-of-five matches, one match at a time in a fixed order, then a
// final between the top two (wire.h be_w_cup). Pure bookkeeping, as tournament.h is for the
// bracket; the league engine (league.c, BE_MODE=botcup) plays every game as a real rated server
// game through the dev-only bot table and hands each result here.
//
// A MATCH is the tournament's be_w_match and its rules: first to BE_TOUR_WIN games the other was
// the fool of, a drawn game played again, decided after BE_TOUR_MAX_GAMES for the one ahead (else
// a). a is named first and sits first in the match's odd games.
//
// THE MATCH SUMMARIES. Each game's story (be_cup_story) is told from the push events the watcher
// hears; a game's lines go to <dir>/<match id>.games, and when a match is decided they and the
// score become <dir>/<match id>.summary.txt, which a person writes the match's script from.
#ifndef BE_BOTCUP_H
#define BE_BOTCUP_H

#include "wire.h"

#include <stdbool.h>
#include <stdint.h>

// The bots' strategy keys, in be_w_cup.bots order, and how the site shows them.
extern const char *const BE_CUP_KEYS[BE_CUP_BOTS];    // "kestrel", "sapper", "sonnet", "haiku"

// A fresh cup over the league rows `bots` (BE_CUP_KEYS order): the round robin in its fixed order,
// the final's seats unknown.
void be_cup_draw(be_w_cup *c, const uint8_t bots[BE_CUP_BOTS], int64_t now_ms);

// The match that needs a game now (the first undecided one, if no game of it is in play and its
// two are known), or -1; the seats, a first in the match's odd games.
int  be_cup_next(const be_w_cup *c, uint8_t seats[2]);
void be_cup_started(be_w_cup *c, int m, const char *gid, int gid_len);
// The game of match `m` ended with `fool_bot` the fool (a league index; BE_TOUR_NONE a draw), or
// never started (`played` false). A decided round-robin match updates the standings; the sixth
// seats the final; the final decides the champion. true when the cup changed.
bool be_cup_result(be_w_cup *c, int m, bool played, uint8_t fool_bot, int64_t now_ms);
// The standings again from the decided round-robin matches (order, mw, ml, gw, gl).
void be_cup_standings(be_w_cup *c);

// Match m's id for the summary files: "m1-kestrel-v-haiku" .. "final-kestrel-v-sapper".
int  be_cup_match_id(const be_w_cup *c, int m, char *out, int cap);

// The file: be_w_cup behind a head naming the rows by id (as tournament.h's).
bool be_cup_load(be_w_cup *c, const char *path, const char *const *ids, int n);
bool be_cup_save(const be_w_cup *c, const char *path, const char *const *ids, int n);

// ---- a game's story --------------------------------------------------------------------------
#define BE_CUP_STORY 12000
typedef struct {
    int      len;
    char     text[BE_CUP_STORY];      // the key moments, a line each
    char     moves[BE_CUP_STORY];     // every move, compact ("k:A 7S 7H | h:C 9S")
    int      moves_len;
    uint32_t n_moves;
    uint32_t seat_moves[2], pickups[2], picked[2], passes[2], trumps[2];
    int8_t   out_order[2];
    int      n_out;
} be_cup_story;

// The game is dealt: trump suit, the face-up card, who leads. names[] are the seats' names.
void be_cup_story_deal(be_cup_story *s, const char *const names[2], int power_suit, int8_t flip_suit, int8_t flip_value,
                       int leader, int deck_count);
// One push event (wire.h BE_EV_*) by `seat`, its cards, the stock left after it.
void be_cup_story_event(be_cup_story *s, const char *const names[2], int power_suit, int kind, int seat,
                        const int8_t *suits, const int8_t *values, int n_cards, int deck_count);
// The game is over (`fool` the seat, -1 a draw): the story's last lines, then its whole text
// (header line "Game <g>: ..." included) appended to `path`. false when it could not be written.
bool be_cup_story_write(be_cup_story *s, const char *const names[2], int fool, int game_no, const char *path);

// The match is decided: <dir>/<id>.summary.txt from its games file and the score.
bool be_cup_summary_write(const be_w_cup *c, int m, const char *const *names, const char *games_path, const char *out_path);

#endif

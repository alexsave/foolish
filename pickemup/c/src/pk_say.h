/* Pick 'Em Up - which sentence a position says (RULES_AND_KERNEL.md 6).
 *
 * THE WORDS ARE THE TABLE'S (pickemup/c/i18n); this file only decides which
 * key a position says and what fills its {placeholders}. Which words go with
 * which position is a question about the game, so it is answered here once
 * rather than in each renderer (the uttt_say.h rule).
 *
 * A BUBBLE READS IN ITS SENDER'S LANGUAGE and is one line shown on every
 * phone, so a caption never says "you": it names the actor from the roster.
 * A screen line is drawn for one phone and may say "you".
 *
 * NO snprintf ANYWHERE: every sentence is a key filled by pk_fill, numbers
 * go through pk_itoa, and names are copied as bytes, so the same code runs
 * in a -nostdlib wasm build.
 *
 * Every function writes a NUL-terminated line into `out` and returns its
 * length in bytes; "" (0) is a real answer. -1 for a buffer too small or an
 * argument out of range.
 *
 * `names` is the roster: names[s] is seat s's nickname (UTF-8), or NULL /
 * "" for the SEAT_FALLBACK line ("Player 3"). `names` itself may be NULL. */
#ifndef PK_SAY_H
#define PK_SAY_H

#include "pk.h"
#include "pk_plan.h"
#include "../i18n/keys.h"

/* THE CAPTION IS ONE LINE (uttt measured 36 characters of a sent bubble's
 * row on iOS 27). pk_say_caption adds clauses after the first only while
 * the whole stays within this many columns. */
#define PK_CAPTION_MAX 36

/* ---- the table ------------------------------------------------------------ */

const char *pk_text(int key);             /* never NULL; "" off the table     */
const char *pk_key_name(int key);         /* "CAP_PLAYED"                     */
int         pk_key_max(int key);          /* column limit, 0 for none         */
int         pk_key_may_be_empty(int key);

/* Columns a string sets in: a CJK or Hangul character is two, a combining
 * mark none, anything else one. */
int pk_text_cols(const char *s);

/* Fill {placeholders} in `tmpl` from `kv` ("name", value, ..., NULL). {game}
 * is always GAME_NAME. An unknown placeholder is left as written. */
int pk_fill(char *out, int cap, const char *tmpl, const char *const *kv);

/* A decimal number, no libc. */
int pk_itoa(int v, char *out, int cap);

/* ---- things ------------------------------------------------------------------ */

int pk_say_card(uint8_t card, char *out, int cap);         /* "7 of circles"      */
int pk_say_seat(const char *const *names, int seat, char *out, int cap);
int pk_say_deck_left(const PkGame *g, char *out, int cap);   /* "27 left"           */
/* The same for any count and any direction: the words a board shows while a
 * plan plays (pk_beats), when the count and the direction are the timeline's. */
int pk_say_deck_n(int deck_n, char *out, int cap);
int pk_say_dir_of(int n_seats, int dir, char *out, int cap);   /* dir: PK_DIR_*     */
int pk_say_dir(const PkGame *g, char *out, int cap);         /* "" at 2 players     */

/* ---- the bubble -------------------------------------------------------------- */

/* The caption of sealed bubble `bubble` (1..bubbles), or of the deal for 0
 * ("Cards dealt. Bo goes first"). Its most important clause, then the next
 * while the line fits PK_CAPTION_MAX: the end, a catch, a "Last card!", the
 * play and its consequence (with the draws), a reshuffle, the next turn. */
int pk_say_caption(const PkGame *g, int bubble, const char *const *names, char *out, int cap);

/* The same caption from a bubble's own events (pk_plan of that one bubble,
 * PK_VIEW_ALL), for a host that already holds them. `seats` is the table's
 * size (a Reverse reads differently at two). */
int pk_say_caption_of(const PkEvent *ev, int n, int seats, const char *const *names,
                      char *out, int cap);

/* The lobby's captions, naming the sender. */
enum { PK_SAY_INVITE = 0, PK_SAY_JOINED, PK_SAY_LEFT };
int pk_say_lobby_caption(int which, const char *who, char *out, int cap);

/* ---- the screen, for one viewer --------------------------------------------- */

int pk_say_headline(const PkGame *g, int viewer, const char *const *names, char *out, int cap);
int pk_say_subline(const PkGame *g, int viewer, const char *const *names, char *out, int cap);

/* VoiceOver */
int pk_say_spoken_card(const PkGame *g, int viewer, int pos, char *out, int cap);
int pk_say_spoken_fan(const char *const *names, int seat, char *out, int cap);
int pk_say_spoken_deck(const PkGame *g, char *out, int cap);
int pk_say_spoken_stack(const PkGame *g, char *out, int cap);

/* ---- the rules page ---------------------------------------------------------- */

int pk_say_rules_title(char *out, int cap);     /* "How to play Pick 'Em Up" */
int pk_say_rule(int i, char *out, int cap);     /* 0..PK_RULES_N-1           */

#endif

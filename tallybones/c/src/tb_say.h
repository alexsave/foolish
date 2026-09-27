/* Tallybones - which sentence a position says (DECISIONS.md T8).
 *
 * THE WORDS ARE THE TABLE'S (tallybones/c/i18n); this file only decides
 * which key a position says and what fills its {placeholders}
 * (pickemup/c/src/pk_say.h's shape and its rules):
 *
 * A BUBBLE READS IN ITS SENDER'S LANGUAGE and is one line shown on every
 * phone, so a caption never says "you": it names the actor from the roster.
 * A screen line is drawn for one phone and may say "you".
 *
 * A CAPTION IS COMPOSED FROM THE BUBBLE'S OWN EVENTS (tb_plan), so a staged
 * bubble's caption comes from its draft plan, where a KEEP's reroll has no
 * values: "Alex keeps 3, 3, 5 and rerolls two" names the kept dice only
 * (T11), because nothing else exists yet.
 *
 * NO snprintf ANYWHERE: every sentence is a key filled by tb_fill, numbers go
 * through tb_itoa, names are copied as bytes, so the same code runs in a
 * -nostdlib wasm build.
 *
 * Every function writes a NUL-terminated line into `out` and returns its
 * length in bytes; "" (0) is a real answer. -1 for a buffer too small or an
 * argument out of range. `names` is the roster (names[s] UTF-8, or NULL /
 * "" for "Player 3"); `names` itself may be NULL. */
#ifndef TB_SAY_H
#define TB_SAY_H

#include "tb.h"
#include "tb_plan.h"
#include "../i18n/keys.h"

/* THE CAPTION IS ONE LINE (uttt measured 36 columns of a sent bubble's row on
 * iOS 27): clauses after the first are added only while the whole fits. */
#define TB_CAPTION_MAX 36

/* ---- the table ------------------------------------------------------------ */

const char *tb_text(int key);             /* never NULL; "" off the table     */
const char *tb_key_name(int key);         /* "CAP_KEEP"                       */
int         tb_key_max(int key);          /* column limit, 0 for none         */
int         tb_key_may_be_empty(int key);
int         tb_text_cols(const char *s);  /* a CJK or Hangul character is two */
/* Fill {placeholders} in `tmpl` from `kv` ("name", value, ..., NULL). {game}
 * is always GAME_NAME. An unknown placeholder is left as written. */
int tb_fill(char *out, int cap, const char *tmpl, const char *const *kv);
int tb_itoa(int v, char *out, int cap);

/* ---- things ------------------------------------------------------------------ */

int tb_say_seat(const char *const *names, int seat, char *out, int cap);
int tb_say_cat(int cat, char *out, int cap);          /* "Full House", "{game}" filled */
int tb_say_row_label(int row, char *out, int cap);    /* TB_ROW_* of tb_api.h: 0..15   */

/* ---- the bubble -------------------------------------------------------------- */

/* The caption of bubble `bubble` (0 the start, else hist[bubble - 1]): its
 * move, then the bonus and the next roll while the line fits TB_CAPTION_MAX;
 * the game's last bubble says only the result. */
int tb_say_caption(const TbGame *g, int bubble, const char *const *names, char *out, int cap);
/* Every clause of it, in the order they happened: MSMessage.summaryText. */
int tb_say_summary(const TbGame *g, int bubble, const char *const *names, char *out, int cap);
/* The same from a bubble's own events (tb_plan of that one bubble, or a
 * staged move's tb_plan_move); `full` 0 the caption, 1 the summary. */
int tb_say_of(const TbEvent *ev, int n, const char *const *names, int full, char *out, int cap);
/* The bubble a host is about to send: move `m` staged on resident `g`. */
int tb_say_move(const TbGame *g, TbMove m, const char *const *names, int full, char *out, int cap);

enum { TB_SAY_INVITE = 0, TB_SAY_JOINED, TB_SAY_LEFT };
int tb_say_lobby_caption(int which, const char *who, char *out, int cap);

/* ---- the screen, for one viewer (a seat, or -1) ------------------------------ */

int tb_say_headline(const TbGame *g, int viewer, const char *const *names, char *out, int cap);
int tb_say_subline(const TbGame *g, int viewer, const char *const *names, char *out, int cap);
int tb_say_spoken_die(const TbGame *g, int i, char *out, int cap);

int tb_say_rules_title(char *out, int cap);
int tb_say_rule(int i, char *out, int cap);          /* 0..TB_RULES_N-1 */

#endif

/* Chui Niu - which sentence a position says.
 *
 * THE WORDS ARE THE TABLE'S (chuiniu/c/i18n); this file only decides which
 * key a position says and what fills its {placeholders}, once, so no
 * renderer re-decides it (pickemup/c/src/pk_say.h's rule).
 *
 * A BUBBLE'S CAPTION is one line shown on every phone, so it never says
 * "you": it names the actor from the roster. A screen line is drawn for one
 * phone and may say "you".
 *
 * NO snprintf ANYWHERE: every sentence is a key filled by cn_fill, numbers
 * go through cn_itoa, names are copied as bytes, so the same code runs in a
 * -nostdlib wasm build.
 *
 * Every function writes a NUL-terminated line into `out` and returns its
 * length in bytes; "" (0) is a real answer; -1 for a buffer too small or an
 * argument out of range. `names[s]` is seat s's nickname (UTF-8), or NULL /
 * "" for SEAT_FALLBACK ("Player 3"); `names` itself may be NULL. */
#ifndef CN_SAY_H
#define CN_SAY_H

#include "cn.h"
#include "cn_plan.h"
#include "../i18n/keys.h"

/* ---- the table ------------------------------------------------------------ */

const char *cn_text(int key);             /* never NULL; "" off the table     */
const char *cn_key_name(int key);         /* "CAP_BID"                         */
int         cn_key_max(int key);          /* column limit, 0 for none         */
int         cn_key_may_be_empty(int key);
int         cn_text_cols(const char *s);  /* a CJK character is two columns   */
/* Fill {placeholders} from `kv` ("name", value, ..., NULL); {game} is always
 * GAME_NAME; an unknown placeholder is left as written. */
int cn_fill(char *out, int cap, const char *tmpl, const char *const *kv);
int cn_itoa(int v, char *out, int cap);

/* ---- things ------------------------------------------------------------------ */

/* "four 3s", or "Four 3s" with `initial` (the start of a sentence). */
int cn_say_bid(int q, int f, int initial, char *out, int cap);
int cn_say_seat(const char *const *names, int seat, char *out, int cap);
int cn_say_dice_n(int n, char *out, int cap);                    /* "4 dice" */

/* ---- the bubble -------------------------------------------------------------- */

/* The caption of move `move` (1..hist_n), or of the start for 0: "Dice
 * rolled. Alex bids first", "Alex bid four 3s", "Bo calls four 3s". A call's
 * caption NEVER says what the call found (DECISIONS K8): the caption is
 * written while the bubble is staged, on the caller's own screen. */
int cn_say_caption(const CnGame *g, int move, const char *const *names, char *out, int cap);
/* The same from that move's own events (cn_plan of it). */
int cn_say_caption_of(const CnEvent *ev, int n, const char *const *names, char *out, int cap);

/* THE OUTCOME of the newest call, a screen line once it is sent: "Bo calls.
 * Four 3s was true, Bo loses a die", then "Bo is out", then "Alex wins",
 * every clause, the most important first (K9). "" before the first call. */
int cn_say_outcome(const CnGame *g, const char *const *names, char *out, int cap);
int cn_say_outcome_of(const CnEvent *ev, int n, const char *const *names, char *out, int cap);

enum { CN_SAY_INVITE = 0, CN_SAY_JOINED, CN_SAY_LEFT };
int cn_say_lobby_caption(int which, const char *who, char *out, int cap);

/* ---- the screen, for one viewer --------------------------------------------- */

/* "Your turn: raise or call", "Bo's turn", "You win", "Alex wins". */
int cn_say_headline(const CnGame *g, int viewer, const char *const *names, char *out, int cap);
/* "Bid to beat: four 3s by Alex", "No bid yet". */
int cn_say_subline(const CnGame *g, const char *const *names, char *out, int cap);
/* "Send to bid four 3s" / "Send to call four 3s": a staged move of mine. */
int cn_say_staged(const CnGame *g, CnMove m, char *out, int cap);

/* "14 dice on the table". */
int cn_say_table(const CnGame *g, char *out, int cap);
/* The newest call's count: "There were five"; "" before the first call. */
int cn_say_reveal_count(const CnGame *g, char *out, int cap);
/* A lobby row: "2. Bo", or "2. Bo (You)" when `mine`. */
int cn_say_lobby_row(const char *const *names, int seat, int mine, char *out, int cap);
/* Why a link did not read, from a negative CN_E* (cn_msg.h): a newer
 * format, a damaged link, or anything else. */
int cn_say_error(int code, char *out, int cap);

int cn_say_rules_title(char *out, int cap);
int cn_say_rule(int i, char *out, int cap);     /* 0..CN_RULES_N-1 */

#endif

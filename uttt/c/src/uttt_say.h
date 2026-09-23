/* EVERY SENTENCE THE APP SAYS, in one table, for the same reason the nine
 * block names and the rulebook are here: which words go with which position
 * is a question about the game, and a second copy in a renderer is a second
 * answer that drifts. English only for now; a language is one more column.
 *
 * Nothing here names a person. A Messages extension gets a per-device UUID
 * for each participant and no name, and a BUBBLE is one bitmap and one
 * caption shown identically on every device - so the bubble's lines are
 * statements about the board, and the word "you" appears only on a screen,
 * which is drawn for one device.
 *
 * No em dashes in any of it. */
#ifndef UTTT_SAY_H
#define UTTT_SAY_H

#include "uttt.h"

enum {
    /* the bubble: baked into the image and its caption, same on every phone */
    UTTT_SAY_BUBBLE_HEADLINE = 0,  /* "A game?", "Your move", "X wins"      */
    UTTT_SAY_BUBBLE_PLACE,         /* "bottom middle", "58 moves", ""       */
    UTTT_SAY_CAPTION,              /* "Sent to the bottom-middle board."    */

    /* the play surface, drawn for `seat` (UTM_SEAT_*) */
    UTTT_SAY_HEADLINE_PRE,         /* words before the drawn mark           */
    UTTT_SAY_HEADLINE_POST,        /* words after it (UTTT_SAY_HEADLINE_MARK) */
    UTTT_SAY_SUBLINE,              /* "Anywhere you like.", "Top left."     */

    /* the spectator's one line */
    UTTT_SAY_WATCH_LABEL,          /* "watching"                            */
    UTTT_SAY_WATCH_LINE,           /* "X to play", "O took it"              */

    /* the lobby, and a bubble that cannot be read */
    UTTT_SAY_WAITING_HEADLINE,
    UTTT_SAY_WAITING_SUBLINE,
    UTTT_SAY_UNREADABLE_HEADLINE,
    UTTT_SAY_UNREADABLE_SUBLINE,

    /* the "you are" indicator, two lines over the drawn mark */
    UTTT_SAY_YOU_ARE_1,
    UTTT_SAY_YOU_ARE_2,

    UTTT_SAY_COUNT
};

/* Write sentence `key` for game `g` as seen from `seat` into `out`.
 * Returns its length; "" (0) is a real answer - a line with nothing to say
 * takes no room. -1 for an unknown key or a buffer too small. */
int uttt_say(int key, const UtttGame *g, int seat, char *out, int cap);

/* The mark the play-surface headline draws between PRE and POST, or 0 for a
 * headline that is words only. "Waiting on <O>", "<X> wins": the other side
 * is named by its mark, drawn in its own ink, because a mark is the only name
 * this side has. */
int uttt_say_headline_mark(const UtttGame *g, int seat);

#endif

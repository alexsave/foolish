/* EVERY SENTENCE THE APP SAYS, in one table, for the same reason the nine
 * block names and the rulebook are here: which words go with which position
 * is a question about the game, and a second copy in a renderer is a second
 * answer that drifts. English only for now; a language is one more column.
 *
 * A BUBBLE is one bitmap and one caption shown identically on every device,
 * so the bubble's lines are statements about the board and the word "you"
 * appears only on a screen, which is drawn for one device. The one person a
 * caption may name is the SENDER, and only through `who` (uttt_say_by): a
 * Messages extension has no names, only a participant UUID, and Messages
 * itself swaps "$<uuid>" in a caption for that person's name on every
 * device. The kernel never sees a name; it is handed the token.
 *
 * No em dashes in any of it. */
#ifndef UTTT_SAY_H
#define UTTT_SAY_H

#include "uttt.h"

enum {
    /* the bubble: baked into the image and its caption, same on every phone */
    UTTT_SAY_BUBBLE_HEADLINE = 0,  /* "A game?", "<X> to play", "<X> wins",
                                      the mark drawn (uttt_say_bubble_mark) */
    UTTT_SAY_BUBBLE_PLACE,         /* "bottom middle", "58 moves", ""       */
    UTTT_SAY_CAPTION,              /* "Sent to the bottom-middle board.",
                                      "<who> won on the diagonal. 58 moves.",
                                      "<who> wants a game. Tap to take it." */

    /* the play surface, drawn for `seat` (UTM_SEAT_*) */
    UTTT_SAY_HEADLINE_PRE,         /* words before the drawn mark           */
    UTTT_SAY_HEADLINE_POST,        /* words after it (UTTT_SAY_HEADLINE_MARK) */
    UTTT_SAY_SUBLINE,              /* "Anywhere you like.", "Top left.",
                                      at the end the winning line spoken:
                                      "Top left, centre, bottom right."     */

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

    /* the one door a screen may offer (utm_door) */
    UTTT_SAY_DOOR_AGAIN,

    /* VOICEOVER. The screen draws a mark where the words would name a side,
     * and a drawn mark is silent, so what VoiceOver reads is the same
     * sentence with the mark spelled: "Waiting on O", "X wins". */
    UTTT_SAY_HEADLINE_SPOKEN,
    UTTT_SAY_YOU_ARE_SPOKEN,       /* "You are X", "" with no seat         */
    UTTT_SAY_DOOR_RULES,           /* the rulebook door                     */

    /* getting a staged bubble out (uttt_msg.h, UTM_SEND_HINT_MS and
     * UTM_INSERT_DOOR) */
    UTTT_SAY_SEND_HINT,            /* under the arrow at Messages' Send     */
    UTTT_SAY_DOOR_SEND,            /* the door when every insert went unanswered */

    UTTT_SAY_COUNT
};

/* Write sentence `key` for game `g` as seen from `seat` into `out`.
 * Returns its length; "" (0) is a real answer - a line with nothing to say
 * takes no room. -1 for an unknown key or a buffer too small. */
int uttt_say(int key, const UtttGame *g, int seat, char *out, int cap);

/* The same, with `who` standing for the SENDER of the bubble being written -
 * in practice "$" and the local participant's UUID, which Messages renders as
 * a name. Only the invitation and the win name anybody, because those are the
 * two captions docs/UI.html writes with a name in them, and in both the
 * person named is the one sending: the creator sends the invitation and the
 * winner sends the winning move. NULL or "" words the same sentence without a
 * person ("X won ...", "New game?"). */
int uttt_say_by(int key, const UtttGame *g, int seat, const char *who,
                char *out, int cap);

/* The mark the BUBBLE's headline draws before its words, or 0 for "A game?"
 * and "A draw". The bubble cannot say "Your move" - it is one bitmap, and on
 * the sender's own phone it would be false - so it names the side to play by
 * its mark, drawn in its own ink, the way the screen names the other side. */
int uttt_say_bubble_mark(const UtttGame *g);

/* The mark the play-surface headline draws between PRE and POST, or 0 for a
 * headline that is words only. "Waiting on <O>", "<X> wins": the other side
 * is named by its mark, drawn in its own ink, because a mark is the only name
 * this side has. */
int uttt_say_headline_mark(const UtttGame *g, int seat);

/* WHAT VOICEOVER READS ON SQUARE `mv` (block*9 + cell): "Top left board,
 * centre square, empty", "... X", "... O". The square's rectangle is
 * uttt_cell_rect's, the inverse of uttt_hit. -1 for an `mv` off the board
 * or a buffer too small. */
int uttt_say_cell(const UtttGame *g, int mv, char *out, int cap);

#endif

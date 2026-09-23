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
    /* the bubble: baked into the image and its caption, same on every phone;
     * the image has words only once the game is over (uttt_bubble) */
    UTTT_SAY_BUBBLE_HEADLINE = 0,  /* "<X> wins", "A draw", "" - the mark
                                      drawn (uttt_say_bubble_mark)          */
    UTTT_SAY_BUBBLE_PLACE,         /* "58 moves", ""                        */
    UTTT_SAY_CAPTION,              /* "O to play, bottom-middle board",
                                      "X won on the diagonal in 58 moves",
                                      "New game?" - one line (uttt_caption) */

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

/* THE CAPTION IS ONE LINE. Messages sets it in one row under the 300-point
 * bubble and wraps a longer one onto a second (owner: never). Measured on
 * UtttRig (iOS 27, TESTFLIGHT_PLAN.md 11): a sent bubble is 309.7 points
 * wide with 17 points of padding each side, 275.7 for the words, and the
 * captions set at 7.41-7.56 points a character ("Sent anywhere on the sheet"
 * 196.7, "X won on the diagonal in 25 moves" 244.7) - 36 characters. The
 * limit leaves four of them spare for wider glyphs. */
#define UTTT_CAPTION_MAX 32

/* The caption from its parts: `over` (0, X, O or draw), `turn` the side to
 * play, `block` where they go (0..8, 9 anywhere; -1 for an empty board),
 * `line` the won line (uttt_won_line), `n` the plies, `who` as uttt_say_by.
 * Unnamed, every caption is at most UTTT_CAPTION_MAX characters: a win whose
 * line would not fit is said without it ("X won in 81 moves"). */
int uttt_caption(int over, int turn, int block, int line, int n, const char *who,
                 char *out, int cap);

/* The mark the BUBBLE's headline draws before its words - the winner's - or
 * 0: only a finished game's bubble has words (uttt_bubble). The bubble
 * cannot say "You win" - it is one bitmap, false on the loser's phone - so
 * it names the winner by its mark, drawn in its own ink. */
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

#include "uttt_say.h"
#include "uttt_draw.h"
#include "uttt_msg.h"
#include <stdio.h>
#include <string.h>

static int put(char *out, int cap, const char *s)
{
    int n = (int)strlen(s);
    if (n >= cap) return -1;
    memcpy(out, s, (size_t)n + 1);
    return n;
}

static int putf(int cap, int n)
{
    return (n < 0 || n >= cap) ? -1 : n;
}

int uttt_say_headline_mark(const UtttGame *g, int seat)
{
    int you = utm_seat_mark(seat);
    if (g->over == UTTT_DRAW) return 0;
    if (g->over) return g->over == you ? 0 : g->over;
    return g->turn == you ? 0 : g->turn;
}

/* HOW A WINNING LINE IS SAID in a caption, numbered as uttt_line_mask. */
static const char *const LINE_SAID[8] = {
    "across the top", "across the middle", "across the bottom",
    "down the left", "down the middle", "down the right",
    "on the diagonal", "on the diagonal",
};

/* "Top left, centre, bottom right." - the three blocks of the winning line,
 * in board order, which is the order a finger traces it. */
static int say_line(const UtttGame *g, char *out, int cap)
{
    int i = uttt_won_line(g);
    if (i < 0) return put(out, cap, "");
    unsigned m = uttt_line_mask(i);
    const char *b[3];
    int k = 0;
    for (int blk = 0; blk < 9 && k < 3; blk++)
        if ((m >> blk) & 1u) b[k++] = uttt_place_name(blk, 0);
    int n = snprintf(out, (size_t)cap, "%s, %s, %s.", b[0], b[1], b[2]);
    if (n < 0 || n >= cap) return -1;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    return n;
}

int uttt_say(int key, const UtttGame *g, int seat, char *out, int cap)
{
    if (!out || cap < 1) return -1;
    int you = utm_seat_mark(seat);
    int a = uttt_active(g);

    switch (key) {
    case UTTT_SAY_BUBBLE_HEADLINE:
        /* TWO WORDS OVER TWO LINES: the headline has about 87 points and
         * "Taken back" in bold 16 does not fit it ("Taken ba..." in the
         * transcript), so the headline says what is left and the blue line
         * says why. */
        if (seat == UTM_SEAT_CLOSED) return put(out, cap, "No game");
        /* AN EMPTY BOARD IS NOT A MOVE. Nobody has a seat yet, so there is no
         * move to be anybody's; the invitation asks the question instead. */
        switch (g->over) {
        case UTTT_DRAW: return put(out, cap, "A draw");
        case UTTT_X:    return put(out, cap, "X wins");
        case UTTT_O:    return put(out, cap, "O wins");
        default:        return put(out, cap, g->n_plies ? "Your move" : "A game?");
        }

    case UTTT_SAY_BUBBLE_PLACE:
        if (seat == UTM_SEAT_CLOSED) return put(out, cap, "taken back");
        /* A finished game has nowhere to send anybody, so the line says how
         * long it took; an invitation has nowhere either, and says nothing. */
        if (g->over) return putf(cap, snprintf(out, (size_t)cap, "%d moves", g->n_plies));
        if (!g->n_plies) return put(out, cap, "");
        return put(out, cap, uttt_place_name(a, 0));

    case UTTT_SAY_CAPTION:
        if (seat == UTM_SEAT_CLOSED) return put(out, cap, "Game taken back.");
        switch (g->over) {
        case UTTT_X: case UTTT_O: {
            /* docs/UI.html 05: "Alex won on the diagonal. 58 moves." The
             * name is WP4's ($<uuid> substitution); the mark stands in. */
            int i = uttt_won_line(g);
            return putf(cap, snprintf(out, (size_t)cap, "%s won %s. %d moves.",
                                      g->over == UTTT_X ? "X" : "O",
                                      i < 0 ? "" : LINE_SAID[i], g->n_plies));
        }
        case UTTT_DRAW:
            return putf(cap, snprintf(out, (size_t)cap, "Drawn. Nine blocks, no line. %d moves.",
                                      g->n_plies));
        default: break;
        }
        if (!g->n_plies) return put(out, cap, "New Ultimate Tic Tac Toe game");
        if (a == 9) return put(out, cap, "Sent anywhere on the sheet.");
        return putf(cap, snprintf(out, (size_t)cap, "Sent to the %s board.",
                                       uttt_place_name(a, 1)));

    case UTTT_SAY_HEADLINE_PRE:
        if (g->over == UTTT_DRAW) return put(out, cap, "Drawn");
        if (g->over) return put(out, cap, g->over == you ? "You win" : "");
        return put(out, cap, g->turn == you ? "Your move" : "Waiting on ");

    case UTTT_SAY_HEADLINE_POST:
        if (g->over && g->over != UTTT_DRAW && g->over != you)
            return put(out, cap, " wins");
        return put(out, cap, "");

    case UTTT_SAY_SUBLINE:
        /* When it is not your turn this is WHERE YOU SENT THEM, the one thing
         * worth reading on a board you cannot touch. */
        if (g->over == UTTT_DRAW) return put(out, cap, "Nine blocks, no line.");
        if (g->over) return say_line(g, out, cap);
        if (g->turn == you) return put(out, cap, a == 9 ? "Anywhere you like." : "");
        if (a == 9) return put(out, cap, "Anywhere they like.");
        {
            const char *p = uttt_place_name(a, 0);
            int n = snprintf(out, (size_t)cap, "%s.", p);
            if (n < 0 || n >= cap) return -1;
            if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
            return n;
        }

    case UTTT_SAY_WATCH_LABEL:
        return put(out, cap, "watching");

    case UTTT_SAY_WATCH_LINE:
        switch (g->over) {
        case UTTT_DRAW: return put(out, cap, "Drawn");
        case UTTT_X:    return put(out, cap, "X took it");
        case UTTT_O:    return put(out, cap, "O took it");
        default:        return put(out, cap, g->turn == UTTT_O ? "O to play" : "X to play");
        }

    case UTTT_SAY_WAITING_HEADLINE:
        return put(out, cap, "Waiting");
    case UTTT_SAY_WAITING_SUBLINE:
        /* NO MARK, and no hint of one: the joiner will be X, and until
         * somebody joins there is nobody to be anything. docs/UI.html, 02. */
        return put(out, cap, "Nobody has taken it yet.");
    case UTTT_SAY_UNREADABLE_HEADLINE:
        return put(out, cap, "Can't read that");
    case UTTT_SAY_UNREADABLE_SUBLINE:
        return put(out, cap, "That board came from a newer version of the app.");

    case UTTT_SAY_CLOSED_HEADLINE:
        return put(out, cap, "Taken back");
    case UTTT_SAY_CLOSED_SUBLINE:
        return put(out, cap, "Nobody can take this one.");

    case UTTT_SAY_DOOR_TAKE_BACK: return put(out, cap, "Take it back");
    case UTTT_SAY_DOOR_AGAIN:     return put(out, cap, "Again");

    case UTTT_SAY_YOU_ARE_1: return put(out, cap, "you");
    case UTTT_SAY_YOU_ARE_2: return put(out, cap, "are");

    default:
        return -1;
    }
}

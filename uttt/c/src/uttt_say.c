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

/* THE WINNING LINE BY NAME, for the end subline: "Left column", "Top row",
 * "Diagonal" (owner, over UI.html's "Top left, centre, bottom right."),
 * numbered as uttt_line_mask and worded to agree with LINE_SAID - which says
 * "on the diagonal" for both, so the subline does not tell them apart
 * either. */
static const char *const LINE_NAMED[8] = {
    "Top row", "Middle row", "Bottom row",
    "Left column", "Middle column", "Right column",
    "Diagonal", "Diagonal",
};

static int say_line(const UtttGame *g, char *out, int cap)
{
    int i = uttt_won_line(g);
    return put(out, cap, i < 0 ? "" : LINE_NAMED[i]);
}

int uttt_say_bubble_mark(const UtttGame *g)
{
    if (g->over == UTTT_X || g->over == UTTT_O) return g->over;
    if (g->over || !g->n_plies) return 0;
    return g->turn;
}

int uttt_say(int key, const UtttGame *g, int seat, char *out, int cap)
{
    return uttt_say_by(key, g, seat, NULL, out, cap);
}

int uttt_say_by(int key, const UtttGame *g, int seat, const char *who,
                char *out, int cap)
{
    if (!out || cap < 1) return -1;
    int named = who && who[0];
    int you = utm_seat_mark(seat);
    int a = uttt_active(g);

    switch (key) {
    case UTTT_SAY_BUBBLE_HEADLINE:
        /* AN EMPTY BOARD IS NOT A MOVE. Nobody has a seat yet, so there is no
         * move to be anybody's; the invitation asks the question instead.
         * Otherwise the words follow a DRAWN mark (uttt_say_bubble_mark):
         * "<O> to play" is true on both phones, where "Your move" - docs/UI.html
         * option 02 - is false on the sender's own copy of the same bitmap. */
        switch (g->over) {
        case UTTT_DRAW: return put(out, cap, "A draw");
        case UTTT_X: case UTTT_O: return put(out, cap, "wins");
        default:        return put(out, cap, g->n_plies ? "to play" : "A game?");
        }

    case UTTT_SAY_BUBBLE_PLACE:
        /* A finished game has nowhere to send anybody, so the line says how
         * long it took; an invitation has nowhere either, and says nothing. */
        if (g->over) return putf(cap, snprintf(out, (size_t)cap, "%d moves", g->n_plies));
        if (!g->n_plies) return put(out, cap, "");
        return put(out, cap, uttt_place_name(a, 0));

    case UTTT_SAY_CAPTION:
        switch (g->over) {
        case UTTT_X: case UTTT_O: {
            /* "X won on the diagonal in 58 moves" (owner, over UI.html 05's
             * "Alex won on the diagonal. 58 moves."). The winner made the
             * last move, so the winner is the sender and `who` is their
             * name; without one the mark stands in. */
            int i = uttt_won_line(g);
            return putf(cap, snprintf(out, (size_t)cap, "%s won %s in %d moves",
                                      named ? who : g->over == UTTT_X ? "X" : "O",
                                      i < 0 ? "" : LINE_SAID[i], g->n_plies));
        }
        case UTTT_DRAW:
            return putf(cap, snprintf(out, (size_t)cap, "Drawn in %d moves",
                                      g->n_plies));
        default: break;
        }
        /* docs/UI.html 01: "Alex wants a game. Tap to take it." The creator
         * sends the invitation, so the sender is the one asking. Nobody
         * named, it is "New game?" (owner, 2026-09-23). */
        if (!g->n_plies)
            return named ? putf(cap, snprintf(out, (size_t)cap,
                                              "%s wants a game. Tap to take it", who))
                         : put(out, cap, "New game?");
        if (a == 9) return put(out, cap, "Sent anywhere on the sheet");
        return putf(cap, snprintf(out, (size_t)cap, "Sent to the %s board",
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
        if (g->over == UTTT_DRAW) return put(out, cap, "Nine blocks, no line");
        if (g->over) return say_line(g, out, cap);
        if (g->turn == you) return put(out, cap, a == 9 ? "Anywhere you like" : "");
        if (a == 9) return put(out, cap, "Anywhere they like");
        {
            const char *p = uttt_place_name(a, 0);
            int n = snprintf(out, (size_t)cap, "%s", p);
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
        return put(out, cap, "Nobody has taken it yet");
    case UTTT_SAY_UNREADABLE_HEADLINE:
        return put(out, cap, "Can't read that");
    case UTTT_SAY_UNREADABLE_SUBLINE:
        return put(out, cap, "That board came from a newer version of the app");

    case UTTT_SAY_DOOR_AGAIN: return put(out, cap, "Again");

    case UTTT_SAY_HEADLINE_SPOKEN: {
        char pre[64], post[64];
        int m = uttt_say_headline_mark(g, seat);
        if (uttt_say(UTTT_SAY_HEADLINE_PRE, g, seat, pre, sizeof pre) < 0 ||
            uttt_say(UTTT_SAY_HEADLINE_POST, g, seat, post, sizeof post) < 0)
            return -1;
        return putf(cap, snprintf(out, (size_t)cap, "%s%s%s", pre,
                                  m == UTTT_X ? "X" : m == UTTT_O ? "O" : "", post));
    }
    case UTTT_SAY_YOU_ARE_SPOKEN:
        return put(out, cap, you == UTTT_X ? "You are X" : you == UTTT_O ? "You are O" : "");
    case UTTT_SAY_DOOR_RULES: return put(out, cap, "Rulebook");
    case UTTT_SAY_SEND_HINT:  return put(out, cap, "Send");
    case UTTT_SAY_DOOR_SEND:  return put(out, cap, "Send a board");

    case UTTT_SAY_YOU_ARE_1: return put(out, cap, "you");
    case UTTT_SAY_YOU_ARE_2: return put(out, cap, "are");

    default:
        return -1;
    }
}

int uttt_say_cell(const UtttGame *g, int mv, char *out, int cap)
{
    if (!out || cap < 1 || mv < 0 || mv > 80) return -1;
    int v = uttt_cell(g, mv);
    int n = snprintf(out, (size_t)cap, "%s board, %s square, %s",
                     uttt_place_name(mv / 9, 0), uttt_place_name(mv % 9, 0),
                     v == UTTT_X ? "X" : v == UTTT_O ? "O" : "empty");
    if (n < 0 || n >= cap) return -1;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    return n;
}

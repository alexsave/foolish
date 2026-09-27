#include "uttt_say.h"
#include "uttt_draw.h"
#include "uttt_msg.h"
#include "uttt_lang.h"
#include <stdio.h>
#include <string.h>

static int put(char *out, int cap, const char *s)
{
    int n = (int)strlen(s);
    if (n >= cap) return -1;
    memcpy(out, s, (size_t)n + 1);
    return n;
}


/* THE WORDS ARE THE TABLE'S (uttt_lang.h, uttt/c/i18n): every sentence
 * below is a key in the current language, and this file only decides which
 * key a position says and what fills its {placeholders}. */
#define T(k) uttt_text(UT_K_##k)

static const char *mark_name(int m) { return m == UTTT_X ? "X" : m == UTTT_O ? "O" : ""; }

/* "58 moves", in the plural the language gives 58 */
static int say_moves(int n, char *out, int cap)
{
    char num[12];
    snprintf(num, sizeof num, "%d", n);
    const char *kv[] = { "n", num, 0 };
    return uttt_fill(out, cap, uttt_text(UT_K_MOVES_ONE + uttt_plural(uttt_lang(), n)), kv);
}

int uttt_caption(int over, int turn, int block, int line, int n, const char *who,
                 char *out, int cap)
{
    if (!out || cap < 1) return -1;
    int named = who && who[0];
    const char *mark = over == UTTT_X || turn == UTTT_X ? "X" : "O";
    char num[12], moves[64];
    snprintf(num, sizeof num, "%d", n);
    if (say_moves(n, moves, sizeof moves) < 0) return -1;
    switch (over) {
    case UTTT_X: case UTTT_O: {
        /* "X won in 21 moves" and nothing about where: the bubble's image
         * shows the winning line, and naming it fit one transcript row in
         * too few languages (owner, 2026-09-26). The winner made the last
         * move, so the winner is the sender and `who` is their name; without
         * one the mark stands in. */
        (void)line;
        const char *by = named ? who : mark_name(over);
        const char *kv[] = { "who", by, "moves", moves, "n", num, 0 };
        int k = uttt_fill(out, cap, T(CAP_WON), kv);
        return k;
    }
    case UTTT_DRAW: {
        const char *kv[] = { "moves", moves, "n", num, 0 };
        return uttt_fill(out, cap, T(CAP_DRAWN), kv);
    }
    default: break;
    }
    /* docs/UI.html 01: "Alex wants a game. Tap to take it." The creator
     * sends the invitation, so the sender is the one asking. Nobody named,
     * it is "New game?" (owner, 2026-09-23). */
    if (block < 0) {
        const char *kv[] = { "who", who, 0 };
        return named ? uttt_fill(out, cap, T(CAP_INVITE), kv) : put(out, cap, T(CAP_NEW_GAME));
    }
    /* WHOSE TURN, and not where: the yellow tint on the bubble's image
     * already shows which board is live, so a "centre board" or "anywhere"
     * suffix only repeated it (owner, 2026-09-26). */
    const char *kv[] = { "mark", mark, 0 };
    return uttt_fill(out, cap, T(CAP_TO_PLAY), kv);
}

int uttt_say_headline_mark(const UtttGame *g, int seat)
{
    int you = utm_seat_mark(seat);
    if (g->over == UTTT_DRAW) return 0;
    if (g->over) return g->over == you ? 0 : g->over;
    return g->turn == you ? 0 : g->turn;
}

/* THE WINNING LINE BY NAME, for the end subline: "Left column", "Top row",
 * "Diagonal" (owner, over UI.html's "Top left, centre, bottom right."),
 * numbered as uttt_line_mask and worded to agree with line_said - which says
 * "on the diagonal" for both, so the subline does not tell them apart
 * either. */
static int say_line(const UtttGame *g, char *out, int cap)
{
    static const int KEY[8] = {
        UT_K_END_ROW_TOP, UT_K_END_ROW_MIDDLE, UT_K_END_ROW_BOTTOM,
        UT_K_END_COL_LEFT, UT_K_END_COL_MIDDLE, UT_K_END_COL_RIGHT,
        UT_K_END_DIAGONAL, UT_K_END_DIAGONAL,
    };
    int i = uttt_won_line(g);
    return put(out, cap, i < 0 ? "" : uttt_text(KEY[i]));
}

int uttt_say_watch_mark(const UtttGame *g)
{
    if (g->over == UTTT_DRAW) return 0;
    return g->over ? g->over : g->turn;
}

uint32_t uttt_say_headline_ink(const UtttGame *g)
{
    /* THE VERDICT IS SET IN THE WINNER'S INK: "You win" and "<O> wins" in
     * O red when O won (owner, 2026-09-26: O's win was set in X blue). A draw
     * keeps the X blue the end has always been set in; a live game is ink. */
    if (g->over == UTTT_X || g->over == UTTT_O) return uttt_mark_ink(g->over);
    return g->over == UTTT_DRAW ? uttt_mark_ink(UTTT_X) : UTTT_INK;
}

int uttt_say_bubble_mark(const UtttGame *g)
{
    return g->over == UTTT_X || g->over == UTTT_O ? g->over : 0;
}

int uttt_say(int key, const UtttGame *g, int seat, char *out, int cap)
{
    return uttt_say_by(key, g, seat, NULL, out, cap);
}

int uttt_say_by(int key, const UtttGame *g, int seat, const char *who,
                char *out, int cap)
{
    if (!out || cap < 1) return -1;
    int you = utm_seat_mark(seat);
    int a = uttt_active(g);

    switch (key) {
    case UTTT_SAY_BUBBLE_HEADLINE:
        /* ONLY A FINISHED GAME'S BUBBLE HAS WORDS (owner: the caption says
         * the rest). They follow a DRAWN mark (uttt_say_bubble_mark), true on
         * both phones, where "You win" is false on the loser's copy. */
        switch (g->over) {
        case UTTT_DRAW: return put(out, cap, T(BUBBLE_DRAW));
        case UTTT_X: case UTTT_O: return put(out, cap, T(BUBBLE_WINS));
        default:        return put(out, cap, "");
        }

    case UTTT_SAY_BUBBLE_PLACE:
        /* how long it took */
        if (g->over) return say_moves(g->n_plies, out, cap);
        return put(out, cap, "");

    case UTTT_SAY_CAPTION:
        return uttt_caption(g->over, g->turn, g->n_plies ? a : -1, uttt_won_line(g),
                            g->n_plies, who, out, cap);

    /* PRE IS LEFT OF THE DRAWN MARK AND POST RIGHT OF IT, on the screen
     * (keys.h): a right-to-left language puts the words it reads first in
     * POST. A headline with no mark is all PRE. */
    case UTTT_SAY_HEADLINE_PRE:
        if (g->over == UTTT_DRAW) return put(out, cap, T(HEAD_DRAWN));
        if (g->over) return put(out, cap, g->over == you ? T(HEAD_YOU_WIN) : T(HEAD_WINS_PRE));
        return put(out, cap, g->turn == you ? T(HEAD_YOUR_MOVE) : T(HEAD_WAITING_PRE));

    case UTTT_SAY_HEADLINE_POST:
        if (!uttt_say_headline_mark(g, seat)) return put(out, cap, "");
        return put(out, cap, g->over ? T(HEAD_WINS_POST) : T(HEAD_WAITING_POST));

    case UTTT_SAY_SUBLINE:
        /* WAITING ON THEM SAYS NOTHING UNDER IT (owner, 2026-09-25): it
         * named where you sent them ("Middle left"), which the board's
         * highlighter already shows. */
        if (g->over == UTTT_DRAW) return put(out, cap, T(END_DRAW));
        if (g->over) return say_line(g, out, cap);
        /* A LIVE GAME SAYS NOTHING UNDER THE HEADLINE (owner, 2026-09-26):
         * "Anywhere you like" repeated the yellow tint over the whole sheet,
         * as "Middle left" repeated it over one block before it went. */
        return put(out, cap, "");

    case UTTT_SAY_WATCH_LABEL:
        return put(out, cap, T(WATCH_LABEL));

    case UTTT_SAY_WATCH_LINE:
        /* After the drawn mark (uttt_say_watch_mark): a letter here was the
         * one place on the sheet a side was TYPED rather than drawn. */
        if (g->over == UTTT_DRAW) return put(out, cap, T(HEAD_DRAWN));
        return put(out, cap, g->over ? T(WATCH_TOOK) : T(WATCH_TO_PLAY));

    /* VOICEOVER hears whole sentences in reading order, each its own key,
     * rather than the screen's halves glued round a letter: which side of
     * the mark the words sit on is a question about the screen. */
    case UTTT_SAY_WATCH_SPOKEN: {
        const char *kv[] = { "mark", mark_name(uttt_say_watch_mark(g)), 0 };
        if (g->over == UTTT_DRAW) return put(out, cap, T(HEAD_DRAWN));
        return uttt_fill(out, cap, g->over ? T(SPOKEN_WATCH_TOOK) : T(CAP_TO_PLAY), kv);
    }

    case UTTT_SAY_WAITING_HEADLINE:
        return put(out, cap, T(LOBBY_WAITING));
    case UTTT_SAY_WAITING_SUBLINE:
        /* NO MARK, and no hint of one: the joiner will be X, and until
         * somebody joins there is nobody to be anything. docs/UI.html, 02. */
        return put(out, cap, T(LOBBY_NOBODY));
    case UTTT_SAY_UNREADABLE_HEADLINE:
        return put(out, cap, T(UNREADABLE));
    case UTTT_SAY_UNREADABLE_SUBLINE:
        return put(out, cap, T(UNREADABLE_WHY));

    case UTTT_SAY_DOOR_AGAIN: return put(out, cap, T(DOOR_AGAIN));

    case UTTT_SAY_HEADLINE_SPOKEN: {
        int m = uttt_say_headline_mark(g, seat);
        const char *kv[] = { "mark", mark_name(m), 0 };
        if (m) return uttt_fill(out, cap, g->over ? T(SPOKEN_WINS) : T(SPOKEN_WAITING), kv);
        return uttt_say(UTTT_SAY_HEADLINE_PRE, g, seat, out, cap);
    }
    case UTTT_SAY_YOU_ARE_SPOKEN: {
        const char *kv[] = { "mark", mark_name(you), 0 };
        return you == UTTT_X || you == UTTT_O ? uttt_fill(out, cap, T(SPOKEN_YOU_ARE), kv)
                                              : put(out, cap, "");
    }
    case UTTT_SAY_DOOR_RULES: return put(out, cap, T(DOOR_RULES));
    case UTTT_SAY_SEND_HINT:  return put(out, cap, T(SEND_HINT));
    case UTTT_SAY_DOOR_SEND:  return put(out, cap, T(DOOR_SEND));
    case UTTT_SAY_DOOR_COPY:  return put(out, cap, T(DOOR_COPY));
    case UTTT_SAY_DOOR_COPIED: return put(out, cap, T(DOOR_COPIED));

    case UTTT_SAY_YOU_ARE_1: return put(out, cap, T(YOU_ARE_1));
    case UTTT_SAY_YOU_ARE_2: return put(out, cap, T(YOU_ARE_2));

    default:
        return -1;
    }
}

int uttt_say_cell(const UtttGame *g, int mv, char *out, int cap)
{
    if (!out || cap < 1 || mv < 0 || mv > 80) return -1;
    int v = uttt_cell(g, mv);
    const char *kv[] = { "board", uttt_place_name(mv / 9), "cell", uttt_place_name(mv % 9),
                         "state", v == UTTT_X || v == UTTT_O ? mark_name(v) : T(CELL_EMPTY), 0 };
    int n = uttt_fill(out, cap, T(CELL), kv);
    if (n < 0) return -1;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    return n;
}

/* ---------------------------------------------------------------- the rules */
/* THE RULES, in the kernel with every other sentence: it is the one thing
 * that knows what they are, and a second copy in a renderer is a second
 * rulebook that drifts. docs/RULES.html (owner-approved, 2026-09-26), in its
 * order and in every language (uttt/c/i18n, RULE_1..RULE_8); each line has
 * its drawing (uttt_draw_rule). No em dashes, no curly quotes. */
int uttt_rules_count(void) { return UT_RULES_N; }

const char *uttt_rules_line(int i)
{
    if (i < 0 || i >= uttt_rules_count()) return "";
    return uttt_text(UT_K_RULE_1 + i);
}

const char *uttt_rules_title(void) { return T(RULES_TITLE); }

/* `needle` in `hay`, or NULL: the replay page's freestanding libc has no
 * strstr, and this file is in its build. */
static const char *find(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 0;
    for (const char *p = hay; *p; p++)
        if (!strncmp(p, needle, n)) return p;
    return 0;
}

/* THE TWO YELLOWS, named in the text and drawn round it as the board draws
 * them: the promise's pen outline, the wash's flat tint. Each language names
 * them in its own words (RULES_OUTLINE, RULES_TINT), and
 * tests/uttt_lang_test.c holds every language to naming them word for word
 * inside the rule they are drawn in. The offsets are BYTES of UTF-8; the
 * host turns them into its own string's units (UtttKernel.swift). */
int uttt_rules_yellow(int i, int *at, int *len)
{
    const char *line = uttt_rules_line(i);
    const char *yellow[2] = { T(RULES_OUTLINE), T(RULES_TINT) };
    for (int k = 0; k < 2; k++) {
        const char *p = find(line, yellow[k]);
        if (!p) continue;
        if (at) *at = (int)(p - line);
        if (len) *len = (int)strlen(yellow[k]);
        return k + 1;
    }
    if (at) *at = 0;
    if (len) *len = 0;
    return UTTT_RULES_PLAIN;
}

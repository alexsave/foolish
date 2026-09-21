/* The kernel's fast paths against the obvious ones.
 *
 *     make -C uttt/c line
 *
 * Two of them. `uttt_line` over every board there is, and `uttt_play`'s
 * legality rule against the list `uttt_legal` builds - because both were
 * rewritten for speed and both decide what the game IS.
 *
 * 4^9 boards times two marks is 524,288 cases and it runs in a blink, so
 * there is no reason to sample. `uttt_line` is 62% of a bot's runtime and it
 * decides who won: a wrong answer here is not a slow game, it is a different
 * game. */
#include "../src/uttt.h"
#include <stdio.h>

static const uint8_t LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}
};

/* The version this replaced, kept verbatim as the thing to agree with. */
static int slow_line(const uint8_t *nine, uint8_t mark)
{
    for (int i = 0; i < 8; i++)
        if (nine[LINES[i][0]] == mark &&
            nine[LINES[i][1]] == mark &&
            nine[LINES[i][2]] == mark) return 1;
    return 0;
}

/* The legal-move list the old way: walk the board a byte at a time. Kept
 * verbatim so the bitmask version has something to be checked against. */
static int slow_legal(const UtttGame *g, uint8_t *out)
{
    int n = 0;
    if (g->over) return 0;
    int lo = 0, hi = 9;
    if (g->forced != UTTT_ANY && g->block[g->forced] == UTTT_OPEN) {
        lo = g->forced; hi = g->forced + 1;
    }
    for (int b = lo; b < hi; b++) {
        if (g->block[b] != UTTT_OPEN) continue;
        for (int c = 0; c < 9; c++)
            if (g->cell[b * 9 + c] == UTTT_OPEN) out[n++] = (uint8_t)(b * 9 + c);
    }
    return n;
}

/* uttt_play accepts exactly the moves uttt_legal lists, and no others.
 *
 * `uttt_play` used to answer this BY calling `uttt_legal` and scanning the
 * result, which made the two trivially consistent and cost 24% of a bot's
 * runtime. Now it answers in three comparisons, so the agreement has to be
 * checked rather than assumed - over every square of every position, not
 * just the legal ones. */
static long check_legality(long games, long *positions)
{
    uint64_t rs = 0x6A09E667F3BCC909ull;
    long bad = 0;
    for (long k = 0; k < games; k++) {
        UtttGame g; uttt_init(&g);
        for (;;) {
            uint8_t list[81], slow[81];
            int n = uttt_legal(&g, list);
            int sn = slow_legal(&g, slow);
            (*positions)++;
            /* the masks and the byte walk must agree exactly, in order */
            if (sn != n) bad++;
            else for (int i = 0; i < n; i++) if (list[i] != slow[i]) { bad++; break; }
            /* the truth, as a set */
            int legal[81] = {0};
            for (int i = 0; i < n; i++) legal[list[i]] = 1;
            for (int mv = 0; mv < 81; mv++) {
                UtttGame t = g;
                int took = uttt_play(&t, (uint8_t)mv);
                if (took != legal[mv]) bad++;
            }
            /* and one past the end, which a byte can hold and a board cannot */
            UtttGame t = g;
            if (uttt_play(&t, 81) || uttt_play(&t, 255)) bad++;
            if (n <= 0) break;
            rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
            uttt_play(&g, list[rs % (uint64_t)n]);
        }
    }
    return bad;
}

int main(void)
{
    long cases = 0, bad = 0;
    uint8_t nine[9];
    for (long v = 0; v < 262144L; v++) {          /* 4^9 */
        long t = v;
        for (int i = 0; i < 9; i++) { nine[i] = (uint8_t)(t & 3); t >>= 2; }
        for (uint8_t mark = UTTT_X; mark <= UTTT_O; mark++) {
            cases++;
            if (uttt_line(nine, mark) != slow_line(nine, mark)) bad++;
        }
    }
    printf("uttt_line: %ld boards checked against the old one, %ld disagree\n",
           cases, bad);

    long positions = 0;
    long legal_bad = check_legality(400, &positions);
    printf("uttt_legal and uttt_play: %ld positions, the move list against a "
           "byte walk and\n  all 81 squares against the list, %ld disagree\n",
           positions, legal_bad);
    return (bad || legal_bad) ? 1 : 0;
}

/* The fast `uttt_line` against the obvious one, over every board there is.
 *
 *     make -C uttt/c line
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
    return bad ? 1 : 0;
}

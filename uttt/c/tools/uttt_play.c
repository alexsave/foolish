/* Play Ultimate Tic-Tac-Toe against the best bot in the file.
 *
 *     make -C uttt/c play              quill at 400 rollouts
 *     ./uttt/c/build/uttt_play 4000    ...and a much longer think
 *
 * COORDINATES ARE THE WHOLE SHEET, row and column 1..9, because that is
 * what a person looking at the picture can count. The block-and-cell pair
 * the rules are written in is derived from it; nobody should have to hold
 * two numbering schemes in their head to take a turn.
 *
 * Empty squares you may play print as +, empty squares you may not as the
 * dim dot, so the rule that makes this game hard - your move chooses where
 * your opponent plays next - is visible rather than explained.
 */
#include "../src/uttt.h"
#include "../src/uttt_bots.h"
#include "../src/uttt_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DIM   "\033[90m"
#define BOLD  "\033[1m"
#define XCOL  "\033[36m"     /* cyan  */
#define OCOL  "\033[33m"     /* amber */
#define HI    "\033[32m"     /* the squares you may use */
#define OFF   "\033[0m"

static int use_colour = 1;
/* --auto plays your side at random. It is here so the finish - the result
 * line and the replay code - can be exercised without a person. */
static int autoplay = 0;
static const char *c(const char *s) { return use_colour ? s : ""; }

/* row/col 0..8 of the whole sheet -> the move index the rules use */
static int rc_to_move(int r, int col)
{
    return ((r / 3) * 3 + col / 3) * 9 + (r % 3) * 3 + col % 3;
}

static void put_mark(uint8_t m, const char *empty)
{
    if (m == UTTT_X)      printf("%s%sX%s", c(BOLD), c(XCOL), c(OFF));
    else if (m == UTTT_O) printf("%s%sO%s", c(BOLD), c(OCOL), c(OFF));
    else                  printf("%s", empty);
}

static void draw(const UtttGame *g, int last)
{
    unsigned live = g->over ? 0u : uttt_legal_blocks(g);

    printf("\n      %s1 2 3   4 5 6   7 8 9%s\n", c(DIM), c(OFF));
    printf("    %s+-------+-------+-------+%s\n", c(DIM), c(OFF));
    for (int r = 0; r < 9; r++) {
        printf("  %s%d %s|%s ", c(DIM), r + 1, c(DIM), c(OFF));
        for (int col = 0; col < 9; col++) {
            int mv = rc_to_move(r, col);
            uint8_t m = uttt_cell(g, mv);
            int playable = (live >> (mv / 9)) & 1u;

            if (m == UTTT_OPEN && playable)
                printf("%s+%s", c(HI), c(OFF));
            else if (m == UTTT_OPEN)
                printf("%s.%s", c(DIM), c(OFF));
            else
                put_mark(m, ".");

            /* a ring around the square just played */
            if (mv == last) printf("%s<%s", c(BOLD), c(OFF));
            else if (col % 3 == 2) printf(" ");
            else printf(" ");

            if (col % 3 == 2 && col < 8) printf("%s|%s ", c(DIM), c(OFF));
        }
        printf("%s|%s\n", c(DIM), c(OFF));
        if (r % 3 == 2 && r < 8)
            printf("    %s+-------+-------+-------+%s\n", c(DIM), c(OFF));
    }
    printf("    %s+-------+-------+-------+%s\n", c(DIM), c(OFF));

    /* the nine boards, which is the game you are actually playing */
    printf("\n    boards won:  ");
    for (int b = 0; b < 9; b++) {
        uint8_t s = uttt_block(g, b);
        if (s == UTTT_OPEN)     printf("%s.%s", c(DIM), c(OFF));
        else if (s == UTTT_DRAW) printf("%s#%s", c(DIM), c(OFF));
        else put_mark(s, ".");
        if (b % 3 == 2 && b < 8) printf("   ");
        else printf(" ");
    }
    printf("\n");
}

static int read_move(const UtttGame *g, uint8_t *out)
{
    char line[128];
    for (;;) {
        unsigned live = uttt_legal_blocks(g);
        if (__builtin_popcount(live) > 1)
            printf("\n  your move %s(anywhere)%s, row col: ", c(DIM), c(OFF));
        else {
            int b = __builtin_ctz(live);
            printf("\n  your move %s(board %d, row %d-%d col %d-%d)%s, row col: ",
                   c(DIM), b + 1, (b / 3) * 3 + 1, (b / 3) * 3 + 3,
                   (b % 3) * 3 + 1, (b % 3) * 3 + 3, c(OFF));
        }
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) return 0;
        if (line[0] == 'q' || line[0] == 'Q') return 0;

        int r, col;
        if (sscanf(line, "%d %d", &r, &col) != 2 &&
            sscanf(line, "%d,%d", &r, &col) != 2) {
            printf("  two numbers 1..9, like \"5 5\". q to quit.\n");
            continue;
        }
        if (r < 1 || r > 9 || col < 1 || col > 9) {
            printf("  both between 1 and 9.\n");
            continue;
        }
        int mv = rc_to_move(r - 1, col - 1);
        if (uttt_cell(g, mv) != UTTT_OPEN) {
            printf("  that square is taken.\n");
            continue;
        }
        if (!((live >> (mv / 9)) & 1u)) {
            printf("  not that board - play where the %s+%s marks are.\n",
                   c(HI), c(OFF));
            continue;
        }
        *out = (uint8_t)mv;
        return 1;
    }
}

int main(int argc, char **argv)
{
    int budget = argc > 1 ? atoi(argv[1]) : 400;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-colour") == 0) use_colour = 0;
        if (strcmp(argv[i], "--auto") == 0) autoplay = 1;
    }
    if (getenv("NO_COLOR")) use_colour = 0;

    uint64_t rs = (uint64_t)time(NULL) * 6364136223846793005ull + 1442695040888963407ull;

    UtttGame g;
    uttt_init(&g);

    /* The bot moves first, which is the harder side to face and the whole
     * point of the exercise. */
    printf("\n%s  Ultimate Tic-Tac-Toe%s   you are %s%sO%s, quill is %s%sX%s"
           " at %d rollouts a move\n",
           c(BOLD), c(OFF), c(BOLD), c(OCOL), c(OFF), c(BOLD), c(XCOL), c(OFF),
           budget);
    printf("%s  win three squares in a line to take a board; three boards in a"
           " line wins.\n  where you play inside a board decides which board"
           " the other side plays in next.%s\n", c(DIM), c(OFF));

    int last = -1;
    char said[96]; said[0] = 0;
    while (!g.over) {
        uint8_t list[81];
        if (uttt_legal(&g, list) <= 0) break;

        if (g.turn == UTTT_X) {
            clock_t t0 = clock();
            uint8_t mv = uttt_bot_move(BOT_QUILL, &g, budget, &rs);
            double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
            uttt_play(&g, mv);
            last = mv;
            /* The board is drawn once a cycle, by the side that has to look
             * at it, and this goes underneath it rather than above. */
            snprintf(said, sizeof said, "  quill plays row %d col %d  %s(%.2fs)%s",
                     (mv / 9 / 3) * 3 + (mv % 9) / 3 + 1,
                     (mv / 9 % 3) * 3 + (mv % 9) % 3 + 1, c(DIM), dt, c(OFF));
        } else {
            draw(&g, last);
            if (said[0]) { printf("\n%s\n", said); said[0] = 0; }
            uint8_t mv;
            if (autoplay) {
                int n = uttt_legal(&g, list);
                mv = list[(int)(rs % (uint64_t)n)];
                rs = rs * 6364136223846793005ull + 1442695040888963407ull;
                printf("\n  you play row %d col %d\n",
                       (mv / 9 / 3) * 3 + (mv % 9) / 3 + 1,
                       (mv / 9 % 3) * 3 + (mv % 9) % 3 + 1);
            } else if (!read_move(&g, &mv)) { printf("\n  bye.\n"); return 0; }
            uttt_play(&g, mv);
            last = mv;
        }
    }

    draw(&g, last);
    if (said[0]) printf("\n%s\n", said);
    printf("\n%s  %s%s\n", c(BOLD),
           g.over == UTTT_DRAW ? "a draw." :
           g.over == UTTT_O ? "you win. genuinely well played." :
                              "quill wins.", c(OFF));

    uint8_t buf[64];
    int n = uttt_encode(&g, buf, sizeof buf);
    if (n > 0) {
        printf("  %d plies, replay code ", g.n_plies);
        for (int i = 0; i < n; i++) printf("%02x", buf[i]);
        printf("\n");
    }
    return 0;
}

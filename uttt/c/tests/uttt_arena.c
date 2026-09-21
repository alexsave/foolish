/* Round robin. Every bot against every other, both colours, and the result
 * printed as a table you can argue with.
 *
 *     make -C uttt/c arena                 200 games a pairing
 *     ./uttt/c/build/uttt_arena 400 300    400 games, 300 rollouts a candidate
 */
#include "../src/uttt_bots.h"
#include "../src/uttt_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t seed_of(int a, int b, int g)
{
    return 0x9E3779B97F4A7C15ull ^ ((uint64_t)a << 40)
         ^ ((uint64_t)b << 24) ^ ((uint64_t)g * 1000003ull);
}

/* One game. `x` plays first. Returns UTTT_X / UTTT_O / UTTT_DRAW. */
static uint8_t duel(UtttBot x, UtttBot o, int budget, uint64_t seed, int *plies)
{
    UtttGame g; uttt_init(&g);
    uint64_t rs = seed | 1;
    for (;;) {
        uint8_t list[81];
        if (uttt_legal(&g, list) <= 0) break;
        uttt_play(&g, uttt_bot_move(g.turn == UTTT_X ? x : o, &g, budget, &rs));
    }
    if (plies) *plies = g.n_plies;
    return g.over;
}

/* WHAT A BOT ACTUALLY DOES, as opposed to how often it wins. Every bot plays
 * itself, so the numbers describe its own taste rather than a matchup. */
static void profile(int games, int budget)
{
    printf("%-8s %7s %7s %7s %8s %8s %8s\n", "bot", "plies", "draws",
           "opener", "gift%", "blocks", "bytes");
    for (int a = 0; a < BOT_COUNT; a++) {
        long plies = 0, draws = 0, gifts = 0, decided = 0, blocks = 0, bytes = 0;
        int opener[81]; memset(opener, 0, sizeof opener);
        for (int i = 0; i < games; i++) {
            UtttGame g; uttt_init(&g);
            uint64_t rs = seed_of(a, a, i) | 1;
            for (;;) {
                uint8_t list[81];
                if (uttt_legal(&g, list) <= 0) break;
                uint8_t mv = uttt_bot_move((UtttBot)a, &g, budget, &rs);
                if (g.n_plies == 0) opener[mv]++;
                /* A GIFT is a move whose target block is already decided: the
                 * opponent may then play anywhere, which is the single most
                 * valuable thing you can hand them. */
                if (uttt_block(&g, mv % 9) != UTTT_OPEN) gifts++;
                decided++;
                uttt_play(&g, mv);
            }
            plies += g.n_plies;
            if (g.over == UTTT_DRAW) draws++;
            for (int b = 0; b < 9; b++)
                if (uttt_block(&g, b) == UTTT_X || uttt_block(&g, b) == UTTT_O) blocks++;
            uint8_t buf[64];
            int len = uttt_encode(&g, buf, sizeof buf);
            if (len > 0) bytes += len;
        }
        int bo = 0; for (int k = 1; k < 81; k++) if (opener[k] > opener[bo]) bo = k;
        printf("%-8s %7.1f %6.1f%% %4d/%-2d %7.1f%% %8.1f %8.1f\n",
               UTTT_BOT_NAME[a], (double)plies / games,
               (double)draws * 100 / games, bo / 9, bo % 9,
               (double)gifts * 100 / decided,
               (double)blocks / games, (double)bytes / games);
    }
    printf("\n  plies  mean game length when this bot plays both sides\n"
           "  draws  share of those games nobody won\n"
           "  opener the block/cell it opens with most often\n"
           "  gift%%  share of its moves that send the opponent to a DECIDED\n"
           "         block, handing them a free choice of the whole sheet\n"
           "  blocks how many of the nine get won rather than drawn\n"
           "  bytes  the coded game, which falls as the play gets tighter\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "profile") == 0) {
        profile(argc > 2 ? atoi(argv[2]) : 120, argc > 3 ? atoi(argv[3]) : 60);
        return 0;
    }
    int games  = argc > 1 ? atoi(argv[1]) : 200;
    int budget = argc > 2 ? atoi(argv[2]) : 200;

    printf("round robin: %d games a pairing (half as X, half as O), "
           "%d rollouts a candidate\n\n", games, budget);

    double win[BOT_COUNT][BOT_COUNT];   /* score for row against column */
    memset(win, 0, sizeof win);
    double secs[BOT_COUNT]; memset(secs, 0, sizeof secs);
    long   moves[BOT_COUNT]; memset(moves, 0, sizeof moves);

    for (int a = 0; a < BOT_COUNT; a++)
        for (int b = a + 1; b < BOT_COUNT; b++) {
            double sa = 0;
            for (int i = 0; i < games; i++) {
                /* THE SAME SEED FOR BOTH COLOURS. Half the games have a as X
                 * and half have b as X, and the pair shares a seed, so a
                 * lucky opening cannot favour one of them. */
                UtttBot x = (i & 1) ? b : a, o = (i & 1) ? a : b;
                clock_t t0 = clock();
                uint8_t w = duel(x, o, budget, seed_of(a, b, i / 2), NULL);
                double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
                secs[a] += dt / 2; secs[b] += dt / 2;
                if (w == UTTT_DRAW) sa += 0.5;
                else if ((int)(w == UTTT_X ? x : o) == a) sa += 1;
            }
            win[a][b] = sa / games;
            win[b][a] = 1 - win[a][b];
        }

    printf("%-9s", "");
    for (int b = 0; b < BOT_COUNT; b++) printf("%9s", UTTT_BOT_NAME[b]);
    printf("%9s\n", "overall");
    for (int a = 0; a < BOT_COUNT; a++) {
        printf("%-9s", UTTT_BOT_NAME[a]);
        double tot = 0; int n = 0;
        for (int b = 0; b < BOT_COUNT; b++) {
            if (a == b) { printf("%9s", "-"); continue; }
            printf("%8.1f%%", win[a][b] * 100);
            tot += win[a][b]; n++;
        }
        printf("%8.1f%%\n", tot / n * 100);
    }

    printf("\nseconds of thinking per bot (both sides of its pairings)\n");
    for (int a = 0; a < BOT_COUNT; a++)
        printf("  %-9s %6.1fs\n", UTTT_BOT_NAME[a], secs[a]);

    (void)moves;
    return 0;
}

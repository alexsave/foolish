/* Round robin. Every bot against every other, both colours, and the result
 * printed as a table you can argue with.
 *
 *     make -C uttt/c arena                 200 games a pairing
 *     ./uttt/c/build/uttt_arena 400 300    400 games, 300 rollouts a candidate
 *     ./uttt/c/build/uttt_arena 400 300 8  ...across eight processes
 *
 * THE THIRD ARGUMENT IS WHY A LADDER STOPPED TAKING AN HOUR. Every game
 * already derives its own seed from (a, b, index), so games are independent
 * and reproducible, and a run can be cut into shards that pool exactly: the
 * k-th worker plays the games whose index is k modulo the worker count, in
 * EVERY pairing, so each one does the same slice of the cheap matchups and
 * the dear ones and they finish together. Splitting by PAIRING instead would
 * hand one worker random-against-random and another quill-against-sniper.
 *
 * It is processes rather than threads because the bots keep state in globals
 * - the weights, the solver's table, the tree pool - and fork gives each
 * worker its own copy of all of it for nothing. The same reason says do not
 * reach for threads here later.
 *
 * Seconds are CPU, not wall clock, so they still add up across workers and
 * still say what they said before.
 */
#include "../src/uttt_bots.h"
#include "../src/uttt_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* What one worker owes the parent: unnormalised scores, plus the thinking. */
typedef struct {
    double sum[BOT_COUNT][BOT_COUNT];
    double secs[BOT_COUNT];
} Tally;

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

    int jobs = argc > 3 ? atoi(argv[3]) : 1;
    if (jobs < 1) jobs = 1;

    double win[BOT_COUNT][BOT_COUNT];   /* score for row against column */
    memset(win, 0, sizeof win);
    Tally all; memset(&all, 0, sizeof all);

    int pipes[64][2], nw = jobs > 64 ? 64 : jobs;
    for (int k = 0; k < nw; k++) {
        if (nw > 1) {
            if (pipe(pipes[k]) != 0) { perror("pipe"); return 1; }
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); return 1; }
            if (pid > 0) { close(pipes[k][1]); continue; }
            close(pipes[k][0]);
        }

        /* the worker (or, at one job, this process) */
        Tally t; memset(&t, 0, sizeof t);
        for (int a = 0; a < BOT_COUNT; a++)
            for (int b = a + 1; b < BOT_COUNT; b++)
                for (int i = 0; i < games; i++) {
                    if (nw > 1 && i % nw != k) continue;
                    /* THE SAME SEED FOR BOTH COLOURS. Half the games have a
                     * as X and half have b as X, and the pair shares a seed,
                     * so a lucky opening cannot favour one of them. */
                    UtttBot x = (i & 1) ? b : a, o = (i & 1) ? a : b;
                    clock_t t0 = clock();
                    uint8_t w = duel(x, o, budget, seed_of(a, b, i / 2), NULL);
                    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
                    t.secs[a] += dt / 2; t.secs[b] += dt / 2;
                    if (w == UTTT_DRAW) t.sum[a][b] += 0.5;
                    else if ((int)(w == UTTT_X ? x : o) == a) t.sum[a][b] += 1;
                }

        if (nw == 1) { all = t; break; }
        ssize_t off = 0, n;
        while (off < (ssize_t)sizeof t &&
               (n = write(pipes[k][1], (char *)&t + off, sizeof t - off)) > 0)
            off += n;
        close(pipes[k][1]);
        _exit(0);
    }

    if (nw > 1) {
        for (int k = 0; k < nw; k++) {
            Tally t; ssize_t off = 0, n;
            while (off < (ssize_t)sizeof t &&
                   (n = read(pipes[k][0], (char *)&t + off, sizeof t - off)) > 0)
                off += n;
            close(pipes[k][0]);
            if (off != (ssize_t)sizeof t) {
                fprintf(stderr, "worker %d returned %zd of %zu bytes\n",
                        k, off, sizeof t);
                return 1;
            }
            for (int a = 0; a < BOT_COUNT; a++) {
                all.secs[a] += t.secs[a];
                for (int b = 0; b < BOT_COUNT; b++) all.sum[a][b] += t.sum[a][b];
            }
        }
        while (wait(NULL) > 0) { }
    }

    double *secs = all.secs;
    for (int a = 0; a < BOT_COUNT; a++)
        for (int b = a + 1; b < BOT_COUNT; b++) {
            win[a][b] = all.sum[a][b] / games;
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

    return 0;
}

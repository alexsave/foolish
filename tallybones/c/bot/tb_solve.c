#define _DEFAULT_SOURCE   /* clock_gettime, sysconf(_SC_NPROCESSORS_ONLN) on Linux */
#define _DARWIN_C_SOURCE  /* ... and on macOS */
/* tb_solve - the exact expected final score of optimal single-player play,
 * and games played by that policy.
 *
 *   tb_solve                    build the table, print the exact EV
 *   tb_solve play N [SEED]      ... then play N games (deal_rng, tb_bot.h),
 *                               print the mean, its standard error and a
 *                               histogram in bins of 10 points
 *   -t THREADS                  workers (default: every core)
 */
#include "tb_bot.h"
#include "../../../shared/c/stats/stats.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t games = 0, seedv = 0;
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) { threads = atoi(argv[++i]); continue; }
        if (pos == 0 && !strcmp(argv[i], "play")) { pos = 1; continue; }
        if (pos == 1) { games = strtoull(argv[i], 0, 10); pos = 2; continue; }
        if (pos == 2) { seedv = strtoull(argv[i], 0, 10); pos = 3; continue; }
        fprintf(stderr, "usage: tb_solve [-t THREADS] [play N [SEED]]\n");
        return 2;
    }
    TbBot *b = malloc(sizeof *b);
    if (!b) return 1;
    double t0 = now();
    if (!tb_bot_build(b, threads)) { fprintf(stderr, "tb_solve: thread failure\n"); return 1; }
    double t1 = now();
    double ev = tb_bot_value(b, TB_FULL_CARD, 0);
    printf("exact expected final score of optimal play: %.6f\n", ev);
    printf("table: %d x %d states, built in %.2f s on %d threads\n", TB_BOT_MASKS, TB_BOT_UPPER, t1 - t0,
           threads);
    if (games) {
        uint8_t seed[32];
        memcpy(seed, "tallybones bot simulation seed..", 32);
        for (int i = 0; i < 8; i++) seed[16 + i] = (uint8_t)(seedv >> (8 * i));
        static TbBotSim sim;
        if (!tb_bot_simulate(b, seed, games, threads, &sim)) { fprintf(stderr, "tb_solve: bad game\n"); return 1; }
        StatSums st = { (double)sim.n, (double)sim.sum, (double)sim.sumsq };
        double n = st.n, mean = stat_mean(&st), var = stat_variance(&st), se = stat_stderr(&st);
        printf("played %llu games in %.2f s: mean %.4f, sd %.4f, standard error %.4f, exact - mean = %+.4f (%.2f SE)\n",
               (unsigned long long)sim.n, now() - t1, mean, sqrt(var), se, ev - mean, (ev - mean) / se);
        int lo = TB_BOT_MAX_SCORE, hi = 0;
        for (int s = 0; s < TB_BOT_MAX_SCORE; s++)
            if (sim.hist[s]) { if (s < lo) lo = s; hi = s; }
        uint64_t binmax = 0, bins[TB_BOT_MAX_SCORE / 10] = { 0 };
        for (int s = 0; s < TB_BOT_MAX_SCORE; s++) bins[s / 10] += sim.hist[s];
        for (int i = 0; i < TB_BOT_MAX_SCORE / 10; i++) if (bins[i] > binmax) binmax = bins[i];
        printf("min %d, max %d\n", lo, hi);
        for (int i = lo / 10; i <= hi / 10; i++) {
            printf("%3d-%3d %8llu %6.3f%% ", i * 10, i * 10 + 9, (unsigned long long)bins[i], 100.0 * bins[i] / n);
            for (uint64_t j = 0; j < bins[i] * 50 / binmax; j++) putchar('#');
            putchar('\n');
        }
    }
    free(b);
    return 0;
}

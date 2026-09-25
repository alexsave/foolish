/* The post-game analyser, from the command line.
 *
 *     make -C uttt/c analyse
 *     ./uttt/c/build/uttt_analyse <code | https://www.foolish.cards/uttt/<code>> [playouts]
 *
 * Every ply held against quill at a fixed, seeded budget (default 20000
 * playouts a position); see src/uttt_analyse.h. The report goes to stdout,
 * progress and the time taken to stderr, so the report is the same bytes
 * every run. */
#include "../src/uttt_analyse.h"
#include "../src/uttt_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void progress(int ply, int n)
{
    fprintf(stderr, "\r  ply %d/%d", ply, n);
    if (ply == n) fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <replay code or link> [playouts]\n", argv[0]);
        return 2;
    }
    UtttGame g;
    if (!uttt_replay_read(argv[1], &g, NULL)) {
        fprintf(stderr, "not a game: %s\n", argv[1]);
        return 1;
    }
    long playouts = argc > 2 ? atol(argv[2]) : 20000;
    if (playouts < 1) playouts = 1;

    static UtttNote notes[UTTT_MAX_PLIES];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uttt_analyse(&g, playouts, notes, progress);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "  analysed in %.1fs\n",
            (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    uttt_analyse_print(stdout, &g, notes, playouts);
    return 0;
}

/* Play a thousand games, encode every one, decode every one, and check that
 * what comes back is the same game move for move. Then report what it cost.
 *
 *     make -C uttt/c run
 */
#include "../src/uttt_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t RS;
static uint32_t rnd(uint32_t n)          /* xorshift64*, no libc dependency */
{
    RS ^= RS >> 12; RS ^= RS << 25; RS ^= RS >> 27;
    return (uint32_t)(((RS * 2685821657736338717ull) >> 33) % n);
}

/* Two bots. `uniform` picks any legal move. `stretch` refuses to close a block
 * when it has any alternative - a closed block freezes its empty cells for the
 * rest of the game, so avoiding that is how a game gets long. */
static uint8_t pick(const UtttGame *g, const uint8_t *list, int n, int stretch)
{
    if (!stretch) return list[rnd((uint32_t)n)];
    uint8_t safe[81]; int ns = 0;
    for (int i = 0; i < n; i++) {
        UtttGame t = *g;
        uttt_play(&t, list[i]);
        if (t.block[list[i] / 9] == UTTT_OPEN) safe[ns++] = list[i];
    }
    return ns ? safe[rnd((uint32_t)ns)] : list[rnd((uint32_t)n)];
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

typedef struct { double bytes, ideal; int plies; } Row;

static void report(const char *name, Row *r, int n)
{
    double *by = malloc(sizeof(double) * (size_t)n);
    double *id = malloc(sizeof(double) * (size_t)n);
    double *pl = malloc(sizeof(double) * (size_t)n);
    double sb = 0, si = 0, sp = 0;
    int longest = 0; double longest_bytes = 0, most_bytes = 0;
    for (int i = 0; i < n; i++) {
        by[i] = r[i].bytes; id[i] = r[i].ideal; pl[i] = r[i].plies;
        sb += r[i].bytes; si += r[i].ideal; sp += r[i].plies;
        if (r[i].plies > longest) { longest = r[i].plies; longest_bytes = r[i].bytes; }
        if (r[i].bytes > most_bytes) most_bytes = r[i].bytes;
    }
    qsort(by, (size_t)n, sizeof(double), cmp_d);
    qsort(id, (size_t)n, sizeof(double), cmp_d);
    qsort(pl, (size_t)n, sizeof(double), cmp_d);
    int p50 = n / 2, p95 = (int)(n * 0.95), p99 = (int)(n * 0.99);

    printf("\n%s  (%d games)\n", name, n);
    printf("  plies   mean %5.1f   p50 %3.0f   p95 %3.0f   max %3.0f\n",
           sp / n, pl[p50], pl[p95], pl[n - 1]);
    printf("  bytes   mean %5.2f   p50 %3.0f   p95 %3.0f   p99 %3.0f   max %3.0f\n",
           sb / n, by[p50], by[p95], by[p99], by[n - 1]);
    printf("  ideal   mean %5.2f bits (%5.2f bytes)   coder overhead %+.2f bytes\n",
           si / n, si / n / 8.0, sb / n - si / n / 8.0);
    printf("  longest game %d plies, %.0f bytes;  most expensive %.0f bytes\n",
           longest, longest_bytes, most_bytes);
    free(by); free(id); free(pl);
}

/* `curve` prints the mean cost of ply k, which is what the design document
 * draws. It comes out of this kernel so the picture and the shipped rules
 * cannot drift apart. */
static void curve(int games)
{
    static double acc[UTTT_MAX_PLIES]; static int cnt[UTTT_MAX_PLIES];
    uint8_t list[81];
    for (int i = 0; i < games; i++) {
        UtttGame g; uttt_init(&g);
        for (;;) {
            int n = uttt_legal(&g, list);
            if (n <= 0) break;
            acc[g.n_plies] += log2((double)n);
            cnt[g.n_plies]++;
            uttt_play(&g, list[rnd((uint32_t)n)]);
        }
    }
    printf("[");
    for (int k = 0; k < UTTT_MAX_PLIES; k++) {
        if (cnt[k] < games / 400) break;
        printf("%s%.3f", k ? "," : "", acc[k] / cnt[k]);
    }
    printf("]\n");
}

/* How long games actually are, for both bots. Prints two rows of counts
 * indexed from ply 0, so the document can draw the distribution rather than
 * quote a mean at it. */
static void hist(int games)
{
    static int h[2][UTTT_MAX_PLIES + 1];
    uint8_t list[81];
    for (int mode = 0; mode < 2; mode++) {
        RS = 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < games; i++) {
            UtttGame g; uttt_init(&g);
            for (;;) {
                int n = uttt_legal(&g, list);
                if (n <= 0) break;
                uttt_play(&g, pick(&g, list, n, mode));
            }
            h[mode][g.n_plies]++;
        }
    }
    for (int mode = 0; mode < 2; mode++) {
        printf("%s[", mode ? "," : "[");
        for (int k = 0; k <= UTTT_MAX_PLIES; k++)
            printf("%s%d", k ? "," : "", h[mode][k]);
        printf("]");
    }
    printf("]\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "hist") == 0) {
        hist(argc > 2 ? atoi(argv[2]) : 100000);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "curve") == 0) {
        RS = 0x9E3779B97F4A7C15ull;
        curve(argc > 2 ? atoi(argv[2]) : 20000);
        return 0;
    }
    int games = argc > 1 ? atoi(argv[1]) : 1000;
    RS = argc > 2 ? strtoull(argv[2], NULL, 10) : 0x9E3779B97F4A7C15ull;

    Row *ru = malloc(sizeof(Row) * (size_t)games);
    Row *rs = malloc(sizeof(Row) * (size_t)games);
    int fails = 0, checked = 0, gmax = 0;
    uint8_t gmax_moves[UTTT_MAX_PLIES]; int gmax_n = 0; double gmax_bytes = 0;

    for (int mode = 0; mode < 2; mode++) {
        Row *rows = mode ? rs : ru;
        for (int i = 0; i < games; i++) {
            UtttGame g; uttt_init(&g);
            uint8_t list[81];
            for (;;) {
                int n = uttt_legal(&g, list);
                if (n <= 0) break;
                uttt_play(&g, pick(&g, list, n, mode));
            }
            rows[i].bytes = 0; rows[i].ideal = 0; rows[i].plies = g.n_plies;
            uint8_t buf[256];
            int len = uttt_encode(&g, buf, sizeof buf);
            if (len < 0) { fails++; continue; }

            UtttGame back;
            if (!uttt_decode(&back, buf, (size_t)len)) { fails++; continue; }
            if (back.n_plies != g.n_plies ||
                memcmp(back.move, g.move, (size_t)g.n_plies) != 0 ||
                memcmp(back.cell, g.cell, sizeof g.cell) != 0 ||
                back.over != g.over) { fails++; continue; }
            checked++;

            rows[i].bytes = len;
            rows[i].ideal = uttt_ideal_bits(&g);
            rows[i].plies = g.n_plies;
            if (g.n_plies > gmax) {
                gmax = g.n_plies; gmax_n = g.n_plies; gmax_bytes = len;
                memcpy(gmax_moves, g.move, (size_t)g.n_plies);
            }
        }
    }

    /* EVERY PREFIX, not just the finished game. The suite encoded only games
     * that had been played out, so a decoder that always played to the end
     * passed it - and no bubble in a live thread could be read back. */
    int partial = 0, partial_fail = 0;
    for (int t2 = 0; t2 < 200; t2++) {
        UtttGame g; uttt_init(&g);
        uint8_t list[81];
        RS = 0x9E3779B97F4A7C15ull ^ ((uint64_t)t2 * 7919u);
        for (;;) {
            int n = uttt_legal(&g, list);
            if (n <= 0) break;
            uttt_play(&g, pick(&g, list, n, t2 & 1));
            uint8_t b[64];
            int ln = uttt_encode(&g, b, sizeof b);
            UtttGame back;
            partial++;
            if (ln < 0 || !uttt_decode(&back, b, (size_t)ln)
                || back.n_plies != g.n_plies
                || memcmp(back.move, g.move, (size_t)g.n_plies) != 0
                || memcmp(back.cell, g.cell, sizeof g.cell) != 0
                || back.forced != g.forced || back.turn != g.turn
                || back.over != g.over) {
                if (partial_fail < 4)
                    printf("  PARTIAL FAIL at %d plies (%d bytes)\n",
                           g.n_plies, ln);
                partial_fail++;
            }
        }
    }
    printf("mid-game round trip: %d positions, %d mismatches\n",
           partial, partial_fail);
    fails += partial_fail;

    printf("round trip: %d games encoded and decoded, %d mismatches\n",
           checked, fails - partial_fail);
    report("uniform bot", ru, games);
    report("stretch bot (never closes a block if it can help it)", rs, games);

    printf("\nthe longest game seen: %d plies in %.0f bytes\n", gmax_n, gmax_bytes);
    printf("  moves:");
    for (int i = 0; i < gmax_n; i++) printf("%s%d", i % 27 ? " " : "\n   ", gmax_moves[i]);
    double fac = 0;
    for (int i = 1; i <= 81; i++) fac += log2(i);
    printf("\n\nceiling for comparison: 81 plies is the most the board can hold,\n"
           "and log2(81!) = %.0f bits = %.0f bytes if every move were free.\n",
           fac, fac / 8);
    free(ru); free(rs);
    return fails ? 1 : 0;
}

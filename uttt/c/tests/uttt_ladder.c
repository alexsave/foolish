/* The bot ladder: a round robin where every ENTRANT is a bot at its own
 * rollout budget, so one engine can fill two tiers (quill at 200 and 4000),
 * then Elo fitted to the whole table (Bradley-Terry, iterated).
 *
 *     ./uttt/c/build/uttt_ladder [games-per-pairing] [workers]
 *
 * Seeds are (a, b, game) as in the arena, both colours share a seed, and the
 * run is cut into shards by game index across forked workers.
 */
#include "../src/uttt_bots.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { UtttBot bot; int budget; const char *label; } Entrant;
static const Entrant E[] = {
    { BOT_RANDOM, 1,    "random"      },
    { BOT_BIRO,   1,    "biro"        },
    { BOT_ROLLER, 200,  "roller@200"  },
    { BOT_NIB,    200,  "nib@200"     },
    { BOT_QUILL,  200,  "quill@200"   },
    { BOT_QUILL,  1000, "quill@1000"  },
    { BOT_QUILL,  4000, "quill@4000"  },
};
#define N ((int)(sizeof E / sizeof E[0]))

static uint64_t seed_of(int a, int b, int g)
{
    return 0x9E3779B97F4A7C15ull ^ ((uint64_t)a << 40) ^ ((uint64_t)b << 24)
         ^ ((uint64_t)g * 1000003ull);
}

static uint8_t duel(const Entrant *x, const Entrant *o, uint64_t seed)
{
    UtttGame g; uttt_init(&g);
    uint64_t rs = seed | 1;
    for (;;) {
        uint8_t list[81];
        if (uttt_legal(&g, list) <= 0) break;
        const Entrant *m = g.turn == UTTT_X ? x : o;
        uttt_play(&g, uttt_bot_move(m->bot, &g, m->budget, &rs));
    }
    return g.over;
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 40;
    int nw = argc > 2 ? atoi(argv[2]) : 8;
    if (nw < 1) nw = 1; if (nw > 64) nw = 64;
    static double sum[N][N];
    int pipes[64][2];
    for (int k = 0; k < nw; k++) {
        if (pipe(pipes[k]) != 0) { perror("pipe"); return 1; }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) { close(pipes[k][1]); continue; }
        close(pipes[k][0]);
        static double t[N][N];
        for (int a = 0; a < N; a++)
            for (int b = a + 1; b < N; b++)
                for (int i = 0; i < games; i++) {
                    if (i % nw != k) continue;
                    const Entrant *x = (i & 1) ? &E[b] : &E[a], *o = (i & 1) ? &E[a] : &E[b];
                    uint8_t w = duel(x, o, seed_of(a, b, i / 2));
                    if (w == UTTT_DRAW) t[a][b] += 0.5;
                    else if ((w == UTTT_X ? x : o) == &E[a]) t[a][b] += 1;
                }
        if (write(pipes[k][1], t, sizeof t) != (ssize_t)sizeof t) _exit(1);
        _exit(0);
    }
    for (int k = 0; k < nw; k++) {
        static double t[N][N]; size_t off = 0; ssize_t n;
        while (off < sizeof t && (n = read(pipes[k][0], (char *)t + off, sizeof t - off)) > 0) off += n;
        close(pipes[k][0]);
        if (off != sizeof t) { fprintf(stderr, "worker %d short\n", k); return 1; }
        for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) sum[a][b] += t[a][b];
    }
    for (int a = 0; a < N; a++) for (int b = a + 1; b < N; b++) sum[b][a] = games - sum[a][b];

    /* Bradley-Terry by MM iteration, then Elo = 400 log10(strength), random = 0. */
    double s[N]; for (int i = 0; i < N; i++) s[i] = 1;
    for (int it = 0; it < 5000; it++) {
        double ns[N];
        for (int i = 0; i < N; i++) {
            double w = 0, d = 0;
            for (int j = 0; j < N; j++) if (j != i) {
                w += sum[i][j] + 0.5;                  /* a half-point prior keeps 100% finite */
                d += (games + 1.0) / (s[i] + s[j]);
            }
            ns[i] = w / d;
        }
        for (int i = 0; i < N; i++) s[i] = ns[i] / ns[0];
    }
    printf("%d games a pairing, %d entrants\n\n%-12s", games, N, "");
    for (int j = 0; j < N; j++) printf(" %6.6s", E[j].label);
    printf("   elo\n");
    for (int i = 0; i < N; i++) {
        printf("%-12s", E[i].label);
        for (int j = 0; j < N; j++)
            if (i == j) printf(" %6s", "-"); else printf(" %5.0f%%", 100.0 * sum[i][j] / games);
        printf(" %5.0f\n", 400 * log10(s[i]));
    }
    return 0;
}

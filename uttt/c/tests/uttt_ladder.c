/* The bot ladder: a round robin where every ENTRANT is a bot at its own
 * rollout budget, so one engine can fill two tiers (quill at 200 and 4000),
 * then Elo fitted to the whole table (Bradley-Terry, iterated).
 *
 *     ./uttt/c/build/uttt_ladder [games-per-pairing] [workers] [entrant...]
 *
 * An entrant on the command line is `engine@budget` or `engine@budget~noise`
 * (quill@4000, biro~60); any given replace the shipped seven, so a head to
 * head is `uttt_ladder 200 8 quill@4000 fountain@4000`. Each entrant's CPU
 * time a move is printed beside its Elo, because "equal budget" is a claim
 * about time and not about the number after the @.
 *
 * Seeds are (a, b, game) as in the arena, both colours share a seed, and the
 * run is cut into shards by game index across forked workers.
 */
#include "../src/uttt_bots.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* noise: percent of moves played uniformly at random instead - the only way
 * to get a tier between random and any bot that looks ahead at all. */
typedef struct { UtttBot bot; int budget; int noise; const char *label; } Entrant;
static const Entrant E0[] = {   /* the shipped-name ladder, docs/BOT_NAMES.md */
    { BOT_RANDOM, 1,    0,  "random"       },
    { BOT_BIRO,   1,    60, "biro~60"      },
    { BOT_BIRO,   1,    0,  "biro"         },
    { BOT_ROLLER, 25,   0,  "roller@25"    },
    { BOT_ROLLER, 200,  0,  "roller@200"   },
    { BOT_QUILL,  10,   0,  "quill@10"     },
    { BOT_QUILL,  4000, 0,  "quill@4000"   },
};
#define N0 ((int)(sizeof E0 / sizeof E0[0]))
#define NMAX 16
static Entrant E[NMAX];
static int N;
static double cpu[NMAX];      /* seconds thinking, per entrant, this worker */
static long   nmoves[NMAX];

static int parse(const char *spec, Entrant *e)
{
    char name[32]; int k = 0;
    while (spec[k] && spec[k] != '@' && spec[k] != '~' && k < 31) { name[k] = spec[k]; k++; }
    name[k] = 0;
    e->bot = BOT_COUNT;
    for (int b = 0; b < BOT_COUNT; b++) if (!strcmp(name, UTTT_BOT_NAME[b])) e->bot = (UtttBot)b;
    if (e->bot == BOT_COUNT) return 0;
    e->budget = 1; e->noise = 0; e->label = spec;
    const char *at = strchr(spec, '@'), *ti = strchr(spec, '~');
    if (at) e->budget = atoi(at + 1);
    if (ti) e->noise = atoi(ti + 1);
    return 1;
}

static double now_cpu(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t salt;   /* LADDER_SEED=n: a fresh stream, for confirming a tuned result */

static uint64_t seed_of(int a, int b, int g)
{
    return (salt * 0xD1B54A32D192ED03ull) ^ 0x9E3779B97F4A7C15ull ^ ((uint64_t)a << 40) ^ ((uint64_t)b << 24)
         ^ ((uint64_t)g * 1000003ull);
}

static uint8_t duel(const Entrant *x, const Entrant *o, uint64_t seed)
{
    UtttGame g; uttt_init(&g);
    uint64_t rs = seed | 1;
    uttt_bots_forget();            /* a game depends on its seed, not on the last one */
    for (;;) {
        uint8_t list[81];
        if (uttt_legal(&g, list) <= 0) break;
        const Entrant *m = g.turn == UTTT_X ? x : o;
        rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
        if (m->noise && (int)(rs % 100) < m->noise) {
            int n = uttt_legal(&g, list);
            rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
            uttt_play(&g, list[rs % (uint64_t)n]);
        } else {
            double t0 = now_cpu();
            uint8_t mv = uttt_bot_move(m->bot, &g, m->budget, &rs);
            cpu[m - E] += now_cpu() - t0; nmoves[m - E]++;
            uttt_play(&g, mv);
        }
    }
    return g.over;
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 40;
    int nw = argc > 2 ? atoi(argv[2]) : 8;
    if (nw < 1) nw = 1; if (nw > 64) nw = 64;
    if (getenv("LADDER_SEED")) salt = strtoull(getenv("LADDER_SEED"), NULL, 10);
    if (argc > 3) {
        for (int i = 3; i < argc && N < NMAX; i++)
            if (!parse(argv[i], &E[N++])) { fprintf(stderr, "no engine: %s\n", argv[i]); return 1; }
    } else {
        for (N = 0; N < N0; N++) E[N] = E0[N];
    }
    static double sum[NMAX][NMAX], tcpu[NMAX]; static long tmoves[NMAX];
    int pipes[64][2];
    for (int k = 0; k < nw; k++) {
        if (pipe(pipes[k]) != 0) { perror("pipe"); return 1; }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) { close(pipes[k][1]); continue; }
        close(pipes[k][0]);
        static double t[NMAX][NMAX];
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
        if (write(pipes[k][1], cpu, sizeof cpu) != (ssize_t)sizeof cpu) _exit(1);
        if (write(pipes[k][1], nmoves, sizeof nmoves) != (ssize_t)sizeof nmoves) _exit(1);
        _exit(0);
    }
    for (int k = 0; k < nw; k++) {
        static struct { double t[NMAX][NMAX]; double c[NMAX]; long m[NMAX]; } r;
        size_t off = 0; ssize_t n;
        while (off < sizeof r && (n = read(pipes[k][0], (char *)&r + off, sizeof r - off)) > 0) off += n;
        close(pipes[k][0]);
        if (off != sizeof r) { fprintf(stderr, "worker %d short\n", k); return 1; }
        for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) sum[a][b] += r.t[a][b];
        for (int a = 0; a < N; a++) { tcpu[a] += r.c[a]; tmoves[a] += r.m[a]; }
    }
    for (int a = 0; a < N; a++) for (int b = a + 1; b < N; b++) sum[b][a] = games - sum[a][b];

    /* Bradley-Terry by MM iteration, then Elo = 400 log10(strength), random = 0. */
    double s[NMAX]; for (int i = 0; i < N; i++) s[i] = 1;
    for (int it = 0; it < 5000; it++) {
        double ns[NMAX];
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
    for (int j = 0; j < N; j++) printf(" %6.6s", E[j].label + (strlen(E[j].label) > 6 ? strlen(E[j].label) - 6 : 0));
    printf("   elo  ms/move\n");
    for (int i = 0; i < N; i++) {
        printf("%-12s", E[i].label);
        for (int j = 0; j < N; j++)
            if (i == j) printf(" %6s", "-"); else printf(" %5.0f%%", 100.0 * sum[i][j] / games);
        printf(" %5.0f %8.2f\n", 400 * log10(s[i]),
               tmoves[i] ? 1000.0 * tcpu[i] / tmoves[i] : 0.0);
    }
    return 0;
}

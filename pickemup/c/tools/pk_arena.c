/* The arena: the bots of src/pk_bot.h against each other, at 2, 4 and 8
 * players, every move checked by the kernel, and the result printed as a
 * table you can argue with (uttt/c/tests/uttt_arena.c's shape).
 *
 *     make -C pickemup/c arena                    2,000 games a line-up, 8 workers
 *     ./pickemup/c/build/pk_arena 400 8           400 games, 8 workers
 *     ./pickemup/c/build/pk_arena 400 8 24        ...at 2 and 4 players only
 *     ./pickemup/c/build/pk_arena 400 8 248 x     ...and the extra line-ups
 *     ./pickemup/c/build/pk_arena 400 8 2 16      ...line-ups 1 and 6 only (the list below)
 *
 * A LINE-UP IS TWO AGENTS SEATED ALTERNATELY (A B A B ...), and every seed is
 * played twice, once each way round (B A B A ...), so seat 1's first move and
 * a lucky deal cannot favour either side. With half the seats each, an
 * agent's share of the wins is 50% when the two are equally good, at every
 * table size; the 95% interval is Wilson's over the games played (D66).
 * "cards" is the mean number of cards an agent's seats still hold at the end.
 *
 * Seeds are fixed: game i of line-up L at n players is deal (n, L, i / 2),
 * so a run is reproducible and can be cut into shards that pool exactly;
 * worker k plays the games whose index is k modulo the worker count (uttt's
 * reason: every worker gets the same slice of the cheap and the dear
 * line-ups). Processes, not threads, as uttt says. Seconds are CPU.
 *
 * A game the kernel refused a bot's move in is counted and printed; the
 * arena exits 1 if there is one, so `make arena` is also a legality check. */
#include "../src/pk_bot.h"
#include "../../../shared/c/stats/seed_hash.h"
#include "../../../shared/c/stats/stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct {
    const char *name;
    int         strategy;
    uint8_t     flags;
} Agent;

enum { A_RANDOM, A_GREEDY, A_MC, A_MC_RW, A_MC_NB, A_COUNT };
static const Agent AGENT[A_COUNT] = {
    { "random", PK_BOT_RANDOM, 0 },
    { "greedy", PK_BOT_GREEDY, 0 },
    { "mc",     PK_BOT_MC,     0 },
    { "mc-rw",  PK_BOT_MC,     PK_KNOB_RANDOM_WILD },   /* the wild's suit at random */
    { "mc-nb",  PK_BOT_MC,     PK_KNOB_NO_BELIEF },     /* worlds ignore every void */
};

typedef struct { int a, b; } Lineup;
static const Lineup LINEUP[] = {
    { A_MC, A_RANDOM },
    { A_MC, A_GREEDY },
    { A_GREEDY, A_RANDOM },
    { A_MC_RW, A_RANDOM },
    { A_MC, A_MC_RW },
    /* the extras (fourth argument x) */
    { A_MC_RW, A_GREEDY },
    { A_MC, A_MC_NB },
};
#define N_MAIN   5
#define N_LINEUP ((int)(sizeof LINEUP / sizeof LINEUP[0]))
static const int SIZES[3] = { 2, 4, 8 };

typedef struct {
    double wins[3][N_LINEUP][2];     /* games won by side 0 (A) and side 1 (B) */
    double cards[3][N_LINEUP][2];    /* cards left, summed per game (seat mean) */
    double games[3][N_LINEUP];
    double stuck[3][N_LINEUP], longs[3][N_LINEUP];
    double secs[3][N_LINEUP];
    double refused;
} Tally;

static void seed_of(uint8_t seed[32], int n, int l, int i)
{
    seed_hash32((uint64_t)n * 1000003ull + (uint64_t)l * 7919ull + (uint64_t)i * 104729ull, seed);
}

/* The knobs every MC seat plays with: the defaults, or a research override
 * from the environment (PK_W1, PK_W2, PK_W3, PK_DEPTH, PK_WILD_KEEP, PK_DRAW_KEEP,
 * PK_SOFT_MOD), as foolish's arena sweeps override OG_* (bot_knobs.h). */
static PkBotKnobs g_knobs;
static void knobs_from_env(void)
{
    pk_bot_knobs_default(&g_knobs);
    const char *v;
    if ((v = getenv("PK_W1")))        g_knobs.w1 = (uint16_t)atoi(v);
    if ((v = getenv("PK_W2")))        g_knobs.w2 = (uint16_t)atoi(v);
    if ((v = getenv("PK_W3")))        g_knobs.w3 = (uint16_t)atoi(v);
    if ((v = getenv("PK_DEPTH")))     g_knobs.depth = (uint16_t)atoi(v);
    if ((v = getenv("PK_WILD_KEEP"))) g_knobs.wild_keep = (uint16_t)atoi(v);
    if ((v = getenv("PK_DRAW_KEEP"))) g_knobs.draw_keep = (uint16_t)atoi(v);
    if ((v = getenv("PK_SOFT_MOD")))  g_knobs.soft_mod = (uint8_t)atoi(v);
}

/* One game. side[s] is 0 (agent A) or 1 (agent B). The winner's side, or -1
 * when a move was refused. */
static int play(int n, const int side[PK_MAX_SEATS], const Lineup *L, const uint8_t seed[32],
                uint64_t rs, PkGame *g, int *over)
{
    uint8_t strat[PK_MAX_SEATS];
    PkBotKnobs knobs[PK_MAX_SEATS];
    for (int s = 0; s < n; s++) {
        const Agent *a = &AGENT[side[s] ? L->b : L->a];
        strat[s] = (uint8_t)a->strategy;
        knobs[s] = g_knobs;
        knobs[s].flags = a->flags;
        knobs[s].seed ^= (uint64_t)s * 0x100000001b3ull;
    }
    if (!pk_new(g, seed, n)) return -1;
    int r;
    while ((r = pk_bot_round(g, strat, knobs, &rs)) > 0) { }
    if (r < 0) return -1;
    *over = g->over;
    return side[g->winner];
}

static void run_worker(int games, int k, int nw, int nl, int lmask, int size_mask, Tally *t)
{
    static PkGame g;
    for (int si = 0; si < 3; si++) {
        if (!(size_mask >> si & 1)) continue;
        int n = SIZES[si];
        for (int l = 0; l < nl; l++)
            for (int i = 0; i < games; i++) {
                if (i % nw != k || !(lmask >> l & 1)) continue;
                int side[PK_MAX_SEATS];
                for (int s = 0; s < n; s++) side[s] = (s + i) & 1;
                uint8_t seed[32];
                seed_of(seed, n, l, i / 2);
                clock_t t0 = clock();
                int over = 0;
                int w = play(n, side, &LINEUP[l], seed, 0x2545f4914f6cdd1dull ^ (uint64_t)(i / 2), &g, &over);
                t->secs[si][l] += (double)(clock() - t0) / CLOCKS_PER_SEC;
                if (w < 0) { t->refused++; continue; }
                t->games[si][l]++;
                t->wins[si][l][w]++;
                if (over == PK_OVER_STUCK) t->stuck[si][l]++;
                if (over == PK_OVER_LONG) t->longs[si][l]++;
                double c[2] = { 0, 0 };
                for (int s = 0; s < n; s++) c[side[s]] += g.hand_n[s];
                t->cards[si][l][0] += c[0] / (n / 2);
                t->cards[si][l][1] += c[1] / (n / 2);
            }
    }
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 2000;
    int jobs = argc > 2 ? atoi(argv[2]) : 8;
    int size_mask = 7;
    if (argc > 3) {
        size_mask = 0;
        for (const char *p = argv[3]; *p; p++)
            for (int si = 0; si < 3; si++)
                if (*p - '0' == SIZES[si]) size_mask |= 1 << si;
    }
    int nl = N_LINEUP, lmask = (1 << N_MAIN) - 1;
    if (argc > 4) {
        lmask = 0;
        for (const char *p = argv[4]; *p; p++)
            if (*p == 'x') lmask = (1 << N_LINEUP) - 1;
            else if (*p >= '0' && *p - '0' < N_LINEUP) lmask |= 1 << (*p - '0');
    }
    if (games < 2) games = 2;
    if (jobs < 1) jobs = 1;
    if (jobs > 64) jobs = 64;

    knobs_from_env();
    PkBotKnobs d = g_knobs;
    printf("pick 'em up arena: %d games a line-up (each seed both ways round), %d worker%s\n",
           games, jobs, jobs == 1 ? "" : "s");
    printf("mc knobs: worlds %d/%d/%d, depth %d (0 = to the end), wild_keep %d, draw_keep %d, "
           "soft voids in %d of %d worlds\n\n", d.w1, d.w2, d.w3, d.depth, d.wild_keep, d.draw_keep,
           d.soft_mod ? d.soft_mod - 1 : 0, d.soft_mod);

    static Tally all;
    memset(&all, 0, sizeof all);
    int pipes[64][2];
    for (int k = 0; k < jobs; k++) {
        if (jobs > 1) {
            if (pipe(pipes[k]) != 0) { perror("pipe"); return 1; }
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); return 1; }
            if (pid > 0) { close(pipes[k][1]); continue; }
            close(pipes[k][0]);
        }
        static Tally t;
        memset(&t, 0, sizeof t);
        run_worker(games, k, jobs, nl, lmask, size_mask, &t);
        if (jobs == 1) { all = t; break; }
        ssize_t off = 0, w;
        while (off < (ssize_t)sizeof t && (w = write(pipes[k][1], (char *)&t + off, sizeof t - (size_t)off)) > 0)
            off += w;
        close(pipes[k][1]);
        _exit(0);
    }
    if (jobs > 1) {
        for (int k = 0; k < jobs; k++) {
            static Tally t;
            ssize_t off = 0, r;
            while (off < (ssize_t)sizeof t && (r = read(pipes[k][0], (char *)&t + off, sizeof t - (size_t)off)) > 0)
                off += r;
            close(pipes[k][0]);
            if (off != (ssize_t)sizeof t) { fprintf(stderr, "worker %d returned %zd bytes\n", k, off); return 1; }
            double *a = (double *)&all, *b = (double *)&t;
            for (size_t i = 0; i < sizeof t / sizeof(double); i++) a[i] += b[i];
        }
        while (wait(NULL) > 0) { }
    }

    for (int si = 0; si < 3; si++) {
        if (!(size_mask >> si & 1)) continue;
        printf("%d players\n", SIZES[si]);
        printf("  %-18s %6s  %-22s %6s  %-22s %6s %6s %6s %8s\n", "line-up (A vs B)", "games",
               "A wins [95% CI]", "cards", "B wins [95% CI]", "cards", "stuck", "long", "cpu s");
        for (int l = 0; l < nl; l++) {
            if (!(lmask >> l & 1)) continue;
            double n = all.games[si][l], pa = n > 0 ? all.wins[si][l][0] / n : 0, pb = n > 0 ? all.wins[si][l][1] / n : 0;
            double loa, hia, lob, hib;
            stat_wilson(all.wins[si][l][0], n, STAT_Z95, &loa, &hia);
            stat_wilson(all.wins[si][l][1], n, STAT_Z95, &lob, &hib);
            char name[40];
            snprintf(name, sizeof name, "%s vs %s", AGENT[LINEUP[l].a].name, AGENT[LINEUP[l].b].name);
            printf("  %-18s %6.0f  %5.1f%% [%5.1f, %5.1f] %s %6.2f  %5.1f%% [%5.1f, %5.1f]   %6.2f %6.0f %6.0f %8.1f\n",
                   name, n, 100 * pa, 100 * loa, 100 * hia,
                   (loa > 0.5 || hia < 0.5) ? "*" : " ",
                   n > 0 ? all.cards[si][l][0] / n : 0, 100 * pb, 100 * lob, 100 * hib,
                   n > 0 ? all.cards[si][l][1] / n : 0, all.stuck[si][l], all.longs[si][l], all.secs[si][l]);
        }
        /* head to head: row's share of the wins against column */
        printf("\n  head to head (row's win share against column)\n  %-8s", "");
        for (int b = 0; b < A_COUNT; b++) printf("%9s", AGENT[b].name);
        printf("\n");
        for (int a = 0; a < A_COUNT; a++) {
            int any = 0;
            for (int l = 0; l < nl; l++)
                any |= (lmask >> l & 1) && (LINEUP[l].a == a || LINEUP[l].b == a);
            if (!any) continue;
            printf("  %-8s", AGENT[a].name);
            for (int b = 0; b < A_COUNT; b++) {
                int found = 0;
                for (int l = 0; l < nl && !found; l++) {
                    double n = all.games[si][l];
                    if (n <= 0) continue;
                    if (LINEUP[l].a == a && LINEUP[l].b == b) { printf("%8.1f%%", 100 * (all.wins[si][l][0] / n)); found = 1; }
                    else if (LINEUP[l].b == a && LINEUP[l].a == b) { printf("%8.1f%%", 100 * (all.wins[si][l][1] / n)); found = 1; }
                }
                if (!found) printf("%9s", "-");
            }
            printf("\n");
        }
        printf("\n");
    }
    printf("* : A's interval is clear of 50%%\n");
    if (all.refused > 0) {
        printf("REFUSED: %.0f games had a bot move the kernel refused\n", all.refused);
        return 1;
    }
    printf("every move of every game was legal (the kernel applied each one)\n");
    return 0;
}

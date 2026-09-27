/* Chui Niu - the arena: bots play whole games against each other, offline.
 *
 *   build/cn_arena --seats=4 --mix=bot,random,random,random --games=4000
 *
 * --mix names one policy per seat. Game i shifts the mix by i seats, so every
 * policy sits in every position equally often (position matters: seat 0
 * opens round 1). Every game's seed and every decision's seed are functions
 * of --seed and the game index, so a run is identical at any --threads.
 *
 * Policies:
 *   bot      the belief bot (cn_bot.c), default constants
 *   random   uniform over the legal options (cn_bot_random.c): THE baseline
 * and the bot's own ablations, each one switch away from `bot` (BOT.md):
 *   prior    no opponent model: every hidden hand from the prior, and no bid
 *            ever updates anything
 *   noread   the decision-time belief, but rollout bids update nothing
 *   readers  rollout opponents read my bids too, from the public belief
 *   fitbeta  each seat's temperature fitted from its revealed hands
 *
 * Prints, per policy: seat-games, win rate with its 95% Wilson interval, and
 * mean dice lost per game with a 95% normal interval. --fast is a smoke run
 * (few games, few worlds). */
#include "cn_bot.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { P_BOT, P_RANDOM, P_PRIOR, P_NOREAD, P_READERS, P_FITBETA, P_N };
static const char *const P_NAME[P_N] = { "bot", "random", "prior", "noread", "readers", "fitbeta" };

static int      A_seats = 2, A_games = 1000, A_threads = 8, A_worlds = -1;
static uint64_t A_seed = 1;
static int      A_mix[CN_MAX_SEATS];
static CnBotCfg A_cfg[P_N];

typedef struct { uint8_t pol[CN_MAX_SEATS], lost[CN_MAX_SEATS], winner; uint16_t moves; } Result;
static Result     *A_res;
static atomic_int  A_next;

static int policy_of(const char *s)
{
    for (int p = 0; p < P_N; p++) if (!strcmp(s, P_NAME[p])) return p;
    return -1;
}

static void play(int gi, CnSeen *seen)
{
    uint64_t x = A_seed * 0x100000001B3ull + (uint64_t)gi;
    uint8_t seed[32];
    for (int i = 0; i < 4; i++) {
        uint64_t v = cn_splitmix(&x);
        memcpy(seed + 8 * i, &v, 8);
    }
    static _Thread_local CnGame g;
    cn_new(&g, seed, A_seats);
    Result *r = &A_res[gi];
    for (int s = 0; s < A_seats; s++) r->pol[s] = (uint8_t)A_mix[(s + gi) % A_seats];
    uint64_t rrng = cn_splitmix(&x);
    while (g.phase != CN_PH_OVER) {
        int seat = g.turn;
        int pol = r->pol[seat];
        CnMove m;
        if (pol == P_RANDOM) m = cn_random_choose(&g, &rrng);
        else {
            if (!cn_seen(&g, seat, seen)) { fprintf(stderr, "game %d: seen failed\n", gi); exit(2); }
            uint64_t ds = x ^ ((uint64_t)g.hist_n << 20) ^ (uint64_t)seat;
            m = cn_bot_choose(seen, &A_cfg[pol], cn_splitmix(&ds));
        }
        if (!cn_apply(&g, seat, m)) {
            fprintf(stderr, "game %d: %s at seat %d played an illegal move (%d, %d)\n", gi, P_NAME[pol], seat, m.q, m.f);
            exit(2);
        }
    }
    for (int s = 0; s < A_seats; s++) r->lost[s] = (uint8_t)(CN_START_DICE - g.dice_n[s]);
    r->winner = g.winner;
    r->moves = g.hist_n;
}

static void *worker(void *arg)
{
    (void)arg;
    CnSeen *seen = malloc(sizeof *seen);
    for (;;) {
        int gi = atomic_fetch_add(&A_next, 1);
        if (gi >= A_games) break;
        play(gi, seen);
    }
    free(seen);
    return 0;
}

static void wilson(double k, double n, double *lo, double *hi)
{
    const double z = 1.959964;
    double p = k / n, d = 1 + z * z / n;
    double c = (p + z * z / (2 * n)) / d, h = z * sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d;
    *lo = c - h;
    *hi = c + h;
}

int main(int argc, char **argv)
{
    const char *mix = 0;
    int fast = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--seats=", 8)) A_seats = atoi(a + 8);
        else if (!strncmp(a, "--games=", 8)) A_games = atoi(a + 8);
        else if (!strncmp(a, "--threads=", 10)) A_threads = atoi(a + 10);
        else if (!strncmp(a, "--worlds=", 9)) A_worlds = atoi(a + 9);
        else if (!strncmp(a, "--seed=", 7)) A_seed = strtoull(a + 7, 0, 10);
        else if (!strncmp(a, "--mix=", 6)) mix = a + 6;
        else if (!strcmp(a, "--fast")) fast = 1;
        else { fprintf(stderr, "unknown argument %s\n", a); return 2; }
    }
    if (A_seats < CN_MIN_SEATS || A_seats > CN_MAX_SEATS) { fprintf(stderr, "--seats is 2..6\n"); return 2; }
    if (fast) { A_games = A_games > 200 ? 200 : A_games; if (A_worlds < 0) A_worlds = 24; }
    if (A_threads < 1) A_threads = 1;
    /* the mix: "bot,random" cycles to fill every seat */
    char buf[256];
    snprintf(buf, sizeof buf, "%s", mix ? mix : "bot,random");
    int np = 0, pols[CN_MAX_SEATS];
    for (char *t = strtok(buf, ","); t && np < CN_MAX_SEATS; t = strtok(0, ",")) {
        int p = policy_of(t);
        if (p < 0) { fprintf(stderr, "unknown policy %s\n", t); return 2; }
        pols[np++] = p;
    }
    for (int s = 0; s < A_seats; s++) A_mix[s] = pols[s % np];

    for (int p = 0; p < P_N; p++) {
        cn_bot_cfg_default(&A_cfg[p]);
        if (A_worlds > 0) A_cfg[p].worlds = A_worlds;
    }
    A_cfg[P_PRIOR].use_belief = 0;
    A_cfg[P_PRIOR].observe = 0;
    A_cfg[P_NOREAD].observe = 0;
    A_cfg[P_READERS].observe = 1;
    A_cfg[P_READERS].opp_reads = 1;
    A_cfg[P_FITBETA].fit_beta = 1;

    A_res = calloc((size_t)A_games, sizeof *A_res);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t th[64];
    int nt = A_threads > 64 ? 64 : A_threads;
    for (int i = 0; i < nt; i++) pthread_create(&th[i], 0, worker, 0);
    for (int i = 0; i < nt; i++) pthread_join(th[i], 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;

    double seats[P_N] = { 0 }, wins[P_N] = { 0 }, lost[P_N] = { 0 }, lost2[P_N] = { 0 }, moves = 0;
    for (int gi = 0; gi < A_games; gi++) {
        const Result *r = &A_res[gi];
        moves += r->moves;
        for (int s = 0; s < A_seats; s++) {
            int p = r->pol[s];
            seats[p] += 1;
            wins[p] += r->winner == s;
            lost[p] += r->lost[s];
            lost2[p] += (double)r->lost[s] * r->lost[s];
        }
    }
    printf("seats %d  games %d  seed %llu  worlds %d  mix", A_seats, A_games, (unsigned long long)A_seed,
           A_cfg[P_BOT].worlds);
    for (int s = 0; s < A_seats; s++) printf("%c%s", s ? ',' : ' ', P_NAME[A_mix[s]]);
    printf("  (%.1fs, %.1f moves/game)\n", secs, moves / A_games);
    printf("fair share of wins: %.3f\n", 1.0 / A_seats);
    printf("%-8s %8s %8s %7s  %-17s %9s  %s\n", "policy", "seats", "wins", "win", "win 95% CI", "diceLost", "95% CI");
    for (int p = 0; p < P_N; p++) {
        if (!seats[p]) continue;
        double lo, hi;
        wilson(wins[p], seats[p], &lo, &hi);
        double mu = lost[p] / seats[p], var = lost2[p] / seats[p] - mu * mu;
        double se = sqrt(var > 0 ? var / seats[p] : 0);
        printf("%-8s %8.0f %8.0f %7.3f  [%.3f, %.3f]    %9.3f  [%.3f, %.3f]\n", P_NAME[p], seats[p], wins[p], wins[p] / seats[p],
               lo, hi, mu, mu - 1.96 * se, mu + 1.96 * se);
    }
    free(A_res);
    return 0;
}

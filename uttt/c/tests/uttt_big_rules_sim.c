/* THE SEND RULES SIDE BY SIDE (docs/BIG_BOARD_SEND_RULE.md): whole depth-5
 * games under one rule, by uniform random play or by two greedy bots at the
 * 243 page's defaults, and what they decided.
 *
 *     make -C uttt/c big-rules-sim ARGS="<rule 0|1|2> <random games> <bot games> [seed]"
 *
 * One line a game and a summary: plies, the result, the 3 x 3s, 9 x 9s,
 * 27 x 27s and 81 x 81s decided (won and drawn), and how local play was: the
 * share of plies whose 81 x 81 is not the previous ply's. A measurement, not
 * a test: nothing here can fail.
 */
#include "../src/uttt_big_bot.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int32_t LIST[UTB_LEAVES_MAX];

typedef struct { long plies, decided[5], won[5], jumps; int x, o, d, n; double s; } Sum;

static void one(Sum *t, int rule, int bots, uint64_t seed, int n)
{
    static const char *const NAME[3] = { "A", "B'", "B" };
    UtbGame g;
    utb_init_rule(&g, 5, rule);
    UtbBot b[2];
    utb_bot_seat(&b[0], seed, UTTT_X);
    utb_bot_seat(&b[1], seed, UTTT_O);
    uint64_t rs = seed * 0x9e3779b97f4a7c15ull + 1;
    long jumps = 0;
    clock_t t0 = clock();
    while (!g.over) {
        int mv;
        if (bots) {
            mv = utb_bot_move(&b[g.turn - 1], &g);
        } else {
            int m = utb_legal(&g, LIST, UTB_LEAVES_MAX);
            rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
            mv = LIST[rs % (uint64_t)m];
        }
        if (g.last >= 0 && mv / 6561 != g.last / 6561) jumps++;
        if (!utb_play(&g, mv)) { printf("illegal move %d\n", mv); exit(1); }
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    long dec[5] = { 0 }, won[5] = { 0 };
    for (int id = 0; id < utb_nodes(5); id++) {
        int L = utb_node_level(&g, id), st = g.node[id];
        if (st != UTTT_OPEN) { dec[L]++; won[L] += st != UTTT_DRAW; }
    }
    char r = g.over == UTTT_X ? 'X' : g.over == UTTT_O ? 'O' : 'D';
    printf("  rule %-2s %s game %d: %6d plies, %c; decided 3x3 %4ld (won %4ld), 9x9 %3ld (%3ld), 27x27 %2ld (%2ld), "
           "81x81 %ld (%ld); 81x81 changed on %4.1f%% of plies; %.1f s\n",
           NAME[rule], bots ? "bots  " : "random", n, g.n_plies, r, dec[4], won[4], dec[3], won[3], dec[2], won[2],
           dec[1], won[1], 100.0 * (double)jumps / g.n_plies, s);
    fflush(stdout);
    t->plies += g.n_plies;
    for (int L = 0; L < 5; L++) { t->decided[L] += dec[L]; t->won[L] += won[L]; }
    t->jumps += jumps;
    t->x += r == 'X'; t->o += r == 'O'; t->d += r == 'D';
    t->n++;
    t->s += s;
}

static void summary(const Sum *t, int rule, const char *who)
{
    static const char *const NAME[3] = { "A", "B'", "B" };
    if (!t->n) return;
    const double n = t->n;
    printf("rule %-2s %s, %d games: %.0f plies a game; X %d, O %d, drawn %d; decided a game: 3x3 %.0f (won %.0f), "
           "9x9 %.1f (%.1f), 27x27 %.1f (%.1f), 81x81 %.1f (%.1f); 81x81 changed on %.1f%% of plies; %.1f s a game\n",
           NAME[rule], who, t->n, t->plies / n, t->x, t->o, t->d, t->decided[4] / n, t->won[4] / n,
           t->decided[3] / n, t->won[3] / n, t->decided[2] / n, t->won[2] / n, t->decided[1] / n, t->won[1] / n,
           100.0 * (double)t->jumps / (double)t->plies, t->s / n);
}

int main(int argc, char **argv)
{
    if (argc < 4) { printf("usage: %s <rule 0|1|2> <random games> <bot games> [seed]\n", argv[0]); return 2; }
    const int rule = atoi(argv[1]), nr = atoi(argv[2]), nb = atoi(argv[3]);
    const uint64_t seed = argc > 4 ? strtoull(argv[4], NULL, 10) : 20261003;
    if (rule < UTB_RULE_SHIFT || rule > UTB_RULE_CLIMB_FREE) return 2;
    Sum r = { 0 }, b = { 0 };
    for (int i = 0; i < nr; i++) one(&r, rule, 0, seed + (uint64_t)i, i);
    for (int i = 0; i < nb; i++) one(&b, rule, 1, seed + 1000 + (uint64_t)i, i);
    summary(&r, rule, "random");
    summary(&b, rule, "bots at the page's defaults (6 plies, budget 150,000)");
    return 0;
}

// match - two bots play whole games on the rules engine (gd_rules, the
// repetition rule included), the seats swapped every game, the lowest seat
// with a move acting first (foolish's playout order). Prints each bot's wins,
// and for Seoul the candidates and playouts a decision took, which with the
// Game Boy's time per playout (make simbench) gives its time per decision.
//
// usage: match BOT_A BOT_B [games] [first_seed]   (bots: random, newyork, seoul)
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"
#include "../src/gd_bot_hw.h"
#include "../src/gd_bot_seoul.h"
#include "../src/gd_bot_moscow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BOT_RANDOM, BOT_NEWYORK, BOT_SEOUL, BOT_MOSCOW };
// Moscow: its decisions, and their time on the Game Boy as modeled from the
// costs measured there (README): 20 ms a world sampled and converted, 3 ms to
// play a candidate's move on a world, 3.3 ms a ply, 8.2 ms a solver node
static unsigned long mo_decisions, mo_hard, mo_cands, mo_playouts, mo_max_playouts, mo_plies, mo_solves[4];
static unsigned long long mo_nodes;
static unsigned long mo_max_nodes;
static double mo_ms, mo_ms_max, mo_ms_solve, mo_ms_endgame_mc;
static unsigned long seoul_plies;
static unsigned long seoul_decisions, seoul_cands, seoul_playouts, seoul_max_playouts, seoul_hard;

static int bot_of(const char *s) {
    if (!strcmp(s, "random")) return BOT_RANDOM;
    if (!strcmp(s, "newyork")) return BOT_NEWYORK;
    if (!strcmp(s, "seoul")) return BOT_SEOUL;
    if (!strcmp(s, "moscow")) return BOT_MOSCOW;
    fprintf(stderr, "unknown bot %s\n", s);
    exit(2);
}

static void choose(int bot, const gd_game *g, uint8_t seat, gd_move *m) {
    if (bot == BOT_NEWYORK) { gd_hw_choose(g, seat, m); return; }
    if (bot == BOT_SEOUL) {
        gd_seoul_choose(g, seat, m);
        seoul_decisions++;
        seoul_cands += gd_seoul_cands;
        seoul_playouts += gd_seoul_playouts;
        seoul_plies += gd_seoul_plies;
        if (gd_seoul_playouts > seoul_max_playouts) seoul_max_playouts = gd_seoul_playouts;
        if (gd_seoul_cands > 1) seoul_hard++;
        return;
    }
    if (bot == BOT_MOSCOW) {
        gd_moscow_choose(g, seat, m);
        mo_decisions++;
        mo_cands += gd_moscow_cands;
        mo_playouts += gd_moscow_playouts;
        mo_plies += gd_moscow_plies;
        mo_nodes += gd_moscow_nodes;
        mo_solves[gd_moscow_solve]++;
        if (gd_moscow_nodes > mo_max_nodes) mo_max_nodes = gd_moscow_nodes;
        if (gd_moscow_playouts > mo_max_playouts) mo_max_playouts = gd_moscow_playouts;
        if (gd_moscow_cands > 1) mo_hard++;
        const double worlds = gd_moscow_playouts ? (double)(MOSCOW_W1 + MOSCOW_W2) : 0.0;
        const double ms = worlds * 20.0 + gd_moscow_playouts * 3.0 + gd_moscow_plies * 3.3 + gd_moscow_nodes * 8.2;
        mo_ms += ms;
        mo_ms_solve += gd_moscow_nodes * 8.2;
        if (g->stock_n == 0 && !g->has_flipped) mo_ms_endgame_mc += ms - gd_moscow_nodes * 8.2;
        if (ms > mo_ms_max) mo_ms_max = ms;
        return;
    }
    const uint32_t n = gd_menu_count(g, seat);
    m->type = 0xFF;
    if (n) gd_menu_move(g, seat, gd_rng_below(n), m);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: match BOT_A BOT_B [games] [first_seed]\n"); return 2; }
    const int a = bot_of(argv[1]), b = bot_of(argv[2]);
    const unsigned long games = argc > 3 ? strtoul(argv[3], 0, 10) : 1000;
    const unsigned long first = argc > 4 ? strtoul(argv[4], 0, 10) : 1;
    unsigned long wa = 0, wb = 0, draws = 0, cut = 0;
    for (unsigned long gi = 0; gi < games; gi++) {
        gd_game g;
        gd_rep r;
        uint8_t deck[36];
        // A plays seat 0 in even games, seat 1 in odd ones
        const int bots[2] = { gi & 1 ? b : a, gi & 1 ? a : b };
        gd_rng_seed((uint16_t)(first + gi / 2));    // each deal played both ways round
        for (int i = 0; i < 36; i++) deck[i] = (uint8_t)i;
        for (int i = 35; i > 0; i--) { uint8_t j = (uint8_t)gd_rng_below((uint32_t)i + 1u), t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
        gd_deal(&g, &r, deck, (uint8_t)gd_rng_below(2));
        int moves = 0;
        while (g.status == GD_PLAYING && moves < 3000) {
            int acted = 0;
            for (uint8_t seat = 0; seat < 2 && !acted; seat++) {
                if (!gd_menu_kind(&g, seat) || (gd_menu_kind(&g, seat) == GD_KIND_DEFENDER && !gd_uncovered(&g))) continue;
                gd_move m;
                choose(bots[seat], &g, seat, &m);
                if (m.type == 0xFF) continue;
                if (!gd_apply(&g, &r, seat, &m)) { fprintf(stderr, "illegal move from bot %d\n", bots[seat]); return 1; }
                acted = 1;
            }
            if (!acted) break;
            moves++;
        }
        const uint8_t res = gd_result(&g);
        if (res == GD_RESULT_NONE) cut++;
        else if (res == GD_RESULT_DRAW) draws++;
        else {
            const uint8_t winner = res == GD_RESULT_FOOL0 ? 1 : 0;
            if ((winner == 0) == !(gi & 1)) wa++; else wb++;
        }
    }
    printf("%s vs %s, %lu games: %s wins %lu (%.1f%%), %s wins %lu (%.1f%%), draws %lu, unfinished %lu\n",
           argv[1], argv[2], games, argv[1], wa, 100.0 * wa / games, argv[2], wb, 100.0 * wb / games, draws, cut);
    if (seoul_decisions)
        printf("  Seoul: %lu decisions, %.1f%% with a choice, %.2f candidates and %.1f playouts a decision on average, %lu playouts at most\n",
               seoul_decisions, 100.0 * seoul_hard / seoul_decisions, (double)seoul_cands / seoul_decisions,
               (double)seoul_playouts / seoul_decisions, seoul_max_playouts);
    if (seoul_playouts)
        printf("  Seoul's playouts: %.1f plies each on average\n", (double)seoul_plies / seoul_playouts);
    if (mo_decisions) {
        printf("  Moscow: %lu decisions, %.1f%% with a choice, %.2f candidates and %.1f playouts a decision (%lu at most), %.1f plies a playout\n",
               mo_decisions, 100.0 * mo_hard / mo_decisions, (double)mo_cands / mo_decisions, (double)mo_playouts / mo_decisions,
               mo_max_playouts, mo_playouts ? (double)mo_plies / mo_playouts : 0.0);
        const unsigned long solved = mo_solves[1] + mo_solves[2] + mo_solves[3];
        printf("  Moscow's endgame solves: %lu (a proven win %lu, all moves lose %lu, out of nodes %lu), %.0f nodes a solve, %lu at most\n",
               solved, mo_solves[1], mo_solves[2], mo_solves[3], solved ? (double)mo_nodes / solved : 0.0, mo_max_nodes);
        printf("  Moscow modeled on the Game Boy: %.2f s a decision on average, %.1f s at most (of the average: solver %.2f s, Monte Carlo with the stock gone %.2f s)\n",
               mo_ms / mo_decisions / 1000.0, mo_ms_max / 1000.0, mo_ms_solve / mo_decisions / 1000.0, mo_ms_endgame_mc / mo_decisions / 1000.0);
    }
    return 0;
}

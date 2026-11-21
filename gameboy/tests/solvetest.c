// solvetest - the endgame solver (src/gd_sim.c gd_solve) against foolish's
// (../foolish/c/src/cordite_sim.c cd_sim_solve, read-only, the build of
// simtest). For each seed: a deal, then moves on both rule engines (uniformly
// random from the full menu, or New York's, half each, so the endgames are
// both odd and like real play) until the stock and the face-up trump are
// gone; then at every ply until the game ends, the board as both solvers'
// states, solved from each seat's side:
//   - the full window: when both finish within their budgets, the values must
//     be identical (win or loss and the distance to the end);
//   - the windows Moscow searches with, (0, 2000) and (-1, 0): the result must
//     fall on the same side of the window, and be identical inside it.
// Both tables are emptied before each solve, so each is a fresh search.
//
// usage: solvetest [games] [first_seed] [budget]
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"
#include "../src/gd_sim.h"
#include "../src/gd_bot_hw.h"
#include "game.h"
#include "legal.h"
#include "cordite_sim.h"
#include "foolish_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double dt_sim_random(void) { return (double)(uint16_t)gd_rng_below(65536ul) / 65536.0; }

static unsigned long g_seed, g_ply;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "MISMATCH seed %lu ply %lu: ", g_seed, g_ply); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 0; } } while (0)

static unsigned long st_pos, st_solves, st_both, st_only_gd, st_only_f, st_neither;
static unsigned long st_win, st_loss, st_draw, st_win_cmp;
static unsigned long long st_nodes_gd;
static long budget_cap = 200000;

// a window's verdict: below (<= alpha), above (>= beta), or the exact value
static int side(int v, int a, int b) { return v <= a ? -1 : v >= b ? 1 : 0; }

static int solve_both(const gd_game *g, const Game *f, int me) {
    gd_sim s;
    SimState fs;
    gd_sim_from_game(&s, g);
    cd_sim_from_game(&fs, f);
    static const int win[3][2] = { { -2000, 2000 }, { 0, 2000 }, { -1, 0 } };
    for (int w = 0; w < 3; w++) {
        const int a = win[w][0], b = win[w][1];
        uint32_t bud = (uint32_t)budget_cap;
        uint8_t ab_g = 0;
        int ab_f = 0;
        gd_solve_reset();
        const int vg = gd_solve(&s, (uint8_t)me, (int16_t)a, (int16_t)b, &bud, 0, &ab_g);
        st_nodes_gd += (unsigned long long)((uint32_t)budget_cap - bud);
        cd_sim_solve_reset();
        const int vf = cd_sim_solve(&fs, me, a, b, budget_cap, &ab_f);
        st_solves++;
        if (w == 0) {
            if (!ab_g && !ab_f) st_both++;
            else if (!ab_g) st_only_gd++;
            else if (!ab_f) st_only_f++;
            else st_neither++;
        }
        if (ab_g || ab_f) continue;
        if (w == 0) {
            CHECK(vg == vf, "seat %d full window: %d, foolish %d", me, vg, vf);
            if (vg > 0) st_win++; else if (vg < 0) st_loss++; else st_draw++;
        } else {
            const int sg = side(vg, a, b), sf = side(vf, a, b);
            CHECK(sg == sf, "seat %d window (%d, %d): %d, foolish %d", me, a, b, vg, vf);
            if (sg == 0) CHECK(vg == vf, "seat %d window (%d, %d) exact: %d, foolish %d", me, a, b, vg, vf);
            st_win_cmp++;
        }
    }
    return 1;
}

static int run_seed(unsigned long seed, Game *f) {
    gd_game g;
    uint8_t deck[36];
    Card fdeck[36];
    g_seed = seed; g_ply = 0;
    gd_rng_seed((uint16_t)seed);
    for (int i = 0; i < 36; i++) deck[i] = (uint8_t)i;
    for (int i = 35; i > 0; i--) { uint8_t j = (uint8_t)gd_rng_below((uint32_t)i + 1u), t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    uint8_t fallback = (uint8_t)gd_rng_below(2);
    gd_deal(&g, 0, deck, fallback);
    memset(f, 0, sizeof *f);
    f->num_players = 2;
    for (int i = 0; i < 36; i++) fdeck[i] = f_of(deck[i]);
    game_force_first_attacker(fallback);
    start_game_with_deck(f, fdeck, 36);
    game_force_first_attacker(-1);
    for (g_ply = 0; g_ply < 3000 && g.status == GD_PLAYING; g_ply++) {
        if (g.stock_n == 0 && !g.has_flipped) {
            st_pos++;
            for (int me = 0; me < 2; me++) if (!solve_both(&g, f, me)) return 0;
        }
        const uint32_t n0 = gd_menu_count(&g, 0), n1 = gd_menu_count(&g, 1);
        if (!n0 && !n1) break;
        gd_move m;
        uint8_t seat;
        const uint32_t pick = gd_rng_below(n0 + n1);
        seat = pick < n0 ? 0 : 1;
        if (gd_rng_below(2)) {
            // New York's move for a seat that has one
            gd_hw_choose(&g, seat, &m);
            if (m.type == 0xFF || !gd_legal(&g, seat, &m)) gd_menu_move(&g, seat, seat ? pick - n0 : pick, &m);
        } else {
            gd_menu_move(&g, seat, seat ? pick - n0 : pick, &m);
        }
        CHECK(f_apply(f, seat, &m), "foolish refused a move");
        game_settle_status(f);
        CHECK(gd_apply(&g, 0, seat, &m), "gd refused a move");
    }
    return 1;
}

int main(int argc, char **argv) {
    unsigned long games = argc > 1 ? strtoul(argv[1], 0, 10) : 300;
    unsigned long first = argc > 2 ? strtoul(argv[2], 0, 10) : 1;
    if (argc > 3) budget_cap = strtol(argv[3], 0, 10);
    Game *f = calloc(1, sizeof(Game));
    if (!f) return 2;
    for (unsigned long sd = first; sd < first + games; sd++)
        if (!run_seed(sd, f)) { printf("FAIL\n"); return 1; }
    printf("solvetest: %lu positions with the stock gone, %lu solves (budget %ld each)\n", st_pos, st_solves, budget_cap);
    printf("  full window, both finished %lu (wins %lu, losses %lu, draws %lu: identical), only this one %lu, only foolish's %lu, neither %lu\n",
           st_both, st_win, st_loss, st_draw, st_only_gd, st_only_f, st_neither);
    printf("  Moscow's windows (0, 2000) and (-1, 0), both finished: %lu, same verdict\n", st_win_cmp);
    printf("  this solver's nodes: %.1f a solve\nPASS\n", (double)st_nodes_gd / (double)st_solves);
    return 0;
}

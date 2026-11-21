// solvetest - the endgame solver (src/gd_solve.c) against foolish's
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
#include "../src/gd_solve.h"
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

static int solve_pair(const gd_sim *s0, const SimState *fs0, int me) {
    gd_sim s = *s0;
    SimState fs = *fs0;
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

static int solve_both(const gd_game *g, const Game *f, int me) {
    gd_sim s;
    SimState fs;
    gd_sim_from_game(&s, g);
    cd_sim_from_game(&fs, f);
    return solve_pair(&s, &fs, me);
}

// A random small endgame, built field by field for both solvers: 1 to 6
// cards a hand, up to 3 attacks of at most 2 ranks (never more uncovered than
// the defender holds), each covered or not, and sometimes the attacker has
// said done. Games rarely reach some lines (a transfer of several cards that
// empties the hand, say), and these reach them all the time.
static uint64_t vmask(int value) {
    uint64_t m = 0;
    for (int su = 0; su < 4; su++) m |= 1ull << (su * 13 + value - 1);
    return m;
}
static unsigned long st_random;
static int random_endgame(void) {
    uint8_t deck[36], k = 0;
    gd_sim s;
    SimState fs;
    memset(&s, 0, sizeof s);
    memset(&fs, 0, sizeof fs);
    for (int i = 0; i < 36; i++) deck[i] = (uint8_t)i;
    for (int i = 35; i > 0; i--) { uint8_t j = (uint8_t)gd_rng_below((uint32_t)i + 1u), t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    const uint8_t trump = (uint8_t)gd_rng_below(4), def = (uint8_t)gd_rng_below(2), att = (uint8_t)(def ^ 1u);
    const int hn[2] = { 1 + (int)gd_rng_below(6), 1 + (int)gd_rng_below(6) };
    for (int p = 0; p < 2; p++) for (int i = 0; i < hn[p]; i++) { const uint8_t c = deck[k++]; GD_SET(s.hand[p], c); }
    // the table: attacks of up to two ranks drawn from the rest
    int nb = (int)gd_rng_below(4), unc = 0;
    const uint8_t r1 = (uint8_t)gd_rng_below(9), r2 = (uint8_t)gd_rng_below(9);
    uint8_t atk[3], dfc[3];
    int n = 0;
    for (int i = k; i < 36 && n < nb; i++) {
        const uint8_t c = deck[i];
        if (gd_rank_of[c] != r1 && gd_rank_of[c] != r2) continue;
        if (GD_HAS(s.tbl, c)) continue;
        atk[n] = c; dfc[n] = GD_NONE;
        GD_SET(s.tbl, c);
        // covered half the time, by a card from the rest that beats it
        if (gd_rng_below(2))
            for (int j = k; j < 36; j++) {
                const uint8_t d = deck[j];
                if (d == c || GD_HAS(s.tbl, d) || (gd_rank_of[d] != r1 && gd_rank_of[d] != r2)) continue;
                if (gd_can_cover(c, d, trump)) { dfc[n] = d; GD_SET(s.tbl, d); break; }
            }
        if (dfc[n] == GD_NONE) unc++;
        n++;
    }
    nb = n;
    if (unc > hn[def]) return 1;                       // not a position the rules allow
    for (int i = 0; i < nb; i++) {
        s.tranks |= (uint16_t)(1u << gd_rank_of[atk[i]]);
        if (dfc[i] != GD_NONE) s.tranks |= (uint16_t)(1u << gd_rank_of[dfc[i]]);
        else GD_SET(s.unc, atk[i]);
    }
    s.nb = (uint8_t)nb;
    s.trump = trump;
    s.flipped = GD_NONE;
    const int ontable = gd_count(&s.tbl);
    s.discard = (uint8_t)(36 - hn[0] - hn[1] - ontable);
    const int said_done = nb > 0 && unc > 0 && gd_rng_below(4) == 0;
    s.flags = (uint8_t)((def ? SIM_DEF1 : 0u) | (att ? SIM_FA1 : 0u) | (said_done ? (att ? SIM_GOOD1 : SIM_GOOD0) : 0u));
    s.first_out = GD_NONE;
    // the same board as foolish's state
    fs.num_players = 2;
    fs.power_suit = trump;
    fs.defender = (int8_t)def;
    fs.first_attacker = (int8_t)att;
    fs.status = GAME_STATUS_PLAYING;
    fs.num_battles = (uint8_t)nb;
    fs.discard_pile_length = s.discard;
    fs.good_mask = said_done ? (1u << att) : 0u;
    for (int p = 0; p < 2; p++) {
        for (int c = 0; c < 36; c++) if (GD_HAS(s.hand[p], c)) fs.hand[p] |= 1ull << fid_of((uint8_t)c);
        fs.status_p[p] = PLAYER_STATUS_IN;
        fs.elim_order[p] = -1;
    }
    fs.in_mask = 3;
    for (int i = 0; i < nb; i++) {
        fs.atk[i] = (uint8_t)fid_of(atk[i]);
        fs.table_vmask |= vmask(gd_rank_of[atk[i]] + MIN_VALUE_SMALL);
        if (dfc[i] != GD_NONE) {
            fs.def[i] = (uint8_t)fid_of(dfc[i]);
            fs.covered_mask |= 1ull << i;
            fs.table_vmask |= vmask(gd_rank_of[dfc[i]] + MIN_VALUE_SMALL);
        }
    }
    st_random++;
    for (int me = 0; me < 2; me++) if (!solve_pair(&s, &fs, me)) return 0;
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
    // then random small endgames, as many as there were games times 20
    for (unsigned long i = 0; i < games * 20; i++) {
        g_seed = first + i; g_ply = 0;
        gd_rng_seed((uint16_t)(first + i * 7919u + 17u));
        if (!random_endgame()) { printf("FAIL\n"); return 1; }
    }
    printf("solvetest: %lu positions with the stock gone in games, %lu random small endgames, %lu solves (budget %ld each)\n", st_pos, st_random, st_solves, budget_cap);
    printf("  full window, both finished %lu (wins %lu, losses %lu, draws %lu: identical), only this one %lu, only foolish's %lu, neither %lu\n",
           st_both, st_win, st_loss, st_draw, st_only_gd, st_only_f, st_neither);
    printf("  Moscow's windows (0, 2000) and (-1, 0), both finished: %lu, same verdict\n", st_win_cmp);
    printf("  this solver's nodes: %.1f a solve\nPASS\n", (double)st_nodes_gd / (double)st_solves);
    return 0;
}

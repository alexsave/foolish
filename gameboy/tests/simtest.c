// simtest - the rollout engine (src/gd_sim.c) against foolish's own
// (../foolish/c/src/cordite_sim.c, read-only, compiled into build/ with
// game_random renamed to dt_sim_random, which hands it this engine's draws:
// u = r / 65536 for each 16-bit draw r). For each seed: a deal, a random
// number of uniformly random moves on both rule engines, then the board as
// both rollout states, then both stepped one ply at a time
// (gd_sim_step, cd_sim_one_step): the same seat must act with the same draws,
// and every field of the two states must agree after every ply, to the end
// of the game. Then whole playouts (gd_sim_playout, cd_sim_playout) from the
// same boards must finish the same way.
//
// usage: simtest [games] [first_seed]
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"
#include "../src/gd_sim.h"
#include "game.h"
#include "legal.h"
#include "cordite_sim.h"
#include "foolish_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One policy ply (cordite_sim.c, exported without a header declaration).
int cd_sim_one_step(SimState *s);

double dt_sim_random(void) { return (double)(uint16_t)gd_rng_below(65536ul) / 65536.0; }

static unsigned long g_seed, g_ply;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "MISMATCH seed %lu ply %lu: ", g_seed, g_ply); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 0; } } while (0)

static unsigned long st_boards, st_plies, st_playouts, st_results[3];

// foolish's SimState draws a random INDEX of its deck array; this engine the
// same index of its stock in card order. So the array is put in card order:
// a determinized world's stock order is no part of the game.
static void sort_deck(SimState *ss) {
    for (int i = 1; i < ss->deck_n; i++)
        for (int j = i; j > 0 && gd_of_fid(ss->deck[j - 1]) > gd_of_fid(ss->deck[j]); j--) {
            uint8_t t = ss->deck[j]; ss->deck[j] = ss->deck[j - 1]; ss->deck[j - 1] = t;
        }
}

static void mask_of_u64(gd_mask *m, uint64_t h) {
    memset(m, 0, sizeof *m);
    for (int f = 0; f < 52; f++) if ((h >> f) & 1u) { uint8_t c = gd_of_fid(f); if (c < 36) GD_SET(*m, c); }
}

static int same(const gd_sim *s, const SimState *f) {
    gd_mask m;
    for (int p = 0; p < 2; p++) {
        mask_of_u64(&m, f->hand[p]);
        CHECK(!memcmp(&m, &s->hand[p], sizeof m), "hand %d", p);
        CHECK(((f->out_mask >> p) & 1u) == ((s->flags & (p ? SIM_OUT1 : SIM_OUT0)) != 0), "out %d", p);
        CHECK(((f->good_mask >> p) & 1u) == ((s->flags & (p ? SIM_GOOD1 : SIM_GOOD0)) != 0), "good %d", p);
    }
    memset(&m, 0, sizeof m);
    for (int i = 0; i < f->deck_n; i++) GD_SET(m, gd_of_fid(f->deck[i]));
    CHECK(!memcmp(&m, &s->stock, sizeof m), "stock");
    CHECK((f->has_flipped != 0) == ((s->flags & SIM_FLIPPED) != 0), "has_flipped");
    if (f->has_flipped) CHECK(gd_of_fid(f->flipped_id) == s->flipped, "flipped");
    CHECK(f->power_suit == s->trump, "trump");
    CHECK(f->num_battles == s->nb, "battles %d vs %d", f->num_battles, s->nb);
    gd_mask unc, tbl;
    memset(&unc, 0, sizeof unc); memset(&tbl, 0, sizeof tbl);
    for (int i = 0; i < f->num_battles; i++) {
        GD_SET(tbl, gd_of_fid(f->atk[i]));
        if ((f->covered_mask >> i) & 1u) GD_SET(tbl, gd_of_fid(f->def[i]));
        else GD_SET(unc, gd_of_fid(f->atk[i]));
    }
    CHECK(!memcmp(&unc, &s->unc, sizeof unc), "uncovered attacks");
    CHECK(!memcmp(&tbl, &s->tbl, sizeof tbl), "table cards");
    CHECK(f->defender == ((s->flags & SIM_DEF1) ? 1 : 0), "defender %d", f->defender);
    CHECK(f->first_attacker == ((s->flags & SIM_FA1) ? 1 : 0), "first attacker %d", f->first_attacker);
    CHECK(f->discard_pile_length == s->discard, "discard %d vs %d", f->discard_pile_length, s->discard);
    CHECK((f->num_eliminated ? f->elim_order[0] : 0xFF) == (s->first_out == GD_NONE ? 0xFF : s->first_out), "first out");
    return 1;
}

static int sim_done_f(const SimState *f) { return __builtin_popcount(f->in_mask) == 1; }

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
    // a random stretch of uniformly random moves on both rule engines
    const int warm = (int)gd_rng_below(120);
    for (int k = 0; k < warm && g.status == GD_PLAYING; k++) {
        const uint32_t n0 = gd_menu_count(&g, 0), n1 = gd_menu_count(&g, 1);
        if (!n0 && !n1) break;
        const uint32_t pick = gd_rng_below(n0 + n1);
        const uint8_t seat = pick < n0 ? 0 : 1;
        gd_move m;
        gd_menu_move(&g, seat, seat ? pick - n0 : pick, &m);
        CHECK(f_apply(f, seat, &m), "foolish refused a warm-up move");
        game_settle_status(f);
        CHECK(gd_apply(&g, 0, seat, &m), "gd refused a warm-up move");
    }
    if (g.status != GD_PLAYING) return 1;

    // the board as both rollout states, stepped ply by ply
    gd_sim s, s0;
    SimState fs, fs0;
    gd_sim_from_game(&s, &g);
    cd_sim_from_game(&fs, f);
    sort_deck(&fs);
    s0 = s; fs0 = fs;
    st_boards++;
    if (!same(&s, &fs)) return 0;
    for (g_ply = 1; g_ply <= 2000 && !gd_sim_done(&s); g_ply++) {
        const uint16_t before = gd_rng_get();
        const int8_t a = gd_sim_step(&s);
        const uint16_t after = gd_rng_get();
        gd_rng_set(before);
        const int b = cd_sim_one_step(&fs);
        CHECK(gd_rng_get() == after, "the two drew differently");
        CHECK(a == b, "seat %d acted, foolish's %d", a, b);
        if (a < 0) break;
        st_plies++;
        if (!same(&s, &fs)) return 0;
        CHECK(gd_sim_done(&s) == sim_done_f(&fs), "game over");
    }
    // whole playouts from the same board, for each seat's finish
    for (int me = 0; me < 2; me++) {
        gd_sim ps = s0;
        SimState pf = fs0;
        const uint16_t before = gd_rng_get();
        const uint8_t r = gd_sim_playout(&ps, (uint8_t)me, 1000);
        const uint16_t after = gd_rng_get();
        gd_rng_set(before);
        const int rf = cd_sim_playout(&pf, me, 1000, 1);
        CHECK(gd_rng_get() == after, "playout draws differ");
        CHECK(r == rf, "playout for seat %d: %d vs foolish %d", me, r, rf);
        st_playouts++;
        st_results[r]++;
    }
    return 1;
}

int main(int argc, char **argv) {
    unsigned long games = argc > 1 ? strtoul(argv[1], 0, 10) : 2000;
    unsigned long first = argc > 2 ? strtoul(argv[2], 0, 10) : 1;
    Game *f = calloc(1, sizeof(Game));
    if (!f) return 2;
    for (unsigned long sd = first; sd < first + games; sd++)
        if (!run_seed(sd, f)) { printf("FAIL\n"); return 1; }
    printf("simtest: %lu boards, %lu plies stepped and compared, %lu playouts compared (won %lu, lost %lu, unfinished %lu)\nPASS\n",
           st_boards, st_plies, st_playouts, st_results[1], st_results[2], st_results[0]);
    return 0;
}

// hide_eval (experiment harness; not shipped). Self-play octogen-vs-octogen over
// a seed range and report seat-0's results. Seat 0 applies the info-hiding rule
// iff bit 0 of OG_HIDE_MASK is set (read by octogen_strategy.c). Run it twice
// over the SAME seeds — OG_HIDE_MASK=1 (seat0 hides) and OG_HIDE_MASK=0 (control)
// — and join per-seed for a paired win-rate comparison. Prints per-seed finishes
// (`S <seed> <fp>`) plus a summary with the override fire count.
//   build: clang -O3 -Isrc -DCD_TT_BITS=20 -DOG_HIDE_UNCOVERABLE <core> hide_eval.c -lm
#include "game.h"
#include "legal.h"
#include "strategy.h"
#include "cordite_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern long og_hide_fire_count;   // OG_HIDE_UNCOVERABLE analysis counter

static int g_opp_key = STRAT_OCTOGEN;   // seats 1+; seat 0 is always octogen(hide)

static int dispatch(int key, const Game *g, int pi, const LegalMoves *m) {
    switch (key) {
        case STRAT_OCTOGEN:          return octogen_strategy_choose(g, pi, m, NULL);
        case STRAT_HANDWRITTEN:      return handwritten_strategy_choose(g, pi, m, NULL);
        case STRAT_HANDWRITTEN_PROD: return handwritten_prod_strategy_choose(g, pi, m, NULL);
        case STRAT_CORDITE:          return cordite_strategy_choose(g, pi, m, NULL);
        case STRAT_SEMTEX:           return semtex_strategy_choose(g, pi, m, NULL);
        case STRAT_ESPRESSO:         return espresso_strategy_choose(g, pi, m, NULL);
        case STRAT_RANDOM:           return random_strategy_choose(g, pi, m, NULL);
        default:                     return octogen_strategy_choose(g, pi, m, NULL);
    }
}

// seat-0 finish position (1=winner ... n=durak/fool), or -1 if game didn't end.
static int play_one(const uint8_t seed[32], int n) {
    cd_sim_solve_reset();
    game_set_seed(1);
    game_set_deal_seed_bytes((uint8_t *)seed, 32);
    Game g; memset(&g, 0, sizeof g);
    g.num_players = (int8_t)n;
    for (int i = 0; i < n; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)(i == 0 ? STRAT_OCTOGEN : g_opp_key);
        snprintf(g.players[i].player_id, sizeof g.players[i].player_id, "p%d", i);
    }
    start_game(&g);
    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 6000) {
        int elig[MAX_PLAYERS], n_e = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(game_random() * (i + 1)); if (j < 0) j = 0; if (j > i) j = i;
            int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int pi = elig[k]; LegalMoves moves; calculate_legal_moves(&g, pi, &moves);
            if (moves.n == 0) continue;
            int idx = dispatch(g.players[pi].strategy_key, &g, pi, &moves);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx]; bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(&g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover(&g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass(&g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(&g, pi); break;
                case MOVE_GOOD:   ok = handle_good(&g, pi); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }
    int fool = game_done(&g);
    if (fool < 0) return -1;
    // finish position of seat 0: 1 + (index in elimination order); durak = n.
    for (int i = 0; i < g.num_eliminated; i++) if (g.elimination_order[i] == 0) return i + 1;
    return n;   // seat 0 never went out -> it is the fool
}

int main(int argc, char **argv) {
    long seed0 = argc > 1 ? atol(argv[1]) : 1;
    long count = argc > 2 ? atol(argv[2]) : 20000;
    int  n     = argc > 3 ? atoi(argv[3]) : 2;
    int verbose = argc > 4 ? atoi(argv[4]) : 1;   // 1 = print per-seed lines
    if (argc > 5) { int k = parse_strategy(argv[5]); if (k >= 0) g_opp_key = k; }
    long wins = 0, valid = 0, sumfp = 0;
    og_hide_fire_count = 0;
    for (long s = seed0; s < seed0 + count; s++) {
        uint8_t seed[32]; uint64_t x = (uint64_t)s * 0x9E3779B97F4A7C15ull + 0x1234567;
        for (int i = 0; i < 32; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; seed[i] = (uint8_t)(x & 0xff); }
        int fp = play_one(seed, n);
        if (fp < 0) continue;
        valid++; sumfp += fp; wins += (fp == 1);
        if (verbose) printf("S %ld %d\n", s, fp);
    }
    const char *mask = getenv("OG_HIDE_MASK"); if (!mask) mask = "0";
    const char *oppn = argc > 5 ? argv[5] : "octogen";
    fprintf(stderr, "MASK=%s opp=%s n=%d games=%ld win0=%.3f%% mean_fp=%.4f fires=%ld\n",
            mask, oppn, n, valid, 100.0 * wins / (valid ? valid : 1),
            (double)sumfp / (valid ? valid : 1), og_hide_fire_count);
    return 0;
}

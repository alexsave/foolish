// C-side benchmark: the REAL kernel's cd_sim_solve_d — octogen/cordite's
// exact bitboard endgame solver (sim_solve_rec + sim_gen_moves + TT), the
// top of the measured native profile (36% + 20% of instructions at 2p,
// PROFILE_HOTPATH.md §T3). Compiled at the SHIPPED TT configuration
// (-DCD_TT_BITS=12 -DCD_TT_2WAY -DCD_TT_PACK8, same as bots.wasm).
//
// Workload: the deck-empty 2-players-IN endgame states from the shared dump
// (the only positions the solver is defined on). Deterministic: no RNG in
// the solver; the checksum covers value, aborted flag AND the node budget
// consumed, so the Rust port must expand the identical tree.
#include "../c/src/game.h"
#include "../c/src/legal.h"
#include "../c/src/cordite_sim.h"
#include "bench_common.h"

#define SOLVE_BUDGET 200000L
#define MAX_ENDGAME_CARDS 16

static void state_to_game(const PocState *s, Game *g) {
    memset(g, 0, sizeof(*g));
    g->status = (int8_t)s->status;
    g->num_players = (int8_t)s->num_players;
    g->power_suit = (int8_t)s->power_suit;
    g->first_attacker = (int8_t)s->first_attacker;
    g->defender = (int8_t)s->defender;
    g->num_battles = (int8_t)s->num_battles;
    g->deck_count = (int16_t)s->deck_count;
    g->discard_pile_length = (int16_t)s->discard_len;
    g->has_flipped = s->has_flipped != 0;
    g->flipped = s->has_flipped ? card_of_id(s->flipped_id) : (Card){0, 0};
    for (int i = 0; i < s->deck_count; i++) g->deck[i] = card_of_id(s->deck[i]);
    for (int i = 0; i < s->num_battles; i++) {
        g->table_battles[i].attack = card_of_id(s->atk[i]);
        g->table_battles[i].defense = (s->def[i] == 255) ? CARD_NONE : card_of_id(s->def[i]);
    }
    for (int p = 0; p < s->num_players; p++) {
        g->players[p].status = (int8_t)s->pstatus[p];
        g->players[p].hand_count = (int8_t)s->hand_count[p];
        for (int j = 0; j < s->hand_count[p]; j++)
            g->players[p].hand[j] = card_of_id(s->hand[p][j]);
    }
    g->num_eliminated = (int8_t)s->num_eliminated;
    for (int i = 0; i < s->num_eliminated; i++) g->elimination_order[i] = (int8_t)s->elim[i];
    g->good_players_mask = s->good_mask;
    g->num_logs = 0;
}

// The solver's domain gate: deck empty, exactly 2 players IN, small hands.
static int is_endgame(const PocState *s) {
    if (s->status != GAME_STATUS_PLAYING) return 0;
    if (s->deck_count != 0 || s->has_flipped) return 0;
    int in = 0, cards = 0;
    for (int p = 0; p < s->num_players; p++) {
        if (s->pstatus[p] == PLAYER_STATUS_IN) { in++; cards += s->hand_count[p]; }
    }
    if (in != 2) return 0;
    for (int i = 0; i < s->num_battles; i++) cards++; // table cards count too
    return cards <= MAX_ENDGAME_CARDS;
}

static Game g_tmp;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "states.bin";
    int reps = argc > 2 ? atoi(argv[2]) : 20;
    unsigned n_all;
    PocState *all = load_states(path, &n_all);

    unsigned n = 0;
    SimState *tmpl = (SimState *)calloc(n_all, sizeof(SimState));
    int *me = (int *)malloc(n_all * sizeof(int));
    for (unsigned i = 0; i < n_all; i++) {
        if (!is_endgame(&all[i])) continue;
        state_to_game(&all[i], &g_tmp);
        cd_sim_from_game(&tmpl[n], &g_tmp);
        me[n] = all[i].actor;
        n++;
    }
    if (n == 0) { fprintf(stderr, "no endgame states in dump\n"); return 1; }

    uint64_t sum = FNV_INIT;
    long total_nodes = 0;
    double best = 1e30, t_total = 0;
    for (int r = 0; r < reps; r++) {
        uint64_t rep_sum = FNV_INIT;
        long rep_nodes = 0;
        double t0 = now_s();
        for (unsigned i = 0; i < n; i++) {
            SimState s = tmpl[i];
            cd_sim_solve_reset();
            long budget = SOLVE_BUDGET;
            int aborted = 0;
            int v = cd_sim_solve_d(&s, me[i], -1000, 1000, &budget, 0, &aborted);
            rep_nodes += SOLVE_BUDGET - budget;
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)(int32_t)v);
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)aborted);
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)(SOLVE_BUDGET - budget));
        }
        double dt = now_s() - t0;
        t_total += dt;
        if (dt < best) best = dt;
        sum = rep_sum;
        total_nodes = rep_nodes;
    }

    printf("bench=solve impl=c states=%u reps=%d nodes_per_pass=%ld checksum=%016llx\n",
           n, reps, total_nodes, (unsigned long long)sum);
    printf("bench=solve impl=c best_ms=%.3f mean_ms=%.3f us_per_solve=%.2f mnodes_per_s=%.2f peak_rss_kb=%ld\n",
           best * 1e3, t_total / reps * 1e3,
           best * 1e6 / n, (double)total_nodes / best / 1e6, peak_rss_kb());
    free(tmpl); free(me); free(all);
    return 0;
}

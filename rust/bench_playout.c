// C-side benchmark: the REAL kernel's cd_sim_playout (c/src/cordite_sim.c,
// the cordite/octogen Monte-Carlo rollout core) over SimStates built from the
// dumped decision states. Deterministic: the engine LCG is re-seeded per
// playout with a seed both harnesses derive identically, so the Rust port
// must reproduce the playout results bit-for-bit.
#include "../c/src/game.h"
#include "../c/src/legal.h"
#include "../c/src/cordite_sim.h"
#include "bench_common.h"

#define GAME_PREFIX ((size_t)offsetof(Game, logs))

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

static Game g_tmp;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "states.bin";
    int reps = argc > 2 ? atoi(argv[2]) : 40;
    unsigned max_states = (unsigned)(argc > 3 ? atoi(argv[3]) : 600);
    unsigned n;
    PocState *st = load_states(path, &n);
    if (n > max_states) n = max_states;

    SimState *tmpl = (SimState *)calloc(n, sizeof(SimState));
    int *actors = (int *)malloc(n * sizeof(int));
    for (unsigned i = 0; i < n; i++) {
        state_to_game(&st[i], &g_tmp);
        cd_sim_from_game(&tmpl[i], &g_tmp);
        actors[i] = st[i].actor;
    }

    uint64_t sum = FNV_INIT;
    double best = 1e30, t_total = 0;
    long total_playouts = 0;
    for (int r = 0; r < reps; r++) {
        uint64_t rep_sum = FNV_INIT;
        double t0 = now_s();
        for (unsigned i = 0; i < n; i++) {
            SimState s = tmpl[i];
            uint32_t seed = 0x9E3779B9u ^ (i * 2654435761u) ^ ((uint32_t)r * 40503u);
            game_set_seed(seed);
            int fp = cd_sim_playout(&s, actors[i], 2000, 0);
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)fp);
            rep_sum = fnv1a_u32(rep_sum, s.in_mask);
            rep_sum = fnv1a_u32(rep_sum, s.out_mask);
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)s.num_eliminated);
            rep_sum = fnv1a(rep_sum, s.elim_order, sizeof(s.elim_order));
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)s.discard_pile_length);
        }
        double dt = now_s() - t0;
        t_total += dt;
        if (dt < best) best = dt;
        sum ^= rep_sum;   // every rep differs (seeded per rep); fold them all in
        total_playouts += n;
    }

    printf("bench=playout impl=c states=%u reps=%d checksum=%016llx\n",
           n, reps, (unsigned long long)sum);
    printf("bench=playout impl=c best_ms=%.3f mean_ms=%.3f us_per_playout=%.3f peak_rss_kb=%ld sizeof_SimState=%zu\n",
           best * 1e3, t_total / reps * 1e3,
           best * 1e6 / n, peak_rss_kb(), sizeof(SimState));
    free(tmpl); free(actors); free(st);
    (void)total_playouts;
    return 0;
}

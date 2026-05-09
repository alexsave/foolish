// Evaluator. Loads NN weights and plays N games of nitro vs an opponent
// (espresso or random), reporting wins/losses. Each game uses
// seed = (start_seed + i), seeded into both LCGs.
//
// Usage:
//   cnitro_eval --weights=weights.bin --opp=espresso --from=1 --to=1000

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/nn.h"
#include "../src/nitro_strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

static const char *get_arg(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0 && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') return argv[i] + 2 + kl + 1;
    }
    return def;
}
static int parse_int(const char *s, int def) { return s ? atoi(s) : def; }

static int play_one(uint32_t seed, int opp_strat) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = 2;
    g.players[0].status = PLAYER_STATUS_READY;
    g.players[1].status = PLAYER_STATUS_READY;
    g.players[0].strategy_key = STRAT_NITRO;
    g.players[1].strategy_key = (int8_t)opp_strat;
    snprintf(g.players[0].player_id, sizeof(g.players[0].player_id), "p0");
    snprintf(g.players[1].player_id, sizeof(g.players[1].player_id), "p1");
    start_game(&g);

    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 2000) {
        int elig[2]; int n_e = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(game_random() * (i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            int tmp = elig[i]; elig[i] = elig[j]; elig[j] = tmp;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int p = elig[k];
            LegalMoves moves;
            calculate_legal_moves(&g, p, &moves);
            if (moves.n == 0) continue;
            int strat = g.players[p].strategy_key;
            int idx;
            if (strat == STRAT_RANDOM)        idx = random_strategy_choose(&g, p, &moves, NULL);
            else if (strat == STRAT_ESPRESSO) idx = espresso_strategy_choose(&g, p, &moves, NULL);
            else                              idx = nitro_strategy_choose(&g, p, &moves, NULL);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx];
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(&g, p, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (&g, p, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (&g, p, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(&g, p); break;
                case MOVE_GOOD:   ok = handle_good  (&g, p); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }
    int loser = game_done(&g);
    if (loser < 0) return -1;
    return loser == 0 ? 0 : 1; // 1 = nitro won
}

int main(int argc, char **argv) {
    const char *weights = get_arg(argc, argv, "weights", "weights.bin");
    const char *opp_str = get_arg(argc, argv, "opp", "espresso");
    int seed_lo = parse_int(get_arg(argc, argv, "from", "1"), 1);
    int seed_hi = parse_int(get_arg(argc, argv, "to", "1000"), 1000);

    int opp;
    if (strcmp(opp_str, "espresso") == 0 || strcmp(opp_str, "esp") == 0) opp = STRAT_ESPRESSO;
    else if (strcmp(opp_str, "random") == 0 || strcmp(opp_str, "rand") == 0) opp = STRAT_RANDOM;
    else { fprintf(stderr, "unknown opp '%s'\n", opp_str); return 2; }

    NNParams *p = malloc(sizeof(NNParams));
    if (!nn_load(weights, p)) {
        fprintf(stderr, "failed to load weights from %s\n", weights);
        return 1;
    }
    nitro_strategy_set_params(p);

    setvbuf(stderr, NULL, _IOLBF, 0);
    int wins = 0, draws = 0, total = 0;
    clock_t start = clock();
    for (int s = seed_lo; s <= seed_hi; s++) {
        int r = play_one((uint32_t)s, opp);
        total++;
        if (r == 1) wins++;
        else if (r < 0) draws++;
        if (total % 100 == 0) {
            double dt = (double)(clock() - start) / CLOCKS_PER_SEC;
            fprintf(stderr, "  ... %d games, wins=%d (%.1f%%), dt=%.1fs\n",
                    total, wins, 100.0 * wins / total, dt);
        }
    }
    double dt = (double)(clock() - start) / CLOCKS_PER_SEC;
    int losses = total - wins - draws;
    printf("nitro vs %s  seeds %d..%d  wins=%d/%d (%.1f%%)  losses=%d  draws=%d  dt=%.1fs\n",
           opp_str, seed_lo, seed_hi, wins, total, 100.0 * wins / total,
           losses, draws, dt);
    free(p);
    return 0;
}

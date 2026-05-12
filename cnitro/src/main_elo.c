// ELO arena. Picks random player counts and strategy pools, plays games,
// updates per-strategy ELO using the same pairwise formula the production
// server uses (see common_utils.ts:calculateEloChange and
// utils.ts:updateEloRatings).
//
// Usage:
//   cnitro_elo --games=500 \
//              --pool=random,espresso,handwritten,robusta,firecracker,gunpowder \
//              --pcs=2,3,4,5,6,7,8 \
//              --snapshot-every=50 \
//              [--nitro-weights=weights.bin]
//
// All competitors start at ELO 1000. Each game picks one player count from
// --pcs and fills the seats by sampling strategies uniformly (with
// replacement) from --pool. After each game we run pairwise ELO updates
// for every strategy seat in the game and add the changes to the per-
// strategy running totals.

#include "game.h"
#include "legal.h"
#include "strategy.h"
#include "nitro_strategy.h"
#include "robusta_strategy.h"
#include "firecracker_strategy.h"
#include "gunpowder_strategy.h"
#include "nn.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define K_FACTOR        32.0
#define STARTING_ELO    1000.0
#define MAX_STRATS      16

typedef struct {
    char   name[32];
    int    strat_key;
    double elo;
    int    games;
    int    wins;
    int    durak;
} Strategy;

static Strategy STRATS[MAX_STRATS];
static int      N_STRATS = 0;

static const char *get_arg(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0
            && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') {
            return argv[i] + 2 + kl + 1;
        }
    }
    return def;
}
static int parse_int_arg(const char *s, int def) { return s ? atoi(s) : def; }

static int parse_strategy(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "random") || !strcmp(s, "rand"))       return STRAT_RANDOM;
    if (!strcmp(s, "espresso") || !strcmp(s, "esp"))      return STRAT_ESPRESSO;
    if (!strcmp(s, "handwritten") || !strcmp(s, "hw"))    return STRAT_HANDWRITTEN;
    if (!strcmp(s, "nitro"))                              return STRAT_NITRO;
    if (!strcmp(s, "robusta") || !strcmp(s, "rob"))       return STRAT_ROBUSTA;
    if (!strcmp(s, "firecracker") || !strcmp(s, "fc"))    return STRAT_FIRECRACKER;
    if (!strcmp(s, "gunpowder") || !strcmp(s, "gp"))      return STRAT_GUNPOWDER;
    return -1;
}

static int dispatch_choose(int strat, const Game *g, int pi, const LegalMoves *moves) {
    switch (strat) {
        case STRAT_RANDOM:      return random_strategy_choose(g, pi, moves, NULL);
        case STRAT_ESPRESSO:    return espresso_strategy_choose(g, pi, moves, NULL);
        case STRAT_HANDWRITTEN: return handwritten_strategy_choose(g, pi, moves, NULL);
        case STRAT_NITRO:       return nitro_strategy_choose(g, pi, moves, NULL);
        case STRAT_ROBUSTA:     return robusta_strategy_choose(g, pi, moves, NULL);
        case STRAT_FIRECRACKER: return firecracker_strategy_choose(g, pi, moves, NULL);
        case STRAT_GUNPOWDER:   return gunpowder_strategy_choose(g, pi, moves, NULL);
        default:                return -1;
    }
}

static double elo_change(double player_rating, double opp_rating, double actual_score) {
    double expected = 1.0 / (1.0 + pow(10.0, (opp_rating - player_rating) / 400.0));
    return round(K_FACTOR * (actual_score - expected));
}

// Play one game with seat-i using strategy index `seat_strats[i]`.
// Writes the finish position of each seat into rankings_out (0 = winner,
// num_players - 1 = durak). Returns true on success.
static bool play_one_game(uint32_t seed, int num_players, const int *seat_strats,
                          int *rankings_out) {
    game_set_seed(seed);
    random_strategy_set_seed(seed);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)num_players;
    for (int i = 0; i < num_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)STRATS[seat_strats[i]].strat_key;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 4000) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(game_random() * (i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int pi = elig[k];
            LegalMoves moves;
            calculate_legal_moves(&g, pi, &moves);
            if (moves.n == 0) continue;
            int idx = dispatch_choose(g.players[pi].strategy_key, &g, pi, &moves);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx];
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(&g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (&g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (&g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(&g, pi); break;
                case MOVE_GOOD:   ok = handle_good  (&g, pi); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }
    if (game_done(&g) < 0) return false;
    // rankings[i] = seat that finished position i (0-indexed).
    // elimination_order is the seat order of clearings; the remaining seat is durak.
    for (int i = 0; i < num_players; i++) rankings_out[i] = -1;
    int rank = 0;
    for (int i = 0; i < g.num_eliminated; i++) rankings_out[rank++] = g.elimination_order[i];
    for (int s = 0; s < num_players; s++) {
        bool found = false;
        for (int i = 0; i < g.num_eliminated; i++) if (g.elimination_order[i] == s) { found = true; break; }
        if (!found) { rankings_out[rank++] = s; break; }
    }
    return rank == num_players;
}

// Apply pairwise ELO updates from one game. seat_strats[i] = strategy index
// at seat i; rankings[r] = seat that finished in position r.
static void update_elos_from_game(const int *seat_strats, const int *rankings, int num_players) {
    double changes[MAX_PLAYERS] = {0};
    for (int i = 0; i < num_players; i++) {
        int seat_i = rankings[i];
        double pr = STRATS[seat_strats[seat_i]].elo;
        for (int j = 0; j < num_players; j++) {
            if (i == j) continue;
            int seat_j = rankings[j];
            double opr = STRATS[seat_strats[seat_j]].elo;
            double score = (i < j) ? 1.0 : ((i > j) ? 0.0 : 0.5);
            changes[seat_i] += elo_change(pr, opr, score);
        }
    }
    for (int s = 0; s < num_players; s++) {
        int seat = s;
        STRATS[seat_strats[seat]].elo += changes[seat];
        STRATS[seat_strats[seat]].games++;
    }
    // wins/durak (winner = rankings[0], durak = rankings[num_players-1])
    STRATS[seat_strats[rankings[0]]].wins++;
    STRATS[seat_strats[rankings[num_players - 1]]].durak++;
}

static int cmp_by_elo(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    if (STRATS[ib].elo > STRATS[ia].elo) return 1;
    if (STRATS[ib].elo < STRATS[ia].elo) return -1;
    return 0;
}

static void print_snapshot(int game_no, double t) {
    int order[MAX_STRATS]; for (int i = 0; i < N_STRATS; i++) order[i] = i;
    qsort(order, N_STRATS, sizeof(int), cmp_by_elo);
    printf("\n=== ELO after %d games  (t=%.1fs) ===\n", game_no, t);
    printf("  %-14s %8s  %6s  %6s  %6s  %6s\n",
           "strategy", "elo", "games", "wins", "durak", "winΔ");
    for (int k = 0; k < N_STRATS; k++) {
        int i = order[k];
        double winr = STRATS[i].games ? (double)STRATS[i].wins / STRATS[i].games : 0.0;
        double durakr = STRATS[i].games ? (double)STRATS[i].durak / STRATS[i].games : 0.0;
        printf("  %-14s %8.1f  %6d  %5.1f%%  %5.1f%%  %+6.1f\n",
               STRATS[i].name, STRATS[i].elo, STRATS[i].games,
               winr * 100.0, durakr * 100.0,
               STRATS[i].elo - STARTING_ELO);
    }
}

static double wall_secs(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    const char *pool_str = get_arg(argc, argv, "pool",
                                    "random,handwritten,espresso,robusta,firecracker,gunpowder");
    const char *pcs_str  = get_arg(argc, argv, "pcs", "2,3,4,5,6,7,8");
    int games            = parse_int_arg(get_arg(argc, argv, "games", "500"), 500);
    int snap_every       = parse_int_arg(get_arg(argc, argv, "snapshot-every", "50"), 50);
    uint32_t seed0       = (uint32_t)parse_int_arg(get_arg(argc, argv, "seed", "1"), 1);
    const char *nitro_w  = get_arg(argc, argv, "nitro-weights", NULL);

    // Parse strategy pool.
    char buf[256]; strncpy(buf, pool_str, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char *tok = strtok(buf, ",");
    while (tok && N_STRATS < MAX_STRATS) {
        int sk = parse_strategy(tok);
        if (sk < 0) { fprintf(stderr, "unknown strategy: %s\n", tok); return 2; }
        snprintf(STRATS[N_STRATS].name, sizeof(STRATS[N_STRATS].name), "%s", tok);
        STRATS[N_STRATS].strat_key = sk;
        STRATS[N_STRATS].elo = STARTING_ELO;
        N_STRATS++;
        tok = strtok(NULL, ",");
    }
    if (N_STRATS < 2) { fprintf(stderr, "pool needs at least 2 strategies\n"); return 2; }

    // Parse pcs.
    int pcs[7]; int n_pcs = 0;
    char pbuf[64]; strncpy(pbuf, pcs_str, sizeof(pbuf) - 1); pbuf[sizeof(pbuf) - 1] = 0;
    for (char *t = strtok(pbuf, ","); t && n_pcs < 7; t = strtok(NULL, ",")) {
        int n = atoi(t);
        if (n >= 2 && n <= MAX_PLAYERS) pcs[n_pcs++] = n;
    }
    if (n_pcs == 0) { fprintf(stderr, "no valid player counts\n"); return 2; }

    // Optionally load nitro weights.
    NNParams *p_nitro = NULL;
    bool need_nitro = false;
    for (int i = 0; i < N_STRATS; i++) if (STRATS[i].strat_key == STRAT_NITRO) need_nitro = true;
    if (need_nitro) {
        if (!nitro_w) {
            fprintf(stderr, "pool includes nitro but no --nitro-weights given\n");
            return 2;
        }
        p_nitro = malloc(sizeof(NNParams));
        if (!nn_load(nitro_w, p_nitro)) {
            fprintf(stderr, "failed to load nitro weights from %s\n", nitro_w);
            return 2;
        }
        nitro_strategy_set_params(p_nitro);
    }

    printf("=== ELO arena ===  pool=[%s]  pcs=[%s]  games=%d  k=%.0f\n",
           pool_str, pcs_str, games, K_FACTOR);

    setvbuf(stdout, NULL, _IOLBF, 0);
    double t_start = wall_secs();
    int rng_state = (int)seed0 * 2654435761u;

    for (int gi = 1; gi <= games; gi++) {
        // Pick PC.
        rng_state = rng_state * 1103515245 + 12345;
        int pc = pcs[((unsigned)rng_state >> 16) % (unsigned)n_pcs];
        // Sample strategies for each seat (with replacement).
        int seat_strats[MAX_PLAYERS];
        for (int s = 0; s < pc; s++) {
            rng_state = rng_state * 1103515245 + 12345;
            seat_strats[s] = ((unsigned)rng_state >> 16) % (unsigned)N_STRATS;
        }
        int rankings[MAX_PLAYERS];
        uint32_t game_seed = (uint32_t)(seed0 + (uint32_t)gi * 2654435761u);
        if (!play_one_game(game_seed, pc, seat_strats, rankings)) {
            fprintf(stderr, "game %d (pc=%d) aborted; skipping\n", gi, pc);
            continue;
        }
        update_elos_from_game(seat_strats, rankings, pc);

        if (gi % snap_every == 0 || gi == games) {
            print_snapshot(gi, wall_secs() - t_start);
        }
    }

    printf("\n=== final ELO ===\n");
    print_snapshot(games, wall_secs() - t_start);

    if (p_nitro) free(p_nitro);
    return 0;
}

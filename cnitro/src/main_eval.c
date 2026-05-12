// Evaluator. Plays N games at one or more player counts with a chosen
// protagonist strategy at seat 0 against `--opp` everywhere else, then
// reports either:
//   * 2p (default): win/loss vs opp, matching the legacy CLI.
//   * Multi-player: per-pc mean finish position + finish-position histogram.
//
// CLI:
//   cnitro_eval --strategy=nitro|dynamite [other flags]
//
// Strategy-specific:
//   nitro:    --weights=<tokenized NN binary>      (legacy)
//   dynamite: --ckpt=<GrpoNet checkpoint>          (GRPO SFT/RL output)
//
// Other flags:
//   --opp=espresso|random|handwritten|hw   (default espresso for nitro,
//                                            handwritten for dynamite)
//   --players=N or N1,N2,...               (default 2 = legacy)
//   --from=, --to=                          legacy 2p seed range
//   --games= and --seed-start=              per-pc multi-player path

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/nn.h"
#include "../src/nitro_strategy.h"
#include "../src/grpo_net.h"
#include "../src/dynamite_strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

static double wall_secs(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static const char *get_arg(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0 && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') return argv[i] + 2 + kl + 1;
    }
    return def;
}
static int parse_int(const char *s, int def) { return s ? atoi(s) : def; }

// Dispatch on strategy_key — seat 0 uses `protagonist`, others use `opp`.
static int dispatch_choose(int strat, const Game *g, int pi, const LegalMoves *moves) {
    switch (strat) {
        case STRAT_RANDOM:      return random_strategy_choose(g, pi, moves, NULL);
        case STRAT_ESPRESSO:    return espresso_strategy_choose(g, pi, moves, NULL);
        case STRAT_HANDWRITTEN: return handwritten_strategy_choose(g, pi, moves, NULL);
        case STRAT_NITRO:       return nitro_strategy_choose(g, pi, moves, NULL);
        case STRAT_DYNAMITE:    return dynamite_strategy_choose(g, pi, moves, NULL);
        case STRAT_ROBUSTA:     return robusta_strategy_choose(g, pi, moves, NULL);
        case STRAT_FIRECRACKER: return firecracker_strategy_choose(g, pi, moves, NULL);
        default:                return -1;
    }
}

static const char SUIT_CHAR[4] = { 'S', 'H', 'C', 'D' };
static const char *VALUE_STR[14] = {
    "?", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"
};
static void format_card_short(Card c, char *buf, size_t n) {
    const char *v = (c.value >= 1 && c.value <= 13) ? VALUE_STR[c.value] : "?";
    char s = (c.suit >= 0 && c.suit < 4) ? SUIT_CHAR[(int)c.suit] : '?';
    snprintf(buf, n, "%s%c", v, s);
}
static void format_card_list(const Card *cards, int n, char *buf, size_t buflen) {
    buf[0] = 0;
    for (int i = 0; i < n; i++) {
        char tmp[8]; format_card_short(cards[i], tmp, sizeof(tmp));
        strncat(buf, tmp, buflen - strlen(buf) - 1);
        if (i + 1 < n) strncat(buf, ",", buflen - strlen(buf) - 1);
    }
}

// Verbose variant of play_one: prints every move with player, type, cards.
static int play_one_verbose(uint32_t seed, int n_players, int protagonist, int opp) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)((i == 0) ? protagonist : opp);
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);
    printf("seed=%u  trump=%c  deck=%d  hands:", seed, SUIT_CHAR[(int)g.power_suit], g.deck_count);
    for (int i = 0; i < n_players; i++) {
        char hbuf[128]; format_card_list(g.players[i].hand, g.players[i].hand_count, hbuf, sizeof(hbuf));
        printf("  p%d=[%s]", i, hbuf);
    }
    printf("\n");

    int iters = 0;
    int step = 0;
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
            const char *mt =
                m->type == MOVE_ATTACK ? "ATTACK" :
                m->type == MOVE_COVER  ? "COVER " :
                m->type == MOVE_PASS   ? "PASS  " :
                m->type == MOVE_PICKUP ? "PICKUP" :
                m->type == MOVE_GOOD   ? "GOOD  " : "?";
            char cbuf[128]; format_card_list(m->cards, m->n_cards, cbuf, sizeof(cbuf));
            char table_buf[256]; table_buf[0] = 0;
            for (int b = 0; b < g.num_battles; b++) {
                char ab[8], db[8] = {0};
                format_card_short(g.table_battles[b].attack, ab, sizeof(ab));
                if (g.table_battles[b].has_defense)
                    format_card_short(g.table_battles[b].defense, db, sizeof(db));
                char piece[24];
                snprintf(piece, sizeof(piece), "%s/%s ", ab, db[0] ? db : "_");
                strncat(table_buf, piece, sizeof(table_buf) - strlen(table_buf) - 1);
            }
            printf("[%3d] p%d  %s  [%-22s]  hand=%d  table=[%s]\n",
                   ++step, pi, mt, cbuf, g.players[pi].hand_count,
                   table_buf[0] ? table_buf : "(empty)");
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
    if (game_done(&g) < 0) { printf("game incomplete\n"); return -1; }
    printf("\nelimination order:");
    for (int i = 0; i < g.num_eliminated; i++) printf(" p%d", g.elimination_order[i]);
    printf("\n");
    for (int i = 0; i < g.num_eliminated; i++) {
        if (g.elimination_order[i] == 0) return i + 1;
    }
    return g.num_players;
}

// Play one game. Returns dynamite/nitro seat-0 finish position (1..N).
// Position N == durak. -1 if the game aborted incomplete.
static int play_one(uint32_t seed, int n_players, int protagonist, int opp) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)((i == 0) ? protagonist : opp);
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
    if (game_done(&g) < 0) return -1;
    for (int i = 0; i < g.num_eliminated; i++) {
        if (g.elimination_order[i] == 0) return i + 1;
    }
    return g.num_players;   // seat 0 is the durak
}

static int parse_strategy(const char *s) {
    if (strcmp(s, "espresso") == 0 || strcmp(s, "esp") == 0) return STRAT_ESPRESSO;
    if (strcmp(s, "random")   == 0 || strcmp(s, "rand") == 0) return STRAT_RANDOM;
    if (strcmp(s, "handwritten") == 0 || strcmp(s, "hw") == 0) return STRAT_HANDWRITTEN;
    if (strcmp(s, "nitro")    == 0) return STRAT_NITRO;
    if (strcmp(s, "dynamite") == 0) return STRAT_DYNAMITE;
    if (strcmp(s, "robusta")  == 0) return STRAT_ROBUSTA;
    if (strcmp(s, "firecracker") == 0 || strcmp(s, "fc") == 0) return STRAT_FIRECRACKER;
    return -1;
}

int main(int argc, char **argv) {
    const char *strat_str = get_arg(argc, argv, "strategy", "nitro");
    int protagonist = parse_strategy(strat_str);
    if (protagonist < 0) { fprintf(stderr, "unknown strategy '%s'\n", strat_str); return 2; }

    const char *opp_default = (protagonist == STRAT_DYNAMITE) ? "handwritten" : "espresso";
    const char *opp_str = get_arg(argc, argv, "opp", opp_default);
    int opp = parse_strategy(opp_str);
    if (opp < 0) { fprintf(stderr, "unknown opp '%s'\n", opp_str); return 2; }

    const char *pcs = get_arg(argc, argv, "players", "2");

    // Load whichever weight file the protagonist needs.
    NNParams *p_nitro = NULL;
    GrpoNet   net;     memset(&net, 0, sizeof(net));
    bool      have_dynamite = false;
    if (protagonist == STRAT_NITRO) {
        const char *weights = get_arg(argc, argv, "weights", "weights.bin");
        p_nitro = malloc(sizeof(NNParams));
        if (!nn_load(weights, p_nitro)) {
            fprintf(stderr, "failed to load weights from %s\n", weights);
            free(p_nitro);
            return 1;
        }
        nitro_strategy_set_params(p_nitro);
    } else if (protagonist == STRAT_DYNAMITE) {
        const char *ckpt = get_arg(argc, argv, "ckpt", "grpo_sft.bin");
        grpo_net_alloc(&net);
        if (!grpo_net_load(&net, ckpt)) {
            fprintf(stderr, "failed to load checkpoint %s\n", ckpt);
            grpo_net_free(&net);
            return 1;
        }
        dynamite_strategy_set_net(&net);
        have_dynamite = true;
    }

    setvbuf(stderr, NULL, _IOLBF, 0);
    double t0 = wall_secs();

    // Verbose single-game path: --inspect=<seed> prints every move.
    const char *inspect_str = get_arg(argc, argv, "inspect", NULL);
    if (inspect_str) {
        int seed = atoi(inspect_str);
        int n = atoi(pcs);
        if (n < 2) n = 2;
        printf("=== INSPECT  %s vs %s  seed=%d  pc=%d ===\n",
               strat_str, opp_str, seed, n);
        int fp = play_one_verbose((uint32_t)seed, n, protagonist, opp);
        printf("\nseat-0 finish position: %d (1=winner, %d=durak)\n", fp, n);
        return 0;
    }

    // Legacy 2p path: --from / --to over a single player count.
    if (strchr(pcs, ',') == NULL && atoi(pcs) == 2
        && get_arg(argc, argv, "from", NULL) != NULL) {
        int seed_lo = parse_int(get_arg(argc, argv, "from", "1"), 1);
        int seed_hi = parse_int(get_arg(argc, argv, "to", "1000"), 1000);
        int wins = 0, draws = 0, total = 0;
        for (int s = seed_lo; s <= seed_hi; s++) {
            int fp = play_one((uint32_t)s, 2, protagonist, opp);
            total++;
            if (fp == 1) wins++;
            else if (fp < 0) draws++;
            if (total % 100 == 0) {
                double dt = wall_secs() - t0;
                fprintf(stderr, "  ... %d games, wins=%d (%.1f%%), dt=%.1fs\n",
                        total, wins, 100.0 * wins / total, dt);
            }
        }
        double dt = wall_secs() - t0;
        int losses = total - wins - draws;
        printf("%s vs %s  seeds %d..%d  wins=%d/%d (%.1f%%)  losses=%d  draws=%d  dt=%.1fs\n",
               strat_str, opp_str, seed_lo, seed_hi, wins, total, 100.0 * wins / total,
               losses, draws, dt);
    } else {
        // Multi-player path: report mean finish position + histogram per pc.
        int games  = parse_int(get_arg(argc, argv, "games", "200"), 200);
        uint32_t seed0 = (uint32_t)parse_int(get_arg(argc, argv, "seed-start", "200001"), 200001);
        printf("=== %s vs %s ===  games_per_pc=%d  seed_start=%u\n",
               strat_str, opp_str, games, seed0);
        printf("\n  pc  mean_finish  baseline  win_rate   histogram(p1..pN)\n");
        const char *q = pcs;
        while (*q) {
            int n = atoi(q);
            while (*q && *q != ',') q++;
            if (*q == ',') q++;
            if (n < 2 || n > MAX_PLAYERS) continue;
            uint64_t fp_sum = 0;
            uint64_t hist[MAX_PLAYERS + 1] = {0};
            int valid = 0;
            struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int gi = 0; gi < games; gi++) {
                int fp = play_one(seed0 + (uint32_t)gi, n, protagonist, opp);
                if (fp < 0) continue;
                fp_sum += (uint64_t)fp;
                hist[fp]++;
                valid++;
                if ((gi + 1) % 100 == 0) {
                    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
                    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
                    double mean_so_far = valid ? (double)fp_sum / valid : 0.0;
                    double win_so_far  = valid ? (double)hist[1] / valid : 0.0;
                    fprintf(stderr,
                            "    [pc=%d] %d/%d games  t=%.1fs  rate=%.1f g/s  mean=%.3f  win=%.1f%%\n",
                            n, gi + 1, games, dt, (gi + 1) / (dt > 0 ? dt : 1.0),
                            mean_so_far, win_so_far * 100.0);
                }
            }
            double mean_fp  = valid ? (double)fp_sum / valid : 0.0;
            double baseline = 1.0 + (double)(n - 1) / 2.0;
            double win_rate = valid ? (double)hist[1] / valid : 0.0;
            printf("  %2d  %11.3f  %8.3f  %7.1f%%   ",
                   n, mean_fp, baseline, win_rate * 100.0);
            for (int k = 1; k <= n; k++) printf("%llu ", (unsigned long long)hist[k]);
            printf("\n");
        }
    }

    if (p_nitro) free(p_nitro);
    if (have_dynamite) grpo_net_free(&net);
    return 0;
}

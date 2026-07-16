// Distill dumper. Plays all-cordite self-play games (CD_BUDGET=prod,
// CD_RACE off) with the same seeded harness loop as main_eval's play_one,
// and for every cordite decision with 2..--max-moves legal moves writes one
// CSV row per candidate:
//
//   game_seed,decision_id,candidate_index,chosen,f0,...,f43
//
// The dump is deterministic for a given (--players, --games, --seed-start):
// the feature extractor restores the random_strategy stream around its
// oracle calls (see distill_feat.c) and cordite saves/restores the game RNG
// itself, so the game traces are identical to plain all-cordite self-play.
//
// CLI:
//   cnitro_distill [--players=4] [--games=100] [--seed-start=300001]
//                  [--max-moves=64] [--out=path]

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cli_util.h"
#include "../src/distill_feat.h"
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

static long g_decisions = 0;       // dumped decisions (moves.n >= 2, <= cap)
static long g_rows = 0;
static long g_skipped_big = 0;     // decisions skipped for moves.n > cap
static long g_timed = 0;           // all real decisions (moves.n >= 2)
static double g_choose_secs = 0.0; // cordite time, for the baseline us/decision

// play_one from main_eval.c with every seat on cordite, plus the dump hook.
// Returns seat-0 finish position, or -1 if the game aborted incomplete.
static int play_one_dump(uint32_t seed, int n_players, int max_moves, FILE *out) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)STRAT_CORDITE;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    static double feats[DISTILL_NUM_FEATURES];
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
            double t0 = wall_secs();
            int idx = cordite_strategy_choose(&g, pi, &moves, NULL);
            if (moves.n >= 2) { g_choose_secs += wall_secs() - t0; g_timed++; }
            if (idx < 0 || idx >= moves.n) continue;

            if (moves.n >= 2) {
                if (moves.n > max_moves) {
                    g_skipped_big++;
                } else {
                    // Dump this decision: one row per candidate.
                    long did = g_decisions++;
                    distill_decision_reset();
                    for (int ci = 0; ci < moves.n; ci++) {
                        distill_features(&g, pi, &moves.moves[ci], &moves, feats);
                        fprintf(out, "%u,%ld,%d,%d", seed, did, ci, ci == idx);
                        for (int f = 0; f < DISTILL_NUM_FEATURES; f++)
                            fprintf(out, ",%.17g", feats[f]);
                        fputc('\n', out);
                        g_rows++;
                    }
                }
            }

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
    return g.num_players;
}

int main(int argc, char **argv) {
    int n_players  = parse_int(get_arg(argc, argv, "players", "4"), 4);
    int games      = parse_int(get_arg(argc, argv, "games", "100"), 100);
    uint32_t seed0 = (uint32_t)parse_int(get_arg(argc, argv, "seed-start", "300001"), 300001);
    int max_moves  = parse_int(get_arg(argc, argv, "max-moves", "64"), 64);
    const char *out_path = get_arg(argc, argv, "out", NULL);

    if (n_players < 2 || n_players > MAX_PLAYERS) {
        fprintf(stderr, "bad --players\n"); return 2;
    }

    // Label generation is always the production budget with racing OFF.
    setenv("CD_BUDGET", "prod", 1);
    unsetenv("CD_RACE");

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) { fprintf(stderr, "cannot open %s\n", out_path); return 2; }
    }

    fprintf(out, "game_seed,decision_id,candidate_index,chosen");
    for (int f = 0; f < DISTILL_NUM_FEATURES; f++) fprintf(out, ",f%d", f);
    fputc('\n', out);

    setvbuf(stderr, NULL, _IOLBF, 0);
    double t0 = wall_secs();
    int done = 0;
    for (int gi = 0; gi < games; gi++) {
        int fp = play_one_dump(seed0 + (uint32_t)gi, n_players, max_moves, out);
        if (fp >= 0) done++;
        if ((gi + 1) % 25 == 0) {
            double dt = wall_secs() - t0;
            fprintf(stderr, "  ... %d/%d games  decisions=%ld  rows=%ld  "
                    "%.2f g/s  cordite %.0f us/decision\n",
                    gi + 1, games, g_decisions, g_rows, (gi + 1) / dt,
                    g_timed ? 1e6 * g_choose_secs / (double)g_timed : 0.0);
        }
    }
    double dt = wall_secs() - t0;
    fprintf(stderr, "pc=%d games=%d done=%d decisions=%ld rows=%ld "
            "skipped_big=%ld dt=%.1fs (%.2f g/s) cordite_mean=%.0f us/decision\n",
            n_players, games, done, g_decisions, g_rows, g_skipped_big, dt,
            games / (dt > 0 ? dt : 1),
            g_timed ? 1e6 * g_choose_secs / (double)g_timed : 0.0);
    if (out != stdout) fclose(out);
    return 0;
}

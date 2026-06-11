// Showcase generator. Plays all-cordite games (big brain vs big brain) and
// dumps each finished game's log stream as one JSON line on stdout. A TS
// translator (tests/cordite_showcase.ts) reads these, scores them for drama,
// and encodes the wildest into a self-contained replay URL.
//
// The log stream mirrors the production game_logs exactly (same LOG_* types,
// same order), so the TS replay codec round-trips a C-played game unchanged.
//
// Usage:
//   cnitro_showcase --games=80 --pcs=4,6 --seed=1 > games.jsonl
//
// NOTE: player counts are restricted to those where the C and TS engines
// agree on deck size (2..4 -> 36-card, 6..8 -> 52-card). n=5 differs (C uses
// 36, TS uses 52) and is rejected.

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cli_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

static void emit_pair(const LogPair *p) {
    int ts = p->has_target ? p->target.suit  : -1;
    int tv = p->has_target ? p->target.value : -1;
    printf("[%d,%d,%d,%d]", p->primary.suit, p->primary.value, ts, tv);
}

// Dump one finished game as a JSON object on a single line.
static void emit_game(const Game *g, int np, uint32_t seed) {
    printf("{\"np\":%d,\"seed\":%u,\"flip\":[%d,%d],\"logs\":[",
           np, seed, g->flipped.suit, g->flipped.value);
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        if (i) printf(",");
        printf("[%d,%d,%d,[", l->log_type, l->player_idx, l->defender_index);
        for (int k = 0; k < l->num_pairs; k++) {
            if (k) printf(",");
            emit_pair(&l->pairs[k]);
        }
        printf("]]");
    }
    printf("]}\n");
}

// Play one all-cordite game to completion. Returns true if it finished with a
// single durak, false if it stalled.
static bool play_all_cordite(Game *g, int np, uint32_t seed) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) {
        g->players[i].status = PLAYER_STATUS_READY;
        g->players[i].strategy_key = (int8_t)STRAT_CORDITE;
        snprintf(g->players[i].player_id, sizeof(g->players[i].player_id), "p%d", i);
    }
    start_game(g);

    int iters = 0;
    while (game_done(g) < 0 && iters++ < 4000) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g->num_players; i++) if (should_bot_act(g, i)) elig[n_e++] = i;
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
            calculate_legal_moves(g, pi, &moves);
            if (moves.n == 0) continue;
            int idx = cordite_strategy_choose(g, pi, &moves, NULL);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx];
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(g, pi); break;
                case MOVE_GOOD:   ok = handle_good  (g, pi); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }
    return game_done(g) >= 0;
}

int main(int argc, char **argv) {
    int games      = parse_int(get_arg(argc, argv, "games", "80"), 80);
    uint32_t seed0 = (uint32_t)parse_int(get_arg(argc, argv, "seed", "1"), 1);
    const char *pcs = get_arg(argc, argv, "pcs", "4,6");

    setvbuf(stderr, NULL, _IOLBF, 0);

    int pc_list[8]; int n_pc = 0;
    char pbuf[64]; strncpy(pbuf, pcs, sizeof(pbuf) - 1); pbuf[sizeof(pbuf) - 1] = 0;
    for (char *t = strtok(pbuf, ","); t && n_pc < 8; t = strtok(NULL, ",")) {
        int n = atoi(t);
        if (n == 5) { fprintf(stderr, "skipping n=5 (C/TS deck mismatch)\n"); continue; }
        if (n >= 2 && n <= MAX_PLAYERS) pc_list[n_pc++] = n;
    }
    if (n_pc == 0) { fprintf(stderr, "no valid player counts\n"); return 2; }

    int emitted = 0, stalled = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int pi = 0; pi < n_pc; pi++) {
        int np = pc_list[pi];
        for (int gi = 0; gi < games; gi++) {
            uint32_t seed = seed0 + (uint32_t)(pi * 100000 + gi);
            Game g;
            if (!play_all_cordite(&g, np, seed)) { stalled++; continue; }
            emit_game(&g, np, seed);
            emitted++;
            if ((gi + 1) % 10 == 0) {
                struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
                double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
                fprintf(stderr, "  [np=%d] %d/%d games  %.1fs  (%.1f g/s)\n",
                        np, gi + 1, games, dt, emitted / (dt > 0 ? dt : 1));
            }
        }
    }
    fprintf(stderr, "emitted %d games, %d stalled\n", emitted, stalled);
    return 0;
}

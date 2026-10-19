// C-side benchmark: the REAL kernel's calculate_legal_moves (c/src/legal.c,
// compiled with the production native flags) over the dumped decision states.
// Prints a checksum that the Rust port must reproduce exactly.
#include "../c/src/game.h"
#include "../c/src/legal.h"
#include "bench_common.h"
#include "bench_game.h"

static LegalMoves g_out;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "states.bin";
    int reps = argc > 2 ? atoi(argv[2]) : 40;
    unsigned n;
    PocState *st = load_states(path, &n);

    // Prebuild prefix-sized Games so the timed loop measures enumeration only.
    unsigned char *arena = (unsigned char *)malloc((size_t)n * GAME_PREFIX);
    int *actors = (int *)malloc(n * sizeof(int));
    for (unsigned i = 0; i < n; i++) {
        state_to_game(&st[i], (Game *)(arena + (size_t)i * GAME_PREFIX));
        actors[i] = st[i].actor;
    }

    uint64_t sum = FNV_INIT;
    uint64_t total_moves = 0;
    double best = 1e30, t_total = 0;
    for (int r = 0; r < reps; r++) {
        uint64_t rep_sum = FNV_INIT;
        uint64_t rep_moves = 0;
        double t0 = now_s();
        for (unsigned i = 0; i < n; i++) {
            const Game *g = (const Game *)(arena + (size_t)i * GAME_PREFIX);
            calculate_legal_moves(g, actors[i], &g_out);
            rep_moves += (uint64_t)g_out.n;
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)g_out.n);
            for (int m = 0; m < g_out.n; m++) {
                const LegalMove *mv = &g_out.moves[m];
                rep_sum = fnv1a_u32(rep_sum, (uint32_t)((mv->type << 8) | (uint8_t)mv->n_cards));
                for (int c = 0; c < mv->n_cards; c++) {
                    rep_sum = fnv1a_u32(rep_sum, (uint32_t)card_to_id(mv->cards[c]));
                    if (mv->type == MOVE_COVER)
                        rep_sum = fnv1a_u32(rep_sum, (uint32_t)card_to_id(mv->attack_cards[c]));
                }
            }
        }
        double dt = now_s() - t0;
        t_total += dt;
        if (dt < best) best = dt;
        sum = rep_sum;
        total_moves = rep_moves;
    }

    printf("bench=legal impl=c states=%u reps=%d moves_per_pass=%llu checksum=%016llx\n",
           n, reps, (unsigned long long)total_moves, (unsigned long long)sum);
    printf("bench=legal impl=c best_ms=%.3f mean_ms=%.3f ns_per_call=%.1f ns_per_move=%.2f peak_rss_kb=%ld sizeof_LegalMoves=%zu sizeof_Game=%zu\n",
           best * 1e3, t_total / reps * 1e3,
           best * 1e9 / n, best * 1e9 / (double)total_moves,
           peak_rss_kb(), sizeof(LegalMoves), sizeof(Game));
    free(arena); free(actors); free(st);
    return 0;
}

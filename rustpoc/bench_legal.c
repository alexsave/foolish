// C-side benchmark: the REAL kernel's calculate_legal_moves (c/src/legal.c,
// compiled with the production native flags) over the dumped decision states.
// Prints a checksum that the Rust port must reproduce exactly.
#include "../c/src/game.h"
#include "../c/src/legal.h"
#include "bench_common.h"

// Rebuild a full Game from a portable state. Games are stored prefix-only
// (through the fields legal.c reads; logs are never touched by enumeration),
// the same trick the kernel's own solver scratch uses (solve_clone_prefix).
#define GAME_PREFIX ((size_t)offsetof(Game, logs))

static void state_to_game(const PocState *s, Game *g) {
    memset(g, 0, GAME_PREFIX);
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

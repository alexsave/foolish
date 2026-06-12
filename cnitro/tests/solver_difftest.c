// Exact-solver difftest: struct cd_solve vs bitboard cd_sim_solve.
//
// Plays real games (handwritten rollout policy on the struct engine). At every
// node where the deck is empty and exactly 2 players are IN — the regime the
// exact endgame solver runs in — it solves the FULL game value from each IN
// player's perspective with a large budget and a wide window, with BOTH
// engines, and asserts the values are bit-identical. Any genuine divergence is
// a solver bug (one engine plays a different exact game). Reports the count.
//
// Usage: ./solver_difftest [num_players] [games] [verbose]
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cordite_sim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int cd_struct_solve_test(const Game *g, int me, int alpha, int beta,
                                long budget, int *aborted);

static int n_pos = 0, n_cmp = 0, n_mismatch = 0, n_abort = 0;

static long BIG_BUDGET = 2000000L;

static void compare_at(const Game *g, int gi, int verbose) {
    // Find the (up to 2) IN players.
    int in[8], nin = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) in[nin++] = i;
    if (nin != 2) return;
    if (g->deck_count > 0 || g->has_flipped) return;
    n_pos++;

    for (int k = 0; k < nin; k++) {
        int me = in[k];
        int sa = 0, ba = 0;
        // wide window so neither engine prunes — full exact resolution
        int sv = cd_struct_solve_test(g, me, -2000, 2000, BIG_BUDGET, &sa);
        SimState s; cd_sim_from_game(&s, g);
        cd_sim_solve_reset();
        int bv = cd_sim_solve(&s, me, -2000, 2000, BIG_BUDGET, &ba);
        if (sa || ba) { n_abort++; continue; }   // skip unresolved (budget/depth)
        n_cmp++;
        if (sv != bv) {
            n_mismatch++;
            if (verbose && n_mismatch <= 6) {
                fprintf(stderr, "MISMATCH game %d me=%d struct=%d bit=%d "
                        "power=%d def=%d fa=%d nb=%d good=%u\n", gi, me, sv, bv,
                        g->power_suit, g->defender, g->first_attacker,
                        g->num_battles, g->good_players_mask);
                for (int i = 0; i < g->num_players; i++) {
                    if (g->players[i].status != PLAYER_STATUS_IN) continue;
                    fprintf(stderr, "  p%d hand:", i);
                    for (int j = 0; j < g->players[i].hand_count; j++)
                        fprintf(stderr, " s%dv%d", g->players[i].hand[j].suit, g->players[i].hand[j].value);
                    fprintf(stderr, "\n");
                }
                for (int i = 0; i < g->num_battles; i++)
                    fprintf(stderr, "  battle%d atk s%dv%d cov=%d%s\n", i,
                            g->table_battles[i].attack.suit, g->table_battles[i].attack.value,
                            g->table_battles[i].has_defense,
                            g->table_battles[i].has_defense ? "" : "");
            }
        }
    }
}

// Drive a game with the handwritten rollout policy (struct engine).
static int struct_step(Game *g) {
    for (int pi = 0; pi < g->num_players; pi++) {
        if (!should_bot_act(g, pi)) continue;
        LegalMoves moves;
        calculate_legal_moves_lite(g, pi, &moves);
        if (moves.n == 0) continue;
        int idx = handwritten_strategy_choose(g, pi, &moves, NULL);
        if (idx < 0 || idx >= moves.n) continue;
        bool ok;
        switch (moves.moves[idx].type) {
            case MOVE_ATTACK: ok = handle_attack(g, pi, moves.moves[idx].cards, moves.moves[idx].n_cards); break;
            case MOVE_COVER:  ok = handle_cover(g, pi, moves.moves[idx].cards, moves.moves[idx].attack_cards, moves.moves[idx].n_cards); break;
            case MOVE_PASS:   ok = handle_pass(g, pi, moves.moves[idx].cards, moves.moves[idx].n_cards); break;
            case MOVE_PICKUP: ok = handle_pickup(g, pi); break;
            case MOVE_GOOD:   ok = handle_good(g, pi); break;
            default: ok = false;
        }
        if (ok) return pi;
    }
    return -1;
}

int main(int argc, char **argv) {
    int np = argc > 1 ? atoi(argv[1]) : 2;
    int games = argc > 2 ? atoi(argv[2]) : 400;
    int verbose = argc > 3;
    if (argc > 4) BIG_BUDGET = atol(argv[4]);

    for (int gi = 0; gi < games; gi++) {
        game_set_seed(7000 + gi * 13);
        random_strategy_set_seed(7000 + gi * 13);
        Game g; memset(&g, 0, sizeof(g));
        g.num_players = np;
        for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
        start_game(&g);

        int guard = 0;
        while (game_done(&g) < 0 && guard++ < 4000) {
            compare_at(&g, gi, verbose);
            if (struct_step(&g) < 0) break;
        }
    }
    fprintf(stderr, "solver_difftest np=%d games=%d endgame_positions=%d "
            "compared=%d aborted=%d mismatches=%d\n",
            np, games, n_pos, n_cmp, n_abort, n_mismatch);
    return n_mismatch > 0 ? 1 : 0;
}

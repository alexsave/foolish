// Difftest cd_sim_apply_root_move vs cd_apply/handle_*: for many real game
// positions, apply each legal move with both engines and compare resulting
// state (hands, battles, defender, statuses).
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cordite_sim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int card_id_of(Card c) { return c.suit * 13 + (c.value - 1); }

static int apply_struct(Game *g, int p, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, p, m->cards, m->n_cards);
        case MOVE_COVER:  return handle_cover(g, p, m->cards, m->attack_cards, m->n_cards);
        case MOVE_PASS:   return handle_pass(g, p, m->cards, m->n_cards);
        case MOVE_PICKUP: return handle_pickup(g, p);
        case MOVE_GOOD:   return handle_good(g, p);
    }
    return 0;
}

int main(int argc, char **argv) {
    int np = argc > 1 ? atoi(argv[1]) : 4;
    int games = argc > 2 ? atoi(argv[2]) : 300;
    int verbose = argc > 3;
    long total = 0, okmismatch = 0, statemismatch = 0;

    for (int gi = 0; gi < games; gi++) {
        game_set_seed(5000 + gi * 13);
        random_strategy_set_seed(5000 + gi * 13);
        Game g; memset(&g, 0, sizeof(g));
        g.num_players = np;
        for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
        start_game(&g);

        int guard = 0;
        while (game_done(&g) < 0 && guard++ < 3000) {
            // for each actor who should act, test each of their legal moves
            for (int pi = 0; pi < np; pi++) {
                if (!should_bot_act(&g, pi)) continue;
                LegalMoves moves;
                calculate_legal_moves(&g, pi, &moves);
                for (int mi = 0; mi < moves.n; mi++) {
                    Game gc; memcpy(&gc, &g, sizeof(g));
                    SimState s; cd_sim_from_game(&s, &g);
                    uint32_t rng = game_rng_get();
                    int ok_s = apply_struct(&gc, pi, &moves.moves[mi]);
                    game_rng_set(rng);
                    int ok_b = cd_sim_apply_root_move(&s, pi, &moves.moves[mi]);
                    total++;
                    if ((ok_s != 0) != (ok_b != 0)) {
                        okmismatch++;
                        if (verbose && okmismatch <= 20)
                            fprintf(stderr, "g%d ok mismatch type=%d s=%d b=%d\n", gi, moves.moves[mi].type, ok_s, ok_b);
                        continue;
                    }
                    if (!ok_s) continue;
                    // compare states
                    int mm = 0;
                    if (gc.defender != s.defender || gc.first_attacker != s.first_attacker
                        || gc.num_battles != s.num_battles) mm = 1;
                    for (int p = 0; p < np; p++) {
                        uint64_t h = 0;
                        for (int j = 0; j < gc.players[p].hand_count; j++) h |= (1ull << card_id_of(gc.players[p].hand[j]));
                        if (h != s.hand[p]) mm = 1;
                        if (gc.players[p].status != s.status_p[p]) mm = 1;
                    }
                    if (!mm) for (int b = 0; b < gc.num_battles; b++) {
                        if (card_id_of(gc.table_battles[b].attack) != s.atk[b]) mm = 1;
                        int cs = !card_is_none(gc.table_battles[b].defense);
                        int cb = (s.covered_mask & (1ull << b)) != 0;
                        if (cs != cb) mm = 1;
                        if (cs && card_id_of(gc.table_battles[b].defense) != s.def[b]) mm = 1;
                    }
                    if (mm) {
                        statemismatch++;
                        if (verbose && statemismatch <= 20)
                            fprintf(stderr, "g%d STATE mismatch type=%d actor=%d def s=%d b=%d nb s=%d b=%d\n",
                                    gi, moves.moves[mi].type, pi, gc.defender, s.defender, gc.num_battles, s.num_battles);
                    }
                }
            }
            // advance with handwritten to explore positions
            for (int pi = 0; pi < np; pi++) {
                if (!should_bot_act(&g, pi)) continue;
                LegalMoves moves; calculate_legal_moves_lite(&g, pi, &moves);
                if (moves.n == 0) continue;
                int idx = handwritten_strategy_choose(&g, pi, &moves, NULL);
                if (idx < 0 || idx >= moves.n) continue;
                if (apply_struct(&g, pi, &moves.moves[idx])) break;
            }
        }
    }
    fprintf(stderr, "apply_difftest np=%d games=%d total=%ld ok_mismatch=%ld state_mismatch=%ld\n",
            np, games, total, okmismatch, statemismatch);
    return (okmismatch || statemismatch) ? 1 : 0;
}

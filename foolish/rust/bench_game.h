// bench_game.h - a portable PocState rebuilt as a kernel Game, for the four C
// benches (legal, playout, solve) that replay the dumped decision states.
//
// Games are rebuilt prefix-only: the fields legal.c, the playout and the solver
// read, through `logs` (which no bench touches), the same trick the kernel's
// own solver scratch uses (solve_clone_prefix). A caller that keeps a Game
// across calls leaves the tail as it was; num_logs is zero.
#ifndef BENCH_GAME_H
#define BENCH_GAME_H

#include <stddef.h>
#include <string.h>
#include "../c/src/game.h"
#include "bench_common.h"

#define GAME_PREFIX ((size_t)offsetof(Game, logs))

static inline void state_to_game(const PocState *s, Game *g) {
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

#endif

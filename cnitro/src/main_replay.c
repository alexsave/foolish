// Replay a single game with arbitrary strategies, printing every move and
// the relevant state. Used to investigate specific seeds (e.g. cases where
// random crushes espresso — what is random actually doing?).
//
// Usage:
//   cnitro_replay --seed=49107 --pairs=rand-esp

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static const char *get_arg(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0 && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') return argv[i] + 2 + kl + 1;
    }
    return def;
}
static int parse_int(const char *s, int def) { return s ? atoi(s) : def; }

static const char *suit_name(int s) {
    static const char *n[] = { "S","H","C","D" };
    return s >= 0 && s < 4 ? n[s] : "?";
}
static const char *value_name(int v) {
    static const char *vs[] = {"-","A","2","3","4","5","6","7","8","9","10","J","Q","K","A"};
    if (v >= 0 && v < (int)(sizeof(vs)/sizeof(vs[0]))) return vs[v];
    return "?";
}
static void print_card(Card c, int trump) {
    printf("%s%s", value_name(c.value), suit_name(c.suit));
    if (c.suit == trump) printf("*");
}

static int strat_id(const char *s) {
    if (!strcmp(s,"esp")||!strcmp(s,"espresso")) return STRAT_ESPRESSO;
    if (!strcmp(s,"rand")||!strcmp(s,"random")) return STRAT_RANDOM;
    if (!strcmp(s,"hw")||!strcmp(s,"handwritten")) return STRAT_HANDWRITTEN;
    fprintf(stderr,"unknown strat '%s'\n",s); exit(2);
}

static int run_strategy(int strat, const Game *g, int p, const LegalMoves *moves) {
    if (strat == STRAT_RANDOM) return random_strategy_choose(g, p, moves, NULL);
    if (strat == STRAT_ESPRESSO) return espresso_strategy_choose(g, p, moves, NULL);
    if (strat == STRAT_HANDWRITTEN) return handwritten_strategy_choose(g, p, moves, NULL);
    return -1;
}

static void print_hand(const Player *p, int trump) {
    for (int i = 0; i < p->hand_count; i++) {
        if (i) printf(" ");
        print_card(p->hand[i], trump);
    }
}

static void print_state(const Game *g) {
    int trump = g->power_suit;
    printf("  trump=%s  deck=%d  flip=", suit_name(trump), g->deck_count);
    if (g->has_flipped) print_card(g->flipped, trump); else printf("-");
    printf("  defender=p%d\n", g->defender);
    for (int i = 0; i < g->num_players; i++) {
        printf("  p%d (%s, %d): ", i,
               g->players[i].status == PLAYER_STATUS_IN ? "IN" :
               g->players[i].status == PLAYER_STATUS_OUT ? "OUT" : "?",
               g->players[i].hand_count);
        print_hand(&g->players[i], trump);
        printf("\n");
    }
    if (g->num_battles > 0) {
        printf("  table:");
        for (int i = 0; i < g->num_battles; i++) {
            printf(" ");
            print_card(g->table_battles[i].attack, trump);
            if (g->table_battles[i].has_defense) {
                printf("/");
                print_card(g->table_battles[i].defense, trump);
            } else printf("/_");
        }
        printf("\n");
    }
}

static void describe_move(const LegalMove *m, int trump) {
    switch (m->type) {
        case MOVE_ATTACK:
            printf("ATTACK [");
            for (int i = 0; i < m->n_cards; i++) { if (i) printf(","); print_card(m->cards[i], trump); }
            printf("]");
            break;
        case MOVE_COVER:
            printf("COVER [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                print_card(m->cards[i], trump); printf("→"); print_card(m->attack_cards[i], trump);
            }
            printf("]");
            break;
        case MOVE_PASS:
            printf("PASS [");
            for (int i = 0; i < m->n_cards; i++) { if (i) printf(","); print_card(m->cards[i], trump); }
            printf("]");
            break;
        case MOVE_PICKUP: printf("PICKUP"); break;
        case MOVE_GOOD:   printf("GOOD"); break;
        default: printf("?"); break;
    }
}

int main(int argc, char **argv) {
    int seed = parse_int(get_arg(argc, argv, "seed", "1"), 1);
    const char *pairs = get_arg(argc, argv, "pairs", "rand-esp");
    int n_p = parse_int(get_arg(argc, argv, "players", "2"), 2);

    int strats[MAX_PLAYERS]; int n_strats = 0;
    char buf[256]; strncpy(buf, pairs, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "-", &save); t && n_strats < MAX_PLAYERS;
         t = strtok_r(NULL, "-", &save)) {
        strats[n_strats++] = strat_id(t);
    }
    if (n_strats == 1) {
        for (int i = 1; i < n_p; i++) strats[i] = strats[0];
        n_strats = n_p;
    }
    if (n_strats != n_p) { fprintf(stderr,"need %d strats, got %d\n",n_p,n_strats); return 2; }

    game_set_seed((uint32_t)seed);
    random_strategy_set_seed((uint32_t)seed);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_p;
    for (int i = 0; i < n_p; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)strats[i];
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    int trump = g.power_suit;
    printf("=== seed=%d  trump=%s ===\n", seed, suit_name(trump));
    printf("Initial deal:\n");
    for (int i = 0; i < n_p; i++) {
        printf("  p%d (%s): ", i,
               strats[i] == STRAT_RANDOM ? "rand" :
               strats[i] == STRAT_ESPRESSO ? "esp" :
               strats[i] == STRAT_HANDWRITTEN ? "hw" : "?");
        print_hand(&g.players[i], trump);
        printf("\n");
    }
    printf("Flipped: ");
    if (g.has_flipped) print_card(g.flipped, trump); else printf("-");
    printf("  first_attacker=p%d  defender=p%d\n\n", g.first_attacker, g.defender);

    int move_no = 0, iters = 0;
    int max_iters = 4000;
    while (game_done(&g) < 0 && iters++ < max_iters) {
        int elig[MAX_PLAYERS]; int n_e = 0;
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
            int idx = run_strategy(strats[p], &g, p, &moves);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx];

            move_no++;
            printf("[%d] p%d (%s) %s ", move_no, p,
                   strats[p] == STRAT_RANDOM ? "rand" :
                   strats[p] == STRAT_ESPRESSO ? "esp" :
                   strats[p] == STRAT_HANDWRITTEN ? "hw" : "?",
                   p == g.defender ? "DEF" : "ATK");
            describe_move(m, trump);
            printf("   (had %d legal)\n", moves.n);

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
    printf("\n=== Game over: ");
    if (loser >= 0) printf("p%d is the durak (held %d cards)", loser, g.players[loser].hand_count);
    else printf("no winner");
    printf(" — total moves: %d ===\n", move_no);
    if (loser >= 0) {
        printf("Final hands:\n");
        for (int i = 0; i < n_p; i++) {
            printf("  p%d: %d cards", i, g.players[i].hand_count);
            if (g.players[i].hand_count > 0) {
                printf("  ");
                print_hand(&g.players[i], trump);
            }
            printf("\n");
        }
    }
    return 0;
}

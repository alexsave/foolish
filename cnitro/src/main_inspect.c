// Debug inspector. Loads NN weights and plays a single game, printing — at
// every nitro decision — the legal options in plain English along with the
// model's softmax probability for each. Useful for sanity-checking what the
// trained policy has actually learned.
//
// Usage:
//   cnitro_inspect --weights=weights.bin --seed=1 [--opp=espresso|random]

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/nn.h"
#include "../src/nitro_strategy.h"
#include "../src/tokenize.h"
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
    static const char *names[] = { "S", "H", "C", "D" };
    return s >= 0 && s < 4 ? names[s] : "?";
}
static const char *value_name(int v) {
    static const char *vs[] = {"-","2","3","4","5","6","7","8","9","10","J","Q","K","A"};
    if (v >= 0 && v < (int)(sizeof(vs)/sizeof(vs[0]))) return vs[v];
    return "?";
}
static void print_card(Card c) { printf("%s%s", value_name(c.value), suit_name(c.suit)); }
static void print_card_with_trump(Card c, int trump) {
    print_card(c);
    if (c.suit == trump) printf("*");
}

static void describe_move(const LegalMove *m, int trump) {
    switch (m->type) {
        case MOVE_ATTACK:
            printf("ATTACK [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                print_card_with_trump(m->cards[i], trump);
            }
            printf("]");
            break;
        case MOVE_COVER:
            printf("COVER [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                print_card_with_trump(m->cards[i], trump);
                printf("→");
                print_card_with_trump(m->attack_cards[i], trump);
            }
            printf("]");
            break;
        case MOVE_PASS:
            printf("PASS [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                print_card_with_trump(m->cards[i], trump);
            }
            printf("]");
            break;
        case MOVE_PICKUP: printf("PICKUP"); break;
        case MOVE_GOOD:   printf("GOOD"); break;
        default: printf("?"); break;
    }
}

static void print_state_summary(const Game *g, int bot_idx) {
    int trump = g->power_suit;
    const Player *me = &g->players[bot_idx];
    int opp_idx = 1 - bot_idx;
    const Player *opp = &g->players[opp_idx];

    printf("  role: %s   trump: %s   deck:%d", bot_idx == g->defender ? "DEFENDER" : "ATTACKER",
           suit_name(trump), g->deck_count + (g->has_flipped ? 1 : 0));
    printf("\n  my hand (%d): ", me->hand_count);
    for (int i = 0; i < me->hand_count; i++) {
        if (i) printf(" ");
        print_card_with_trump(me->hand[i], trump);
    }
    printf("\n  opp hand (%d): ", opp->hand_count);
    for (int i = 0; i < opp->hand_count; i++) {
        if (i) printf(" ");
        print_card_with_trump(opp->hand[i], trump);
    }
    if (g->num_battles > 0) {
        printf("\n  table:");
        for (int i = 0; i < g->num_battles; i++) {
            printf(" ");
            print_card_with_trump(g->table_battles[i].attack, trump);
            if (g->table_battles[i].has_defense) {
                printf("/");
                print_card_with_trump(g->table_battles[i].defense, trump);
            } else {
                printf("/_");
            }
        }
    }
    printf("\n");
}

// Score every full LegalMove by running the autoregressive policy in
// "evaluation" mode: multiply the per-step probabilities the model would
// assign at each atomic step. Returns probability mass per move.
static void score_moves(const NNParams *p, const Game *g, int bot_idx,
                        const LegalMoves *moves, double *out_prob) {
    static ForwardCache fc;
    int trump = g->power_suit;

    // Step 1 — collect first-step legal actions, like nitro_strategy.
    int first_legal[NUM_ACTIONS]; int n_first = 0;
    int8_t first_role[NUM_ACTIONS];
    for (int i = 0; i < NUM_ACTIONS; i++) first_role[i] = -1;
    for (int i = 0; i < moves->n; i++) {
        int t = moves->moves[i].type;
        if (t != MOVE_ATTACK && t != MOVE_COVER && t != MOVE_PASS) continue;
        for (int j = 0; j < moves->moves[i].n_cards; j++) {
            Card c = moves->moves[i].cards[j];
            int a = card_action_id(c.suit, c.value, trump);
            bool seen = false;
            for (int k = 0; k < n_first; k++) if (first_legal[k] == a) { seen = true; break; }
            if (!seen) { first_legal[n_first++] = a; first_role[a] = (int8_t)t; }
        }
    }
    bool has_pickup = false, has_good = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PICKUP) has_pickup = true;
        if (moves->moves[i].type == MOVE_GOOD) has_good = true;
    }
    if (has_pickup) first_legal[n_first++] = ACTION_PICKUP;
    if (has_good)   first_legal[n_first++] = ACTION_STOP;

    // Probability of each first-step action.
    InProgress ip0 = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    Tokenized tk; tokenize(g, bot_idx, &ip0, &tk);
    nn_forward(p, tk.tokens, tk.n_tokens, &fc);
    bool mask0[NUM_ACTIONS] = { false };
    for (int i = 0; i < n_first; i++) mask0[first_legal[i]] = true;
    float probs0[NUM_ACTIONS];
    nn_softmax_masked(fc.logits, mask0, probs0);

    // For each move, follow its full atomic trajectory and multiply probs.
    for (int mi = 0; mi < moves->n; mi++) {
        const LegalMove *m = &moves->moves[mi];
        double prob = 0;

        if (m->type == MOVE_PICKUP) {
            prob = probs0[ACTION_PICKUP];
        } else if (m->type == MOVE_GOOD) {
            prob = probs0[ACTION_STOP];
        } else {
            // attack / cover / pass: pick first card → extend until STOP.
            // Use the move's emitted card order (matches collect).
            int first_a = card_action_id(m->cards[0].suit, m->cards[0].value, trump);
            prob = probs0[first_a];

            // Now walk the rest. Re-tokenize at each step.
            InProgress ip;
            ip.role = m->type == MOVE_ATTACK ? INPROG_ATTACK
                    : m->type == MOVE_COVER  ? INPROG_COVER : INPROG_PASS;
            ip.n_cards_chosen = 1;
            ip.cards_chosen[0] = m->cards[0];

            for (int s = 1; s <= m->n_cards; s++) {
                // Compute the next-step legal set used by the inference path.
                // STOP is legal iff some matching move has exactly this many cards.
                bool stop_ok = false;
                int next_legal[NUM_ACTIONS]; int n_next = 0;
                bool seen[NUM_ACTIONS] = { false };
                for (int mj = 0; mj < moves->n; mj++) {
                    if (moves->moves[mj].type != m->type) continue;
                    if (moves->moves[mj].n_cards < ip.n_cards_chosen) continue;
                    bool ok = true;
                    for (int k = 0; k < ip.n_cards_chosen && ok; k++) {
                        bool found = false;
                        for (int q = 0; q < moves->moves[mj].n_cards; q++) {
                            if (card_eq(moves->moves[mj].cards[q], ip.cards_chosen[k])) { found = true; break; }
                        }
                        if (!found) ok = false;
                    }
                    if (!ok) continue;
                    if (moves->moves[mj].n_cards == ip.n_cards_chosen) stop_ok = true;
                    if (moves->moves[mj].n_cards > ip.n_cards_chosen) {
                        for (int q = 0; q < moves->moves[mj].n_cards; q++) {
                            Card c = moves->moves[mj].cards[q];
                            bool used = false;
                            for (int k = 0; k < ip.n_cards_chosen; k++) {
                                if (card_eq(ip.cards_chosen[k], c)) { used = true; break; }
                            }
                            if (used) continue;
                            int a = card_action_id(c.suit, c.value, trump);
                            if (!seen[a]) { seen[a] = true; next_legal[n_next++] = a; }
                        }
                    }
                }
                if (stop_ok) next_legal[n_next++] = ACTION_STOP;
                if (n_next == 0) break;
                if (n_next == 1) {
                    // forced — no choice, prob unchanged
                    if (s == m->n_cards) break;
                    Card cd; action_id_to_card(next_legal[0], trump, &cd);
                    ip.cards_chosen[ip.n_cards_chosen++] = cd;
                    continue;
                }
                tokenize(g, bot_idx, &ip, &tk);
                nn_forward(p, tk.tokens, tk.n_tokens, &fc);
                bool mask[NUM_ACTIONS] = { false };
                for (int i = 0; i < n_next; i++) mask[next_legal[i]] = true;
                float probs[NUM_ACTIONS];
                nn_softmax_masked(fc.logits, mask, probs);

                if (s == m->n_cards) {
                    // STOP step
                    prob *= probs[ACTION_STOP];
                    break;
                }
                int a = card_action_id(m->cards[s].suit, m->cards[s].value, trump);
                prob *= probs[a];
                ip.cards_chosen[ip.n_cards_chosen++] = m->cards[s];
            }
        }
        out_prob[mi] = prob;
    }
}

static int play_with_inspect(const NNParams *params, uint32_t seed, int opp_strat, int max_decisions) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = 2;
    g.players[0].status = PLAYER_STATUS_READY;
    g.players[1].status = PLAYER_STATUS_READY;
    g.players[0].strategy_key = STRAT_NITRO;
    g.players[1].strategy_key = (int8_t)opp_strat;
    snprintf(g.players[0].player_id, sizeof(g.players[0].player_id), "p0");
    snprintf(g.players[1].player_id, sizeof(g.players[1].player_id), "p1");
    start_game(&g);

    int decisions = 0;
    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 2000) {
        int elig[2]; int n_e = 0;
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
            int strat = g.players[pi].strategy_key;
            int idx;

            if (strat == STRAT_NITRO && decisions < max_decisions) {
                printf("\n=== Decision #%d (nitro is %s) ===\n",
                       decisions + 1, pi == g.defender ? "DEFENDER" : "ATTACKER");
                print_state_summary(&g, pi);

                idx = nitro_strategy_choose(&g, pi, &moves, NULL);

                // Raw next-action softmax — across ALL 42 actions, no mask.
                {
                    InProgress ip0 = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
                    Tokenized tk; tokenize(&g, pi, &ip0, &tk);
                    static ForwardCache fc_raw;
                    nn_forward(params, tk.tokens, tk.n_tokens, &fc_raw);
                    bool full[NUM_ACTIONS]; for (int i = 0; i < NUM_ACTIONS; i++) full[i] = true;
                    float raw[NUM_ACTIONS];
                    nn_softmax_masked(fc_raw.logits, full, raw);
                    int order_a[NUM_ACTIONS];
                    for (int i = 0; i < NUM_ACTIONS; i++) order_a[i] = i;
                    for (int i = 0; i < NUM_ACTIONS; i++) {
                        for (int j = i + 1; j < NUM_ACTIONS; j++) {
                            if (raw[order_a[j]] > raw[order_a[i]]) {
                                int t = order_a[i]; order_a[i] = order_a[j]; order_a[j] = t;
                            }
                        }
                    }
                    printf("  raw next-action top-3 (any, no legal mask):\n");
                    int trump = g.power_suit;
                    for (int k = 0; k < 3; k++) {
                        int a = order_a[k];
                        printf("    [%5.1f%%]  ", raw[a] * 100.0);
                        if (a == ACTION_PICKUP) printf("PICKUP");
                        else if (a == ACTION_STOP) printf("STOP");
                        else { Card c; action_id_to_card(a, trump, &c); print_card_with_trump(c, trump); }
                        printf("\n");
                    }
                }

                double *probs = malloc(moves.n * sizeof(double));
                score_moves(params, &g, pi, &moves, probs);

                // Sort by probability (descending) and print top K.
                int order[MAX_LEGAL_MOVES];
                for (int i = 0; i < moves.n; i++) order[i] = i;
                for (int i = 0; i < moves.n; i++) {
                    for (int j = i + 1; j < moves.n; j++) {
                        if (probs[order[j]] > probs[order[i]]) {
                            int t = order[i]; order[i] = order[j]; order[j] = t;
                        }
                    }
                }
                int top_k = moves.n < 8 ? moves.n : 8;
                printf("  top moves (%d total):\n", moves.n);
                for (int i = 0; i < top_k; i++) {
                    int o = order[i];
                    printf("    [%5.1f%%]%s ", probs[o] * 100.0,
                           o == idx ? " <-" : "  ");
                    describe_move(&moves.moves[o], g.power_suit);
                    printf("\n");
                }
                free(probs);
                decisions++;
            } else if (strat == STRAT_RANDOM) {
                idx = random_strategy_choose(&g, pi, &moves, NULL);
            } else if (strat == STRAT_ESPRESSO) {
                idx = espresso_strategy_choose(&g, pi, &moves, NULL);
            } else {
                idx = nitro_strategy_choose(&g, pi, &moves, NULL);
            }
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
    int loser = game_done(&g);
    printf("\n=== Game over: nitro %s ===\n", loser == 0 ? "LOST" : (loser == 1 ? "WON" : "draw/no winner"));
    return 0;
}

int main(int argc, char **argv) {
    const char *weights = get_arg(argc, argv, "weights", "/tmp/smoke_w.bin");
    int seed = parse_int(get_arg(argc, argv, "seed", "1"), 1);
    const char *opp_str = get_arg(argc, argv, "opp", "espresso");
    int max_decisions = parse_int(get_arg(argc, argv, "max", "20"), 20);

    int opp;
    if (strcmp(opp_str, "espresso") == 0 || strcmp(opp_str, "esp") == 0) opp = STRAT_ESPRESSO;
    else if (strcmp(opp_str, "random") == 0 || strcmp(opp_str, "rand") == 0) opp = STRAT_RANDOM;
    else { fprintf(stderr, "unknown opp '%s'\n", opp_str); return 2; }

    NNParams *p = malloc(sizeof(NNParams));
    if (!nn_load(weights, p)) {
        fprintf(stderr, "failed to load weights from %s\n", weights);
        return 1;
    }
    nitro_strategy_set_params(p);

    int rc = play_with_inspect(p, (uint32_t)seed, opp, max_decisions);
    free(p);
    return rc;
}

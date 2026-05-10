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

// Pretty-print one action token (PICKUP / STOP / a card).
static void print_action(int a, int trump) {
    if (a == ACTION_PICKUP) printf("PICKUP");
    else if (a == ACTION_STOP) printf("STOP");
    else { Card c; action_id_to_card(a, trump, &c); print_card_with_trump(c, trump); }
}

// Sort first n entries of `idx` by `score[idx[i]]` descending, in place.
static void sort_indices_desc_by_score(int *idx, int n, const float *score) {
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (score[idx[j]] > score[idx[i]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        }
    }
}

// Compute the legal "next step" set for an autoregressive trajectory: which
// cards may extend the in-progress move, and whether STOP is legal here.
// Walks the move list to find which atomic actions extend the partial.
static int compute_next_legal(const LegalMoves *moves, const InProgress *ip,
                              int trump, int *next_legal, bool *stop_ok) {
    *stop_ok = false;
    bool seen[NUM_ACTIONS] = { false };
    int n = 0;
    int role = ip->role == INPROG_ATTACK ? MOVE_ATTACK
             : ip->role == INPROG_COVER  ? MOVE_COVER
             : ip->role == INPROG_PASS   ? MOVE_PASS  : -1;
    for (int mj = 0; mj < moves->n; mj++) {
        if (moves->moves[mj].type != role) continue;
        if (moves->moves[mj].n_cards < ip->n_cards_chosen) continue;
        bool match = true;
        for (int k = 0; k < ip->n_cards_chosen && match; k++) {
            bool found = false;
            for (int q = 0; q < moves->moves[mj].n_cards; q++) {
                if (card_eq(moves->moves[mj].cards[q], ip->cards_chosen[k])) { found = true; break; }
            }
            if (!found) match = false;
        }
        if (!match) continue;
        if (moves->moves[mj].n_cards == ip->n_cards_chosen) *stop_ok = true;
        if (moves->moves[mj].n_cards > ip->n_cards_chosen) {
            for (int q = 0; q < moves->moves[mj].n_cards; q++) {
                Card c = moves->moves[mj].cards[q];
                bool used = false;
                for (int k = 0; k < ip->n_cards_chosen; k++) {
                    if (card_eq(ip->cards_chosen[k], c)) { used = true; break; }
                }
                if (used) continue;
                int a = card_action_id(c.suit, c.value, trump);
                if (!seen[a]) { seen[a] = true; next_legal[n++] = a; }
            }
        }
    }
    if (*stop_ok) next_legal[n++] = ACTION_STOP;
    return n;
}

// Walk the autoregressive policy step-by-step, printing the model's softmax
// over each step's legal next-actions and marking the action the strategy
// actually picks (argmax under the legal mask). This mirrors what
// nitro_strategy_choose does internally so the highlighted action and the
// final move both reflect the model's actual choices, not the canonical
// card order of any specific LegalMove.
static void print_atomic_walk(const NNParams *params, const Game *g, int bot_idx,
                              const LegalMoves *moves) {
    static ForwardCache fc;
    int trump = g->power_suit;
    bool is_def = (bot_idx == g->defender);

    // ---- Step 1: first action across all roles ----------------------
    bool has_pickup = false, has_good = false;
    int first_legal[NUM_ACTIONS]; int n_first = 0;
    int8_t first_role[NUM_ACTIONS];
    bool seen[NUM_ACTIONS] = { false };
    for (int i = 0; i < NUM_ACTIONS; i++) first_role[i] = -1;
    for (int i = 0; i < moves->n; i++) {
        int t = moves->moves[i].type;
        if (t == MOVE_ATTACK || t == MOVE_COVER || t == MOVE_PASS) {
            for (int j = 0; j < moves->moves[i].n_cards; j++) {
                Card c = moves->moves[i].cards[j];
                int a = card_action_id(c.suit, c.value, trump);
                if (!seen[a]) { seen[a] = true; first_legal[n_first++] = a; first_role[a] = (int8_t)t; }
            }
        } else if (t == MOVE_PICKUP) {
            has_pickup = true;
            if (!seen[ACTION_PICKUP]) { seen[ACTION_PICKUP] = true; first_legal[n_first++] = ACTION_PICKUP; }
        } else if (t == MOVE_GOOD) {
            has_good = true;
            if (!seen[ACTION_STOP]) { seen[ACTION_STOP] = true; first_legal[n_first++] = ACTION_STOP; }
        }
    }
    if (n_first == 0) return;

    InProgress ip0 = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    Tokenized tk; tokenize(g, bot_idx, &ip0, &tk);
    nn_forward(params, tk.tokens, tk.n_tokens, &fc);
    bool mask0[NUM_ACTIONS] = { false };
    for (int i = 0; i < n_first; i++) mask0[first_legal[i]] = true;
    float probs0[NUM_ACTIONS];
    nn_softmax_masked(fc.logits, mask0, probs0);

    // The strategy's argmax pick (replicates nitro_strategy_choose Step 1).
    int chosen_first = first_legal[0];
    for (int i = 1; i < n_first; i++) {
        if (probs0[first_legal[i]] > probs0[chosen_first]) chosen_first = first_legal[i];
    }

    int order[NUM_ACTIONS];
    for (int i = 0; i < n_first; i++) order[i] = first_legal[i];
    sort_indices_desc_by_score(order, n_first, probs0);

    printf("  step 1 (first action, %d legal):\n", n_first);
    int show = n_first < 6 ? n_first : 6;
    for (int i = 0; i < show; i++) {
        int a = order[i];
        printf("    [%5.1f%%]%s ", probs0[a] * 100.0, a == chosen_first ? " <-" : "  ");
        print_action(a, trump);
        printf("\n");
    }

    // ---- Resolve role from first action (mirrors nitro_strategy_choose) ----
    if (chosen_first == ACTION_PICKUP) { printf("  → PICKUP\n"); return; }
    if (chosen_first == ACTION_STOP)   { printf("  → %s\n", has_good ? "GOOD" : "STOP"); return; }
    int role_move_type;
    int8_t inferred = first_role[chosen_first];
    if (inferred < 0) role_move_type = is_def ? MOVE_COVER : MOVE_ATTACK;
    else if (is_def) {
        Card card; action_id_to_card(chosen_first, trump, &card);
        bool passable = false, coverable = false;
        for (int i = 0; i < moves->n; i++) {
            if (moves->moves[i].type == MOVE_PASS)
                for (int j = 0; j < moves->moves[i].n_cards; j++)
                    if (card_eq(moves->moves[i].cards[j], card)) passable = true;
            if (moves->moves[i].type == MOVE_COVER)
                for (int j = 0; j < moves->moves[i].n_cards; j++)
                    if (card_eq(moves->moves[i].cards[j], card)) coverable = true;
        }
        if (passable) role_move_type = MOVE_PASS;
        else if (coverable) role_move_type = MOVE_COVER;
        else role_move_type = inferred;
    } else role_move_type = inferred;

    InProgress ip;
    ip.role = role_move_type == MOVE_ATTACK ? INPROG_ATTACK
            : role_move_type == MOVE_COVER  ? INPROG_COVER : INPROG_PASS;
    ip.n_cards_chosen = 1;
    action_id_to_card(chosen_first, trump, &ip.cards_chosen[0]);
    (void)has_pickup;

    // ---- Subsequent autoregressive steps ----------------------------
    int step_no = 2;
    while (true) {
        int next_legal[NUM_ACTIONS]; bool stop_ok = false;
        int n_next = compute_next_legal(moves, &ip, trump, next_legal, &stop_ok);
        if (n_next == 0) break;

        if (n_next == 1) {
            printf("  step %d (forced): ", step_no);
            print_action(next_legal[0], trump);
            printf("\n");
            if (next_legal[0] == ACTION_STOP) break;
            Card cd; action_id_to_card(next_legal[0], trump, &cd);
            ip.cards_chosen[ip.n_cards_chosen++] = cd;
            step_no++;
            continue;
        }

        tokenize(g, bot_idx, &ip, &tk);
        nn_forward(params, tk.tokens, tk.n_tokens, &fc);
        bool mask[NUM_ACTIONS] = { false };
        for (int i = 0; i < n_next; i++) mask[next_legal[i]] = true;
        float probs[NUM_ACTIONS];
        nn_softmax_masked(fc.logits, mask, probs);
        int chosen_step = next_legal[0];
        for (int i = 1; i < n_next; i++) {
            if (probs[next_legal[i]] > probs[chosen_step]) chosen_step = next_legal[i];
        }

        for (int i = 0; i < n_next; i++) order[i] = next_legal[i];
        sort_indices_desc_by_score(order, n_next, probs);
        printf("  step %d (after [", step_no);
        for (int k = 0; k < ip.n_cards_chosen; k++) {
            if (k) printf(",");
            print_card_with_trump(ip.cards_chosen[k], trump);
        }
        printf("], %d legal):\n", n_next);
        int show_n = n_next < 6 ? n_next : 6;
        for (int i = 0; i < show_n; i++) {
            int a = order[i];
            printf("    [%5.1f%%]%s ", probs[a] * 100.0, a == chosen_step ? " <-" : "  ");
            print_action(a, trump);
            printf("\n");
        }
        step_no++;
        if (chosen_step == ACTION_STOP) break;
        Card cd; action_id_to_card(chosen_step, trump, &cd);
        ip.cards_chosen[ip.n_cards_chosen++] = cd;
    }

    // Final composed move — describe what the strategy actually picks.
    printf("  → %s [", role_move_type == MOVE_ATTACK ? "ATTACK"
                     : role_move_type == MOVE_COVER  ? "COVER"  : "PASS");
    for (int k = 0; k < ip.n_cards_chosen; k++) {
        if (k) printf(",");
        print_card_with_trump(ip.cards_chosen[k], trump);
    }
    printf("]\n");
}

// Predict the opponent's next first-action from the bot's perspective
// (their hand is hidden by the no-cheat tokenizer). Returns top-K legal
// actions sorted by predicted probability, and the rank of the actual
// first action they took (0-indexed; -1 if not in legal set).
typedef struct {
    int   top_actions[NUM_ACTIONS];
    float top_probs[NUM_ACTIONS];
    int   n;
    int   actual_action;
    int   actual_rank;
    float actual_prob;
} OppPrediction;

static void predict_opp_action(const NNParams *params, const Game *g, int bot_idx,
                               const LegalMoves *opp_moves, int opp_chosen,
                               OppPrediction *out) {
    static ForwardCache fc;
    int trump = g->power_suit;
    out->n = 0; out->actual_rank = -1; out->actual_action = -1; out->actual_prob = 0;

    // Aggregate opp's first-step legal set (we know it because legal-move
    // computation is from observable state — only the cover/attack values
    // matter, not opp's hand directly... but cards used DO depend on opp's
    // hand. The model can't know this set a priori; we use it just as an
    // evaluation mask to compute predictions over what opp COULD play.)
    int legal[NUM_ACTIONS]; int n_legal = 0;
    bool seen[NUM_ACTIONS] = { false };
    for (int i = 0; i < opp_moves->n; i++) {
        int t = opp_moves->moves[i].type;
        if (t == MOVE_ATTACK || t == MOVE_COVER || t == MOVE_PASS) {
            for (int j = 0; j < opp_moves->moves[i].n_cards; j++) {
                Card c = opp_moves->moves[i].cards[j];
                int a = card_action_id(c.suit, c.value, trump);
                if (!seen[a]) { seen[a] = true; legal[n_legal++] = a; }
            }
        } else if (t == MOVE_PICKUP) {
            if (!seen[ACTION_PICKUP]) { seen[ACTION_PICKUP] = true; legal[n_legal++] = ACTION_PICKUP; }
        } else if (t == MOVE_GOOD) {
            if (!seen[ACTION_STOP]) { seen[ACTION_STOP] = true; legal[n_legal++] = ACTION_STOP; }
        }
    }
    if (n_legal == 0) return;

    // Tokenize from BOT's POV — the model does not see opp's hand. Hence
    // its prediction is genuinely a guess from history + visible state.
    InProgress ip = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    Tokenized tk; tokenize(g, bot_idx, &ip, &tk);
    nn_forward(params, tk.tokens, tk.n_tokens, &fc);
    bool mask[NUM_ACTIONS] = { false };
    for (int i = 0; i < n_legal; i++) mask[legal[i]] = true;
    float probs[NUM_ACTIONS];
    nn_softmax_masked(fc.logits, mask, probs);

    int order[NUM_ACTIONS];
    for (int i = 0; i < n_legal; i++) order[i] = legal[i];
    sort_indices_desc_by_score(order, n_legal, probs);
    out->n = n_legal;
    for (int i = 0; i < n_legal; i++) {
        out->top_actions[i] = order[i];
        out->top_probs[i] = probs[order[i]];
    }

    // Resolve actual action: the first-step token of the move opp picked.
    const LegalMove *am = &opp_moves->moves[opp_chosen];
    int actual = -1;
    if (am->type == MOVE_PICKUP) actual = ACTION_PICKUP;
    else if (am->type == MOVE_GOOD) actual = ACTION_STOP;
    else if (am->n_cards > 0) actual = card_action_id(am->cards[0].suit, am->cards[0].value, trump);
    out->actual_action = actual;
    out->actual_prob = (actual >= 0) ? probs[actual] : 0;
    for (int i = 0; i < n_legal; i++) {
        if (order[i] == actual) { out->actual_rank = i; break; }
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

    // Opp-prediction running tally. At every opp turn we run the model from
    // bot's POV (so opp's hand is hidden by the no-cheat tokenizer) and ask
    // "where does the model think opp will play next?" then compare against
    // the action opp actually took. This is a measurement, not a training
    // signal — it shows how well the trunk's implicit belief lines up with
    // reality.
    int opp_n = 0, opp_top1 = 0, opp_top3 = 0;
    double opp_prob_actual_sum = 0;

    int decisions = 0;
    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 2000) {
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
            int strat = g.players[pi].strategy_key;
            int idx;
            bool printed_decision = false;
            int trump = g.power_suit;

            if (strat == STRAT_NITRO) {
                idx = nitro_strategy_choose(&g, pi, &moves, NULL);
                if (decisions < max_decisions) {
                    printed_decision = true;
                    printf("\n=== Decision #%d (nitro is %s) ===\n",
                           decisions + 1, pi == g.defender ? "DEFENDER" : "ATTACKER");
                    print_state_summary(&g, pi);
                    print_atomic_walk(params, &g, pi, &moves);
                    decisions++;
                }
            } else {
                // Opp turn. First, run the bot-POV model to see what it
                // expects the opponent to play.
                OppPrediction op;
                int bot_idx = (pi == 0) ? 1 : 0;
                if (strat == STRAT_RANDOM)           idx = random_strategy_choose(&g, pi, &moves, NULL);
                else if (strat == STRAT_ESPRESSO)    idx = espresso_strategy_choose(&g, pi, &moves, NULL);
                else if (strat == STRAT_HANDWRITTEN) idx = handwritten_strategy_choose(&g, pi, &moves, NULL);
                else                                 idx = nitro_strategy_choose(&g, pi, &moves, NULL);
                if (idx < 0 || idx >= moves.n) continue;

                predict_opp_action(params, &g, bot_idx, &moves, idx, &op);
                if (op.n > 0 && op.actual_action >= 0 && op.actual_rank >= 0) {
                    opp_n++;
                    if (op.actual_rank == 0) opp_top1++;
                    if (op.actual_rank < 3)  opp_top3++;
                    opp_prob_actual_sum += op.actual_prob;

                    if (decisions < max_decisions) {
                        printf("\n--- Opp turn (model prediction from bot POV) ---\n");
                        int show = op.n < 5 ? op.n : 5;
                        for (int i = 0; i < show; i++) {
                            int a = op.top_actions[i];
                            printf("    [%5.1f%%]%s ", op.top_probs[i] * 100.0,
                                   a == op.actual_action ? " <-" : "  ");
                            print_action(a, trump);
                            printf("\n");
                        }
                        printf("    actual rank: %d  (model gave %.1f%% to actual)\n",
                               op.actual_rank + 1, op.actual_prob * 100.0);
                    }
                }
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
            (void)printed_decision;
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }
    int loser = game_done(&g);
    printf("\n=== Game over: nitro %s   (decisions printed: %d) ===\n",
           loser == 0 ? "LOST" : (loser == 1 ? "WON" : "draw/no winner"), decisions);
    if (opp_n > 0) {
        printf("Opp-prediction: %d turns measured  top1=%.1f%%  top3=%.1f%%  mean P(actual)=%.1f%%\n",
               opp_n,
               100.0 * opp_top1 / opp_n,
               100.0 * opp_top3 / opp_n,
               100.0 * opp_prob_actual_sum / opp_n);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *weights = get_arg(argc, argv, "weights", "/tmp/smoke_w.bin");
    int seed = parse_int(get_arg(argc, argv, "seed", "1"), 1);
    const char *opp_str = get_arg(argc, argv, "opp", "espresso");
    // Default unbounded — show every nitro decision through end of game.
    // (1v1 games never approach 1000 decisions; the cap is just a safety
    // belt for inspector inputs.)
    int max_decisions = parse_int(get_arg(argc, argv, "max", "1000"), 1000);

    int opp;
    if (strcmp(opp_str, "espresso") == 0 || strcmp(opp_str, "esp") == 0) opp = STRAT_ESPRESSO;
    else if (strcmp(opp_str, "random") == 0 || strcmp(opp_str, "rand") == 0) opp = STRAT_RANDOM;
    else if (strcmp(opp_str, "handwritten") == 0 || strcmp(opp_str, "hw") == 0) opp = STRAT_HANDWRITTEN;
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

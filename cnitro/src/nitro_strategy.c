// Nitro NN strategy. Direct port of neuralChooseMove in
// supabase/functions/_shared/strategies/nitro_strategy.ts. We don't port the
// heuristic fallback — when no NN params are loaded, this strategy can't
// play and the caller will pick something else.

#include "nitro_strategy.h"
#include "tokenize.h"
#include <string.h>

static const NNParams *g_params = NULL;
// Single-threaded inference cache — large to keep off the stack.
static ForwardCache g_fc;

void nitro_strategy_set_params(const NNParams *p) { g_params = p; }

static int neural_pick_action(const Game *g, int bot_idx,
                              const InProgress *ip,
                              const int *legal_actions, int n_legal) {
    Tokenized t;
    tokenize(g, bot_idx, ip, &t);
    nn_forward(g_params, t.tokens, t.n_tokens, &g_fc);
    bool mask[NUM_ACTIONS] = { false };
    for (int i = 0; i < n_legal; i++) mask[legal_actions[i]] = true;
    float probs[NUM_ACTIONS];
    nn_softmax_masked(g_fc.logits, mask, probs);
    int best = legal_actions[0];
    float best_p = -1e30f;
    for (int i = 0; i < n_legal; i++) {
        int a = legal_actions[i];
        if (probs[a] > best_p) { best_p = probs[a]; best = a; }
    }
    return best;
}

// Returns indices into moves of moves whose type==role and whose card list
// includes every card in `chosen` (regardless of order).
static int filter_prefix(const LegalMoves *moves, int role_move_type,
                         const Card *chosen, int n_chosen,
                         int *out_idx) {
    int n = 0;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != role_move_type) continue;
        if (m->n_cards < n_chosen) continue;
        bool ok = true;
        for (int j = 0; j < n_chosen && ok; j++) {
            bool found = false;
            for (int k = 0; k < m->n_cards; k++) {
                if (card_eq(m->cards[k], chosen[j])) { found = true; break; }
            }
            if (!found) ok = false;
        }
        if (ok) out_idx[n++] = i;
    }
    return n;
}

int nitro_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0 || g_params == NULL) return -1;
    int trump = g->power_suit;
    bool is_def = (bot_idx == g->defender);

    // Step 1: gather first-step legal actions.
    bool has_pickup = false, has_good = false;
    int  pickup_idx = -1, good_idx = -1;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PICKUP) { has_pickup = true; pickup_idx = i; }
        if (moves->moves[i].type == MOVE_GOOD)   { has_good = true; good_idx = i; }
    }

    int first_step_legal[NUM_ACTIONS]; int n_first = 0;
    int8_t first_step_role[NUM_ACTIONS];
    for (int i = 0; i < NUM_ACTIONS; i++) first_step_role[i] = -1;

    // Loop pools by type and collect each first-card action.
    for (int i = 0; i < moves->n; i++) {
        int t = moves->moves[i].type;
        int role_move_type;
        if (t == MOVE_ATTACK) role_move_type = MOVE_ATTACK;
        else if (t == MOVE_COVER) role_move_type = MOVE_COVER;
        else if (t == MOVE_PASS) role_move_type = MOVE_PASS;
        else continue;
        for (int j = 0; j < moves->moves[i].n_cards; j++) {
            Card c = moves->moves[i].cards[j];
            int a = card_action_id(c.suit, c.value, trump);
            bool seen = false;
            for (int k = 0; k < n_first; k++) if (first_step_legal[k] == a) { seen = true; break; }
            if (!seen) {
                first_step_legal[n_first++] = a;
                first_step_role[a] = (int8_t)role_move_type;
            }
        }
    }
    if (has_pickup) first_step_legal[n_first++] = ACTION_PICKUP;
    if (has_good)   first_step_legal[n_first++] = ACTION_STOP;

    if (n_first == 0) return 0;

    InProgress ip = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    int first_action = neural_pick_action(g, bot_idx, &ip, first_step_legal, n_first);

    if (first_action == ACTION_PICKUP) return pickup_idx;
    if (first_action == ACTION_STOP)   return has_good ? good_idx : 0;

    // Resolve role.
    int role_move_type;
    int8_t inferred = first_step_role[first_action];
    if (inferred < 0) role_move_type = is_def ? MOVE_COVER : MOVE_ATTACK;
    else if (is_def) {
        Card card; action_id_to_card(first_action, trump, &card);
        // Pass takes priority for defender, then cover.
        bool passable = false, coverable = false;
        for (int i = 0; i < moves->n; i++) {
            if (moves->moves[i].type == MOVE_PASS) {
                for (int j = 0; j < moves->moves[i].n_cards; j++)
                    if (card_eq(moves->moves[i].cards[j], card)) { passable = true; break; }
            }
            if (moves->moves[i].type == MOVE_COVER) {
                for (int j = 0; j < moves->moves[i].n_cards; j++)
                    if (card_eq(moves->moves[i].cards[j], card)) { coverable = true; break; }
            }
        }
        if (passable) role_move_type = MOVE_PASS;
        else if (coverable) role_move_type = MOVE_COVER;
        else role_move_type = inferred;
    } else {
        role_move_type = inferred;
    }

    Card first_card; action_id_to_card(first_action, trump, &first_card);
    ip.role = role_move_type == MOVE_ATTACK ? INPROG_ATTACK
            : role_move_type == MOVE_COVER  ? INPROG_COVER
            : role_move_type == MOVE_PASS   ? INPROG_PASS : INPROG_IDLE;
    ip.cards_chosen[0] = first_card;
    ip.n_cards_chosen = 1;

    int matching_idx[MAX_LEGAL_MOVES];

    // Step 2+: extend.
    while (true) {
        int n_matching = filter_prefix(moves, role_move_type, ip.cards_chosen, ip.n_cards_chosen, matching_idx);
        if (n_matching == 0) {
            for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == role_move_type) return i;
            return 0;
        }
        bool stop_ok = false;
        for (int i = 0; i < n_matching; i++) {
            if (moves->moves[matching_idx[i]].n_cards == ip.n_cards_chosen) { stop_ok = true; break; }
        }
        // Next-card legal candidates.
        int next_legal[NUM_ACTIONS]; int n_next = 0;
        bool seen[NUM_ACTIONS] = { false };
        for (int mi = 0; mi < n_matching; mi++) {
            const LegalMove *m = &moves->moves[matching_idx[mi]];
            if (m->n_cards <= ip.n_cards_chosen) continue;
            for (int j = 0; j < m->n_cards; j++) {
                Card c = m->cards[j];
                bool used = false;
                for (int k = 0; k < ip.n_cards_chosen; k++) {
                    if (card_eq(ip.cards_chosen[k], c)) { used = true; break; }
                }
                if (used) continue;
                int a = card_action_id(c.suit, c.value, trump);
                if (!seen[a]) { seen[a] = true; next_legal[n_next++] = a; }
            }
        }
        if (stop_ok) next_legal[n_next++] = ACTION_STOP;
        if (n_next == 0) return matching_idx[0];
        if (n_next == 1 && next_legal[0] == ACTION_STOP) {
            for (int i = 0; i < n_matching; i++) {
                if (moves->moves[matching_idx[i]].n_cards == ip.n_cards_chosen) return matching_idx[i];
            }
            return matching_idx[0];
        }
        int act = neural_pick_action(g, bot_idx, &ip, next_legal, n_next);
        if (act == ACTION_STOP) {
            for (int i = 0; i < n_matching; i++) {
                if (moves->moves[matching_idx[i]].n_cards == ip.n_cards_chosen) return matching_idx[i];
            }
            return matching_idx[0];
        }
        Card cd; action_id_to_card(act, trump, &cd);
        ip.cards_chosen[ip.n_cards_chosen++] = cd;
    }
}

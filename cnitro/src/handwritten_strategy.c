// Handwritten 1v1 strategy — port of HandwrittenBotStrategy in
// supabase/functions/_shared/strategies/handwritten_strategy.ts.
//
// The model: never done attacking, attack with as many cards as possible,
// avoid trump attacks while the deck (or flipped trump) still has cards,
// cover only when ALL uncovered attacks can be covered together (else
// pickup), prefer non-trump everywhere, choose lowest-value tie-breaks.
//
// MOVE_WAIT is referenced in the TS source but the C legal-move enumerator
// never emits it (calculate_legal_moves only produces ATTACK/COVER/PASS/
// PICKUP/GOOD), so all `wait` branches are dropped in this port.

#include "strategy.h"
#include "card.h"
#include "game.h"
#include <stdint.h>
#include <stddef.h>

static inline int card_score(Card c, int power_suit) {
    return c.value + (c.suit == power_suit ? 1000 : 0);
}

static int compute_total_card_count(const Game *g) {
    int table = 0;
    for (int i = 0; i < g->num_battles; i++) {
        table += 1 + (g->table_battles[i].has_defense ? 1 : 0);
    }
    int hands = 0;
    for (int i = 0; i < g->num_players; i++) hands += g->players[i].hand_count;
    return g->deck_count + g->discard_pile_length + table + hands + (g->has_flipped ? 1 : 0);
}

static double trump_attack_probability(const Game *g) {
    if (g->deck_count > 0 || g->has_flipped) return 0.02;
    int total = compute_total_card_count(g);
    if (total < 1) total = 1;
    double ratio = (double)g->discard_pile_length / total;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    double p = 0.65 + 0.35 * ratio;
    if (p < 0.5) p = 0.5;
    if (p > 0.95) p = 0.95;
    return p;
}

static bool move_has_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power_suit) return true;
    }
    return false;
}

static bool move_all_non_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power_suit) return false;
    }
    return true;
}

static int sum_card_score(const LegalMove *m, int power_suit) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += card_score(m->cards[i], power_suit);
    return s;
}

// Among `idxs[0..n)` (indices into moves->moves), find the one whose
// n_cards is maximal; among ties, lowest summed score. Returns chosen idx.
static int pick_max_cards_lowest_score(const LegalMoves *moves, int power_suit,
                                       const int *idxs, int n) {
    int max_n = -1;
    for (int i = 0; i < n; i++) {
        int nc = moves->moves[idxs[i]].n_cards;
        if (nc > max_n) max_n = nc;
    }
    int best = -1;
    int best_score = INT32_MAX;
    for (int i = 0; i < n; i++) {
        const LegalMove *m = &moves->moves[idxs[i]];
        if (m->n_cards != max_n) continue;
        int s = sum_card_score(m, power_suit);
        if (s < best_score) { best_score = s; best = idxs[i]; }
    }
    return best;
}

int handwritten_strategy_choose(const Game *g, int bot_idx,
                                const LegalMoves *moves, void *ctx) {
    (void)bot_idx; (void)ctx;
    if (moves->n == 0) return -1;
    int power = g->power_suit;

    int attacks[MAX_LEGAL_MOVES],   n_attacks = 0;
    int covers[MAX_LEGAL_MOVES],    n_covers = 0;
    int passes[MAX_LEGAL_MOVES],    n_passes = 0;
    int goods[MAX_LEGAL_MOVES],     n_goods = 0;
    int pickups[MAX_LEGAL_MOVES],   n_pickups = 0;
    for (int i = 0; i < moves->n; i++) {
        switch (moves->moves[i].type) {
            case MOVE_ATTACK: attacks[n_attacks++] = i; break;
            case MOVE_COVER:  covers[n_covers++]   = i; break;
            case MOVE_PASS:   passes[n_passes++]   = i; break;
            case MOVE_GOOD:   goods[n_goods++]     = i; break;
            case MOVE_PICKUP: pickups[n_pickups++] = i; break;
            default: break;
        }
    }

    // ---- Attack branch ----------------------------------------------
    if (n_attacks > 0) {
        int non_trump[MAX_LEGAL_MOVES]; int n_nt = 0;
        int trump[MAX_LEGAL_MOVES];     int n_tr = 0;
        for (int i = 0; i < n_attacks; i++) {
            const LegalMove *m = &moves->moves[attacks[i]];
            if (move_all_non_trump(m, power)) non_trump[n_nt++] = attacks[i];
            else if (move_has_trump(m, power)) trump[n_tr++] = attacks[i];
        }
        const int *candidates = NULL; int n_cand = 0;
        if (n_nt > 0) {
            candidates = non_trump; n_cand = n_nt;
        } else if (n_tr > 0) {
            if (game_random() < trump_attack_probability(g)) {
                candidates = trump; n_cand = n_tr;
            } else {
                // Decline trump attack: prefer GOOD (end round) over falling
                // through to pass/cover. (TS also checks `wait`, dropped.)
                if (n_goods > 0) return goods[0];
                // else fall through to non-attack branches
            }
        }
        if (candidates) {
            return pick_max_cards_lowest_score(moves, power, candidates, n_cand);
        }
    }

    // ---- Pass branch (lowest-value cards) ---------------------------
    if (n_passes > 0) {
        int best = passes[0];
        int best_score = INT32_MAX;
        for (int i = 0; i < n_passes; i++) {
            int s = sum_card_score(&moves->moves[passes[i]], power);
            if (s < best_score) { best_score = s; best = passes[i]; }
        }
        return best;
    }

    // ---- Cover branch — only if we can cover ALL uncovered attacks --
    if (n_covers > 0) {
        int uncovered = 0;
        for (int i = 0; i < g->num_battles; i++) {
            if (!g->table_battles[i].has_defense) uncovered++;
        }
        int full[MAX_LEGAL_MOVES]; int n_full = 0;
        for (int i = 0; i < n_covers; i++) {
            if (moves->moves[covers[i]].n_cards == uncovered) full[n_full++] = covers[i];
        }
        if (n_full > 0) {
            // PRODUCT of card scores (TS uses *=, matching the original aiDefend
            // logic: penalize using power cards much more than additively).
            int best = full[0];
            // Use double for the product to avoid overflow when scores can
            // exceed 1000 (trump bonus) over multiple cards.
            double best_score = 1e30;
            for (int i = 0; i < n_full; i++) {
                const LegalMove *m = &moves->moves[full[i]];
                double s = 1.0;
                for (int j = 0; j < m->n_cards; j++) s *= (double)card_score(m->cards[j], power);
                if (s < best_score) { best_score = s; best = full[i]; }
            }
            return best;
        }
        // Can't fully cover → fall through (no partial cover).
    }

    // ---- Non-attack/cover/pass/pickup moves: pick GOOD if available -
    if (n_goods > 0) {
        // The TS picks randomly among "non-attack non-pickup non-wait" — only
        // GOOD ever lands here for our legal-move set, so pick the first.
        int idx = (int)(game_random() * n_goods);
        if (idx < 0) idx = 0; if (idx >= n_goods) idx = n_goods - 1;
        return goods[idx];
    }

    // ---- Forced attack fallback -------------------------------------
    if (n_attacks > 0) {
        if (g->deck_count > 0 || g->has_flipped) {
            int nt[MAX_LEGAL_MOVES]; int nn = 0;
            for (int i = 0; i < n_attacks; i++) {
                if (move_all_non_trump(&moves->moves[attacks[i]], power)) nt[nn++] = attacks[i];
            }
            if (nn > 0) {
                // Prefer most cards, ties → lowest summed score.
                return pick_max_cards_lowest_score(moves, power, nt, nn);
            }
            if (n_goods > 0) return goods[0];
        }
        // No good fallback — pick most-cards, lowest-score among all attacks.
        return pick_max_cards_lowest_score(moves, power, attacks, n_attacks);
    }

    // ---- Pickup as absolute last resort -----------------------------
    if (n_pickups > 0) return pickups[0];

    // ---- Final fallback: random move (should be unreachable) --------
    int idx = (int)(game_random() * moves->n);
    if (idx < 0) idx = 0; if (idx >= moves->n) idx = moves->n - 1;
    return idx;
}

// Handwritten 1v1 strategy — port of HandwrittenBotStrategy in
// server/api/common/strategies/handwritten_strategy.ts.
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
        table += 1 + (!card_is_none(g->table_battles[i].defense) ? 1 : 0);
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

// (pick_max_cards_lowest_score removed — the "max n_cards, then lowest score"
// reduction it did over a stored index subset is now streamed inline via hw_mcl
// in handwritten_strategy_choose, so no per-category index array is built.)

// (handwritten_rollout_choose, the direct rollout chooser, is gone: the
// bitboard rollout in cordite_sim.c replaced it, and it had no caller left. It
// also emitted a transfer without asking game_pass_allowed, so it would have
// played the passing game at a podkidnoy table had anything called it.)

// "max n_cards, then lowest summed score, first-index tie-break" as a streaming
// reduction (M2-stream, docs/BOTS_WASM_MEMORY_PLAN.md): raising the running
// max_nc resets the best; an equal max_nc refines by score — one pass, identical
// result to the two-pass pick_max_cards_lowest_score.
typedef struct { int nc, score, idx; } HwMcl;   // nc<0 => empty
static inline void hw_mcl(HwMcl *a, int n_cards, int idx, int sum) {
    if (n_cards > a->nc) { a->nc = n_cards; a->score = sum; a->idx = idx; }
    else if (n_cards == a->nc && sum < a->score) { a->score = sum; a->idx = idx; }
}

int handwritten_strategy_choose(const Game *g, int bot_idx,
                                const LegalMoves *moves, void *ctx) {
    (void)bot_idx; (void)ctx;
    if (moves->n == 0) return -1;
    int power = g->power_suit;

    // M2-stream prototype: the original bucketed move INDICES into five
    // int[MAX_LEGAL_MOVES] arrays (+2 more in the attack branch) — a 112 KiB
    // stack frame at MAX_LEGAL_MOVES=4096 — only to run per-category argmax/
    // argmin reductions. Those reductions are computed here in ONE streaming
    // pass over `moves` with a handful of scalars: behavior-identical (same
    // selection, same branch order, same game_random() draw points), frame ~0.
    int uncovered = 0;
    for (int i = 0; i < g->num_battles; i++)
        if (!!card_is_none(g->table_battles[i].defense)) uncovered++;

    HwMcl atk = { -1, 0, -1 }, nt = { -1, 0, -1 }, tr = { -1, 0, -1 };
    int n_attacks = 0, n_nt = 0, n_tr = 0;
    int n_passes = 0, pass_best = -1, pass_best_score = INT32_MAX;
    int n_covers = 0, full_best = -1; double full_best_prod = 1e30;
    int n_goods = 0, first_good = -1;
    int n_pickups = 0, first_pickup = -1;

    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        switch (m->type) {
            case MOVE_ATTACK: {
                n_attacks++;
                int sum = sum_card_score(m, power);
                hw_mcl(&atk, m->n_cards, i, sum);
                if (move_all_non_trump(m, power)) { n_nt++; hw_mcl(&nt, m->n_cards, i, sum); }
                else if (move_has_trump(m, power)) { n_tr++; hw_mcl(&tr, m->n_cards, i, sum); }
                break;
            }
            case MOVE_COVER: {
                n_covers++;
                if (m->n_cards == uncovered) {
                    double s = 1.0;
                    for (int j = 0; j < m->n_cards; j++) s *= (double)card_score(m->cards[j], power);
                    if (s < full_best_prod) { full_best_prod = s; full_best = i; }
                }
                break;
            }
            case MOVE_PASS: {
                n_passes++;
                int s = sum_card_score(m, power);
                if (s < pass_best_score) { pass_best_score = s; pass_best = i; }
                break;
            }
            case MOVE_GOOD:   if (first_good < 0)   first_good = i;   n_goods++;   break;
            case MOVE_PICKUP: if (first_pickup < 0) first_pickup = i; n_pickups++; break;
            default: break;
        }
    }

    // ---- Attack branch ----------------------------------------------
    if (n_attacks > 0) {
        int cand = -1;
        if (n_nt > 0) {
            cand = nt.idx;
        } else if (n_tr > 0) {
            if (game_random() < trump_attack_probability(g)) {
                cand = tr.idx;
            } else {
                // Decline trump attack: prefer GOOD (end round) over falling
                // through to pass/cover. (TS also checks `wait`, dropped.)
                if (n_goods > 0) return first_good;
                // else fall through to non-attack branches
            }
        }
        if (cand >= 0) return cand;
    }

    // ---- Pass branch (lowest-value cards) ---------------------------
    if (n_passes > 0) return pass_best;

    // ---- Cover branch — only if we can cover ALL uncovered attacks --
    // (full_best is the lowest score-PRODUCT full cover; product penalizes
    // power cards multiplicatively, matching the original aiDefend logic.)
    if (n_covers > 0 && full_best >= 0) return full_best;
    // n_covers>0 but no full cover → fall through (no partial cover).

    // ---- Non-attack/cover/pass/pickup moves: pick GOOD if available -
    if (n_goods > 0) {
        // The TS picks randomly among "non-attack non-pickup non-wait" — only
        // GOOD ever lands here for our legal-move set. Consume the identical
        // draw, then walk to the idx-th GOOD (goods are rare; no stored list).
        int idx = (int)(game_random() * n_goods);
        if (idx < 0) idx = 0;
        if (idx >= n_goods) idx = n_goods - 1;
        int seen = 0;
        for (int i = 0; i < moves->n; i++)
            if (moves->moves[i].type == MOVE_GOOD) { if (seen == idx) return i; seen++; }
        return first_good;
    }

    // ---- Forced attack fallback -------------------------------------
    if (n_attacks > 0) {
        if (g->deck_count > 0 || g->has_flipped) {
            if (n_nt > 0) return nt.idx;     // most cards, ties → lowest score
            if (n_goods > 0) return first_good;
        }
        // No good fallback — most-cards, lowest-score among all attacks.
        return atk.idx;
    }

    // ---- Pickup as absolute last resort -----------------------------
    if (n_pickups > 0) return first_pickup;

    // ---- Final fallback: random move (should be unreachable) --------
    int idx = (int)(game_random() * moves->n);
    if (idx < 0) idx = 0;
    if (idx >= moves->n) idx = moves->n - 1;
    return idx;
}

// Hacker strategy — port of HackerStrategy in
// server/api/common/strategies/hacker_strategy.ts.
//
// Perfect-information "cheating" strategy: it inspects every player's hand
// (defender's hand for attack scoring, all other hands for cover safety).
// Fully deterministic — the TS source never calls Math.random(), so this
// port consumes zero RNG draws.
//
// The TS code sorts candidate (move, score) arrays with a stable sort on
// `a.score - b.score` and takes element [0]. That is exactly "earliest move
// (in enumeration order) with the strictly minimal score", so we use a
// first-min scan instead of materializing and sorting an array — provably
// identical to the stable sort + [0].
//
// selectOptimalPass in the TS computes the next defender's hand size, but
// BOTH branches return passMoves[0]; the computation only selects a log
// line. Dropped here (presentation only) — we return the first pass move.
//
// cardDisplay/console.log strings are presentation and are dropped; every
// computation feeding the choice is mirrored.

// strategy.h's parse_strategy needs strcmp, which the freestanding wasm
// libc stub doesn't declare — include the core headers directly instead.
#include "card.h"
#include "game.h"
#include "legal.h"

// Mirrors analyzeAttackEffectiveness: score an attack card against the
// defender's (fully visible) hand. Lower is better.
static int analyze_attack_effectiveness(Card attack_card, const Player *defender,
                                        int power_suit) {
    bool can_defender_cover = false;
    for (int i = 0; i < defender->hand_count; i++) {
        if (can_cover(attack_card, defender->hand[i], power_suit)) {
            can_defender_cover = true;
            break;
        }
    }
    bool can_defender_pass = false;
    for (int i = 0; i < defender->hand_count; i++) {
        if (defender->hand[i].value == attack_card.value) {
            can_defender_pass = true;
            break;
        }
    }

    // Perfect attack: can't cover AND can't pass.
    if (!can_defender_cover && !can_defender_pass) return 1;
    // Good attack: can't cover but can pass.
    if (!can_defender_cover && can_defender_pass) return 2;
    // Mediocre attack: coverable but not passable — cost of cheapest defense.
    if (can_defender_cover && !can_defender_pass) {
        // TS reduce keeps the FIRST minimum (strict <); only the value feeds
        // the score, so tracking the min value is enough.
        int cheapest = -1;
        for (int i = 0; i < defender->hand_count; i++) {
            if (!can_cover(attack_card, defender->hand[i], power_suit)) continue;
            if (cheapest < 0 || defender->hand[i].value < cheapest)
                cheapest = defender->hand[i].value;
        }
        return 10 + cheapest;
    }
    // Poor attack: both coverable and passable.
    return 50 + attack_card.value;
}

// Mirrors analyzeCoverSafety: score a cover card against the set of ranks
// held by all other players. Lower is better.
static int analyze_cover_safety(Card cover_card, uint32_t other_ranks_mask,
                                int power_suit) {
    bool is_trump = (cover_card.suit == power_suit);
    int card_rank = cover_card.value;
    bool is_safe_rank = (other_ranks_mask & (1u << card_rank)) == 0;

    if (is_safe_rank && !is_trump) return 1;   // safe non-trump
    if (is_safe_rank && is_trump)  return 2;   // safe trump
    if (!is_safe_rank && !is_trump) return 10 + card_rank; // risky non-trump
    return 20 + card_rank;                     // risky trump
}

// Mirrors shouldStrategicallyGiveUp: true when any uncovered attack is
// undefendable, or when the summed cheapest-defense cost exceeds 40% of the
// hand's total value. Arithmetic done in double exactly as the TS does
// (totalCost / ((sum/len) * len)).
static bool should_strategically_give_up(const Game *g, const Player *bot) {
    int n_uncovered = 0;
    int total_defense_cost = 0;
    int undefendable_count = 0;

    for (int b = 0; b < g->num_battles; b++) {
        if (!card_is_none(g->table_battles[b].defense)) continue;
        n_uncovered++;
        Card attack = g->table_battles[b].attack;
        int cheapest = -1;   // first strict min, as in the TS reduce
        for (int i = 0; i < bot->hand_count; i++) {
            if (!can_cover(attack, bot->hand[i], g->power_suit)) continue;
            if (cheapest < 0 || bot->hand[i].value < cheapest)
                cheapest = bot->hand[i].value;
        }
        if (cheapest < 0) undefendable_count++;
        else total_defense_cost += cheapest;
    }

    if (n_uncovered == 0) return false;

    // Give up if any attack is undefendable.
    if (undefendable_count > 0) return true;

    // undefendable_count == 0 with n_uncovered > 0 implies the hand is
    // non-empty (each uncovered attack found a cover card).
    double sum = 0.0;
    for (int i = 0; i < bot->hand_count; i++) sum += (double)bot->hand[i].value;
    double average_hand_value = sum / (double)bot->hand_count;
    double defense_cost_ratio =
        (double)total_defense_cost / (average_hand_value * (double)bot->hand_count);

    return defense_cost_ratio > 0.4;
}

int hacker_strategy_choose(const Game *g, int bot_idx,
                           const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;   // TS throws; no index to return.

    bool is_defender = (bot_idx == g->defender);

    // First index of each type (TS filter keeps enumeration order, and every
    // non-scored branch returns element [0] of its filtered list).
    int first_pass = -1, first_good = -1, first_wait = -1, first_pickup = -1;
    bool any_attack = false, any_cover = false;
    for (int i = 0; i < moves->n; i++) {
        switch (moves->moves[i].type) {
            case MOVE_ATTACK: any_attack = true; break;
            case MOVE_COVER:  any_cover = true; break;
            case MOVE_PASS:   if (first_pass < 0)   first_pass = i;   break;
            case MOVE_GOOD:   if (first_good < 0)   first_good = i;   break;
            case MOVE_WAIT:   if (first_wait < 0)   first_wait = i;   break;
            case MOVE_PICKUP: if (first_pickup < 0) first_pickup = i; break;
            default: break;
        }
    }

    // ---- Attack branch (selectOptimalAttack) -------------------------
    if (any_attack && !is_defender) {
        const Player *defender = &g->players[g->defender];
        int best = -1, best_score = 0;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_ATTACK) continue;
            // TS scores 1000 for a cardless move, else analyzes cards[0] only.
            int score = (m->n_cards == 0)
                ? 1000
                : analyze_attack_effectiveness(m->cards[0], defender,
                                               g->power_suit);
            if (best < 0 || score < best_score) { best = i; best_score = score; }
        }
        return best;
    }

    // ---- Defense branch (selectOptimalDefense) -----------------------
    // Entered only when cover moves exist AND we are the defender; a
    // defender without covers falls through to pass/good/wait/pickup below.
    if (any_cover && is_defender) {
        // Strategic pickup check comes first.
        if (should_strategically_give_up(g, &g->players[bot_idx])
                && first_pickup >= 0) {
            return first_pickup;
        }

        // selectOptimalCover: ranks held by every OTHER player (all seats,
        // regardless of status — OUT players just have empty hands).
        uint32_t other_ranks_mask = 0;
        for (int p = 0; p < g->num_players; p++) {
            if (p == bot_idx) continue;
            const Player *pl = &g->players[p];
            for (int i = 0; i < pl->hand_count; i++)
                other_ranks_mask |= 1u << pl->hand[i].value;
        }

        int best = -1, best_score = 0;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_COVER) continue;
            int score = (m->n_cards == 0)
                ? 1000
                : analyze_cover_safety(m->cards[0], other_ranks_mask,
                                       g->power_suit);
            if (best < 0 || score < best_score) { best = i; best_score = score; }
        }
        return best;
        // (TS falls back to pickup / legalMoves[0] only when coverMoves is
        // empty, which can't happen here — any_cover guards this branch.)
    }

    // ---- Pass (selectOptimalPass returns passMoves[0] either way) ----
    if (first_pass >= 0) return first_pass;

    // ---- Good --------------------------------------------------------
    if (first_good >= 0) return first_good;

    // ---- Wait ----------------------------------------------------------
    if (first_wait >= 0) return first_wait;

    // ---- Pickup as last resort ----------------------------------------
    if (first_pickup >= 0) return first_pickup;

    // ---- Final fallback: legalMoves[0] ---------------------------------
    return 0;
}

// Simple heuristic strategy — port of SimpleHeuristicStrategy in
// server/api/common/strategies/simple_heuristic_strategy.ts.
//
// The model: attack with the lowest-value non-trump cards (trump +20),
// defend with the cheapest cover (trump +10) unless a strategic give-up
// says to pick up, prefer passing (lowest value, trump +20), then good,
// wait, pickup, and finally legalMoves[0].
//
// The TS picks `filtered.sort(cmp)[0]` with an ascending-score comparator.
// Array.prototype.sort is stable, so that is exactly the first move (in
// original enumeration order) achieving the minimum score — implemented
// here as an argmin with strict `<` (first wins ties). The comparator's
// `if (!a.cards || !b.cards) return 0` guard can't fire (attack/cover/pass
// moves always carry cards) and is dropped.
//
// No RNG: the TS never calls Math.random().

// strategy.h is not included: its parse_strategy() inline needs strcmp,
// which the freestanding wasm shim <string.h> doesn't provide. game.h +
// legal.h carry everything this file uses, and the chooser signature below
// matches StrategyFn exactly.
#include "card.h"
#include "game.h"
#include "legal.h"

// getAttackPreference / getCardValue (identical bodies, trump_bonus 20) and
// getDefensePreference (trump_bonus 10): sum of value + bonus-if-trump.
static int sum_card_value(const LegalMove *m, int power_suit, int trump_bonus) {
    int total = 0;
    for (int i = 0; i < m->n_cards; i++) {
        total += m->cards[i].value
               + (m->cards[i].suit == power_suit ? trump_bonus : 0);
    }
    return total;
}

// shouldGiveUp(bot.hand, game): give up when we can't defend every
// uncovered attack, when defending would spend more than half our trumps,
// or when 3+ strong attacks face a weak hand.
static bool should_give_up(const Game *g, const Player *bot) {
    int power = g->power_suit;
    int n_uncovered = 0;
    int defendable = 0;
    int trumps_needed = 0;
    int attack_value_sum = 0;

    for (int i = 0; i < g->num_battles; i++) {
        if (!card_is_none(g->table_battles[i].defense)) continue;  // defense !== null
        Card attack = g->table_battles[i].attack;
        n_uncovered++;
        attack_value_sum += attack.value;

        // TS builds sameSuitCards / trumpCards filters but only tests
        // non-emptiness, so first match suffices (hand order irrelevant).
        bool can_defend = false;
        for (int j = 0; j < bot->hand_count; j++) {
            if (bot->hand[j].suit == attack.suit
                && bot->hand[j].value > attack.value) {
                can_defend = true;
                break;
            }
        }
        if (!can_defend) {
            for (int j = 0; j < bot->hand_count; j++) {
                if (bot->hand[j].suit == power
                    && can_cover(attack, bot->hand[j], power)) {
                    can_defend = true;
                    trumps_needed++;  // one per battle, like the TS filter
                    break;
                }
            }
        }
        if (can_defend) defendable++;
    }

    if (n_uncovered == 0) return false;

    // Give up if we can't defend all attacks.
    if (defendable < n_uncovered) return true;

    // Give up if we'd need too many trumps (TS: trumpsNeeded > trumpCount/2,
    // float division).
    int trump_count = 0;
    for (int j = 0; j < bot->hand_count; j++) {
        if (bot->hand[j].suit == power) trump_count++;
    }
    if ((double)trumps_needed > (double)trump_count / 2.0) return true;

    // Give up if attacks are too strong for a weak hand. bot->hand_count is
    // never 0 here (this is only reached from the cover branch, so the bot
    // holds at least one card) — the TS NaN-average case can't occur.
    double avg_attack = (double)attack_value_sum / (double)n_uncovered;
    int hand_value_sum = 0;
    for (int j = 0; j < bot->hand_count; j++) hand_value_sum += bot->hand[j].value;
    double avg_hand = (double)hand_value_sum / (double)bot->hand_count;
    if (avg_attack > avg_hand + 2.0 && n_uncovered >= 3) return true;

    return false;
}

// First move of the minimum summed score among moves of `type` — mirrors
// selectAttackMove / selectDefenseMove / selectPassMove (stable sort, [0]).
// Returns -1 when no move of that type exists.
static int argmin_by_type(const LegalMoves *moves, int8_t type,
                          int power_suit, int trump_bonus) {
    int best = -1;
    int best_score = 0;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type != type) continue;
        int s = sum_card_value(&moves->moves[i], power_suit, trump_bonus);
        if (best < 0 || s < best_score) {
            best = i;
            best_score = s;
        }
    }
    return best;
}

static int first_of_type(const LegalMoves *moves, int8_t type) {
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == type) return i;
    }
    return -1;
}

int simple_heuristic_strategy_choose(const Game *g, int bot_idx,
                                     const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;  // TS throws here; can't happen in play.

    int power = g->power_suit;
    bool is_defender = (bot_idx == g->defender);
    bool is_attacker = (bot_idx == g->first_attacker) || !is_defender;

    // Attack strategy: play lowest non-trump card (selectAttackMove).
    if (is_attacker) {
        int best = argmin_by_type(moves, MOVE_ATTACK, power, 20);
        if (best >= 0) return best;
    }

    // Defense strategy: minimal defense or strategic give-up.
    if (is_defender && first_of_type(moves, MOVE_COVER) >= 0) {
        if (should_give_up(g, &g->players[bot_idx])) {
            int pickup = first_of_type(moves, MOVE_PICKUP);
            if (pickup >= 0) return pickup;
        }
        return argmin_by_type(moves, MOVE_COVER, power, 10);
    }

    // Pass strategy: prefer passing when possible (selectPassMove, note the
    // TS scores passes with getCardValue — the same +20 trump bonus as
    // attacks, not the defense +10).
    {
        int best = argmin_by_type(moves, MOVE_PASS, power, 20);
        if (best >= 0) return best;
    }

    // Good moves.
    {
        int good = first_of_type(moves, MOVE_GOOD);
        if (good >= 0) return good;
    }

    // Wait when appropriate (the C enumerator never emits MOVE_WAIT, but
    // keep the branch to mirror the TS priority order exactly).
    {
        int wait = first_of_type(moves, MOVE_WAIT);
        if (wait >= 0) return wait;
    }

    // Pickup as last resort.
    {
        int pickup = first_of_type(moves, MOVE_PICKUP);
        if (pickup >= 0) return pickup;
    }

    // Default: first available move.
    return 0;
}

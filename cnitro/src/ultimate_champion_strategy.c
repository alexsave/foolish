// Ultimate Champion strategy — direct port of UltimateChampionStrategy in
// supabase/functions/_shared/strategies/ultimate_champion_strategy.ts.
//
// The TS never uses Array.sort here — every "best move" selection is a
// `reduce` that only replaces the incumbent on a STRICT `<` of the score, so
// ties keep the earliest move in legalMoves order. We mirror that with
// single-pass first-strict-min scans over the (identically ordered) moves
// list; the TS `filter`s are folded into predicates, which preserves order.
//
// Math.random() maps to random_strategy_random(); the number and order of
// draws is preserved exactly (see the per-site comments below).
//
// The TS class also declares coverage_consideration / duplicate_preference /
// positional_strategy, but chooseMove never reads them — omitted.

#include "card.h"
#include "game.h"
#include "legal.h"

// ---------- tuned parameters (verbatim from the TS class fields) --------

static const bool   trump_count_awareness = true;   // +1.1% effect
static const bool   prefer_passing = true;          // +0.9% effect

static const double bluff_attack_prob = 0.25;
static const double give_up_threshold = 0.95;
static const double trump_conservation_level = 0.60;
static const double risk_taking_propensity = 0.40;
static const double opponent_hand_weight = 0.90;
static const double deck_size_sensitivity = 0.65;
static const double attack_continuation_prob = 0.75;
static const double defense_desperation_threshold = 0.45;
static const double passing_aggressiveness = 0.25;
static const double endgame_strategy_switch = 0.50;

// ---------- move predicates (the TS filter lambdas) ----------------------
// All of these are only applied to attack/cover/pass moves, whose `cards`
// array always exists and is non-empty, so the TS `move.cards && ...` guards
// are vacuously true and not mirrored.

// move.cards.some(card => card.value >= 10)
static bool pred_any_value_ge10(const LegalMove *m, int power_suit) {
    (void)power_suit;
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].value >= 10) return true;
    return false;
}
// move.cards.every(card => card.suit !== power_suit)
static bool pred_all_non_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].suit == power_suit) return false;
    return true;
}
// move.cards.some(card => card.suit === power_suit)
static bool pred_any_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].suit == power_suit) return true;
    return false;
}
// move.cards.some(card => card.value >= 8 && card.value <= 11)
static bool pred_any_value_8_to_11(const LegalMove *m, int power_suit) {
    (void)power_suit;
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].value >= 8 && m->cards[i].value <= 11) return true;
    }
    return false;
}
// move.cards.every(card => card.value <= 8)
static bool pred_all_value_le8(const LegalMove *m, int power_suit) {
    (void)power_suit;
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].value > 8) return false;
    return true;
}

typedef bool (*MovePred)(const LegalMove *m, int power_suit);

// getAttackScore / getCardScore (trump_bonus = 20) and getDefenseScore
// (trump_bonus = 10) collapse to one sum.
static int score_cards(const LegalMove *m, int power_suit, int trump_bonus) {
    int score = 0;
    for (int i = 0; i < m->n_cards; i++) {
        score += m->cards[i].value + (m->cards[i].suit == power_suit ? trump_bonus : 0);
    }
    return score;
}

// The TS reduce pattern (getBestAttackMove / getLowestCostDefense /
// selectPassMove): first strict minimum over matching moves, in list order.
// Returns the move index, or -1 if no move matches (TS never reduces an
// empty filter result — callers check emptiness first).
static int reduce_lowest(const LegalMoves *moves, int type, MovePred pred,
                         int power_suit, int trump_bonus) {
    int best = -1;
    int best_score = 0;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != type) continue;
        if (pred && !pred(m, power_suit)) continue;
        int s = score_cards(m, power_suit, trump_bonus);
        if (best < 0 || s < best_score) { best = i; best_score = s; }
    }
    return best;
}

static int count_matching(const LegalMoves *moves, int type, MovePred pred,
                          int power_suit) {
    int n = 0;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type == type && (!pred || pred(m, power_suit))) n++;
    }
    return n;
}

// k-th (0-based) matching move, mirroring filtered[Math.floor(rand * len)].
static int nth_matching(const LegalMoves *moves, int type, MovePred pred,
                        int power_suit, int k) {
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type == type && (!pred || pred(m, power_suit))) {
            if (k == 0) return i;
            k--;
        }
    }
    return -1; // unreachable: k < count by construction
}

// Math.floor(Math.random() * n) for n >= 1: the draw is non-negative, so
// (int) truncation equals floor. Same IEEE ops as the TS.
static int random_index(int n) {
    int k = (int)(random_strategy_random() * (double)n);
    if (k >= n) k = n - 1; // unreachable guard (rand < 1.0)
    return k;
}

// ---------- shouldGiveUpBasic --------------------------------------------

static bool should_give_up_basic(const Game *g, const Player *bot) {
    // uncoveredAttacks = table_battles with defense === null.
    int uncovered = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (!g->table_battles[i].has_defense) uncovered++;
    }
    if (uncovered == 0) return false;

    int total_trump_cost = 0;
    int total_same_suit_cost = 0;
    int undefendable_attacks = 0;

    for (int i = 0; i < g->num_battles; i++) {
        if (g->table_battles[i].has_defense) continue;
        Card attack = g->table_battles[i].attack;

        // trumpOptions: trump cards that canCover; keep only the min value.
        bool has_trump_option = false;
        int min_trump = 0;
        // sameSuitOptions: same suit, strictly higher value; min value.
        bool has_same_suit = false;
        int min_same_suit = 0;
        for (int h = 0; h < bot->hand_count; h++) {
            Card c = bot->hand[h];
            if (c.suit == g->power_suit && can_cover(attack, c, g->power_suit)) {
                if (!has_trump_option || c.value < min_trump) min_trump = c.value;
                has_trump_option = true;
            }
            if (c.suit == attack.suit && c.value > attack.value) {
                if (!has_same_suit || c.value < min_same_suit) min_same_suit = c.value;
                has_same_suit = true;
            }
        }

        if (!has_trump_option && !has_same_suit) {
            undefendable_attacks++;
        } else if (has_same_suit) {
            total_same_suit_cost += min_same_suit; // can defend with same suit (preferred)
        } else {
            total_trump_cost += min_trump;         // must use trump
        }
    }

    // Give up if any attacks are undefendable.
    if (undefendable_attacks > 0) return true;

    // Give up if defense would consume too many high-value cards.
    if (uncovered >= 3 && (total_trump_cost + total_same_suit_cost) > 30) return true;

    // Give up if we'd have to use multiple high trumps.
    int high_trumps = 0;
    for (int h = 0; h < bot->hand_count; h++) {
        if (bot->hand[h].suit == g->power_suit && bot->hand[h].value >= 12) high_trumps++;
    }
    if (total_trump_cost >= 24 && high_trumps >= 2) return true;

    return false;
}

// ---------- ultimateGiveUpDecision ----------------------------------------

static bool ultimate_give_up_decision(const Game *g, const Player *bot) {
    bool base_should = should_give_up_basic(g, bot);

    // Give Up Threshold: one unconditional Math.random() draw.
    if (random_strategy_random() < (1.0 - give_up_threshold)) return true;

    // Deck Size Sensitivity (0.65 > 0.5: always active).
    if (deck_size_sensitivity > 0.5) {
        double deck_ratio = (double)g->deck_count / 36.0;
        if (deck_ratio > 0.5) {
            if (!base_should) return false;
        } else {
            if (base_should) return true;
        }
    }

    // Endgame Strategy Switch — dead in TS: 0.50 > 0.5 is false. Kept verbatim.
    if (bot->hand_count <= 3 && endgame_strategy_switch > 0.5) {
        int trump_cards = 0;
        for (int h = 0; h < bot->hand_count; h++) {
            if (bot->hand[h].suit == g->power_suit) trump_cards++;
        }
        if (trump_cards >= 2) return false;
    }

    return base_should;
}

// ---------- selectDefenseCard ---------------------------------------------

static int select_defense_card(const Game *g, const Player *bot,
                               const LegalMoves *moves) {
    // Trump Count Awareness (hard-coded true).
    if (trump_count_awareness) {
        int trump_count = 0;
        for (int h = 0; h < bot->hand_count; h++) {
            if (bot->hand[h].suit == g->power_suit) trump_count++;
        }
        double conservation_multiplier = 1.0 + trump_conservation_level;
        // Math.floor on positive doubles == (int) truncation.
        int many_trumps_threshold = (int)(4.0 * conservation_multiplier);
        int few_trumps_threshold = (int)(2.0 / conservation_multiplier);
        if (few_trumps_threshold < 1) few_trumps_threshold = 1; // Math.max(1, ...)

        if (trump_count >= many_trumps_threshold) {
            int idx = reduce_lowest(moves, MOVE_COVER, pred_any_trump, g->power_suit, 10);
            if (idx >= 0) return idx;
        }
        if (trump_count <= few_trumps_threshold) {
            int idx = reduce_lowest(moves, MOVE_COVER, pred_all_non_trump, g->power_suit, 10);
            if (idx >= 0) return idx;
        }
    }

    // Defense Desperation Threshold: one draw, only when hand is small.
    if (bot->hand_count <= 3) {
        double desperation_level = 1.0 - defense_desperation_threshold;
        if (random_strategy_random() < desperation_level) {
            int idx = reduce_lowest(moves, MOVE_COVER, pred_any_value_ge10, g->power_suit, 10);
            if (idx >= 0) return idx;
        }
    }

    // Risk Taking Propensity — dead in TS: 0.40 > 0.7 is false. Kept verbatim.
    // (The TS only draws Math.random() when trumpDefenses is non-empty.)
    if (risk_taking_propensity > 0.7) {
        int idx = reduce_lowest(moves, MOVE_COVER, pred_any_trump, g->power_suit, 10);
        if (idx >= 0 && random_strategy_random() < 0.4) return idx;
    }

    // Default: minimal defense.
    return reduce_lowest(moves, MOVE_COVER, NULL, g->power_suit, 10);
}

// ---------- selectDefenseMove ----------------------------------------------

static int select_defense_move(const Game *g, int bot_idx, const LegalMoves *moves) {
    const Player *bot = &g->players[bot_idx];

    int uncovered = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (!g->table_battles[i].has_defense) uncovered++;
    }
    if (uncovered > 0) {
        if (ultimate_give_up_decision(g, bot)) {
            for (int i = 0; i < moves->n; i++) {
                if (moves->moves[i].type == MOVE_PICKUP) return i; // strategic pickup
            }
        }
    }

    int idx = select_defense_card(g, bot, moves);
    if (idx >= 0) return idx;

    return 0; // default fallback: legalMoves[0]
}

// ---------- selectAttackMove ------------------------------------------------

static int select_attack_move(const Game *g, const LegalMoves *moves) {
    // (The TS also reads the defender's hand size here but never uses it.)

    // Bluff Attack Probability: one unconditional draw; a second draw only
    // when the bluff fires AND a >=10 attack exists.
    if (random_strategy_random() < bluff_attack_prob) {
        int n = count_matching(moves, MOVE_ATTACK, pred_any_value_ge10, g->power_suit);
        if (n > 0) {
            return nth_matching(moves, MOVE_ATTACK, pred_any_value_ge10,
                                g->power_suit, random_index(n));
        }
    }

    // Trump Conservation Level (0.60 > 0.5: always active).
    if (trump_conservation_level > 0.5) {
        int idx = reduce_lowest(moves, MOVE_ATTACK, pred_all_non_trump, g->power_suit, 20);
        if (idx >= 0) return idx;
    }

    // Risk Taking Propensity — both arms dead in TS (0.40). Kept verbatim.
    if (risk_taking_propensity > 0.7) {
        int n = count_matching(moves, MOVE_ATTACK, pred_any_value_8_to_11, g->power_suit);
        if (n > 0) {
            return nth_matching(moves, MOVE_ATTACK, pred_any_value_8_to_11,
                                g->power_suit, random_index(n));
        }
    } else if (risk_taking_propensity < 0.3) {
        int idx = reduce_lowest(moves, MOVE_ATTACK, pred_all_value_le8, g->power_suit, 20);
        if (idx >= 0) return idx;
    }

    // Default: smart attack preference.
    return reduce_lowest(moves, MOVE_ATTACK, NULL, g->power_suit, 20);
}

// ---------- chooseMove --------------------------------------------------------

int ultimate_champion_strategy_choose(const Game *g, int bot_idx,
                                      const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1; // TS throws

    bool is_defender = (bot_idx == g->defender);
    bool is_attacker = (bot_idx == g->first_attacker) || !is_defender;

    // One scan for the filters the TS builds up front / lazily.
    int first_good = -1, first_pass = -1, first_wait = -1, first_pickup = -1;
    bool has_attack = false, has_cover = false;
    for (int i = 0; i < moves->n; i++) {
        switch (moves->moves[i].type) {
        case MOVE_GOOD:   if (first_good < 0) first_good = i;     break;
        case MOVE_ATTACK: has_attack = true;                      break;
        case MOVE_COVER:  has_cover = true;                       break;
        case MOVE_PASS:   if (first_pass < 0) first_pass = i;     break;
        case MOVE_WAIT:   if (first_wait < 0) first_wait = i;     break;
        case MOVE_PICKUP: if (first_pickup < 0) first_pickup = i; break;
        default: break;
        }
    }

    // Attack Continuation Probability via "good": one draw when a good move
    // exists and there is at least one battle on the table.
    if (first_good >= 0 && g->num_battles >= 1) {
        double continue_prob = attack_continuation_prob;
        int defender_hand_size = g->players[g->defender].hand_count;

        // Opponent Hand Size Weight (0.90 > 0.5: always active).
        if (opponent_hand_weight > 0.5) {
            if (defender_hand_size <= 2) {
                continue_prob *= 1.5; // more pressure against weak opponents
            } else if (defender_hand_size >= 5) {
                continue_prob *= 0.7; // less pressure against strong opponents
            }
        }

        if (random_strategy_random() > continue_prob) return first_good;
    }

    // Attack strategy.
    if (has_attack && is_attacker) return select_attack_move(g, moves);

    // Defense strategy.
    if (has_cover && is_defender) return select_defense_move(g, bot_idx, moves);

    // Pass strategy (prefer_passing hard-coded true).
    if (prefer_passing) {
        if (first_pass >= 0) {
            // Passing Aggressiveness: one draw when a pass move exists.
            double pass_chance = 0.3 + (passing_aggressiveness * 0.4);
            if (random_strategy_random() < pass_chance) {
                // TS: (game.defender + 1) % game.players.length — includes
                // OUT players; the `if (nextDefender && ...)` existence check
                // is always true since the index is in range.
                int next_defender = (g->defender + 1) % g->num_players;
                if (g->players[next_defender].hand_count
                        <= g->players[bot_idx].hand_count) {
                    // selectPassMove: lowest getCardScore (trump +20).
                    return reduce_lowest(moves, MOVE_PASS, NULL, g->power_suit, 20);
                }
            }
        }
    }

    // Good moves (if not chosen earlier probabilistically).
    if (first_good >= 0) return first_good;

    // Wait when appropriate (TS also console.logs here — side effect only).
    if (first_wait >= 0) return first_wait;

    // Pickup as last resort.
    if (first_pickup >= 0) return first_pickup;

    // Default: first available move.
    return 0;
}

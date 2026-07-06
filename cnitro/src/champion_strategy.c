// Champion strategy — direct port of ChampionStrategy.chooseMove in
// supabase/functions/_shared/strategies/champion_strategy.ts.
//
// Behavioral notes (this must match the TS move-for-move):
// - Math.random() maps to random_strategy_random(). There are exactly two
//   call sites: the early-good gate in chooseMove (consumed only when a good
//   move exists AND table_battles.length >= 1, mirroring the JS &&
//   short-circuit) and conservativeGiveUpDecision (consumed on every call,
//   after shouldGiveUpBasic which itself uses no RNG).
// - The TS .sort(cmp) calls (sortByAttackPreference, the default branch of
//   selectDefenseCard) are stable; we run a stable insertion sort over move
//   indices with the identical score-ascending comparator and take element 0,
//   exactly like the TS.
// - The TS .reduce() minimum pickers (selectPassMove, getLowestRankCard) keep
//   the earlier element on ties (strict <); mirrored with first-min scans.
// - The TS `!move.cards` comparator/reducer guards can never fire (every move
//   type they see carries cards) and are not modeled.

#include "card.h"
#include "game.h"
#include "legal.h"

int champion_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

// Strategy parameters (ChampionStrategy readonly fields). The TS also
// declares ATTACK_CONTINUATION_PROB (0.75), TRUMP_CONSERVATION_LEVEL (0.70)
// and RISK_TAKING_PROPENSITY (0.35) but never reads them; omitted here.
#define CHAMPION_GIVE_UP_THRESHOLD       0.92
#define CHAMPION_EARLY_ATTACK_END_CHANCE 0.30
#define CHAMPION_OPPONENT_HAND_WEIGHT    0.90

// ---------- score helpers (all integer-valued in the TS) ----------------

// getAttackPreferenceScore: lower = better to attack with.
static int get_attack_preference_score(const LegalMove *m, int trump) {
    int score = 0;
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == trump) score += m->cards[i].value + 20;
        else score += m->cards[i].value;
    }
    return score;
}

// getDefenseScore: prefer non-trump, then lower ranks.
static int get_defense_score(const LegalMove *m, int trump) {
    int score = 0;
    for (int i = 0; i < m->n_cards; i++) {
        score += m->cards[i].value + (m->cards[i].suit == trump ? 15 : 0);
    }
    return score;
}

// getCardScore.
static int get_card_score(const LegalMove *m, int trump) {
    int score = 0;
    for (int i = 0; i < m->n_cards; i++) {
        score += m->cards[i].value + (m->cards[i].suit == trump ? 20 : 0);
    }
    return score;
}

// cardBeats.
static bool card_beats(Card card, Card target, int trump) {
    return (card.suit == target.suit && card.value > target.value)
        || (card.suit == trump && target.suit != trump);
}

// Stable insertion sort of move indices by score ascending. Elements move
// only past strictly-greater scores, so equal scores keep their original
// order — same as the TS stable .sort((a, b) => aScore - bScore).
static void stable_sort_by_score(int *idx, int *score, int n) {
    for (int i = 1; i < n; i++) {
        int id = idx[i], sc = score[i];
        int j = i - 1;
        while (j >= 0 && score[j] > sc) {
            idx[j + 1] = idx[j];
            score[j + 1] = score[j];
            j--;
        }
        idx[j + 1] = id;
        score[j + 1] = sc;
    }
}

// filterAggressiveAttacks predicate: cards.some(v >= 8 && v <= 11).
static bool has_medium_card(const LegalMove *m) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].value >= 8 && m->cards[i].value <= 11) return true;
    }
    return false;
}

// filterConservativeAttacks predicate: cards.every(v <= 8).
static bool all_low_cards(const LegalMove *m) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].value > 8) return false;
    }
    return true;
}

// ---------- selectAttackMove ---------------------------------------------

static int select_attack_move(const Game *g, const LegalMoves *moves) {
    int defender_hand_size = g->players[g->defender].hand_count;

    // Opponent strength assessment. OPPONENT_HAND_WEIGHT > 0.5 always holds,
    // kept literal for fidelity. filter mode: 0 = all attacks, 1 = aggressive
    // (some card 8..11), 2 = conservative (every card <= 8). Like the TS
    // ternaries, an empty filter result falls back to all attack moves.
    int use_filter = 0;
    if (CHAMPION_OPPONENT_HAND_WEIGHT > 0.5) {
        if (defender_hand_size <= 3) use_filter = 1;
        else if (defender_hand_size >= 6) use_filter = 2;
    }
    if (use_filter != 0) {
        bool nonempty = false;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_ATTACK) continue;
            if ((use_filter == 1 && has_medium_card(m))
                || (use_filter == 2 && all_low_cards(m))) {
                nonempty = true;
                break;
            }
        }
        if (!nonempty) use_filter = 0;
    }

    // sortByAttackPreference over the adjusted moves; return sorted[0].
    int idx[MAX_LEGAL_MOVES];
    int score[MAX_LEGAL_MOVES];
    int n = 0;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != MOVE_ATTACK) continue;
        if (use_filter == 1 && !has_medium_card(m)) continue;
        if (use_filter == 2 && !all_low_cards(m)) continue;
        idx[n] = i;
        score[n] = get_attack_preference_score(m, g->power_suit);
        n++;
    }
    stable_sort_by_score(idx, score, n);
    return idx[0];   // caller guarantees at least one attack move
}

// ---------- selectPassMove -------------------------------------------------

// reduce keeping the lowest getCardScore; strict < keeps the earlier move.
static int select_pass_move(const Game *g, const LegalMoves *moves) {
    int best = -1;
    int best_score = 0;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type != MOVE_PASS) continue;
        int s = get_card_score(&moves->moves[i], g->power_suit);
        if (best < 0 || s < best_score) {
            best = i;
            best_score = s;
        }
    }
    return best;   // caller guarantees at least one pass move
}

// ---------- give-up logic --------------------------------------------------

static bool should_give_up_basic(const Game *g, const Player *bot) {
    int uncovered = 0;
    int total_trump_cost = 0;
    int total_same_suit_cost = 0;
    int undefendable = 0;

    for (int b = 0; b < g->num_battles; b++) {
        if (!card_is_none(g->table_battles[b].defense)) continue;   // uncovered only
        uncovered++;
        Card attack = g->table_battles[b].attack;
        bool has_trump_opt = false;
        int min_trump = 0;
        bool has_same = false;
        int min_same = 0;
        for (int i = 0; i < bot->hand_count; i++) {
            Card c = bot->hand[i];
            if (c.suit == g->power_suit && card_beats(c, attack, g->power_suit)) {
                if (!has_trump_opt || c.value < min_trump) min_trump = c.value;
                has_trump_opt = true;
            }
            // Note: a trump attack counts its higher-trump answers as
            // "same suit" (the preferred branch), exactly like the TS.
            if (c.suit == attack.suit && c.value > attack.value) {
                if (!has_same || c.value < min_same) min_same = c.value;
                has_same = true;
            }
        }
        if (!has_trump_opt && !has_same) undefendable++;
        else if (has_same) total_same_suit_cost += min_same;
        else total_trump_cost += min_trump;
    }

    if (uncovered == 0) return false;
    if (undefendable > 0) return true;
    if (uncovered >= 3 && total_trump_cost + total_same_suit_cost > 30) return true;

    int high_trumps = 0;
    for (int i = 0; i < bot->hand_count; i++) {
        if (bot->hand[i].suit == g->power_suit && bot->hand[i].value >= 12) high_trumps++;
    }
    if (total_trump_cost >= 24 && high_trumps >= 2) return true;

    return false;
}

static bool conservative_give_up_decision(const Game *g, const Player *bot) {
    bool base_give_up = should_give_up_basic(g, bot);

    // Ultra-conservative threshold: 8% unconditional give-up. RNG consumed
    // on every call, after the (RNG-free) base computation.
    if (random_strategy_random() < (1.0 - CHAMPION_GIVE_UP_THRESHOLD)) return true;

    if (!base_give_up) return false;

    int trump_cards = 0;
    for (int i = 0; i < bot->hand_count; i++) {
        if (bot->hand[i].suit == g->power_suit) trump_cards++;
    }
    if (trump_cards >= 3) return false;
    if (g->deck_count > 10) return false;

    int defender_hand_size = g->players[g->defender].hand_count;
    if (defender_hand_size <= 3 && trump_cards >= 2) return false;

    return base_give_up;   // true here
}

// ---------- selectDefenseCard -----------------------------------------------

static bool move_some_trump(const LegalMove *m, int trump) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == trump) return true;
    }
    return false;
}

static bool move_every_non_trump(const LegalMove *m, int trump) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == trump) return false;
    }
    return true;
}

static int select_defense_card(const Game *g, const Player *bot, const LegalMoves *moves) {
    int trump_count = 0;
    for (int i = 0; i < bot->hand_count; i++) {
        if (bot->hand[i].suit == g->power_suit) trump_count++;
    }

    // Many trumps: willing to spend one. getLowestRankCard = reduce keeping
    // the lowest getCardScore (strict <, earlier move wins ties).
    if (trump_count >= 4) {
        int best = -1;
        int best_score = 0;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_COVER || !move_some_trump(m, g->power_suit)) continue;
            int s = get_card_score(m, g->power_suit);
            if (best < 0 || s < best_score) {
                best = i;
                best_score = s;
            }
        }
        if (best >= 0) return best;
    }

    // Few trumps: avoid them if possible.
    if (trump_count <= 1) {
        int best = -1;
        int best_score = 0;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_COVER || !move_every_non_trump(m, g->power_suit)) continue;
            int s = get_card_score(m, g->power_suit);
            if (best < 0 || s < best_score) {
                best = i;
                best_score = s;
            }
        }
        if (best >= 0) return best;
    }

    // Default: stable sort of the cover moves by getDefenseScore; take [0].
    int idx[MAX_LEGAL_MOVES];
    int score[MAX_LEGAL_MOVES];
    int n = 0;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type != MOVE_COVER) continue;
        idx[n] = i;
        score[n] = get_defense_score(&moves->moves[i], g->power_suit);
        n++;
    }
    stable_sort_by_score(idx, score, n);
    return idx[0];   // caller guarantees at least one cover move
}

// ---------- selectDefenseMove -------------------------------------------------

static int select_defense_move(const Game *g, int bot_idx, const LegalMoves *moves) {
    const Player *bot = &g->players[bot_idx];

    int uncovered = 0;
    for (int b = 0; b < g->num_battles; b++) {
        if (!!card_is_none(g->table_battles[b].defense)) uncovered++;
    }

    // Conservative give-up decision (only evaluated with uncovered attacks).
    if (uncovered > 0) {
        if (conservative_give_up_decision(g, bot)) {
            for (int i = 0; i < moves->n; i++) {
                if (moves->moves[i].type == MOVE_PICKUP) return i;
            }
        }
    }

    // Defense with trump count awareness.
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_COVER) {
            return select_defense_card(g, bot, moves);
        }
    }

    // Consider passing if the next defender has similar or fewer cards.
    bool any_pass = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PASS) {
            any_pass = true;
            break;
        }
    }
    if (any_pass) {
        int next_defender = get_next_player_index(g, g->defender);
        int next_defender_hand_size = g->players[next_defender].hand_count;
        if (next_defender_hand_size <= bot->hand_count) {
            return select_pass_move(g, moves);
        }
    }

    // Default: first available move.
    return 0;
}

// ---------- chooseMove ---------------------------------------------------

int champion_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;   // TS throws here; no index to return

    bool is_defender = (bot_idx == g->defender);
    bool is_attacker = (bot_idx == g->first_attacker) || !is_defender;

    int first_good = -1;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_GOOD) {
            first_good = i;
            break;
        }
    }

    // Early attack ending: 30% chance to say good. C's && short-circuits
    // like the JS, so the RNG draw only happens when both guards hold.
    if (first_good >= 0 && g->num_battles >= 1
        && random_strategy_random() < CHAMPION_EARLY_ATTACK_END_CHANCE) {
        return first_good;
    }

    // Attack strategy.
    bool any_attack = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_ATTACK) {
            any_attack = true;
            break;
        }
    }
    if (any_attack && is_attacker) return select_attack_move(g, moves);

    // Defense strategy.
    if (is_defender) return select_defense_move(g, bot_idx, moves);

    // Pass strategy.
    bool any_pass = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PASS) {
            any_pass = true;
            break;
        }
    }
    if (any_pass) return select_pass_move(g, moves);

    // Good (if not chosen earlier).
    if (first_good >= 0) return first_good;

    // Wait when appropriate.
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_WAIT) return i;
    }

    // Pickup as last resort.
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PICKUP) return i;
    }

    // Default: first available move.
    return 0;
}

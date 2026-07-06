// Handwritten PRODUCTION strategy — exact port of HandwrittenBotStrategy in
// supabase/functions/_shared/strategies/handwritten_strategy.ts
// (STRAT_HANDWRITTEN_PROD). Verified move-for-move against the TS original
// on identical positions with a pinned RNG stream.
//
// NOT the same as handwritten_strategy.c: that file is the arena/rollout
// variant (cordite's Monte-Carlo rollout policy and its cordite_sim.c
// bitboard mirror are tuned and frozen against it) and drifted from the TS
// production bot — see strategy.h. This file mirrors the TS only.
//
// Behavioral notes (this must match the TS move-for-move):
// - Math.random() maps to random_strategy_random(). Three call sites, in
//   control-flow order, at most one draw each per decision:
//   (1) shouldAttackWithTrump — consumed only when attack moves exist, none
//       is all-non-trump, and at least one contains a trump. The probability
//       is computed before the draw and uses no RNG itself; the decline
//       branch's `.toFixed(2)` recomputation is RNG-free presentation and is
//       dropped.
//   (2) the random index into the "other non-attack" (in practice: good)
//       moves — consumed even when only one such move exists;
//   (3) the final random fallback. A single decision can consume (1) then
//       (2), or (1) then (3), exactly like the TS control flow.
// - getTrumpAttackProbability's early exit reads `game.deck_length`, a
//   projection field nothing updates after DB load ("unused but necessary
//   for type", utils.ts): the wasm engine's applyStateToGame never writes it
//   and the parity harness pins it at 0 for the whole game (mkGame in
//   e2e/bot_parity.test.ts). The disjunction `deck_length > 0 || flipped !==
//   null` therefore reduces to the flipped test, mirrored as g->has_flipped
//   alone — deliberately NOT g->deck_count, which would diverge whenever the
//   stale TS field (0) and the live deck disagree. (In every reachable
//   kernel state deck_count > 0 implies has_flipped anyway: the flipped
//   trump is drawn last.)
// - The forced-attack fallback reads the LIVE `game.deck.length`, mirrored
//   as g->deck_count.
// - Every TS .sort(cmp) here is stable and only sorted[0] is read:
//   sort-desc-by-count + [0] = first move (enumeration order) with the max
//   card count — note the early-deck nonTrumpFallback applies NO score
//   tie-break; sort by (count desc, score asc) + [0] = first strict-min
//   summed score among the max-count moves. Mirrored as first-match scans;
//   no sort is materialized.
// - The reduce-style minimum pickers (maxCardMoves, passMoves,
//   findBestCoverCombination) start at Infinity with strict <, so the first
//   candidate always displaces the seed and ties keep the earliest move;
//   mirrored as first-strict-min scans.
// - findBestCoverCombination multiplies integer card scores in JS DOUBLE
//   arithmetic (full covers can exceed 2^53 and round); mirrored with the
//   identical left-to-right double product. Its null result is impossible
//   once fullCoverMoves is non-empty (every element passes the type/cards
//   guard), so `best >= 0` is the exact gate.
// - `move.cards` truthiness guards never fire (attack/pass/cover moves
//   always carry cards) and cards.every(non-trump) is vacuously true on an
//   empty array; both are modeled by the natural n_cards loops.
// - Math.floor(random() * len) on len >= 1 → (int) cast, NO clamp (the TS
//   has none; random() < 1 keeps the index in range).
// - Dropped as presentation only: the bot lookup by player_id (feeds log
//   strings and findBestCoverCombination's unused botPlayer parameter) and
//   every console.log.

// strategy.h's parse_strategy needs strcmp, which the freestanding wasm
// libc stub doesn't declare — include the core headers directly instead.
#include "card.h"
#include "game.h"
#include "legal.h"

int handwritten_prod_strategy_choose(const Game *g, int bot_idx,
                                     const LegalMoves *moves, void *ctx);

// cardScore: value + (1000 if power suit).
static int card_score(Card c, int power_suit) {
    return c.value + (c.suit == power_suit ? 1000 : 0);
}

// cards.every(c => c.suit !== game.power_suit); vacuously true when empty.
static bool every_non_trump(const LegalMove *m, int power) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power) return false;
    }
    return true;
}

// cards.some(c => c.suit === game.power_suit).
static bool some_trump(const LegalMove *m, int power) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power) return true;
    }
    return false;
}

// Summed cardScore of a move's cards (integers throughout; exact in the TS
// doubles too, so int comparison is identical).
static int sum_card_score(const LegalMove *m, int power) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += card_score(m->cards[i], power);
    return s;
}

// computeTotalCardCount: deck + discard + table (1 per attack + 1 per
// defense) + every player's hand (including OUT players' empty hands) +
// flipped (1 if present).
static int compute_total_card_count(const Game *g) {
    int table = 0;
    for (int i = 0; i < g->num_battles; i++) {
        table += 1 + (!card_is_none(g->table_battles[i].defense) ? 1 : 0);
    }
    int hands = 0;
    for (int i = 0; i < g->num_players; i++) hands += g->players[i].hand_count;
    return g->deck_count + g->discard_pile_length + table + hands
         + (g->has_flipped ? 1 : 0);
}

// getTrumpAttackProbability. Uses no RNG.
static double trump_attack_probability(const Game *g) {
    // TS: `if (game.deck_length > 0 || game.flipped !== null) return 0.02;`
    // — deck_length is the stale projection field (see header), so this is
    // the flipped test alone.
    if (g->has_flipped) return 0.02;

    // Endgame: deck exhausted, flipped taken.
    int total = compute_total_card_count(g);   // Math.max(1, ...)
    if (total < 1) total = 1;
    double ratio = (double)g->discard_pile_length / (double)total;
    if (ratio > 1.0) ratio = 1.0;              // Math.min(1, ...)
    if (ratio < 0.0) ratio = 0.0;              // Math.max(0, ...)
    double p = 0.65 + 0.35 * ratio;
    if (p > 0.95) p = 0.95;                    // Math.min(0.95, ...)
    if (p < 0.5)  p = 0.5;                     // Math.max(0.5, ...)
    return p;
}

// Attack-move classes: ALL = continueAttackMoves/doneAttackMoves (every
// MOVE_ATTACK), NON_TRUMP = cards all non-trump, TRUMP = some card trump.
// NON_TRUMP and TRUMP partition the attacks (every attack has cards).
#define ATK_ALL       0
#define ATK_NON_TRUMP 1
#define ATK_TRUMP     2

static bool attack_matches(const LegalMove *m, int power, int cls) {
    if (m->type != MOVE_ATTACK) return false;
    if (cls == ATK_NON_TRUMP) return every_non_trump(m, power);
    if (cls == ATK_TRUMP)     return some_trump(m, power);
    return true;
}

// score_tiebreak=false mirrors `arr.sort((a,b) => bLen - aLen)` + [0]: the
// stable sort keeps enumeration order among equal counts, so [0] is the
// FIRST move with the maximum card count. score_tiebreak=true additionally
// mirrors the reduce over maxCardMoves (and the (count desc, score asc)
// comparator of the final doneAttackMoves sort): first strict-min summed
// cardScore among the max-count moves. Caller guarantees a match exists.
static int pick_attack(const Game *g, const LegalMoves *moves,
                       int cls, bool score_tiebreak) {
    int power = g->power_suit;
    int max_cards = -1;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (attack_matches(m, power, cls) && m->n_cards > max_cards) {
            max_cards = m->n_cards;
        }
    }
    int best = -1;
    int best_score = 0;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (!attack_matches(m, power, cls) || m->n_cards != max_cards) continue;
        if (!score_tiebreak) return i;
        int s = sum_card_score(m, power);
        if (best < 0 || s < best_score) {
            best = i;
            best_score = s;
        }
    }
    return best;
}

// "Other" moves for the random-pick branch: everything that is not attack /
// cover / pass / pickup / wait — in practice only MOVE_GOOD, but the
// predicate is kept literal.
static bool is_other_move(const LegalMove *m) {
    return m->type != MOVE_ATTACK && m->type != MOVE_COVER
        && m->type != MOVE_PASS && m->type != MOVE_PICKUP
        && m->type != MOVE_WAIT;
}

// ---------- chooseMove ------------------------------------------------------

int handwritten_prod_strategy_choose(const Game *g, int bot_idx,
                                     const LegalMoves *moves, void *ctx) {
    (void)bot_idx; (void)ctx;
    if (moves->n == 0) return -1;   // TS throws here; no index to return
    int power = g->power_suit;

    bool any_attack = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_ATTACK) { any_attack = true; break; }
    }

    // ---- continue-attack branch: prefer non-trump; gate trump by prob ----
    if (any_attack) {
        bool any_non_trump = false;
        bool any_trump = false;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_ATTACK) continue;
            if (every_non_trump(m, power)) any_non_trump = true;
            if (some_trump(m, power))      any_trump = true;
        }

        int cls = -1;
        if (any_non_trump) {
            cls = ATK_NON_TRUMP;
        } else if (any_trump) {
            // shouldAttackWithTrump: probability computed first (RNG-free),
            // then exactly one Math.random() draw.
            if (random_strategy_random() < trump_attack_probability(g)) {
                cls = ATK_TRUMP;
            } else {
                // Decline the trump attack: prefer good (end round), then
                // wait; otherwise fall through to the pass/cover logic.
                for (int i = 0; i < moves->n; i++) {
                    if (moves->moves[i].type == MOVE_GOOD) return i;
                }
                for (int i = 0; i < moves->n; i++) {
                    if (moves->moves[i].type == MOVE_WAIT) return i;
                }
            }
        }
        if (cls >= 0) return pick_attack(g, moves, cls, true);
    }

    // ---- pass branch: first strict-min summed cardScore -------------------
    {
        int best = -1;
        int best_score = 0;
        for (int i = 0; i < moves->n; i++) {
            if (moves->moves[i].type != MOVE_PASS) continue;
            int s = sum_card_score(&moves->moves[i], power);
            if (best < 0 || s < best_score) {
                best = i;
                best_score = s;
            }
        }
        if (best >= 0) return best;
    }

    // ---- cover branch: only if we can cover ALL uncovered attacks ---------
    {
        int uncovered = 0;
        for (int b = 0; b < g->num_battles; b++) {
            if (!!card_is_none(g->table_battles[b].defense)) uncovered++;
        }
        // fullCoverMoves: attack_cards.length === uncovered; the enumerator
        // pairs one attack card per cover card, so that length is n_cards.
        // findBestCoverCombination: first strict-min PRODUCT of card scores,
        // accumulated left-to-right in double like the JS.
        int best = -1;
        double best_score = 0.0;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_COVER || m->n_cards != uncovered) continue;
            double s = 1.0;
            for (int j = 0; j < m->n_cards; j++) {
                s *= (double)card_score(m->cards[j], power);
            }
            if (best < 0 || s < best_score) {
                best = i;
                best_score = s;
            }
        }
        if (best >= 0) return best;
        // No full cover → never cover partially; fall through.
    }

    // ---- prefer wait over the remaining moves ------------------------------
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_WAIT) return i;
    }

    // ---- other non-attack moves (good): random pick ------------------------
    {
        int count = 0;
        for (int i = 0; i < moves->n; i++) {
            if (is_other_move(&moves->moves[i])) count++;
        }
        if (count > 0) {
            // Math.floor(Math.random() * count): draw consumed even when
            // count == 1. random() < 1 keeps k in [0, count-1]; no clamp.
            int k = (int)(random_strategy_random() * (double)count);
            for (int i = 0; i < moves->n; i++) {
                if (!is_other_move(&moves->moves[i])) continue;
                if (k == 0) return i;
                k--;
            }
        }
    }

    // ---- forced ("done") attack fallback -----------------------------------
    if (any_attack) {
        // Re-applied no-early-trump rule reads the LIVE deck here.
        if (g->deck_count > 0 || g->has_flipped) {
            bool any_non_trump = false;
            for (int i = 0; i < moves->n; i++) {
                if (attack_matches(&moves->moves[i], power, ATK_NON_TRUMP)) {
                    any_non_trump = true;
                    break;
                }
            }
            if (any_non_trump) {
                // Sorted by count desc only — NO score tie-break: the first
                // (enumeration order) max-count non-trump attack.
                return pick_attack(g, moves, ATK_NON_TRUMP, false);
            }
            // Prefer good/wait instead of trump in the early phase; if
            // neither exists, fall through to the unrestricted sort below.
            for (int i = 0; i < moves->n; i++) {
                if (moves->moves[i].type == MOVE_GOOD) return i;
            }
            for (int i = 0; i < moves->n; i++) {
                if (moves->moves[i].type == MOVE_WAIT) return i;
            }
        }
        // (count desc, score asc) stable sort + [0].
        return pick_attack(g, moves, ATK_ALL, true);
    }

    // ---- pickup as absolute last resort -------------------------------------
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PICKUP) return i;
    }

    // ---- final fallback: random move (TS: "should never reach here") --------
    return (int)(random_strategy_random() * (double)moves->n);
}

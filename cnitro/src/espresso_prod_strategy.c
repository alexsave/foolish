// Espresso (production) strategy — exact behavioral mirror of
// EspressoStrategy in
// supabase/functions/_shared/strategies/espresso_strategy.ts.
//
// NOTE: this is NOT src/espresso_strategy.c. That file is the arena /
// cordite-rollout espresso (tuned and frozen — cordite's Monte-Carlo
// behavior depends on it); it drifted from the TS production bot. This
// file mirrors the production TS move-for-move.
//
// Behavioral notes (this must match the TS move-for-move):
// - Math.random() maps to random_strategy_random(). Call sites, in order:
//     1. choose1v1Logic trump-attack gate: `allowTrumpAttack ||
//        Math.random() < getTrumpAttackProbability(game)` — one draw
//        consumed only when the JS || short-circuit reaches it (i.e. no
//        non-trump attacks exist, some trump attack exists, and the
//        opponent cannot cover our lowest attack trump).
//     2. tryRandomTweaks aggressive-trump gate: one draw consumed whenever
//        that statement is reached (mixed attacks empty, non-trump attacks
//        empty, trump attacks present, bot not defender), regardless of
//        whether the gate then passes.
//     3. choose1v1Logic final fallback: Math.floor(Math.random() * n).
//   getTrumpAttackProbability / rolloutRound / predictCover are RNG-free.
//   The handwritten fallback (hw.chooseMove) consumes its own draws inside
//   handwritten_prod_strategy_choose, after tryRandomTweaks' draw — same
//   order as the TS.
// - Discard memory: the TS keeps `private memory = new Map<gameId,
//   {seenCards}>` — PERSISTENT static state that lives for the strategy
//   instance's lifetime, is fed by rescanning ALL of game.logs on every
//   chooseMove (updateDiscardMemory), and is NEVER cleared, not even
//   between games. The kernel is not told the game id, so this mirror
//   keeps one persistent set (a suit x value grid), OR-accumulated from
//   the logs at the top of every choose: exactly the TS behavior whenever
//   every decision served by one kernel instance carries the same game id.
//   That is what the parity harness exercises (all its games share id
//   'botparity', and the TS side demonstrably carries seen cards across
//   games — rebuilding from the current game's logs alone diverges). With
//   unique per-game ids the TS starts each game with a fresh set; the
//   kernel cannot observe that without game-id plumbing.
//   `if (pair.primary)` maps to a suit >= 0 guard (hidden/absent cards
//   travel as {-1,-1}).
// - getDeckCards loops `value <= 14` although the real max value is
//   ACE_VALUE = 13, so four phantom value-14 cards (never in any hand, so
//   never "known") always land in the reconstructed deck and inflate
//   stillInPlay / disposedUtility. Ported verbatim.
// - Every TS .sort(cmp) here is consumed only through element [0] or a
//   "top count" filter:
//     * doneAttacks.sort(count desc, scoreSum asc)[0] == first move (in
//       enumeration order) with the max count, min sum among those — a
//       first-strict-best two-key scan is provably identical to the stable
//       sort (same trick as hacker_strategy.c).
//     * tryPositionBias / tryRandomTweaks sort by count desc, filter the
//       max-count prefix, then reduce to the first strict-min sum. A stable
//       sort preserves enumeration order inside the max-count class, so
//       "max count, then first strict-min sum in enumeration order" is
//       identical.
// - TS .reduce / manual min loops keep the FIRST strict minimum (strict <
//   / strict >); mirrored with first-match scans. Best-move loops that TS
//   seeds with element [0] and -Infinity/Infinity sentinels are mirrored
//   with a take-first flag: the first (finite) eval always beats the
//   sentinels, and element [0] can never strictly beat itself.
// - Rollout attack grouping iterates a Map in insertion order == order of
//   first occurrence of each value in `matching`; mirrored with a
//   first-occurrence scan.
// - `strategy_key === 'random'` maps to strategy_key == 0 (STRAT_RANDOM in
//   strategy.h / STRAT.random in wasm/bots.ts). strategy.h itself is not
//   included: its parse_strategy needs strcmp, absent from the wasm shim.
// - All eval arithmetic is done in double with the TS's left-to-right
//   term order; counts/sums/scores are exact small integers so the double
//   ==/</> comparisons match JS exactly.
// - console.log output is presentation and is dropped; every computation
//   feeding a choice is mirrored.

#include "card.h"
#include "game.h"
#include "legal.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// The production handwritten mirror (chooseNP's `this.hw.chooseMove`
// fallback). Prototype matches strategy.h.
int handwritten_prod_strategy_choose(const Game *g, int bot_idx,
                                     const LegalMoves *moves, void *ctx);

int espresso_prod_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx);

// strategy_key id of the TS 'random' strategy (STRAT_RANDOM).
#define EP_KEY_RANDOM 0

// Card values are 1..13 (plus the phantom 14 in the deck reconstruction),
// so a [NUM_SUITS][16] grid indexed by (suit, value) models a Set of keys.
#define EP_VAL_SLOTS 16

// ---------- small helpers ----------------------------------------------

// cardScore.
static inline int ep_card_score(Card c, int power_suit) {
    return c.value + (c.suit == power_suit ? 1000 : 0);
}

// computeTotalCardCount.
static int ep_total_card_count(const Game *g) {
    int table = 0;
    for (int i = 0; i < g->num_battles; i++) {
        table += 1 + (g->table_battles[i].has_defense ? 1 : 0);
    }
    int hands = 0;
    for (int i = 0; i < g->num_players; i++) hands += g->players[i].hand_count;
    return g->deck_count + g->discard_pile_length + table + hands
         + (g->has_flipped ? 1 : 0);
}

// getTrumpAttackProbability (RNG-free).
static double ep_trump_attack_probability(const Game *g) {
    if (g->deck_count > 0 || g->has_flipped) return 0.02;
    int total = ep_total_card_count(g);
    if (total < 1) total = 1;                      // Math.max(1, ...)
    double ratio = (double)g->discard_pile_length / (double)total;
    if (ratio < 0.0) ratio = 0.0;                  // Math.max(0, Math.min(1, ...))
    if (ratio > 1.0) ratio = 1.0;
    double p = 0.65 + 0.35 * ratio;
    if (p > 0.95) p = 0.95;                        // Math.min(0.95, ...)
    if (p < 0.5) p = 0.5;                          // Math.max(0.5, ...)
    return p;
}

// getOpponent — returns a seat index, or -1 for null. The TS `defender &&`
// / `firstAttacker &&` truthy guards map to index range checks.
static int ep_get_opponent(const Game *g, int bot_idx) {
    int in_count = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) in_count++;
    }
    if (in_count == 0) return -1;
    if (g->defender >= 0 && g->defender < g->num_players
        && g->defender != bot_idx
        && g->players[g->defender].status == PLAYER_STATUS_IN) {
        return g->defender;
    }
    if (g->first_attacker >= 0 && g->first_attacker < g->num_players
        && g->first_attacker != bot_idx
        && g->players[g->first_attacker].status == PLAYER_STATUS_IN) {
        return g->first_attacker;
    }
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) return i;
    }
    return -1;
}

// getMem(game.id).seenCards — the TS strategy instance's persistent,
// never-cleared discard memory (see the header note on why a single set is
// the exact realizable mirror without game-id plumbing). _Thread_local to
// match the engine's RNG statics; the wasm build strips it.
static _Thread_local bool ep_seen_mem[NUM_SUITS][EP_VAL_SLOTS];

// Game-id plumbing: the TS kept one seen-set PER game id (a Map on the
// strategy instance), so two different games never shared memory. The kernel
// serves many games from one instance; the bridge hashes game.id and calls
// this before each decision, and a key change resets the set (an LRU of one:
// sequential games match the TS exactly; INTERLEAVED games restart their
// memory on each switch where the TS would resume — memory only sharpens
// tie-breaks, and that beats cross-game contamination). Callers that never
// set a key (native arena) keep the single persistent set.
static _Thread_local uint32_t ep_game_key = 0;
void espresso_prod_set_game_key(uint32_t key);
void espresso_prod_set_game_key(uint32_t key) {
    if (key == ep_game_key) return;
    ep_game_key = key;
    memset(ep_seen_mem, 0, sizeof ep_seen_mem);
}

// updateDiscardMemory: add every discard-log primary to the persistent set.
// Only ever adds — exactly like the TS.
static void ep_update_discard_memory(const Game *g) {
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type != LOG_DISCARD) continue;
        for (int j = 0; j < g->logs[i].num_pairs; j++) {
            Card p = g->logs[i].pairs[j].primary;
            // TS: `if (pair.primary)` — hidden/absent primaries are {-1,-1}.
            if (p.suit >= 0 && p.suit < NUM_SUITS
                && p.value >= 0 && p.value < EP_VAL_SLOTS) {
                ep_seen_mem[p.suit][p.value] = true;
            }
        }
    }
}

// countSeenTrumps: distinct seen cards with the power suit.
static int ep_count_seen_trumps(const Game *g) {
    int ps = g->power_suit;
    if (ps < 0 || ps >= NUM_SUITS) return 0;
    int count = 0;
    for (int v = 0; v < EP_VAL_SLOTS; v++) {
        if (ep_seen_mem[ps][v]) count++;
    }
    return count;
}

// ---------- predictCover (greedy lowest-score cover per attack) --------

// covers->n <= opponent hand size, so MAX_HAND_SIZE bounds it.
typedef struct {
    Card cards[MAX_HAND_SIZE];
    int  n;
} EpCardList;

static bool ep_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static void ep_predict_cover(const Card *attack, int attack_n,
                             const Card *opp_hand, int opp_n,
                             int power_suit,
                             EpCardList *covers, bool *pickup) {
    Card remaining[MAX_HAND_SIZE];
    int rn = opp_n;
    for (int i = 0; i < opp_n; i++) remaining[i] = opp_hand[i];
    covers->n = 0;
    *pickup = false;
    for (int a = 0; a < attack_n; a++) {
        int best_idx = -1;
        int best_score = INT32_MAX;   // Infinity; strict < keeps the earliest
        for (int i = 0; i < rn; i++) {
            if (can_cover(attack[a], remaining[i], power_suit)) {
                int s = ep_card_score(remaining[i], power_suit);
                if (s < best_score) { best_score = s; best_idx = i; }
            }
        }
        if (best_idx == -1) {
            covers->n = 0;            // TS returns coverCards: []
            *pickup = true;
            return;
        }
        covers->cards[covers->n++] = remaining[best_idx];
        for (int i = best_idx + 1; i < rn; i++) remaining[i - 1] = remaining[i];
        rn--;                          // splice(bestIdx, 1)
    }
}

// ---------- rolloutRound ------------------------------------------------

typedef struct {
    Card my_hand[MAX_HAND_SIZE];
    int  my_n;
    Card opp_hand[MAX_HAND_SIZE * 2];  // pickup appends the attack cards
    int  opp_n;
    bool pickup;
} EpRollout;

static void ep_rollout_round(const Card *first_attack, int fa_n,
                             const Card *my_hand, int my_n,
                             const Card *opp_hand, int opp_n,
                             int power_suit, EpRollout *r) {
    Card my[MAX_HAND_SIZE];
    int mN = my_n;
    for (int i = 0; i < my_n; i++) my[i] = my_hand[i];
    Card opp[MAX_HAND_SIZE * 2];
    int oN = opp_n;
    for (int i = 0; i < opp_n; i++) opp[i] = opp_hand[i];

    bool table_values[EP_VAL_SLOTS] = { false };

    Card attack[MAX_MOVE_CARDS > 8 ? MAX_MOVE_CARDS : 8];
    int an = fa_n;
    for (int i = 0; i < fa_n; i++) attack[i] = first_attack[i];

    for (int iter = 0; iter < 5; iter++) {   // maxIters default 5
        // myH = myH.filter(not in attackKeys)
        int new_mN = 0;
        for (int i = 0; i < mN; i++) {
            if (!ep_contains(attack, an, my[i])) my[new_mN++] = my[i];
        }
        mN = new_mN;
        for (int i = 0; i < an; i++) table_values[attack[i].value] = true;

        EpCardList covers;
        bool pickup;
        ep_predict_cover(attack, an, opp, oN, power_suit, &covers, &pickup);
        if (pickup) {
            for (int i = 0; i < an; i++) opp[oN++] = attack[i];
            r->pickup = true;
            goto done;
        }
        // oppH = oppH.filter(not in coverKeys)
        int new_oN = 0;
        for (int i = 0; i < oN; i++) {
            if (!ep_contains(covers.cards, covers.n, opp[i])) opp[new_oN++] = opp[i];
        }
        oN = new_oN;
        for (int i = 0; i < covers.n; i++) table_values[covers.cards[i].value] = true;

        // matching = myH.filter(value on table && non-trump), kept in order.
        Card matching[MAX_HAND_SIZE];
        int mn = 0;
        for (int i = 0; i < mN; i++) {
            if (table_values[my[i].value] && my[i].suit != power_suit)
                matching[mn++] = my[i];
        }
        if (mn == 0) { r->pickup = false; goto done; }

        // Group by value; Map insertion order == first occurrence order in
        // `matching`. Pick the largest group, ties by smaller value sum
        // (strict, so the earlier-inserted group wins equal (count, sum)).
        bool group_done[EP_VAL_SLOTS] = { false };
        Card best_group[NUM_SUITS];
        int best_n = 0;              // bestCount = 0
        int best_sum = INT32_MAX;    // bestSum = Infinity
        for (int i = 0; i < mn; i++) {
            int v = matching[i].value;
            if (group_done[v]) continue;
            group_done[v] = true;
            Card group[NUM_SUITS];
            int gn = 0, sum = 0;
            for (int j = 0; j < mn; j++) {
                if (matching[j].value == v) { group[gn++] = matching[j]; sum += v; }
            }
            if (gn > best_n || (gn == best_n && sum < best_sum)) {
                best_n = gn;
                best_sum = sum;
                for (int j = 0; j < gn; j++) best_group[j] = group[j];
            }
        }
        // attackCards = bestGroup.slice(0, oppH.length)
        an = best_n < oN ? best_n : oN;
        for (int i = 0; i < an; i++) attack[i] = best_group[i];
        if (an == 0) { r->pickup = false; goto done; }
    }
    r->pickup = false;
done:
    r->my_n = mN;
    r->opp_n = oN;
    for (int i = 0; i < mN; i++) r->my_hand[i] = my[i];
    for (int i = 0; i < oN; i++) r->opp_hand[i] = opp[i];
}

// ---------- move predicates ---------------------------------------------

// nonTrumpAttacks predicate: cards.every(suit !== power_suit).
static bool ep_attack_all_non_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power_suit) return false;
    }
    return true;
}

// trumpAttacks predicate: cards.some(suit === power_suit).
static bool ep_attack_any_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power_suit) return true;
    }
    return false;
}

// mixed-attack half-predicate: cards.some(suit !== power_suit).
static bool ep_attack_any_non_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit != power_suit) return true;
    }
    return false;
}

static int ep_score_sum(const LegalMove *m, int power_suit) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += ep_card_score(m->cards[i], power_suit);
    return s;
}

static int ep_value_sum(const LegalMove *m) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += m->cards[i].value;
    return s;
}

// ---------- choose1v1Logic ------------------------------------------------

static int ep_choose_1v1(const Game *g, int bot_idx, const LegalMoves *moves) {
    const Player *bot = &g->players[bot_idx];
    int opp_seat = ep_get_opponent(g, bot_idx);
    const Player *opp = (opp_seat >= 0) ? &g->players[opp_seat] : NULL;
    int ps = g->power_suit;

    // ---- continueAttackMoves --------------------------------------------
    int n_attack = 0, n_non_trump = 0, n_trump = 0;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != MOVE_ATTACK) continue;
        n_attack++;
        if (ep_attack_all_non_trump(m, ps)) n_non_trump++;
        else n_trump++;   // some(trump) is the complement of every(non-trump)
    }

    // candidate_mode: 0 = none, 1 = nonTrumpAttacks, 2 = trumpAttacks.
    int candidate_mode = 0;
    if (n_attack > 0) {
        if (n_non_trump > 0) {
            candidate_mode = 1;
        } else if (n_trump > 0) {
            bool allow_trump_attack = false;
            if (opp) {
                // myLowestTrumpInAttack = Math.min over the trump cards of
                // every trump-attack move (flatMap + filter + map).
                int my_lowest = INT32_MAX;
                for (int i = 0; i < moves->n; i++) {
                    const LegalMove *m = &moves->moves[i];
                    if (m->type != MOVE_ATTACK || !ep_attack_any_trump(m, ps)) continue;
                    for (int j = 0; j < m->n_cards; j++) {
                        if (m->cards[j].suit == ps && m->cards[j].value < my_lowest)
                            my_lowest = m->cards[j].value;
                    }
                }
                for (int j = 0; j < opp->hand_count; j++) {
                    if (opp->hand[j].suit == ps && opp->hand[j].value > my_lowest) {
                        allow_trump_attack = true;
                        break;
                    }
                }
            }
            // `allowTrumpAttack || Math.random() < prob` — the draw happens
            // only when the || short-circuit reaches it.
            if (allow_trump_attack
                || random_strategy_random() < ep_trump_attack_probability(g)) {
                candidate_mode = 2;
            } else {
                for (int i = 0; i < moves->n; i++) {
                    if (moves->moves[i].type == MOVE_GOOD) return i;  // goodMoves[0]
                }
                // No good move: candidateMoves stays empty; fall through to
                // the pass/cover/... cascade below, exactly like the TS.
            }
        }
    }

    if (candidate_mode != 0) {
        // passWindow = opponent && table_battles.every(defense === null)
        bool pass_window = (opp != NULL);
        if (pass_window) {
            for (int i = 0; i < g->num_battles; i++) {
                if (g->table_battles[i].has_defense) { pass_window = false; break; }
            }
        }
        // inOpps / minOppHand / leader (first in-opponent at the minimum).
        bool has_in_opp = false;
        int min_opp_hand = 0;
        for (int i = 0; i < g->num_players; i++) {
            if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
            if (!has_in_opp || g->players[i].hand_count < min_opp_hand)
                min_opp_hand = g->players[i].hand_count;
            has_in_opp = true;
        }
        // minOppHand is Infinity when there are no in-opps; every use below
        // is guarded by has_in_opp (== comparisons against Infinity are false).
        bool defender_is_leader = has_in_opp
            && g->players[g->defender].hand_count == min_opp_hand
            && g->players[g->defender].status == PLAYER_STATUS_IN
            && g->defender != bot_idx;
        int leader_seat = -1;   // inOpps.find(hand.length === minOppHand) ?? null
        if (has_in_opp) {
            for (int i = 0; i < g->num_players; i++) {
                if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
                if (g->players[i].hand_count == min_opp_hand) { leader_seat = i; break; }
            }
        }
        bool leader_is_attacker = (leader_seat >= 0 && leader_seat != g->defender);
        bool deck_active = (g->deck_count > 0 || g->has_flipped);

        // TS seeds best/bestEval/bestCount/bestSum from evals[0], then loops
        // over ALL evals; element [0] can never strictly beat itself, so a
        // take-first scan is identical (rollouts still run in move order).
        int best = -1;
        double best_eval = 0.0;
        int best_count = 0;
        int best_sum = 0;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_ATTACK) continue;
            bool any_trump = ep_attack_any_trump(m, ps);
            if (candidate_mode == 1 ? any_trump : !any_trump) continue;

            // evalMove: {eval: 0, passable: false} when no opponent.
            bool passable = false;
            double e = 0.0;
            if (opp) {
                int v = m->cards[0].value;
                if (pass_window) {
                    for (int j = 0; j < opp->hand_count; j++) {
                        if (opp->hand[j].value == v) { passable = true; break; }
                    }
                }
                EpRollout r;
                ep_rollout_round(m->cards, m->n_cards, bot->hand, bot->hand_count,
                                 opp->hand, opp->hand_count, ps, &r);
                int my_trumps = 0, opp_trumps = 0;
                for (int j = 0; j < r.my_n; j++)
                    if (r.my_hand[j].suit == ps) my_trumps++;
                for (int j = 0; j < r.opp_n; j++)
                    if (r.opp_hand[j].suit == ps) opp_trumps++;
                double size_weight = (r.pickup || !deck_active) ? 1.0 : 0.0;
                double pickup_bonus = r.pickup ? 3.0 : 0.0;
                double block_leader_bonus = (r.pickup && defender_is_leader) ? 4.0 : 0.0;
                double leader_pile_on_penalty = 0.0;
                if (leader_is_attacker) {
                    int my_v = m->cards[0].value;
                    int leader_matches = 0;
                    const Player *leader = &g->players[leader_seat];
                    for (int j = 0; j < leader->hand_count; j++)
                        if (leader->hand[j].value == my_v) leader_matches++;
                    leader_pile_on_penalty = (double)leader_matches * 0.7;
                }
                e = size_weight * (double)(r.opp_n - r.my_n)
                  + 1.5 * (double)(my_trumps - opp_trumps)
                  + pickup_bonus
                  + block_leader_bonus
                  - leader_pile_on_penalty;
            }
            double final_eval = passable ? e - 1000.0 : e;
            int cnt = m->n_cards;
            int sum = ep_score_sum(m, ps);
            bool better = (best < 0)
                || final_eval > best_eval
                || (final_eval == best_eval && cnt > best_count)
                || (final_eval == best_eval && cnt == best_count && sum < best_sum);
            if (better) {
                best = i;
                best_eval = final_eval;
                best_count = cnt;
                best_sum = sum;
            }
        }
        return best;   // candidateMoves nonempty by construction
    }

    // ---- passMoves --------------------------------------------------------
    int first_pass = -1;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PASS) { first_pass = i; break; }
    }
    if (first_pass >= 0 && opp) {
        // best seeded with passMoves[0] / -Infinity: the first (finite) eval
        // always wins, then strict > keeps the earliest maximum.
        int best = -1;
        double best_eval = 0.0;
        Card all_attacks[MAX_BATTLES + MAX_MOVE_CARDS];
        for (int i = first_pass; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_PASS) continue;
            int an = 0;
            for (int j = 0; j < g->num_battles; j++)
                all_attacks[an++] = g->table_battles[j].attack;
            for (int j = 0; j < m->n_cards; j++)
                all_attacks[an++] = m->cards[j];
            EpCardList covers;
            bool pickup;
            ep_predict_cover(all_attacks, an, opp->hand, opp->hand_count, ps,
                             &covers, &pickup);
            // myH = bot.hand minus the passed cards.
            int my_n = 0;
            Card myH[MAX_HAND_SIZE];
            for (int j = 0; j < bot->hand_count; j++) {
                if (!ep_contains(m->cards, m->n_cards, bot->hand[j]))
                    myH[my_n++] = bot->hand[j];
            }
            Card oppH[MAX_HAND_SIZE + MAX_BATTLES + MAX_MOVE_CARDS];
            int opp_n = 0;
            if (pickup) {
                for (int j = 0; j < opp->hand_count; j++) oppH[opp_n++] = opp->hand[j];
                for (int j = 0; j < an; j++) oppH[opp_n++] = all_attacks[j];
            } else {
                for (int j = 0; j < opp->hand_count; j++) {
                    if (!ep_contains(covers.cards, covers.n, opp->hand[j]))
                        oppH[opp_n++] = opp->hand[j];
                }
            }
            int my_t = 0, opp_t = 0;
            for (int j = 0; j < my_n; j++) if (myH[j].suit == ps) my_t++;
            for (int j = 0; j < opp_n; j++) if (oppH[j].suit == ps) opp_t++;
            bool deck_active = (g->deck_count > 0 || g->has_flipped);
            double size_weight = (pickup || !deck_active) ? 1.0 : 0.0;
            double pickup_bonus = pickup ? 3.0 : 0.0;
            double e = size_weight * (double)(opp_n - my_n)
                     + 1.5 * (double)(my_t - opp_t)
                     + pickup_bonus;
            if (best < 0 || e > best_eval) { best_eval = e; best = i; }
        }
        return best;
    }
    if (first_pass >= 0) return first_pass;

    // ---- coverMoves ---------------------------------------------------------
    bool any_cover = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_COVER) { any_cover = true; break; }
    }
    if (any_cover) {
        int uncovered = 0;
        for (int i = 0; i < g->num_battles; i++)
            if (!g->table_battles[i].has_defense) uncovered++;
        // fullCovers: attack_cards.length === uncovered (n_cards counts the
        // cover/attack pairs of a cover move).
        bool any_full = false;
        for (int i = 0; i < moves->n; i++) {
            if (moves->moves[i].type == MOVE_COVER
                && moves->moves[i].n_cards == uncovered) { any_full = true; break; }
        }
        if (any_full) {
            // known = every hand (all seats, any status) + table + flipped +
            // seenCards; deck = the complement over suit 0..3, value
            // startValue..14 (verbatim TS bound — value 14 is phantom).
            bool known[NUM_SUITS][EP_VAL_SLOTS];
            // Seed with the persistent discard memory (mem.seenCards).
            for (int s = 0; s < NUM_SUITS; s++)
                for (int v = 0; v < EP_VAL_SLOTS; v++)
                    known[s][v] = ep_seen_mem[s][v];
            for (int i = 0; i < g->num_players; i++) {
                for (int j = 0; j < g->players[i].hand_count; j++) {
                    Card c = g->players[i].hand[j];
                    known[c.suit][c.value] = true;
                }
            }
            for (int i = 0; i < g->num_battles; i++) {
                Card a = g->table_battles[i].attack;
                known[a.suit][a.value] = true;
                if (g->table_battles[i].has_defense) {
                    Card d = g->table_battles[i].defense;
                    known[d.suit][d.value] = true;
                }
            }
            if (g->has_flipped) known[g->flipped.suit][g->flipped.value] = true;

            int start_value = g->num_players >= 6 ? 1 : 5;
            // stillInPlay = getDeckCards(game) + IN opponents' hands.
            Card still[4 * 14 + MAX_PLAYERS * MAX_HAND_SIZE];
            int sn = 0;
            for (int suit = 0; suit < 4; suit++) {
                for (int value = start_value; value <= 14; value++) {
                    if (!known[suit][value]) {
                        Card c = { (int8_t)suit, (int8_t)value };
                        still[sn++] = c;
                    }
                }
            }
            for (int i = 0; i < g->num_players; i++) {
                if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
                for (int j = 0; j < g->players[i].hand_count; j++)
                    still[sn++] = g->players[i].hand[j];
            }

            bool table_v[EP_VAL_SLOTS] = { false };
            for (int i = 0; i < g->num_battles; i++) {
                table_v[g->table_battles[i].attack.value] = true;
                if (g->table_battles[i].has_defense)
                    table_v[g->table_battles[i].defense.value] = true;
            }
            int all_opp_trumps = 0;
            for (int i = 0; i < g->num_players; i++) {
                if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
                for (int j = 0; j < g->players[i].hand_count; j++)
                    if (g->players[i].hand[j].suit == ps) all_opp_trumps++;
            }

            // best seeded with fullCovers[0] / -Infinity / Infinity / Infinity:
            // the first move always wins, then the TS strict tie chain.
            int best = -1;
            double best_eval = 0.0;
            int best_max = 0;
            int best_sum = 0;
            for (int i = 0; i < moves->n; i++) {
                const LegalMove *m = &moves->moves[i];
                if (m->type != MOVE_COVER || m->n_cards != uncovered) continue;

                Card remaining[MAX_HAND_SIZE];
                int rn = 0;
                for (int j = 0; j < bot->hand_count; j++) {
                    if (!ep_contains(m->cards, m->n_cards, bot->hand[j]))
                        remaining[rn++] = bot->hand[j];
                }
                int my_trumps_after = 0;
                for (int j = 0; j < rn; j++)
                    if (remaining[j].suit == ps) my_trumps_after++;

                int defendable = 0;
                for (int p = 0; p < g->num_players; p++) {
                    if (p == bot_idx || g->players[p].status != PLAYER_STATUS_IN) continue;
                    for (int j = 0; j < g->players[p].hand_count; j++) {
                        Card oc = g->players[p].hand[j];
                        for (int k = 0; k < rn; k++) {
                            if (can_cover(oc, remaining[k], ps)) { defendable++; break; }
                        }
                    }
                }
                int disposed_utility = 0;
                int pile_on = 0;
                for (int j = 0; j < m->n_cards; j++) {
                    Card c = m->cards[j];
                    for (int t = 0; t < sn; t++)
                        if (can_cover(still[t], c, ps)) disposed_utility++;
                    // pileOnRisk: 0 when the value is already on the table.
                    if (!table_v[c.value]) {
                        for (int p = 0; p < g->num_players; p++) {
                            if (p == bot_idx || g->players[p].status != PLAYER_STATUS_IN) continue;
                            if (p == g->defender) continue;   // otherAttackers
                            for (int k = 0; k < g->players[p].hand_count; k++)
                                if (g->players[p].hand[k].value == c.value) pile_on++;
                        }
                    }
                }
                double e = (double)defendable * 0.5
                         + 1.5 * (double)(my_trumps_after - all_opp_trumps)
                         - 0.3 * (double)disposed_utility
                         - 1.0 * (double)pile_on;
                int mx = ep_card_score(m->cards[0], ps);   // n_cards >= 1
                int sm = 0;
                for (int j = 0; j < m->n_cards; j++) {
                    int sc = ep_card_score(m->cards[j], ps);
                    if (sc > mx) mx = sc;
                    sm += sc;
                }
                bool better = (best < 0)
                    || e > best_eval
                    || (e == best_eval && mx < best_max)
                    || (e == best_eval && mx == best_max && sm < best_sum);
                if (better) { best = i; best_eval = e; best_max = mx; best_sum = sm; }
            }
            return best;
        }
        // No full cover: fall through (TS continues past the cover block).
    }

    // ---- goodMoves ------------------------------------------------------------
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_GOOD) return i;
    }

    // ---- doneAttacks: stable sort by (count desc, cardScore sum asc), take [0]
    // == first attack move with the max count, min sum among those.
    {
        int best = -1, best_count = 0, best_sum = 0;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_ATTACK) continue;
            int cnt = m->n_cards;
            int sm = ep_score_sum(m, ps);
            if (best < 0 || cnt > best_count || (cnt == best_count && sm < best_sum)) {
                best = i;
                best_count = cnt;
                best_sum = sm;
            }
        }
        if (best >= 0) return best;
    }

    // ---- pickupMoves ------------------------------------------------------------
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PICKUP) return i;
    }

    // ---- legalMoves[Math.floor(Math.random() * legalMoves.length)] -------
    return (int)(random_strategy_random() * (double)moves->n);
}

// ---------- tryPositionBias -------------------------------------------------

// Returns a move index or -1 (TS null).
static int ep_try_position_bias(const Game *g, int bot_idx, const LegalMoves *moves) {
    int in_count = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) in_count++;
    }
    if (in_count < 3) return -1;
    if (bot_idx == g->defender) return -1;

    // seatsAwayInPlayers(defender, botIndex): count IN seats strictly
    // between them (bot itself not counted), +1; -1 if never reached.
    int N = g->num_players;
    int seats_from_defender = -1;
    {
        int count = 0;
        int i = g->defender;
        for (int safety = 0; safety < N * 2; safety++) {
            i = (i + 1) % N;
            if (i == bot_idx) { seats_from_defender = count + 1; break; }
            if (g->players[i].status == PLAYER_STATUS_IN) count++;
        }
    }
    bool becomes_defender_on_good = (seats_from_defender == 1);
    bool becomes_defender_on_pickup = (seats_from_defender == 2);
    if (!becomes_defender_on_good && !becomes_defender_on_pickup) return -1;

    bool any_attack = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_ATTACK) { any_attack = true; break; }
    }
    if (!any_attack) return -1;

    const Player *defender = &g->players[g->defender];
    int ps = g->power_suit;

    // Predicate per move: onPickup → every card coverable by the defender;
    // onGood → some card NOT coverable. Scan the attacks in order twice:
    // stable sort by count desc + max-count filter + first strict-min
    // cardScore sum == first max-count attack passing the predicate with
    // the strictly smallest sum.
    int max_ct = -1;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != MOVE_ATTACK) continue;
        bool ok;
        if (becomes_defender_on_pickup) {
            ok = true;   // every(canCoverByDefender)
            for (int j = 0; j < m->n_cards && ok; j++) {
                bool coverable = false;
                for (int k = 0; k < defender->hand_count; k++) {
                    if (can_cover(m->cards[j], defender->hand[k], ps)) { coverable = true; break; }
                }
                if (!coverable) ok = false;
            }
        } else {
            ok = false;  // some(!canCoverByDefender)
            for (int j = 0; j < m->n_cards && !ok; j++) {
                bool coverable = false;
                for (int k = 0; k < defender->hand_count; k++) {
                    if (can_cover(m->cards[j], defender->hand[k], ps)) { coverable = true; break; }
                }
                if (!coverable) ok = true;
            }
        }
        if (ok && m->n_cards > max_ct) max_ct = m->n_cards;
    }
    if (max_ct < 0) return -1;   // filter empty

    int best = -1, best_sum = 0;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != MOVE_ATTACK || m->n_cards != max_ct) continue;
        bool ok;
        if (becomes_defender_on_pickup) {
            ok = true;
            for (int j = 0; j < m->n_cards && ok; j++) {
                bool coverable = false;
                for (int k = 0; k < defender->hand_count; k++) {
                    if (can_cover(m->cards[j], defender->hand[k], ps)) { coverable = true; break; }
                }
                if (!coverable) ok = false;
            }
        } else {
            ok = false;
            for (int j = 0; j < m->n_cards && !ok; j++) {
                bool coverable = false;
                for (int k = 0; k < defender->hand_count; k++) {
                    if (can_cover(m->cards[j], defender->hand[k], ps)) { coverable = true; break; }
                }
                if (!coverable) ok = true;
            }
        }
        if (!ok) continue;
        int s = ep_score_sum(m, ps);
        if (best < 0 || s < best_sum) { best = i; best_sum = s; }
    }
    return best;
}

// ---------- tryRandomTweaks ---------------------------------------------------

// Returns a move index or -1 (TS null).
static int ep_try_random_tweaks(const Game *g, int bot_idx,
                                const LegalMoves *moves, int in_count) {
    const Player *bot = &g->players[bot_idx];
    bool is_defender = (bot_idx == g->defender);
    int ps = g->power_suit;

    bool any_attack = false;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_ATTACK) { any_attack = true; break; }
    }
    if (any_attack && !is_defender) {
        // mixed: length > 1, some trump, some non-trump.
        // Stable sort count desc + top filter + first strict-min RAW value
        // sum == first max-count mixed attack with the smallest value sum.
        int max_ct = -1;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type != MOVE_ATTACK || m->n_cards <= 1) continue;
            if (!ep_attack_any_trump(m, ps) || !ep_attack_any_non_trump(m, ps)) continue;
            if (m->n_cards > max_ct) max_ct = m->n_cards;
        }
        if (max_ct >= 0) {
            int best = -1, best_sum = 0;
            for (int i = 0; i < moves->n; i++) {
                const LegalMove *m = &moves->moves[i];
                if (m->type != MOVE_ATTACK || m->n_cards != max_ct) continue;
                if (!ep_attack_any_trump(m, ps) || !ep_attack_any_non_trump(m, ps)) continue;
                int s = ep_value_sum(m);
                if (best < 0 || s < best_sum) { best = i; best_sum = s; }
            }
            return best;
        }

        // nonTrump attacks: only when there are NONE do we consider the
        // aggressive trump push.
        bool any_non_trump = false;
        for (int i = 0; i < moves->n; i++) {
            const LegalMove *m = &moves->moves[i];
            if (m->type == MOVE_ATTACK && ep_attack_all_non_trump(m, ps)) {
                any_non_trump = true;
                break;
            }
        }
        if (!any_non_trump) {
            bool any_trump_attack = false;
            for (int i = 0; i < moves->n; i++) {
                const LegalMove *m = &moves->moves[i];
                if (m->type == MOVE_ATTACK && ep_attack_any_trump(m, ps)) {
                    any_trump_attack = true;
                    break;
                }
            }
            if (any_trump_attack) {
                // aggressiveProb = Math.min(0.80, 0.20 * (inCount - 1))
                double aggressive_prob = 0.20 * (double)(in_count - 1);
                if (aggressive_prob > 0.80) aggressive_prob = 0.80;
                int total_trumps = in_count > 4 ? 14 : 10;
                int seen = ep_count_seen_trumps(g);
                int my_trumps = 0;
                for (int i = 0; i < bot->hand_count; i++)
                    if (bot->hand[i].suit == ps) my_trumps++;
                // oppVisibleTrumps: every player except the bot, ANY status.
                int opp_visible_trumps = 0;
                for (int i = 0; i < g->num_players; i++) {
                    if (i == bot_idx) continue;
                    for (int j = 0; j < g->players[i].hand_count; j++)
                        if (g->players[i].hand[j].suit == ps) opp_visible_trumps++;
                }
                int flipped_trump = (g->has_flipped && g->flipped.suit == ps) ? 1 : 0;
                int trumps_in_deck = total_trumps - seen - my_trumps
                                   - opp_visible_trumps - flipped_trump;
                if (trumps_in_deck <= 0) aggressive_prob = 0.95;
                // RNG draw happens whenever this point is reached.
                if (random_strategy_random() < aggressive_prob) {
                    int tmax = -1;
                    for (int i = 0; i < moves->n; i++) {
                        const LegalMove *m = &moves->moves[i];
                        if (m->type != MOVE_ATTACK || !ep_attack_any_trump(m, ps)) continue;
                        if (m->n_cards > tmax) tmax = m->n_cards;
                    }
                    int best = -1, best_sum = 0;
                    for (int i = 0; i < moves->n; i++) {
                        const LegalMove *m = &moves->moves[i];
                        if (m->type != MOVE_ATTACK || m->n_cards != tmax) continue;
                        if (!ep_attack_any_trump(m, ps)) continue;
                        int s = ep_value_sum(m);
                        if (best < 0 || s < best_sum) { best = i; best_sum = s; }
                    }
                    return best;
                }
            }
        }
    }

    return -1;
}

// ---------- chooseMove --------------------------------------------------------

int espresso_prod_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;   // TS throws; no index to return.
    ep_update_discard_memory(g);

    // opps = players other than the bot with status IN.
    int in_opps = 0;
    bool any_random = false;
    for (int i = 0; i < g->num_players; i++) {
        if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
        in_opps++;
        if (g->players[i].strategy_key == EP_KEY_RANDOM) any_random = true;
    }
    int in_count = in_opps + 1;

    // switch (inCount): 2 → choose2P (== choose1v1Logic); 3..8 → chooseNP;
    // default (1, or >8 which cannot happen) → choose2P.
    if (in_count >= 3 && in_count <= 8) {
        int bias = ep_try_position_bias(g, bot_idx, moves);
        if (bias >= 0) return bias;
        if (any_random) {
            int tweak = ep_try_random_tweaks(g, bot_idx, moves, in_count);
            if (tweak >= 0) return tweak;
            return handwritten_prod_strategy_choose(g, bot_idx, moves, ctx);
        }
        return ep_choose_1v1(g, bot_idx, moves);
    }
    return ep_choose_1v1(g, bot_idx, moves);
}

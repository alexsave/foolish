// Espresso 1v1 strategy — direct port of EspressoStrategy.choose1v1Logic in
// supabase/functions/_shared/strategies/espresso_strategy.ts.
//
// We don't need the multi-player code paths (chooseNP, tryPositionBias,
// tryRandomTweaks): for 2-player IN counts, espresso always falls into
// choose1v1Logic (see the inCount switch in chooseMove).
//
// Discard memory is reconstructed each call from game.logs (same as TS — the
// TS code persists `seenCards` per-game-id via a Map, but it only ever adds,
// so a fresh scan of the logs is equivalent).

#include "strategy.h"
#include "card.h"
#include "game.h"
#include <string.h>
#include <stdint.h>

// ---------- helpers ---------------------------------------------------

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

static double get_trump_attack_probability(const Game *g) {
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

// Pick the opponent the same way the TS code does.
static const Player *get_opponent(const Game *g, int bot_idx) {
    int in_count = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) in_count++;
    }
    if (in_count == 0) return NULL;
    if (g->defender != bot_idx && g->players[g->defender].status == PLAYER_STATUS_IN) {
        return &g->players[g->defender];
    }
    if (g->first_attacker != bot_idx && g->players[g->first_attacker].status == PLAYER_STATUS_IN) {
        return &g->players[g->first_attacker];
    }
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) return &g->players[i];
    }
    return NULL;
}

// ---------- predict cover (greedy lowest-score canCover) -------------

typedef struct {
    Card cards[MAX_HAND_SIZE];
    int  n;
} CardSet;

static void predict_cover(const Card *attack, int attack_n,
                          const Card *opp_hand, int opp_n,
                          int power_suit,
                          CardSet *covers, bool *pickup) {
    Card remaining[MAX_HAND_SIZE];
    int rn = opp_n;
    for (int i = 0; i < opp_n; i++) remaining[i] = opp_hand[i];
    covers->n = 0;
    *pickup = false;
    for (int a = 0; a < attack_n; a++) {
        int best_idx = -1;
        int best_score = INT32_MAX;
        for (int i = 0; i < rn; i++) {
            if (can_cover(attack[a], remaining[i], power_suit)) {
                int s = card_score(remaining[i], power_suit);
                if (s < best_score) { best_score = s; best_idx = i; }
            }
        }
        if (best_idx < 0) {
            covers->n = 0;
            *pickup = true;
            return;
        }
        covers->cards[covers->n++] = remaining[best_idx];
        for (int i = best_idx + 1; i < rn; i++) remaining[i - 1] = remaining[i];
        rn--;
    }
}

// ---------- rollout one round ----------------------------------------

typedef struct {
    Card my_hand[MAX_HAND_SIZE];
    int  my_n;
    Card opp_hand[MAX_HAND_SIZE];
    int  opp_n;
    bool pickup;
} RolloutResult;

static bool set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static void rollout_round(const Card *first_attack, int fa_n,
                          const Card *my_hand, int my_n,
                          const Card *opp_hand, int opp_n,
                          int power_suit, RolloutResult *r) {
    Card my[MAX_HAND_SIZE]; int mN = my_n;
    Card opp[MAX_HAND_SIZE]; int oN = opp_n;
    for (int i = 0; i < my_n; i++) my[i] = my_hand[i];
    for (int i = 0; i < opp_n; i++) opp[i] = opp_hand[i];

    bool table_values[16] = { false };

    Card attack[MAX_MOVE_CARDS];
    int an = fa_n;
    for (int i = 0; i < fa_n; i++) attack[i] = first_attack[i];

    for (int iter = 0; iter < 5; iter++) {
        // Drop attack cards from my hand (they're moving to the table).
        int new_mN = 0;
        for (int i = 0; i < mN; i++) {
            if (!set_contains(attack, an, my[i])) my[new_mN++] = my[i];
        }
        mN = new_mN;
        for (int i = 0; i < an; i++) table_values[attack[i].value] = true;

        CardSet covers; bool pickup;
        predict_cover(attack, an, opp, oN, power_suit, &covers, &pickup);
        if (pickup) {
            // Opp picks up; attacks join opp hand.
            for (int i = 0; i < an; i++) opp[oN++] = attack[i];
            r->pickup = true;
            r->my_n = mN; r->opp_n = oN;
            for (int i = 0; i < mN; i++) r->my_hand[i] = my[i];
            for (int i = 0; i < oN; i++) r->opp_hand[i] = opp[i];
            return;
        }
        // Opp covered with `covers`. Drop those from opp hand.
        int new_oN = 0;
        for (int i = 0; i < oN; i++) {
            if (!set_contains(covers.cards, covers.n, opp[i])) opp[new_oN++] = opp[i];
        }
        oN = new_oN;
        for (int i = 0; i < covers.n; i++) table_values[covers.cards[i].value] = true;

        // Find next attack: highest-count value group from my hand whose value
        // is on table & non-trump. Tie-break by lowest sum.
        Card matching[MAX_HAND_SIZE]; int mn = 0;
        for (int i = 0; i < mN; i++) {
            if (table_values[my[i].value] && my[i].suit != power_suit) matching[mn++] = my[i];
        }
        if (mn == 0) {
            r->pickup = false; r->my_n = mN; r->opp_n = oN;
            for (int i = 0; i < mN; i++) r->my_hand[i] = my[i];
            for (int i = 0; i < oN; i++) r->opp_hand[i] = opp[i];
            return;
        }
        bool seen_v[16] = { false };
        Card best_group[MAX_MOVE_CARDS]; int best_n = 0; int best_sum = INT32_MAX;
        for (int i = 0; i < mn; i++) {
            int v = matching[i].value;
            if (seen_v[v]) continue;
            seen_v[v] = true;
            Card group[MAX_MOVE_CARDS]; int gn = 0; int sum = 0;
            for (int j = 0; j < mn; j++) {
                if (matching[j].value == v) { group[gn++] = matching[j]; sum += matching[j].value; }
            }
            // TS: bestCount > current OR (==count AND smaller sum)
            bool better = (gn > best_n) || (gn == best_n && sum < best_sum);
            if (better) {
                best_n = gn; best_sum = sum;
                for (int j = 0; j < gn; j++) best_group[j] = group[j];
            }
        }
        // attackCards = bestGroup.slice(0, oppH.length)
        an = best_n < oN ? best_n : oN;
        for (int i = 0; i < an; i++) attack[i] = best_group[i];
        if (an == 0) {
            r->pickup = false; r->my_n = mN; r->opp_n = oN;
            for (int i = 0; i < mN; i++) r->my_hand[i] = my[i];
            for (int i = 0; i < oN; i++) r->opp_hand[i] = opp[i];
            return;
        }
    }
    r->pickup = false; r->my_n = mN; r->opp_n = oN;
    for (int i = 0; i < mN; i++) r->my_hand[i] = my[i];
    for (int i = 0; i < oN; i++) r->opp_hand[i] = opp[i];
}

// ---------- helpers for moves ----------------------------------------

static bool move_is_attack_all_non_trump(const LegalMove *m, int power_suit) {
    if (m->type != MOVE_ATTACK) return false;
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].suit == power_suit) return false;
    return true;
}
static bool move_is_attack_any_trump(const LegalMove *m, int power_suit) {
    if (m->type != MOVE_ATTACK) return false;
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].suit == power_suit) return true;
    return false;
}

// ---------- choose --------------------------------------------------

int espresso_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    const Player *bot = &g->players[bot_idx];
    const Player *opp = get_opponent(g, bot_idx);

    // ---- continueAttackMoves ---------------------------------------------
    int n_attack = 0, n_non_trump = 0, n_trump = 0;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_ATTACK) {
            n_attack++;
            if (move_is_attack_all_non_trump(&moves->moves[i], g->power_suit)) n_non_trump++;
            else if (move_is_attack_any_trump(&moves->moves[i], g->power_suit)) n_trump++;
        }
    }

    // We pick "candidateMoves" — either non-trump (if any) or all-trump under
    // a probability gate. Stash indices into moves->moves[].
    int candidate_idx[MAX_LEGAL_MOVES];
    int cn = 0;
    bool fall_through_to_other = false;

    if (n_attack > 0) {
        if (n_non_trump > 0) {
            for (int i = 0; i < moves->n; i++) {
                if (move_is_attack_all_non_trump(&moves->moves[i], g->power_suit)) candidate_idx[cn++] = i;
            }
        } else if (n_trump > 0) {
            // Lowest trump value across all trump-attack moves.
            int my_lowest_trump = ACE_VALUE + 1;
            for (int i = 0; i < moves->n; i++) {
                if (move_is_attack_any_trump(&moves->moves[i], g->power_suit)) {
                    for (int j = 0; j < moves->moves[i].n_cards; j++) {
                        if (moves->moves[i].cards[j].suit == g->power_suit
                            && moves->moves[i].cards[j].value < my_lowest_trump) {
                            my_lowest_trump = moves->moves[i].cards[j].value;
                        }
                    }
                }
            }
            bool allow = false;
            if (opp) {
                for (int j = 0; j < opp->hand_count; j++) {
                    if (opp->hand[j].suit == g->power_suit && opp->hand[j].value > my_lowest_trump) {
                        allow = true; break;
                    }
                }
            }
            if (allow || game_random() < get_trump_attack_probability(g)) {
                for (int i = 0; i < moves->n; i++) {
                    if (move_is_attack_any_trump(&moves->moves[i], g->power_suit)) candidate_idx[cn++] = i;
                }
            } else {
                // Try good[0]; otherwise fall through to the rest of the cascade.
                for (int i = 0; i < moves->n; i++) {
                    if (moves->moves[i].type == MOVE_GOOD) return i;
                }
                fall_through_to_other = true;
            }
        }
    }

    if (cn > 0) {
        // ---- Evaluate candidateMoves with rollout + heuristics --------
        int in_opps_count = 0;
        int min_opp_hand = INT32_MAX;
        int min_opp_idx = -1;
        for (int i = 0; i < g->num_players; i++) {
            if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
            in_opps_count++;
            if (g->players[i].hand_count < min_opp_hand) {
                min_opp_hand = g->players[i].hand_count;
                min_opp_idx = i;
            }
        }
        bool defender_is_leader = false;
        if (g->defender != bot_idx && g->players[g->defender].status == PLAYER_STATUS_IN
            && g->players[g->defender].hand_count == min_opp_hand) defender_is_leader = true;
        bool leader_is_attacker = (min_opp_idx >= 0 && min_opp_idx != g->defender);
        const Player *leader = (min_opp_idx >= 0) ? &g->players[min_opp_idx] : NULL;
        bool deck_active = (g->deck_count > 0 || g->has_flipped);

        // Pass-window: opponent exists AND every battle is uncovered.
        bool pass_window = (opp != NULL);
        for (int i = 0; i < g->num_battles; i++) {
            if (g->table_battles[i].has_defense) { pass_window = false; break; }
        }

        int best = candidate_idx[0];
        double best_eval = -1e30;
        int best_count = -1;
        int best_sum = INT32_MAX;
        bool first_eval = true;

        for (int ci = 0; ci < cn; ci++) {
            const LegalMove *m = &moves->moves[candidate_idx[ci]];
            int v = m->cards[0].value;
            bool passable = false;
            if (pass_window && opp) {
                for (int j = 0; j < opp->hand_count; j++) if (opp->hand[j].value == v) { passable = true; break; }
            }
            double e = 0.0;
            if (opp) {
                RolloutResult r;
                rollout_round(m->cards, m->n_cards, bot->hand, bot->hand_count,
                              opp->hand, opp->hand_count, g->power_suit, &r);
                int my_trumps = 0; for (int i = 0; i < r.my_n; i++) if (r.my_hand[i].suit == g->power_suit) my_trumps++;
                int opp_trumps = 0; for (int i = 0; i < r.opp_n; i++) if (r.opp_hand[i].suit == g->power_suit) opp_trumps++;
                double size_weight = (r.pickup || !deck_active) ? 1.0 : 0.0;
                double pickup_bonus = r.pickup ? 3.0 : 0.0;
                double block_leader = (r.pickup && defender_is_leader) ? 4.0 : 0.0;
                double leader_pile_on_pen = 0.0;
                if (leader_is_attacker && leader) {
                    int my_v = m->cards[0].value;
                    int matches = 0;
                    for (int i = 0; i < leader->hand_count; i++) if (leader->hand[i].value == my_v) matches++;
                    leader_pile_on_pen = matches * 0.7;
                }
                e = size_weight * (r.opp_n - r.my_n)
                  + 1.5 * (my_trumps - opp_trumps)
                  + pickup_bonus + block_leader - leader_pile_on_pen;
            }
            (void)in_opps_count;
            double final_eval = passable ? e - 1000.0 : e;
            int cnt = m->n_cards;
            int sum = 0;
            for (int i = 0; i < cnt; i++) sum += card_score(m->cards[i], g->power_suit);

            bool take = first_eval
                || final_eval > best_eval
                || (final_eval == best_eval && cnt > best_count)
                || (final_eval == best_eval && cnt == best_count && sum < best_sum);
            if (take) {
                best = candidate_idx[ci];
                best_eval = final_eval;
                best_count = cnt;
                best_sum = sum;
                first_eval = false;
            }
        }
        return best;
    }

    (void)fall_through_to_other;

    // ---- pass moves ---------------------------------------------------
    int pass_idx[MAX_LEGAL_MOVES]; int pn = 0;
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_PASS) pass_idx[pn++] = i;
    if (pn > 0 && opp) {
        int best = pass_idx[0];
        double best_eval = -1e30; bool first = true;
        Card all_attacks[MAX_BATTLES + MAX_MOVE_CARDS];
        for (int pi = 0; pi < pn; pi++) {
            const LegalMove *m = &moves->moves[pass_idx[pi]];
            int an = 0;
            for (int i = 0; i < g->num_battles; i++) all_attacks[an++] = g->table_battles[i].attack;
            for (int i = 0; i < m->n_cards; i++) all_attacks[an++] = m->cards[i];
            CardSet covers; bool pickup;
            predict_cover(all_attacks, an, opp->hand, opp->hand_count, g->power_suit, &covers, &pickup);

            // myH: bot.hand minus pass cards
            Card myH[MAX_HAND_SIZE]; int my_n = 0;
            for (int i = 0; i < bot->hand_count; i++) {
                if (!set_contains(m->cards, m->n_cards, bot->hand[i])) myH[my_n++] = bot->hand[i];
            }
            Card oppH[MAX_HAND_SIZE + MAX_BATTLES + MAX_MOVE_CARDS]; int opp_n = 0;
            if (pickup) {
                for (int i = 0; i < opp->hand_count; i++) oppH[opp_n++] = opp->hand[i];
                for (int i = 0; i < an; i++) oppH[opp_n++] = all_attacks[i];
            } else {
                for (int i = 0; i < opp->hand_count; i++) {
                    if (!set_contains(covers.cards, covers.n, opp->hand[i])) oppH[opp_n++] = opp->hand[i];
                }
            }
            int my_t = 0; for (int i = 0; i < my_n; i++) if (myH[i].suit == g->power_suit) my_t++;
            int opp_t = 0; for (int i = 0; i < opp_n; i++) if (oppH[i].suit == g->power_suit) opp_t++;
            bool deck_active = (g->deck_count > 0 || g->has_flipped);
            double size_weight = (pickup || !deck_active) ? 1.0 : 0.0;
            double pickup_bonus = pickup ? 3.0 : 0.0;
            double e = size_weight * (opp_n - my_n) + 1.5 * (my_t - opp_t) + pickup_bonus;
            if (first || e > best_eval) { best_eval = e; best = pass_idx[pi]; first = false; }
        }
        return best;
    }
    if (pn > 0) return pass_idx[0];

    // ---- cover moves --------------------------------------------------
    int cover_idx[MAX_LEGAL_MOVES]; int cnv = 0;
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_COVER) cover_idx[cnv++] = i;
    if (cnv > 0) {
        int uncovered = 0;
        for (int i = 0; i < g->num_battles; i++) if (!g->table_battles[i].has_defense) uncovered++;

        // Full-cover moves only.
        int full_idx[MAX_LEGAL_MOVES]; int fn = 0;
        for (int i = 0; i < cnv; i++) {
            // attack_cards length stored as n_cards (TS uses cards.length === uncovered).
            if (moves->moves[cover_idx[i]].n_cards == uncovered) full_idx[fn++] = cover_idx[i];
        }
        if (fn > 0) {
            // Build "stillInPlay" = deck cards (reconstructed from log discards) + opponents' hands.
            Card known[64]; int kn = 0;
            for (int i = 0; i < g->num_players; i++) {
                for (int j = 0; j < g->players[i].hand_count; j++) known[kn++] = g->players[i].hand[j];
            }
            for (int i = 0; i < g->num_battles; i++) {
                known[kn++] = g->table_battles[i].attack;
                if (g->table_battles[i].has_defense) known[kn++] = g->table_battles[i].defense;
            }
            if (g->has_flipped) known[kn++] = g->flipped;
            // Discard memory from logs.
            for (int i = 0; i < g->num_logs; i++) {
                if (g->logs[i].log_type == LOG_DISCARD) {
                    for (int j = 0; j < g->logs[i].num_pairs; j++) {
                        known[kn++] = g->logs[i].pairs[j].primary;
                    }
                }
            }
            int start_v = (g->num_players > 4) ? 1 : MIN_VALUE_2P;
            Card deck_pool[80]; int dn = 0;
            for (int suit = 0; suit < 4; suit++) {
                for (int v = start_v; v <= 14; v++) {  // TS uses 14 (not ACE_VALUE) — port verbatim
                    Card c = { (int8_t)suit, (int8_t)v };
                    bool seen = false;
                    for (int i = 0; i < kn; i++) if (card_eq(known[i], c)) { seen = true; break; }
                    if (!seen) deck_pool[dn++] = c;
                }
            }
            // stillInPlay = deck_pool + opponents' hands (in TS this is what gets pushed).
            Card still[160]; int sn = 0;
            for (int i = 0; i < dn; i++) still[sn++] = deck_pool[i];
            for (int i = 0; i < g->num_players; i++) {
                if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
                for (int j = 0; j < g->players[i].hand_count; j++) still[sn++] = g->players[i].hand[j];
            }

            bool table_v[16] = { false };
            for (int i = 0; i < g->num_battles; i++) {
                table_v[g->table_battles[i].attack.value] = true;
                if (g->table_battles[i].has_defense) table_v[g->table_battles[i].defense.value] = true;
            }
            int all_opp_trumps = 0;
            for (int i = 0; i < g->num_players; i++) {
                if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
                for (int j = 0; j < g->players[i].hand_count; j++)
                    if (g->players[i].hand[j].suit == g->power_suit) all_opp_trumps++;
            }

            int best = full_idx[0];
            double best_eval = -1e30; int best_max = INT32_MAX; int best_sum = INT32_MAX;
            bool first = true;

            for (int fi = 0; fi < fn; fi++) {
                const LegalMove *m = &moves->moves[full_idx[fi]];

                Card remaining[MAX_HAND_SIZE]; int rn = 0;
                for (int i = 0; i < bot->hand_count; i++) {
                    if (!set_contains(m->cards, m->n_cards, bot->hand[i])) remaining[rn++] = bot->hand[i];
                }
                int my_trumps_after = 0;
                for (int i = 0; i < rn; i++) if (remaining[i].suit == g->power_suit) my_trumps_after++;

                int defendable = 0;
                for (int i = 0; i < g->num_players; i++) {
                    if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
                    for (int j = 0; j < g->players[i].hand_count; j++) {
                        Card oc = g->players[i].hand[j];
                        for (int k = 0; k < rn; k++) {
                            if (can_cover(oc, remaining[k], g->power_suit)) { defendable++; break; }
                        }
                    }
                }
                int disposed_utility = 0;
                int pile_on = 0;
                for (int i = 0; i < m->n_cards; i++) {
                    Card c = m->cards[i];
                    int n = 0;
                    for (int t = 0; t < sn; t++) if (can_cover(still[t], c, g->power_suit)) n++;
                    disposed_utility += n;
                    if (!table_v[c.value]) {
                        for (int p2 = 0; p2 < g->num_players; p2++) {
                            if (p2 == bot_idx || g->players[p2].status != PLAYER_STATUS_IN) continue;
                            if (p2 == g->defender) continue;
                            for (int j = 0; j < g->players[p2].hand_count; j++) {
                                if (g->players[p2].hand[j].value == c.value) pile_on++;
                            }
                        }
                    }
                }
                double e = defendable * 0.5
                         + 1.5 * (my_trumps_after - all_opp_trumps)
                         - 0.3 * disposed_utility
                         - 1.0 * pile_on;
                int mx = 0, sm = 0;
                for (int i = 0; i < m->n_cards; i++) {
                    int sc = card_score(m->cards[i], g->power_suit);
                    if (sc > mx) mx = sc;
                    sm += sc;
                }
                bool take = first
                    || e > best_eval
                    || (e == best_eval && mx < best_max)
                    || (e == best_eval && mx == best_max && sm < best_sum);
                if (take) { best = full_idx[fi]; best_eval = e; best_max = mx; best_sum = sm; first = false; }
            }
            return best;
        }
    }

    // ---- good ---------------------------------------------------------
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_GOOD) return i;

    // ---- done attacks (sort by count desc, score asc) -----------------
    int done_idx[MAX_LEGAL_MOVES]; int dnv = 0;
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_ATTACK) done_idx[dnv++] = i;
    if (dnv > 0) {
        int best = done_idx[0];
        const LegalMove *bm = &moves->moves[best];
        int best_count = bm->n_cards;
        int best_sum = 0;
        for (int i = 0; i < bm->n_cards; i++) best_sum += card_score(bm->cards[i], g->power_suit);
        for (int i = 1; i < dnv; i++) {
            const LegalMove *m = &moves->moves[done_idx[i]];
            int sm = 0;
            for (int j = 0; j < m->n_cards; j++) sm += card_score(m->cards[j], g->power_suit);
            int cnt = m->n_cards;
            bool take = (cnt > best_count) || (cnt == best_count && sm < best_sum);
            if (take) { best = done_idx[i]; best_count = cnt; best_sum = sm; }
        }
        return best;
    }

    // ---- pickup -------------------------------------------------------
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_PICKUP) return i;

    // ---- random fallback (uses Math.random — game_random) -------------
    int idx = (int)(game_random() * moves->n);
    if (idx < 0) idx = 0;
    if (idx >= moves->n) idx = moves->n - 1;
    return idx;
}

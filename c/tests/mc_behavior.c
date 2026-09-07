// Empirical opponent-model statistics for an MC bot (octogen): what does it do
// where the handwritten rollout policy assumes something else? Plays N
// heads-up (or pc-way) games strategy-vs-strategy and, at every decision of
// seat 0, records: as a throw-in attacker with a legal non-trump throw-in,
// did it throw or say GOOD, and the value profile of thrown vs held matching
// cards; as a defender holding a full greedy cover, did it cover or pick up,
// split by whether the greedy cover needed a trump. Deck-alive only (the
// regime the rollout model matters most for).
#include "game.h"
#include "legal.h"
#include "strategy.h"
#include "cli_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long thr_opp = 0, thr_good = 0, thr_partial = 0, thr_full = 0;
static long thr_val_thrown[14] = {0}, thr_val_held[14] = {0};
static long def_full = 0, def_full_pick = 0, def_fullT = 0, def_fullT_pick = 0;
static long first_n = 0, first_not_lowest = 0;
// Exact handwritten-match per decision type (deck alive), and deviation kinds.
static long m_first = 0, m_first_eq = 0, m_first_higher = 0, m_first_fewer = 0, m_first_more = 0;
static long m_thr = 0, m_thr_eq = 0, m_thr_good = 0, m_thr_fewer = 0, m_thr_other = 0;
static long m_def = 0, m_def_eq = 0, m_def_pick = 0, m_def_cover_other = 0, m_def_nopass = 0, m_def_other = 0;
static long first_ncards[8] = {0};
static long oc_trump_for_nt = 0, oc_higher_same = 0, oc_nt_for_trump = 0, oc_assign = 0, oc_partial = 0;
static long fh_single_of_pair = 0, fh_pair_over_lower_single = 0, fh_single_over_lower_single = 0;
static long pick_nt = 0, pick_nt_n = 0, pick_tr = 0, pick_tr_n = 0;
static long first_rank_hist[8] = {0};   // rank of led value among distinct held non-trump values

static int dispatch(int strat, const Game *g, int pi, const LegalMoves *m) {
    switch (strat) {
        case STRAT_OCTOGEN: return octogen_strategy_choose(g, pi, m, NULL);
        case STRAT_CORDITE: return cordite_strategy_choose(g, pi, m, NULL);
        case STRAT_SEMTEX:  return semtex_strategy_choose(g, pi, m, NULL);
        case STRAT_HANDWRITTEN: return handwritten_strategy_choose(g, pi, m, NULL);
        default: return random_strategy_choose(g, pi, m, NULL);
    }
}

static int move_sum(const LegalMove *m, int power) {
    int s = 0; for (int k = 0; k < m->n_cards; k++) s += m->cards[k].value + (m->cards[k].suit == power ? 1000 : 0);
    return s;
}
static bool same_cards(const LegalMove *a, const LegalMove *b) {
    if (a->type != b->type || a->n_cards != b->n_cards) return false;
    for (int i = 0; i < a->n_cards; i++) {
        bool f = false;
        for (int j = 0; j < b->n_cards; j++) if (card_eq(a->cards[i], b->cards[j])) f = true;
        if (!f) return false;
    }
    return true;
}
static void record(const Game *g, int pi, const LegalMoves *moves, int idx) {
    const Player *P = &g->players[pi];
    int power = g->power_suit;
    bool deck_alive = g->deck_count > 0 || g->has_flipped;
    if (!deck_alive) return;
    const LegalMove *m = &moves->moves[idx];
    bool is_def = (pi == g->defender);
    // Handwritten's choice on the identical state (the rollout model).
    {
        uint32_t rs = game_rng_get();
        int hi = handwritten_strategy_choose(g, pi, moves, NULL);
        game_rng_set(rs);
        if (hi >= 0) {
            const LegalMove *h = &moves->moves[hi];
            bool eq = same_cards(m, h);
            if (!is_def && g->num_battles == 0 && m->type == MOVE_ATTACK) {
                m_first++; if (eq) m_first_eq++;
                else if (m->n_cards < h->n_cards) m_first_fewer++;
                else if (m->n_cards > h->n_cards) m_first_more++;
                else if (m->cards[0].value > h->cards[0].value) {
                    m_first_higher++;
                    int cnt_led = 0, cnt_hw = 0;
                    for (int j = 0; j < P->hand_count; j++) {
                        if (P->hand[j].suit == power) continue;
                        if (P->hand[j].value == m->cards[0].value) cnt_led++;
                        if (P->hand[j].value == h->cards[0].value) cnt_hw++;
                    }
                    if (m->n_cards == 1 && cnt_led == 1 && cnt_hw == 1) fh_single_over_lower_single++;
                    else if (m->n_cards >= 2 && cnt_hw == 1) fh_pair_over_lower_single++;
                }
                if (m->n_cards < h->n_cards) {
                    int cnt_led = 0;
                    for (int j = 0; j < P->hand_count; j++)
                        if (P->hand[j].suit != power && P->hand[j].value == m->cards[0].value) cnt_led++;
                    if (m->n_cards == 1 && cnt_led >= 2) fh_single_of_pair++;
                }
                first_ncards[m->n_cards < 7 ? m->n_cards : 7]++;
                int distinct[16] = {0};
                for (int j = 0; j < P->hand_count; j++) if (P->hand[j].suit != power) distinct[P->hand[j].value] = 1;
                int rank = 0;
                for (int v = 1; v < m->cards[0].value; v++) rank += distinct[v];
                first_rank_hist[rank < 7 ? rank : 7]++;
            } else if (!is_def && g->num_battles > 0) {
                m_thr++; if (eq) m_thr_eq++;
                else if (m->type == MOVE_GOOD) m_thr_good++;
                else if (m->type == MOVE_ATTACK && h->type == MOVE_ATTACK && m->n_cards < h->n_cards) m_thr_fewer++;
                else m_thr_other++;
            } else if (is_def && g->num_battles > 0) {
                m_def++; if (eq) m_def_eq++;
                else if (m->type == MOVE_PICKUP) m_def_pick++;
                else if (m->type == MOVE_COVER && h->type == MOVE_COVER) {
                    m_def_cover_other++;
                    if (m->n_cards < h->n_cards) oc_partial++;
                    else {
                        int mt = 0, ht = 0, msum = 0, hsum = 0;
                        for (int k = 0; k < m->n_cards; k++) { if (m->cards[k].suit == power) mt++; msum += m->cards[k].value; }
                        for (int k = 0; k < h->n_cards; k++) { if (h->cards[k].suit == power) ht++; hsum += h->cards[k].value; }
                        if (mt > ht) oc_trump_for_nt++;
                        else if (mt < ht) oc_nt_for_trump++;
                        else if (msum > hsum) oc_higher_same++;
                        else oc_assign++;
                    }
                }
                else if (h->type == MOVE_PASS) m_def_nopass++;
                else m_def_other++;
            }
        }
    }
    if (!is_def && g->num_battles > 0) {
        // throw-in opportunity: any legal non-trump attack?
        bool has_nt = false;
        for (int i = 0; i < moves->n; i++) {
            if (moves->moves[i].type != MOVE_ATTACK) continue;
            bool nt = true;
            for (int k = 0; k < moves->moves[i].n_cards; k++) if (moves->moves[i].cards[k].suit == power) nt = false;
            if (nt) { has_nt = true; break; }
        }
        if (!has_nt) return;
        thr_opp++;
        // matching non-trump cards held
        bool tv[16] = {0};
        for (int b = 0; b < g->num_battles; b++) {
            tv[g->table_battles[b].attack.value] = true;
            if (!card_is_none(g->table_battles[b].defense)) tv[g->table_battles[b].defense.value] = true;
        }
        int held = 0;
        for (int j = 0; j < P->hand_count; j++)
            if (P->hand[j].suit != power && tv[P->hand[j].value]) { held++; thr_val_held[P->hand[j].value]++; }
        if (m->type == MOVE_GOOD) { thr_good++; return; }
        if (m->type != MOVE_ATTACK) return;
        for (int k = 0; k < m->n_cards; k++) if (m->cards[k].suit != power) thr_val_thrown[m->cards[k].value]++;
        int defcap = g->players[g->defender].hand_count;
        int unc = 0;
        for (int b = 0; b < g->num_battles; b++) if (card_is_none(g->table_battles[b].defense)) unc++;
        int cap = defcap - unc;
        if (m->n_cards < held && m->n_cards < cap) thr_partial++; else thr_full++;
        return;
    }
    if (is_def && g->num_battles > 0) {
        // greedy full cover available?
        bool used[MAX_HAND_SIZE] = {0};
        bool full = true, needT = false;
        for (int b = 0; b < g->num_battles; b++) {
            if (!card_is_none(g->table_battles[b].defense)) continue;
            int best = -1, bs = 1 << 20;
            for (int j = 0; j < P->hand_count; j++) {
                if (used[j]) continue;
                if (!can_cover(g->table_battles[b].attack, P->hand[j], power)) continue;
                int sc = P->hand[j].value + (P->hand[j].suit == power ? 1000 : 0);
                if (sc < bs) { bs = sc; best = j; }
            }
            if (best < 0) { full = false; break; }
            used[best] = true;
            if (P->hand[best].suit == power) needT = true;
        }
        if (!full) return;
        if (needT) { def_fullT++; if (m->type == MOVE_PICKUP) def_fullT_pick++; }
        else       { def_full++;  if (m->type == MOVE_PICKUP) def_full_pick++; }
        // pickup rate by number of uncovered cards
        int unc = 0; for (int b = 0; b < g->num_battles; b++) if (card_is_none(g->table_battles[b].defense)) unc++;
        if (needT) { pick_tr_n += unc; if (m->type == MOVE_PICKUP) pick_tr += unc; }
        else { pick_nt_n += unc; if (m->type == MOVE_PICKUP) pick_nt += unc; }
        return;
    }
    if (!is_def && g->num_battles == 0 && m->type == MOVE_ATTACK) {
        first_n++;
        int minv = 99;
        for (int j = 0; j < P->hand_count; j++) if (P->hand[j].suit != power && P->hand[j].value < minv) minv = P->hand[j].value;
        bool lowest = false;
        for (int k = 0; k < m->n_cards; k++) if (m->cards[k].value == minv) lowest = true;
        if (!lowest) first_not_lowest++;
    }
}

int main(int argc, char **argv) {
    int strat = parse_strategy(get_arg(argc, argv, "strategy", "octogen"));
    int opp   = parse_strategy(get_arg(argc, argv, "opp", "octogen"));
    int n     = parse_int(get_arg(argc, argv, "players", "2"), 2);
    int games = parse_int(get_arg(argc, argv, "games", "100"), 100);
    uint32_t seed0 = (uint32_t)parse_int(get_arg(argc, argv, "seed-start", "700001"), 700001);
    for (int gi = 0; gi < games; gi++) {
        uint32_t seed = seed0 + (uint32_t)gi;
        game_set_seed(seed); random_strategy_set_seed(seed);
        Game g; memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)n;
        for (int i = 0; i < n; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = (int8_t)(i == 0 ? strat : opp);
            snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
        }
        start_game(&g);
        int iters = 0;
        while (game_done(&g) < 0 && iters++ < 4000) {
            int elig[MAX_PLAYERS], n_e = 0;
            for (int i = 0; i < n; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
            if (!n_e) break;
            for (int i = n_e - 1; i > 0; i--) { int j = (int)(game_random() * (i + 1)); if (j > i) j = i; int t = elig[i]; elig[i] = elig[j]; elig[j] = t; }
            bool acted = false;
            for (int k = 0; k < n_e; k++) {
                int pi = elig[k];
                LegalMoves moves; calculate_legal_moves(&g, pi, &moves);
                if (!moves.n) continue;
                int idx = dispatch(g.players[pi].strategy_key, &g, pi, &moves);
                if (idx < 0 || idx >= moves.n) continue;
                if (pi == 0) record(&g, pi, &moves, idx);
                const LegalMove *m = &moves.moves[idx];
                bool ok = false;
                switch (m->type) {
                    case MOVE_ATTACK: ok = handle_attack(&g, pi, m->cards, m->n_cards); break;
                    case MOVE_COVER:  ok = handle_cover (&g, pi, m->cards, m->attack_cards, m->n_cards); break;
                    case MOVE_PASS:   ok = handle_pass  (&g, pi, m->cards, m->n_cards); break;
                    case MOVE_PICKUP: ok = handle_pickup(&g, pi); break;
                    case MOVE_GOOD:   ok = handle_good  (&g, pi); break;
                    default: break;
                }
                if (ok) { acted = true; break; }
            }
            if (!acted) break;
        }
    }
    printf("throw-in opportunities (legal non-trump throw-in, deck alive): %ld\n", thr_opp);
    printf("  said GOOD: %ld (%.1f%%)   threw all/capped: %ld   threw partial: %ld\n",
           thr_good, thr_opp ? 100.0 * thr_good / thr_opp : 0, thr_full, thr_partial);
    printf("  value  thrown  held(matching non-trump)  throw-rate\n");
    for (int v = 1; v <= 13; v++)
        if (thr_val_held[v]) printf("  %5d  %6ld  %6ld  %.2f\n", v, thr_val_thrown[v], thr_val_held[v], (double)thr_val_thrown[v] / thr_val_held[v]);
    printf("defender with full greedy cover (non-trump): %ld, picked up: %ld (%.1f%%)\n",
           def_full, def_full_pick, def_full ? 100.0 * def_full_pick / def_full : 0);
    printf("defender with full greedy cover needing a trump: %ld, picked up: %ld (%.1f%%)\n",
           def_fullT, def_fullT_pick, def_fullT ? 100.0 * def_fullT_pick / def_fullT : 0);
    printf("first attacks: %ld, not containing the lowest non-trump value: %ld (%.1f%%)\n",
           first_n, first_not_lowest, first_n ? 100.0 * first_not_lowest / first_n : 0);
    printf("\nHANDWRITTEN MATCH (deck alive):\n");
    printf("  first attack: %ld  eq=%.1f%%  fewer-cards=%.1f%%  more-cards=%.1f%%  higher-value(same n)=%.1f%%\n",
           m_first, 100.0 * m_first_eq / (m_first ? m_first : 1), 100.0 * m_first_fewer / (m_first ? m_first : 1),
           100.0 * m_first_more / (m_first ? m_first : 1), 100.0 * m_first_higher / (m_first ? m_first : 1));
    printf("    n_cards hist:"); for (int i = 1; i < 8; i++) printf(" %d:%ld", i, first_ncards[i]); printf("\n");
    printf("    led-value rank among held non-trump values:"); for (int i = 0; i < 8; i++) printf(" %d:%ld", i, first_rank_hist[i]); printf("\n");
    printf("    higher-value: single-over-lower-single=%ld pair-over-lower-single=%ld; fewer: single-of-pair=%ld\n",
           fh_single_over_lower_single, fh_pair_over_lower_single, fh_single_of_pair);
    printf("    other-cover: trump-instead-of-nontrump=%ld nontrump-instead-of-trump=%ld higher-same-trumpness=%ld other-assignment=%ld partial=%ld\n",
           oc_trump_for_nt, oc_nt_for_trump, oc_higher_same, oc_assign, oc_partial);
    printf("  throw-in decision: %ld  eq=%.1f%%  GOOD-instead=%.1f%%  fewer=%.1f%%  other=%.1f%%\n",
           m_thr, 100.0 * m_thr_eq / (m_thr ? m_thr : 1), 100.0 * m_thr_good / (m_thr ? m_thr : 1),
           100.0 * m_thr_fewer / (m_thr ? m_thr : 1), 100.0 * m_thr_other / (m_thr ? m_thr : 1));
    printf("  defense: %ld  eq=%.1f%%  pickup-instead=%.1f%%  other-cover=%.1f%%  no-pass=%.1f%%  other=%.1f%%\n",
           m_def, 100.0 * m_def_eq / (m_def ? m_def : 1), 100.0 * m_def_pick / (m_def ? m_def : 1),
           100.0 * m_def_cover_other / (m_def ? m_def : 1), 100.0 * m_def_nopass / (m_def ? m_def : 1),
           100.0 * m_def_other / (m_def ? m_def : 1));
    return 0;
}

// Distill feature extractor — cheap per-move scalars plus four "oracle"
// meta-features (does this candidate equal the pick of each fast heuristic
// bot?). The oracles run once per decision (see distill_decision_reset) and
// are strong priors: they cost a few microseconds combined, versus cordite's
// milliseconds of Monte-Carlo.
//
// RNG discipline: espresso_prod / handwritten_prod / champion consume the
// random_strategy LCG stream (their Math.random mirror). Before running them
// we re-seed that LCG from a hash of the public decision state, so the
// oracle answers are a pure function of the position (reproducible between
// the dump harness and the inference strategy). Afterwards we restore the
// stream to its pre-observation state exactly: the LCG state is recovered by
// drawing once (the draw returns state/2^32, and a uint32 is exact in a
// double) and inverting the LCG step (multiplier inverse mod 2^32), so the
// outer harness's random stream is not perturbed by feature extraction.
// (cordite itself uses the SEPARATE game RNG, which it saves/restores; the
// oracles here never touch that stream.)
//
// wasm-clean: C99, no libm, no exp/sqrt, plain arithmetic only.

#include "distill_feat.h"
#include "strategy.h"
#include "card.h"

// ---------- random_strategy LCG snapshot/restore -------------------------

// game.c: state = state * 1664525 + 1013904223 (mod 2^32), draw = state/2^32.
// 4276115653 is the inverse of 1664525 mod 2^32. Recover the CURRENT state by
// drawing once and stepping back. (If the recovered state is 0 — probability
// 2^-32 — random_strategy_set_seed maps it to 1; accepted.)
static uint32_t df_rs_state(void) {
    double r = random_strategy_random();
    uint32_t after = (uint32_t)(r * 4294967296.0);
    return (after - 1013904223u) * 4276115653u;
}

static uint32_t df_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// ---------- per-decision cache --------------------------------------------

typedef struct {
    int valid;
    // Oracle picks (indices into all->moves; -1 = oracle abstained).
    int hp_idx, sh_idx, ch_idx, ep_idx;
    // State scalars, identical for every candidate of the decision.
    double st[15];
    // Decision-level aggregates over the CANDIDATE LIST (state features
    // cancel inside a within-decision comparison, so "pick up when every
    // cover is expensive" is only expressible as aggregate x type-one-hot
    // interactions on the pickup/good rows).
    double min_cover_cost;    // cheapest cover's sum(value + 20 per trump); 0 if none
    double min_cover_trumps;  // trumps that cheapest cover spends
    double min_attack_cost;   // cheapest attack's sum cost; 0 if none
} DecisionCache;

static _Thread_local DecisionCache df_cache;
// Within-type rank of each candidate under cordite's stage-0 candidate keys
// (cd_pick_candidates), and whether it survives cordite's per-type candidate
// caps (attack 12 / cover 10 / pass 3, good+pickup always). Moves outside
// that set can never be cordite's answer — a hard prior worth learning.
static _Thread_local int8_t df_rank[MAX_LEGAL_MOVES];
static _Thread_local int8_t df_in_cand[MAX_LEGAL_MOVES];
static _Thread_local double df_key[MAX_LEGAL_MOVES];
static _Thread_local int    df_ord[MAX_LEGAL_MOVES];

void distill_decision_reset(void) { df_cache.valid = 0; }

static int df_uncovered(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_battles; i++)
        if (!!card_is_none(g->table_battles[i].defense)) n++;
    return n;
}

// Mirror of cordite's cd_card_score / cd_pick_candidates stage-0 keys
// (lower = tried first): attack -n*10000+sum(score), cover prod(score)-n/2,
// pass sum(score). Rank ties break like cd_ranked_insert: earlier enumerated
// wins.
static double df_cd_key(const LegalMove *m, int power) {
    double sum = 0.0, prod = 1.0;
    for (int j = 0; j < m->n_cards; j++) {
        int sc = m->cards[j].value + (m->cards[j].suit == power ? 1000 : 0);
        sum += (double)sc;
        prod *= (double)sc;
    }
    switch (m->type) {
        case MOVE_ATTACK: return -(double)m->n_cards * 10000.0 + sum;
        case MOVE_COVER:  return prod - (double)m->n_cards * 0.5;
        default:          return sum;   // pass; good/pickup are rank 0 anyway
    }
}

static void df_fill_ranks(const Game *g, const LegalMoves *all) {
    int power = g->power_suit;
    df_cache.min_cover_cost = 0.0;
    df_cache.min_cover_trumps = 0.0;
    df_cache.min_attack_cost = 0.0;
    bool have_cover = false, have_attack = false;
    for (int i = 0; i < all->n; i++) {
        const LegalMove *mi = &all->moves[i];
        if (mi->type != MOVE_COVER && mi->type != MOVE_ATTACK) continue;
        double cost = 0.0, tr = 0.0;
        for (int j = 0; j < mi->n_cards; j++) {
            cost += (double)mi->cards[j].value
                  + (mi->cards[j].suit == power ? 20.0 : 0.0);
            if (mi->cards[j].suit == power) tr += 1.0;
        }
        if (mi->type == MOVE_COVER) {
            if (!have_cover || cost < df_cache.min_cover_cost) {
                df_cache.min_cover_cost = cost;
                df_cache.min_cover_trumps = tr;
                have_cover = true;
            }
        } else if (!have_attack || cost < df_cache.min_attack_cost) {
            df_cache.min_attack_cost = cost;
            have_attack = true;
        }
    }
    // Rank within type by (key, enumeration index) — the cd_ranked_insert
    // order. Keys are computed once; a shell sort over (type, key, idx)
    // keeps big move lists (hundreds of cover combinations) cheap and is
    // wasm-clean (no qsort).
    int n = all->n;
    for (int i = 0; i < n; i++) {
        df_key[i] = df_cd_key(&all->moves[i], power);
        df_ord[i] = i;
    }
    for (int gap = n / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < n; i++) {
            int v = df_ord[i];
            int8_t vt = all->moves[v].type;
            int j = i;
            while (j >= gap) {
                int u = df_ord[j - gap];
                int8_t ut = all->moves[u].type;
                if (ut < vt || (ut == vt && (df_key[u] < df_key[v]
                    || (df_key[u] == df_key[v] && u < v)))) break;
                df_ord[j] = u;
                j -= gap;
            }
            df_ord[j] = v;
        }
    }
    int rank = 0;
    int8_t prev_type = -1;
    for (int k = 0; k < n; k++) {
        int i = df_ord[k];
        int8_t t = all->moves[i].type;
        rank = (t == prev_type) ? rank + 1 : 0;
        prev_type = t;
        if (t != MOVE_ATTACK && t != MOVE_COVER && t != MOVE_PASS) {
            df_rank[i] = 0;
            df_in_cand[i] = 1;
            continue;
        }
        int cap = (t == MOVE_ATTACK) ? 12 : (t == MOVE_COVER) ? 10 : 3;
        df_rank[i] = (int8_t)(rank > 120 ? 120 : rank);
        df_in_cand[i] = (rank < cap) ? 1 : 0;
    }
}

static void df_fill_decision(const Game *g, int bot_idx, const LegalMoves *all) {
    const Player *me = &g->players[bot_idx];
    int power = g->power_suit;

    df_fill_ranks(g, all);

    int in_count = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) in_count++;

    int trumps = 0, vmin = 0, vmax = 0, vsum = 0;
    for (int j = 0; j < me->hand_count; j++) {
        int v = me->hand[j].value;
        if (me->hand[j].suit == power) trumps++;
        if (j == 0 || v < vmin) vmin = v;
        if (j == 0 || v > vmax) vmax = v;
        vsum += v;
    }

    int next = get_next_player_index(g, bot_idx);

    double *st = df_cache.st;
    st[0]  = (double)g->num_players;
    st[1]  = (double)in_count;
    st[2]  = (double)me->hand_count;
    st[3]  = (double)g->players[g->defender].hand_count;
    st[4]  = (double)g->players[next].hand_count;
    st[5]  = (double)g->deck_count;
    st[6]  = (double)g->num_battles;
    st[7]  = (double)df_uncovered(g);
    st[8]  = (bot_idx == g->defender) ? 1.0 : 0.0;
    st[9]  = (bot_idx == g->first_attacker) ? 1.0 : 0.0;
    st[10] = (double)trumps;
    st[11] = (double)vmin;
    st[12] = (double)vmax;
    st[13] = me->hand_count ? (double)vsum / (double)me->hand_count : 0.0;
    st[14] = (g->deck_count > 0 || g->has_flipped) ? 1.0 : 0.0;

    // Oracles. Re-seed the random_strategy stream deterministically from the
    // public decision state (each oracle gets its own sub-seed so one
    // oracle's draw count never shifts another's stream), then restore.
    uint32_t saved = df_rs_state();
    uint32_t base = df_mix((uint32_t)g->num_logs * 2654435761u,
                           ((uint32_t)g->deck_count << 10)
                           ^ ((uint32_t)bot_idx << 20)
                           ^ (uint32_t)all->n);

    random_strategy_set_seed(df_mix(base, 0xD1571001u));
    df_cache.hp_idx = handwritten_prod_strategy_choose(g, bot_idx, all, (void *)0);
    df_cache.sh_idx = simple_heuristic_strategy_choose(g, bot_idx, all, (void *)0);
    random_strategy_set_seed(df_mix(base, 0xD1571002u));
    df_cache.ch_idx = champion_strategy_choose(g, bot_idx, all, (void *)0);
    random_strategy_set_seed(df_mix(base, 0xD1571003u));
    df_cache.ep_idx = espresso_prod_strategy_choose(g, bot_idx, all, (void *)0);

    random_strategy_set_seed(saved);
    df_cache.valid = 1;
}

// ---------- feature vector -------------------------------------------------

// Layout (keep in sync with DISTILL_NUM_FEATURES and distill_train.py):
//   0..14  state: num_players, in_count, my/defender/next hand counts,
//          deck_count, num_battles, uncovered, am_defender,
//          am_first_attacker, my trump count, my min/max/avg value,
//          deck_alive
//  15..19  move type one-hots: attack, cover, pass, pickup, good
//  20..28  move: n_cards, sum/min/max/avg value, trumps used, highest trump
//          spent, sum cost (value + 20 per trump), hand fraction spent
//  29      cover overkill: sum(cover cost - attack cost) over cover pairs
//  30      leaves hand empty (card-spending move plays the whole hand)
//  31      cards left in hand after the move
//  32..35  oracle meta: equals handwritten_prod / simple_heuristic /
//          champion / espresso_prod pick
//  36..43  interactions: deck_alive*trumps_used, deck_alive*sum_value,
//          endgame going-out, pickup*uncovered, good*num_battles,
//          attack*defender_hand, endgame trump spend, move_min - hand_min
//  44      in cordite's stage-0 candidate set (per-type key caps)
//  45..47  within-type cd-key rank+1, split per type: attack, cover, pass
//  48      best of its type under the cd key (rank 0)
//  49..54  alternative-cost interactions: pickup * min cover cost,
//          pickup * cheapest-cover trumps, pickup * deck_alive,
//          good * min attack cost, good * deck_alive, cover * (own cost -
//          min cover cost)
int distill_features(const Game *g, int bot_idx, const LegalMove *m,
                     const LegalMoves *all, double *out) {
    if (!df_cache.valid) df_fill_decision(g, bot_idx, all);
    const double *st = df_cache.st;
    int power = g->power_suit;
    int idx = (int)(m - all->moves);

    for (int i = 0; i < 15; i++) out[i] = st[i];

    out[15] = (m->type == MOVE_ATTACK) ? 1.0 : 0.0;
    out[16] = (m->type == MOVE_COVER)  ? 1.0 : 0.0;
    out[17] = (m->type == MOVE_PASS)   ? 1.0 : 0.0;
    out[18] = (m->type == MOVE_PICKUP) ? 1.0 : 0.0;
    out[19] = (m->type == MOVE_GOOD)   ? 1.0 : 0.0;

    int n = m->n_cards;
    int msum = 0, mmin = 0, mmax = 0, mtr = 0, mtr_hi = 0, mcost = 0;
    for (int j = 0; j < n; j++) {
        int v = m->cards[j].value;
        int tr = (m->cards[j].suit == power);
        msum += v;
        if (j == 0 || v < mmin) mmin = v;
        if (j == 0 || v > mmax) mmax = v;
        mcost += v + (tr ? 20 : 0);
        if (tr) { mtr++; if (v > mtr_hi) mtr_hi = v; }
    }
    double hand = st[2] > 0 ? st[2] : 1.0;
    out[20] = (double)n;
    out[21] = (double)msum;
    out[22] = (double)mmin;
    out[23] = (double)mmax;
    out[24] = n ? (double)msum / (double)n : 0.0;
    out[25] = (double)mtr;
    out[26] = (double)mtr_hi;
    out[27] = (double)mcost;
    out[28] = (double)n / hand;

    double overkill = 0.0;
    if (m->type == MOVE_COVER) {
        for (int j = 0; j < n; j++) {
            int cc = m->cards[j].value + (m->cards[j].suit == power ? 20 : 0);
            int ac = m->attack_cards[j].value
                   + (m->attack_cards[j].suit == power ? 20 : 0);
            overkill += (double)(cc - ac);
        }
    }
    out[29] = overkill;

    bool spends = (m->type == MOVE_ATTACK || m->type == MOVE_COVER
                   || m->type == MOVE_PASS);
    out[30] = (spends && n > 0 && (double)n >= st[2]) ? 1.0 : 0.0;
    out[31] = spends ? st[2] - (double)n : st[2];

    out[32] = (idx == df_cache.hp_idx) ? 1.0 : 0.0;
    out[33] = (idx == df_cache.sh_idx) ? 1.0 : 0.0;
    out[34] = (idx == df_cache.ch_idx) ? 1.0 : 0.0;
    out[35] = (idx == df_cache.ep_idx) ? 1.0 : 0.0;

    double deck_alive = st[14];
    out[36] = deck_alive * (double)mtr;
    out[37] = deck_alive * (double)msum;
    out[38] = (1.0 - deck_alive) * out[30];        // going out in the endgame
    out[39] = out[18] * st[7];                     // pickup * uncovered count
    out[40] = out[19] * st[6];                     // good * num_battles
    out[41] = out[15] * st[3];                     // attack * defender hand
    out[42] = (1.0 - deck_alive) * (double)mtr;    // endgame trump spend
    out[43] = n ? (double)mmin - st[11] : 0.0;     // playing above my minimum

    double rank1 = (double)(df_rank[idx] + 1);
    out[44] = (double)df_in_cand[idx];
    out[45] = out[15] * rank1;                     // attack rank+1
    out[46] = out[16] * rank1;                     // cover rank+1
    out[47] = out[17] * rank1;                     // pass rank+1
    out[48] = (df_rank[idx] == 0) ? 1.0 : 0.0;

    out[49] = out[18] * df_cache.min_cover_cost;
    out[50] = out[18] * df_cache.min_cover_trumps;
    out[51] = out[18] * deck_alive;
    out[52] = out[19] * df_cache.min_attack_cost;
    out[53] = out[19] * deck_alive;
    out[54] = out[16] * ((double)mcost - df_cache.min_cover_cost);

    return DISTILL_NUM_FEATURES;
}

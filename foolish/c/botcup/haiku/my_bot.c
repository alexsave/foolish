// haiku: determinized Monte Carlo over the legal menu.
//
// For each decision the bot samples several complete worlds consistent with
// what it can see (its own hand, the table, the discard pile, the trump and
// the hand counts), applies each legal move in each world, and plays the rest
// of the game out with a fast heuristic policy for every seat. The move with
// the best average placement points for this seat wins. Candidate moves share
// the same sampled worlds and the same rollout random streams (common random
// numbers), so the comparison between moves is not drowned in sampling noise.
#include "botcup.h"
#include <limits.h>
#ifdef HK_STATS
#include <stdio.h>
#endif
#include <string.h>
#include <time.h>

#define HK_MAXC       12      // candidate moves evaluated per decision
#define HK_MAXW       64      // worlds sampled per decision at most
#define HK_BUDGET_MS  4.0     // soft per-decision time budget
#define HK_ROLL_STEPS 800     // hard cap on a rollout's referee steps

static double hk_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

// xorshift32 over a caller-held state; never zero.
static uint32_t hk_rnd(uint32_t *s) {
    uint32_t x = *s ? *s : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// Persistent state for the decision stream (not shared with the referee).
static uint32_t hk_stream = 0x6A09E667u;

static Game hk_world;     // sampled world (scratch)
static Game hk_trial;     // candidate applied to a world (scratch)
static LegalMoves hk_rmoves;  // rollout menu (scratch)

static int hk_uncovered(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_battles; i++)
        if (card_is_none(g->table_battles[i].defense)) n++;
    return n;
}

// Heuristic weights. Experiment builds (-DHK_TUNE) read them from HKW, a comma
// list in this order, so a sweep needs no rebuild.
enum {
    W_A_BASE, W_A_VAL, W_A_TRUMP, W_A_CARD,          // first lead
    W_T_BASE, W_T_VAL, W_T_TRUMP, W_T_CARD, W_T_MARGIN, // throw-in
    W_C_BASE, W_C_CARD, W_C_COST, W_C_FULL, W_C_TRUMPCOST, // cover
    W_P_BASE, W_P_CARD, W_P_COST,                    // pass
    W_U_BASE, W_U_TABLE,                             // pickup
    W_G_ALL, W_G_NO,                                 // good
    W_COUNT
};
static int hk_w[W_COUNT] = {
    100, 1, 25, 20,
    93, 0, 6, 15, 30,
    20, 15, 1, 32, 20,
    60, 15, 1,
    5, 3,
    70, 10,
};

#ifdef HK_TUNE
#include <stdio.h>
#include <stdlib.h>
static void hk_load_weights(void) {
    const char *e = getenv("HKW");
    int i = 0;
    while (e && *e && i < W_COUNT) {
        hk_w[i++] = (int)strtol(e, (char **)&e, 10);
        if (*e == ',') e++;
    }
}
#endif

static int hk_cost(Card c, int trump) {
    return c.value * hk_w[W_C_COST] + (c.suit == trump ? hk_w[W_C_TRUMPCOST] : 0);
}

// Heuristic preference for a menu entry. Only the ORDER matters inside a
// rollout; at the top level it is the prior that orders the candidates.
static int hk_score(const Game *g, const LegalMove *m) {
    const int trump = g->power_suit;
    int sumv = 0, trumps = 0, cost = 0;
    const int k = m->n_cards;
    for (int i = 0; i < k; i++) {
        sumv += m->cards[i].value;
        if (m->cards[i].suit == trump) trumps++;
        cost += hk_cost(m->cards[i], trump);
    }
    switch (m->type) {
    case MOVE_ATTACK:
        if (g->num_battles == 0)
            return hk_w[W_A_BASE] - hk_w[W_A_VAL] * sumv - hk_w[W_A_TRUMP] * trumps + hk_w[W_A_CARD] * k;
        {
            int margin = g->players[g->defender].hand_count - hk_uncovered(g) - k;
            int s = hk_w[W_T_BASE] - hk_w[W_T_VAL] * sumv - hk_w[W_T_TRUMP] * trumps + hk_w[W_T_CARD] * k;
            if (margin < 1) s -= hk_w[W_T_MARGIN];
            return s;
        }
    case MOVE_COVER: {
        int unc = hk_uncovered(g);
        int s = hk_w[W_C_BASE] + hk_w[W_C_CARD] * k - cost;
        if (k >= unc) s += hk_w[W_C_FULL];
        return s;
    }
    case MOVE_PASS:
        return hk_w[W_P_BASE] + hk_w[W_P_CARD] * k - hk_w[W_P_COST] * cost;
    case MOVE_PICKUP: {
        int table = 0;
        for (int i = 0; i < g->num_battles; i++)
            table += 1 + (card_is_none(g->table_battles[i].defense) ? 0 : 1);
        return hk_w[W_U_BASE] - hk_w[W_U_TABLE] * table;
    }
    case MOVE_GOOD:
        return (g->num_battles > 0 && hk_uncovered(g) == 0) ? hk_w[W_G_ALL] : hk_w[W_G_NO];
    default:
        return 0;
    }
}

// Best-scoring entry of the menu; equal scores are broken uniformly at random.
static int hk_policy_pick(const Game *g, const LegalMoves *m, uint32_t *rs) {
    int best = 0, best_s = INT_MIN, ties = 0;
    for (int i = 0; i < m->n; i++) {
        int s = hk_score(g, &m->moves[i]);
        if (s > best_s) { best_s = s; best = i; ties = 1; }
        else if (s == best_s) {
            ties++;
            if (hk_rnd(rs) % (uint32_t)ties == 0) best = i;
        }
    }
    return best;
}

// Plays `t` to the end with the heuristic policy for every seat. Returns the
// points `me` ends with (num_players - place). A game that does not finish
// (the engine can cycle pass/pickup forever in a two-seat endgame) is worth 0
// to everyone in the referee, so it is worth 0 here too: the search must not
// prefer lines that loop.
static int hk_rollout(Game *t, int me, uint32_t *rs) {
    for (int it = 0; it < HK_ROLL_STEPS && game_done(t) < 0; it++) {
        int order[MAX_PLAYERS], ne = 0;
        for (int s = 0; s < t->num_players; s++)
            if (should_bot_act(t, s)) order[ne++] = s;
        if (ne == 0) break;
        for (int i = ne - 1; i > 0; i--) {
            int j = (int)(hk_rnd(rs) % (uint32_t)(i + 1));
            int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }
        bool acted = false;
        for (int k = 0; k < ne && !acted; k++) {
            int s = order[k];
            calculate_legal_moves_lite(t, s, &hk_rmoves);
            if (hk_rmoves.n == 0) continue;
            int idx = hk_policy_pick(t, &hk_rmoves, rs);
            acted = legal_move_apply(t, s, &hk_rmoves.moves[idx]);
        }
        if (!acted) return 0;
    }
    if (game_done(t) < 0) return 0;
    for (int i = 0; i < t->num_eliminated; i++)
        if (t->elimination_order[i] == me) return t->num_players - (i + 1);
    return 0;
}

// Where a card was last seen, read from the public logs. A card that has not
// been seen moving is UNKNOWN (stock or somebody's hand); a picked-up card is
// known to be in the picker's hand until it is played.
#define HK_LOC_UNK   (-1)
#define HK_LOC_TABLE (-2)
#define HK_LOC_DISC  (-3)

#define HK_ID(c) ((c).suit * 13 + (c).value - 1)

static void hk_track(const Game *view, int8_t loc[52]) {
    const int minv = min_value_for(view->num_players);
    for (int id = 0; id < 52; id++) loc[id] = HK_LOC_UNK;
    for (int k = 0; k < view->num_logs; k++) {
        const GameLog *l = &view->logs[k];
        int8_t where;
        switch (l->log_type) {
        case LOG_DRAW:    where = l->player_idx; break;     // masked draws are skipped below
        case LOG_PICKUP:  where = l->player_idx; break;
        case LOG_ATTACK:
        case LOG_COVER:
        case LOG_PASS:    where = HK_LOC_TABLE; break;
        case LOG_DISCARD: where = HK_LOC_DISC; break;
        default:          continue;
        }
        for (int i = 0; i < l->num_pairs; i++) {
            Card c = l->pairs[i].primary;
            if (c.suit < 0 || c.suit > 3 || c.value < minv || c.value > ACE_VALUE) continue;  // hidden
            loc[HK_ID(c)] = where;
        }
    }
}

// Fills `w` with a world consistent with what `me` can see. Cards that were
// seen moving into another seat's hand go to that seat; everything else that is
// unseen is dealt at random into the stock and the other hands in the counts
// the view reports. If the tracked facts do not reconcile with those counts (a
// truncated log, a log that was dropped), the whole deal falls back to a uniform
// sample of the cards that are not visibly placed.
static void hk_sample_world(const Game *view, int me, const int8_t loc[52], Game *w, uint32_t *rs) {
    game_clone(w, view);
    const int np = view->num_players;
    const int minv = min_value_for(np);

    bool in_hand[52] = { false };
    for (int i = 0; i < view->players[me].hand_count; i++) in_hand[HK_ID(view->players[me].hand[i])] = true;
    bool flip[52] = { false };
    if (view->has_flipped) flip[HK_ID(view->flipped)] = true;

    int need = view->deck_count;
    for (int p = 0; p < np; p++) if (p != me) need += view->players[p].hand_count;

    // Tracked pools.
    int free_[52], nf = 0;
    int forced[MAX_PLAYERS][52], nfor[MAX_PLAYERS];
    for (int p = 0; p < np; p++) nfor[p] = 0;
    for (int suit = 0; suit < NUM_SUITS; suit++)
        for (int v = minv; v <= ACE_VALUE; v++) {
            int id = suit * 13 + (v - 1);
            if (in_hand[id] || flip[id]) continue;
            int8_t where = loc[id];
            if (where == HK_LOC_UNK) free_[nf++] = id;
            else if (where >= 0 && where < np && where != me) forced[where][nfor[where]++] = id;
        }
    for (int i = nf - 1; i > 0; i--) {
        int j = (int)(hk_rnd(rs) % (uint32_t)(i + 1));
        int tmp = free_[i]; free_[i] = free_[j]; free_[j] = tmp;
    }
    // Surplus free cards are discards that the log no longer shows.
    int total = nf;
    for (int p = 0; p < np; p++) total += nfor[p];
    bool ok = total >= need;
    for (int p = 0; p < np && ok; p++)
        if (p != me && nfor[p] > view->players[p].hand_count) ok = false;
    if (ok) {
        int drop = total - need;             // trimmed from the free pool only
        if (drop > nf) ok = false;
        else nf -= drop;
    }

#ifdef HK_STATS
    { static long n_w, n_fb; n_w++; if (!ok) n_fb++; if (n_w % 20000 == 0) fprintf(stderr, "HKSTAT worlds=%ld fallback=%ld\n", n_w, n_fb); }
#endif
#ifdef HK_NO_TRACK              // experiment builds: uniform sampling only
    ok = false;
#endif
    if (ok) {
        int ptr = 0;
        for (int p = 0; p < np; p++) {
            if (p == me) continue;
            int hc = view->players[p].hand_count;
            int k = 0;
            for (int i = 0; i < nfor[p]; i++) w->players[p].hand[k++] = card_of_id(forced[p][i]);
            while (k < hc) w->players[p].hand[k++] = card_of_id(free_[ptr++]);
        }
        for (int i = 0; i < view->deck_count; i++) w->deck[i] = card_of_id(free_[ptr++]);
        return;
    }

    // Fallback: uniform over everything not visibly placed.
    bool known[52] = { false };
    for (int i = 0; i < view->num_battles; i++) {
        known[HK_ID(view->table_battles[i].attack)] = true;
        if (!card_is_none(view->table_battles[i].defense)) known[HK_ID(view->table_battles[i].defense)] = true;
    }
    for (int k = 0; k < view->num_logs; k++) {
        const GameLog *l = &view->logs[k];
        if (l->log_type != LOG_DISCARD) continue;
        for (int i = 0; i < l->num_pairs; i++) {
            Card c = l->pairs[i].primary;
            if (c.suit >= 0 && c.suit <= 3 && c.value >= 1 && c.value <= ACE_VALUE) known[HK_ID(c)] = true;
        }
    }
    int pool[52], np_ = 0;
    for (int id = 0; id < 52; id++) {
        const int v = id % 13 + 1;
        if (v < minv || in_hand[id] || flip[id] || known[id]) continue;
        pool[np_++] = id;
    }
        for (int i = np_ - 1; i > 0; i--) {
        int j = (int)(hk_rnd(rs) % (uint32_t)(i + 1));
        int tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
    }
    if (np_ > need) np_ = need;
    int ptr = 0;
    for (int p = 0; p < np; p++) {
        if (p == me) continue;
        for (int i = 0; i < view->players[p].hand_count; i++) {
            int id = ptr < np_ ? pool[ptr++] : (np_ > 0 ? pool[hk_rnd(rs) % (uint32_t)np_] : 0);
            w->players[p].hand[i] = card_of_id(id);
        }
    }
    for (int i = 0; i < view->deck_count; i++) {
        int id = ptr < np_ ? pool[ptr++] : (np_ > 0 ? pool[hk_rnd(rs) % (uint32_t)np_] : 0);
        w->deck[i] = card_of_id(id);
    }
}

static int hk_choose(const Game *view, int seat, const LegalMoves *moves) {
#ifdef HK_TUNE
    static bool loaded = false;
    if (!loaded) { hk_load_weights(); loaded = true; }
#endif
    const int n = moves->n;
    if (n <= 1) return 0;

    // Candidate set: the whole menu when it is small, else the best-prior few.
    int prior[MAX_LEGAL_MOVES];
    int order[MAX_LEGAL_MOVES];
    for (int i = 0; i < n; i++) { prior[i] = hk_score(view, &moves->moves[i]); order[i] = i; }
    for (int i = 1; i < n; i++) {           // stable insertion sort, best prior first
        int x = order[i], j = i;
        while (j > 0 && prior[order[j - 1]] < prior[x]) { order[j] = order[j - 1]; j--; }
        order[j] = x;
    }
    const int nc = n < HK_MAXC ? n : HK_MAXC;
    int8_t loc[52];
    hk_track(view, loc);

    double sum[HK_MAXC];
    int cnt[HK_MAXC];
    for (int c = 0; c < nc; c++) { sum[c] = 0.0; cnt[c] = 0; }

    const double t0 = hk_now_ms();
    (void)t0;
    int worlds = 0;
#ifdef HK_PRIOR_ONLY            // experiment builds: the heuristic prior alone
    worlds = HK_MAXW;
    (void)t0;
#else
    do {
        const uint32_t base = hk_rnd(&hk_stream);
        uint32_t ws = base;
        hk_sample_world(view, seat, loc, &hk_world, &ws);
        for (int c = 0; c < nc; c++) {
            game_clone(&hk_trial, &hk_world);
            if (!legal_move_apply(&hk_trial, seat, &moves->moves[order[c]])) continue;
            uint32_t rs = base ^ 0x9E3779B9u;      // same rollout stream for every candidate
            sum[c] += hk_rollout(&hk_trial, seat, &rs);
            cnt[c]++;
        }
        worlds++;
#ifdef HK_FIXED_WORLDS          // experiment builds: a fixed world count, so runs repeat
    } while (worlds < HK_FIXED_WORLDS);
#else
    } while (worlds < HK_MAXW && hk_now_ms() - t0 < HK_BUDGET_MS);
#endif
#endif

    int best = 0;
    double best_avg = -1.0;
    for (int c = 0; c < nc; c++) {
        if (cnt[c] == 0) continue;
        double avg = sum[c] / cnt[c];
        if (avg > best_avg) { best_avg = avg; best = c; }
    }
    return order[best];
}

const BotEntry my_bot = { "haiku", hk_choose, NULL };

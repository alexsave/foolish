// gd_bot_moscow - see gd_bot_moscow.h. Each step names its twin in foolish's
// cordite_strategy.c.
#include "gd_bot_moscow.h"
#include "gd_sim.h"
#include "gd_solve.h"
#include "gd_rng.h"
#include "gd_bot_hw.h"
#ifdef MOSCOW_TRACE
#include <stdio.h>
#endif

uint8_t gd_moscow_cands, gd_moscow_solve;
uint16_t gd_moscow_playouts;
uint32_t gd_moscow_plies, gd_moscow_nodes;

#define NW (MOSCOW_W1 + MOSCOW_W2)

static gd_move cand[MOSCOW_MAX_CANDS];
static uint8_t nc;
static gd_game world;
static gd_sim root, trial, wsim;
static uint16_t world_seed[NW];
static uint8_t wins[MOSCOW_MAX_CANDS], alive[MOSCOW_MAX_CANDS];
static gd_move mv;

// ---- the endgame (cd_try_endgame_solve) --------------------------------------------

// Cordite's pass 1: hunt a proven win, all root moves sharing one budget, the
// window from 0 up (losing lines prune at once) and rising to the best win
// found. Returns the menu index of the fastest proven win, or -1. Its pass 2
// (with no win, drop the moves proven to lose when some move is proven not
// to) is left out: at two players a draw needs both seats out at once, which
// the rules make all but impossible, so a move proven not to lose is a win,
// which pass 1 already took; in 10,000 solves against Seoul it never fired.
static int16_t endgame(const gd_game *g, uint8_t seat, uint16_t n) {
    uint32_t bud = MOSCOW_WIN_NODES;
    int16_t alpha = 0, best_v = 0, v, best = -1;
    uint8_t ab, any_abort = 0;
    uint16_t i;
    gd_solve_reset();
    gd_sim_from_game(&root, g);
    for (i = 0; i < n; i++) {
        gd_menu_move(g, seat, i, &mv);
        trial = root;
        gd_sim_apply_move(&trial, seat, &mv);
        v = gd_solve(&trial, seat, alpha, 2000, &bud, 1, &ab);
        if (!bud) { gd_moscow_solve = MOSCOW_SOLVE_ABORT; return -1; }   // drained: no claims
        if (ab) { any_abort = 1; continue; }
        if (v > best_v) { best_v = v; best = (int16_t)i; }
        if (v > alpha) alpha = v;
    }
    if (best >= 0) gd_moscow_solve = MOSCOW_SOLVE_WIN;
    else gd_moscow_solve = any_abort ? MOSCOW_SOLVE_ABORT : MOSCOW_SOLVE_LOST;
    return best;
}

// The same move: kind and card set (a cover's pairing aside).
static uint8_t same_move(const gd_move *a, const gd_move *b) {
    uint8_t i, j, f;
    if (a->type != b->type || a->n != b->n) return 0;
    for (i = 0; i < a->n; i++) {
        f = 0;
        for (j = 0; j < b->n; j++) if (a->cards[i] == b->cards[j]) { f = 1; break; }
        if (!f) return 0;
    }
    return 1;
}

// ---- the candidates (cd_pick_candidates) --------------------------------------------

// cd_card_score: the value, plus 1000 for a trump (rank + 6 here: the same order)
static uint16_t score(const gd_game *g, uint8_t c) {
    return (uint16_t)(gd_rank_of[c] + 6u + (gd_suit_of[c] == g->trump ? 1000u : 0u));
}

// The candidates, each kind ranked by cordite's key (lower first; a tie keeps
// the menu's order) and cut: 5 attacks (most cards, then the lowest sum),
// 3 covers (the lowest product of scores, then the most cards), 2 transfers
// (the lowest sum), then done and pick up; at most 6 in all.
#define CAP_ATK 5
#define CAP_COV 3
#define CAP_PAS 2
static gd_move list[3][CAP_ATK];
static uint32_t lkey[3][CAP_ATK];
static uint8_t ln[3];

static void ranked(uint8_t kind, uint8_t cap, uint32_t k, const gd_move *m) {
    uint8_t pos = ln[kind], j;
    while (pos > 0 && lkey[kind][pos - 1] > k) pos--;
    if (pos >= cap) return;
    j = ln[kind] < cap ? ln[kind] : (uint8_t)(cap - 1u);
    for (; j > pos; j--) { list[kind][j] = list[kind][j - 1]; lkey[kind][j] = lkey[kind][j - 1]; }
    list[kind][pos] = *m;
    lkey[kind][pos] = k;
    if (ln[kind] < cap) ln[kind]++;
}

static void candidates(const gd_game *g, uint8_t seat, uint16_t n) {
    uint16_t i;
    uint8_t j, k, has_good = 0, has_pickup = 0;
    uint32_t sum, prod;
    gd_move good, pickup;
    ln[0] = ln[1] = ln[2] = 0;
    nc = 0;
    for (i = 0; i < n; i++) {
        gd_menu_move(g, seat, i, &mv);
        switch (mv.type) {
        case GD_ATTACK:
            for (sum = 0, j = 0; j < mv.n; j++) sum += score(g, mv.cards[j]);
            ranked(0, CAP_ATK, (uint32_t)(GD_MAXB - mv.n) * 100000ul + sum, &mv);
            break;
        case GD_COVER:
            // cordite's prod - 0.5 n, doubled: exact until the product passes
            // 2^31 (four trumps in one cover), where it saturates
            for (prod = 1, j = 0; j < mv.n; j++) {
                const uint16_t sc = score(g, mv.cards[j]);
                prod = prod > 0x7FFFFFFFul / sc ? 0x7FFFFFFFul : prod * sc;
            }
            ranked(1, CAP_COV, prod * 2u - mv.n, &mv);
            break;
        case GD_PASS:
            for (sum = 0, j = 0; j < mv.n; j++) sum += score(g, mv.cards[j]);
            ranked(2, CAP_PAS, sum, &mv);
            break;
        case GD_GOOD:   good = mv; has_good = 1; break;
        case GD_PICKUP: pickup = mv; has_pickup = 1; break;
        }
    }
    // New York's move first: Seoul's anchor, and the tie-break (not
    // cordite's: with a twelfth of its worlds, Moscow needs a strong first
    // guess; measured +6 points against Seoul at the same time)
    gd_hw_choose(g, seat, &mv);
    if (mv.type != 0xFF && gd_legal(g, seat, &mv)) cand[nc++] = mv;
    for (k = 0; k < 3; k++)
        for (j = 0; j < ln[k] && nc < MOSCOW_MAX_CANDS; j++) if (!same_move(&cand[0], &list[k][j]) || !nc) cand[nc++] = list[k][j];
    if (has_good && nc < MOSCOW_MAX_CANDS && (!nc || !same_move(&cand[0], &good))) cand[nc++] = good;
    if (has_pickup && nc < MOSCOW_MAX_CANDS && (!nc || !same_move(&cand[0], &pickup))) cand[nc++] = pickup;
}

// ---- a world (cd_sample_world) ---------------------------------------------------

// The cards Moscow cannot place, shuffled, the opponent's unknown cards first
// then the stock. With the voids (3 worlds of 4), the opponent's cards are the
// first ones it is not believed to lack; if too few are left, no voids.
static void sample_world(const gd_game *g, uint8_t me, uint8_t use_voids) {
    const uint8_t opp = (uint8_t)(me ^ 1u);
    uint8_t pool[GD_NCARDS], n = 0, c, i, j, t, need, allowed = 0, k;
    gd_mask seen = g->hand[me];
    for (i = 0; i < 5; i++) seen.b[i] |= g->gone.b[i] | g->known[opp].b[i];
    for (i = 0; i < g->nb; i++) { GD_SET(seen, g->att[i]); if (g->def[i] != GD_NONE) GD_SET(seen, g->def[i]); }
    if (g->has_flipped) GD_SET(seen, g->flipped);
    for (c = 0; c < GD_NCARDS; c++) if (!GD_HAS(seen, c)) pool[n++] = c;
    for (i = n; i > 1; i--) {
        j = gd_rng_small(i);
        t = pool[i - 1]; pool[i - 1] = pool[j]; pool[j] = t;
    }
    world = *g;
    need = (uint8_t)(gd_hand_n(g, opp) - gd_count(&g->known[opp]));
    world.hand[opp] = g->known[opp];
    if (use_voids) {
        for (i = 0; i < n; i++) if (!GD_HAS(g->nocover[opp], pool[i])) allowed++;
        if (allowed < need) use_voids = 0;
    }
    if (use_voids) {
        // the opponent's cards: the allowed ones in shuffled order; the stock:
        // everything else in shuffled order
        for (i = 0, k = 0; i < n && k < need; i++)
            if (!GD_HAS(g->nocover[opp], pool[i])) { GD_SET(world.hand[opp], pool[i]); pool[i] = GD_NONE; k++; }
        for (i = 0, j = 0; i < n && j < g->stock_n; i++)
            if (pool[i] != GD_NONE) world.stock[j++] = pool[i];
        return;
    }
    for (i = 0; i < need && i < n; i++) GD_SET(world.hand[opp], pool[i]);
    for (j = 0; j < g->stock_n && i < n; j++, i++) world.stock[j] = pool[i];
}

// ---- the decision (cordite_choose_impl) ---------------------------------------------

// Candidate k on the world in trial: its move, then the playout; 1 if Moscow
// won it.
static uint8_t play(uint8_t seat, uint8_t k) {
    gd_sim_apply_move(&trial, seat, &cand[k]);
    gd_moscow_playouts++;
    if (gd_sim_playout(&trial, seat, MOSCOW_MAX_TURNS) == 1) { gd_moscow_plies += gd_sim_plies; return 1; }
    gd_moscow_plies += gd_sim_plies;
    return 0;
}

static void worlds(const gd_game *g, uint8_t seat, uint8_t w_lo, uint8_t w_hi) {
    uint8_t w, k;
    uint16_t play_seed;
    for (w = w_lo; w < w_hi; w++) {
        // the time bound: no new world once the decision has spent its plies
        // (a solver node costs about 2.5 plies on the Game Boy: 8.2 ms, 3.3 ms)
        if (gd_moscow_plies + gd_moscow_nodes * 5u / 2u >= MOSCOW_PLY_BUDGET) return;
        gd_rng_set(world_seed[w]);
        sample_world(g, seat, (uint8_t)((w & 3u) != 3u));
        gd_sim_from_game(&wsim, &world);
        play_seed = gd_rng_u16();
        for (k = 0; k < nc; k++) {
            if (!alive[k]) continue;
            trial = wsim;
            gd_rng_set(play_seed);              // the same draws for every candidate
            wins[k] += play(seat, k);
        }
    }
}

void gd_moscow_choose(const gd_game *g, uint8_t seat, gd_move *out) GD_BANKED {
    uint16_t n, after;
    int16_t won;
    uint8_t k, n_alive, worst, best, w;
    out->type = 0xFF;
    out->n = 0;
    gd_moscow_cands = 0;
    gd_moscow_playouts = 0;
    gd_moscow_plies = 0;
    gd_moscow_nodes = 0;
    gd_moscow_solve = MOSCOW_SOLVE_NONE;
    {
        const uint32_t n32 = gd_menu_count(g, seat);
        if (!n32) return;
        n = n32 > 0xFFFFu ? 0xFFFFu : (uint16_t)n32;
    }
    if (n == 1) { gd_menu_move(g, seat, 0, out); return; }

    // the exact endgame: both hands known
    if (g->stock_n == 0 && !g->has_flipped && n <= MOSCOW_ROOT_MAX
        && gd_hand_n(g, 0) + gd_hand_n(g, 1) <= MOSCOW_SOLVE_CARDS) {
        won = endgame(g, seat, n);
        gd_moscow_nodes = gd_solve_nodes;
#ifdef MOSCOW_TRACE
        fprintf(stderr, "SOLVE %u %u %u %lu\n", gd_hand_n(g, 0) + gd_hand_n(g, 1), g->nb, gd_moscow_solve, (unsigned long)gd_solve_nodes);
#endif
        if (won >= 0) { gd_menu_move(g, seat, (uint16_t)won, out); return; }
    }

    candidates(g, seat, n);
    gd_moscow_cands = nc;
    if (!nc) return;
    if (nc == 1) { *out = cand[0]; return; }

    for (w = 0; w < NW; w++) {
        world_seed[w] = gd_rng_u16();
        if (!world_seed[w]) world_seed[w] = 1;
    }
    after = gd_rng_get();
    for (k = 0; k < nc; k++) { wins[k] = 0; alive[k] = 1; }

    // stage 1: every candidate on W1 worlds; keep the best two, dropping the
    // LAST of tied candidates (they are ranked cheapest first: cordite's fix)
    worlds(g, seat, 0, MOSCOW_W1);
    for (n_alive = nc; n_alive > 2; n_alive--) {
        worst = 0xFF;
        for (k = 0; k < nc; k++)
            if (alive[k] && (worst == 0xFF || wins[k] <= wins[worst])) worst = k;
        alive[worst] = 0;
    }
    // stage 2: the two on W2 more worlds
    worlds(g, seat, MOSCOW_W1, NW);
    best = 0xFF;
    for (k = 0; k < nc; k++)
        if (alive[k] && (best == 0xFF || wins[k] > wins[best])) best = k;   // the first of ties
    *out = cand[best];
    gd_rng_set(after);
}

// gd_bot_hw - see gd_bot_hw.h. Each branch names the place in foolish's
// handwritten_prod_strategy_choose it ports.
#include "gd_bot_hw.h"
#include "gd_rng.h"

#define TRUMP_SCORE 1000u            // cardScore: value + 1000 for a trump

// foolish card values are 5..13 (6..A); a rank here is 0..8.
#define VALUE(c) ((uint8_t)(gd_rank_of[c] + 5u))

static uint16_t card_score(uint8_t c, uint8_t trump) {
    return (uint16_t)(VALUE(c) + (gd_suit_of[c] == trump ? TRUMP_SCORE : 0u));
}

// ---- the soft double -------------------------------------------------------------------

void gd_dbl_one(gd_dbl *d) {
    d->x = 1;
    d->big = 0;
}

// An exact 32-bit value as the 53-bit form.
static void to_big(gd_dbl *d) {
    uint8_t i;
    int16_t sh = 52;
    uint32_t x = d->x;
    for (i = 0; i < 8; i++) d->m[i] = 0;
    while (!(x & 0x80000000ul)) { x <<= 1; sh++; }     // x != 0
    // x now has its top bit at 31: m = x << 21
    sh -= 31;
    d->m[2] = (uint8_t)(x << 5);
    d->m[3] = (uint8_t)(x >> 3);
    d->m[4] = (uint8_t)(x >> 11);
    d->m[5] = (uint8_t)(x >> 19);
    d->m[6] = (uint8_t)(x >> 27);
    d->e = (int16_t)-sh;
    d->big = 1;
}

static void big_mul(gd_dbl *d, uint16_t f);

// The largest x with x * f in 32 bits, for each card score f (5..13, and
// 1005..1013 for a trump): a table, because a 32-bit division per multiply
// was most of New York's time on the Game Boy.
#define LIM(f) (0xFFFFFFFFul / (f))
static const uint32_t fit32[18] = {
    LIM(5), LIM(6), LIM(7), LIM(8), LIM(9), LIM(10), LIM(11), LIM(12), LIM(13),
    LIM(1005), LIM(1006), LIM(1007), LIM(1008), LIM(1009), LIM(1010), LIM(1011), LIM(1012), LIM(1013),
};

void gd_dbl_mul(gd_dbl *d, uint16_t f) {
    if (!d->big) {
        const uint32_t lim = f >= 1005u && f <= 1013u ? fit32[f - 1005u + 9u]
                           : f >= 5u && f <= 13u ? fit32[f - 5u] : 0xFFFFFFFFul / f;
        if (d->x <= lim) { d->x *= f; return; }
        to_big(d);
    }
    big_mul(d, f);
}

static void big_mul(gd_dbl *d, uint16_t f) {
    uint8_t p[10], i, top, guard, sticky, lsb;
    int16_t bits, s, k;
    uint32_t carry = 0;
    for (i = 0; i < 8; i++) {
        carry += (uint32_t)d->m[i] * f;
        p[i] = (uint8_t)carry;
        carry >>= 8;
    }
    p[8] = (uint8_t)carry;
    p[9] = (uint8_t)(carry >> 8);
    // its bit length
    for (top = 10; top > 0 && !p[top - 1]; top--) {}
    bits = (int16_t)(8 * (top - 1));
    for (i = p[top - 1]; i; i >>= 1) bits++;
    s = bits - 53;
    if (s <= 0) {                    // exact (f == 1)
        for (i = 0; i < 8; i++) d->m[i] = p[i];
        return;
    }
    guard = (uint8_t)((p[(s - 1) >> 3] >> ((s - 1) & 7)) & 1u);
    sticky = 0;
    for (k = 0; k < s - 1; k++) if ((p[k >> 3] >> (k & 7)) & 1u) { sticky = 1; break; }
    lsb = (uint8_t)((p[s >> 3] >> (s & 7)) & 1u);
    // shift right by s
    for (k = 0; k < 8; k++) {
        const int16_t b = (int16_t)(k * 8 + s);
        uint16_t w = p[b >> 3];
        if ((b >> 3) + 1 < 10) w |= (uint16_t)p[(b >> 3) + 1] << 8;
        d->m[k] = (uint8_t)(w >> (b & 7));
    }
    d->e = (int16_t)(d->e + s);
    if (guard && (sticky || lsb)) {
        for (k = 0; k < 8; k++) { if (++d->m[k]) break; }
        if (d->m[6] & 0x20) {        // reached 2^53: one more bit off (it is even)
            for (k = 0; k < 8; k++) d->m[k] = (uint8_t)((d->m[k] >> 1) | (k < 7 ? (uint8_t)(d->m[k + 1] << 7) : 0));
            d->e++;
        }
    }
}

int8_t gd_dbl_cmp(const gd_dbl *a, const gd_dbl *b) {
    int8_t i;
    gd_dbl ta, tb;
    if (!a->big && !b->big) return a->x < b->x ? -1 : a->x > b->x ? 1 : 0;
    if (!a->big) { ta = *a; to_big(&ta); a = &ta; }
    if (!b->big) { tb = *b; to_big(&tb); b = &tb; }
    if (a->e != b->e) return a->e < b->e ? -1 : 1;
    for (i = 7; i >= 0; i--) if (a->m[(uint8_t)i] != b->m[(uint8_t)i]) return a->m[(uint8_t)i] < b->m[(uint8_t)i] ? -1 : 1;
    return 0;
}

// ---- getTrumpAttackProbability, as a threshold on a 16-bit draw ----------------------------
// p = 0.02 while the trump card is under the deck; else 0.65 + 0.35 * discard /
// total, held to [0.5, 0.95]. As a fraction, p = (13t + 7d) / (20t), and the
// count of r in [0, 65536) with r / 65536 < p is ceil(65536 p).
uint16_t gd_hw_trump_threshold(uint8_t has_flipped, uint8_t discard, uint8_t total) {
    uint32_t num, den;
    if (has_flipped) return 1311;                    // ceil(65536 * 0.02)
    if (total < 1) total = 1;
    if (discard > total) discard = total;
    if (7u * discard >= 6u * total) return 62260;    // held at 0.95: ceil(62259.2)
    num = 16384ul * (13ul * total + 7ul * discard);  // 65536 (13t + 7d) / (20t)
    den = 5ul * total;
    return (uint16_t)((num + den - 1u) / den);
}

// ---- the random draws -----------------------------------------------------------------------
// foolish draws a double u in [0, 1); here u = r / 65536, r uniform over 16 bits.
uint8_t gd_hw_draws;                 // draws the last decision made (tests/difftest.c replays them)
static uint16_t draw16(void) { gd_hw_draws++; return (uint16_t)gd_rng_below(65536ul); }

// ---- the cards a branch works with -----------------------------------------------------------

static uint8_t hand_cards(const gd_mask *h, uint8_t *out) {
    uint8_t c, n = 0;
    for (c = 0; c < GD_NCARDS; c++) if (GD_HAS(*h, c)) out[n++] = c;
    return n;
}

// The attack candidates: on an empty table the whole hand, grouped by rank;
// else the cards whose rank is on the table. *cap is the most cards one attack
// may hold (the defender's hand, less the uncovered attacks).
static uint8_t attack_pool(const gd_game *g, uint8_t seat, uint8_t *pool, int16_t *cap) {
    const uint8_t kind = gd_menu_kind(g, seat);
    uint8_t all[GD_NCARDS], n = hand_cards(&g->hand[seat], all), i, k = 0;
    const uint16_t ranks = gd_table_ranks(g);
    if (kind == GD_KIND_FIRST) {
        *cap = gd_hand_n(g, g->defender);
        for (i = 0; i < n; i++) pool[k++] = all[i];
    } else if (kind == GD_KIND_ATTACKER) {
        *cap = (int16_t)gd_hand_n(g, g->defender) - (int16_t)gd_uncovered(g);
        for (i = 0; i < n; i++) if ((ranks >> gd_rank_of[all[i]]) & 1u) pool[k++] = all[i];
    } else {
        *cap = 0;
    }
    if (*cap < 1) return 0;
    return k;
}

#define CLS_ALL       0
#define CLS_NON_TRUMP 1
#define CLS_TRUMP     2

// The best k-subset of `c` (in id order) for a class: the fewest trumps the
// class allows, then the lowest values; equal values are taken in id order,
// which is foolish's lexicographically first among equal scores. Writes the
// move's cards in id order.
static void best_subset(const uint8_t *c, uint8_t n, uint8_t k, uint8_t cls, uint8_t trump, gd_move *m) {
    uint8_t nt = 0, i, t, take[GD_NCARDS], v;
    for (i = 0; i < n; i++) { take[i] = 0; if (gd_suit_of[c[i]] != trump) nt++; }
    t = k > nt ? (uint8_t)(k - nt) : 0;
    if (cls == CLS_TRUMP && t == 0) t = 1;
    // t lowest trumps (one suit: distinct values)
    for (v = 0; v < GD_NRANKS && t; v++)
        for (i = 0; i < n && t; i++) if (gd_suit_of[c[i]] == trump && gd_rank_of[c[i]] == v) { take[i] = 1; t--; }
    // the rest: lowest non-trumps, ties in id order
    t = (uint8_t)(k - (k > nt ? k - nt : 0));
    if (cls == CLS_TRUMP && k <= nt) t = (uint8_t)(k - 1);
    for (v = 0; v < GD_NRANKS && t; v++)
        for (i = 0; i < n && t; i++) if (gd_suit_of[c[i]] != trump && gd_rank_of[c[i]] == v && !take[i]) { take[i] = 1; t--; }
    m->type = GD_ATTACK;
    m->n = 0;
    for (i = 0; i < n; i++) if (take[i]) m->cards[m->n++] = c[i];
}


// pick_attack (foolish, with its score tie-break, the only way it is reached):
// of the attacks of a class, the most cards, then the lowest summed cardScore,
// then the first in foolish's menu order. Returns 0 when the class has no attack.
static uint8_t pick_attack(const gd_game *g, uint8_t seat, uint8_t cls, gd_move *m) {
    uint8_t pool[GD_NCARDS], n, i, r;
    int16_t cap;
    const uint8_t trump = g->trump;
    n = attack_pool(g, seat, pool, &cap);
    if (!n) return 0;
    if (gd_menu_kind(g, seat) == GD_KIND_FIRST) {
        // One rank a move. foolish lists the ranks in the order their first
        // card appears in the hand, and each rank's subsets by size then
        // lexicographically.
        uint8_t seen = 0, best_k = 0, best_r = 0xFF, grp[4], gn, gt, k;
        uint16_t best_s = 0, s;
        uint16_t done = 0;
        for (i = 0; i < n; i++) {
            const uint8_t rank = gd_rank_of[pool[i]];
            if ((done >> rank) & 1u) continue;
            done |= (uint16_t)(1u << rank);
            gn = 0; gt = 0;
            for (r = 0; r < n; r++) if (gd_rank_of[pool[r]] == rank) { grp[gn++] = pool[r]; if (gd_suit_of[pool[r]] == trump) gt = 1; }
            // the class's largest move in this rank
            if (cls == CLS_NON_TRUMP) k = (uint8_t)(gn - gt);
            else if (cls == CLS_TRUMP) k = gt ? gn : 0;
            else k = gn;
            if ((int16_t)k > cap) k = (uint8_t)cap;
            if (!k) continue;
            s = (uint16_t)(k * (rank + 5u));
            if (cls == CLS_TRUMP || (cls == CLS_ALL && k > gn - gt)) s += TRUMP_SCORE;
            if (k > best_k || (k == best_k && s < best_s)) { best_k = k; best_s = s; best_r = rank; }
            seen = 1;
        }
        if (!seen) return 0;
        gn = 0;
        for (r = 0; r < n; r++) if (gd_rank_of[pool[r]] == best_r) grp[gn++] = pool[r];
        best_subset(grp, gn, best_k, cls, trump, m);
        return 1;
    }
    // A throw-in: any cards of the table's ranks.
    {
        uint8_t nt = 0, has_t = 0, k;
        for (i = 0; i < n; i++) { if (gd_suit_of[pool[i]] != trump) nt++; else has_t = 1; }
        if (cls == CLS_NON_TRUMP) k = nt;
        else if (cls == CLS_TRUMP) k = has_t ? n : 0;
        else k = n;
        if ((int16_t)k > cap) k = (uint8_t)cap;
        if (!k) return 0;
        best_subset(pool, n, k, cls, trump, m);
        return 1;
    }
}

// ---- findBestCoverCombination: the full cover with the least product ----------------------
// foolish lists full covers by assigning the uncovered attacks, in table order,
// each a covering card in hand order; the first strict minimum of the double
// product of card scores wins. That is found here by a depth-first walk in the
// same order, with two things that change only its speed:
//   - a full cover is first found by augmenting paths, trying the cheapest
//     cards first; if there is none the walk is skipped, and if there is one
//     its product is the bound the walk starts with;
//   - the walk drops a branch whose product so far is above the bound (every
//     later factor is at least 5, and rounding never lowers a product), and
//     keeps the first leaf at or below it, lowering the bound on a strictly
//     smaller one. So the leaf it keeps is the first, in foolish's order, of
//     those with the least product.
static const gd_game *cv_g;
static uint8_t cv_att[GD_MAXB], cv_n, cv_hand[GD_NCARDS], cv_hn;
static uint8_t cv_pick[GD_MAXB], cv_best[GD_MAXB], cv_found;
static uint16_t cv_min[GD_MAXB];     // each attack's cheapest covering card score
static uint32_t cv_used[2];
static gd_dbl cv_best_p, cv_prod[GD_MAXB + 1];
uint16_t gd_hw_nodes;                // nodes the last cover search visited (tests)

static void cover_walk(uint8_t depth) {
    uint8_t i;
    gd_hw_nodes++;
    if (depth == cv_n) {
        const int8_t c = gd_dbl_cmp(&cv_prod[depth], &cv_best_p);
        if (c < 0 || (c == 0 && !cv_found)) {
            cv_best_p = cv_prod[depth];
            for (i = 0; i < cv_n; i++) cv_best[i] = cv_pick[i];
            cv_found = 1;
        }
        return;
    }
    {
        // A lower bound on every leaf below: the product so far times each
        // remaining attack's cheapest cover, multiplied in the same order. A
        // leaf multiplies factors no smaller, and rounding never decreases as
        // its input grows, so no leaf is below it.
        gd_dbl lb = cv_prod[depth];
        for (i = depth; i < cv_n; i++) gd_dbl_mul(&lb, cv_min[i]);
        // Once a leaf is kept, a later leaf of the same product comes after it
        // in foolish's order and cannot replace it, so a tie is dropped too.
        const int8_t c = gd_dbl_cmp(&lb, &cv_best_p);
        if (c > 0 || (c == 0 && cv_found)) return;
    }
    for (i = 0; i < cv_hn; i++) {
        const uint8_t c = cv_hand[i];
        if ((cv_used[i >> 5] >> (i & 31)) & 1ul) continue;
        if (!gd_can_cover(cv_att[depth], c, cv_g->trump)) continue;
        cv_prod[depth + 1] = cv_prod[depth];
        gd_dbl_mul(&cv_prod[depth + 1], card_score(c, cv_g->trump));
        cv_used[i >> 5] |= 1ul << (i & 31);
        cv_pick[depth] = c;
        cover_walk((uint8_t)(depth + 1));
        cv_used[i >> 5] &= ~(1ul << (i & 31));
    }
}

// Augmenting paths: owner[k] is the attack hand card k (in cheapest-first
// order) covers, or 0xFF. Returns 1 when every attack got a card.
static uint8_t mt_card[GD_NCARDS], mt_owner[GD_NCARDS], mt_seen[GD_NCARDS];

static uint8_t mt_augment(uint8_t a) {
    uint8_t k;
    for (k = 0; k < cv_hn; k++) {
        if (mt_seen[k] || !gd_can_cover(cv_att[a], mt_card[k], cv_g->trump)) continue;
        mt_seen[k] = 1;
        if (mt_owner[k] == 0xFF || mt_augment(mt_owner[k])) { mt_owner[k] = a; return 1; }
    }
    return 0;
}

static uint8_t first_bound(void) {
    uint8_t i, k, j;
    // the hand by card score, cheapest first (stable: ties in id order)
    j = 0;
    for (i = 0; i < 2; i++)
        for (k = 0; k < GD_NRANKS; k++) {
            uint8_t h;
            for (h = 0; h < cv_hn; h++)
                if (gd_rank_of[cv_hand[h]] == k && (gd_suit_of[cv_hand[h]] == cv_g->trump) == i) mt_card[j++] = cv_hand[h];
        }
    for (k = 0; k < cv_hn; k++) mt_owner[k] = 0xFF;
    for (i = 0; i < cv_n; i++) {
        for (k = 0; k < cv_hn; k++) mt_seen[k] = 0;
        if (!mt_augment(i)) return 0;
    }
    gd_dbl_one(&cv_best_p);
    for (i = 0; i < cv_n; i++)
        for (k = 0; k < cv_hn; k++)
            if (mt_owner[k] == i) { gd_dbl_mul(&cv_best_p, card_score(mt_card[k], cv_g->trump)); break; }
    return 1;
}

static uint8_t best_full_cover(const gd_game *g, uint8_t seat, gd_move *m) {
    uint8_t i;
    cv_g = g;
    cv_n = 0;
    for (i = 0; i < g->nb; i++) if (g->def[i] == GD_NONE) cv_att[cv_n++] = g->att[i];
    if (!cv_n) return 0;
    cv_hn = hand_cards(&g->hand[seat], cv_hand);
    if (!first_bound()) return 0;          // no full cover at all
    for (i = 0; i < cv_n; i++) {
        uint8_t h;
        cv_min[i] = 0xFFFF;
        for (h = 0; h < cv_hn; h++)
            if (gd_can_cover(cv_att[i], cv_hand[h], g->trump) && card_score(cv_hand[h], g->trump) < cv_min[i])
                cv_min[i] = card_score(cv_hand[h], g->trump);
    }
    cv_used[0] = cv_used[1] = 0;
    cv_found = 0;
    gd_dbl_one(&cv_prod[0]);
    cover_walk(0);
    m->type = GD_COVER;
    m->n = cv_n;
    for (i = 0; i < cv_n; i++) { m->cards[i] = cv_best[i]; m->atts[i] = cv_att[i]; }
    return 1;
}

// ---- chooseMove ----------------------------------------------------------------------------

void gd_hw_choose(const gd_game *g, uint8_t seat, gd_move *out) GD_BANKED {
    const uint8_t kind = gd_menu_kind(g, seat), trump = g->trump;
    uint8_t pool[GD_NCARDS], n, i, any_attack, any_nt = 0, any_t = 0, has_good;
    int16_t cap;
    out->type = 0xFF;
    out->n = 0;
    gd_hw_draws = 0;
    gd_hw_nodes = 0;
    // foolish returns -1 on an empty menu: no branch, or a defender with every attack covered
    if (kind == GD_KIND_NONE || (kind == GD_KIND_DEFENDER && !gd_uncovered(g))) return;
    has_good = kind == GD_KIND_ATTACKER;          // good is on an attacker's menu, and only there
    n = attack_pool(g, seat, pool, &cap);
    any_attack = n > 0;
    for (i = 0; i < n; i++) { if (gd_suit_of[pool[i]] != trump) any_nt = 1; else any_t = 1; }

    // ---- continue-attack branch: prefer non-trump; gate trump by probability ----
    if (any_attack) {
        if (any_nt) { pick_attack(g, seat, CLS_NON_TRUMP, out); return; }
        if (any_t) {
            // bot_total_card_count: every card the game holds, the face-up trump included
            uint8_t total = (uint8_t)(g->stock_n + g->discard + gd_hand_n(g, 0) + gd_hand_n(g, 1) + (g->has_flipped ? 1u : 0u));
            uint16_t t;
            for (i = 0; i < g->nb; i++) total = (uint8_t)(total + 1u + (g->def[i] != GD_NONE ? 1u : 0u));
            t = gd_hw_trump_threshold(g->has_flipped, g->discard, total);
            if (draw16() < t) { pick_attack(g, seat, CLS_TRUMP, out); return; }
            // decline: good if there is one (no wait in this menu), else fall through
            if (has_good) { out->type = GD_GOOD; return; }
        }
    }

    // ---- pass branch: the least summed cardScore, i.e. one card, a non-trump first ----
    if (kind == GD_KIND_DEFENDER && gd_pass_rank(g) != 0xFF
        && (int16_t)gd_hand_n(g, gd_next_seat(g, seat)) - (int16_t)g->nb >= 1) {
        uint8_t c, pick = 0xFF;
        const uint8_t v = gd_pass_rank(g);
        for (c = 0; c < GD_NCARDS; c++)
            if (GD_HAS(g->hand[seat], c) && gd_rank_of[c] == v && (pick == 0xFF || (gd_suit_of[pick] == trump && gd_suit_of[c] != trump)))
                pick = c;
        if (pick != 0xFF) { out->type = GD_PASS; out->n = 1; out->cards[0] = pick; return; }
    }

    // ---- cover branch: only a cover of every uncovered attack ----
    if (kind == GD_KIND_DEFENDER && best_full_cover(g, seat, out)) return;

    // ---- other non-attack moves (good): a draw picks one of them ----
    if (has_good) {
        // floor(u * 1) is 0 whatever u is, but the draw is made, as foolish makes it
        (void)draw16();
        out->type = GD_GOOD;
        return;
    }

    // ---- forced attack fallback ----
    // foolish first tries, while the deck is alive, the first max-count
    // non-trump attack with no score tie-break, then good or wait. Neither can
    // happen here: this point is reached with an attack only when there is no
    // non-trump attack (the continue-attack branch returns on any) and no good
    // (it would have been taken above). So only its last step remains.
    if (any_attack) {
        pick_attack(g, seat, CLS_ALL, out);
        return;
    }

    // ---- pickup as the last resort ----
    if (kind == GD_KIND_DEFENDER && gd_uncovered(g)) { out->type = GD_PICKUP; return; }

    // ---- foolish's final fallback (a random move) is unreachable: every seat with
    // a menu returns above (a defender always has pickup while an attack is uncovered,
    // and has no menu once all are covered).
}

// gd_menu - the legal-move menu as a count and an unrank (gd_rules.h): every
// legal move exactly once, so a uniform random move is one draw below the
// count. Split from gd_rules.c so the ROM can keep it in its own bank (it is
// the random bot's, and the largest part of the rules); it reads the board
// only through gd_rules' functions.
#include "gd_rules.h"
#include "gd_binom.h"

#define CARD(s, r) ((uint8_t)((s) * GD_NRANKS + (r)))

static uint32_t binom(uint8_t n, uint8_t k) {
    if (k > n) return 0;
    if (k > (uint8_t)(n - k)) k = (uint8_t)(n - k);
    return gd_binom_tab[n][k];
}

// ---- subsets: k-combinations of a card list, k = 1..kmax, by k then lex -------

static uint8_t lst[GD_NCARDS];
static uint8_t lst_n;

static uint32_t subsets_count(uint8_t n, int16_t kmax) {
    uint32_t c = 0;
    uint8_t k;
    if (kmax > n) kmax = n;
    for (k = 1; (int16_t)k <= kmax; k++) c += binom(n, k);
    return c;
}

static void subsets_unrank(uint8_t n, int16_t kmax, uint32_t idx, gd_move *m) {
    uint8_t k = 1, i, left;
    uint32_t c;
    if (kmax > n) kmax = n;
    while ((int16_t)k < kmax && idx >= (c = binom(n, k))) { idx -= c; k++; }
    m->n = 0;
    left = k;
    for (i = 0; i < n && left; i++) {
        c = binom((uint8_t)(n - i - 1u), (uint8_t)(left - 1u));
        if (idx < c) { m->cards[m->n++] = lst[i]; left--; }
        else idx -= c;
    }
}

// ---- covers: count and unrank partial matchings ------------------------------
//
// A cover move is a set of uncovered attacks, each given its own covering card.
// The rows are the uncovered attacks in this order: trumps by rank descending,
// then each other suit (ascending) by rank descending. In that order every
// row's covers of each KIND (its own suit, trumps) contain every earlier row's
// covers of that kind, so the number of free covers a row has depends only on
// how many earlier rows took that kind: t trumps so far, s own-suit cards so
// far in this suit's group. The count of ways to finish from row i is then a
// table over (i, t, s), which is how a menu that can run to millions of
// moves is counted and unranked without being listed.
#define CV_T 10
#define CV_S 5
static struct {
    uint8_t u, T, S;
    uint8_t att[GD_MAXB];
    uint8_t own[GD_MAXB];
    uint8_t trc[GD_MAXB];
    uint8_t newg[GD_MAXB + 1];
} cv;
static uint32_t cdp[(GD_MAXB + 1) * CV_T * CV_S];
#define CI(i, t, s) ((((uint16_t)(i) * (cv.T + 1u)) + (t)) * (cv.S + 1u) + (s))

static uint8_t count_above(const gd_mask *h, uint8_t suit, uint8_t rank) {
    uint8_t n = 0, r;
    for (r = (uint8_t)(rank + 1u); r < GD_NRANKS; r++) if (GD_HAS(*h, CARD(suit, r))) n++;
    return n;
}

static void cover_build(const gd_game *g, uint8_t seat) {
    const gd_mask *h = &g->hand[seat];
    gd_mask unc;
    uint8_t i, s, r, gs, tr = g->trump, first;
    int8_t rr;
    for (i = 0; i < 5; i++) unc.b[i] = 0;
    for (i = 0; i < g->nb; i++) if (g->def[i] == GD_NONE) GD_SET(unc, g->att[i]);
    cv.u = 0;
    cv.S = 0;
    cv.T = 0;
    for (r = 0; r < GD_NRANKS; r++) if (GD_HAS(*h, CARD(tr, r))) cv.T++;
    // The trump group, then each other suit: a group of rows by rank descending.
    for (gs = 0; gs <= GD_NSUITS; gs++) {
        s = gs == 0 ? tr : (uint8_t)(gs - 1u);
        if (gs > 0 && s == tr) continue;
        first = 1;
        for (rr = GD_NRANKS - 1; rr >= 0; rr--) {
            const uint8_t c = CARD(s, rr);
            if (!GD_HAS(unc, c)) continue;
            cv.att[cv.u] = c;
            cv.newg[cv.u] = first;
            if (s == tr) { cv.own[cv.u] = 0; cv.trc[cv.u] = count_above(h, tr, (uint8_t)rr); }
            else { cv.own[cv.u] = count_above(h, s, (uint8_t)rr); cv.trc[cv.u] = cv.T; }
            first = 0;
            cv.u++;
        }
        // This group's own-suit matches reach at most min(rows, covers of its
        // lowest attack). A suit has 9 cards, so that is at most 4.
        if (!first && s != tr) {
            uint8_t ng = 0, k;
            for (k = cv.u; k > 0 && !cv.newg[k - 1u]; k--) ng++;
            ng++;
            k = cv.own[cv.u - 1u] < ng ? cv.own[cv.u - 1u] : ng;
            if (k > cv.S) cv.S = k;
        }
    }
    cv.newg[cv.u] = 1;
    for (i = cv.u + 1u; i-- > 0;) {
        uint8_t t, ss;
        for (t = 0; t <= cv.T; t++) {
            for (ss = 0; ss <= cv.S; ss++) {
                uint32_t v;
                uint8_t ns, ns1;
                if (i == cv.u) { cdp[CI(i, t, ss)] = 1; continue; }
                ns = cv.newg[i + 1u] ? 0 : ss;
                ns1 = cv.newg[i + 1u] ? 0 : (uint8_t)(ss + 1u);
                v = cdp[CI(i + 1u, t, ns)];
                if (cv.own[i] > ss && ns1 <= cv.S)
                    v += (uint32_t)(cv.own[i] - ss) * cdp[CI(i + 1u, t, ns1)];
                if (cv.trc[i] > t)
                    v += (uint32_t)(cv.trc[i] - t) * cdp[CI(i + 1u, t + 1u, ns)];
                cdp[CI(i, t, ss)] = v;
            }
        }
    }
}

// The j-th (from 0) card of `suit` above `rank` in `h` and not in `used`.
static uint8_t nth_free(const gd_mask *h, const gd_mask *used, uint8_t suit, int8_t rank, uint8_t j) {
    uint8_t r;
    for (r = (uint8_t)(rank + 1); r < GD_NRANKS; r++) {
        const uint8_t c = CARD(suit, r);
        if (!GD_HAS(*h, c) || GD_HAS(*used, c)) continue;
        if (j == 0) return c;
        j--;
    }
    return GD_NONE;
}

static void cover_unrank(const gd_game *g, uint8_t seat, uint32_t q, gd_move *m) {
    const gd_mask *h = &g->hand[seat];
    gd_mask used;
    uint8_t i, t = 0, s = 0;
    uint32_t p = q + 1u;   // path 0 is the empty matching, which is no move
    for (i = 0; i < 5; i++) used.b[i] = 0;
    m->type = GD_COVER;
    m->n = 0;
    for (i = 0; i < cv.u; i++) {
        const uint8_t ns = cv.newg[i + 1u] ? 0 : s;
        const uint8_t ns1 = cv.newg[i + 1u] ? 0 : (uint8_t)(s + 1u);
        const uint8_t a = cv.att[i];
        uint32_t cnt = cdp[CI(i + 1u, t, ns)];
        uint8_t c;
        if (p < cnt) { s = ns; continue; }
        p -= cnt;
        if (cv.own[i] > s && ns1 <= cv.S) {
            const uint32_t per = cdp[CI(i + 1u, t, ns1)];
            const uint32_t tot = (uint32_t)(cv.own[i] - s) * per;
            if (p < tot) {
                c = nth_free(h, &used, gd_suit_of[a], (int8_t)gd_rank_of[a], (uint8_t)(p / per));
                p %= per;
                GD_SET(used, c);
                m->cards[m->n] = c; m->atts[m->n] = a; m->n++;
                s = ns1;
                continue;
            }
            p -= tot;
        }
        cnt = cdp[CI(i + 1u, t + 1u, ns)];
        c = nth_free(h, &used, g->trump,
                     gd_suit_of[a] == g->trump ? (int8_t)gd_rank_of[a] : (int8_t)-1, (uint8_t)(p / cnt));
        p %= cnt;
        GD_SET(used, c);
        m->cards[m->n] = c; m->atts[m->n] = a; m->n++;
        t++;
        s = ns;
    }
}

// ---- the menu ------------------------------------------------------------------

static void list_rank(const gd_mask *h, uint8_t rank) {
    uint8_t s;
    lst_n = 0;
    for (s = 0; s < GD_NSUITS; s++) if (GD_HAS(*h, CARD(s, rank))) lst[lst_n++] = CARD(s, rank);
}

static void list_ranks(const gd_mask *h, uint16_t ranks) {
    uint8_t c;
    lst_n = 0;
    for (c = 0; c < GD_NCARDS; c++) if (GD_HAS(*h, c) && (ranks >> gd_rank_of[c]) & 1u) lst[lst_n++] = c;
}

// The pass section's list (lst) and its k ceiling; 0 when there is no pass.
static int16_t pass_setup(const gd_game *g, uint8_t seat) {
    const uint8_t v = gd_pass_rank(g);
    if (v == 0xFF) return 0;
    list_rank(&g->hand[seat], v);
    return (int16_t)gd_hand_n(g, gd_next_seat(g, seat)) - (int16_t)g->nb;
}

uint32_t gd_menu_count(const gd_game *g, uint8_t seat) GD_BANKED {
    const uint8_t kind = gd_menu_kind(g, seat);
    uint32_t n = 0;
    uint8_t r;
    int16_t cap;
    if (kind == GD_KIND_FIRST) {
        const int16_t d = gd_hand_n(g, g->defender);
        for (r = 0; r < GD_NRANKS; r++) { list_rank(&g->hand[seat], r); n += subsets_count(lst_n, d); }
    } else if (kind == GD_KIND_DEFENDER) {
        const uint8_t unc = gd_uncovered(g);
        if (unc) { cover_build(g, seat); n += cdp[0] - 1u; n++; }
        cap = pass_setup(g, seat);
        if (cap > 0) n += subsets_count(lst_n, cap);
    } else if (kind == GD_KIND_ATTACKER) {
        list_ranks(&g->hand[seat], gd_table_ranks(g));
        n += subsets_count(lst_n, (int16_t)gd_hand_n(g, g->defender) - (int16_t)gd_uncovered(g));
        n++;
    }
    return n;
}

void gd_menu_move(const gd_game *g, uint8_t seat, uint32_t idx, gd_move *m) GD_BANKED {
    const uint8_t kind = gd_menu_kind(g, seat);
    uint32_t c;
    uint8_t r;
    m->n = 0;
    m->type = 0xFF;
    if (kind == GD_KIND_FIRST) {
        const int16_t d = gd_hand_n(g, g->defender);
        m->type = GD_ATTACK;
        for (r = 0; r < GD_NRANKS; r++) {
            list_rank(&g->hand[seat], r);
            c = subsets_count(lst_n, d);
            if (idx < c) { subsets_unrank(lst_n, d, idx, m); return; }
            idx -= c;
        }
    } else if (kind == GD_KIND_DEFENDER) {
        int16_t cap;
        if (gd_uncovered(g)) {
            cover_build(g, seat);
            c = cdp[0] - 1u;
            if (idx < c) { cover_unrank(g, seat, idx, m); return; }
            idx -= c;
            if (idx == 0) { m->type = GD_PICKUP; return; }
            idx--;
        }
        cap = pass_setup(g, seat);
        m->type = GD_PASS;
        subsets_unrank(lst_n, cap, idx, m);
    } else if (kind == GD_KIND_ATTACKER) {
        const int16_t kmax = (int16_t)gd_hand_n(g, g->defender) - (int16_t)gd_uncovered(g);
        list_ranks(&g->hand[seat], gd_table_ranks(g));
        c = subsets_count(lst_n, kmax);
        if (idx < c) { m->type = GD_ATTACK; subsets_unrank(lst_n, kmax, idx, m); return; }
        m->type = GD_GOOD;
    }
}


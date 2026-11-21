// gd_rules - see gd_rules.h. Every rule here is a port of a named place in
// foolish/c/src/game.c or legal.c, for two seats, and tests/difftest.c plays
// both engines side by side to prove they agree.
#include "gd_rules.h"
#include "gd_binom.h"

const uint8_t gd_suit_of[GD_NCARDS] = {
    0,0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2,2, 3,3,3,3,3,3,3,3,3,
};
const uint8_t gd_rank_of[GD_NCARDS] = {
    0,1,2,3,4,5,6,7,8, 0,1,2,3,4,5,6,7,8, 0,1,2,3,4,5,6,7,8, 0,1,2,3,4,5,6,7,8,
};

#define CARD(s, r) ((uint8_t)((s) * GD_NRANKS + (r)))
#define BIT(s) ((uint8_t)(1u << (s)))

static const uint8_t pop4[16] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };

uint8_t gd_count(const gd_mask *m) {
    uint8_t n = 0, i;
    for (i = 0; i < 5; i++) n += (uint8_t)(pop4[m->b[i] & 15u] + pop4[m->b[i] >> 4]);
    return n;
}

uint8_t gd_hand_n(const gd_game *g, uint8_t seat) { return gd_count(&g->hand[seat]); }

uint8_t gd_uncovered(const gd_game *g) {
    uint8_t n = 0, i;
    for (i = 0; i < g->nb; i++) if (g->def[i] == GD_NONE) n++;
    return n;
}

static uint32_t binom(uint8_t n, uint8_t k) {
    if (k > n) return 0;
    if (k > (uint8_t)(n - k)) k = (uint8_t)(n - k);
    return gd_binom_tab[n][k];
}

// foolish can_cover
static uint8_t can_cover(uint8_t a, uint8_t d, uint8_t trump) {
    if (gd_suit_of[a] != gd_suit_of[d]) return gd_suit_of[d] == trump && gd_suit_of[a] != trump;
    return gd_rank_of[d] > gd_rank_of[a];
}

static uint8_t in_count(const gd_game *g) {
    return (uint8_t)(2u - ((g->out & 1u) ? 1u : 0u) - ((g->out & 2u) ? 1u : 0u));
}

// foolish get_next_player_index, two seats.
static uint8_t next_seat(const gd_game *g, uint8_t s) {
    uint8_t n;
    if (in_count(g) <= 1) return s;
    n = (uint8_t)(s ^ 1u);
    return n;
}

static uint8_t no_cards_left(const gd_game *g) { return g->stock_n == 0 && !g->has_flipped; }

// foolish game_done: the fool, or 0xFF while the game runs.
static uint8_t done_fool(const gd_game *g) {
    if (in_count(g) == 1) return (g->out & 1u) ? 1 : 0;
    return 0xFF;
}

uint8_t gd_result(const gd_game *g) {
    if (g->status != GD_OVER) return GD_RESULT_NONE;
    if (in_count(g) == 2) return GD_RESULT_DRAW;
    return (g->out & 1u) ? GD_RESULT_FOOL1 : GD_RESULT_FOOL0;
}

static void settle(gd_game *g) {
    if (g->status == GD_PLAYING && done_fool(g) != 0xFF) g->status = GD_OVER;
}

// ---- deck and draws (foolish draw_card, draw_up_to_six, refill_player_hands) ----

static uint8_t draw_card(gd_game *g, uint8_t *out) {
    uint8_t i;
    if (g->stock_n == 0) {
        if (!g->has_flipped) return 0;
        *out = g->flipped;
        g->has_flipped = 0;
        return 1;
    }
    *out = g->stock[0];
    for (i = 1; i < g->stock_n; i++) g->stock[i - 1] = g->stock[i];
    g->stock_n--;
    return 1;
}

static void draw_up_to_six(gd_game *g, uint8_t seat) {
    uint8_t c;
    while (gd_hand_n(g, seat) < GD_HAND) {
        if (!draw_card(g, &c)) break;
        GD_SET(g->hand[seat], c);
    }
    if (gd_hand_n(g, seat) == 0 && !(g->out & BIT(seat))) g->out |= BIT(seat);
}

// The bout's first attacker draws first, the rest of the table skipping the
// defender, and the defender LAST, always.
static void refill(gd_game *g) {
    uint8_t s, p, visited = 0;
    if (no_cards_left(g)) {
        for (s = 0; s < 2; s++)
            if (gd_hand_n(g, s) == 0 && !(g->out & BIT(s))) g->out |= BIT(s);
        return;
    }
    p = g->fa;
    do {
        if (visited & BIT(p)) break;
        visited |= BIT(p);
        if (p != g->defender) draw_up_to_six(g, p);
        p = next_seat(g, p);
    } while (p != g->fa);
    draw_up_to_six(g, g->defender);
}

// ---- the deal (foolish start_game_dealt) --------------------------------------

static uint8_t stock_holds_non_ace(const gd_game *g) {
    uint8_t i;
    for (i = 0; i < g->stock_n; i++) if (gd_rank_of[g->stock[i]] != GD_ACE) return 1;
    return 0;
}

void gd_deal(gd_game *g, gd_rep *r, const uint8_t deck[GD_NCARDS], uint8_t fallback_seat) {
    uint8_t i, f = 0, s, lowest = 0xFF, lowest_seat = fallback_seat;
    for (i = 0; i < 5; i++) { g->hand[0].b[i] = 0; g->hand[1].b[i] = 0; }
    for (i = 0; i < GD_HAND; i++) { GD_SET(g->hand[0], deck[i]); GD_SET(g->hand[1], deck[GD_HAND + i]); }
    for (i = 0; i < GD_STOCK; i++) g->stock[i] = deck[2 * GD_HAND + i];
    g->stock_n = GD_STOCK;
    g->has_flipped = 0;
    // THE TRUMP RULE: an Ace is not turned while a non-Ace is left; it goes to
    // the bottom of the stock and the next card is turned.
    for (;;) {
        if (!draw_card(g, &f)) break;
        if (gd_rank_of[f] == GD_ACE && stock_holds_non_ace(g)) { g->stock[g->stock_n++] = f; continue; }
        break;
    }
    g->flipped = f;
    g->has_flipped = 1;
    g->trump = gd_suit_of[f];
    g->nb = 0;
    g->good = 0;
    g->out = 0;
    g->discard = 0;
    g->status = GD_PLAYING;
    // The lowest trump leads (foolish derive_lowest_power_index).
    for (s = 0; s < 2; s++)
        for (i = 0; i < GD_NRANKS; i++)
            if (GD_HAS(g->hand[s], CARD(g->trump, i)) && i < lowest) { lowest = i; lowest_seat = s; }
    g->fa = lowest_seat;
    g->defender = (uint8_t)(lowest_seat ^ 1u);
    if (r) { r->fill = 0; r->head = 0; }
}

// ---- the repetition draw (foolish rep_checkpoint, game_rep_checkpoint) ---------
//
// Within one history the stock, the discard and the seats in are fixed, so the
// stock's set, the discard's set, the trump card and the statuses are too; the
// table is empty, the goods are zero, and seat 1's hand is every card left. So
// seat 0's hand and the leader (the defender is the other seat) are the whole
// position, compared exactly.
static uint8_t rep_checkpoint(gd_game *g, gd_rep *r, uint8_t drew) {
    uint8_t pos[5], i, k, seen = 0, stock, inm;
    if (!r || drew || g->status != GD_PLAYING || done_fool(g) != 0xFF) return 0;
    stock = (uint8_t)(g->stock_n + (g->has_flipped ? 1u : 0u));
    inm = (uint8_t)(3u & (uint8_t)~g->out);
    if (r->fill && (r->stock != stock || r->discard != g->discard || r->in_mask != inm)) { r->fill = 0; r->head = 0; }
    if (!r->fill) { r->stock = stock; r->discard = g->discard; r->in_mask = inm; }
    for (i = 0; i < 5; i++) pos[i] = g->hand[0].b[i];
    pos[4] |= (uint8_t)(g->fa << 7);
    for (k = 0; k < r->fill; k++) {
        for (i = 0; i < 5 && r->pos[k][i] == pos[i]; i++) {}
        if (i == 5) seen++;
    }
    for (i = 0; i < 5; i++) r->pos[r->head][i] = pos[i];
    r->head = (uint8_t)((r->head + 1u) % GD_REP_RING);
    if (r->fill < GD_REP_RING) r->fill++;
    return seen >= 2;
}

// ---- small helpers over cards ---------------------------------------------------

static uint16_t table_ranks(const gd_game *g) {
    uint16_t m = 0;
    uint8_t i;
    for (i = 0; i < g->nb; i++) {
        m |= (uint16_t)(1u << gd_rank_of[g->att[i]]);
        if (g->def[i] != GD_NONE) m |= (uint16_t)(1u << gd_rank_of[g->def[i]]);
    }
    return m;
}

static uint8_t battle_of_uncovered(const gd_game *g, uint8_t attack) {
    uint8_t i;
    for (i = 0; i < g->nb; i++) if (g->def[i] == GD_NONE && g->att[i] == attack) return i;
    return 0xFF;
}

// The cards as a set: 0 when one is not a card, repeats, or is not in `hand`.
static uint8_t cards_in(const uint8_t *c, uint8_t n, const gd_mask *hand, gd_mask *set) {
    uint8_t i;
    for (i = 0; i < 5; i++) set->b[i] = 0;
    for (i = 0; i < n; i++) {
        if (c[i] >= GD_NCARDS || GD_HAS(*set, c[i])) return 0;
        if (hand && !GD_HAS(*hand, c[i])) return 0;
        GD_SET(*set, c[i]);
    }
    return 1;
}

static uint8_t all_same_rank(const uint8_t *c, uint8_t n) {
    uint8_t i;
    for (i = 1; i < n; i++) if (gd_rank_of[c[i]] != gd_rank_of[c[0]]) return 0;
    return 1;
}

static uint8_t any_covered(const gd_game *g) {
    uint8_t i;
    for (i = 0; i < g->nb; i++) if (g->def[i] != GD_NONE) return 1;
    return 0;
}

// The pass precondition shared by the menu and the handler: a table of
// uncovered attacks all of one rank. That rank, or 0xFF.
static uint8_t pass_rank(const gd_game *g) {
    uint8_t i, v;
    if (g->nb == 0 || any_covered(g)) return 0xFF;
    v = gd_rank_of[g->att[0]];
    for (i = 1; i < g->nb; i++) if (gd_rank_of[g->att[i]] != v) return 0xFF;
    return v;
}

// ---- the menu: which branch (foolish calc_moves) ---------------------------------

#define KIND_NONE     0
#define KIND_FIRST    1   // the first attacker on an empty table
#define KIND_DEFENDER 2
#define KIND_ATTACKER 3   // a non-defender on a table, who has not said good

static uint8_t menu_kind(const gd_game *g, uint8_t seat) {
    if (g->status != GD_PLAYING || seat > 1 || (g->out & BIT(seat))) return KIND_NONE;
    if (g->nb == 0) return seat == g->fa ? KIND_FIRST : KIND_NONE;
    if (seat == g->defender) return KIND_DEFENDER;
    return (g->good & BIT(seat)) ? KIND_NONE : KIND_ATTACKER;
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
    const uint8_t v = pass_rank(g);
    if (v == 0xFF) return 0;
    list_rank(&g->hand[seat], v);
    return (int16_t)gd_hand_n(g, next_seat(g, seat)) - (int16_t)g->nb;
}

uint32_t gd_menu_count(const gd_game *g, uint8_t seat) {
    const uint8_t kind = menu_kind(g, seat);
    uint32_t n = 0;
    uint8_t r;
    int16_t cap;
    if (kind == KIND_FIRST) {
        const int16_t d = gd_hand_n(g, g->defender);
        for (r = 0; r < GD_NRANKS; r++) { list_rank(&g->hand[seat], r); n += subsets_count(lst_n, d); }
    } else if (kind == KIND_DEFENDER) {
        const uint8_t unc = gd_uncovered(g);
        if (unc) { cover_build(g, seat); n += cdp[0] - 1u; n++; }
        cap = pass_setup(g, seat);
        if (cap > 0) n += subsets_count(lst_n, cap);
    } else if (kind == KIND_ATTACKER) {
        list_ranks(&g->hand[seat], table_ranks(g));
        n += subsets_count(lst_n, (int16_t)gd_hand_n(g, g->defender) - (int16_t)gd_uncovered(g));
        n++;
    }
    return n;
}

void gd_menu_move(const gd_game *g, uint8_t seat, uint32_t idx, gd_move *m) {
    const uint8_t kind = menu_kind(g, seat);
    uint32_t c;
    uint8_t r;
    m->n = 0;
    m->type = 0xFF;
    if (kind == KIND_FIRST) {
        const int16_t d = gd_hand_n(g, g->defender);
        m->type = GD_ATTACK;
        for (r = 0; r < GD_NRANKS; r++) {
            list_rank(&g->hand[seat], r);
            c = subsets_count(lst_n, d);
            if (idx < c) { subsets_unrank(lst_n, d, idx, m); return; }
            idx -= c;
        }
    } else if (kind == KIND_DEFENDER) {
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
    } else if (kind == KIND_ATTACKER) {
        const int16_t kmax = (int16_t)gd_hand_n(g, g->defender) - (int16_t)gd_uncovered(g);
        list_ranks(&g->hand[seat], table_ranks(g));
        c = subsets_count(lst_n, kmax);
        if (idx < c) { m->type = GD_ATTACK; subsets_unrank(lst_n, kmax, idx, m); return; }
        m->type = GD_GOOD;
    }
}

uint8_t gd_in_menu(const gd_game *g, uint8_t seat, const gd_move *m) {
    const uint8_t kind = menu_kind(g, seat);
    gd_mask set, atts;
    uint8_t i;
    if (kind == KIND_NONE) return 0;
    switch (m->type) {
    case GD_ATTACK:
        if (m->n == 0 || !cards_in(m->cards, m->n, &g->hand[seat], &set)) return 0;
        if (kind == KIND_FIRST) return all_same_rank(m->cards, m->n) && m->n <= gd_hand_n(g, g->defender);
        if (kind != KIND_ATTACKER) return 0;
        for (i = 0; i < m->n; i++) if (!((table_ranks(g) >> gd_rank_of[m->cards[i]]) & 1u)) return 0;
        return (int16_t)m->n <= (int16_t)gd_hand_n(g, g->defender) - (int16_t)gd_uncovered(g);
    case GD_COVER:
        if (kind != KIND_DEFENDER || m->n == 0) return 0;
        if (!cards_in(m->cards, m->n, &g->hand[seat], &set) || !cards_in(m->atts, m->n, 0, &atts)) return 0;
        for (i = 0; i < m->n; i++)
            if (battle_of_uncovered(g, m->atts[i]) == 0xFF || !can_cover(m->atts[i], m->cards[i], g->trump)) return 0;
        return 1;
    case GD_PICKUP:
        return kind == KIND_DEFENDER && gd_uncovered(g) > 0;
    case GD_PASS:
        if (kind != KIND_DEFENDER || m->n == 0 || !cards_in(m->cards, m->n, &g->hand[seat], &set)) return 0;
        if (pass_rank(g) == 0xFF || gd_rank_of[m->cards[0]] != pass_rank(g) || !all_same_rank(m->cards, m->n)) return 0;
        return (int16_t)m->n <= (int16_t)gd_hand_n(g, next_seat(g, seat)) - (int16_t)g->nb;
    case GD_GOOD:
        return kind == KIND_ATTACKER;
    }
    return 0;
}

// ---- the handlers' acceptance (foolish handle_*, in their order) -------------------

uint8_t gd_legal(const gd_game *g, uint8_t seat, const gd_move *m) {
    gd_mask set, atts;
    uint8_t i;
    if (g->status != GD_PLAYING || seat > 1) return 0;
    switch (m->type) {
    case GD_ATTACK:
        if (m->n == 0 || seat == g->defender) return 0;
        if (!cards_in(m->cards, m->n, &g->hand[seat], &set)) return 0;
        if (g->nb == 0) {
            if (!all_same_rank(m->cards, m->n) || seat != g->fa) return 0;
        } else {
            for (i = 0; i < m->n; i++) if (!((table_ranks(g) >> gd_rank_of[m->cards[i]]) & 1u)) return 0;
        }
        if (gd_hand_n(g, g->defender) < (uint8_t)(gd_uncovered(g) + m->n)) return 0;
        return g->nb + m->n <= GD_MAXB;
    case GD_COVER:
        if (m->n == 0 || gd_uncovered(g) == 0 || seat != g->defender) return 0;
        if (!cards_in(m->cards, m->n, &g->hand[seat], &set)) return 0;
        for (i = 0; i < m->n; i++) if (m->atts[i] >= GD_NCARDS || battle_of_uncovered(g, m->atts[i]) == 0xFF) return 0;
        if (!cards_in(m->atts, m->n, 0, &atts)) return 0;
        for (i = 0; i < m->n; i++) if (!can_cover(m->atts[i], m->cards[i], g->trump)) return 0;
        return 1;
    case GD_PASS:
        if (m->n == 0) return 0;
        if (!all_same_rank(m->cards, m->n) || !cards_in(m->cards, m->n, 0, &set)) return 0;
        if (seat != g->defender || !cards_in(m->cards, m->n, &g->hand[seat], &set)) return 0;
        if (g->nb == 0 || any_covered(g)) return 0;
        for (i = 0; i < g->nb; i++) if (gd_rank_of[g->att[i]] != gd_rank_of[m->cards[0]]) return 0;
        if (gd_hand_n(g, next_seat(g, g->defender)) < (uint8_t)(m->n + g->nb)) return 0;
        return g->nb + m->n <= GD_MAXB;
    case GD_PICKUP:
        return seat == g->defender && g->nb > 0;
    case GD_GOOD:
        if (g->out & BIT(seat)) return 0;
        if (seat == g->defender) return 0;
        if (g->nb == 0 && seat == g->fa) return 0;
        return !(g->good & BIT(seat));
    }
    return 0;
}

// ---- applying (foolish handle_*) -------------------------------------------------

static void end_bout_discard(gd_game *g) {
    g->discard = (uint8_t)(g->discard + 2u * g->nb);
    g->nb = 0;
    refill(g);
}

uint8_t gd_apply(gd_game *g, gd_rep *r, uint8_t seat, const gd_move *m) {
    uint8_t i;
    if (!gd_legal(g, seat, m)) return 0;
    switch (m->type) {
    case GD_ATTACK:
        for (i = 0; i < m->n; i++) {
            GD_CLR(g->hand[seat], m->cards[i]);
            g->att[g->nb] = m->cards[i];
            g->def[g->nb] = GD_NONE;
            g->nb++;
        }
        g->good = 0;
        // An attacker leaves only when the stock is exhausted too.
        if (gd_hand_n(g, seat) == 0 && no_cards_left(g)) g->out |= BIT(seat);
        break;
    case GD_COVER: {
        const uint8_t d = seat;
        g->good = 0;
        for (i = 0; i < m->n; i++) {
            g->def[battle_of_uncovered(g, m->atts[i])] = m->cards[i];
            GD_CLR(g->hand[d], m->cards[i]);
        }
        if (gd_hand_n(g, d) == 0) {
            // A cover with the last card ends the bout: discard, refill, and the
            // defender leads, unless the refill left them empty: then they are out.
            end_bout_discard(g);
            g->fa = g->defender;
            g->good = 0;
            if (gd_hand_n(g, d) == 0) {
                g->out |= BIT(g->fa);
                g->fa = next_seat(g, g->fa);
            }
            g->defender = next_seat(g, g->fa);
        }
        break;
    }
    case GD_PASS: {
        const uint8_t next = next_seat(g, g->defender);
        for (i = 0; i < m->n; i++) {
            GD_CLR(g->hand[seat], m->cards[i]);
            g->att[g->nb] = m->cards[i];
            g->def[g->nb] = GD_NONE;
            g->nb++;
        }
        g->good = 0;
        if (no_cards_left(g) && gd_hand_n(g, seat) == 0) g->out |= BIT(seat);
        g->defender = next;
        break;
    }
    case GD_PICKUP: {
        const uint8_t stock_before = g->stock_n, flip_before = g->has_flipped;
        g->good = 0;
        for (i = 0; i < g->nb; i++) {
            GD_SET(g->hand[g->defender], g->att[i]);
            if (g->def[i] != GD_NONE) GD_SET(g->hand[g->defender], g->def[i]);
        }
        g->nb = 0;
        refill(g);
        g->fa = next_seat(g, g->defender);
        g->defender = next_seat(g, g->fa);
        g->good = 0;
        if (rep_checkpoint(g, r, g->stock_n != stock_before || g->has_flipped != flip_before))
            g->status = GD_OVER;
        break;
    }
    case GD_GOOD:
        g->good |= BIT(seat);
        // Every attacker still in has said good, over a fully covered table: the
        // bout ends on the discard and the defender leads next.
        {
            uint8_t s, n_att = 0, all_good = 1;
            for (s = 0; s < 2; s++)
                if (s != g->defender && !(g->out & BIT(s))) { n_att++; if (!(g->good & BIT(s))) all_good = 0; }
            if (n_att == 0) all_good = 0;
            if (all_good && g->nb > 0 && gd_uncovered(g) == 0) {
                end_bout_discard(g);
                g->fa = g->defender;
                g->defender = next_seat(g, g->fa);
                g->good = 0;
            }
        }
        break;
    }
    settle(g);
    return 1;
}

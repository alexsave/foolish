// gd_sim - see gd_sim.h. Each function names its twin in foolish's
// cordite_sim.c; tests/simtest.c steps both engines ply by ply and compares.
#include "gd_sim.h"
#include "gd_rng.h"
#include "gd_botlib.h"
#include "gd_simtab.h"

// Phase timing for the bench ROM (tests/simbench.c builds it with SIM_PROF);
// nothing elsewhere.
#ifndef SIM_PROF_DRAW_IN
#define SIM_PROF_DRAW_IN()  ((void)0)
#define SIM_PROF_DRAW_OUT() ((void)0)
#define SIM_PROF_MARK(i)    ((void)0)
#endif

// ---- 5-byte masks, table-driven (no variable shifts: SDCC miscompiled one) ----
typedef const uint8_t *cm;

// Unrolled: SDCC turns a 5-step loop through a pointer into far slower code.
#define M_COUNT(m) ((uint8_t)(SIM_POP8[(m).b[0]] + SIM_POP8[(m).b[1]] + SIM_POP8[(m).b[2]] + SIM_POP8[(m).b[3]] + SIM_POP8[(m).b[4]]))
#define M_EMPTY(m) (!((m).b[0] | (m).b[1] | (m).b[2] | (m).b[3] | (m).b[4]))
#define M_AND(d, a, t)    do { (d).b[0] = (a).b[0] & (t)[0]; (d).b[1] = (a).b[1] & (t)[1]; (d).b[2] = (a).b[2] & (t)[2]; \
                               (d).b[3] = (a).b[3] & (t)[3]; (d).b[4] = (a).b[4] & (t)[4]; } while (0)
#define M_ANDNOT(d, a, t) do { (d).b[0] = (a).b[0] & (uint8_t)~(t)[0]; (d).b[1] = (a).b[1] & (uint8_t)~(t)[1]; \
                               (d).b[2] = (a).b[2] & (uint8_t)~(t)[2]; (d).b[3] = (a).b[3] & (uint8_t)~(t)[3]; \
                               (d).b[4] = (a).b[4] & (uint8_t)~(t)[4]; } while (0)
#define M_OR(d, a)        do { (d).b[0] |= (a).b[0]; (d).b[1] |= (a).b[1]; (d).b[2] |= (a).b[2]; (d).b[3] |= (a).b[3]; (d).b[4] |= (a).b[4]; } while (0)
#define M_COPY(d, a)      do { (d).b[0] = (a).b[0]; (d).b[1] = (a).b[1]; (d).b[2] = (a).b[2]; (d).b[3] = (a).b[3]; (d).b[4] = (a).b[4]; } while (0)
#define M_CLEAR(d)        do { (d).b[0] = 0; (d).b[1] = 0; (d).b[2] = 0; (d).b[3] = 0; (d).b[4] = 0; } while (0)

static uint8_t m_count(const gd_mask *m) { return M_COUNT(*m); }
static uint8_t m_empty(const gd_mask *m) { return M_EMPTY(*m); }
// The lowest card in the mask, or GD_NONE.
static uint8_t m_low(const gd_mask *m) {
    if (m->b[0]) return SIM_LOW8[m->b[0]];
    if (m->b[1]) return (uint8_t)(8u + SIM_LOW8[m->b[1]]);
    if (m->b[2]) return (uint8_t)(16u + SIM_LOW8[m->b[2]]);
    if (m->b[3]) return (uint8_t)(24u + SIM_LOW8[m->b[3]]);
    if (m->b[4]) return (uint8_t)(32u + SIM_LOW8[m->b[4]]);
    return GD_NONE;
}
static void m_and(gd_mask *d, const gd_mask *a, cm b) { M_AND(*d, *a, b); }
static void m_andnot(gd_mask *d, const gd_mask *a, cm b) { M_ANDNOT(*d, *a, b); }
static void m_or(gd_mask *d, const gd_mask *a) { M_OR(*d, *a); }
static void m_clear(gd_mask *d) { M_CLEAR(*d); }

static const uint16_t RANK_BIT[9] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };

#define SEAT_BIT(seat, b0) ((uint8_t)((b0) << (seat)))      /* seat is 0 or 1: a constant shift */
#define IS_OUT(s, p)  ((s)->flags & ((p) ? SIM_OUT1 : SIM_OUT0))
#define IS_GOOD(s, p) ((s)->flags & ((p) ? SIM_GOOD1 : SIM_GOOD0))
#define DEF(s)        (((s)->flags & SIM_DEF1) ? 1u : 0u)
#define FA(s)         (((s)->flags & SIM_FA1) ? 1u : 0u)

#define set_def(s, p)  do { if (p) (s)->flags |= SIM_DEF1; else (s)->flags &= (uint8_t)~SIM_DEF1; } while (0)
#define set_fa(s, p)   do { if (p) (s)->flags |= SIM_FA1; else (s)->flags &= (uint8_t)~SIM_FA1; } while (0)
#define clear_good(s)  ((s)->flags &= (uint8_t)~(SIM_GOOD0 | SIM_GOOD1))

// ---- the board ----------------------------------------------------------------------

// both seats in, one, or none (the out bits are two adjacent flags)
#define BOTH_IN(s) (!((s)->flags & (SIM_OUT0 | SIM_OUT1)))
#define ONE_IN(s)  ((((s)->flags & (SIM_OUT0 | SIM_OUT1)) == SIM_OUT0) || (((s)->flags & (SIM_OUT0 | SIM_OUT1)) == SIM_OUT1))

// sim_next_player, two seats: the other seat, unless one or none is left in.
#define next_seat(s, cur) (BOTH_IN(s) ? (uint8_t)((cur) ^ 1u) : (uint8_t)(cur))

uint8_t gd_sim_done(const gd_sim *s) { return ONE_IN(s); }

#define no_cards_left(s) (!((s)->flags & SIM_FLIPPED) && m_empty(&(s)->stock))
#define uncovered(s)     m_count(&(s)->unc)
#define all_covered(s)   ((s)->nb > 0 && m_empty(&(s)->unc))

static void eliminate(gd_sim *s, uint8_t p) {
    s->flags |= p ? SIM_OUT1 : SIM_OUT0;
    if (s->first_out == GD_NONE) s->first_out = p;
}

// sim_draw: a card at index floor(u * n) of the stock (in card order; the test
// gives foolish's deck array the same order), else the face-up trump.
static uint8_t draw(gd_sim *s, uint8_t *out) {
    uint8_t n = m_count(&s->stock), k, i, b, c;
    SIM_PROF_DRAW_IN();
    if (!n) {
        if (!(s->flags & SIM_FLIPPED)) return 0;
        *out = s->flipped;
        s->flags &= (uint8_t)~SIM_FLIPPED;
        SIM_PROF_DRAW_OUT();
        return 1;
    }
    {
        // floor(r * n / 65536) without a 32-bit multiply (n <= 24): r * n =
        // hi * 256 + lo with hi = (r >> 8) * n and lo = (r & 255) * n
        const uint16_t r = (uint16_t)gd_rng_below(65536ul);
        const uint16_t hi = (uint16_t)((uint8_t)(r >> 8) * (uint16_t)n), lo = (uint16_t)((uint8_t)r * (uint16_t)n);
        k = (uint8_t)((uint16_t)(hi + (lo >> 8)) >> 8);
    }
    for (i = 0; i < 5; i++) {
        b = SIM_POP8[s->stock.b[i]];
        if (k < b) break;
        k = (uint8_t)(k - b);
    }
    b = s->stock.b[i];
    for (;;) {                                     // the k-th set bit of this byte
        c = SIM_LOW8[b];
        if (!k) break;
        b &= (uint8_t)(b - 1u);
        k--;
    }
    c = (uint8_t)(i * 8u + c);
    GD_CLR(s->stock, c);
    *out = c;
    SIM_PROF_DRAW_OUT();
    return 1;
}

static void draw_up_to_six(gd_sim *s, uint8_t p) {
    uint8_t c, n = m_count(&s->hand[p]);
    for (; n < GD_HAND; n++) {
        if (!draw(s, &c)) break;
        GD_SET(s->hand[p], c);
    }
    if (!n && !IS_OUT(s, p)) eliminate(s, p);
}

// sim_refill: the first attacker (if not the defender), then the other
// non-defender, then the defender, last always.
static void refill(gd_sim *s) {
    uint8_t p, visited = 0;
    if (no_cards_left(s)) {
        for (p = 0; p < 2; p++) if (!IS_OUT(s, p) && m_empty(&s->hand[p])) eliminate(s, p);
        return;
    }
    p = FA(s);
    do {
        if (visited & (p ? 2u : 1u)) break;
        visited |= p ? 2u : 1u;
        if (p != DEF(s)) draw_up_to_six(s, p);
        p = next_seat(s, p);
    } while (p != FA(s));
    draw_up_to_six(s, DEF(s));
}

static void clear_table(gd_sim *s) {
    M_CLEAR(s->unc);
    M_CLEAR(s->tbl);
    s->tranks = 0;
    s->nb = 0;
}

// ---- the handlers (sim_apply_*) ----------------------------------------------------

static void put_attack(gd_sim *s, uint8_t p, uint8_t c) {
    GD_CLR(s->hand[p], c);
    GD_SET(s->unc, c);
    GD_SET(s->tbl, c);
    s->tranks |= RANK_BIT[gd_rank_of[c]];
    s->nb++;
}

static void apply_attack(gd_sim *s, uint8_t p, const gd_mask *cards) {
    gd_mask left = *cards;
    uint8_t c;
    while ((c = m_low(&left)) != GD_NONE) { GD_CLR(left, c); put_attack(s, p, c); }
    clear_good(s);
    if (m_empty(&s->hand[p]) && no_cards_left(s)) eliminate(s, p);
}

// The cover: every uncovered attack beaten by its card (cards[i] beats atts[i]).
static void apply_cover(gd_sim *s, uint8_t p, const uint8_t *cards, const uint8_t *atts, uint8_t n) {
    uint8_t i;
    for (i = 0; i < n; i++) {
        GD_CLR(s->unc, atts[i]);
        GD_CLR(s->hand[p], cards[i]);
        GD_SET(s->tbl, cards[i]);
        s->tranks |= RANK_BIT[gd_rank_of[cards[i]]];
    }
    if (m_empty(&s->hand[p])) {
        s->discard = (uint8_t)(s->discard + 2u * s->nb);
        clear_table(s);
        refill(s);
        set_fa(s, DEF(s));
        clear_good(s);
        if (m_empty(&s->hand[FA(s)])) {
            const uint8_t fa = FA(s);
            if (!IS_OUT(s, fa)) eliminate(s, fa);
            set_fa(s, next_seat(s, fa));
        }
        set_def(s, next_seat(s, FA(s)));
        return;
    }
    clear_good(s);
}

static void apply_pass(gd_sim *s, uint8_t p, uint8_t c) {
    const uint8_t next = next_seat(s, DEF(s));
    put_attack(s, p, c);
    clear_good(s);
    if (no_cards_left(s) && m_empty(&s->hand[p])) eliminate(s, p);
    set_def(s, next);
}

static void apply_pickup(gd_sim *s, uint8_t p) {
    m_or(&s->hand[p], &s->tbl);
    clear_table(s);
    refill(s);
    set_fa(s, next_seat(s, DEF(s)));
    set_def(s, next_seat(s, FA(s)));
    clear_good(s);
}

static void round_transition(gd_sim *s) {
    s->discard = (uint8_t)(s->discard + 2u * s->nb);
    clear_table(s);
    refill(s);
    set_fa(s, DEF(s));
    set_def(s, next_seat(s, FA(s)));
    clear_good(s);
}

static void apply_good(gd_sim *s, uint8_t p) {
    uint8_t a;
    s->flags |= p ? SIM_GOOD1 : SIM_GOOD0;
    a = (uint8_t)(DEF(s) ^ 1u);                     // the attacker, at two seats
    if (!IS_OUT(s, a) && IS_GOOD(s, a) && all_covered(s)) round_transition(s);
}

// ---- the rollout policy (sim_handwritten_move) -------------------------------------

// sim_first_attack_group: of the rank groups in `h`, the one with the most
// cards the defender can take (ties to the lowest rank), its lowest cards.
static uint8_t first_attack_group(const gd_sim *s, const gd_mask *h, gd_mask *out) {
    const uint8_t defcap = m_count(&s->hand[DEF(s)]);
    gd_mask hh = *h, g;
    uint8_t best_eff = 0, best_v = 0xFF, v, sz, eff, c;
    if (!defcap || m_empty(h)) return 0;
    while (!m_empty(&hh)) {
        v = gd_rank_of[m_low(&hh)];
        m_and(&g, h, SIM_RANK[v]);
        m_andnot(&hh, &hh, SIM_RANK[v]);
        sz = m_count(&g);
        eff = sz < defcap ? sz : defcap;
        if (eff > best_eff || (eff == best_eff && v < best_v)) { best_eff = eff; best_v = v; }
    }
    m_and(&g, h, SIM_RANK[best_v]);
    m_clear(out);
    for (; best_eff; best_eff--) { c = m_low(&g); GD_CLR(g, c); GD_SET(*out, c); }
    return 1;
}

// sim_attack_group_core: every card of `h`, or when they are more than the
// defender can take, the lowest-scoring ones (non-trumps by rank, then trumps;
// ties in card order).
static uint8_t attack_group(const gd_sim *s, const gd_mask *h, int8_t defcap, gd_mask *out) {
    uint8_t m, r, su, c;
    if (m_empty(h) || defcap <= 0) return 0;
    m = m_count(h);
    if ((int8_t)m <= defcap) { *out = *h; return 1; }
    m_clear(out);
    m = (uint8_t)defcap;
    for (r = 0; r < GD_NRANKS && m; r++)
        for (su = 0; su < GD_NSUITS && m; su++) {
            c = (uint8_t)(su * GD_NRANKS + r);
            if (su != s->trump && GD_HAS(*h, c)) { GD_SET(*out, c); m--; }
        }
    for (r = 0; r < GD_NRANKS && m; r++) {
        c = (uint8_t)(s->trump * GD_NRANKS + r);
        if (GD_HAS(*h, c)) { GD_SET(*out, c); m--; }
    }
    return 1;
}

// sim_greedy_full_cover: each uncovered attack takes its lowest higher card of
// its suit, else (a non-trump attack) the lowest trump. foolish walks the
// attacks in table order; this walks them in card order (the state keeps no
// order). The cards it ends up using are the same either way for these
// nested cover sets; tests/simtest.c checks it on every cover.
static uint8_t greedy_cover(const gd_sim *s, uint8_t p, uint8_t *cards, uint8_t *atts, uint8_t *n) {
    gd_mask avail = s->hand[p], t, left = s->unc;
    uint8_t a, best;
    *n = 0;
    while ((a = m_low(&left)) != GD_NONE) {
        GD_CLR(left, a);
        M_AND(t, avail, SIM_HIGHER[a]);
        best = m_low(&t);
        if (best == GD_NONE && gd_suit_of[a] != s->trump) {
            M_AND(t, avail, SIM_SUIT[s->trump]);
            best = m_low(&t);
        }
        if (best == GD_NONE) return 0;
        GD_CLR(avail, best);
        cards[*n] = best;
        atts[*n] = a;
        (*n)++;
    }
    return 1;
}

// sim_pass_move: one card of the attacks' rank, a non-trump first.
static uint8_t pass_card(const gd_sim *s, uint8_t p) {
    gd_mask m, nt;
    uint8_t v, c;
    if (!s->nb || s->nb != m_count(&s->unc)) return GD_NONE;     // all uncovered
    c = m_low(&s->unc);
    v = gd_rank_of[c];
    if (s->tranks != RANK_BIT[v]) return GD_NONE;                 // all one rank
    m_and(&m, &s->hand[p], SIM_RANK[v]);
    if (m_empty(&m)) return GD_NONE;
    if ((int8_t)m_count(&s->hand[next_seat(s, DEF(s))]) - (int8_t)s->nb < 1) return GD_NONE;
    m_andnot(&nt, &m, SIM_SUIT[s->trump]);
    return m_low(m_empty(&nt) ? &m : &nt);
}

// sim_trump_attack_prob as a threshold on a 16-bit draw: 0.02 while any stock
// or the face-up trump is left.
static uint16_t trump_gate(const gd_sim *s) {
    const uint8_t total = (uint8_t)(m_count(&s->stock) + s->discard + m_count(&s->tbl)
                                    + m_count(&s->hand[0]) + m_count(&s->hand[1]) + ((s->flags & SIM_FLIPPED) ? 1u : 0u));
    return gd_trump_threshold((uint8_t)(!m_empty(&s->stock) || (s->flags & SIM_FLIPPED)), s->discard, total);
}

static uint16_t draw16(void) { uint16_t r; SIM_PROF_DRAW_IN(); r = (uint16_t)gd_rng_below(65536ul); SIM_PROF_DRAW_OUT(); return r; }

#define MV_NONE   0
#define MV_ATTACK 1
#define MV_COVER  2
#define MV_PASS   3
#define MV_PICKUP 4
#define MV_GOOD   5

static uint8_t mv_cards[GD_MAXB], mv_atts[GD_MAXB], mv_n, mv_card;
static gd_mask mv_set;

// The policy's move for seat p (sim_handwritten_move): the kind, its cards in
// mv_set (an attack), mv_card (a pass) or mv_cards/mv_atts (a cover).
static uint8_t policy(gd_sim *s, uint8_t p) {
    const uint8_t first = s->nb == 0, is_def = p == DEF(s);
    uint8_t can_attack;
    gd_mask h_tab, h;
    int8_t defcap = 0;
    if (first) can_attack = p == FA(s);
    else can_attack = !is_def && !IS_GOOD(s, p);
    if (can_attack && !first) {
        M_AND(h_tab, s->hand[p], SIM_RANKSET[s->tranks]);
        defcap = (int8_t)((int8_t)m_count(&s->hand[DEF(s)]) - (int8_t)uncovered(s));
    }
    if (can_attack) {
        uint8_t got;
        // prefer non-trump attacks
        if (first) { m_andnot(&h, &s->hand[p], SIM_SUIT[s->trump]); got = first_attack_group(s, &h, &mv_set); }
        else { m_andnot(&h, &h_tab, SIM_SUIT[s->trump]); got = attack_group(s, &h, defcap, &mv_set); }
        if (got) return MV_ATTACK;
        // with trumps, behind the gate
        if (first) got = first_attack_group(s, &s->hand[p], &mv_set);
        else got = attack_group(s, &h_tab, defcap, &mv_set);
        if (got) {
            if (draw16() < trump_gate(s)) return MV_ATTACK;
            if (!first) return MV_GOOD;               // decline: good
        }
    }
    // the defender: pass, else a full cover, else pick up
    if (is_def && s->nb > 0) {
        mv_card = pass_card(s, p);
        if (mv_card != GD_NONE) return MV_PASS;
        if (greedy_cover(s, p, mv_cards, mv_atts, &mv_n)) return MV_COVER;
        return MV_PICKUP;
    }
    // good, with foolish's draw
    if (!is_def && s->nb > 0 && !IS_GOOD(s, p)) {
        (void)draw16();
        return MV_GOOD;
    }
    // the forced attack (reached by a first attacker who declined)
    if (can_attack) {
        if (!m_empty(&s->stock) || (s->flags & SIM_FLIPPED)) {
            if (first) { m_andnot(&h, &s->hand[p], SIM_SUIT[s->trump]); if (first_attack_group(s, &h, &mv_set)) return MV_ATTACK; }
            else { m_andnot(&h, &h_tab, SIM_SUIT[s->trump]); if (attack_group(s, &h, defcap, &mv_set)) return MV_ATTACK; }
            if (!first) return MV_GOOD;
        }
        if (first ? first_attack_group(s, &s->hand[p], &mv_set) : attack_group(s, &h_tab, defcap, &mv_set)) return MV_ATTACK;
    }
    return MV_NONE;
}

// ---- the playout ------------------------------------------------------------------------

static uint8_t should_act(const gd_sim *s, uint8_t p) {
    if (IS_OUT(s, p)) return 0;
    if (s->nb == 0) return p == FA(s);
    if (p == DEF(s)) return !all_covered(s);
    return !IS_GOOD(s, p);
}

int8_t gd_sim_step(gd_sim *s) GD_BANKED {
    uint8_t p, k;
    if (gd_sim_done(s)) return -1;
    for (p = 0; p < 2; p++) {
        if (!should_act(s, p)) continue;
        SIM_PROF_MARK(0);
        k = policy(s, p);
        SIM_PROF_MARK(1);
        switch (k) {
        case MV_NONE:   continue;
        case MV_ATTACK: apply_attack(s, p, &mv_set); break;
        case MV_COVER:  apply_cover(s, p, mv_cards, mv_atts, mv_n); break;
        case MV_PASS:   apply_pass(s, p, mv_card); break;
        case MV_PICKUP: apply_pickup(s, p); break;
        case MV_GOOD:   apply_good(s, p); break;
        }
        SIM_PROF_MARK(2);
        return (int8_t)p;
    }
    return -1;
}

uint8_t gd_sim_playout(gd_sim *s, uint8_t me, uint16_t max_turns) GD_BANKED {
    uint16_t turns = 0;
    while (!gd_sim_done(s) && turns++ < max_turns)
        if (gd_sim_step(s) < 0) break;
    if (!gd_sim_done(s)) return 0;
    return s->first_out == me ? 1 : 2;
}

void gd_sim_from_game(gd_sim *s, const gd_game *g) {
    uint8_t i;
    s->hand[0] = g->hand[0];
    s->hand[1] = g->hand[1];
    m_clear(&s->stock);
    for (i = 0; i < g->stock_n; i++) GD_SET(s->stock, g->stock[i]);
    m_clear(&s->unc);
    m_clear(&s->tbl);
    s->tranks = 0;
    for (i = 0; i < g->nb; i++) {
        GD_SET(s->tbl, g->att[i]);
        s->tranks |= RANK_BIT[gd_rank_of[g->att[i]]];
        if (g->def[i] == GD_NONE) GD_SET(s->unc, g->att[i]);
        else { GD_SET(s->tbl, g->def[i]); s->tranks |= RANK_BIT[gd_rank_of[g->def[i]]]; }
    }
    s->nb = g->nb;
    s->flipped = g->flipped;
    s->trump = g->trump;
    s->discard = g->discard;
    s->flags = (uint8_t)((g->has_flipped ? SIM_FLIPPED : 0u) | (g->defender ? SIM_DEF1 : 0u) | (g->fa ? SIM_FA1 : 0u)
                         | ((g->good & 1u) ? SIM_GOOD0 : 0u) | ((g->good & 2u) ? SIM_GOOD1 : 0u)
                         | ((g->out & 1u) ? SIM_OUT0 : 0u) | ((g->out & 2u) ? SIM_OUT1 : 0u));
    s->first_out = (g->out & 1u) ? 0 : (g->out & 2u) ? 1 : GD_NONE;
}

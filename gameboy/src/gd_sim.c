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

#if defined(__SDCC)
// SM83 assembly for the mask operations the policy runs most: SDCC's code for
// a 5-byte loop through three pointers cost 346 cycles a call (m_and), against
// about 60 here. Old calling convention (all arguments on the stack, an 8-bit
// result in E) so the register use below is certain. The C versions after the
// #else are the same operations for the Mac build, and tests/simbench.sh makes
// the ROM play exactly the clang build's plies.
static uint8_t m_count(const gd_mask *m) { return M_COUNT(*m); }   // the C one is faster here

static uint8_t m_empty(const gd_mask *m) __sdcccall(0) __naked {
    (void)m;
    __asm
        ldhl    sp, #2
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        ld      a, (hl+)
        or      a, (hl)
        inc     hl
        or      a, (hl)
        inc     hl
        or      a, (hl)
        inc     hl
        or      a, (hl)
        ld      e, #1
        ret     Z
        ld      e, #0
        ret
    __endasm;
}

// d = a & t (t a ROM table row)
static void m_and(gd_mask *d, const gd_mask *a, cm t) __sdcccall(0) __naked {
    (void)d; (void)a; (void)t;
    __asm
        ldhl    sp, #2
        ld      a, (hl+)
        ld      c, a
        ld      a, (hl+)
        ld      b, a            ; bc = d
        ld      a, (hl+)
        ld      e, a
        ld      a, (hl+)
        ld      d, a            ; de = a
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a            ; hl = t
        .rept 5
        ld      a, (de)
        and     a, (hl)
        ld      (bc), a
        inc     de
        inc     hl
        inc     bc
        .endm
        ret
    __endasm;
}

// d = a & ~t
static void m_andnot(gd_mask *d, const gd_mask *a, cm t) __sdcccall(0) __naked {
    (void)d; (void)a; (void)t;
    __asm
        ldhl    sp, #2
        ld      a, (hl+)
        ld      c, a
        ld      a, (hl+)
        ld      b, a
        ld      a, (hl+)
        ld      e, a
        ld      a, (hl+)
        ld      d, a
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        .rept 5
        ld      a, (hl)
        cpl
        push    hl
        ld      h, d
        ld      l, e
        and     a, (hl)
        pop     hl
        ld      (bc), a
        inc     de
        inc     hl
        inc     bc
        .endm
        ret
    __endasm;
}
#else
static uint8_t m_count(const gd_mask *m) { return M_COUNT(*m); }
static uint8_t m_empty(const gd_mask *m) { return M_EMPTY(*m); }
static void m_and(gd_mask *d, const gd_mask *a, cm b) { M_AND(*d, *a, b); }
static void m_andnot(gd_mask *d, const gd_mask *a, cm b) { M_ANDNOT(*d, *a, b); }
#endif
// The lowest card in the mask, or GD_NONE.
static uint8_t m_low(const gd_mask *m) {
    if (m->b[0]) return SIM_LOW8[m->b[0]];
    if (m->b[1]) return (uint8_t)(8u + SIM_LOW8[m->b[1]]);
    if (m->b[2]) return (uint8_t)(16u + SIM_LOW8[m->b[2]]);
    if (m->b[3]) return (uint8_t)(24u + SIM_LOW8[m->b[3]]);
    if (m->b[4]) return (uint8_t)(32u + SIM_LOW8[m->b[4]]);
    return GD_NONE;
}
static void m_or(gd_mask *d, const gd_mask *a) { M_OR(*d, *a); }
static void m_clear(gd_mask *d) { M_CLEAR(*d); }

static const uint16_t RANK_BIT[9] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };

#define SEAT_BIT(seat, b0) ((uint8_t)((b0) << (seat)))      /* seat is 0 or 1: a constant shift */
#define IS_OUT(p)  (W.flags & ((p) ? SIM_OUT1 : SIM_OUT0))
#define IS_GOOD(p) (W.flags & ((p) ? SIM_GOOD1 : SIM_GOOD0))
#define DEF()        ((W.flags & SIM_DEF1) ? 1u : 0u)
#define FA()         ((W.flags & SIM_FA1) ? 1u : 0u)

#define set_def(p)  do { if (p) W.flags |= SIM_DEF1; else W.flags &= (uint8_t)~SIM_DEF1; } while (0)
#define set_fa(p)   do { if (p) W.flags |= SIM_FA1; else W.flags &= (uint8_t)~SIM_FA1; } while (0)
#define clear_good()  (W.flags &= (uint8_t)~(SIM_GOOD0 | SIM_GOOD1))

// The one working state every function below reads and writes: a fixed
// address, which SDCC reaches far faster than a state passed by pointer. A
// playout copies its board in and the result out.
static gd_sim W;

// ---- the board ----------------------------------------------------------------------

// both seats in, one, or none (the out bits are two adjacent flags)
#define BOTH_IN() (!(W.flags & (SIM_OUT0 | SIM_OUT1)))
#define ONE_IN()  (((W.flags & (SIM_OUT0 | SIM_OUT1)) == SIM_OUT0) || ((W.flags & (SIM_OUT0 | SIM_OUT1)) == SIM_OUT1))

// sim_next_player, two seats: the other seat, unless one or none is left in.
#define next_seat(cur) (BOTH_IN() ? (uint8_t)((cur) ^ 1u) : (uint8_t)(cur))

static uint8_t done_w(void) { return ONE_IN(); }

uint8_t gd_sim_done(const gd_sim *s) {
    const uint8_t o = s->flags & (SIM_OUT0 | SIM_OUT1);
    return o == SIM_OUT0 || o == SIM_OUT1;
}

#define no_cards_left() (!(W.flags & SIM_FLIPPED) && m_empty(&W.stock))
#define uncovered()     m_count(&W.unc)
#define all_covered()   (W.nb > 0 && m_empty(&W.unc))

static void eliminate(uint8_t p) {
    W.flags |= p ? SIM_OUT1 : SIM_OUT0;
    if (W.first_out == GD_NONE) W.first_out = p;
}

// sim_draw: a card at index floor(u * n) of the stock (in card order; the test
// gives foolish's deck array the same order), else the face-up trump.
static uint8_t draw(uint8_t *out) {
    uint8_t n = m_count(&W.stock), k, i, b, c;
    SIM_PROF_DRAW_IN();
    if (!n) {
        if (!(W.flags & SIM_FLIPPED)) return 0;
        *out = W.flipped;
        W.flags &= (uint8_t)~SIM_FLIPPED;
        SIM_PROF_DRAW_OUT();
        return 1;
    }
    {
        // floor(r * n / 65536) without a 32-bit multiply (n <= 24): r * n =
        // hi * 256 + lo with hi = (r >> 8) * n and lo = (r & 255) * n
        const uint16_t r = gd_rng_u16();
        const uint16_t hi = (uint16_t)((uint8_t)(r >> 8) * (uint16_t)n), lo = (uint16_t)((uint8_t)r * (uint16_t)n);
        k = (uint8_t)((uint16_t)(hi + (lo >> 8)) >> 8);
    }
    for (i = 0; i < 5; i++) {
        b = SIM_POP8[W.stock.b[i]];
        if (k < b) break;
        k = (uint8_t)(k - b);
    }
    b = W.stock.b[i];
    for (;;) {                                     // the k-th set bit of this byte
        c = SIM_LOW8[b];
        if (!k) break;
        b &= (uint8_t)(b - 1u);
        k--;
    }
    c = (uint8_t)(i * 8u + c);
    GD_CLR(W.stock, c);
    *out = c;
    SIM_PROF_DRAW_OUT();
    return 1;
}

static void draw_up_to_six(uint8_t p) {
    uint8_t c, n = m_count(&W.hand[p]);
    for (; n < GD_HAND; n++) {
        if (!draw(&c)) break;
        GD_SET(W.hand[p], c);
    }
    if (!n && !IS_OUT(p)) eliminate(p);
}

// sim_refill: the first attacker (if not the defender), then the other
// non-defender, then the defender, last always.
static void refill(void) {
    uint8_t p, visited = 0;
    if (no_cards_left()) {
        for (p = 0; p < 2; p++) if (!IS_OUT(p) && m_empty(&W.hand[p])) eliminate(p);
        return;
    }
    p = FA();
    do {
        if (visited & (p ? 2u : 1u)) break;
        visited |= p ? 2u : 1u;
        if (p != DEF()) draw_up_to_six(p);
        p = next_seat(p);
    } while (p != FA());
    draw_up_to_six(DEF());
}

static void clear_table(void) {
    M_CLEAR(W.unc);
    M_CLEAR(W.tbl);
    W.tranks = 0;
    W.nb = 0;
}

// ---- the handlers (sim_apply_*) ----------------------------------------------------

static void put_attack(uint8_t p, uint8_t c) {
    GD_CLR(W.hand[p], c);
    GD_SET(W.unc, c);
    GD_SET(W.tbl, c);
    W.tranks |= RANK_BIT[gd_rank_of[c]];
    W.nb++;
}

static void apply_attack(uint8_t p, const gd_mask *cards) {
    gd_mask left = *cards;
    uint8_t c;
    while ((c = m_low(&left)) != GD_NONE) { GD_CLR(left, c); put_attack(p, c); }
    clear_good();
    if (m_empty(&W.hand[p]) && no_cards_left()) eliminate(p);
}

// The cover: every uncovered attack beaten by its card (cards[i] beats atts[i]).
static void apply_cover(uint8_t p, const uint8_t *cards, const uint8_t *atts, uint8_t n) {
    uint8_t i;
    for (i = 0; i < n; i++) {
        GD_CLR(W.unc, atts[i]);
        GD_CLR(W.hand[p], cards[i]);
        GD_SET(W.tbl, cards[i]);
        W.tranks |= RANK_BIT[gd_rank_of[cards[i]]];
    }
    if (m_empty(&W.hand[p])) {
        W.discard = (uint8_t)(W.discard + 2u * W.nb);
        clear_table();
        refill();
        set_fa(DEF());
        clear_good();
        if (m_empty(&W.hand[FA()])) {
            const uint8_t fa = FA();
            if (!IS_OUT(fa)) eliminate(fa);
            set_fa(next_seat(fa));
        }
        set_def(next_seat(FA()));
        return;
    }
    clear_good();
}

static void apply_pass(uint8_t p, uint8_t c) {
    const uint8_t next = next_seat(DEF());
    put_attack(p, c);
    clear_good();
    if (no_cards_left() && m_empty(&W.hand[p])) eliminate(p);
    set_def(next);
}

static void apply_pickup(uint8_t p) {
    m_or(&W.hand[p], &W.tbl);
    clear_table();
    refill();
    set_fa(next_seat(DEF()));
    set_def(next_seat(FA()));
    clear_good();
}

static void round_transition(void) {
    W.discard = (uint8_t)(W.discard + 2u * W.nb);
    clear_table();
    refill();
    set_fa(DEF());
    set_def(next_seat(FA()));
    clear_good();
}

static void apply_good(uint8_t p) {
    uint8_t a;
    W.flags |= p ? SIM_GOOD1 : SIM_GOOD0;
    a = (uint8_t)(DEF() ^ 1u);                     // the attacker, at two seats
    if (!IS_OUT(a) && IS_GOOD(a) && all_covered()) round_transition();
}

// ---- the rollout policy (sim_handwritten_move) -------------------------------------

// sim_first_attack_group: of the rank groups in `h`, the one with the most
// cards the defender can take (ties to the lowest rank), its lowest cards.
static uint8_t first_attack_group(const gd_mask *h, gd_mask *out) {
    const uint8_t defcap = m_count(&W.hand[DEF()]);
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
static uint8_t attack_group(const gd_mask *h, int8_t defcap, gd_mask *out) {
    uint8_t m, r, su, c;
    if (m_empty(h) || defcap <= 0) return 0;
    m = m_count(h);
    if ((int8_t)m <= defcap) { *out = *h; return 1; }
    m_clear(out);
    m = (uint8_t)defcap;
    for (r = 0; r < GD_NRANKS && m; r++)
        for (su = 0; su < GD_NSUITS && m; su++) {
            c = (uint8_t)(su * GD_NRANKS + r);
            if (su != W.trump && GD_HAS(*h, c)) { GD_SET(*out, c); m--; }
        }
    for (r = 0; r < GD_NRANKS && m; r++) {
        c = (uint8_t)(W.trump * GD_NRANKS + r);
        if (GD_HAS(*h, c)) { GD_SET(*out, c); m--; }
    }
    return 1;
}

// sim_greedy_full_cover: each uncovered attack takes its lowest higher card of
// its suit, else (a non-trump attack) the lowest trump. foolish walks the
// attacks in table order; this walks them in card order (the state keeps no
// order). The cards it ends up using are the same either way for these
// nested cover sets; tests/simtest.c checks it on every cover.
static uint8_t greedy_cover(uint8_t p, uint8_t *cards, uint8_t *atts, uint8_t *n) {
    gd_mask avail = W.hand[p], t, left = W.unc;
    uint8_t a, best;
    *n = 0;
    while ((a = m_low(&left)) != GD_NONE) {
        GD_CLR(left, a);
        M_AND(t, avail, SIM_HIGHER[a]);
        best = m_low(&t);
        if (best == GD_NONE && gd_suit_of[a] != W.trump) {
            M_AND(t, avail, SIM_SUIT[W.trump]);
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
static uint8_t pass_card(uint8_t p) {
    gd_mask m, nt;
    uint8_t v, c;
    if (!W.nb || W.nb != m_count(&W.unc)) return GD_NONE;     // all uncovered
    c = m_low(&W.unc);
    v = gd_rank_of[c];
    if (W.tranks != RANK_BIT[v]) return GD_NONE;                 // all one rank
    m_and(&m, &W.hand[p], SIM_RANK[v]);
    if (m_empty(&m)) return GD_NONE;
    if ((int8_t)m_count(&W.hand[next_seat(DEF())]) - (int8_t)W.nb < 1) return GD_NONE;
    m_andnot(&nt, &m, SIM_SUIT[W.trump]);
    return m_low(m_empty(&nt) ? &m : &nt);
}

// sim_trump_attack_prob as a threshold on a 16-bit draw: 0.02 while any stock
// or the face-up trump is left.
static uint16_t trump_gate(void) {
    const uint8_t total = (uint8_t)(m_count(&W.stock) + W.discard + m_count(&W.tbl)
                                    + m_count(&W.hand[0]) + m_count(&W.hand[1]) + ((W.flags & SIM_FLIPPED) ? 1u : 0u));
    return gd_trump_threshold((uint8_t)(!m_empty(&W.stock) || (W.flags & SIM_FLIPPED)), W.discard, total);
}

static uint16_t draw16(void) { uint16_t r; SIM_PROF_DRAW_IN(); r = gd_rng_u16(); SIM_PROF_DRAW_OUT(); return r; }

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
static uint8_t policy(uint8_t p) {
    const uint8_t first = W.nb == 0, is_def = p == DEF();
    uint8_t can_attack;
    gd_mask h_tab, h;
    int8_t defcap = 0;
    if (first) can_attack = p == FA();
    else can_attack = !is_def && !IS_GOOD(p);
    if (can_attack && !first) {
        M_AND(h_tab, W.hand[p], SIM_RANKSET[W.tranks]);
        defcap = (int8_t)((int8_t)m_count(&W.hand[DEF()]) - (int8_t)uncovered());
        // Nothing to add: both attack groups come back empty, and the good
        // branch below is where the policy ends (its draw included). Straight there.
        if (M_EMPTY(h_tab) || defcap <= 0) { (void)draw16(); return MV_GOOD; }
    }
    if (can_attack) {
        uint8_t got;
        // prefer non-trump attacks
        if (first) { m_andnot(&h, &W.hand[p], SIM_SUIT[W.trump]); got = first_attack_group(&h, &mv_set); }
        else { m_andnot(&h, &h_tab, SIM_SUIT[W.trump]); got = attack_group(&h, defcap, &mv_set); }
        if (got) return MV_ATTACK;
        // with trumps, behind the gate
        if (first) got = first_attack_group(&W.hand[p], &mv_set);
        else got = attack_group(&h_tab, defcap, &mv_set);
        if (got) {
            if (draw16() < trump_gate()) return MV_ATTACK;
            if (!first) return MV_GOOD;               // decline: good
        }
    }
    // the defender: pass, else a full cover, else pick up
    if (is_def && W.nb > 0) {
        mv_card = pass_card(p);
        if (mv_card != GD_NONE) return MV_PASS;
        if (greedy_cover(p, mv_cards, mv_atts, &mv_n)) return MV_COVER;
        return MV_PICKUP;
    }
    // good, with foolish's draw
    if (!is_def && W.nb > 0 && !IS_GOOD(p)) {
        (void)draw16();
        return MV_GOOD;
    }
    // the forced attack (reached by a first attacker who declined)
    if (can_attack) {
        if (!m_empty(&W.stock) || (W.flags & SIM_FLIPPED)) {
            if (first) { m_andnot(&h, &W.hand[p], SIM_SUIT[W.trump]); if (first_attack_group(&h, &mv_set)) return MV_ATTACK; }
            else { m_andnot(&h, &h_tab, SIM_SUIT[W.trump]); if (attack_group(&h, defcap, &mv_set)) return MV_ATTACK; }
            if (!first) return MV_GOOD;
        }
        if (first ? first_attack_group(&W.hand[p], &mv_set) : attack_group(&h_tab, defcap, &mv_set)) return MV_ATTACK;
    }
    return MV_NONE;
}

// ---- the playout ------------------------------------------------------------------------

static uint8_t should_act(uint8_t p) {
    if (IS_OUT(p)) return 0;
    if (W.nb == 0) return p == FA();
    if (p == DEF()) return !all_covered();
    return !IS_GOOD(p);
}

// One ply on W.
static int8_t step_w(void) {
    uint8_t p, k;
    if (done_w()) return -1;
    for (p = 0; p < 2; p++) {
        if (!should_act(p)) continue;
        SIM_PROF_MARK(0);
        k = policy(p);
        SIM_PROF_MARK(1);
        switch (k) {
        case MV_NONE:   continue;
        case MV_ATTACK: apply_attack(p, &mv_set); break;
        case MV_COVER:  apply_cover(p, mv_cards, mv_atts, mv_n); break;
        case MV_PASS:   apply_pass(p, mv_card); break;
        case MV_PICKUP: apply_pickup(p); break;
        case MV_GOOD:   apply_good(p); break;
        }
        SIM_PROF_MARK(2);
        return (int8_t)p;
    }
    return -1;
}

int8_t gd_sim_step(gd_sim *s) GD_BANKED {
    int8_t r;
    W = *s;
    r = step_w();
    *s = W;
    return r;
}

uint16_t gd_sim_plies;

uint8_t gd_sim_playout(gd_sim *s, uint8_t me, uint16_t max_turns) GD_BANKED {
    uint16_t turns = 0;
    W = *s;
    gd_sim_plies = 0;
    while (!done_w() && turns++ < max_turns) {
        if (step_w() < 0) break;
        gd_sim_plies++;
    }
    *s = W;
    if (!done_w()) return 0;
    return W.first_out == me ? 1 : 2;
}

void gd_sim_from_game(gd_sim *s, const gd_game *g) GD_BANKED {
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

// gd_sim - see gd_sim.h. Each function names its twin in foolish's
// cordite_sim.c; tests/simtest.c steps both engines ply by ply and compares.
#include "gd_simboard.h"

uint8_t gd_sim_done(const gd_sim *s) {
    const uint8_t o = s->flags & (SIM_OUT0 | SIM_OUT1);
    return o == SIM_OUT0 || o == SIM_OUT1;
}

// ---- what only the rollout uses ----------------------------------------------------

#if defined(__SDCC)
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
static void m_andnot(gd_mask *d, const gd_mask *a, cm b) { M_ANDNOT(*d, *a, b); }
#endif

static uint8_t done_w(void) { return ONE_IN(); }

static void apply_pass(uint8_t p, uint8_t c) {
    const uint8_t next = next_seat(DEF());
    put_attack(p, c);
    clear_good();
    if (no_cards_left() && m_empty(&W.hand[p])) eliminate(p);
    set_def(next);
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

// A menu move (gd_rules) played on a rollout state (cd_sim_apply_root_move).
void gd_sim_apply_move(gd_sim *s, uint8_t seat, const gd_move *m) GD_BANKED {
    gd_mask set;
    uint8_t i, cards[GD_MAXB], atts[GD_MAXB];
    W = *s;
    m_clear(&set);
    for (i = 0; i < m->n; i++) GD_SET(set, m->cards[i]);
    switch (m->type) {
    case GD_ATTACK: apply_attack(seat, &set); break;
    case GD_PASS:   apply_pass_set(seat, &set); break;
    case GD_PICKUP: apply_pickup(seat); break;
    case GD_GOOD:   apply_good(seat); break;
    case GD_COVER:
        for (i = 0; i < m->n; i++) { cards[i] = m->cards[i]; atts[i] = m->atts[i]; }
        apply_cover(seat, cards, atts, m->n);
        break;
    }
    *s = W;
}


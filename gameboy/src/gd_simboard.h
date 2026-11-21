// gd_simboard - the board of the rollout state and its rules: the 5-byte mask
// operations, the working state W, and foolish's sim_apply_* handlers, as
// static code. Not a library: each engine that plays on the board includes it
// once and so has its own copy of the code and of W in its own ROM bank (the
// rollout engine, gd_sim.c, and the endgame solver, gd_solve.c), so neither
// pays a bank switch per call. One source, so one set of rules: the sim test
// proves the rollout engine's copy, the solver test the solver's.
#ifndef GD_SIMBOARD_H
#define GD_SIMBOARD_H

#include "gd_sim.h"
#include "gd_rng.h"
#include "gd_botlib.h"
#include "gd_simtab.h"
#include <string.h>

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

#else
static uint8_t m_count(const gd_mask *m) { return M_COUNT(*m); }
static uint8_t m_empty(const gd_mask *m) { return M_EMPTY(*m); }
static void m_and(gd_mask *d, const gd_mask *a, cm b) { M_AND(*d, *a, b); }
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

static uint8_t should_act(uint8_t p) {
    if (IS_OUT(p)) return 0;
    if (W.nb == 0) return p == FA();
    if (p == DEF()) return !all_covered();
    return !IS_GOOD(p);
}
// sim_apply_pass, any number of cards
static void apply_pass_set(uint8_t p, const gd_mask *cards) {
    const uint8_t next = next_seat(DEF());
    gd_mask left = *cards;
    uint8_t c;
    while ((c = m_low(&left)) != GD_NONE) { GD_CLR(left, c); put_attack(p, c); }
    clear_good();
    if (no_cards_left() && m_empty(&W.hand[p])) eliminate(p);
    set_def(next);
}

#endif

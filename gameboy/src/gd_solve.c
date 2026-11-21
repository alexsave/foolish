// gd_solve - see gd_solve.h. On its own copy of the board (gd_simboard.h),
// in its own ROM bank.
#include "gd_simboard.h"
#include "gd_solve.h"

// ==== the endgame solver (cordite_sim.c: cd_sim_solve_d, sim_solve_rec) ====
//
// Exact alpha-beta when the stock and the face-up trump are gone and both
// seats are in, so nothing is hidden and nothing is drawn: foolish's search
// node for node in its rules (the actor is the defender if it acts, else the
// lowest seat that does; values are +-(1000 - depth) from `me`'s side, 0 for a
// draw; fail-soft; the table keeps exact values only, those strictly inside
// the window they were found in; depth cap 48; a node with more than 96 moves
// aborts the solve). The move set is foolish's full menu with one difference:
// foolish lists a cover once per pairing of cards to attacks, which all lead
// to the same position, and this lists each (attacks covered, cards used)
// once. A repeated child cannot change a minimum or a maximum, so every value
// is the same; tests/solvetest.c proves it against foolish's solver.
//
// The search is a loop over an explicit stack, not a recursion (48 frames of
// SDCC's locals would not fit the CPU stack), and the stack lives in cartridge
// RAM (bank 0 of four 8 KB banks; banks 1 to 3 hold the table). On the Mac the
// same layout is plain arrays.

#define SOLVE_MAX_DEPTH 48
#define SOLVE_MOVES_CAP 96
#define SOLVE_FRAMES    (SOLVE_MAX_DEPTH + 1)
#define SOLVE_SLOTS     640         // the move stack, shared by the frames on the path
#define TT_ENTRIES      1024        // a bank
#define TT_EMPTY        (-128)

// Phase timing in the solver bench only (tests/solvecheck.c built with
// SOLVE_PROF): [0] the node's checks, hash and probe, [1] move generation,
// [2] applying a move with its state copies, [3] the bookkeeping going back up.
#ifdef SOLVE_PROF
void gb_prof_mark(uint8_t i);
#define SOLVE_MARK(i) gb_prof_mark(i)
#else
#define SOLVE_MARK(i) ((void)0)
#endif

#define SM_ATTACK 1
#define SM_COVER  2
#define SM_PASS   3
#define SM_PICKUP 4
#define SM_GOOD   5

typedef struct {
    uint8_t type;
    gd_mask set;            // the cards played
    uint8_t sub;            // a cover: the attacks covered, a bit each in card order of the uncovered
    uint8_t pad;
} sol_move;                 // 8 bytes

typedef struct {
    int16_t alpha, beta, alpha0, beta0, best;
    sol_move *mbase;
    uint8_t n, i, maxing, actor;
    uint8_t bank;
    uint16_t idx;
    uint8_t tag[4];
    uint8_t pad;
} sol_frame;                // 24 bytes

#if defined(__SDCC)
#define ST_BASE ((gd_sim *)0xA000u)
#define FR_BASE ((sol_frame *)(0xA000u + SOLVE_FRAMES * sizeof(gd_sim)))
#define MV_BASE ((sol_move *)(0xA000u + SOLVE_FRAMES * sizeof(gd_sim) + SOLVE_FRAMES * sizeof(sol_frame)))
#define RAM_BANK(b) (*(volatile uint8_t *)0x4000u = (b))
#define TT_TAGS ((uint8_t *)0xA000u)
#define TT_VALS ((int8_t *)0xB000u)
#else
static gd_sim    st_mem[SOLVE_FRAMES];
static sol_frame fr_mem[SOLVE_FRAMES];
static sol_move  mv_mem[SOLVE_SLOTS];
static uint8_t   tt_mem[4][8192];
static uint8_t   ram_bank;
#define ST_BASE st_mem
#define FR_BASE fr_mem
#define MV_BASE mv_mem
#define RAM_BANK(b) (ram_bank = (b))
#define TT_TAGS (&tt_mem[ram_bank][0])
#define TT_VALS ((int8_t *)&tt_mem[ram_bank][0x1000])
#endif
#define MV_END (MV_BASE + SOLVE_SLOTS)
#if defined(__SDCC)
// the stack must fit bank 0 of the cart's RAM (0xA000 to 0xBFFF)
typedef char sol_fits[(SOLVE_FRAMES * sizeof(gd_sim) + SOLVE_FRAMES * sizeof(sol_frame) + SOLVE_SLOTS * sizeof(sol_move) <= 0x1F00u) ? 1 : -1];   /* 0xBF00: the hash table */
#endif

static sol_move *mv_top;
static uint8_t gen_over;            // the node has too many moves, or the move stack is full
static uint8_t gen_n;

// the next move slot, or none (and the node aborts)
static sol_move *slot(void) {
    if (gen_n >= SOLVE_MOVES_CAP + 1 || mv_top + gen_n >= MV_END) { gen_over = 1; return 0; }
    return mv_top + gen_n++;
}

// a card in or out of a mask, by lookup
#define SET_C(m, c) ((m).b[SIM_CBYTE[c]] |= SIM_CBIT[c])
#define CLR_C(m, c) ((m).b[SIM_CBYTE[c]] &= (uint8_t)~SIM_CBIT[c])

// the cards of a mask, in card order
static uint8_t ids_of(const gd_mask *m, uint8_t *ids) {
    uint8_t n = 0, i, b, base = 0;
    for (i = 0; i < 5; i++, base += 8) {
        b = m->b[i];
        while (b) { ids[n++] = (uint8_t)(base + SIM_LOW8[b]); b &= (uint8_t)(b - 1u); }
    }
    return n;
}

// every subset of ids[0..k) with 1..cap cards, as moves of a type: include
// or skip each card in turn, never past cap
static const uint8_t *gs_ids;
static uint8_t gs_k, gs_cap, gs_type;
static gd_mask gs_set;
static void subsets_rec(uint8_t j, uint8_t cnt) {
    sol_move *m;
    if (gen_over) return;
    if (j == gs_k) {
        if (!cnt || !(m = slot())) return;
        m->type = gs_type;
        m->sub = 0;
        m->set = gs_set;
        return;
    }
    if (cnt < gs_cap) {
        const uint8_t c = gs_ids[j];
        SET_C(gs_set, c);
        subsets_rec((uint8_t)(j + 1), (uint8_t)(cnt + 1));
        CLR_C(gs_set, c);
    }
    subsets_rec((uint8_t)(j + 1), cnt);
}
static void gen_subsets(const uint8_t *ids, uint8_t k, uint8_t cap, uint8_t type) {
    gs_ids = ids; gs_k = k; gs_cap = cap; gs_type = type;
    m_clear(&gs_set);
    subsets_rec(0, 0);
}

// the uncovered attacks the defender holds at least one card to beat, in card
// order (a cover's attack subset is a bit each of these)
// bt = the defender's cards that beat a
static gd_mask bt;
#define BEATERS(a, def) m_and(&bt, &W.hand[def], SIM_BEAT[W.trump][a])
static uint8_t coverable(uint8_t def, uint8_t *u) {
    uint8_t all[GD_NCARDS], k = ids_of(&W.unc, all), n = 0, i, a;
    for (i = 0; i < k; i++) {
        a = all[i];
        BEATERS(a, def);
        if (!m_empty(&bt)) u[n++] = a;
    }
    return n;
}

static uint8_t cv_att[GD_HAND * 3], cv_k, cv_sub, cv_def;
static gd_mask cv_used;
static sol_move *cv_block;          // the first move of this attack subset (the dedupe)

static void cover_rec(uint8_t t) {
    gd_mask opts;
    uint8_t c, a;
    sol_move *m;
    if (gen_over) return;
    if (t == cv_k) {
        for (m = cv_block; m < mv_top + gen_n; m++)
            if (m->set.b[0] == cv_used.b[0] && m->set.b[1] == cv_used.b[1] && m->set.b[2] == cv_used.b[2]
                && m->set.b[3] == cv_used.b[3] && m->set.b[4] == cv_used.b[4]) return;
        if (!(m = slot())) return;
        m->type = SM_COVER;
        m->set = cv_used;
        m->sub = cv_sub;
        return;
    }
    a = cv_att[t];
    BEATERS(a, cv_def);
    M_ANDNOT(opts, bt, cv_used.b);
    while ((c = m_low(&opts)) != GD_NONE) {
        CLR_C(opts, c);
        SET_C(cv_used, c);
        cover_rec((uint8_t)(t + 1));
        CLR_C(cv_used, c);
        if (gen_over) return;
    }
}

static uint8_t sol_should_act(uint8_t p) { return should_act(p); }

// sim_gen_moves: the actor's whole menu onto the move stack; returns the count
static uint8_t gen_moves(uint8_t p) {
    uint8_t ids[GD_NCARDS], k, cap, v, nu, j;
    uint8_t u[GD_NCARDS];
    gd_mask h, g;
    uint16_t sub, end;
    sol_move *m;
    gen_n = 0;
    gen_over = 0;
    if (W.nb == 0) {
        if (p != FA()) return 0;
        // first attack: each rank's cards, any 1..(defender's cards) of them
        cap = m_count(&W.hand[DEF()]);
        for (v = 0; v < GD_NRANKS && !gen_over; v++) {
            m_and(&g, &W.hand[p], SIM_RANK[v]);
            if (m_empty(&g)) continue;
            k = ids_of(&g, ids);
            gen_subsets(ids, k, cap, SM_ATTACK);
        }
        return gen_n;
    }
    if (p == DEF()) {
        // covers: each non-empty set of uncovered attacks, each set of cards
        // that can beat them one to one
        nu = coverable(p, u);
        if (nu > 8) { gen_over = 1; return gen_n; }
        cv_def = p;
        end = SIM_POW2[nu];
        for (sub = 1; sub < end && !gen_over; sub++) {
            cv_k = 0;
            for (j = 0; j < nu; j++) if (sub & SIM_POW2[j]) cv_att[cv_k++] = u[j];
            cv_sub = (uint8_t)sub;
            m_clear(&cv_used);
            cv_block = mv_top + gen_n;
            cover_rec(0);
        }
        if (gen_over) return gen_n;
        if (!all_covered()) { if (!(m = slot())) return gen_n; m->type = SM_PICKUP; m_clear(&m->set); m->sub = 0; }
        // transfers: all attacks uncovered and of one rank, any 1..k of that rank
        if (W.nb == uncovered()) {
            v = gd_rank_of[m_low(&W.unc)];
            if (W.tranks == RANK_BIT[v]) {
                m_and(&g, &W.hand[p], SIM_RANK[v]);
                if (!m_empty(&g)) {
                    const int8_t kmax = (int8_t)((int8_t)m_count(&W.hand[next_seat(DEF())]) - (int8_t)W.nb);
                    if (kmax >= 1) { k = ids_of(&g, ids); gen_subsets(ids, k, (uint8_t)kmax, SM_PASS); }
                }
            }
        }
        return gen_n;
    }
    if (IS_GOOD(p)) return 0;
    // throw-ins: any 1..(defender's cards - uncovered) cards of the table's ranks
    M_AND(h, W.hand[p], SIM_RANKSET[W.tranks]);
    {
        const int8_t defcap = (int8_t)((int8_t)m_count(&W.hand[DEF()]) - (int8_t)uncovered());
        if (!m_empty(&h) && defcap > 0) {
            k = ids_of(&h, ids);
            gen_subsets(ids, k, (uint8_t)defcap, SM_ATTACK);
        }
    }
    if (gen_over) return gen_n;
    if (!(m = slot())) return gen_n;
    m->type = SM_GOOD; m_clear(&m->set); m->sub = 0;
    return gen_n;
}

static void apply_sol(uint8_t p, const sol_move *m) {
    uint8_t cards[GD_NCARDS], atts[GD_NCARDS], u[GD_NCARDS], nu, n, j, k;
    switch (m->type) {
    case SM_ATTACK: apply_attack(p, &m->set); break;
    case SM_PASS:   apply_pass_set(p, &m->set); break;
    case SM_PICKUP: apply_pickup(p); break;
    case SM_GOOD:   apply_good(p); break;
    case SM_COVER:
        nu = coverable(p, u);
        n = ids_of(&m->set, cards);
        for (j = 0, k = 0; j < nu; j++) if (m->sub & (uint8_t)SIM_POW2[j]) atts[k++] = u[j];
        apply_cover(p, cards, atts, n);
        break;
    }
}

// Pearson lanes over the position (both hands, the uncovered attacks, the
// table, the roles and the trump): hk[0] and two bits of hk[1] the slot,
// hk[1] the bank, hk[1..4] the tag. Each lane is h = T[h ^ byte], started at
// T[lane]. On the Game Boy the table is copied to 0xBF00 of the cart's RAM
// (bank 0, by gd_solve_reset) so a lookup is one load through HL; the C
// version below is the same function, for the Mac build.
static uint8_t hk[5], hx;
#include <stddef.h>
// the assembly reads the hands at W + 0 and the uncovered attacks and table at W + 15
typedef char hash_layout[(offsetof(gd_sim, hand) == 0 && offsetof(gd_sim, unc) == 15 && offsetof(gd_sim, tbl) == 20) ? 1 : -1];
#define HASH_T ((uint8_t *)0xBF00u)
#if defined(__SDCC)
static void hash_lanes(void) __naked {
    __asm
        ld      h, #0xBF
        ld      l, #0
        ld      c, (hl)
        ld      de, #_W
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      de, #(_W + 15)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (_hx)
        xor     a, c
        ld      l, a
        ld      a, (hl)
        ld      (_hk + 0), a
        ld      l, #1
        ld      c, (hl)
        ld      de, #_W
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      de, #(_W + 15)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (_hx)
        xor     a, c
        ld      l, a
        ld      a, (hl)
        ld      (_hk + 1), a
        ld      l, #2
        ld      c, (hl)
        ld      de, #_W
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      de, #(_W + 15)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (_hx)
        xor     a, c
        ld      l, a
        ld      a, (hl)
        ld      (_hk + 2), a
        ld      l, #3
        ld      c, (hl)
        ld      de, #_W
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      de, #(_W + 15)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (_hx)
        xor     a, c
        ld      l, a
        ld      a, (hl)
        ld      (_hk + 3), a
        ld      l, #4
        ld      c, (hl)
        ld      de, #_W
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      de, #(_W + 15)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (de)
        inc     de
        xor     a, c
        ld      l, a
        ld      c, (hl)
        ld      a, (_hx)
        xor     a, c
        ld      l, a
        ld      a, (hl)
        ld      (_hk + 4), a
        ret
    __endasm;
}
#else
static void hash_lanes(void) {
    uint8_t j, i, h;
    const uint8_t *p;
    for (j = 0; j < 5; j++) {
        h = SIM_PEARSON[j];
        p = (const uint8_t *)&W.hand[0];
        for (i = 0; i < 10; i++) h = SIM_PEARSON[h ^ p[i]];
        p = (const uint8_t *)&W.unc;
        for (i = 0; i < 10; i++) h = SIM_PEARSON[h ^ p[i]];
        hk[j] = SIM_PEARSON[h ^ hx];
    }
}
#endif
static void hash_w(void) {
    hx = (uint8_t)((W.flags & (SIM_DEF1 | SIM_FA1 | SIM_GOOD0 | SIM_GOOD1)) | (uint8_t)(W.trump << 5));
    hash_lanes();
}

uint32_t gd_solve_nodes;            // nodes expanded, all solves since the last reset

void gd_solve_reset(void) GD_BANKED {
    uint8_t b;
    uint16_t i;
    for (b = 1; b <= 3; b++) {
        RAM_BANK(b);
        for (i = 0; i < TT_ENTRIES; i++) TT_VALS[i] = TT_EMPTY;
    }
    RAM_BANK(0);
#if defined(__SDCC)
    for (i = 0; i < 256; i++) HASH_T[i] = SIM_PEARSON[i];
#endif
    gd_solve_nodes = 0;
}

int16_t gd_solve(const gd_sim *root, uint8_t me, int16_t alpha, int16_t beta,
                 uint32_t *budget, uint8_t depth0, uint8_t *aborted) GD_BANKED {
    sol_frame *f = FR_BASE;
    gd_sim *st = ST_BASE;
    uint8_t depth = depth0, actor, o, n;
    int16_t v, result = 0;
    uint32_t bud = *budget;
    uint8_t *tag, tb;
    uint16_t ti;
    int8_t tv;
    RAM_BANK(0);
    *st = *root;
    mv_top = MV_BASE;
    f->alpha = alpha;
    f->beta = beta;
enter:
    SOLVE_MARK(0);
    W = *st;
    o = W.flags & (SIM_OUT0 | SIM_OUT1);
    if (o) {
        if (o == (SIM_OUT0 | SIM_OUT1)) v = 0;
        else {
            const uint8_t loser = (o == SIM_OUT0) ? 1u : 0u;
            v = (int16_t)(1000 - depth);
            if (loser == me) v = (int16_t)-v;
        }
        goto up;
    }
    if (depth >= SOLVE_MAX_DEPTH) goto abort;
    if (bud == 0 || --bud == 0) goto abort;
    gd_solve_nodes++;
    if (sol_should_act(DEF())) actor = DEF();
    else if (sol_should_act(0)) actor = 0;
    else if (sol_should_act(1)) actor = 1;
    else { v = 0; goto up; }
    hash_w();
    // The frame is in bank 0 of the cart's RAM and the table in banks 1 to 3:
    // everything the probe needs is taken out of the frame before the switch.
    tb = (uint8_t)(1u + SIM_MOD3[hk[1]]);
    ti = (uint16_t)(hk[0] | ((uint16_t)(hk[1] & 3u) << 8));
    f->bank = tb;
    f->idx = ti;
    f->tag[0] = hk[1]; f->tag[1] = hk[2]; f->tag[2] = hk[3]; f->tag[3] = hk[4];
    RAM_BANK(tb);
    tv = TT_VALS[ti];
    tag = TT_TAGS + (ti << 2);
    if (tv != TT_EMPTY && tag[0] == hk[1] && tag[1] == hk[2] && tag[2] == hk[3] && tag[3] == hk[4]) {
        RAM_BANK(0);
        // canonical: seat 0's side, plies to the end
        if (me) tv = (int8_t)-tv;
        if (tv > 0) v = (int16_t)(1000 - depth - tv);
        else if (tv < 0) v = (int16_t)-(1000 - depth + tv);
        else v = 0;
        goto up;
    }
    RAM_BANK(0);
    SOLVE_MARK(1);
    f->mbase = mv_top;
    n = gen_moves(actor);
    SOLVE_MARK(2);
    if (gen_over || n > SOLVE_MOVES_CAP) goto abort;
    if (!n) { v = 0; goto up; }
    mv_top += n;
    f->n = n;
    f->i = 0;
    f->actor = actor;
    f->maxing = actor == me;
    f->best = f->maxing ? -2000 : 2000;
    f->alpha0 = f->alpha;
    f->beta0 = f->beta;
next:
    SOLVE_MARK(0);
    W = *st;
    apply_sol(f->actor, f->mbase + f->i);
    f->i++;
    st++;
    *st = W;
    f[1].alpha = f->alpha;
    f[1].beta = f->beta;
    f++;
    depth++;
    SOLVE_MARK(3);
    goto enter;
up:
    SOLVE_MARK(0);
    if (f == FR_BASE) { result = v; goto done; }
    f--;
    st--;
    depth--;
    if (f->maxing) { if (v > f->best) f->best = v; if (f->best > f->alpha) f->alpha = f->best; }
    else { if (v < f->best) f->best = v; if (f->best < f->beta) f->beta = f->best; }
    if (f->alpha < f->beta && f->i < f->n) { SOLVE_MARK(4); goto next; }
    mv_top = f->mbase;
    v = f->best;
    if (v > f->alpha0 && v < f->beta0) {
        int16_t s0 = me ? (int16_t)-v : v;
        if (s0 > 0) tv = (int8_t)(1000 - s0 - depth);
        else if (s0 < 0) tv = (int8_t)-(1000 + s0 - depth);
        else tv = 0;
        // out of the frame (bank 0) first, then into the table's bank
        tb = f->bank;
        ti = f->idx;
        hk[1] = f->tag[0]; hk[2] = f->tag[1]; hk[3] = f->tag[2]; hk[4] = f->tag[3];
        RAM_BANK(tb);
        tag = TT_TAGS + (ti << 2);
        tag[0] = hk[1]; tag[1] = hk[2]; tag[2] = hk[3]; tag[3] = hk[4];
        TT_VALS[ti] = tv;
        RAM_BANK(0);
    }
    SOLVE_MARK(4);
    goto up;
abort:
    *budget = bud;
    *aborted = 1;
    return 0;
done:
    *budget = bud;
    *aborted = 0;
    return result;
}

#ifdef SOLVE_BENCH
// The solver's parts, each run `iters` times on the board `s` from the
// actor's seat, timed by `now` (the bench ROM's scanline clock): [0] the
// hash, [1] the two state copies a child costs, [2] generating the moves,
// [3] applying the first move.
void gd_solve_bench(const gd_sim *s, uint16_t iters, uint32_t (*now)(void), uint32_t *out) GD_BANKED {
    uint16_t k;
    uint32_t t;
    uint8_t actor;
    gd_sim *st = ST_BASE;
    RAM_BANK(0);
    *st = *s;
    W = *st;
    actor = should_act(DEF()) ? DEF() : should_act(0) ? 0 : 1;
    mv_top = MV_BASE;
    t = now(); for (k = 0; k < iters; k++) hash_w(); out[0] = now() - t;
    t = now(); for (k = 0; k < iters; k++) { W = *st; st[1] = W; } out[1] = now() - t;
    t = now(); for (k = 0; k < iters; k++) { W = *st; gen_moves(actor); } out[2] = now() - t;
    gen_moves(actor);
    t = now(); for (k = 0; k < iters; k++) { W = *st; apply_sol(actor, MV_BASE); } out[3] = now() - t;
    out[4] = gen_n;
    {
        uint8_t u[GD_NCARDS];
        t = now(); for (k = 0; k < iters; k++) coverable(actor, u); out[5] = now() - t;
        t = now(); for (k = 0; k < iters; k++) ids_of(&W.hand[actor], u); out[6] = now() - t;
        t = now(); for (k = 0; k < iters; k++) { gen_n = 0; slot(); } out[7] = now() - t;
        out[8] = actor == DEF() ? 1 : W.nb == 0 ? 2 : 3;
    }
}
#endif


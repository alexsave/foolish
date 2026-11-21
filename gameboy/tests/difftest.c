// difftest - plays seeded two-player games on BOTH engines at once: this one
// (src/gd_rules.c, src/gd_play.c) and foolish's production kernel
// (../foolish/c/src/game.c, legal.c, read-only, compiled into build/). Both
// seats choose uniformly random legal moves from the one RNG (src/gd_rng.c);
// every move is applied to both engines, and after every move the test checks
// that the two agree on:
//   - each seat's legal move SET (this engine's count + unrank against
//     foolish's enumerated menu, as sorted canonical keys),
//   - hands, the stock in draw order, the trump card and suit, the table in
//     battle order, the leader, the defender, the goods, the outs, the
//     discard count, the status and the result (fool, or the repetition draw),
//   - on random probes: the handlers' accept/reject (gd_legal vs handle_*),
//     menu membership, and the gesture rules the d-pad is built on (pills,
//     coverable battles, the Cover button's aim and the move it resolves to).
//
// usage: difftest [games] [first_seed]
#include "../src/gd_rules.h"
#include "../src/gd_play.h"
#include "../src/gd_rng.h"
#include "../src/gd_bot_hw.h"
#include "../src/gd_botlib.h"

#include "game.h"
#include "legal.h"
#include "strategy.h"
#include <math.h>
#include "foolish_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The two card encodings are mapped explicitly in foolish_map.h.

// ---- New York against foolish's handwritten_prod ---------------------------------
// foolish's bot file is compiled with random_strategy_random renamed to
// dt_random (Makefile), which hands it this engine's draws, u = r / 65536.
static uint16_t dt_queue[8];
static int dt_n, dt_at, dt_over;
double dt_random(void) {
    if (dt_at >= dt_n) { dt_over = 1; return 0.0; }
    return dt_queue[dt_at++] / 65536.0;
}

extern uint16_t gd_hw_nodes;
static unsigned long hw_nodes_max, hw_nodes_sum, hw_covers;
static unsigned long hw_compared, hw_agree, hw_by_type[5], hw_draws_total;



// ---- failure reporting ----------------------------------------------------------
static unsigned long g_seed_now, g_move_now;
static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "MISMATCH seed %lu move %lu: ", g_seed_now, g_move_now); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_fail = 1; return 0; } } while (0)

// ---- canonical move keys ------------------------------------------------------------
#define KEYLEN 40
typedef struct { uint8_t k[KEYLEN]; } Key;

static void key_make(Key *k, uint8_t type, uint8_t n, const uint8_t *cards, const uint8_t *atts) {
    uint8_t pc[GD_MAXB * 2 + 2], i, j;
    memset(k, 0, sizeof *k);
    k->k[0] = type;
    k->k[1] = n;
    // pairs sorted by card id
    for (i = 0; i < n; i++) { pc[2 * i] = cards[i]; pc[2 * i + 1] = type == GD_COVER ? atts[i] : 0xFF; }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && pc[2 * (j - 1)] > pc[2 * j]; j--) {
            uint8_t t0 = pc[2 * j], t1 = pc[2 * j + 1];
            pc[2 * j] = pc[2 * (j - 1)]; pc[2 * j + 1] = pc[2 * (j - 1) + 1];
            pc[2 * (j - 1)] = t0; pc[2 * (j - 1) + 1] = t1;
        }
    memcpy(k->k + 2, pc, (size_t)2 * n);
}
static int key_cmp(const void *a, const void *b) { return memcmp(a, b, KEYLEN); }

static void key_of_gd(Key *k, const gd_move *m) { key_make(k, m->type, m->n, m->cards, m->atts); }
static void key_of_f(Key *k, const LegalMove *m) {
    uint8_t c[64], a[64];
    for (int i = 0; i < m->n_cards; i++) { c[i] = gd_of(m->cards[i]); a[i] = m->type == MOVE_COVER ? gd_of(m->attack_cards[i]) : 0xFF; }
    key_make(k, (uint8_t)m->type, (uint8_t)m->n_cards, c, a);
}

static LegalMoves *g_lm;            // foolish's menu (heap: 65536 moves)
static Key *g_ka, *g_kb;
static unsigned char *g_wire;
#define WIRE_CAP (MAX_LEGAL_MOVES * (2 + 2 * MAX_MOVE_CARDS) + 4)

// stats
static unsigned long st_games, st_moves, st_menus, st_probes, st_gest, st_truncated, st_cap;
static unsigned long st_res[4], st_hist_types[5];
static uint32_t st_max_menu, st_max_unc, st_max_hand;

// ---- state comparison ------------------------------------------------------------------
static int same_state(const gd_game *g, const Game *f) {
    CHECK(f->power_suit == g->trump, "trump %d vs %d", f->power_suit, g->trump);
    CHECK(f->deck_count == g->stock_n, "stock count %d vs %d", f->deck_count, g->stock_n);
    for (int i = 0; i < g->stock_n; i++) CHECK(gd_of(f->deck[i]) == g->stock[i], "stock[%d]", i);
    CHECK(f->has_flipped == (g->has_flipped != 0), "has_flipped");
    if (g->has_flipped) CHECK(gd_of(f->flipped) == g->flipped, "flipped card");
    CHECK(f->num_battles == g->nb, "battles %d vs %d", f->num_battles, g->nb);
    for (int i = 0; i < g->nb; i++) {
        CHECK(gd_of(f->table_battles[i].attack) == g->att[i], "attack %d", i);
        uint8_t d = card_is_none(f->table_battles[i].defense) ? GD_NONE : gd_of(f->table_battles[i].defense);
        CHECK(d == g->def[i], "cover %d", i);
    }
    for (int s = 0; s < 2; s++) {
        gd_mask m;
        memset(&m, 0, sizeof m);
        for (int i = 0; i < f->players[s].hand_count; i++) {
            uint8_t c = gd_of(f->players[s].hand[i]);
            CHECK(c < 36, "bad foolish card");
            GD_SET(m, c);
        }
        CHECK(memcmp(&m, &g->hand[s], sizeof m) == 0, "hand %d", s);
        CHECK((f->players[s].status == PLAYER_STATUS_OUT) == ((g->out >> s) & 1u), "out %d", s);
        if (gd_hand_n(g, s) > st_max_hand) st_max_hand = gd_hand_n(g, s);
    }
    CHECK(f->first_attacker == g->fa, "first attacker %d vs %d", f->first_attacker, g->fa);
    CHECK(f->defender == g->defender, "defender %d vs %d", f->defender, g->defender);
    CHECK((f->good_players_mask & 3u) == g->good, "goods %u vs %u", f->good_players_mask, g->good);
    CHECK(f->discard_pile_length == g->discard, "discard %d vs %d", f->discard_pile_length, g->discard);
    CHECK((f->status == GAME_STATUS_GAME_OVER) == (g->status == GD_OVER), "status %d vs %d", f->status, g->status);
    uint8_t want = GD_RESULT_NONE;
    if (f->status == GAME_STATUS_GAME_OVER) {
        uint32_t fools = game_fools(f);
        want = fools == 3 ? GD_RESULT_DRAW : fools == 1 ? GD_RESULT_FOOL0 : fools == 2 ? GD_RESULT_FOOL1 : 0xEE;
    }
    CHECK(want == gd_result(g), "result %d vs %d", want, gd_result(g));
    return 1;
}

// Hand order is no part of the rules; sorting foolish's hands into card id
// order (which is this engine's hand order) makes its first-match menu order,
// which play_resolve reads, comparable.
static void sort_foolish_hands(Game *f) {
    for (int s = 0; s < 2; s++) {
        Player *p = &f->players[s];
        unsigned char idx[MAX_HAND_SIZE];
        for (int i = 0; i < p->hand_count; i++) idx[i] = (unsigned char)i;
        for (int i = 1; i < p->hand_count; i++)
            for (int j = i; j > 0 && gd_of(p->hand[idx[j - 1]]) > gd_of(p->hand[idx[j]]); j--) {
                unsigned char t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
            }
        if (!game_rearrange_hand(f, s, idx, p->hand_count)) { fprintf(stderr, "rearrange failed\n"); exit(2); }
    }
}

// ---- the menus -----------------------------------------------------------------------
// Returns the count, or -1 on mismatch. Leaves foolish's menu in g_lm.
static long same_menu(const gd_game *g, const Game *f, uint8_t seat) {
    uint32_t n = gd_menu_count(g, seat);
    calculate_legal_moves(f, seat, g_lm);
    st_menus++;
    if (n > st_max_menu) st_max_menu = n;
    if (gd_uncovered(g) > st_max_unc) st_max_unc = gd_uncovered(g);
    if (g_lm->n >= MAX_LEGAL_MOVES) {
        // foolish truncated its menu at its cap: the sets cannot be compared.
        st_truncated++;
        if (n < (uint32_t)g_lm->n) { CHECK(0, "count %u below foolish's truncated %d", n, g_lm->n); }
        return (long)n;
    }
    if (n != (uint32_t)g_lm->n) { CHECK(0, "seat %d menu count %u vs foolish %d", seat, n, g_lm->n); }
    for (uint32_t i = 0; i < n; i++) {
        gd_move m;
        gd_menu_move(g, seat, i, &m);
        if (!gd_in_menu(g, seat, &m)) { CHECK(0, "unranked move %u not in menu", i); }
        if (!gd_legal(g, seat, &m)) { CHECK(0, "unranked move %u not legal", i); }
        key_of_gd(&g_ka[i], &m);
        key_of_f(&g_kb[i], &g_lm->moves[i]);
    }
    qsort(g_ka, n, sizeof(Key), key_cmp);
    qsort(g_kb, n, sizeof(Key), key_cmp);
    for (uint32_t i = 0; i < n; i++) {
        if (i > 0 && memcmp(&g_ka[i], &g_ka[i - 1], sizeof(Key)) == 0) { CHECK(0, "seat %d duplicate move in unrank", seat); }
        if (memcmp(&g_ka[i], &g_kb[i], sizeof(Key)) != 0) { CHECK(0, "seat %d menu sets differ at %u (type %d vs %d)", seat, i, g_ka[i].k[0], g_kb[i].k[0]); }
    }
    return (long)n;
}

// The lite menu against foolish's calculate_legal_moves_lite, as sets.
static unsigned long st_lite;
static int same_lite_menu(const gd_game *g, const Game *f, uint8_t seat) {
    uint32_t n = gd_menu_count_lite(g, seat);
    calculate_legal_moves_lite(f, seat, g_lm);
    if (g_lm->n >= MAX_LEGAL_MOVES) return 1;
    CHECK(n == (uint32_t)g_lm->n, "seat %d lite menu count %u vs foolish %d", seat, n, g_lm->n);
    for (uint32_t i = 0; i < n; i++) {
        gd_move m;
        gd_menu_move_lite(g, seat, i, &m);
        CHECK(gd_legal(g, seat, &m), "lite move %u not legal", i);
        key_of_gd(&g_ka[i], &m);
        key_of_f(&g_kb[i], &g_lm->moves[i]);
    }
    qsort(g_ka, n, sizeof(Key), key_cmp);
    qsort(g_kb, n, sizeof(Key), key_cmp);
    for (uint32_t i = 0; i < n; i++) {
        CHECK(i == 0 || memcmp(&g_ka[i], &g_ka[i - 1], sizeof(Key)), "seat %d duplicate lite move", seat);
        CHECK(!memcmp(&g_ka[i], &g_kb[i], sizeof(Key)), "seat %d lite menus differ at %u", seat, i);
    }
    st_lite++;
    return 1;
}

static int f_menu_index(const LegalMoves *lm, const gd_move *m) {
    Key a, b;
    key_of_gd(&a, m);
    for (int i = 0; i < lm->n; i++) { key_of_f(&b, &lm->moves[i]); if (!memcmp(&a, &b, sizeof a)) return i; }
    return -1;
}


// New York's move for `seat` against foolish's handwritten_prod on the same
// board, fed the same draws. `keep`: the draws count for the game (New York is
// playing the move); else the RNG is put back as it was. 0 on a mismatch.
static int hw_compare(const gd_game *g, const Game *f, uint8_t seat, int keep, gd_move *mine) {
    const uint16_t before = gd_rng_get();
    gd_hw_choose(g, seat, mine);
    const uint8_t k = gd_hw_draws;
    const uint16_t after = gd_rng_get();
    CHECK(k <= 8, "too many draws");
    gd_rng_set(before);
    for (int i = 0; i < k; i++) dt_queue[i] = (uint16_t)gd_rng_below(65536ul);
    CHECK(gd_rng_get() == after, "draw replay differs");
    if (!keep) gd_rng_set(before);
    calculate_legal_moves(f, seat, g_lm);
    if (g_lm->n == 0) { CHECK(mine->type == 0xFF, "New York moves on an empty menu"); return 1; }
    if (g_lm->n >= MAX_LEGAL_MOVES) return 1;
    dt_n = k; dt_at = 0; dt_over = 0;
    int idx = handwritten_prod_strategy_choose(f, seat, g_lm, NULL);
    CHECK(!dt_over && dt_at == dt_n, "draws: New York %d, foolish %d%s", k, dt_at, dt_over ? " (ran out)" : "");
    CHECK(idx >= 0 && idx < g_lm->n, "foolish picked no move");
    CHECK(mine->type != 0xFF, "New York found no move");
    Key a, b;
    key_of_gd(&a, mine);
    key_of_f(&b, &g_lm->moves[idx]);
    if (memcmp(&a, &b, sizeof a)) {
        fprintf(stderr, "  New York type %d n %d:", mine->type, mine->n);
        for (int i = 0; i < mine->n; i++) fprintf(stderr, " %d/%d", mine->cards[i], mine->atts[i]);
        fprintf(stderr, "\n  foolish  type %d n %d:", b.k[0], b.k[1]);
        for (int i = 0; i < b.k[1]; i++) fprintf(stderr, " %d/%d", b.k[2 + 2 * i], b.k[3 + 2 * i]);
        fprintf(stderr, "\n");
        CHECK(0, "seat %d: New York and handwritten_prod differ", seat);
    }
    if (gd_hw_nodes > 600 && getenv("HW_SHOW")) {
        fprintf(stderr, "nodes %u trump %d found %s; attacks:", gd_hw_nodes, g->trump, mine->type == GD_COVER ? "cover" : "none");
        for (int i = 0; i < g->nb; i++) if (g->def[i] == GD_NONE) fprintf(stderr, " %d", g->att[i]);
        fprintf(stderr, "; hand:");
        for (int c = 0; c < 36; c++) if (GD_HAS(g->hand[seat], c)) fprintf(stderr, " %d", c);
        fprintf(stderr, "\n");
    }
    if (gd_hw_nodes) { hw_covers++; hw_nodes_sum += gd_hw_nodes; if (gd_hw_nodes > hw_nodes_max) hw_nodes_max = gd_hw_nodes; }
    hw_compared++;
    hw_agree++;
    hw_by_type[mine->type]++;
    hw_draws_total += k;
    return 1;
}


// ---- probes ------------------------------------------------------------------------
static uint8_t rnd(uint32_t n) { return (uint8_t)gd_rng_below(n); }

static uint8_t hand_list(const gd_game *g, uint8_t seat, uint8_t *out) {
    uint8_t n = 0;
    for (uint8_t c = 0; c < 36; c++) if (GD_HAS(g->hand[seat], c)) out[n++] = c;
    return n;
}

// A random selection from the seat's hand, 0..3 cards (mostly 1).
static uint8_t random_sel(const gd_game *g, uint8_t seat, uint8_t *sel) {
    uint8_t h[36], hn = hand_list(g, seat, h), n, i, k = 0;
    static const uint8_t sizes[8] = { 0, 1, 1, 1, 1, 2, 2, 3 };
    n = sizes[rnd(8)];
    if (n > hn) n = hn;
    for (i = 0; i < n; i++) {
        uint8_t j = rnd(hn - i), t = h[j];
        h[j] = h[hn - i - 1]; h[hn - i - 1] = t;
        sel[k++] = t;
    }
    return k;
}

static int probe_gestures(const gd_game *g, const Game *f, uint8_t seat) {
    // foolish's board, from its own menu for this seat
    calculate_legal_moves(f, seat, g_lm);
    if (g_lm->n >= MAX_LEGAL_MOVES) return 1;
    int len = legal_menu_write(g_lm, 0, -1, g_wire, WIRE_CAP);
    CHECK(len > 0, "menu write");
    unsigned char table[2 * MAX_BATTLES];
    for (int i = 0; i < f->num_battles; i++) {
        table[2 * i] = (unsigned char)card_to_id(f->table_battles[i].attack);
        table[2 * i + 1] = card_is_none(f->table_battles[i].defense) ? LEGAL_WIRE_NONE
                                                                      : (unsigned char)card_to_id(f->table_battles[i].defense);
    }
    PlayBoard b = { .menu = g_wire, .menu_len = len, .table = table, .n_battles = f->num_battles,
                    .power_suit = f->power_suit, .my_seat = seat, .defender = f->defender,
                    .my_status = f->players[seat].status, .game_status = f->status };
    for (int rep = 0; rep < 3; rep++) {
        uint8_t sel[4];
        uint8_t n = random_sel(g, seat, sel);
        unsigned char fsel[4];
        for (int i = 0; i < n; i++) fsel[i] = (unsigned char)card_to_id(f_of(sel[i]));
        st_gest++;
        unsigned fp = play_pills(&b, fsel, n, 0);
        uint8_t gp = gd_pills(g, seat, sel, n);
        CHECK(fp == gp, "seat %d pills %#x vs %#x (n %d)", seat, fp, gp, n);
        uint64_t fc = play_coverable_battles(&b, fsel, n);
        uint32_t gc = gd_coverable(g, seat, sel, n);
        CHECK(fc == gc, "coverable %#llx vs %#x", (unsigned long long)fc, gc);
        int fb = play_best_cover_target(&b, fsel, n);
        int gb = gd_best_cover(g, seat, sel, n);
        CHECK(fb == gb, "best cover %d vs %d", fb, gb);
        if (fb >= 0) {
            int idx = play_resolve(&b, fsel, n, fb);
            gd_move m;
            uint8_t ok = gd_resolve_cover(g, seat, sel, n, (uint8_t)gb, &m);
            CHECK((idx >= 0) == (ok != 0), "resolve found %d vs %d", idx, ok);
            if (idx >= 0) {
                Key a, k2;
                key_of_gd(&a, &m);
                key_of_f(&k2, &g_lm->moves[idx]);
                CHECK(!memcmp(&a, &k2, sizeof a), "resolved cover differs");
            }
        }
    }
    return 1;
}

static Game *g_scratch;
static int g_cycle_seek;      // 0 uniform, 1 steered to the repetition draw, 2 New York plays seat 1

static int probe_legal(const gd_game *g, const Game *f) {
    for (int rep = 0; rep < 4; rep++) {
        gd_move m;
        uint8_t seat = rnd(2), h[36], hn = hand_list(g, seat, h);
        m.type = rnd(5);
        m.n = (uint8_t)(m.type == GD_PICKUP || m.type == GD_GOOD ? 0 : 1 + rnd(3));
        for (int i = 0; i < m.n; i++) {
            // mostly own cards, sometimes any card (not held, or on the table)
            m.cards[i] = (hn && rnd(5)) ? h[rnd(hn)] : rnd(36);
            m.atts[i] = (g->nb && rnd(5)) ? g->att[rnd(g->nb)] : rnd(36);
        }
        st_probes++;
        memcpy(g_scratch, f, sizeof *f);
        bool fok = f_apply(g_scratch, seat, &m);
        CHECK(fok == (gd_legal(g, seat, &m) != 0), "probe type %d n %d seat %d: foolish %d gd %d", m.type, m.n, seat, fok, !fok);
        calculate_legal_moves(f, seat, g_lm);
        if (g_lm->n < MAX_LEGAL_MOVES) {
            bool fin = f_menu_index(g_lm, &m) >= 0;
            CHECK(fin == (gd_in_menu(g, seat, &m) != 0), "menu membership type %d: foolish %d", m.type, fin);
        }
    }
    return 1;
}


// THE CYCLE-SEEKING POLICY, which steers play into the repetition draw (uniform
// play almost never reaches it): the defender transfers one card when it can and
// takes otherwise, never covers; an attacker leads one card of a rank the
// defender also holds (so it can come back), and otherwise says good. Cards then
// rotate between the hands with no discard, and positions recur. Every choice
// is still a draw from the one RNG, and still applied to both engines.
static void cycle_seek_pick(const gd_game *g, long n0, long n1, uint8_t *seat, gd_move *m) {
    uint8_t s = (g->nb > 0 && (g->defender ? n1 : n0) > 0) ? g->defender : (n0 > 0 ? 0 : 1);
    uint32_t cnt = s ? (uint32_t)n1 : (uint32_t)n0, i, npass = 0, nlead = 0, nany = 0;
    gd_move c;
    int have_pickup = 0, have_good = 0;
    for (i = 0; i < cnt; i++) {
        gd_menu_move(g, s, i, &c);
        if (c.type == GD_PASS && c.n == 1) npass++;
        if (c.type == GD_PICKUP) have_pickup = 1;
        if (c.type == GD_GOOD) have_good = 1;
        if (c.type == GD_ATTACK && c.n == 1) {
            nany++;
            for (uint8_t q = 0; q < 4; q++)
                if (GD_HAS(g->hand[s ^ 1], q * 9 + gd_rank_of[c.cards[0]])) { nlead++; break; }
        }
    }
    uint8_t want; uint32_t k;
    if (npass) { want = GD_PASS; k = gd_rng_below(npass); }
    else if (have_pickup) { want = GD_PICKUP; k = 0; }
    else if (g->nb > 0 && have_good) { want = GD_GOOD; k = 0; }
    else if (nlead) { want = 0xF0; k = gd_rng_below(nlead); }
    else if (nany) { want = GD_ATTACK; k = gd_rng_below(nany); }
    else return;   // keep the uniform pick
    for (i = 0; i < cnt; i++) {
        gd_menu_move(g, s, i, &c);
        int hit = 0;
        if (want == GD_PASS) hit = c.type == GD_PASS && c.n == 1;
        else if (want == GD_PICKUP || want == GD_GOOD) hit = c.type == want;
        else if (c.type == GD_ATTACK && c.n == 1) {
            if (want == GD_ATTACK) hit = 1;
            else for (uint8_t q = 0; q < 4; q++) if (GD_HAS(g->hand[s ^ 1], q * 9 + gd_rank_of[c.cards[0]])) { hit = 1; break; }
        }
        if (hit && k-- == 0) { *m = c; *seat = s; return; }
    }
}


// ---- New York's arithmetic against the doubles it replaces --------------------------
// The trump threshold against `r / 65536.0 < p` for every draw and every board
// count, and the soft double against real double products.
static double hw_p(int flipped, int d, int t) {      // foolish trump_attack_probability
    if (flipped) return 0.02;
    if (t < 1) t = 1;
    double ratio = (double)d / (double)t;
    if (ratio > 1.0) ratio = 1.0;
    if (ratio < 0.0) ratio = 0.0;
    double p = 0.65 + 0.35 * ratio;
    if (p > 0.95) p = 0.95;
    if (p < 0.5) p = 0.5;
    return p;
}

static int hw_unit_checks(void) {
    unsigned long n = 0;
    for (int fl = 0; fl < 2; fl++)
        for (int t = 1; t <= 36; t++)
            for (int d = 0; d <= t; d++) {
                const double p = hw_p(fl, d, t);
                const uint16_t th = gd_trump_threshold((uint8_t)fl, (uint8_t)d, (uint8_t)t);
                for (uint32_t r = 0; r < 65536; r++, n++)
                    if ((r / 65536.0 < p) != (r < th)) {
                        printf("FAIL threshold flipped %d d %d t %d r %u: double %d, threshold %u\n", fl, d, t, r, r / 65536.0 < p, th);
                        return 0;
                    }
            }
    gd_rng_seed(4242);
    static const uint16_t sc[18] = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013 };
    unsigned long prods = 0;
    for (int rep = 0; rep < 200000; rep++) {
        const int len = 1 + (int)gd_rng_below(18);
        gd_dbl a;
        double x = 1.0;
        gd_dbl_one(&a);
        for (int i = 0; i < len; i++) {
            const uint16_t f = sc[gd_rng_below(18)];
            gd_dbl_mul(&a, f);
            x *= (double)f;
            uint64_t m = 0;
            for (int b = 7; b >= 0; b--) m = m << 8 | a.m[b];
            const double got = a.big ? ldexp((double)m, a.e) : (double)a.x;
            if (got != x) {
                printf("FAIL soft double after %d factors: %.17g vs %.17g\n", i + 1, got, x);
                return 0;
            }
            prods++;
        }
    }
    // and the order of any two, across the 32-bit and the 53-bit forms
    for (int rep = 0; rep < 200000; rep++) {
        gd_dbl a, b;
        double x = 1.0, y = 1.0;
        gd_dbl_one(&a); gd_dbl_one(&b);
        const int la = 1 + (int)gd_rng_below(8), lb = 1 + (int)gd_rng_below(8);
        for (int i = 0; i < la; i++) { const uint16_t f = sc[gd_rng_below(18)]; gd_dbl_mul(&a, f); x *= f; }
        for (int i = 0; i < lb; i++) { const uint16_t f = sc[gd_rng_below(18)]; gd_dbl_mul(&b, f); y *= f; }
        const int want = x < y ? -1 : x > y ? 1 : 0;
        if (gd_dbl_cmp(&a, &b) != want) { printf("FAIL soft double order: %.17g vs %.17g\n", x, y); return 0; }
    }
    printf("New York arithmetic: %lu threshold cases and %lu double products agree exactly\n", n, prods);
    return 1;
}

// ---- one game --------------------------------------------------------------------------
#define MOVE_CAP 20000

static int play_game(unsigned long seed, Game *f) {
    gd_game g;
    gd_rep rep;
    uint8_t deck[36];
    Card fdeck[36];
    g_seed_now = seed;
    g_move_now = 0;
    gd_rng_seed((uint16_t)seed);
    for (int i = 0; i < 36; i++) deck[i] = (uint8_t)i;
    for (int i = 35; i > 0; i--) { uint8_t j = (uint8_t)gd_rng_below((uint32_t)i + 1u), t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    uint8_t fallback = (uint8_t)gd_rng_below(2);
    gd_deal(&g, &rep, deck, fallback);

    memset(f, 0, sizeof *f);
    f->num_players = 2;
    for (int i = 0; i < 36; i++) fdeck[i] = f_of(deck[i]);
    game_force_first_attacker(fallback);
    start_game_with_deck(f, fdeck, 36);
    game_force_first_attacker(-1);
    sort_foolish_hands(f);
    if (!same_state(&g, f)) return 0;

    for (;;) {
        long n0, n1;
        if ((n0 = same_menu(&g, f, 0)) < 0) return 0;
        if ((n1 = same_menu(&g, f, 1)) < 0) return 0;
        if (!same_lite_menu(&g, f, 0) || !same_lite_menu(&g, f, 1)) return 0;
        if (g.status != GD_PLAYING) break;
        if (!probe_gestures(&g, f, 0) || !probe_gestures(&g, f, 1)) return 0;
        if (!probe_legal(&g, f)) return 0;
        {
            gd_move hm;
            if (!hw_compare(&g, f, 0, 0, &hm) || !hw_compare(&g, f, 1, 0, &hm)) return 0;
        }
        if (n0 + n1 == 0) { CHECK(0, "nobody can move in a live game"); }
        if (g_move_now >= MOVE_CAP) { st_cap++; break; }
        uint32_t pick = gd_rng_below((uint32_t)(n0 + n1));
        uint8_t seat = pick < (uint32_t)n0 ? 0 : 1;
        gd_move m;
        gd_menu_move(&g, seat, seat ? pick - (uint32_t)n0 : pick, &m);
        if (g_cycle_seek == 1 && gd_rng_below(4) != 0) cycle_seek_pick(&g, n0, n1, &seat, &m);
        if (g_cycle_seek == 2 && seat == 1 && !hw_compare(&g, f, 1, 1, &m)) return 0;
        calculate_legal_moves(f, seat, g_lm);
        int fi = f_menu_index(g_lm, &m);
        if (g_lm->n < MAX_LEGAL_MOVES) CHECK(fi >= 0, "picked move not on foolish's menu");
        bool ok = fi >= 0 ? legal_move_apply(f, seat, &g_lm->moves[fi]) : f_apply(f, seat, &m);
        CHECK(ok, "foolish refused the move (reject %d)", engine_last_reject);
        game_settle_status(f);
        CHECK(gd_apply(&g, &rep, seat, &m), "gd refused its own menu move");
        sort_foolish_hands(f);
        g_move_now++;
        st_moves++;
        st_hist_types[m.type]++;
        if (!same_state(&g, f)) return 0;
    }
    if (getenv("DT_VERBOSE")) fprintf(stderr, "seed %lu mode %d: %lu moves result %d\n", seed, g_cycle_seek, g_move_now, gd_result(&g));
    st_res[gd_result(&g)]++;
    st_games++;
    return 1;
}

int main(int argc, char **argv) {
    unsigned long games = argc > 1 ? strtoul(argv[1], 0, 10) : 2000;
    unsigned long first = argc > 2 ? strtoul(argv[2], 0, 10) : 1;
    Game *f = calloc(1, sizeof(Game));
    g_scratch = calloc(1, sizeof(Game));
    g_lm = malloc(sizeof(LegalMoves));
    g_ka = malloc(sizeof(Key) * MAX_LEGAL_MOVES);
    g_kb = malloc(sizeof(Key) * MAX_LEGAL_MOVES);
    g_wire = malloc(WIRE_CAP);
    if (!f || !g_scratch || !g_lm || !g_ka || !g_kb || !g_wire) return 2;
    // Uniform random play; the same number of games again steered toward the
    // repetition draw (which uniform play almost never reaches); and again
    // with New York playing seat 1.
    if (!hw_unit_checks()) return 1;
    for (g_cycle_seek = 0; g_cycle_seek < 3; g_cycle_seek++)
        for (unsigned long s = first; s < first + games; s++) {
            if (!play_game(s, f)) {
                printf("FAIL after %lu games, %lu moves (%s play)\n", st_games, st_moves,
                       g_cycle_seek == 2 ? "New York on seat 1" : g_cycle_seek ? "cycle-seeking" : "uniform");
                return 1;
            }
        }
    printf("difftest: %lu games, %lu moves, %lu menus compared, %lu handler probes, %lu gesture probes\n",
           st_games, st_moves, st_menus, st_probes, st_gest);
    printf("  moves: attack %lu cover %lu pass %lu pickup %lu good %lu\n",
           st_hist_types[0], st_hist_types[1], st_hist_types[2], st_hist_types[3], st_hist_types[4]);
    printf("  results: seat0 fool %lu, seat1 fool %lu, repetition draw %lu; move cap hit %lu\n",
           st_res[GD_RESULT_FOOL0], st_res[GD_RESULT_FOOL1], st_res[GD_RESULT_DRAW], st_cap);
    printf("  largest menu %u, most uncovered attacks %u, largest hand %u, foolish menus truncated %lu\n",
           st_max_menu, st_max_unc, st_max_hand, st_truncated);
    printf("  lite menus compared %lu\n", st_lite);
    printf("  New York vs handwritten_prod: %lu decisions compared, %lu agree (attack %lu cover %lu pass %lu pickup %lu good %lu), %lu draws\n",
           hw_compared, hw_agree, hw_by_type[0], hw_by_type[1], hw_by_type[2], hw_by_type[3], hw_by_type[4], hw_draws_total);
    printf("  New York's cover search: %lu searches, %.1f nodes on average, %lu at most\n", hw_covers, hw_covers ? (double)hw_nodes_sum / hw_covers : 0.0, hw_nodes_max);
    printf("PASS\n");
    return 0;
}

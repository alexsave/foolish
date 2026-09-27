/* The rules: legality (7.1), apply and effects (7.2), the deck (7.3), the
 * call-out windows (7.7) and the lobby, two-player and exhaustion edges
 * (7.8) of pickemup/docs/RULES_AND_KERNEL.md. Each test names its section;
 * tests/MUTATIONS.md records the mutation that turned each one red.
 *
 *     make -C pickemup/c run
 */
#include "pk_check.h"
#include "../src/pk_internal.h"
#include "../src/pk_lobby.h"

/* 7.3.2: the deck order for seed 00 01 02 .. 1f, format 1. Any change to the
 * card ids, the shuffle loop or the key schedule changes this, and every
 * game ever sent would deal differently: a new format number, never an edit
 * to this vector (RULES_AND_KERNEL.md 4.7). */
#define GOLDEN_SHUFFLE \
    "215b593952531b24064f14334d41640049304829051643660f672c153e262b020b3b1109381" \
    "71f2d03560e63612708313c4c5154401e25070c5d3d23460a195e581d445032341842352f37" \
    "3f0d5c2e226062121c551310282a1a57454765044b5a4a4e3a365f2001"

/* ---- 7.1 legality -------------------------------------------------------------- */

static void t_matching(void)
{
    TEST("7.1.1 matching");
    static const struct { uint8_t top; int live; uint8_t card; int ok; const char *why; } rows[] = {
        { 0 * 24 + 8, 0, 0 * 24 + 12, 1, "suit match (5 of circles, 7 of circles)" },
        { 0 * 24 + 8, 0, 1 * 24 + 8,  1, "number rank match (5 on 5)" },
        { 0 * 24 + 8, 0, 1 * 24 + 10, 0, "neither" },
        { 0 * 24 + 18, 0, 2 * 24 + 18, 1, "skip on skip" },
        { 1 * 24 + 20, 1, 3 * 24 + 21, 1, "reverse on reverse" },
        { 2 * 24 + 22, 2, 0 * 24 + 23, 1, "+2 on +2" },
        { 0 * 24 + 18, 0, 1 * 24 + 20, 0, "reverse on skip of another suit" },
        { 0 * 24 + 8, 0, 96,  1, "wild on a number" },
        { 0 * 24 + 8, 0, 101, 1, "wild +4 on a number" },
        { 97, 2, 98,  1, "wild on wild" },
        { 97, 2, 2 * 24 + 4, 1, "number on a wild's chosen suit" },
        { 97, 2, 0 * 24 + 4, 0, "number off a wild's chosen suit" },
        { 102, 3, 3 * 24 + 17, 1, "9 on a +4's chosen suit" },
        { 102, 3, 1 * 24 + 17, 0, "9 off a +4's chosen suit" },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        PkGame g;
        table(&g, 2, rows[i].top, rows[i].live);
        give(&g, 0, rows[i].card);
        give(&g, 0, num(3, 1, 0) == rows[i].card ? num(3, 2, 0) : num(3, 1, 0));
        CHECK(pk_can_play(&g, 0, 0) == rows[i].ok, "row %u: %s", i, rows[i].why);
    }
}

static void t_pass(void)
{
    TEST("7.1.2 pass");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, num(1, 6, 0));
    give(&g, 0, num(2, 7, 0));
    CHECK(!pk_is_legal(&g, 0, PASS), "pass before a draw, deck available");
    PkGame d = g;
    CHECK(pk_apply(&d, 0, DRAW), "draw");
    CHECK(pk_is_legal(&d, 0, PASS), "pass after one draw");

    /* nothing drawable: the whole deck in seat 1's hand, the stack its top */
    PkGame e = g;
    while (e.deck_n) give(&e, 1, e.deck[e.deck_n - 1]);
    CHECK(!pk_can_draw_any(&e), "nothing drawable");
    CHECK(pk_is_legal(&e, 0, PASS), "pass with nothing drawable and nothing playable");
    PkGame f;
    table(&f, 2, num(0, 5, 0), 0);
    give(&f, 0, num(1, 6, 0));
    give(&f, 0, num(0, 7, 0));
    while (f.deck_n) give(&f, 1, f.deck[f.deck_n - 1]);
    CHECK(!pk_is_legal(&f, 0, PASS), "pass refused: nothing drawable but a playable card");
}

static void t_draw(void)
{
    TEST("7.1.3 draw");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, num(0, 6, 0));          /* playable */
    give(&g, 0, num(1, 1, 0));
    CHECK(pk_is_legal(&g, 0, DRAW), "draw with a deck, holding a playable card (D6)");
    PkGame e = g;
    while (e.deck_n > 1) give(&e, 1, e.deck[e.deck_n - 1]);
    uint8_t last = e.deck[0];
    e.deck_n = 0;
    e.stack[0] = last;
    e.stack[1] = num(0, 5, 0);
    e.stack_n = 2;
    CHECK(pk_is_legal(&e, 0, DRAW), "draw with an empty deck and a stack of 2");
    e.stack[0] = num(0, 5, 0);
    e.stack_n = 1;
    CHECK(!pk_is_legal(&e, 0, DRAW), "draw refused with an empty deck and a stack of 1");
}

static void t_call_leaks_nothing(void)
{
    TEST("7.1.4 call-out leaks nothing");
    int positions = 0;
    for (int k = 0; positions < 10000; k++) {
        PkGame g;
        uint8_t seed[32];
        seed_of(seed, 5000u + (uint32_t)k);
        pk_new(&g, seed, 2 + k % 7);
        int steps = (int)rnd(120);
        for (int i = 0; i < steps && !g.over; i++) bot_step(&g);
        for (int rep = 0; rep < 10; rep++, positions++) {
            int s = (int)rnd(g.n);
            PkAct a[PK_MAX_SEATS * 2 + PK_HAND_CAP * 4 + 4], b[PK_MAX_SEATS * 2 + PK_HAND_CAP * 4 + 4];
            int na = pk_legal(&g, s, a, (int)(sizeof a / sizeof a[0]));
            /* reshape every OTHER seat's hand and every exposed bit */
            PkGame h = g;
            h.exposed = (uint8_t)rnd(256);
            h.b_exposed_at_open = (uint8_t)rnd(256);
            for (int t = 0; t < h.n; t++) {
                if (t == s) continue;
                int want = 1 + (int)rnd(12);
                while (h.hand_n[t] > want && h.deck_n < PK_DECK)
                    h.deck[h.deck_n++] = h.hand[t][--h.hand_n[t]];
                while (h.hand_n[t] < want && h.deck_n > 0)
                    h.hand[t][h.hand_n[t]++] = h.deck[--h.deck_n];
            }
            int nb = pk_legal(&h, s, b, (int)(sizeof b / sizeof b[0]));
            uint8_t ca = 0, cb = 0;
            for (int i = 0; i < na; i++) if (a[i].kind == PK_A_CALL_OUT) ca |= (uint8_t)(1u << a[i].a);
            for (int i = 0; i < nb; i++) if (b[i].kind == PK_A_CALL_OUT) cb |= (uint8_t)(1u << b[i].a);
            CHECK(ca == cb, "seat %d: calls %02x vs %02x after reshaping other hands", s, ca, cb);
        }
    }
}

static void t_say_it(void)
{
    TEST("7.1.5 say-it");
    PkGame g;
    table(&g, 3, num(0, 5, 0), 0);
    give(&g, 0, num(0, 6, 0));
    give(&g, 0, num(1, 1, 0));
    CHECK(pk_apply(&g, 0, PLAY(0)), "play to one card");
    CHECK(g.exposed == 1, "exposed");
    CHECK(!pk_is_legal(&g, 0, SAY), "say refused in the exposing bubble (D3)");
    CHECK(pk_seal(&g), "seal");
    CHECK(pk_is_legal(&g, 0, SAY), "say legal in the sender's next bubble, out of turn");
    CHECK(pk_apply(&g, 0, SAY), "say");
    CHECK(!pk_is_legal(&g, 0, SAY), "say refused once said");
    CHECK(pk_seal(&g) && g.said == 1 && g.exposed == 0, "stamped, not exposed");

    /* exposed, and it is their own turn: a draw takes them off one card */
    PkGame h;
    table(&h, 2, num(0, 5, 0), 0);
    give(&h, 0, num(1, 1, 0));
    h.exposed = 1;
    CHECK(pk_apply(&h, 0, DRAW), "draw");
    CHECK(!pk_is_legal(&h, 0, SAY), "say refused after drawing");
}

static void t_menu_order(void)
{
    TEST("7.1.6 menu order is the format");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, num(1, 5, 0));   /* 0: rank match           */
    give(&g, 0, wild_(0));       /* 1: wild, four suits      */
    give(&g, 0, num(2, 1, 0));   /* 2: no                    */
    give(&g, 0, num(0, 9, 0));   /* 3: suit match            */
    give(&g, 0, wild4_(0));      /* 4: wild +4, four suits   */
    static const PkAct want[] = {
        { PK_A_DRAW, 0, 0, 0 },
        { PK_A_PLAY, 0, PK_NO_SUIT, 0 },
        { PK_A_PLAY, 1, 0, 0 }, { PK_A_PLAY, 1, 1, 0 }, { PK_A_PLAY, 1, 2, 0 }, { PK_A_PLAY, 1, 3, 0 },
        { PK_A_PLAY, 3, PK_NO_SUIT, 0 },
        { PK_A_PLAY, 4, 0, 0 }, { PK_A_PLAY, 4, 1, 0 }, { PK_A_PLAY, 4, 2, 0 }, { PK_A_PLAY, 4, 3, 0 },
    };
    PkAct got[64];
    int n = pk_legal_turn(&g, 0, got, 64);
    CHECK(n == (int)(sizeof want / sizeof want[0]), "menu length %d", n);
    for (int i = 0; i < n && i < (int)(sizeof want / sizeof want[0]); i++)
        CHECK(got[i].kind == want[i].kind && got[i].a == want[i].a && got[i].b == want[i].b,
              "entry %d: %d/%d/%d", i, got[i].kind, got[i].a, got[i].b);
    /* after a draw, PASS closes the list */
    PkGame d = g;
    pk_apply(&d, 0, DRAW);
    n = pk_legal_turn(&d, 0, got, 64);
    CHECK(n > 0 && got[0].kind == PK_A_DRAW && got[n - 1].kind == PK_A_PASS, "DRAW first, PASS last");
    /* bubble-level entries follow the turn actions */
    n = pk_legal(&d, 0, got, 64);
    CHECK(got[n - 1].kind == PK_A_CALL_OUT && got[n - 1].a == 1, "CALL_OUT after the turn actions");
}

static void t_last_wild(void)
{
    TEST("7.1.7 last-card wild has no suit");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, wild_(1));
    PkAct got[16];
    int n = pk_legal_turn(&g, 0, got, 16);
    int plays = 0;
    for (int i = 0; i < n; i++) if (got[i].kind == PK_A_PLAY) plays++;
    CHECK(plays == 1, "one menu entry for a last-card wild, got %d", plays);
    CHECK(pk_is_legal(&g, 0, PLAYW(0, PK_NO_SUIT)) && !pk_is_legal(&g, 0, PLAYW(0, 2)),
          "it is the suitless entry");
}

/* ---- 7.2 apply and effects ------------------------------------------------------ */

static void t_action_cards(void)
{
    TEST("7.2.1 every action card at 2, 3 and 8 players");
    enum { NUM = 0, SKIP, REV, P2, W, W4 };
    static const struct { int n, seat, dir, card, turn, ndir, victim, drew; } rows[] = {
        { 2, 0, +1, SKIP, 0, +1, 1, 0 },
        { 2, 0, +1, REV,  0, +1, 1, 0 },
        { 2, 1, +1, REV,  1, +1, 0, 0 },
        { 2, 0, +1, P2,   0, +1, 1, 2 },
        { 2, 0, +1, W4,   0, +1, 1, 4 },
        { 2, 1, +1, NUM,  0, +1, 0, 0 },
        { 3, 0, +1, SKIP, 2, +1, 1, 0 },
        { 3, 0, +1, REV,  2, -1, 1, 0 },
        { 3, 1, -1, REV,  2, +1, 0, 0 },
        { 3, 0, +1, P2,   2, +1, 1, 2 },
        { 3, 2, +1, NUM,  0, +1, 0, 0 },
        { 3, 2, -1, W,    1, -1, 1, 0 },
        { 8, 7, +1, SKIP, 1, +1, 0, 0 },
        { 8, 0, -1, SKIP, 6, -1, 7, 0 },
        { 8, 3, +1, REV,  2, -1, 2, 0 },
        { 8, 6, +1, W4,   0, +1, 7, 4 },
        { 8, 1, -1, P2,   7, -1, 0, 2 },
        { 8, 4, -1, NUM,  3, -1, 3, 0 },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        PkGame g;
        table(&g, rows[i].n, num(1, 5, 0), 1);
        uint8_t c = rows[i].card == SKIP ? skip_(1, 0) : rows[i].card == REV ? rev_(1, 0)
                  : rows[i].card == P2 ? plus2_(1, 0) : rows[i].card == W ? wild_(0)
                  : rows[i].card == W4 ? wild4_(0) : num(1, 3, 0);
        int s = rows[i].seat;
        give(&g, s, c);
        give(&g, s, num(2, 1, 0));
        give(&g, s, num(2, 2, 0));
        g.turn = (uint8_t)s;
        g.dir = (int8_t)rows[i].dir;
        int before = g.hand_n[rows[i].victim];
        int wild = rows[i].card == W || rows[i].card == W4;
        CHECK(pk_apply(&g, s, wild ? PLAYW(0, 3) : PLAY(0)), "row %u plays", i);
        CHECK(g.turn == rows[i].turn, "row %u: turn %d, want %d", i, g.turn, rows[i].turn);
        CHECK(g.dir == rows[i].ndir, "row %u: dir %d, want %d", i, g.dir, rows[i].ndir);
        CHECK(g.hand_n[rows[i].victim] - before == rows[i].drew, "row %u: victim drew %d, want %d",
              i, g.hand_n[rows[i].victim] - before, rows[i].drew);
        CHECK(conserved(&g), "row %u conserved", i);
    }
}

static int count_kind(const PkEvent *e, int n, int kind)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += e[i].kind == kind;
    return c;
}

typedef struct { PkEvent ev[4096]; int n; } Tape;
static void tape_fn(const PkEvent *e, void *ctx)
{
    Tape *t = (Tape *)ctx;
    if (t->n < 4096) t->ev[t->n++] = *e;
}
static PkSink tape_sink(Tape *t)
{
    PkSink k;
    memset(&k, 0, sizeof k);
    k.fn = tape_fn;
    k.ctx = t;
    k.viewer = PK_VIEW_ALL;
    k.from = -1;
    k.to = 0xFFFF;
    t->n = 0;
    return k;
}

static void t_penalty_short(void)
{
    TEST("7.2.2 penalty short supply");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, wild4_(0));
    give(&g, 0, num(3, 1, 0));
    give(&g, 0, num(3, 2, 0));
    /* stack [a, top]; after the +4 the reshuffle can supply exactly 2 */
    uint8_t a = num(2, 9, 1);
    for (int i = 0; i < g.deck_n; i++)
        if (g.deck[i] == a) { g.deck[i] = g.deck[--g.deck_n]; break; }
    g.stack[0] = a;
    g.stack[1] = num(0, 5, 0);
    g.stack_n = 2;
    while (g.deck_n) give(&g, 1, g.deck[g.deck_n - 1]);
    int before = g.hand_n[1];
    Tape t;
    PkSink k = tape_sink(&t);
    CHECK(pk__apply(&g, 0, PLAYW(0, 1), &k), "play the +4");
    CHECK(g.hand_n[1] - before == 2, "dealt %d, want 2", g.hand_n[1] - before);
    int shorted = -1;
    for (int i = 0; i < t.n; i++) if (t.ev[i].kind == PK_EV_PENALTY_SHORT) shorted = t.ev[i].n;
    CHECK(shorted == 2, "PENALTY_SHORT n=%d, want 2", shorted);
    CHECK(count_kind(t.ev, t.n, PK_EV_PENALTY_DRAW) == 2, "two PENALTY_DRAW events");
    CHECK(conserved(&g), "conserved");
}

static void t_going_out(void)
{
    TEST("7.2.3 going out ends it at once");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, plus2_(0, 0));
    give(&g, 1, num(1, 1, 0));
    give(&g, 1, num(1, 2, 0));
    CHECK(pk_apply(&g, 0, CALL(1)), "a catch in the winning bubble");
    CHECK(pk_apply(&g, 0, PLAY(0)), "last card +2");
    CHECK(g.over == PK_OVER_OUT && g.winner == 0, "over, seat 0 wins");
    CHECK(g.turn == PK_SEAT_NONE, "no turn moves");
    CHECK(g.hand_n[1] == 2, "the victim drew nothing: %d", g.hand_n[1]);
    CHECK(pk_seal(&g), "seal");
    CHECK(g.hand_n[0] == 0 && g.hand_n[1] == 2, "the wrong catch deals nothing either");
}

static void t_hand_order(void)
{
    TEST("7.2.4 hand order");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    uint8_t h[4] = { num(1, 1, 0), num(0, 7, 0), num(2, 2, 0), num(3, 3, 0) };
    for (int i = 0; i < 4; i++) give(&g, 0, h[i]);
    uint8_t x = num(3, 9, 1);
    deck_top(&g, x);
    CHECK(pk_apply(&g, 0, DRAW), "draw");
    CHECK(g.hand_n[0] == 5 && g.hand[0][4] == x, "the draw goes on the right");
    CHECK(pk_apply(&g, 0, PLAY(1)), "play position 1");
    uint8_t want[4] = { h[0], h[2], h[3], x };
    CHECK(g.hand_n[0] == 4 && !memcmp(g.hand[0], want, 4), "gap closed, order kept: %d %d %d %d",
          g.hand[0][0], g.hand[0][1], g.hand[0][2], g.hand[0][3]);
}

/* ---- 7.3 the deck ---------------------------------------------------------------- */

static void t_round_robin(void)
{
    TEST("7.3.1 round-robin deal");
    uint8_t seed[32], a[PK_DECK];
    seed_of(seed, 42);
    for (int c = 0; c < PK_DECK; c++) a[c] = (uint8_t)c;
    pk_shuffle(a, PK_DECK, seed, 0);
    PkGame g;
    CHECK(pk_new(&g, seed, 3), "deal");
    static const int to[7] = { 1, 2, 0, 1, 2, 0, 1 };
    int got[3] = { 0, 0, 0 };
    for (int i = 0; i < 7; i++) {
        int s = to[i];
        CHECK(g.hand[s][got[s]] == a[PK_DECK - 1 - i], "card %d went to seat %d", i + 1, s);
        got[s]++;
    }
    CHECK(g.hand_n[0] == 7 && g.hand_n[1] == 7 && g.hand_n[2] == 7, "seven each");
    CHECK(g.turn == 1 && g.dir == 1, "seat 1 first, clockwise");
    CHECK(conserved(&g), "conserved");
}

static void t_shuffle_golden(void)
{
    TEST("7.3.2 shuffle golden");
    uint8_t seed[32], a[PK_DECK];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    for (int c = 0; c < PK_DECK; c++) a[c] = (uint8_t)c;
    pk_shuffle(a, PK_DECK, seed, 0);
    /* committed: seed 00 01 .. 1f, format 1 */
    static const char golden[] = GOLDEN_SHUFFLE;
    char hex[2 * PK_DECK + 1];
    for (int i = 0; i < PK_DECK; i++) {
        static const char d[] = "0123456789abcdef";
        hex[2 * i] = d[a[i] >> 4];
        hex[2 * i + 1] = d[a[i] & 15];
    }
    hex[2 * PK_DECK] = 0;
    CHECK(!strcmp(hex, golden), "shuffle vector\n  got  %s\n  want %s", hex, golden);
    int seen[PK_DECK] = { 0 };
    for (int i = 0; i < PK_DECK; i++) seen[a[i]]++;
    int perm = 1;
    for (int c = 0; c < PK_DECK; c++) perm &= seen[c] == 1;
    CHECK(perm, "a permutation of 0..103");
}

/* The cards the deal buried, in the order it buried them. */
static int buries(const PkGame *g, uint8_t *out)
{
    PkEvent ev[256];
    int n = pk_plan(g, PK_VIEW_ALL, -1, 0, ev, 256), b = 0;
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) if (ev[i].kind == PK_EV_BURY) out[b++] = ev[i].card;
    return b;
}

static void t_start_card(void)
{
    TEST("7.3.3 start card");
    static const int wants[3] = { 1, 2, 5 };
    for (int w = 0; w < 3; w++) {
        int found = 0;
        for (uint32_t k = 0; k < 100000 && !found; k++) {
            uint8_t seed[32], bur[PK_DECK];
            seed_of(seed, 70000u + k);
            PkGame g;
            if (!pk_new(&g, seed, 4)) { CHECK(0, "pk_new refused seed %u", k); break; }
            int b = buries(&g, bur);
            if (b != wants[w]) continue;
            found = 1;
            uint8_t top = g.stack[g.stack_n - 1];
            CHECK(g.stack_n == 1 && pk_is_number(top), "%d buries: starts on a number", b);
            CHECK(g.live_suit == pk_suit(top), "%d buries: live suit is the start card's", b);
            int all = 1;
            for (int i = 0; i < b; i++) all &= !pk_is_number(bur[i]) && g.deck[i] == bur[b - 1 - i];
            CHECK(all, "%d buries: the buried cards are the bottom of the deck, first deepest", b);
            CHECK(g.deck_n == PK_DECK - 4 * PK_HAND_SIZE - 1, "%d buries: they stay in the deck", b);
            CHECK(conserved(&g), "%d buries: conserved", b);
        }
        CHECK(found, "a seed with %d buries", wants[w]);
    }
}

static void t_reshuffle(void)
{
    TEST("7.3.4 reshuffle determinism");
    for (int r = 1; r <= 3; r++) {
        PkGame g;
        empty_deck(&g, 20, num(0, 5, 0), 0);
        g.reshuffles = (uint16_t)(r - 1);       /* from scratch: nothing walked */
        uint8_t want[PK_DECK];
        memcpy(want, g.stack, 20);
        pk_shuffle(want, 20, g.seed, (uint64_t)r << 32);
        CHECK(pk_apply(&g, 0, DRAW), "r=%d: draw through a reshuffle", r);
        CHECK(g.reshuffles == r, "r=%d: counted", r);
        CHECK(g.hand[0][2] == want[19], "r=%d: drew the new top", r);
        CHECK(g.deck_n == 19 && !memcmp(g.deck, want, 19), "r=%d: the deck is shuffle(block r<<32)", r);
        CHECK(conserved(&g), "r=%d: conserved", r);
    }
    /* two games from one seed and one history agree through three reshuffles */
    uint8_t seed[32];
    seed_of(seed, 9);
    PkGame a, b;
    pk_new(&a, seed, 2);
    pk_new(&b, seed, 2);
    uint64_t save = RS;
    int guard = 0;
    while (a.reshuffles < 3 && !a.over && guard++ < 100000) bot_step(&a);
    uint64_t end = RS;
    RS = save;
    guard = 0;
    while (b.reshuffles < 3 && !b.over && guard++ < 100000) bot_step(&b);
    CHECK(RS == end && pk_hash(&a) == pk_hash(&b), "same seed, same history, same game");
}

static void t_no_overlap(void)
{
    TEST("7.3.5 no overlap");
    uint8_t seed[32], a[PK_DECK];
    seed_of(seed, 3);
    for (int c = 0; c < PK_DECK; c++) a[c] = (uint8_t)c;
    uint64_t end0 = pk_shuffle(a, PK_DECK, seed, 0);
    uint64_t start1 = pk_reshuffle_block(1);
    CHECK(end0 > 0 && end0 <= start1, "initial shuffle reads [0, %llu), reshuffle 1 starts at %llu",
          (unsigned long long)end0, (unsigned long long)start1);
    for (int r = 1; r < 4; r++) {
        uint64_t end = pk_shuffle(a, 103, seed, pk_reshuffle_block(r));
        CHECK(end <= pk_reshuffle_block(r + 1), "reshuffle %d stays below reshuffle %d", r, r + 1);
    }
}

static void t_top_stays(void)
{
    TEST("7.3.6 top stays");
    PkGame g;
    uint8_t top = wild_(2);
    empty_deck(&g, 10, top, 3);
    CHECK(pk_apply(&g, 0, DRAW), "draw through a reshuffle");
    CHECK(g.stack_n == 1 && g.stack[0] == top && g.live_suit == 3, "the top and its live suit stay");
    int in_deck = 0;
    for (int i = 0; i < g.deck_n; i++) in_deck |= g.deck[i] == top;
    CHECK(!in_deck && g.deck_n == 9, "the top is not in the new deck (deck %d)", g.deck_n);
    CHECK(conserved(&g), "conserved");
}

/* ---- 7.7 call-out windows ---------------------------------------------------------- */

static void t_windows(void)
{
    TEST("7.7.1 call-out windows");
    PkGame g;

    exposed3(&g);
    CHECK(g.exposed == 1 && g.turn == 1, "seat 0 exposed, seat 1 to play");
    pk_apply(&g, 1, CALL(0)); pk_apply(&g, 1, PLAY(0));
    CHECK(pk_seal(&g) && g.hand_n[0] == 3 && g.hand_n[1] == 2, "next turn catches: hit, 2 cards");

    exposed3(&g);
    CHECK(pk_apply(&g, 0, SAY) && pk_seal(&g), "says it standalone");
    CHECK(!pk_is_legal(&g, 1, CALL(0)), "a catch on the LAST stamp is refused (D5c, D34)");

    exposed3(&g);
    pk_apply(&g, 1, PLAY(0));
    CHECK(pk_seal(&g) && g.exposed == 0, "next turn does not catch: window closed");
    pk_apply(&g, 2, CALL(0)); pk_apply(&g, 2, PLAY(0));
    CHECK(pk_seal(&g) && g.hand_n[0] == 1 && g.hand_n[2] == 3, "a later catch misses: catcher +1");

    exposed3(&g);
    pk_apply(&g, 2, CALL(0));
    CHECK(pk_seal(&g) && g.hand_n[0] == 3, "an out-of-turn catch before the next turn: hit");

    exposed3(&g);
    pk_apply(&g, 2, CALL(1));
    CHECK(pk_seal(&g) && g.hand_n[2] == 4, "an unrelated wrong catch: catcher +1");
    CHECK(g.exposed == 1, "the window is still open (D4)");
    pk_apply(&g, 1, CALL(0)); pk_apply(&g, 1, PLAY(0));
    CHECK(pk_seal(&g) && g.hand_n[0] == 3, "and the next turn's catch hits");

    /* two players, after a Skip: the exposed player goes again */
    PkGame h;
    table(&h, 2, num(0, 5, 0), 0);
    give(&h, 0, skip_(0, 0)); give(&h, 0, num(1, 1, 0));
    give(&h, 1, num(2, 2, 0)); give(&h, 1, num(2, 3, 0));
    pk_apply(&h, 0, PLAY(0));
    CHECK(pk_turn_ended(&h) && pk_seal(&h), "on one card the bubble ends although the turn is back");
    PkGame h2 = h;
    pk_apply(&h2, 1, CALL(0));
    CHECK(pk_seal(&h2) && h2.hand_n[0] == 3, "2p: the other player catches out of turn: hit");
    pk_apply(&h, 0, DRAW); pk_apply(&h, 0, PASS);
    CHECK(pk_seal(&h) && h.exposed == 0, "2p: the exposed player's own next turn closes it");
    pk_apply(&h, 1, CALL(0));
    CHECK(pk_seal(&h) && h.hand_n[1] == 3, "2p: a later catch misses");

    /* the exposed player draws: off one card, stamp gone */
    PkGame d;
    table(&d, 2, num(0, 5, 0), 0);
    give(&d, 0, num(1, 1, 0));
    d.exposed = 0; d.said = 1;
    pk_apply(&d, 0, DRAW);
    CHECK(d.said == 0 && d.exposed == 0, "a draw removes the LAST stamp");

    /* two exposures in a row: the second turn closes the first */
    exposed3(&g);
    g.hand_n[1] = 2;             /* seat 1 holds 7 of circles and a 2 */
    pk_apply(&g, 1, PLAY(0));
    CHECK(pk_seal(&g) && g.exposed == 2, "seat 1 exposed, seat 0 got away (exposed %02x)", g.exposed);
    PkGame m = g;
    pk_apply(&m, 2, CALL(0));
    CHECK(pk_seal(&m) && m.hand_n[2] == 4, "a catch on the first now misses");
    pk_apply(&g, 2, CALL(1));
    CHECK(pk_seal(&g) && g.hand_n[1] == 3, "a catch on the second hits");
}

static void t_judged_at_open(void)
{
    TEST("7.7.2 judged at open");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, num(0, 6, 0)); give(&g, 0, num(1, 1, 0));
    give(&g, 1, plus2_(0, 0)); give(&g, 1, num(2, 2, 0)); give(&g, 1, num(2, 3, 0));
    pk_apply(&g, 0, PLAY(0));
    pk_seal(&g);
    pk_apply(&g, 1, CALL(0));
    pk_apply(&g, 1, PLAY(0));               /* +2 on seat 0: they draw, then the catch */
    CHECK(g.hand_n[0] == 3, "the +2 landed");
    CHECK(pk_seal(&g), "seal");
    CHECK(g.hand_n[0] == 5 && g.hand_n[1] == 2, "still a hit: 1 + 2 + 2 = %d, catcher %d",
          g.hand_n[0], g.hand_n[1]);
}

static void t_penalty_at_end(void)
{
    TEST("7.7.3 penalty at end");
    PkGame a;
    exposed3(&a);
    PkGame b = a;
    pk_apply(&a, 1, CALL(0));
    pk_apply(&a, 1, DRAW); pk_apply(&a, 1, DRAW);
    pk_apply(&b, 1, DRAW); pk_apply(&b, 1, DRAW);
    CHECK(a.hand_n[1] == 5 && b.hand_n[1] == 5 && !memcmp(a.hand[1], b.hand[1], 5),
          "the catcher's own draws are the same cards with or without the catch");
    CHECK(a.hand_n[0] == 1, "nothing dealt before seal");
}

/* ---- 7.8 lobby, 2-player edges, exhaustion ------------------------------------------ */

static void t_lobby_verdicts(void)
{
    TEST("7.8.1 lobby verdicts exhaustive");
    for (int dm = 0; dm < 2; dm++) {
        int cap = dm ? 2 : 8;
        for (int n = 1; n <= cap; n++)
            for (int newest = -1; newest < n; newest++) {
                PkLobby l;
                pk_lobby_new(&l, dm, 100);
                for (int s = 1; s < n; s++) pk_lobby_join(&l, (uint16_t)(100 + s));
                l.newest = newest < 0 ? PK_SEAT_NONE : (uint8_t)newest;
                int starters = 0;
                for (int me = -1; me < n; me++) {
                    int o = pk_lobby_offered(&l, me);
                    CHECK(o >= PK_LOBBY_START && o <= PK_LOBBY_FULL, "dm %d n %d newest %d me %d: one control",
                          dm, n, newest, me);
                    if (me < 0) CHECK(o == (n < cap ? PK_LOBBY_JOIN : PK_LOBBY_FULL), "outsider: join or full");
                    else CHECK(o != PK_LOBBY_JOIN && o != PK_LOBBY_FULL, "a seated player is not offered a seat");
                    starters += o == PK_LOBBY_START;
                    if (me >= 0 && me == newest && n < cap)
                        CHECK(o != PK_LOBBY_START, "dm %d n %d: the newest joiner cannot start with room", dm, n);
                    if (me >= 0 && n == cap && n >= 2)
                        CHECK(o == PK_LOBBY_START, "dm %d: a full table: everyone may start", dm);
                }
                if (n >= 2) CHECK(starters >= 1, "dm %d n %d newest %d: somebody can start", dm, n, newest);
                CHECK(pk_lobby_can_join_and_start(&l) == (n + 1 == cap),
                      "dm %d n %d: join-and-start only on the filling join", dm, n);
            }
    }
    /* the DM: the joiner fills it and may start in the same bubble */
    PkLobby d;
    pk_lobby_new(&d, 1, 7);
    CHECK(pk_lobby_can_join_and_start(&d), "DM: join-and-start offered");
    int s = pk_lobby_join(&d, 8);
    CHECK(s == 1 && pk_lobby_offered(&d, 1) == PK_LOBBY_START, "DM joiner offered START");
    CHECK(pk_lobby_offered(&d, 0) == PK_LOBBY_START, "DM creator offered START");
    CHECK(pk_lobby_join(&d, 9) < 0, "DM full");
}

static void t_two_routes(void)
{
    TEST("7.8.2 two routes, one deal");
    uint8_t seed[32];
    seed_of(seed, 77);
    PkLobby a, b;
    PkGame ga, gb;
    pk_lobby_new(&a, 0, 1);
    pk_lobby_join(&a, 2);
    pk_lobby_leave(&a, 1);
    pk_lobby_join(&a, 3);
    CHECK(pk_lobby_start(&a, 0, seed, &ga), "join, leave, join, then the creator starts");
    pk_lobby_new(&b, 1, 1);
    pk_lobby_join(&b, 3);
    CHECK(pk_lobby_start(&b, 1, seed, &gb), "join-and-start");
    CHECK(ga.n == 2 && gb.n == 2, "two seats each");
    CHECK(!memcmp(ga.hand, gb.hand, sizeof ga.hand) && !memcmp(ga.deck, gb.deck, sizeof ga.deck),
          "identical hands and deck");
    CHECK(ga.starter == 0 && gb.starter == 1, "the starters differ and the deal does not care");
    PkLobby x, before;
    pk_lobby_new(&x, 0, 1);
    pk_lobby_join(&x, 2);
    pk_lobby_join(&x, 3);
    before = x;
    pk_lobby_leave(&x, 1);
    pk_lobby_join(&x, 4);
    PkEvent ev[8];
    int n = pk_plan_lobby(&before, &x, ev, 8);
    CHECK(n == 2 && ev[0].kind == PK_EV_LOBBY_LEAVE && ev[0].seat == 1
          && ev[1].kind == PK_EV_LOBBY_JOIN && ev[1].seat == 2, "a leave then a join, at their seats");
}

static void t_leave(void)
{
    TEST("7.8.3 leave compacts seats");
    PkLobby l;
    pk_lobby_new(&l, 0, 10);
    pk_lobby_join(&l, 11); pk_lobby_join(&l, 12); pk_lobby_join(&l, 13);
    CHECK(pk_lobby_leave(&l, 1), "seat 1 leaves");
    CHECK(l.n_seats == 3 && pk_lobby_seat_of(&l, 12) == 1 && pk_lobby_seat_of(&l, 13) == 2
          && pk_lobby_seat_of(&l, 11) < 0, "later rows moved down");
    CHECK(pk_lobby_leave(&l, 0) && pk_lobby_seat_of(&l, 12) == 0, "the new seat 0 is the dealer");
    CHECK(pk_lobby_offered(&l, 0) == PK_LOBBY_START, "and may start (newest is the leaver)");
    CHECK(pk_lobby_leave(&l, 0) && l.n_seats == 1, "down to one");
    CHECK(!pk_lobby_can_exit(&l, 0) && pk_lobby_offered(&l, 0) == PK_LOBBY_INVITE,
          "the last one cannot leave, and is offered INVITE");
}

static void t_two_players(void)
{
    TEST("7.8.4 2 players");
    static const int cards[3] = { PK_R_SKIP, PK_R_PLUS2, PK_R_WILD4 };
    for (int i = 0; i < 3; i++) {
        PkGame g;
        table(&g, 2, num(1, 5, 0), 1);
        uint8_t c = cards[i] == PK_R_SKIP ? skip_(1, 0) : cards[i] == PK_R_PLUS2 ? plus2_(1, 0) : wild4_(0);
        give(&g, 0, c); give(&g, 0, num(1, 1, 0)); give(&g, 0, num(1, 2, 0));
        CHECK(pk_apply(&g, 0, pk_is_wild(c) ? PLAYW(0, 1) : PLAY(0)), "card %d plays", i);
        CHECK(g.turn == 0 && !pk_turn_ended(&g), "card %d: the turn comes straight back", i);
        CHECK(pk_can_seal(&g) && pk_is_legal(&g, 0, PLAY(0)), "card %d: seal or continue", i);
        CHECK(pk_apply(&g, 0, PLAY(0)) && g.hist[g.hist_n - 2].kind == PK_A_CONTINUE,
              "card %d: continuing writes CONTINUE", i);
    }
    PkGame g;
    table(&g, 2, num(1, 5, 0), 1);
    give(&g, 0, skip_(1, 0)); give(&g, 0, num(2, 1, 0));
    pk_apply(&g, 0, PLAY(0));
    PkAct menu[8];
    CHECK(g.turn == 0 && pk_turn_ended(&g) && pk_legal_turn(&g, 0, menu, 8) == 0,
          "a play to one card ends the bubble even when the turn comes back (D7)");
    table(&g, 2, num(1, 5, 0), 1);
    give(&g, 0, rev_(1, 0)); give(&g, 0, num(2, 1, 0)); give(&g, 0, num(2, 2, 0));
    pk_apply(&g, 0, PLAY(0));
    CHECK(g.dir == 1 && g.turn == 0, "reverse is a skip, dir unchanged");
}

static void t_stuck(void)
{
    TEST("7.8.5 stuck table");
    for (int d = 0; d < 2; d++) {
        PkGame g;
        table(&g, 3, num(0, 5, 0), 0);
        give(&g, 0, num(1, 1, 0)); give(&g, 0, num(1, 2, 0)); give(&g, 0, num(1, 3, 0));
        give(&g, 1, num(2, 1, 0)); give(&g, 1, num(2, 2, 0));
        give(&g, 2, num(3, 1, 0)); give(&g, 2, num(3, 2, 0));
        g.deck_n = 0;               /* nothing to draw; the stack is its top */
        g.dir = (int8_t)(d ? -1 : 1);
        int passes = 0;
        for (int i = 0; i < 3 && !g.over; i++) {
            int s = g.turn;
            CHECK(pk_apply(&g, s, PASS), "bare pass by %d", s);
            CHECK(pk_seal(&g), "seal");
            passes++;
        }
        CHECK(g.over == PK_OVER_STUCK && passes == 3, "n bare passes end it");
        /* seats 1 and 2 tie on two; from seat 0 (next to move) clockwise it is 1, anticlockwise 2 */
        CHECK(g.winner == (d ? 2 : 1), "dir %d: winner %d", g.dir, g.winner);
    }
    /* a draw in between resets the count */
    PkGame g;
    empty_deck(&g, 1, num(0, 5, 0), 0);
    g.idle_passes = 1;
    CHECK(pk_apply(&g, 0, DRAW) && pk_apply(&g, 0, PASS), "draw, then pass");
    CHECK(g.idle_passes == 0, "a pass that drew is not bare (idle %d)", g.idle_passes);
}

static void t_undo(void)
{
    TEST("7.8.6 undo floor");
    int done = 0;
    for (uint32_t k = 0; k < 2000 && !done; k++) {
        uint8_t seed[32];
        seed_of(seed, 300u + k);
        PkGame g;
        pk_new(&g, seed, 3);
        PkGame before_draw = g;
        CHECK(pk_apply(&g, 1, DRAW), "draw");
        int p = -1;
        for (int i = 0; i < g.hand_n[1]; i++) if (pk_can_play(&g, 1, i) && !pk_is_wild(g.hand[1][i])) { p = i; break; }
        if (p < 0) continue;
        done = 1;
        PkGame before_play = g;
        CHECK(pk_apply(&g, 1, PLAY(p)), "play");
        CHECK(pk_undo(&g), "undo returns the play");
        CHECK(pk_hash(&g) == pk_hash(&before_play), "undo-by-replay equals the state before the play");
        CHECK(!pk_undo(&g), "undo refuses to pass below a draw");
        CHECK(pk_hash(&g) == pk_hash(&before_play), "and leaves the game alone");
        CHECK(pk_floor(&g) == g.hist_n, "the floor is at the draw");
        CHECK(pk_apply(&g, 1, CALL(2)) && pk_uncall(&g), "uncall always works");
        CHECK(pk_hash(&g) == pk_hash(&before_play), "uncall is exact");
        CHECK(pk_to_floor(&g) && pk_hash(&g) == pk_hash(&before_play), "cancel keeps the draw (D9)");
        /* a bubble with only a catch disappears on uncall */
        PkGame h = before_draw;
        CHECK(pk_apply(&h, 2, CALL(0)) && h.b_open, "a catch opens a bubble");
        CHECK(pk_uncall(&h) && !h.b_open && pk_hash(&h) == pk_hash(&before_draw), "and uncall closes it");
    }
    CHECK(done, "found a draw-then-play position");
}

int main(void)
{
    t_matching();
    t_pass();
    t_draw();
    t_call_leaks_nothing();
    t_say_it();
    t_menu_order();
    t_last_wild();
    t_action_cards();
    t_penalty_short();
    t_going_out();
    t_hand_order();
    t_round_robin();
    t_shuffle_golden();
    t_start_card();
    t_reshuffle();
    t_no_overlap();
    t_top_stays();
    t_windows();
    t_judged_at_open();
    t_penalty_at_end();
    t_lobby_verdicts();
    t_two_routes();
    t_leave();
    t_two_players();
    t_stuck();
    t_undo();
    return report("pk_test");
}

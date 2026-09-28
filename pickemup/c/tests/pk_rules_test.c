/* The rules' edges, one test per edge a conformance review of
 * pickemup/docs/RULES_AND_KERNEL.md found no test for: the one-card timing
 * and its collisions (1.8), penalties that run the deck dry (1.6, 1.9, D15),
 * the start card (1.4, D14), two-player chains (D7, D13), going out (1.7,
 * D17), the stops (1.10, 1.11, D16, D23), the history cap (D30), undo and the
 * reshuffle (D8, D50) and the seat resolver after a leave (4.6, D47, D51).
 * Each test names the rule it pins; tests/MUTATIONS.md records the mutation
 * that turned each one red.
 *
 *     make -C pickemup/c run
 */
#include "pk_check.h"
#include "../src/pk_internal.h"
#include "../src/pk_msg.h"
#include "../src/pk_view.h"

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

/* Take card `c` out of the deck entirely (to build a stack by hand). */
static void pull(PkGame *g, uint8_t c)
{
    for (int i = 0; i < g->deck_n; i++)
        if (g->deck[i] == c) {
            for (int j = i; j + 1 < g->deck_n; j++) g->deck[j] = g->deck[j + 1];
            g->deck_n--;
            return;
        }
    fprintf(stderr, "pull: card %d not in the deck\n", c);
    exit(2);
}

/* ---- 1.8 the last card: timing and collisions ------------------------------------ */

/* 2 players: a Skip to one card hands the turn back but ends the bubble (D7);
 * the exposed player's NEXT bubble may say it with their turn, and go out. */
static void t_say_with_turn_2p(void)
{
    TEST("1.8 say it in a later bubble, with the turn");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, skip_(0, 0)); give(&g, 0, num(0, 9, 0));
    give(&g, 1, num(2, 2, 0)); give(&g, 1, num(2, 3, 0));
    CHECK(pk_apply(&g, 0, PLAY(0)) && g.turn == 0 && g.exposed == 1, "a Skip to one card, the turn back");
    CHECK(!pk_is_legal(&g, 0, SAY), "not in the exposing bubble, even with the turn back (D3)");
    CHECK(!pk_is_legal(&g, 0, PLAY(0)) && !pk_is_legal(&g, 0, DRAW), "and nothing more of the turn (D7)");
    CHECK(pk_seal(&g), "seal");
    CHECK(pk_is_legal(&g, 0, SAY) && pk_apply(&g, 0, SAY), "the next bubble says it");
    CHECK(pk_apply(&g, 0, PLAY(0)) && g.over == PK_OVER_OUT && g.winner == 0, "and goes out in it");
    CHECK(pk_seal(&g) && (g.hist[g.hist_n - 2].b & PK_BR_SAID), "the bubble keeps its say");
}

/* D32: a catch reads the stamps as they stood at open, so a stamp the
 * catcher's own +2 wipes inside the bubble still protects its owner. */
static void t_catch_stamp_at_open(void)
{
    TEST("D32 a stamp wiped inside the bubble still protects");
    PkGame g;
    exposed3(&g);                         /* seat 0 on one card; seat 1 to play */
    CHECK(pk_apply(&g, 0, SAY) && pk_seal(&g) && g.said == 1, "seat 0 says it and is stamped");
    g.dir = -1;                           /* so seat 1's +2 lands on seat 0 */
    give(&g, 1, plus2_(0, 0));
    int p = g.hand_n[1] - 1;
    CHECK(pk_apply(&g, 1, PLAY(p)), "seat 1 plays +2 on seat 0");
    CHECK(g.hand_n[0] == 3 && g.said == 0, "the stamp went with the draw");
    CHECK(!pk_is_legal(&g, 1, CALL(0)), "a catch on the seat stamped at open is still refused (D5c, D32)");
    CHECK(pk_is_legal(&g, 1, CALL(2)), "another seat may still be called");
}

/* 1.8: the window stays open through a say-only bubble of somebody else and a
 * catch-only bubble; the first completed turn closes it, whoever takes it. */
static void t_window_collisions(void)
{
    TEST("1.8 the window and the bubbles that do not close it");
    PkGame g;
    exposed3(&g);
    /* seat 2 makes a wrong catch of seat 1, then seat 2 again of seat 1: two
     * catch-only bubbles, and the window on seat 0 is still open */
    CHECK(pk_apply(&g, 2, CALL(1)) && pk_seal(&g), "a catch-only bubble");
    CHECK(pk_apply(&g, 2, CALL(1)) && pk_seal(&g), "another");
    CHECK(g.exposed == 1 && g.hand_n[2] == 5, "the window is open, the catcher paid twice");
    /* the exposed player, out of turn, catches someone wrongly: a draw, so
     * they are off one card and nobody can catch them any more */
    PkGame h = g;
    CHECK(pk_apply(&h, 0, CALL(2)) && pk_seal(&h), "the exposed player's own wrong catch");
    CHECK(h.hand_n[0] == 2 && h.exposed == 0, "draws one, and is no longer exposed");
    CHECK(pk_apply(&h, 1, CALL(0)) && pk_apply(&h, 1, PLAY(0)) && pk_seal(&h) && h.hand_n[1] == 2 + 1,
          "a catch on them now misses: the catcher draws one");
    /* the exposed player says it and catches in one bubble: both count */
    h = g;
    CHECK(pk_apply(&h, 0, CALL(2)) && pk_apply(&h, 0, SAY), "catch, then say, one bubble");
    CHECK(pk_seal(&h) && h.hand_n[0] == 2 && h.said == 0,
          "the miss is dealt at seal, after the say: two cards, no stamp (D5d)");
    /* a say and a turn of somebody else: the say is safe, the turn changes nothing */
    h = g;
    CHECK(pk_apply(&h, 0, SAY) && pk_seal(&h), "the say");
    CHECK(pk_apply(&h, 1, PLAY(0)) && pk_seal(&h) && h.said == 1 && h.exposed == 0,
          "the next turn leaves the stamp");
}

/* D5b: a wrong catch costs the catcher one, and a right one the caught two,
 * both through the normal draw path, reshuffle included. */
static void t_catch_penalty_reshuffles(void)
{
    TEST("D5b a catch penalty reshuffles the stack in");
    PkGame g;
    table(&g, 3, num(0, 5, 0), 0);
    give(&g, 0, num(0, 6, 0)); give(&g, 0, num(1, 1, 0));
    give(&g, 1, num(0, 7, 0)); give(&g, 1, num(2, 2, 0));
    give(&g, 2, num(3, 3, 0)); give(&g, 2, num(3, 4, 0));
    pk_apply(&g, 0, PLAY(0));
    pk_seal(&g);                           /* stack: 5, 6 of circles */
    while (g.deck_n > 1) give(&g, 2, g.deck[g.deck_n - 1]);
    int before = g.hand_n[0];
    CHECK(pk_apply(&g, 2, CALL(0)) && pk_seal(&g), "seat 2 catches seat 0 out of turn");
    CHECK(g.hand_n[0] == before + 2 && g.reshuffles == 1, "a hit, two cards, one of them after a reshuffle (%d, r %d)",
          g.hand_n[0] - before, g.reshuffles);
    CHECK(g.stack_n == 1 && g.stack[0] == num(0, 6, 0), "the top stays");
    CHECK(conserved(&g), "conserved");
}

/* ---- 1.6, 1.9, D15: penalties when the deck runs dry -------------------------------- */

/* The deck holds one card and the stack five under the top: a +2 draws the
 * one, reshuffles, draws the next; the reshuffle triple sits between the two
 * penalty draws. */
static void t_reshuffle_inside_penalty(void)
{
    TEST("1.9 a reshuffle inside a penalty");
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, plus2_(0, 0)); give(&g, 0, num(3, 1, 0)); give(&g, 0, num(3, 2, 0));
    uint8_t under[5] = { num(1, 9, 0), num(1, 9, 1), num(2, 9, 0), num(2, 9, 1), num(3, 9, 0) };
    g.stack_n = 0;
    for (int i = 0; i < 5; i++) { pull(&g, under[i]); g.stack[g.stack_n++] = under[i]; }
    g.stack[g.stack_n++] = num(0, 5, 0);
    while (g.deck_n > 1) give(&g, 1, g.deck[g.deck_n - 1]);
    uint8_t last = g.deck[0];
    int before = g.hand_n[1];
    Tape t;
    PkSink k = tape_sink(&t);
    CHECK(pk__apply(&g, 0, PLAY(0), &k), "the +2");
    CHECK(g.hand_n[1] == before + 2 && g.hand[1][before] == last, "two cards, the deck's last one first");
    CHECK(g.reshuffles == 1 && g.deck_n == 5 && g.stack_n == 1 && g.stack[0] == plus2_(0, 0),
          "reshuffled once: the +2 stays on top, the old top went in (deck %d)", g.deck_n);
    int seq[8], ns = 0;
    for (int i = 0; i < t.n && ns < 8; i++) {
        int kd = t.ev[i].kind;
        if (kd == PK_EV_PENALTY_DRAW || kd == PK_EV_RESHUFFLE_GATHER || kd == PK_EV_RESHUFFLE_SHUFFLE
            || kd == PK_EV_RESHUFFLE_DONE || kd == PK_EV_PENALTY_SHORT)
            seq[ns++] = kd;
    }
    CHECK(ns == 5 && seq[0] == PK_EV_PENALTY_DRAW && seq[1] == PK_EV_RESHUFFLE_GATHER
          && seq[2] == PK_EV_RESHUFFLE_SHUFFLE && seq[3] == PK_EV_RESHUFFLE_DONE && seq[4] == PK_EV_PENALTY_DRAW,
          "draw, the triple, draw (%d events)", ns);
    CHECK(conserved(&g), "conserved");
}

/* The deck empty and the stack only its top: a draw is refused, a +4 is
 * supplied only by the old top (the +4 itself stays), and a catch is forgiven
 * outright. */
static void t_pile_only_top(void)
{
    TEST("1.9 the pile is only its top card");
    PkGame g;
    table(&g, 3, num(0, 5, 0), 0);
    give(&g, 0, wild4_(0)); give(&g, 0, num(3, 1, 0)); give(&g, 0, num(3, 2, 0));
    while (g.deck_n) give(&g, 2, g.deck[g.deck_n - 1]);
    CHECK(!pk_is_legal(&g, 0, DRAW) && !pk_can_draw_any(&g), "nothing to draw");
    int before = g.hand_n[1];
    Tape t;
    PkSink k = tape_sink(&t);
    CHECK(pk__apply(&g, 0, PLAYW(0, 2), &k), "a +4 on seat 1");
    int shorted = -1;
    for (int i = 0; i < t.n; i++) if (t.ev[i].kind == PK_EV_PENALTY_SHORT) shorted = t.ev[i].n;
    CHECK(g.hand_n[1] == before + 1 && g.hand[1][before] == num(0, 5, 0), "the old top is the one card there was");
    CHECK(shorted == 3 && g.reshuffles == 1, "three forgiven (%d), one reshuffle", shorted);
    CHECK(g.stack_n == 1 && g.stack[0] == wild4_(0) && g.live_suit == 2, "the +4 stays, with its suit");
    CHECK(g.turn == 2 && pk_seal(&g), "seat 1 lost the turn");
    /* nothing drawable now: a wrong catch costs nothing, and says so */
    CHECK(!pk_can_draw_any(&g), "nothing drawable");
    Tape t2;
    PkSink k2 = tape_sink(&t2);
    int h1 = g.hand_n[1];
    CHECK(pk__apply(&g, 1, CALL(0), &k2) && pk__seal(&g, &k2), "seat 1 calls seat 0 wrongly");
    shorted = -1;
    for (int i = 0; i < t2.n; i++) if (t2.ev[i].kind == PK_EV_PENALTY_SHORT) shorted = t2.ev[i].n;
    CHECK(g.hand_n[1] == h1 && shorted == 1, "the miss is forgiven (%d)", shorted);
    CHECK(conserved(&g), "conserved");
}

/* ---- 1.4, D14: the start card ---------------------------------------------------------- */

static void t_wild4_start(void)
{
    TEST("D14 a Wild +4 turned at the start is buried");
    int found = 0;
    for (uint32_t k = 0; k < 200000 && found < 3; k++) {
        uint8_t seed[32];
        seed_wide(seed, 3000000u + k);
        int n = 2 + (int)(k % 7);
        PkGame g;
        if (!pk_new(&g, seed, n)) { CHECK(0, "seed %u deals", k); break; }
        PkEvent ev[256];
        int ne = pk_plan(&g, PK_VIEW_ALL, -1, 0, ev, 256);
        int first_flip = -1;
        for (int i = 0; i < ne; i++) if (ev[i].kind == PK_EV_FLIP) { first_flip = ev[i].card; break; }
        if (first_flip < 100) continue;
        found++;
        int ok = 1;
        for (int s = 0; s < n; s++) ok &= g.hand_n[s] == PK_HAND_SIZE;
        CHECK(ok, "seed %u: nobody drew for it", k);
        CHECK(g.turn == 1 && g.dir == 1 && pk_is_number(g.stack[0]) && g.stack_n == 1,
              "seed %u: seat 1 starts on a number", k);
        CHECK(g.live_suit == pk_suit(g.stack[0]), "seed %u: the live suit is the number's, not chosen", k);
        int at = -1;
        for (int i = 0; i < g.deck_n; i++) if (g.deck[i] == first_flip) at = i;
        CHECK(at >= 0 && at < 8, "seed %u: the +4 is at the bottom of the deck (%d)", k, at);
    }
    CHECK(found == 3, "found three deals that turn a +4 first (%d)", found);
}

/* ---- D7, D13: two-player chains ---------------------------------------------------------- */

static void t_chain_2p(void)
{
    TEST("D7 a two-player chain: Skip, Reverse, +2, a number");
    PkGame g;
    table(&g, 2, num(1, 5, 0), 1);
    give(&g, 0, skip_(1, 0)); give(&g, 0, rev_(1, 0)); give(&g, 0, plus2_(1, 0));
    give(&g, 0, num(1, 3, 0)); give(&g, 0, num(2, 1, 0)); give(&g, 0, num(2, 2, 0));
    give(&g, 1, num(3, 1, 0)); give(&g, 1, num(3, 2, 0));
    int b1 = g.hand_n[1];
    CHECK(pk_apply(&g, 0, PLAY(0)) && g.turn == 0, "Skip: back to seat 0");
    CHECK(pk_apply(&g, 0, PLAY(0)) && g.turn == 0 && g.dir == 1, "Reverse: a skip, dir unchanged (D13)");
    CHECK(pk_apply(&g, 0, PLAY(0)) && g.turn == 0 && g.hand_n[1] == b1 + 2, "+2: seat 1 draws, back to seat 0");
    CHECK(pk_apply(&g, 0, PLAY(0)) && g.turn == 1, "a number: the turn goes over");
    CHECK(pk_turn_ended(&g) && pk_seal(&g), "the turn is over and the bubble seals");
    static const int want[] = { PK_A_BUBBLE, PK_A_PLAY, PK_A_CONTINUE, PK_A_PLAY, PK_A_CONTINUE,
                                PK_A_PLAY, PK_A_CONTINUE, PK_A_PLAY };
    int ok = g.hist_n == (int)(sizeof want / sizeof want[0]);
    for (int i = 0; ok && i < g.hist_n; i++) ok = g.hist[i].kind == want[i];
    CHECK(ok, "one CONTINUE per turn that came back (%d records)", g.hist_n);
    CHECK(g.turns == 4 && g.actions == 4 && g.bubbles == 1, "four turns in one bubble");
    PkView v;
    pk_view(&g, 0, &v);
    CHECK(!v.show_dir, "no direction word at two players");
}

/* ---- 1.7, D17: going out ------------------------------------------------------------------ */

static void t_going_out(void)
{
    TEST("1.7 going out on a +4 or a wild");
    for (int n = 2; n <= 3; n++) {
        PkGame g;
        table(&g, n, num(0, 5, 0), 0);
        give(&g, 0, wild4_(1));
        give(&g, 1, num(1, 1, 0)); give(&g, 1, num(1, 2, 0));
        if (n == 3) { give(&g, 2, num(2, 1, 0)); give(&g, 2, num(2, 2, 0)); }
        CHECK(!pk_is_legal(&g, 0, PLAYW(0, 2)), "n %d: a last-card +4 takes no suit (D17)", n);
        CHECK(pk_apply(&g, 0, PLAY(0)), "n %d: goes out on a +4", n);
        CHECK(g.over == PK_OVER_OUT && g.winner == 0 && g.hand_n[1] == 2 && g.live_suit == 0,
              "n %d: the victim draws nothing and the suit is unchanged", n);
        CHECK(g.exposed == 0 && g.turn == PK_SEAT_NONE, "n %d: no exposure, no turn", n);
    }
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, wild_(0));
    give(&g, 1, num(1, 1, 0));
    Tape t;
    PkSink k = tape_sink(&t);
    CHECK(pk__apply(&g, 0, PLAY(0), &k) && g.over == PK_OVER_OUT, "goes out on a wild");
    int suit_ev = 0;
    for (int i = 0; i < t.n; i++) suit_ev += t.ev[i].kind == PK_EV_WILD_SUIT;
    CHECK(suit_ev == 0, "no suit is chosen, so none is announced");
}

/* ---- 1.10, 1.11, D16, D23: the stops ------------------------------------------------------- */

static void t_long_stop(void)
{
    TEST("1.11 the long-game stop, mid-turn and on a play");
    /* 4 players; seats 1 and 3 tie on the fewest */
    PkGame g;
    table(&g, 4, num(0, 5, 0), 0);
    give(&g, 0, num(1, 1, 0)); give(&g, 0, num(1, 2, 0)); give(&g, 0, num(1, 3, 0));
    give(&g, 1, num(2, 1, 0)); give(&g, 1, num(2, 2, 0));
    give(&g, 2, skip_(0, 0)); give(&g, 2, num(3, 1, 0)); give(&g, 2, num(3, 2, 0));
    give(&g, 3, num(3, 3, 0)); give(&g, 3, num(3, 4, 0));
    g.turn = 2;
    g.actions = PK_MAX_ACTIONS - 1;
    PkGame d = g;
    CHECK(pk_apply(&d, 2, DRAW), "the 1,500th action is a draw");
    CHECK(d.over == PK_OVER_LONG && d.winner == 3, "fewest wins; seats 1 and 3 tie, and 3 is next from 2 (%d)", d.winner);
    CHECK(pk_can_seal(&d) && pk_seal(&d), "the bubble seals mid-turn in this one case");
    CHECK(!pk_is_legal(&d, 3, DRAW) && !pk_is_legal(&d, 3, CALL(0)), "nothing after the stop");
    PkGame p = g;
    CHECK(pk_apply(&p, 2, PLAY(0)), "the 1,500th action is a Skip");
    CHECK(p.over == PK_OVER_LONG && p.winner == 1,
          "after the Skip seat 0 would move: 1, 2 and 3 tie on two, and 1 is first from 0 (%d)", p.winner);
}

static void t_bubble_cap(void)
{
    TEST("1.11 the 750-message stop");
    PkGame g;
    exposed3(&g);
    g.bubbles = PK_MAX_BUBBLES - 1;
    CHECK(pk_apply(&g, 2, CALL(1)), "a catch-only bubble is the 750th");
    CHECK(!g.over && pk_seal(&g), "seal");
    CHECK(g.over == PK_OVER_LONG && g.bubbles == PK_MAX_BUBBLES, "the game is over at 750 bubbles");
    CHECK(g.winner == 0, "fewest cards: seat 0 on one (%d)", g.winner);
    CHECK(g.hand_n[2] == 4, "the catch in the 750th bubble was still dealt, before the stop");
    CHECK(!pk_is_legal(&g, g.winner, SAY) && !pk_is_legal(&g, 1, PLAY(0)), "nothing after it");
}

static void t_stuck_ties(void)
{
    TEST("1.10 the stuck table: fewest, and ties by the next to move");
    static const struct { int n, dir, cards[4], want; } rows[] = {
        { 2, +1, { 3, 3 }, 0 },          /* tie: seat 0 is next after seat 1's pass */
        { 3, +1, { 4, 2, 3 }, 1 },       /* no tie */
        { 4, +1, { 2, 5, 2, 3 }, 0 },    /* 0 and 2 tie; 0 is next */
        { 4, -1, { 2, 5, 2, 3 }, 0 },    /* the same the other way round */
        { 4, +1, { 5, 2, 5, 2 }, 1 },    /* 1 and 3 tie; from 0 clockwise 1 comes first */
        { 4, -1, { 5, 2, 5, 2 }, 3 },    /* anticlockwise from 0, 3 comes first */
    };
    for (unsigned r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        PkGame g;
        table(&g, rows[r].n, num(0, 5, 0), 0);
        for (int s = 0; s < rows[r].n; s++)
            for (int i = 0; i < rows[r].cards[s]; i++) {
                static const int ranks[5] = { 1, 2, 3, 4, 6 };   /* never the top's 5 */
                give(&g, s, num(1 + s % 3, ranks[i], s / 3));
            }
        g.deck_n = 0;
        g.dir = (int8_t)rows[r].dir;
        g.turn = 0;
        int passes = 0;
        while (!g.over && passes < 20) {
            int s = g.turn;
            if (!pk_apply(&g, s, PASS) || !pk_seal(&g)) break;
            passes++;
        }
        CHECK(g.over == PK_OVER_STUCK && passes == rows[r].n, "row %u: n bare passes end it (%d)", r, passes);
        CHECK(g.winner == rows[r].want, "row %u: winner %d, want %d", r, g.winner, rows[r].want);
    }
}

/* ---- D30: the history cap ------------------------------------------------------------------ */

/* THE WORST HISTORY: two players who continue whenever they can and prefer
 * the action cards that hand the turn back, so the bubble count stays low and
 * CONTINUE records pile up, driven to the long-game stop. The bound
 * PK_HIST_CAP = 2 x actions + bubbles must hold and the history replay. */
static int greedy_2p(PkGame *g)
{
    if (g->over) return g->b_open ? pk_seal(g) : 0;
    int s = g->b_open ? g->b_sender : g->turn;
    PkAct m[PK_HAND_CAP * 4 + 2];
    int nm = pk_legal_turn(g, s, m, (int)(sizeof m / sizeof m[0]));
    if (nm == 0) return pk_seal(g);
    int pick = -1;
    for (int i = 0; i < nm && pick < 0; i++)
        if (m[i].kind == PK_A_PLAY) {
            int r = pk_rank(g->hand[s][m[i].a]);
            if (r == PK_R_SKIP || r == PK_R_REVERSE || r == PK_R_PLUS2 || r == PK_R_WILD4) pick = i;
        }
    /* otherwise draw, to keep the hands (and the action cards) coming */
    if (pick < 0 && m[0].kind == PK_A_DRAW && rnd(100) < 90) pick = 0;
    if (pick < 0) pick = (int)rnd((uint32_t)nm);
    return pk_apply(g, s, m[pick]);
}

static void t_hist_cap(void)
{
    TEST("D30 the history cap holds at its worst");
    int most = 0, stops = 0;
    for (uint32_t k = 0; k < 40; k++) {
        uint8_t seed[32];
        seed_wide(seed, 4200000u + k);
        static PkGame g, r;
        pk_new(&g, seed, 2);
        for (int step = 0; step < 100000 && !(g.over && !g.b_open); step++)
            if (!greedy_2p(&g)) break;
        stops += g.over == PK_OVER_LONG;
        if (g.hist_n > most) most = g.hist_n;
        CHECK(g.hist_n <= PK_HIST_CAP && g.bubbles <= PK_MAX_BUBBLES && g.actions <= PK_MAX_ACTIONS,
              "game %u: hist %d actions %d bubbles %d within the caps", k, g.hist_n, g.actions, g.bubbles);
        CHECK(pk_replay(&r, &g, g.hist_n) && pk_hash(&r) == pk_hash(&g), "game %u replays", k);
    }
    CHECK(stops > 0, "some game reached the long-game stop (%d)", stops);
    printf("  the worst history: %d records of %d\n", most, PK_HIST_CAP);
}

/* ---- D8, D50: undo and the reshuffle -------------------------------------------------------- */

static void t_undo_reshuffle(void)
{
    TEST("D8 an own-draw reshuffle is never undone");
    PkGame g;
    empty_deck(&g, 12, num(0, 5, 0), 0);
    CHECK(pk_apply(&g, 0, DRAW) && g.reshuffles == 1, "a draw reshuffles");
    PkGame after = g;
    CHECK(!pk_undo(&g), "undo refuses: the draw is the floor");
    CHECK(pk_hash(&g) == pk_hash(&after) && g.reshuffles == 1 && pk_floor(&g) == g.hist_n,
          "the reshuffle stands");
}

/* The same, on a real deal so the undo is the replay the host runs. */
static void t_undo_penalty_reshuffle_real(void)
{
    TEST("D50 undoing a play whose penalty reshuffled is exact");
    int found = 0;
    for (uint32_t k = 0; k < 400 && !found; k++) {
        uint8_t seed[32];
        seed_wide(seed, 5100000u + k);
        static PkGame g, before;
        pk_new(&g, seed, 2);
        for (int step = 0; step < 20000 && !g.over && !found; step++) {
            if (!g.b_open && g.deck_n <= 1 && g.stack_n >= 2) {
                int s = g.turn;
                for (int p = 0; p < g.hand_n[s]; p++) {
                    int r = pk_rank(g.hand[s][p]);
                    if ((r != PK_R_PLUS2 && r != PK_R_WILD4) || g.hand_n[s] < 3 || !pk_can_play(&g, s, p)) continue;
                    before = g;
                    PkAct a = act(PK_A_PLAY, p, pk_is_wild(g.hand[s][p]) ? 0 : PK_NO_SUIT);
                    if (!pk_apply(&g, s, a) || g.reshuffles == before.reshuffles) { g = before; continue; }
                    found = 1;
                    CHECK(pk_undo(&g), "game %u: the play is undoable", k);
                    CHECK(pk_hash(&g) == pk_hash(&before), "game %u: undo is exact, the reshuffle undone too", k);
                    CHECK(g.reshuffles == before.reshuffles && !g.b_open, "game %u: the counter and the draft", k);
                    break;
                }
            }
            if (!found) bot_step(&g);
        }
    }
    CHECK(found, "found a penalty that reshuffles");
}

/* ---- 4.6, D47, D51: the seat resolver after a leave --------------------------------------- */

static const uint8_t *U(const char *s) { return (const uint8_t *)s; }

static void id_tag(const uint8_t seed[32], const char *id, uint8_t tag[PK_TAG_LEN])
{
    pk_tag(seed, (const uint8_t *)id, (int)strlen(id), tag);
}

static void t_resolve_after_leave(void)
{
    TEST("D51 a device that left is never seated by a namesake");
    static PkMsg m;
    static uint8_t recs[PK_REC_BYTES];
    uint8_t seed[32], t[PK_TAG_LEN];
    seed_wide(seed, 777001);
    id_tag(seed, "id-Alex", t);
    CHECK(pk_msg_new(&m, seed, 0, t, U("Alex"), 4) == PK_EOK, "Alex's lobby");
    id_tag(seed, "id-Bo", t);
    CHECK(pk_msg_join(&m, t, U("Bo"), 2) == 1, "Bo");
    uint8_t cleo[PK_TAG_LEN];
    id_tag(seed, "id-Cleo", cleo);
    CHECK(pk_msg_join(&m, cleo, U("Cleo"), 4) == 2, "Cleo, on this device");
    int rn = pk_rec_put(recs, 0, &m, 2);
    CHECK(pk_rec_find(recs, rn, &m) == 2, "Cleo's record");
    CHECK(pk_msg_leave(&m, 2) == PK_EOK, "Cleo leaves");
    CHECK(pk_rec_find(recs, rn, &m) == PK_REC_GONE, "the record now names a row that is gone");
    /* somebody else called Cleo joins: the name is free again */
    id_tag(seed, "id-other-Cleo", t);
    CHECK(pk_msg_join(&m, t, U("Cleo"), 4) == 2, "another Cleo takes seat 2");
    int by;
    int rec = pk_rec_find(recs, rn, &m);
    CHECK(rec == PK_REC_GONE, "still gone: the other Cleo has another tag");
    CHECK(pk_msg_resolve(&m, rec, pk_msg_seat_of_tag(&m, cleo), 0, PK_SENT_UNKNOWN, U("Cleo"), 4, &by) == -1
          && by == PK_BY_NONE, "the leaver's device is not handed the namesake's seat");
    CHECK(pk_msg_resolve(&m, rec, -1, 0, 1, U("Cleo"), 4, &by) == -1,
          "not by the sender witness either");
    CHECK(pk_msg_resolve(&m, -1, -1, 0, PK_SENT_UNKNOWN, U("Cleo"), 4, &by) == 2 && by == PK_BY_NAME,
          "a device with no record still has the name (a rotated id, D47)");
    /* the leaver rejoins under another name: the tag finds the new row */
    CHECK(pk_msg_join(&m, cleo, U("Cleo B"), 6) == 3, "the first Cleo rejoins as Cleo B");
    rec = pk_rec_find(recs, rn, &m);
    CHECK(rec == 3 && pk_msg_resolve(&m, rec, pk_msg_seat_of_tag(&m, cleo), 0, PK_SENT_UNKNOWN,
                                     U("Cleo"), 4, &by) == 3 && by == PK_BY_RECORD,
          "the record finds the rejoined row (%d)", rec);
    /* a record of another game says nothing about this one */
    static PkMsg o;
    uint8_t s2[32];
    seed_wide(s2, 777002);
    CHECK(pk_msg_new(&o, s2, 0, t, U("Zed"), 3) == PK_EOK && pk_rec_find(recs, rn, &o) == -1,
          "no record of this game: -1, not gone");
    /* a roster never holds a name twice, on the wire or by a join */
    static PkMsg dup;
    dup = m;
    memcpy(dup.seat[1].name, "Cleo", 4);
    dup.seat[1].name_len = 4;
    static uint8_t b[PK_MSG_MAX_BYTES];
    CHECK(pk_msg_encode(&dup, b, (int)sizeof b) == PK_EROSTER, "a roster with a name twice is not written");
}

int main(void)
{
    t_say_with_turn_2p();
    t_catch_stamp_at_open();
    t_window_collisions();
    t_catch_penalty_reshuffles();
    t_reshuffle_inside_penalty();
    t_pile_only_top();
    t_wild4_start();
    t_chain_2p();
    t_going_out();
    t_long_stop();
    t_bubble_cap();
    t_stuck_ties();
    t_hist_cap();
    t_undo_reshuffle();
    t_undo_penalty_reshuffle_real();
    t_resolve_after_leave();
    return report("pk_rules_test");
}

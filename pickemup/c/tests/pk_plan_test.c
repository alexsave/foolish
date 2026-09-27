/* Masking (7.5) and the animation plan (7.6) of
 * pickemup/docs/RULES_AND_KERNEL.md. tests/MUTATIONS.md records the mutation
 * that turned each test red.
 *
 *     make -C pickemup/c run
 */
#include "pk_check.h"
#include "../src/pk_internal.h"
#include "../src/pk_view.h"

enum { EV_CAP = 60000 };
static PkEvent EV[EV_CAP];

/* ---- a tape: the sink watching a hand-built position ---------------------------- */

typedef struct { PkEvent ev[512]; int n; } Tape;
static void tape_fn(const PkEvent *e, void *ctx)
{
    Tape *t = (Tape *)ctx;
    if (t->n < 512) t->ev[t->n++] = *e;
}
static PkSink tape(Tape *t)
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

static int same_kinds(const Tape *t, const int *want, int n, const char *what)
{
    int ok = t->n == n;
    for (int i = 0; ok && i < n; i++) ok = t->ev[i].kind == want[i];
    if (!ok) {
        fprintf(stderr, "  %s got:", what);
        for (int i = 0; i < t->n; i++) fprintf(stderr, " %d", t->ev[i].kind);
        fprintf(stderr, "\n  %s want:", what);
        for (int i = 0; i < n; i++) fprintf(stderr, " %d", want[i]);
        fprintf(stderr, "\n");
    }
    return ok;
}

/* A random game played to its end (or `stop` steps), deterministic by index. */
static void random_game(PkGame *g, uint32_t k, int stop)
{
    uint8_t seed[32];
    seed_of(seed, 400000u + k);
    pk_new(g, seed, 2 + (int)(k % 7));
    for (int i = 0; i < stop && !(g->over && !g->b_open); i++) bot_step(g);
}

/* ---- 7.5 masking --------------------------------------------------------------------- */

static void t_counts_never_leak(void)
{
    TEST("7.5.1 other counts never leak");
    int positions = 0;
    for (uint32_t k = 0; k < 1000; k++) {
        PkGame g;
        random_game(&g, k, (int)rnd(200));
        if (g.over || g.n < 3) continue;   /* at 2 players the other count is arithmetic on public counts */
        for (int v = 0; v < g.n; v++) {
            PkView a, b;
            pk_view(&g, v, &a);
            /* move cards between two OTHER seats so both sizes change, and
             * scramble every other seat's exposed bit */
            PkGame h = g;
            int x = (v + 1) % h.n, y = (v + 2) % h.n;
            int moved = 1 + (int)rnd(3);
            for (int i = 0; i < moved && h.hand_n[x] > 1; i++)
                h.hand[y][h.hand_n[y]++] = h.hand[x][--h.hand_n[x]];
            uint8_t mine = (uint8_t)(1u << v);
            h.exposed = (uint8_t)((h.exposed & mine) | (rnd(256) & ~mine));
            pk_view(&h, v, &b);
            CHECK(!memcmp(&a, &b, sizeof a), "game %u viewer %d: the view changed with another hand", k, v);
            positions++;
        }
    }
    CHECK(positions > 1000, "enough positions (%d)", positions);
}

static void t_spectator(void)
{
    TEST("7.5.2 spectator");
    PkGame g;
    random_game(&g, 7, 30);
    PkView v;
    pk_view(&g, PK_VIEW_SPECTATOR, &v);
    int zero = v.my_n == 0 && !v.can_draw && !v.can_pass && !v.can_seal && !v.can_call && !v.my_exposed;
    for (int s = 0; s < PK_MAX_SEATS; s++) zero &= v.reveal_n[s] == 0;
    CHECK(v.me == PK_SEAT_NONE && zero, "a spectator sees no hand and can do nothing");
    CHECK(v.deck_n == g.deck_n && v.top == g.stack[g.stack_n - 1], "but sees the table");
}

static void t_end_reveals(void)
{
    TEST("7.5.3 game end reveals all");
    PkGame g;
    random_game(&g, 11, 100000);
    CHECK(g.over, "the game ended");
    for (int v = -1; v < g.n; v++) {
        PkView w;
        pk_view(&g, v, &w);
        int all = 1;
        for (int s = 0; s < g.n; s++)
            all &= w.reveal_n[s] == g.hand_n[s] && !memcmp(w.reveal_hand[s], g.hand[s], g.hand_n[s]);
        CHECK(all, "viewer %d sees every hand at the end", v);
    }
}

static void t_events_mask(void)
{
    TEST("7.5.4 events mask draws");
    for (uint32_t k = 0; k < 60; k++) {
        PkGame g;
        random_game(&g, 1000 + k, 100000);
        for (int v = -1; v < g.n; v++) {
            int n = pk_plan(&g, v, -1, g.bubbles, EV, EV_CAP);
            CHECK(n > 0, "plan for viewer %d", v);
            for (int i = 0; i < n; i++) {
                const PkEvent *e = &EV[i];
                if (e->kind == PK_EV_DEAL || e->kind == PK_EV_DRAW || e->kind == PK_EV_PENALTY_DRAW) {
                    int hidden = e->card == PK_CARD_HIDDEN;
                    CHECK(hidden == (v != e->seat), "game %u viewer %d: kind %d to seat %d hidden %d",
                          k, v, e->kind, e->seat, hidden);
                }
                if (e->kind == PK_EV_PLAY || e->kind == PK_EV_FLIP || e->kind == PK_EV_BURY
                    || e->kind == PK_EV_START_CARD || e->kind == PK_EV_REVEAL)
                    CHECK(e->card < PK_DECK, "game %u viewer %d: kind %d is public", k, v, e->kind);
            }
        }
    }
}

/* ---- 7.6 plan events --------------------------------------------------------------------- */

static void t_golden(void)
{
    TEST("7.6.1 golden plans");
    Tape t;
    PkSink k;

    /* 1. a plain play */
    PkGame g;
    table(&g, 3, num(0, 5, 0), 0);
    give(&g, 0, num(0, 6, 0)); give(&g, 0, num(1, 1, 0)); give(&g, 0, num(1, 2, 0));
    k = tape(&t);
    pk__apply(&g, 0, PLAY(0), &k);
    pk__seal(&g, &k);
    static const int w1[] = { PK_EV_BUBBLE_BEGIN, PK_EV_PLAY, PK_EV_TURN_TO, PK_EV_BUBBLE_END };
    CHECK(same_kinds(&t, w1, 4, "plain"), "a plain play");
    CHECK(t.ev[1].half == PK_HALF_ACTION && t.ev[2].half == PK_HALF_SETTLE && t.ev[2].seat == 1,
          "PLAY is the action, TURN_TO seat 1 the settlement");

    /* 2. draw 9 with a reshuffle on the 4th, then play */
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, num(0, 6, 0)); give(&g, 0, num(3, 1, 0));
    g.stack_n = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = g.deck[--g.deck_n];
        g.stack[g.stack_n++] = c;
    }
    g.stack[g.stack_n++] = num(0, 5, 0);
    while (g.deck_n > 3) give(&g, 1, g.deck[g.deck_n - 1]);
    k = tape(&t);
    for (int i = 0; i < 9; i++) CHECK(pk__apply(&g, 0, DRAW, &k), "draw %d", i + 1);
    pk__apply(&g, 0, PLAY(0), &k);
    pk__seal(&g, &k);
    static const int w2[] = { PK_EV_BUBBLE_BEGIN, PK_EV_DRAW, PK_EV_DRAW, PK_EV_DRAW,
        PK_EV_RESHUFFLE_GATHER, PK_EV_RESHUFFLE_SHUFFLE, PK_EV_RESHUFFLE_DONE, PK_EV_DRAW,
        PK_EV_DRAW, PK_EV_DRAW, PK_EV_DRAW, PK_EV_DRAW, PK_EV_DRAW, PK_EV_PLAY, PK_EV_TURN_TO,
        PK_EV_BUBBLE_END };
    CHECK(same_kinds(&t, w2, 16, "draw9"), "draw 9 with a reshuffle, then play");
    CHECK(t.ev[4].step == t.ev[7].step && t.ev[4].n == 8 && t.ev[5].i == 1 && t.ev[6].n == 8,
          "the triple shares the 4th draw's step; 8 cards, reshuffle 1");
    CHECK(t.ev[12].i == 9, "the 9th draw says so");

    /* 3. a +2 at two players, then the turn continues */
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, plus2_(0, 0)); give(&g, 0, num(0, 7, 0)); give(&g, 0, num(1, 1, 0));
    k = tape(&t);
    pk__apply(&g, 0, PLAY(0), &k);
    pk__apply(&g, 0, PLAY(0), &k);
    pk__seal(&g, &k);
    static const int w3[] = { PK_EV_BUBBLE_BEGIN, PK_EV_PLAY, PK_EV_PENALTY, PK_EV_PENALTY_DRAW,
        PK_EV_PENALTY_DRAW, PK_EV_TURN_TO, PK_EV_PLAY, PK_EV_TURN_TO, PK_EV_BUBBLE_END };
    CHECK(same_kinds(&t, w3, 9, "plus2"), "+2 at two players with continue");
    CHECK(t.ev[2].seat == 1 && t.ev[2].n == 2 && t.ev[2].i == PK_PEN_PLUS2 && t.ev[5].seat == 0,
          "seat 1 owes two; the turn comes back to seat 0");

    /* 4. a hit catch in a turn bubble */
    exposed3(&g);
    k = tape(&t);
    pk__apply(&g, 1, CALL(0), &k);
    pk__apply(&g, 1, PLAY(0), &k);
    pk__seal(&g, &k);
    static const int w4[] = { PK_EV_BUBBLE_BEGIN, PK_EV_CALL_OUT, PK_EV_PLAY, PK_EV_TURN_TO,
        PK_EV_CALL_HIT, PK_EV_PENALTY, PK_EV_PENALTY_DRAW, PK_EV_PENALTY_DRAW, PK_EV_BUBBLE_END };
    CHECK(same_kinds(&t, w4, 9, "catch"), "a hit catch in a turn bubble");
    CHECK(t.ev[1].seat == 0 && t.ev[1].other == 1 && t.ev[4].seat == 0 && t.ev[5].i == PK_PEN_CAUGHT,
          "seat 1 caught seat 0");

    /* 5. a wild win, and the reveal */
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, wild_(0));
    give(&g, 1, num(1, 1, 0)); give(&g, 1, num(1, 2, 0));
    k = tape(&t);
    pk__apply(&g, 0, PLAYW(0, PK_NO_SUIT), &k);
    pk__seal(&g, &k);
    static const int w5[] = { PK_EV_BUBBLE_BEGIN, PK_EV_PLAY, PK_EV_WIN, PK_EV_REVEAL, PK_EV_REVEAL,
        PK_EV_BUBBLE_END };
    CHECK(same_kinds(&t, w5, 6, "win"), "a wild win with the reveal");
    CHECK(t.ev[2].seat == 0 && t.ev[2].card == wild_(0) && t.ev[2].i == PK_OVER_OUT
          && t.ev[3].seat == 1 && t.ev[4].i == 2, "seat 0 won on the wild; seat 1's two cards shown");

    /* the deal, bubble 0, of a real game */
    uint8_t seed[32];
    seed_of(seed, 5);
    pk_new(&g, seed, 3);
    g.starter = 2;
    int n = pk_plan(&g, 1, -1, 0, EV, EV_CAP);
    CHECK(n > 0 && EV[0].kind == PK_EV_LOBBY_START && EV[0].seat == 2 && EV[0].n == 3,
          "LOBBY_START first, naming the starter");
    CHECK(EV[1].kind == PK_EV_SHUFFLE && EV[1].n == PK_DECK, "then SHUFFLE");
    int ok = 1;
    for (int i = 0; i < 21; i++) {
        const PkEvent *e = &EV[2 + i];
        ok &= e->kind == PK_EV_DEAL && e->seat == (1 + i) % 3 && e->i == i + 1 && e->n == i / 3 + 1;
        ok &= (e->seat == 1) == (e->card != PK_CARD_HIDDEN);
    }
    CHECK(ok, "21 DEALs round-robin from seat 1, one card each, masked but for seat 1");
    CHECK(EV[n - 2].kind == PK_EV_START_CARD && EV[n - 1].kind == PK_EV_TURN_TO && EV[n - 1].seat == 1,
          "START_CARD, then TURN_TO seat 1");
}

/* Every bubble's events, in order, against the ordering guarantees (5.3). */
static void t_ordering(void)
{
    TEST("7.6.2 ordering invariants");
    for (uint32_t k = 0; k < 1000; k++) {
        PkGame g;
        random_game(&g, 2000 + k, 100000);
        int n = pk_plan(&g, PK_VIEW_ALL, -1, g.bubbles, EV, EV_CAP);
        CHECK(n > 0, "game %u plans", k);
        int turn_seen = 0, last_terminal = -1, bubble = 0;
        for (int i = 0; i < n; i++) {
            const PkEvent *e = &EV[i];
            if (i) CHECK(e->step >= EV[i - 1].step, "game %u event %d: steps never decrease", k, i);
            if (e->bubble != bubble) {
                CHECK(e->bubble == bubble + 1 && e->kind == PK_EV_BUBBLE_BEGIN, "game %u: bubbles open in order", k);
                bubble = e->bubble;
                turn_seen = 0;
                last_terminal = -1;
            }
            switch (e->kind) {
            case PK_EV_SAY_IT: case PK_EV_CALL_OUT:
                CHECK(!turn_seen, "game %u event %d: bubble-level before the turn", k, i);
                break;
            case PK_EV_DRAW: case PK_EV_PLAY: case PK_EV_PASS:
                turn_seen = 1;
                if (e->kind != PK_EV_DRAW) last_terminal = i;
                break;
            case PK_EV_CALL_HIT: case PK_EV_CALL_MISS: {
                int later = 0;
                for (int j = i + 1; j < n && EV[j].bubble == e->bubble; j++)
                    later |= EV[j].kind == PK_EV_PLAY || EV[j].kind == PK_EV_PASS || EV[j].kind == PK_EV_DRAW;
                CHECK(!later && (last_terminal >= 0 || !turn_seen), "game %u event %d: the catch after the turn", k, i);
                break;
            }
            case PK_EV_RESHUFFLE_GATHER:
                CHECK(i + 3 < n && EV[i + 1].kind == PK_EV_RESHUFFLE_SHUFFLE
                      && EV[i + 2].kind == PK_EV_RESHUFFLE_DONE
                      && (EV[i + 3].kind == PK_EV_DRAW || EV[i + 3].kind == PK_EV_PENALTY_DRAW)
                      && EV[i + 3].step == e->step && EV[i + 3].half == e->half,
                      "game %u event %d: the triple, then the draw it serves", k, i);
                break;
            case PK_EV_WIN:
                CHECK(i + 1 < n, "game %u: WIN is not the last event", k);
                break;
            default: break;
            }
        }
        CHECK(EV[n - 1].kind == PK_EV_BUBBLE_END, "game %u: ends on BUBBLE_END", k);
    }
}

static int is_catch_settle(const PkEvent *e)
{
    return e->kind == PK_EV_CALL_HIT || e->kind == PK_EV_CALL_MISS
        || (e->kind == PK_EV_PENALTY && (e->i == PK_PEN_CAUGHT || e->i == PK_PEN_WRONG));
}

static void t_cut(void)
{
    TEST("7.6.3 the cut");
    int drafts = 0, catches = 0;
    for (uint32_t k = 0; k < 300; k++) {
        PkGame g;
        uint8_t seed[32];
        seed_of(seed, 7000u + k);
        pk_new(&g, seed, 2 + (int)(k % 7));
        for (int step = 0; step < 3000 && !(g.over && !g.b_open); step++) {
            if (g.b_open && g.b_call != PK_SEAT_NONE) {
                int n = pk_plan_draft(&g, g.b_sender, EV, EV_CAP);
                drafts++;
                for (int i = 0; i < n; i++)
                    CHECK(!is_catch_settle(&EV[i]), "game %u: a staged draft holds a catch's outcome", k);
            }
            int was = g.bubbles;
            bot_step(&g);
            if (g.bubbles != was) {
                int n = pk_plan(&g, g.b_open ? -1 : PK_VIEW_ALL, was, g.bubbles, EV, EV_CAP);
                for (int i = 0; i < n; i++) {
                    const PkEvent *e = &EV[i];
                    if (is_catch_settle(e)) {
                        catches++;
                        CHECK(e->half == PK_HALF_SETTLE, "game %u: a catch outcome is ACTION-half", k);
                    }
                    if (e->kind == PK_EV_PENALTY_DRAW && i && EV[i - 1].kind != PK_EV_PENALTY_DRAW
                        && EV[i - 1].kind != PK_EV_PENALTY && EV[i - 1].kind != PK_EV_RESHUFFLE_DONE)
                        CHECK(0, "game %u: a penalty draw out of its run", k);
                    if (e->kind == PK_EV_TURN_TO || e->kind == PK_EV_SKIP || e->kind == PK_EV_PENALTY
                        || e->kind == PK_EV_WIN || e->kind == PK_EV_REVEAL)
                        CHECK(e->half == PK_HALF_SETTLE, "game %u: kind %d is SETTLE", k, e->kind);
                    if (e->kind == PK_EV_PLAY || e->kind == PK_EV_DRAW || e->kind == PK_EV_PASS
                        || e->kind == PK_EV_SAY_IT || e->kind == PK_EV_CALL_OUT)
                        CHECK(e->half == PK_HALF_ACTION, "game %u: kind %d is ACTION", k, e->kind);
                }
            }
        }
    }
    CHECK(drafts > 50 && catches > 50, "exercised: %d drafts with a catch, %d catch events", drafts, catches);

    /* two players: a continued turn's settlement is released before the next action */
    PkGame g;
    table(&g, 2, num(0, 5, 0), 0);
    give(&g, 0, skip_(0, 0)); give(&g, 0, num(0, 7, 0)); give(&g, 0, num(1, 1, 0));
    Tape t;
    PkSink k = tape(&t);
    pk__apply(&g, 0, PLAY(0), &k);
    int settled = t.n;
    pk__apply(&g, 0, PLAY(0), &k);
    int ok = 1;
    for (int i = 0; i < settled; i++) ok &= t.ev[i].step < t.ev[settled].step;
    CHECK(ok && t.ev[settled - 1].kind == PK_EV_TURN_TO && t.ev[settled - 1].half == PK_HALF_SETTLE,
          "the first turn's SKIP and TURN_TO come before the continued play");
}

static void t_one_card_per_event(void)
{
    TEST("7.6.4 one card per event");
    for (uint32_t k = 0; k < 300; k++) {
        PkGame g;
        random_game(&g, 3000 + k, (int)rnd(400));
        int n = pk_plan(&g, PK_VIEW_ALL, -1, g.bubbles + (g.b_open ? 1 : 0), EV, EV_CAP);
        CHECK(n > 0, "game %u plans", k);
        int to_hands = 0, from_hands = 0, deck = PK_DECK, stack = 0;
        for (int i = 0; i < n; i++) {
            const PkEvent *e = &EV[i];
            switch (e->kind) {
            case PK_EV_DEAL: case PK_EV_DRAW: case PK_EV_PENALTY_DRAW: to_hands++; deck--; break;
            case PK_EV_PLAY: from_hands++; stack++; break;
            case PK_EV_FLIP: deck--; break;
            case PK_EV_BURY: deck++; break;
            case PK_EV_START_CARD: stack++; break;
            case PK_EV_RESHUFFLE_GATHER: deck += e->n; stack -= e->n; break;
            default: break;
            }
        }
        int held = 0;
        for (int s = 0; s < g.n; s++) held += g.hand_n[s];
        CHECK(to_hands - from_hands == held, "game %u: hands %d by events, %d by state", k,
              to_hands - from_hands, held);
        CHECK(deck == g.deck_n && stack == g.stack_n, "game %u: deck %d/%d stack %d/%d", k,
              deck, g.deck_n, stack, g.stack_n);
    }
}

static PkSince SUM;
static void sum_fn(const PkEvent *e, void *ctx)
{
    (void)ctx;
    if (e->kind == PK_EV_DRAW) SUM.drawn[e->seat]++;
    if (e->kind == PK_EV_PENALTY_DRAW) SUM.penalty[e->seat]++;
    if (e->kind == PK_EV_PLAY) SUM.plays[e->seat]++;
    if (e->kind == PK_EV_RESHUFFLE_SHUFFLE) SUM.reshuffles++;
    if (e->kind == PK_EV_SAY_IT) SUM.said |= (uint8_t)(1u << e->seat);
    if (e->kind == PK_EV_REVERSE) SUM.reversed++;
    if (e->kind == PK_EV_SKIP || e->kind == PK_EV_REVERSE_AS_SKIP) SUM.skipped |= (uint8_t)(1u << e->seat);
    if (e->kind == PK_EV_CALL_HIT) { SUM.caught = e->seat; SUM.caught_by = e->other; }
    if (e->kind == PK_EV_CALL_MISS) { SUM.wrong = e->seat; SUM.wrong_on = e->other; }
}

static void t_since(void)
{
    TEST("7.6.5 since");
    for (uint32_t k = 0; k < 300; k++) {
        PkGame g;
        random_game(&g, 4000 + k, 100000);
        int from = (int)rnd((uint32_t)g.bubbles + 1) - 1;
        int to = from + 1 + (int)rnd((uint32_t)(g.bubbles - from));
        if (to > g.bubbles) to = g.bubbles;
        PkSince s;
        CHECK(pk_since(&g, from, to, &s), "game %u: since", k);
        memset(&SUM, 0, sizeof SUM);
        SUM.caught = SUM.caught_by = SUM.wrong = SUM.wrong_on = PK_SEAT_NONE;
        pk_plan_each(&g, PK_VIEW_ALL, from, to, sum_fn, 0);
        CHECK(!memcmp(s.drawn, SUM.drawn, sizeof s.drawn) && !memcmp(s.penalty, SUM.penalty, sizeof s.penalty)
              && !memcmp(s.plays, SUM.plays, sizeof s.plays), "game %u (%d, %d]: per-seat counts", k, from, to);
        CHECK(s.reshuffles == SUM.reshuffles && s.said == SUM.said && s.reversed == SUM.reversed
              && s.skipped == SUM.skipped, "game %u: range facts", k);
        CHECK(s.caught == SUM.caught && s.caught_by == SUM.caught_by && s.wrong == SUM.wrong
              && s.wrong_on == SUM.wrong_on, "game %u: catches", k);
    }
    PkGame g;
    random_game(&g, 1, 50);
    CHECK(pk_plan(&g, 0, -1, g.bubbles, EV, 3) == -1, "a plan that does not fit says so");
}

int main(void)
{
    t_counts_never_leak();
    t_spectator();
    t_end_reveals();
    t_events_mask();
    t_golden();
    t_ordering();
    t_cut();
    t_one_card_per_event();
    t_since();
    return report("pk_plan_test");
}

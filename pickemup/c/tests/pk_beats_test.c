/* The timeline (src/pk_beats.h) against pickemup/docs/UI.html.
 *
 * EVERY EXPECTED NUMBER BELOW IS READ OFF UI.html, never off the code: the
 * vocabulary table, the motion grid's order badges, and the demo script's
 * `T` object and its `DEMO.*` functions (which sleep, await and stagger in
 * exactly the order a grid row names). Each row test says which demo it
 * transcribes. Where the grid's prose and a demo disagree, the prose won and
 * pickemup/docs/ANIMATION_DECISIONS.md says so; the test names the decision.
 *
 * tests/MUTATIONS.md records the mutation that turned each test red.
 *
 *     make -C pickemup/c run
 */
#include "pk_check.h"
#include "../src/pk_beats.h"
#include "../src/pk_internal.h"
#include "../src/pk_view.h"

static PkBeats B1, B2;
enum { EV_CAP = 4096 };
static PkEvent EV[EV_CAP], PREV[EV_CAP];

/* ---- building synthetic plans ----------------------------------------------------- */

static int NE;
static PkEvent *ev(int kind, int half, int seat)
{
    PkEvent *e = &EV[NE++];
    memset(e, 0, sizeof *e);
    e->kind = (uint8_t)kind;
    e->half = (uint8_t)half;
    e->seat = (uint8_t)seat;
    e->other = PK_SEAT_NONE;
    e->card = PK_CARD_NONE;
    e->suit = 0;
    e->deck_n = 40;
    return e;
}
#define A PK_HALF_ACTION
#define S PK_HALF_SETTLE

/* A four-seat board as a view would give it: 40 in the deck, a 3 of circles
 * on a three-card pile, seat 0's turn, my seven. */
static PkBeatFrame board(int me_n)
{
    PkView v;
    memset(&v, 0, sizeof v);
    v.n = 4;
    v.deck_n = 40;
    v.top = 4;             /* 3 of suit 0 */
    v.stack_n = 3;
    v.turn = 0;
    v.dir = PK_DIR_CW;
    v.live_suit = 0;
    v.me = 3;
    v.my_n = (uint8_t)me_n;
    for (int i = 0; i < me_n; i++) v.my_hand[i] = (uint8_t)(24 + i);
    PkBeatFrame f;
    pk_beats_frame_of(&v, 4, &f);
    return f;
}

static int build(int mode, int viewer, int seats, const PkEvent *prev, int prev_n, PkBeats *out)
{
    PkBeatFrame f = board(7);
    return pk_beats_build(EV, NE, &f, viewer, seats, mode, prev, prev_n, 0, out);
}

/* The k-th beat (0-based) of `kind` that plays event kind `evk` (0: any). */
static const PkBeat *nth(const PkBeats *b, int kind, int evk, int k)
{
    for (int i = 0; i < b->n; i++) {
        const PkBeat *x = &b->beat[i];
        if (x->kind == kind && (!evk || x->ev_kind == evk) && k-- == 0) return x;
    }
    return 0;
}
static int count(const PkBeats *b, int kind, int evk)
{
    int c = 0;
    for (int i = 0; i < b->n; i++) c += b->beat[i].kind == kind && (!evk || b->beat[i].ev_kind == evk);
    return c;
}
static uint32_t beat_end_of(const PkBeat *x) { return x->start_ms + x->dur_ms; }
static long st(const PkBeat *x) { return x ? (long)x->start_ms : -1; }
static long du(const PkBeat *x) { return x ? (long)x->dur_ms : -1; }

/* ---- the vocabulary ----------------------------------------------------------------- */

static void t_vocabulary(void)
{
    TEST("vocabulary");
    /* UI.html `const T` */
    CHECK(PK_T_FLIGHT == 500 && PK_T_GAP == 25 && PK_T_DRAW == 320 && PK_T_DRAW_STEP == 110, "flight");
    CHECK(PK_T_DEAL == 320 && PK_T_HALF == 170 && PK_T_FADE == 220 && PK_T_STAMP == 340, "deal, turn, fade, stamp");
    CHECK(PK_T_POP == 260 && PK_T_SHAKE == 60 && PK_T_SPRING == 320, "pop, shake, spring");
    CHECK(PK_T_GATHER == 360 && PK_T_RIFFLE == 140 && PK_T_FATTEN == 240, "the gag");
    CHECK(PK_T_GAME_OVER == 1000 && PK_T_LEAD_LIVE == 16 && PK_T_LEAD_OPEN == 100, "holds");
    /* A15: the Send reminder's fuse, the sister product's (uttt UTM_SEND_HINT_MS) */
    CHECK(PK_T_SEND_HINT == 3000, "the Send reminder waits three seconds");
    /* the curves: ends pinned, the spring and the stamp overshoot, the flight never does */
    for (int e = 0; e < PK_EASE_COUNT; e++) {
        CHECK(pk_ease(e, 0) == 0 && pk_ease(e, 1) == 1, "curve %d ends", e);
    }
    float mx_spring = 0, mx_stamp = 0, prev = 0;
    int mono = 1;
    for (int i = 1; i <= 100; i++) {
        float x = (float)i / 100.0f;
        float f = pk_ease(PK_EASE_FLIGHT, x);
        if (f < prev) mono = 0;
        prev = f;
        if (pk_ease(PK_EASE_SPRING, x) > mx_spring) mx_spring = pk_ease(PK_EASE_SPRING, x);
        if (pk_ease(PK_EASE_STAMP, x) > mx_stamp) mx_stamp = pk_ease(PK_EASE_STAMP, x);
    }
    CHECK(mono && prev <= 1, "the flight curve only rises");
    CHECK(mx_spring > 1.01f && mx_stamp > 1.05f, "card-spring and the stamp overshoot");
    /* cubic-bezier(.25,.46,.45,.94) at x = .5 is .77132 (solved by bisection in doubles) */
    CHECK(pk_ease(PK_EASE_FLIGHT, .5f) > .7710f && pk_ease(PK_EASE_FLIGHT, .5f) < .7717f,
          "the flight curve at half time: %f", (double)pk_ease(PK_EASE_FLIGHT, .5f));
}

/* ---- U20: the deal ---------------------------------------------------------------------- */

static void t_deal_step(void)
{
    TEST("deal step");
    /* dealStep = n => Math.max(45, Math.min(110, 1800 / n)), n = 7 x seats */
    CHECK(pk_beats_deal_step(14) == 110, "2 seats: 110");
    CHECK(pk_beats_deal_step(21) == 85, "3 seats: 85.7");
    CHECK(pk_beats_deal_step(28) == 64, "4 seats: 64.3");
    CHECK(pk_beats_deal_step(35) == 51, "5 seats: 51.4");
    CHECK(pk_beats_deal_step(42) == 45, "6 seats: 42.9 clamps to 45");
    CHECK(pk_beats_deal_step(56) == 45, "8 seats: 45");
    /* sleep(i * step): the 28th card of four seats at 27 x 64.29 = 1735.7 */
    CHECK(pk_beats_deal_at(27, 28) == 1735, "no drift down the deal: %d", pk_beats_deal_at(27, 28));
    CHECK(pk_beats_deal_at(13, 14) == 1430, "two seats, the last card");
}

/* A four-seat game whose start card is (bury = 0) the first flip, or (1) the
 * second, dealt with the deal as bubble 0. */
static int deal_with(PkGame *g, int bury)
{
    for (uint32_t k = 0; k < 4000; k++) {
        uint8_t seed[32];
        seed_wide(seed, 7000 + k);
        pk__new(g, seed, 4, 0, 0);
        int n = pk_plan(g, 0, -1, 0, EV, EV_CAP), flips = 0;
        for (int i = 0; i < n; i++) flips += EV[i].kind == PK_EV_FLIP;
        if (flips == bury + 1) return n;
    }
    return -1;
}

/* DEMO.deal: riffle(2); every card at i x step (320ms, bulge 1.05), mine
 * then flips 160; sleep(250); FLIP 500 (face at the midpoint); a BURY is
 * sleep(300), a 500 flight to +9 deg with no bulge, sleep(120); the halo and
 * the direction fade in as the start card lands. Here with the lobby's rest
 * and fade before it (grid "Start": 500 + 220), and the turn bar 25ms after
 * the last flight (grid "Turn moves"; DECISION A3). */
static void t_deal_row(void)
{
    TEST("deal row");
    static PkGame g;
    for (int bury = 0; bury <= 1; bury++) {
        int n = deal_with(&g, bury);
        CHECK(n > 0, "a deal with %d buried", bury);
        if (n <= 0) return;
        PkBeatFrame f;
        pk_beats_frame_of(0, 4, &f);
        CHECK(pk_beats_build(EV, n, &f, 0, 4, PK_BEATS_ARRIVAL, 0, 0, 0, &B1) > 0, "built");
        const PkBeat *hold = nth(&B1, PK_BK_HOLD, PK_EV_LOBBY_START, 0);
        const PkBeat *fade = nth(&B1, PK_BK_FADE, PK_EV_LOBBY_START, 0);
        CHECK(st(hold) == 16 && du(hold) == 500, "the lobby rests 500 from the 16ms beat");
        CHECK(st(fade) == 516 && du(fade) == 220, "then FADEs to the table");
        const PkBeat *r0 = nth(&B1, PK_BK_RIFFLE, PK_EV_SHUFFLE, 0), *r1 = nth(&B1, PK_BK_RIFFLE, PK_EV_SHUFFLE, 1);
        CHECK(st(r0) == 761 && du(r0) == 196 && r0->parts == 8 && r0->stagger_ms == 8, "riffle 1: 140 + 7 x 8");
        CHECK(st(r1) == 957 && du(r1) == 196, "riffle 2 after it");
        int bad = 0, mine = 0;
        for (int i = 0; i < 28; i++) {
            const PkBeat *d = nth(&B1, PK_BK_FLIGHT, PK_EV_DEAL, i);
            long want = 1153 + (long)i * 1800 / 28;
            if (st(d) != want || du(d) != 320 || d->bulge != 105) bad++;
            if (d && d->to == PK_ANC_HAND) {
                const PkBeat *fl = nth(&B1, PK_BK_FLIP, PK_EV_DEAL, mine++);
                if (st(fl) != want + 320 || du(fl) != 160) bad++;
            }
        }
        CHECK(bad == 0, "28 cards at 1153 + i x 1800 / 28, 320 each, mine flip 160 on landing (%d off)", bad);
        CHECK(mine == 7, "seven land in my hand");
        CHECK(nth(&B1, PK_BK_FLIGHT, PK_EV_DEAL, 27)->to == PK_ANC_HAND, "the dealer (me) gets the last card");
        const PkBeat *f0 = nth(&B1, PK_BK_FLIGHT, PK_EV_FLIP, 0);
        CHECK(st(f0) == 3618 && du(f0) == 500 && (f0->flags & PK_BF_FACE_MID) && f0->rot1 == -3,
              "FLIP 250 after my last card turned (2888 + 320 + 160 + 250): %ld", st(f0));
        long land = 4118;
        if (bury) {
            const PkBeat *by = nth(&B1, PK_BK_FLIGHT, PK_EV_BURY, 0);
            const PkBeat *f1 = nth(&B1, PK_BK_FLIGHT, PK_EV_FLIP, 1);
            CHECK(st(by) == 4418 && by->rot0 == -3 && by->rot1 == 9 && by->bulge == 100 && by->to == PK_ANC_BURY,
                  "BURY: sleep(300), then under the deck at +9 deg, no bulge");
            CHECK(st(f1) == 5038, "the next FLIP 120 after the bury lands: %ld", st(f1));
            land = 5538;
        }
        const PkBeat *h = nth(&B1, PK_BK_HALO, PK_EV_START_CARD, 0), *dir = nth(&B1, PK_BK_FADE, PK_EV_START_CARD, 0);
        CHECK(st(h) == land && du(h) == 220, "the halo fades in as the start card lands");
        CHECK(st(dir) == land && dir->to == PK_ANC_DIR, "the direction mark with it (four seats)");
        const PkBeat *tt = nth(&B1, PK_BK_TURN_BAR, PK_EV_TURN_TO, 0);
        CHECK(st(tt) == land + 25 && tt->to_i == 1, "seat 1 goes first, 25ms after the flight");
        CHECK(B1.total_ms == (uint32_t)(land + 25 + 220), "the deal ends with the turn bar");
        /* the board during it */
        pk_beats_frame(&B1, 0, &f);
        CHECK(f.deck_n == PK_DECK && f.my_n == 0 && f.fans_empty == 0x0E && (f.hold & PK_HOLD_BOARD),
              "before: a full deck, empty fans, the table not yet in");
        CHECK((f.hold & PK_HOLD_DIR) && f.buried_hold == bury, "the direction and the buried card wait");
        pk_beats_frame(&B1, 1153 + 320, &f);
        CHECK(f.fans_empty == 0x0C && f.deck_n == 99, "seat 1's first card has landed; five have left the deck (i x 64.3 <= 320)");
        pk_beats_frame(&B1, B1.total_ms, &f);
        PkView v;
        pk_view(&g, 0, &v);
        CHECK(f.done && f.deck_n == v.deck_n && f.top == v.top && f.my_n == 7 && f.turn == 1 &&
              !f.hold && !f.fans_empty && !f.buried_hold, "after: the dealt table");
        CHECK(!memcmp(f.my_hand, v.my_hand, 7), "my seven in order");
    }
}

/* ---- draws ------------------------------------------------------------------------------------ */

/* Grid "Draw x1", played at my tap: 320ms flight (bulge 1.08) + 180ms flip, the
 * strip's chip pulses 120 (snap + 120ms); DEMO.draw1's drawToHand. */
static void t_draw_live(void)
{
    TEST("draw live");
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 3);
    memcpy(PREV, EV, sizeof(PkEvent));
    PkEvent *d = ev(PK_EV_DRAW, A, 3); d->card = 50; d->n = d->i = 1; d->deck_n = 39;
    CHECK(build(PK_BEATS_STAGE, 3, 4, PREV, 1, &B1) == 3, "flight, flip, pulse");
    const PkBeat *f = nth(&B1, PK_BK_FLIGHT, 0, 0), *fl = nth(&B1, PK_BK_FLIP, 0, 0), *p = nth(&B1, PK_BK_PULSE, 0, 0);
    CHECK(st(f) == 16 && du(f) == 320 && f->bulge == 108 && f->from == PK_ANC_DECK && f->to == PK_ANC_HAND &&
          f->to_i == 7 && f->card == 50, "deck -> the right end of my row");
    CHECK(st(fl) == 336 && du(fl) == 180 && fl->to_i == 7, "lands, flips face");
    PkBeatSample sm;
    pk_beat_sample(f, 200, 0, &sm);
    CHECK(!sm.face && f->sub == PK_FLIGHT_BACK, "my own card flies face down");
    CHECK(st(p) == 16 && du(p) == 120 && p->to == PK_ANC_STRIP && p->amp == 8, "the chip pulses 1.08");
    PkBeatFrame fr;
    pk_beats_frame(&B1, 15, &fr);
    CHECK(fr.my_n == 7 && fr.deck_n == 40, "before the beat");
    pk_beats_frame(&B1, 16, &fr);
    CHECK(fr.my_n == 8 && fr.my_unseen[7] && fr.deck_n == 39, "the slot opens and the count ticks as it leaves");
    pk_beats_frame(&B1, 515, &fr);
    CHECK(fr.my_unseen[7], "still turning");
    pk_beats_frame(&B1, 516, &fr);
    CHECK(!fr.my_unseen[7] && fr.my_hand[7] == 50 && fr.done, "seen once it has turned");
}

/* Grid "Draw xN": C/D/E replay the draws 110ms start to start, overlapping;
 * then the play is a step of its own, 25ms after the last one lands. */
static void t_draws_replayed(void)
{
    TEST("draws replayed");
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 1);
    for (int k = 1; k <= 3; k++) { PkEvent *d = ev(PK_EV_DRAW, A, 1); d->card = PK_CARD_HIDDEN; d->n = d->i = (uint8_t)k; d->deck_n = (uint8_t)(40 - k); }
    PkEvent *p = ev(PK_EV_PLAY, A, 1); p->card = 5; p->i = 7; p->deck_n = 37;
    PkEvent *t = ev(PK_EV_TURN_TO, S, 2); t->other = 1;
    ev(PK_EV_BUBBLE_END, A, 1);
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 0)) == 16 && st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 1)) == 126 &&
          st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 2)) == 236, "16, 126, 236");
    CHECK(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 0)->to == PK_ANC_FAN && nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 0)->to_i == 1,
          "into the drawer's fan");
    CHECK(count(&B1, PK_BK_FLIP, 0) == 0, "a seat's draw lands as one more back: no flip");
    const PkBeat *pl = nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0);
    CHECK(st(pl) == 581, "the play 25 after the last draw lands (236 + 320 + 25): %ld", st(pl));
    CHECK(pl->from == PK_ANC_FAN && (pl->flags & PK_BF_FACE_MID) && pl->bulge == 115, "a back out of the fan, face at the midpoint");
    CHECK(st(nth(&B1, PK_BK_TURN_BAR, 0, 0)) == 1106, "the turn bar 25 after the play lands");
    /* channel C: my own bubble reopened, 100ms after the open, each card flips */
    for (int i = 1; i <= 4; i++) EV[i].seat = 3;
    EV[5].other = 3;
    build(PK_BEATS_OPEN, 3, 4, 0, 0, &B1);
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 0)) == 100 && st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 2)) == 320,
          "on open: 100, 210, 320");
    CHECK(count(&B1, PK_BK_FLIP, PK_EV_DRAW) == 3 && st(nth(&B1, PK_BK_FLIP, 0, 2)) == 640, "each turns on landing");
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0)) == 845, "the play after the last flip (320 + 320 + 180 + 25)");
    CHECK(nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0)->from == PK_ANC_HAND &&
          nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0)->from_i == 7, "my own card from its slot");
}

/* Grid "Reshuffle" (U21), DEMO.drawmany: GATHER each under-card 360ms ease-in,
 * 40ms apart; the deck fattens 240 (the count snaps); riffles twice; then the
 * draw that asked for it. And U21's budget: a five-draw turn with a
 * reshuffle still plays in under four seconds. */
static void t_reshuffle(void)
{
    TEST("reshuffle");
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 1);
    for (int k = 1; k <= 3; k++) { PkEvent *d = ev(PK_EV_DRAW, A, 1); d->card = PK_CARD_HIDDEN; d->n = d->i = (uint8_t)k; d->deck_n = (uint8_t)(3 - k); }
    PkEvent *g = ev(PK_EV_RESHUFFLE_GATHER, A, PK_SEAT_NONE); g->n = 40; g->deck_n = 40;
    PkEvent *s = ev(PK_EV_RESHUFFLE_SHUFFLE, A, PK_SEAT_NONE); s->n = 40; s->i = 1; s->deck_n = 40;
    PkEvent *dn = ev(PK_EV_RESHUFFLE_DONE, A, PK_SEAT_NONE); dn->n = 40; dn->deck_n = 40;
    for (int k = 4; k <= 5; k++) { PkEvent *d = ev(PK_EV_DRAW, A, 1); d->card = PK_CARD_HIDDEN; d->n = d->i = (uint8_t)k; d->deck_n = (uint8_t)(44 - k); }
    PkEvent *p = ev(PK_EV_PLAY, A, 1); p->card = 5; p->deck_n = 38;
    PkEvent *t = ev(PK_EV_TURN_TO, S, 2); t->other = 1;
    ev(PK_EV_BUBBLE_END, A, 1);
    build(PK_BEATS_ARRIVAL, 3, 8, 0, 0, &B1);
    const PkBeat *ga = nth(&B1, PK_BK_GATHER, 0, 0);
    CHECK(st(ga) == 346 && ga->parts == 3 && ga->part_ms == 360 && ga->stagger_ms == 40 && du(ga) == 440 &&
          ga->ease == PK_EASE_IN && ga->from == PK_ANC_STACK && ga->to == PK_ANC_DECK,
          "gather where the fourth draw would have gone (236 + 110), three ghosts 40 apart");
    const PkBeat *fa = nth(&B1, PK_BK_FATTEN, 0, 0);
    CHECK(st(fa) == 786 && du(fa) == 240 && fa->deck_n == 40, "fatten when the last ghost lands; the count snaps");
    CHECK(st(nth(&B1, PK_BK_RIFFLE, 0, 0)) == 1026 && st(nth(&B1, PK_BK_RIFFLE, 0, 1)) == 1222, "two riffles");
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 3)) == 1418, "the waiting draw goes as the gag ends");
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 4)) == 1528, "the next 110 after it");
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0)) == 1873, "the play 25 after the fifth lands");
    CHECK(B1.total_ms == 2618 && B1.total_ms < 4000, "U21: a five-draw turn with a reshuffle in under 4s (%u)", B1.total_ms);
    PkBeatFrame f;
    pk_beats_frame(&B1, 345, &f);
    CHECK(f.stack_n == 3 && f.deck_n == 0, "the pile still has its under-cards; the deck is empty");
    pk_beats_frame(&B1, 346, &f);
    CHECK(f.stack_n == 1 && f.deck_n == 0, "the under-cards leave the pile as the gather starts");
    pk_beats_frame(&B1, 786, &f);
    CHECK(f.deck_n == 40, "the new count as the deck fattens");
}

/* ---- plays ------------------------------------------------------------------------------------- */

static void play_by(int seat, int card, int suit)
{
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, seat);
    PkEvent *p = ev(PK_EV_PLAY, A, seat); p->card = (uint8_t)card; p->suit = (uint8_t)suit; p->i = 2;
}

static void turn_to(int seat, int from)
{
    PkEvent *t = ev(PK_EV_TURN_TO, S, seat); t->other = (uint8_t)from;
    ev(PK_EV_BUBBLE_END, A, from);
}

/* DEMO.playnum: playFromSeat (500, the back turns face at 250, lands -3 deg,
 * the halo on landing), sleep(T.gap), turnTo. */
static void t_play_number(void)
{
    TEST("play a number");
    play_by(0, 30, 1);
    turn_to(1, 0);
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    const PkBeat *p = nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0), *h = nth(&B1, PK_BK_HALO, PK_EV_PLAY, 0);
    CHECK(st(p) == 16 && du(p) == 500 && p->bulge == 115 && p->rot0 == 0 && p->rot1 == -3 && p->ease == PK_EASE_FLIGHT,
          "hand -> pile, 500ms, bulge 1.15, lands -3 deg");
    CHECK(st(h) == 516 && du(h) == 220 && h->suit == 1, "the halo cross-fades to the new suit as it lands");
    CHECK(st(nth(&B1, PK_BK_TURN_BAR, 0, 0)) == 541, "sleep(T.gap): the turn bar 25 after");
    PkBeatSample s;
    pk_beat_sample(p, 16 + 249, 0, &s);
    CHECK(!s.face && s.state == PK_BS_ACTIVE, "a back until half time");
    pk_beat_sample(p, 16 + 250, 0, &s);
    CHECK(s.face, "the face from half time");
    /* WAAPI: the keyframe at offset .5 is on the EASED progress, so the bulge
     * peaks early in time, at 1.15 */
    float peak = 0;
    for (uint32_t ms = 16; ms <= 516; ms++) { pk_beat_sample(p, ms, 0, &s); if (s.scale > peak) peak = s.scale; }
    CHECK(peak > 1.148f && peak <= 1.15f + 1e-4f, "the bulge peaks at 1.15: %f", (double)peak);
    pk_beat_sample(p, 516, 0, &s);
    CHECK(s.state == PK_BS_DONE && s.rot == -3 && s.scale == 1, "lands crooked, full size");
    PkBeatFrame f;
    pk_beats_frame(&B1, 515, &f);
    CHECK(f.top == 4 && f.stack_n == 3, "the old top until it lands");
    pk_beats_frame(&B1, 516, &f);
    CHECK(f.top == 30 && f.stack_n == 4 && f.suit == 1 && f.turn == 0, "on landing: the new top and halo; the turn not yet");
    pk_beats_frame(&B1, 541, &f);
    CHECK(f.turn == 1, "the turn bar moves");
}

/* DEMO.skip: playFromSeat; skipSeat (the slash 260 while the badge dims 900);
 * turnTo. The turn bar 25 after the dim (grid "Turn moves"; DECISION A3). */
static void t_play_skip(void)
{
    TEST("play a skip");
    play_by(0, 18, 0);
    PkEvent *k = ev(PK_EV_SKIP, S, 1); k->other = 0;
    turn_to(2, 0);
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    const PkBeat *sl = nth(&B1, PK_BK_SLASH, 0, 0), *dm = nth(&B1, PK_BK_DIM, 0, 0);
    CHECK(st(sl) == 516 && sl->part_ms == 260 && du(sl) == 900 && sl->to == PK_ANC_FAN && sl->to_i == 1,
          "the slash wipes across the skipped fan as the card lands");
    CHECK(st(dm) == 516 && du(dm) == 900 && dm->amp == 45 && dm->to == PK_ANC_SEAT && dm->to_i == 1,
          "the badge dims to .45 and back, together");
    CHECK(st(nth(&B1, PK_BK_TURN_BAR, 0, 0)) == 1441 && nth(&B1, PK_BK_TURN_BAR, 0, 0)->to_i == 2,
          "then the turn bar jumps past it");
    PkBeatSample s;
    float lo = 1;
    for (uint32_t ms = 516; ms <= 1416; ms++) { pk_beat_sample(dm, ms, 0, &s); if (s.opacity < lo) lo = s.opacity; }
    CHECK(lo > .449f && lo < .451f, "down to .45 and no further: %f", (double)lo);
    pk_beat_sample(dm, 1416, 0, &s);
    CHECK(s.opacity == 1, "and back");
    pk_beat_sample(sl, 516 + 600, 0, &s);
    CHECK(s.state == PK_BS_ACTIVE && s.p == 1, "the slash stays across until the dim ends");
}

/* DEMO.reverse: playFromSeat; turnDir (170 in, the word swaps, 170 out); turnTo. */
static void t_play_reverse(void)
{
    TEST("play a reverse");
    play_by(1, 20, 0);
    PkEvent *r = ev(PK_EV_REVERSE, S, PK_SEAT_NONE); r->other = 1; r->dir = PK_DIR_ACW;
    turn_to(0, 1);
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    const PkBeat *t = nth(&B1, PK_BK_TURN, 0, 0);
    CHECK(st(t) == 516 && du(t) == 340 && t->to == PK_ANC_DIR && t->sub == PK_DIR_ACW, "TURN on the direction box");
    CHECK(st(nth(&B1, PK_BK_TURN_BAR, 0, 0)) == 881, "the turn bar 25 after");
    PkBeatFrame f;
    pk_beats_frame(&B1, 685, &f);
    CHECK(f.dir == PK_DIR_CW, "the old word until the box is edge-on");
    pk_beats_frame(&B1, 686, &f);
    CHECK(f.dir == PK_DIR_ACW, "the new word from 170ms");
    PkBeatSample s;
    pk_beat_sample(t, 516 + 169, 0, &s);
    CHECK(s.rot > 85 && !s.face, "rotated out on Y: %f", (double)s.rot);
    pk_beat_sample(t, 516 + 171, 0, &s);
    CHECK(s.rot < -88 && s.face, "and back in");
}

/* DEMO.draw2: playFromSeat; two drawToSeat at 0 and T.drawStep; skipSeat;
 * turnTo. DEMO.w4: the same with four. */
static void t_play_penalty(void)
{
    TEST("play a +2 / +4");
    for (int four = 0; four <= 1; four++) {
        int owed = four ? 4 : 2;
        play_by(0, four ? 100 : 22, 0);
        if (four) { PkEvent *w = ev(PK_EV_WILD_SUIT, A, 0); w->card = 100; w->suit = 2; }
        PkEvent *pe = ev(PK_EV_PENALTY, S, 1); pe->other = 0; pe->n = (uint8_t)owed; pe->i = four ? PK_PEN_WILD4 : PK_PEN_PLUS2;
        for (int k = 1; k <= owed; k++) { PkEvent *d = ev(PK_EV_PENALTY_DRAW, S, 1); d->card = PK_CARD_HIDDEN; d->n = (uint8_t)owed; d->i = (uint8_t)k; }
        turn_to(2, 0);
        build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
        int bad = 0;
        for (int k = 0; k < owed; k++) {
            const PkBeat *d = nth(&B1, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW, k);
            if (st(d) != 516 + 110 * k || du(d) != 320 || d->to != PK_ANC_FAN || d->to_i != 1) bad++;
        }
        CHECK(bad == 0, "%d backs into the victim's fan, 110 apart, as the card lands", owed);
        long last = 516 + 110 * (owed - 1) + 320;
        CHECK(st(nth(&B1, PK_BK_SLASH, 0, 0)) == last && nth(&B1, PK_BK_SLASH, 0, 0)->to_i == 1,
              "then the victim's skip slash (%ld)", last);
        CHECK(st(nth(&B1, PK_BK_TURN_BAR, 0, 0)) == last + 900 + 25, "then the turn bar past them");
        if (four) {
            CHECK(count(&B1, PK_BK_HALO, PK_EV_PLAY) == 0, "a wild lands with its halo clear");
            CHECK(st(nth(&B1, PK_BK_HALO, PK_EV_WILD_SUIT, 0)) == 516 && nth(&B1, PK_BK_HALO, PK_EV_WILD_SUIT, 0)->suit == 2,
                  "WILD_SUIT turns the halo as it lands");
            CHECK(count(&B1, PK_BK_BAND, 0) == 0, "arrival: the band is already on the card");
        }
    }
}

/* DEMO.wild, my own wild (ANIMATION_DECISIONS A12): the card lands, then its
 * chosen suit's band slides up from under its foot, `band.animate(
 * translateY(100%) -> translateY(0), T.fade, E.out)`, while the halo turns. */
static void t_play_wild(void)
{
    TEST("play a wild: the band slides up");
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 3);
    PkEvent *p = ev(PK_EV_PLAY, A, 3); p->card = 96; p->suit = PK_NO_SUIT; p->i = 2;
    PkEvent *w = ev(PK_EV_WILD_SUIT, A, 3); w->card = 96; w->suit = 2;
    build(PK_BEATS_STAGE, 3, 4, 0, 0, &B1);
    const PkBeat *band = nth(&B1, PK_BK_BAND, 0, 0);
    CHECK(st(band) == 516 && du(band) == 220 && band->suit == 2 && band->to == PK_ANC_STACK,
          "the band as the wild lands, T.fade long, in the chosen suit");
    CHECK(band && band->ease == PK_EASE_EASE_OUT, "E.out");
    PkBeatSample s;
    pk_beat_sample(band, 515, 0, &s);
    CHECK(s.state == PK_BS_PENDING && s.apply && s.p == 0, "hidden under the card's edge before it starts");
    pk_beat_sample(band, 516 + 110, 0, &s);
    CHECK(s.state == PK_BS_ACTIVE && s.p > .5f && s.p < 1, "ease-out: past halfway at half time (%f)", (double)s.p);
    CHECK(s.opacity == 1, "it slides, it does not fade");
    pk_beat_sample(band, 736, 0, &s);
    CHECK(s.state == PK_BS_DONE && s.p == 1, "in place at the end");
    /* the arrival of someone else's wild has its band already on (grid) */
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 0);
    p = ev(PK_EV_PLAY, A, 0); p->card = 96; p->suit = PK_NO_SUIT; p->i = 2;
    w = ev(PK_EV_WILD_SUIT, A, 0); w->card = 96; w->suit = 2;
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    CHECK(count(&B1, PK_BK_BAND, 0) == 0, "a seat's wild arrives with its band on");
}

/* Grid "Pass": the passer's fan shrugs .96 and back, 200ms; mine dims to .5. */
static void t_pass(void)
{
    TEST("pass");
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 0);
    PkEvent *p = ev(PK_EV_PASS, A, 0); p->n = 1;
    turn_to(1, 0);
    NE--;                                           /* a draft has no BUBBLE_END */
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    const PkBeat *s = nth(&B1, PK_BK_SHRUG, 0, 0);
    CHECK(st(s) == 16 && du(s) == 200 && s->amp == 4 && s->to_i == 0, "the shrug");
    CHECK(st(nth(&B1, PK_BK_TURN_BAR, 0, 0)) == 241, "the turn bar after it");
    EV[1].seat = 3;
    build(PK_BEATS_STAGE, 3, 4, EV, 1, &B1);
    const PkBeat *d = nth(&B1, PK_BK_FADE, PK_EV_PASS, 0);
    CHECK(st(d) == 16 && du(d) == 220 && d->to == PK_ANC_HAND && d->amp == 50 && d->sub == 0, "my hand dims to .5, all at once");
    CHECK(B1.held == 1, "the turn bar is held until Send");
}

/* Grid "Say it" and "Call-out" / "Call-out penalty"; DEMO.sayit's stampSlot,
 * DEMO.callout's stampSlot then two drawToSeat at 0 and 110. */
static void t_say_and_catch(void)
{
    TEST("say it, catch");
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 1);
    ev(PK_EV_SAY_IT, A, 1);
    ev(PK_EV_BUBBLE_END, A, 1);
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    const PkBeat *s = nth(&B1, PK_BK_STAMP, 0, 0);
    CHECK(st(s) == 16 && du(s) == 340 && s->sub == PK_STAMP_LAST && s->to == PK_ANC_SLOT && s->to_i == 1 &&
          s->ease == PK_EASE_STAMP, "LAST slams into the sayer's slot");
    PkBeatSample x;
    pk_beat_sample(s, 16, 0, &x);
    CHECK(x.scale > 2.39f && x.opacity == 0, "from 2.4x, unseen");
    PkBeatFrame f;
    pk_beats_frame(&B1, 0, &f);
    CHECK(f.stamp_hold == 2, "the stamp waits for its beat");
    pk_beats_frame(&B1, 16, &f);
    CHECK(f.stamp_hold == 0, "and shows as it slams");

    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 2);
    PkEvent *c = ev(PK_EV_CALL_OUT, A, 1); c->other = 2;
    ev(PK_EV_CALL_HIT, S, 1)->other = 2;
    PkEvent *pe = ev(PK_EV_PENALTY, S, 1); pe->n = 2; pe->i = PK_PEN_CAUGHT;
    for (int k = 1; k <= 2; k++) { PkEvent *d = ev(PK_EV_PENALTY_DRAW, S, 1); d->card = PK_CARD_HIDDEN; d->n = 2; d->i = (uint8_t)k; }
    ev(PK_EV_BUBBLE_END, A, 2);
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    const PkBeat *r = nth(&B1, PK_BK_RING, 0, 0), *h = nth(&B1, PK_BK_STAMP, 0, 0);
    CHECK(st(r) == 16 && du(r) == 120 && r->to == PK_ANC_FAN && r->to_i == 1, "the called fan rings");
    CHECK(st(h) == 161 && h->sub == PK_STAMP_CAUGHT && h->to_i == 1, "at seal, Caught you! slams in");
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW, 0)) == 501 && st(nth(&B1, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW, 1)) == 611,
          "then the two cards, 110 apart");
    CHECK(count(&B1, PK_BK_SLASH, 0) == 0, "a catch skips nobody");
    EV[2].kind = PK_EV_CALL_MISS; EV[2].seat = 2; EV[2].other = 1;
    for (int k = 3; k <= 5; k++) EV[k].seat = 2;
    EV[3].n = 1; EV[4].n = 1; EV[4].i = 1; NE = 5;
    ev(PK_EV_BUBBLE_END, A, 2);
    build(PK_BEATS_ARRIVAL, 3, 4, 0, 0, &B1);
    h = nth(&B1, PK_BK_STAMP, 0, 0);
    CHECK(st(h) == 161 && h->sub == PK_STAMP_WRONG && h->to_i == 2, "a wrong call: grey under the caller");
    CHECK(count(&B1, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW) == 1 && nth(&B1, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW, 0)->to_i == 2,
          "and the caller draws one");
}

/* DEMO.win: playFromSeat; stampSlot OUT; the direction fades; reveal(seat 0)
 * then reveal(seat 2), each card's flip 160 and 60ms apart; sleep(gameOver);
 * the results fade in. */
static void t_win_reveal(void)
{
    TEST("win and reveal");
    for (int viewer = 3; viewer >= 0; viewer -= 3) {
        play_by(1, 30, 1);
        PkEvent *w = ev(PK_EV_WIN, S, 1); w->card = 30; w->i = PK_OVER_OUT;
        for (int k = 1; k <= 3; k++) { PkEvent *r = ev(PK_EV_REVEAL, S, 0); r->card = (uint8_t)k; r->n = 3; r->i = (uint8_t)k; }
        for (int k = 1; k <= 5; k++) { PkEvent *r = ev(PK_EV_REVEAL, S, 2); r->card = (uint8_t)(10 + k); r->n = 5; r->i = (uint8_t)k; }
        ev(PK_EV_BUBBLE_END, A, 1);
        build(PK_BEATS_ARRIVAL, viewer, 4, 0, 0, &B1);
        const PkBeat *o = nth(&B1, PK_BK_STAMP, 0, 0);
        CHECK(st(o) == 516 && o->sub == PK_STAMP_OUT && o->to_i == 1, "OUT as the last card lands");
        CHECK(st(nth(&B1, PK_BK_FADE, PK_EV_WIN, 0)) == 516 && nth(&B1, PK_BK_FADE, PK_EV_WIN, 0)->sub == 0,
              "the direction fades out with it");
        int first = viewer == 3 ? 0 : 3;               /* my own hand is face up already */
        int n = viewer == 3 ? 8 : 5, bad = 0;
        for (int k = 0; k < n; k++) {
            const PkBeat *r = nth(&B1, PK_BK_FLIP, PK_EV_REVEAL, k);
            if (st(r) != 856 + 60 * k || du(r) != 160) bad++;
        }
        CHECK(bad == 0 && count(&B1, PK_BK_FLIP, PK_EV_REVEAL) == n, "viewer %d: %d flips from 856, 60 apart", viewer, n);
        CHECK(nth(&B1, PK_BK_FLIP, PK_EV_REVEAL, 0)->to_i == 0 &&
              nth(&B1, PK_BK_FLIP, PK_EV_REVEAL, n - 1)->to_i == 4, "card by card");
        (void)first;
        long last = 856 + 60 * (n - 1);
        const PkBeat *res = nth(&B1, PK_BK_FADE, 0, 1);
        CHECK(res && res->to == PK_ANC_RESULTS && st(res) == last + 60 + 1000 && du(res) == 220,
              "the results fade in a game-over hold after the reveal");
        PkBeatFrame f;
        pk_beats_frame(&B1, 516, &f);
        CHECK(f.turn == PK_SEAT_NONE && (f.hold & PK_HOLD_RESULTS), "every turn bar goes at OUT");
        pk_beats_frame(&B1, 856 + 80, &f);
        CHECK(f.revealing && f.reveal_shown[viewer == 3 ? 0 : 2] == 1, "the first back is face up at its midpoint");
        pk_beats_frame(&B1, B1.total_ms, &f);
        CHECK(!(f.hold & PK_HOLD_RESULTS) && f.reveal_shown[2] == 5 && f.done, "all shown, the results up");
    }
}

/* ---- the cut (5.3.6) ----------------------------------------------------------------------- */

/* A +2 I play: at stage only the flight (the penalty, the skip and the turn
 * bar are held); at Send the rest plays from the 16ms beat. Every event plays
 * exactly once across the two. */
static void t_cut(void)
{
    TEST("the cut");
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 3);
    int prev_n = NE;
    memcpy(PREV, EV, sizeof(PkEvent) * (size_t)prev_n);
    PkEvent *p = ev(PK_EV_PLAY, A, 3); p->card = 22; p->i = 2;
    PkEvent *pe = ev(PK_EV_PENALTY, S, 0); pe->other = 3; pe->n = 2; pe->i = PK_PEN_PLUS2;
    for (int k = 1; k <= 2; k++) { PkEvent *d = ev(PK_EV_PENALTY_DRAW, S, 0); d->card = PK_CARD_HIDDEN; d->n = 2; d->i = (uint8_t)k; d->deck_n = (uint8_t)(40 - k); }
    PkEvent *t = ev(PK_EV_TURN_TO, S, 1); t->other = 3;
    build(PK_BEATS_STAGE, 3, 4, PREV, prev_n, &B1);
    CHECK(B1.held == 4, "the penalty, its two cards and the turn are held: %d", B1.held);
    CHECK(count(&B1, PK_BK_FLIGHT, PK_EV_PLAY) == 1 && count(&B1, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW) == 0,
          "at stage: only the flight");
    CHECK(nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0)->from == PK_ANC_HAND && (nth(&B1, PK_BK_FLIGHT, PK_EV_PLAY, 0)->flags & PK_BF_LEAVES),
          "out of my hand");
    CHECK(B1.settle_ms == 250 + 736 + 500, "the drawer may collapse 250 + the plan + 500 later (%u)", B1.settle_ms);
    PkBeatFrame f;
    pk_beats_frame(&B1, B1.total_ms, &f);
    CHECK(f.deck_n == 40 && f.turn == 0 && f.my_n == 6 && f.top == 22, "held: no penalty count, no turn move");
    ev(PK_EV_BUBBLE_END, A, 3);
    build(PK_BEATS_SEND, 3, 4, 0, 0, &B2);
    CHECK(B2.held == 0 && count(&B2, PK_BK_FLIGHT, PK_EV_PLAY) == 0, "at Send the play is done");
    CHECK(st(nth(&B2, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW, 0)) == 16 && st(nth(&B2, PK_BK_FLIGHT, PK_EV_PENALTY_DRAW, 1)) == 126,
          "the penalty from the 16ms beat");
    CHECK(st(nth(&B2, PK_BK_SLASH, 0, 0)) == 446 && st(nth(&B2, PK_BK_TURN_BAR, 0, 0)) == 1371, "then the skip and the turn");
    pk_beats_frame(&B2, 0, &f);
    CHECK(f.top == 22 && f.my_n == 6 && f.deck_n == 40, "Send starts from the staged board");
    pk_beats_frame(&B2, B2.total_ms, &f);
    CHECK(f.deck_n == 38 && f.turn == 1, "and ends on the settled one");

    /* 2 players: a Reverse is a Skip and I go on (D7). The held settle is
     * released when I continue, before my next turn's draw. */
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 1);
    p = ev(PK_EV_PLAY, A, 1); p->card = 20; p->i = 0;
    ev(PK_EV_REVERSE_AS_SKIP, S, 0)->other = 1;
    t = ev(PK_EV_TURN_TO, S, 1); t->other = 1;
    prev_n = NE;
    memcpy(PREV, EV, sizeof(PkEvent) * (size_t)prev_n);
    PkEvent *d = ev(PK_EV_DRAW, A, 1); d->card = 60; d->n = d->i = 1;
    build(PK_BEATS_STAGE, 1, 2, PREV, prev_n, &B1);
    CHECK(count(&B1, PK_BK_FLIGHT, PK_EV_PLAY) == 0, "the reverse already flew");
    CHECK(st(nth(&B1, PK_BK_SLASH, 0, 0)) == 16 && st(nth(&B1, PK_BK_TURN_BAR, 0, 0)) == 941,
          "released now: the skip, then the turn");
    CHECK(st(nth(&B1, PK_BK_FLIGHT, PK_EV_DRAW, 0)) == 1186, "then the draw of my next turn");
    CHECK(B1.held == 0, "nothing left to hold");

    /* A Last card! lands at the FRONT of the draft (5.3.3): the draw before it
     * must not fly again. */
    NE = 0;
    ev(PK_EV_BUBBLE_BEGIN, A, 3);
    d = ev(PK_EV_DRAW, A, 3); d->card = 60; d->n = d->i = 1;
    prev_n = NE;
    memcpy(PREV, EV, sizeof(PkEvent) * (size_t)prev_n);
    NE = 1;
    ev(PK_EV_SAY_IT, A, 3);
    d = ev(PK_EV_DRAW, A, 3); d->card = 60; d->n = d->i = 1;
    build(PK_BEATS_STAGE, 3, 4, PREV, prev_n, &B1);
    CHECK(B1.n == 0, "a Last card! after a draw moves nothing and replays nothing (%d beats)", B1.n);
    pk_beats_frame(&B1, 0, &f);
    CHECK(f.my_n == 8 && !f.my_unseen[7], "the drawn card is simply there");
}

/* ---- host motions ------------------------------------------------------------------------ */

static void t_host(void)
{
    TEST("host motions");
    PkBeatFrame f = board(7);
    /* DEMO.undo: sleep(T.beat); fly(g, slot, T.flight, { r0: -3, bulge: 1 }) */
    CHECK(pk_beats_host(PK_HM_UNDO, 30, 6, &f, 3, 4, 0, &B1) == 2, "flight and halo");
    const PkBeat *u = nth(&B1, PK_BK_FLIGHT, 0, 0);
    CHECK(st(u) == 16 && du(u) == 500 && u->bulge == 100 && u->rot0 == -3 && u->rot1 == 0 &&
          u->from == PK_ANC_STACK && u->to == PK_ANC_HAND && u->to_i == 6 && !(u->flags & PK_BF_RETRACT),
          "after the 16ms beat, back to its slot, no bulge");
    PkBeatFrame g;
    pk_beats_frame(&B1, 16, &g);
    CHECK(g.my_unseen[6] && g.my_n == 7, "its slot is open while it flies");
    pk_beats_frame(&B1, 516, &g);
    CHECK(!g.my_unseen[6], "and it is home");
    /* DEMO.undodraw: three shakes, T.shake x 3, linear, 0 -5 5 -4 4 0 */
    pk_beats_host(PK_HM_REFUSED, 0, 7, &f, 3, 4, 0, &B1);
    const PkBeat *s = nth(&B1, PK_BK_SHAKE, 0, 0);
    CHECK(st(s) == 16 && du(s) == 180 && s->amp == 5 && s->to_i == 7 && s->ease == PK_EASE_LINEAR, "three 60ms shakes");
    PkBeatSample x;
    pk_beat_sample(s, 16 + 36, 0, &x);
    CHECK(x.dx < -4.99f && x.dx > -5.01f, "-5 at a fifth: %f", (double)x.dx);
    pk_beat_sample(s, 16 + 72, 0, &x);
    CHECK(x.dx > 4.99f, "+5 at two fifths");
    /* DEMO.wild: fly to the pile face up; scrim fades in; tiles pop 260 spring, 30 apart */
    pk_beats_host(PK_HM_PICKER_OPEN, 96, 5, &f, 3, 4, 0, &B1);
    const PkBeat *w = nth(&B1, PK_BK_FLIGHT, 0, 0), *pop = nth(&B1, PK_BK_POP, 0, 0);
    CHECK(st(w) == 16 && du(w) == 500 && w->from_i == 5 && w->card == 96 && w->bulge == 115, "the wild flies face up");
    CHECK(st(nth(&B1, PK_BK_FADE, 0, 0)) == 516 && nth(&B1, PK_BK_FADE, 0, 0)->to == PK_ANC_SCRIM, "the scrim");
    CHECK(st(pop) == 516 && pop->parts == 5 && pop->part_ms == 260 && pop->stagger_ms == 30 && pop->ease == PK_EASE_SPRING,
          "five tiles pop, 30ms apart");
    /* fill backwards: a tile is at scale 0 before its pop (UI.html `fill: 'backwards'`) */
    pk_beat_sample(pop, 0, 4, &x);
    CHECK(x.state == PK_BS_PENDING && x.apply && x.scale == 0 && x.opacity == 0, "a tile waits unseen");
    pk_beat_sample(nth(&B1, PK_BK_FLIGHT, 0, 0), 0, 0, &x);
    CHECK(!x.apply, "a flight that has not begun is not drawn");
    pk_beats_frame(&B1, 0, &g);
    CHECK((g.hold & PK_HOLD_PENDING), "the wild is not on the pile yet");
    pk_beats_frame(&B1, 516, &g);
    CHECK(!(g.hold & PK_HOLD_PENDING), "now it is");
    /* the tap: the tile rings, the tiles collapse 200 ease-in */
    pk_beats_host(PK_HM_PICKER_PICK, 1, 0, &f, 3, 4, 0, &B1);
    CHECK(st(nth(&B1, PK_BK_RING, 0, 0)) == 16 && st(nth(&B1, PK_BK_COLLAPSE, 0, 0)) == 136 &&
          du(nth(&B1, PK_BK_COLLAPSE, 0, 0)) == 200, "ring 120, then collapse 200");
    /* fill forwards: the collapsed tiles stay gone until the picker is taken down */
    pk_beat_sample(nth(&B1, PK_BK_COLLAPSE, 0, 0), 5000, 0, &x);
    CHECK(x.state == PK_BS_DONE && x.apply && x.opacity == 0, "collapsed tiles stay gone");
    /* the conflict ghost is the undo flight in red */
    pk_beats_host(PK_HM_RETRACT, 30, 6, &f, 3, 4, 0, &B1);
    CHECK((nth(&B1, PK_BK_FLIGHT, 0, 0)->flags & PK_BF_RETRACT) && du(nth(&B1, PK_BK_FLIGHT, 0, 0)) == 500, "retraction ghost");
    pk_beats_delay(&B1, 0, 100);
    CHECK(st(nth(&B1, PK_BK_FLIGHT, 0, 0)) == 116, "delay shifts a plan");
}

/* ---- every real bubble -------------------------------------------------------------------- */

static int decoration(const PkBeat *b)
{
    return b->kind == PK_BK_HALO || b->kind == PK_BK_BAND || b->kind == PK_BK_PULSE ||
           (b->kind == PK_BK_FADE && b->to == PK_ANC_DIR);
}

/* The grid's sequencing: a turn bar never starts before every motion of its
 * turn is over (+25), a play never before the draws ahead of it land (+25),
 * a reveal never before OUT, the results never before the reveal; and the
 * board at the end of a plan is the settled view. */
static int sequenced(const PkBeats *b, char *why)
{
    uint32_t moved = 0;
    int any = 0;
    for (int i = 0; i < b->n; i++) {
        const PkBeat *x = &b->beat[i];
        if ((x->kind == PK_BK_TURN_BAR || (x->kind == PK_BK_FLIGHT && x->ev_kind == PK_EV_PLAY)) && any &&
            x->start_ms < moved + PK_T_GAP) { snprintf(why, 128, "beat %d (%d) at %u before %u + 25", i, x->ev_kind, x->start_ms, moved); return 0; }
        if (x->kind == PK_BK_FLIP && x->ev_kind == PK_EV_REVEAL) {
            const PkBeat *o = 0;
            for (int j = 0; j < i; j++) if (b->beat[j].kind == PK_BK_STAMP && b->beat[j].sub == PK_STAMP_OUT) o = &b->beat[j];
            if (o && x->start_ms < beat_end_of(o)) { snprintf(why, 128, "a reveal before OUT"); return 0; }
        }
        if (!decoration(x) && x->kind != PK_BK_FLIP) {
            uint32_t e = x->start_ms + x->dur_ms;
            if (x->ev_kind != PK_EV_DEAL && e > moved) moved = e;
        }
        if (x->kind == PK_BK_FLIP && x->ev_kind != PK_EV_REVEAL && x->start_ms + x->dur_ms > moved)
            moved = x->start_ms + x->dur_ms;
        if (x->ev_kind != PK_EV_DEAL) any = 1;
    }
    return 1;
}

static int same_board(const PkBeatFrame *f, const PkView *v, char *why)
{
    if (f->deck_n != v->deck_n) { snprintf(why, 128, "deck %d vs %d", f->deck_n, v->deck_n); return 0; }
    if (f->top != v->top) { snprintf(why, 128, "top %d vs %d", f->top, v->top); return 0; }
    if (f->dir != v->dir) { snprintf(why, 128, "dir"); return 0; }
    if (!v->over && f->turn != v->turn) { snprintf(why, 128, "turn %d vs %d", f->turn, v->turn); return 0; }
    if (v->over && f->turn != PK_SEAT_NONE) { snprintf(why, 128, "a turn bar at the end"); return 0; }
    if (f->my_n != v->my_n || memcmp(f->my_hand, v->my_hand, v->my_n)) { snprintf(why, 128, "hand"); return 0; }
    for (int i = 0; i < f->my_n; i++) if (f->my_unseen[i]) { snprintf(why, 128, "card %d unseen", i); return 0; }
    if ((f->hold & ~(v->over ? PK_HOLD_DIR : 0)) || f->stamp_hold || f->fans_empty || f->buried_hold) {
        snprintf(why, 128, "still holding %x %x %x %x", f->hold, f->stamp_hold, f->fans_empty, f->buried_hold);
        return 0;
    }
    if (!f->done) { snprintf(why, 128, "not done"); return 0; }
    return 1;
}

/* A played-out game, bubble by bubble, at every table size: the arrival plan
 * of each bubble for a random viewer ends on that viewer's settled view and
 * keeps the grid's order; and the sender's own taps (A) plus Send (B) play
 * every event the opened bubble (C) plays, exactly once. Also collects the
 * arrival lengths for the budget. */
static uint32_t LEN[8][4000];
static int LEN_N[8];

static void t_real_games(int games)
{
    TEST("real games");
    static PkGame g, h;
    int fails = 0, plans = 0;
    char why[128];
    for (int k = 0; k < games && fails < 5; k++) {
        uint8_t seed[32];
        seed_wide(seed, 90000u + (uint32_t)k);
        int n = 2 + k % 7;
        pk__new(&g, seed, n, 0, 0);
        RS = 0x1234567ull + (uint64_t)(unsigned)k;
        /* the sender's view of the draft as it grows: A at each action */
        int prev_n = 0, a_counts[PK_BK_COUNT] = { 0 };
        for (int step = 0; step < 3000 && !(g.over && !g.b_open); step++) {
            int was_open = g.b_open, before = g.bubbles;
            if (!bot_step(&g)) break;
            if (g.bubbles == before) {
                /* an action on the open draft: channel A for its sender */
                int s = g.b_sender;
                if (!was_open) prev_n = 0;
                int m = pk_plan_draft(&g, s, EV, EV_CAP);
                PkBeatFrame f;
                pk_beats_pre(&g, s, g.bubbles, &f);
                if (pk_beats_build(EV, m, &f, s, n, PK_BEATS_STAGE, PREV, prev_n, 0, &B1) < 0) { fails++; break; }
                for (int i = 0; i < B1.n; i++) if (B1.beat[i].kind != PK_BK_PULSE) a_counts[B1.beat[i].kind]++;
                memcpy(PREV, EV, sizeof(PkEvent) * (size_t)m);
                prev_n = m;
                continue;
            }
            /* a seal: B for the sender, then C and E */
            int s = 0;
            for (int i = g.hist_n - 1; i >= 0; i--)
                if (g.hist[i].kind == PK_A_BUBBLE) { s = g.hist[i].a; break; }
            int m = pk_plan(&g, s, g.bubbles - 1, g.bubbles, EV, EV_CAP);
            PkBeatFrame f;
            pk_beats_pre(&g, s, g.bubbles - 1, &f);
            pk_beats_build(EV, m, &f, s, n, PK_BEATS_SEND, 0, 0, 0, &B2);
            for (int i = 0; i < B2.n; i++) a_counts[B2.beat[i].kind]++;
            PkView v;
            PkBeatFrame e;
            pk_beats_frame(&B2, B2.total_ms, &e);
            pk_view(&g, s, &v);
            if (!same_board(&e, &v, why)) { fails++; CHECK(0, "game %d bubble %d: A + B end %s", k, g.bubbles, why); }
            pk_beats_build(EV, m, &f, s, n, PK_BEATS_OPEN, 0, 0, 0, &B1);
            int c_counts[PK_BK_COUNT] = { 0 };
            for (int i = 0; i < B1.n; i++) c_counts[B1.beat[i].kind]++;
            if (memcmp(a_counts, c_counts, sizeof a_counts)) {
                int kd = 0;
                while (kd < PK_BK_COUNT && a_counts[kd] == c_counts[kd]) kd++;
                /* a wild the sender played shows its band at stage, not on open */
                if (!(kd == PK_BK_BAND)) {
                    fails++;
                    CHECK(0, "game %d bubble %d: kind %d plays %d times at A + B, %d on open", k, g.bubbles, kd,
                          a_counts[kd], c_counts[kd]);
                }
            }
            memset(a_counts, 0, sizeof a_counts);
            prev_n = 0;
            /* E, for a random watcher */
            int w = (int)rnd((uint32_t)n);
            m = pk_plan(&g, w, g.bubbles - 1, g.bubbles, EV, EV_CAP);
            pk_beats_pre(&g, w, g.bubbles - 1, &f);
            if (pk_beats_build(EV, m, &f, w, n, PK_BEATS_ARRIVAL, 0, 0, 0, &B1) < 0) { fails++; continue; }
            plans++;
            pk_beats_frame(&B1, B1.total_ms, &e);
            pk_view(&g, w, &v);
            if (!same_board(&e, &v, why)) { fails++; CHECK(0, "game %d bubble %d viewer %d: %s", k, g.bubbles, w, why); }
            if (!sequenced(&B1, why)) { fails++; CHECK(0, "game %d bubble %d: %s", k, g.bubbles, why); }
            if (!g.over && LEN_N[n - 1] < 4000) LEN[n - 1][LEN_N[n - 1]++] = B1.total_ms;
        }
        (void)h;
    }
    CHECK(fails == 0, "%d plans", plans);
    CHECK(plans > 1000, "enough bubbles were laid out: %d", plans);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* U21's budget for a turn ("a five-draw turn still plays in under four
 * seconds"), held at the 99th percentile of every 8-player bubble an arrival
 * plays; and U20's for the deal at every table size. */
static void t_budget(void)
{
    TEST("budget");
    for (int n = 2; n <= 8; n++) {
        if (!LEN_N[n - 1]) continue;
        qsort(LEN[n - 1], (size_t)LEN_N[n - 1], sizeof(uint32_t), cmp_u32);
        uint32_t p50 = LEN[n - 1][LEN_N[n - 1] / 2], p99 = LEN[n - 1][LEN_N[n - 1] * 99 / 100];
        printf("  %dp arrival: p50 %u ms, p99 %u ms, max %u ms (%d bubbles)\n", n, p50, p99,
               LEN[n - 1][LEN_N[n - 1] - 1], LEN_N[n - 1]);
        if (n == 8) CHECK(p99 < 4000, "8 players: the p99 bubble plays in under 4s (%u)", p99);
    }
    static PkGame g;
    for (int n = 2; n <= 8; n++)
        for (int viewer = 0; viewer < n; viewer++) {
            uint8_t seed[32];
            seed_wide(seed, 500u + (uint32_t)n);
            pk__new(&g, seed, n, 0, 0);
            int m = pk_plan(&g, viewer, -1, 0, EV, EV_CAP);
            pk_beats_build(EV, m, 0, viewer, n, PK_BEATS_OPEN, 0, 0, 0, &B1);
            uint32_t from = (uint32_t)st(nth(&B1, PK_BK_RIFFLE, 0, 0)), to = 0;
            for (int i = 0; i < B1.n; i++)
                if (B1.beat[i].ev_kind == PK_EV_DEAL && B1.beat[i].start_ms + B1.beat[i].dur_ms > to)
                    to = B1.beat[i].start_ms + B1.beat[i].dur_ms;
            CHECK(to - from <= 4200, "%d seats, viewer %d: the shuffle and deal in %u ms", n, viewer, to - from);
        }
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 400;
    t_vocabulary();
    t_deal_step();
    t_deal_row();
    t_draw_live();
    t_draws_replayed();
    t_reshuffle();
    t_play_number();
    t_play_skip();
    t_play_reverse();
    t_play_penalty();
    t_play_wild();
    t_pass();
    t_say_and_catch();
    t_win_reveal();
    t_cut();
    t_host();
    t_real_games(games);
    t_budget();
    return report("pk_beats_test");
}

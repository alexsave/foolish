/* Pick 'Em Up - the plan on a clock. See pk_beats.h, and UI.html's motion grid
 * for every rule below: each case names the grid row it plays. */
#include "pk_beats.h"
#include "pk_internal.h"
#include <string.h>

/* ---- curves ---------------------------------------------------------------- */

static const float BEZ[PK_EASE_COUNT][4] = {
    [PK_EASE_LINEAR]   = { 0, 0, 1, 1 },
    [PK_EASE_FLIGHT]   = { .25f, .46f, .45f, .94f },
    [PK_EASE_SPRING]   = { .3f, 1.22f, .45f, 1 },
    [PK_EASE_STAMP]    = { .2f, 1.5f, .5f, 1 },
    [PK_EASE_IN]       = { .55f, 0, .75f, .2f },
    [PK_EASE_OUT]      = { .25f, .8f, .45f, 1 },
    [PK_EASE_EASE_OUT] = { 0, 0, .58f, 1 },
};

static float bez1(float a, float b, float t)
{
    float u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static float bez1_d(float a, float b, float t)
{
    float u = 1 - t;
    return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
}

float pk_ease(int ease, float x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    if (ease <= PK_EASE_LINEAR || ease >= PK_EASE_COUNT) return x;
    const float *c = BEZ[ease];
    /* x(t) = x: Newton from t = x, then bisection if it wandered */
    float t = x;
    for (int i = 0; i < 8; i++) {
        float d = bez1_d(c[0], c[2], t);
        if (d > -1e-5f && d < 1e-5f) break;
        t -= (bez1(c[0], c[2], t) - x) / d;
    }
    float e = bez1(c[0], c[2], t) - x;
    if (t < 0 || t > 1 || e > 1e-4f || e < -1e-4f) {
        float lo = 0, hi = 1;
        t = x;
        for (int i = 0; i < 30; i++) {
            float v = bez1(c[0], c[2], t);
            if (v < x) lo = t; else hi = t;
            t = (lo + hi) / 2;
        }
    }
    return bez1(c[1], c[3], t);
}

/* ---- U20 ------------------------------------------------------------------------ */

int pk_beats_deal_step(int cards)
{
    if (cards < 1) return PK_T_DEAL_MAX;
    if (cards * PK_T_DEAL_MAX <= PK_T_DEAL_SPREAD) return PK_T_DEAL_MAX;
    if (cards * PK_T_DEAL_MIN >= PK_T_DEAL_SPREAD) return PK_T_DEAL_MIN;
    return PK_T_DEAL_SPREAD / cards;
}

/* i x SPREAD / cards, not i x (SPREAD / cards): the demo's float step, to the
 * millisecond, with no drift down a 56-card deal. */
int pk_beats_deal_at(int i, int cards)
{
    if (cards < 1) return 0;
    if (cards * PK_T_DEAL_MAX <= PK_T_DEAL_SPREAD) return i * PK_T_DEAL_MAX;
    if (cards * PK_T_DEAL_MIN >= PK_T_DEAL_SPREAD) return i * PK_T_DEAL_MIN;
    return i * PK_T_DEAL_SPREAD / cards;
}

/* ---- the board at t = 0 ------------------------------------------------------ */

void pk_beats_frame_of(const PkView *v, int n_seats, PkBeatFrame *out)
{
    memset(out, 0, sizeof *out);
    out->next_ms = PK_BEAT_NEVER;
    if (!v) {
        out->deck_n = PK_DECK;
        out->top = PK_CARD_NONE;
        out->suit = PK_NO_SUIT;
        out->dir = PK_DIR_CW;
        out->turn = PK_SEAT_NONE;
        (void)n_seats;
        return;
    }
    out->deck_n = v->deck_n;
    out->top = v->top;
    out->suit = v->live_suit;
    out->dir = v->dir;
    out->turn = v->over ? PK_SEAT_NONE : v->turn;
    out->stack_n = v->stack_n;
    out->my_n = v->my_n;
    memcpy(out->my_hand, v->my_hand, v->my_n);
}

int pk_beats_pre(const PkGame *g, int viewer, int from, PkBeatFrame *out)
{
    static PkGame tmp;                 /* 17KB: not on the stack of a host thread */
    if (from < 0) { pk_beats_frame_of(0, g->n, out); return 1; }
    int k = g->hist_n, seen = 0;
    for (int i = 0; i < g->hist_n; i++)
        if (g->hist[i].kind == PK_A_BUBBLE) {
            if (seen == from) { k = i; break; }
            seen++;
        }
    if (!pk__replay(&tmp, g, k, 0)) return 0;
    PkView v;
    pk_view(&tmp, viewer, &v);
    pk_beats_frame_of(&v, g->n, out);
    return 1;
}

/* ---- what a beat does to the board ---------------------------------------------- */

enum { EDGE_START = 1, EDGE_MID = 2, EDGE_END = 4 };

static void hand_insert(PkBeatFrame *f, int at, uint8_t card)
{
    if (f->my_n >= PK_HAND_CAP) return;
    if (at < 0 || at > f->my_n) at = f->my_n;
    for (int i = f->my_n; i > at; i--) {
        f->my_hand[i] = f->my_hand[i - 1];
        f->my_unseen[i] = f->my_unseen[i - 1];
    }
    f->my_hand[at] = card;
    f->my_unseen[at] = 1;
    f->my_n++;
}

static void hand_remove(PkBeatFrame *f, int at)
{
    if (at < 0 || at >= f->my_n) return;
    for (int i = at; i + 1 < f->my_n; i++) {
        f->my_hand[i] = f->my_hand[i + 1];
        f->my_unseen[i] = f->my_unseen[i + 1];
    }
    f->my_n--;
    f->my_hand[f->my_n] = 0;
    f->my_unseen[f->my_n] = 0;
}

static uint8_t bit(int s) { return (uint8_t)(s >= 0 && s < 8 ? 1u << s : 0); }

static void commit(PkBeatFrame *f, const PkBeat *b, int edges)
{
    if (edges & EDGE_START) {
        if (b->deck_n != PK_BEAT_NO_DECK) f->deck_n = b->deck_n;
        if (b->flags & PK_BF_LEAVES) hand_remove(f, b->from_i);
        if (b->flags & PK_BF_ADDS) hand_insert(f, b->to_i, b->card);
        if ((b->flags & PK_BF_HIDES) && b->to_i < f->my_n) f->my_unseen[b->to_i] = 1;
        switch (b->kind) {
        case PK_BK_FLIGHT:
            if (b->to == PK_ANC_BURY) {          /* the rejected start card leaves the pile */
                f->top = PK_CARD_NONE;
                if (f->stack_n) f->stack_n--;
            }
            break;
        case PK_BK_GATHER:   if (f->stack_n > 1) f->stack_n = 1; break;
        case PK_BK_HALO:     f->suit = b->suit; break;
        case PK_BK_TURN_BAR: f->turn = b->to_i; break;
        case PK_BK_STAMP:
            f->stamp_hold &= (uint8_t)~bit(b->seat);
            if (b->sub == PK_STAMP_OUT) f->turn = PK_SEAT_NONE;   /* every turn bar goes */
            break;
        case PK_BK_FADE:
            if (b->sub) {
                if (b->to == PK_ANC_DIR) f->hold &= (uint8_t)~PK_HOLD_DIR;
                if (b->to == PK_ANC_RESULTS) f->hold &= (uint8_t)~PK_HOLD_RESULTS;
                if (b->to == PK_ANC_BOARD) f->hold &= (uint8_t)~PK_HOLD_BOARD;
            }
            break;
        case PK_BK_FLIP:
            if (b->to == PK_ANC_FAN) f->revealing = 1;
            break;
        default: break;
        }
    }
    if (edges & EDGE_MID) {
        if (b->kind == PK_BK_TURN) f->dir = b->sub;
        if (b->kind == PK_BK_FLIP && b->to == PK_ANC_FAN && b->seat < PK_MAX_SEATS)
            f->reveal_shown[b->seat]++;
    }
    if (edges & EDGE_END) {
        if (b->flags & PK_BF_TOP) {
            f->top = b->card;
            if (f->stack_n < 255) f->stack_n++;
        }
        if ((b->flags & PK_BF_SHOWS) && b->to_i < f->my_n) f->my_unseen[b->to_i] = 0;
        if (b->flags & PK_BF_DEAL) f->fans_empty &= (uint8_t)~bit(b->seat);
        if (b->kind == PK_BK_FLIGHT && b->to == PK_ANC_BURY && f->buried_hold) f->buried_hold--;
        if (b->kind == PK_BK_FLIGHT && b->ev_i == PK_BEAT_NO_EVENT && b->to == PK_ANC_STACK)
            f->hold &= (uint8_t)~PK_HOLD_PENDING;
        if (b->kind == PK_BK_FADE && !b->sub && b->to == PK_ANC_DIR) f->hold |= PK_HOLD_DIR;
    }
}

/* What a PLAYED beat holds back from t = 0 until it runs. */
static void hold_back(PkBeatFrame *f, const PkBeat *b)
{
    if (b->kind == PK_BK_STAMP) f->stamp_hold |= bit(b->seat);
    if (b->kind == PK_BK_FADE && b->sub) {
        if (b->to == PK_ANC_DIR) f->hold |= PK_HOLD_DIR;
        if (b->to == PK_ANC_RESULTS) f->hold |= PK_HOLD_RESULTS;
        if (b->to == PK_ANC_BOARD) f->hold |= PK_HOLD_BOARD;
    }
    if (b->kind == PK_BK_FLIGHT && b->to == PK_ANC_BURY) f->buried_hold++;
    if ((b->flags & PK_BF_DEAL) && b->to == PK_ANC_FAN) f->fans_empty |= bit(b->seat);
    if (b->kind == PK_BK_FLIGHT && b->ev_i == PK_BEAT_NO_EVENT && b->to == PK_ANC_STACK)
        f->hold |= PK_HOLD_PENDING;
}

static uint32_t beat_end(const PkBeat *b) { return b->start_ms + b->dur_ms; }

/* ---- the builder ------------------------------------------------------------------ */

typedef struct {                 /* the clock: restored after a DONE event   */
    uint32_t cause;              /* the end a next step waits for            */
    int      started;            /* a timed beat exists                      */
    int32_t  land, fx;           /* the last play's landing; its effects' next start */
    int32_t  draw_prev;          /* the previous draw of a run               */
    int32_t  gag_until;          /* a reshuffle gag's end, for its draw      */
    int      pen_open, pen_victim, pen_why;
    int32_t  pen_prev, pen_base, pen_end;
    int32_t  reveal_prev, reveal_base;
    int      won;
    uint32_t deal_base;
    int      deal_i;
    int      prev_kind;
    int32_t  last_play_land;
    int      buried;             /* start cards buried so far (BURY to_i)    */
} Clock;

typedef struct {
    PkBeats *out;
    int      viewer, n_seats, mode, flags;
    uint32_t lead;
    int      done;               /* the current event is DONE               */
    int      overflow;
    int      deal_cards;
    int      hand_n;             /* my hand, as far as the build has got     */
    uint16_t ev_i;
    uint8_t  ev_kind;
    Clock    c;
} B;

static PkBeat mk(B *b, int kind, uint32_t start, int dur)
{
    PkBeat x;
    memset(&x, 0, sizeof x);
    x.kind = (uint8_t)kind;
    x.start_ms = start;
    x.dur_ms = (uint16_t)dur;
    x.part_ms = (uint16_t)dur;
    x.parts = 1;
    x.ev_i = b->ev_i;
    x.ev_kind = b->ev_kind;
    x.card = PK_CARD_NONE;
    x.deck_n = PK_BEAT_NO_DECK;
    x.bulge = 100;
    x.seat = PK_SEAT_NONE;
    x.from_i = x.to_i = 0;
    x.suit = PK_NO_SUIT;
    x.ease = PK_EASE_EASE_OUT;
    return x;
}

/* Keep it (a PLAYED event), or fold it straight into the board (a DONE one). */
static uint32_t add(B *b, PkBeat x)
{
    if (b->done) {
        commit(&b->out->start, &x, EDGE_START | EDGE_MID | EDGE_END);
        return 0;
    }
    if (b->out->n >= PK_BEATS_MAX) { b->overflow = 1; return beat_end(&x); }
    hold_back(&b->out->start, &x);
    b->out->beat[b->out->n++] = x;
    b->c.started = 1;
    uint32_t e = beat_end(&x);
    if (e > b->out->total_ms) b->out->total_ms = e;
    return e;
}

static void wait_for(B *b, uint32_t t) { if (t > b->c.cause) b->c.cause = t; }

/* Between steps: 25ms after what came before (foolish flightGap); the first
 * thing of a plan goes at the lead. */
static uint32_t step_start(B *b) { return b->c.started ? b->c.cause + PK_T_GAP : b->lead; }

/* A play's effects go when it lands; with no play in this plan (at Send, the
 * play landed at stage) they go as a step of their own. */
static uint32_t fx_start(B *b) { return b->c.land >= 0 ? (uint32_t)b->c.fx : step_start(b); }

static int me(const B *b, int seat) { return seat != PK_SEAT_NONE && seat == b->viewer; }

/* The slot the next draw of a run takes: replayed and penalty draws start
 * 110ms apart and overlap (U19); a draw that starts a run is a step. */
static uint32_t draw_slot(B *b, int penalty)
{
    if (penalty) return b->c.pen_prev >= 0 ? (uint32_t)b->c.pen_prev + PK_T_DRAW_STEP : (uint32_t)b->c.pen_base;
    return b->c.draw_prev >= 0 ? (uint32_t)b->c.draw_prev + PK_T_DRAW_STEP : step_start(b);
}

/* Grid "Draw x1": a back flies deck -> the right end of my row (bulge 1.08)
 * and turns over, or deck -> the end of a seat's fan. */
static uint32_t draw_to(B *b, int seat, uint8_t card, uint8_t deck_n, uint32_t s)
{
    PkBeat f = mk(b, PK_BK_FLIGHT, s, PK_T_DRAW);
    f.ease = PK_EASE_FLIGHT;
    f.bulge = 108;
    f.from = PK_ANC_DECK;
    f.deck_n = deck_n;
    f.seat = (uint8_t)seat;
    if (me(b, seat)) {
        f.to = PK_ANC_HAND;
        f.to_i = (uint8_t)b->hand_n;
        f.card = card;
        f.flags = PK_BF_ADDS;
        uint32_t e = add(b, f);
        PkBeat t = mk(b, PK_BK_FLIP, s + PK_T_DRAW, PK_T_DRAW_FLIP);
        t.to = t.from = PK_ANC_HAND;
        t.to_i = t.from_i = (uint8_t)b->hand_n;
        t.card = card;
        t.seat = (uint8_t)seat;
        t.flags = PK_BF_SHOWS;
        t.ease = PK_EASE_LINEAR;
        e = add(b, t);
        b->hand_n++;
        if (b->mode == PK_BEATS_STAGE) {           /* the strip's chip counts up */
            PkBeat p = mk(b, PK_BK_PULSE, s, PK_T_PULSE);
            p.to = p.from = PK_ANC_STRIP;
            p.amp = 8;
            add(b, p);
        }
        return b->done ? 0 : e;
    }
    f.to = PK_ANC_FAN;
    f.to_i = (uint8_t)seat;
    f.card = PK_CARD_HIDDEN;
    return add(b, f);
}

/* Grid "Play a skip" B2: a red bar wipes across the fan (260ms) while the badge
 * dims to .45 and back (900ms); for me, my hand dims. */
static uint32_t skip_mark(B *b, int seat, uint32_t s)
{
    PkBeat sl = mk(b, PK_BK_SLASH, s, PK_T_DIM);
    sl.part_ms = PK_T_SLASH;
    sl.from = sl.to = PK_ANC_FAN;
    sl.from_i = sl.to_i = (uint8_t)seat;
    sl.seat = (uint8_t)seat;
    if (!me(b, seat)) add(b, sl);
    PkBeat d = mk(b, PK_BK_DIM, s, PK_T_DIM);
    d.from = d.to = me(b, seat) ? PK_ANC_HAND : PK_ANC_SEAT;
    d.from_i = d.to_i = (uint8_t)(me(b, seat) ? 0 : seat);
    d.seat = (uint8_t)seat;
    d.amp = 45;
    return add(b, d);
}

static uint32_t stamp(B *b, int seat, int sub, uint32_t s)
{
    PkBeat x = mk(b, PK_BK_STAMP, s, PK_T_STAMP);
    x.ease = PK_EASE_STAMP;
    x.from = x.to = PK_ANC_SLOT;
    x.from_i = x.to_i = (uint8_t)seat;
    x.seat = (uint8_t)seat;
    x.sub = (uint8_t)sub;
    return add(b, x);
}

static void finish_penalty(B *b)
{
    if (!b->c.pen_open) return;
    b->c.pen_open = 0;
    uint32_t t = (uint32_t)b->c.pen_end;
    /* Grid "Play a +2" B3: after the cards, the victim is skipped */
    if (b->c.pen_why == PK_PEN_PLUS2 || b->c.pen_why == PK_PEN_WILD4)
        t = skip_mark(b, b->c.pen_victim, t);
    if (!b->done) {
        b->c.fx = (int32_t)t;
        if (b->c.land < 0) b->c.land = (int32_t)t;
        wait_for(b, t);
    }
}

static void results(B *b)
{
    uint32_t t = (b->c.reveal_prev >= 0 ? (uint32_t)b->c.reveal_prev + PK_T_REVEAL_STEP
                                        : (uint32_t)b->c.reveal_base);
    PkBeat h = mk(b, PK_BK_HOLD, t, PK_T_GAME_OVER);
    h.to = h.from = PK_ANC_BOARD;
    add(b, h);
    PkBeat f = mk(b, PK_BK_FADE, t + PK_T_GAME_OVER, PK_T_FADE);
    f.to = f.from = PK_ANC_RESULTS;
    f.sub = 1;
    wait_for(b, add(b, f));
    b->c.won = 0;
}

static void on_event(B *b, const PkEvent *e)
{
    Clock *c = &b->c;
    uint32_t s, t;
    switch (e->kind) {
    case PK_EV_BUBBLE_BEGIN:
        c->draw_prev = -1;
        c->land = -1;
        break;

    case PK_EV_BUBBLE_END:
        finish_penalty(b);
        if (c->won) results(b);
        break;

    /* Grid "Join" / "Leave" */
    case PK_EV_LOBBY_JOIN:
    case PK_EV_LOBBY_LEAVE: {
        s = step_start(b);
        PkBeat f = mk(b, PK_BK_FADE, s, PK_T_FADE);
        f.from = f.to = PK_ANC_ROW;
        f.from_i = f.to_i = e->seat;
        f.seat = e->seat;
        f.sub = e->kind == PK_EV_LOBBY_JOIN;
        t = add(b, f);
        if (e->kind == PK_EV_LOBBY_LEAVE) {        /* the rows below close up */
            PkBeat h = mk(b, PK_BK_HOLD, t, PK_T_SPRING);
            h.from = h.to = PK_ANC_ROW;
            h.from_i = h.to_i = e->seat;
            h.ease = PK_EASE_SPRING;
            t = add(b, h);
        }
        wait_for(b, t);
        break;
    }

    /* Grid "Start": the lobby rests, then FADEs to the table */
    case PK_EV_LOBBY_START: {
        s = step_start(b);
        PkBeat h = mk(b, PK_BK_HOLD, s, PK_T_LOBBY_REST);
        h.from = h.to = PK_ANC_BOARD;
        add(b, h);
        PkBeat f = mk(b, PK_BK_FADE, s + PK_T_LOBBY_REST, PK_T_FADE);
        f.from = f.to = PK_ANC_BOARD;
        f.sub = 1;
        wait_for(b, add(b, f));
        break;
    }

    /* Grid "Deal" 1: the deck riffles twice */
    case PK_EV_SHUFFLE:
        s = step_start(b);
        for (int k = 0; k < 2; k++) {
            PkBeat r = mk(b, PK_BK_RIFFLE, s, PK_T_RIFFLE + PK_T_RIFFLE_STEP * (PK_BEAT_FAT_LAYERS - 1));
            r.part_ms = PK_T_RIFFLE;
            r.parts = PK_BEAT_FAT_LAYERS;
            r.stagger_ms = PK_T_RIFFLE_STEP;
            r.from = r.to = PK_ANC_DECK;
            r.amp = 7;
            r.rot1 = 4;
            r.deck_n = e->deck_n;
            s = add(b, r);
        }
        wait_for(b, s);
        c->deal_base = s;
        c->deal_i = 0;
        break;

    /* Grid "Deal" 2: round-robin, one card at a time, 320ms each, start to
     * start clamp(1800 / cards, 45, 110); mine lands face down and turns */
    case PK_EV_DEAL: {
        s = c->deal_base + (uint32_t)pk_beats_deal_at(c->deal_i++, b->deal_cards);
        PkBeat f = mk(b, PK_BK_FLIGHT, s, PK_T_DEAL);
        f.ease = PK_EASE_FLIGHT;
        f.bulge = 105;
        f.from = PK_ANC_DECK;
        f.deck_n = e->deck_n;
        f.seat = e->seat;
        f.flags = PK_BF_DEAL;
        if (me(b, e->seat)) {
            f.to = PK_ANC_HAND;
            f.to_i = (uint8_t)b->hand_n;
            f.card = e->card;
            f.flags |= PK_BF_ADDS;
            add(b, f);
            PkBeat fl = mk(b, PK_BK_FLIP, s + PK_T_DEAL, PK_T_DEAL_FLIP);
            fl.from = fl.to = PK_ANC_HAND;
            fl.from_i = fl.to_i = (uint8_t)b->hand_n;
            fl.card = e->card;
            fl.seat = e->seat;
            fl.flags = PK_BF_SHOWS;
            fl.ease = PK_EASE_LINEAR;
            wait_for(b, add(b, fl));
            b->hand_n++;
        } else {
            f.to = PK_ANC_FAN;
            f.to_i = e->seat;
            f.card = PK_CARD_HIDDEN;
            wait_for(b, add(b, f));
        }
        break;
    }

    /* Grid "Start card" 1: a back flies deck -> pile and turns at the midpoint */
    case PK_EV_FLIP: {
        s = c->prev_kind == PK_EV_DEAL ? c->cause + PK_T_DEAL_REST
          : c->prev_kind == PK_EV_BURY ? c->cause + PK_T_BURY_REST
          : step_start(b);
        PkBeat f = mk(b, PK_BK_FLIGHT, s, PK_T_FLIGHT);
        f.ease = PK_EASE_FLIGHT;
        f.bulge = 115;
        f.rot1 = -3;
        f.from = PK_ANC_DECK;
        f.to = PK_ANC_STACK;
        f.card = e->card;
        f.deck_n = e->deck_n;
        f.flags = PK_BF_FACE_MID | PK_BF_TOP;
        wait_for(b, add(b, f));
        break;
    }

    /* Grid "Start card" 2: the non-number slides UNDER the deck, face up, +9 deg */
    case PK_EV_BURY: {
        s = c->cause + PK_T_FLIP_REST;
        PkBeat f = mk(b, PK_BK_FLIGHT, s, PK_T_FLIGHT);
        f.ease = PK_EASE_FLIGHT;
        f.rot0 = -3;
        f.rot1 = 9;
        f.from = PK_ANC_STACK;
        f.to = PK_ANC_BURY;
        f.to_i = (uint8_t)c->buried++;
        f.card = e->card;
        f.deck_n = e->deck_n;
        wait_for(b, add(b, f));
        break;
    }

    /* START_CARD: the halo fades in; the direction mark with it (not at 2) */
    case PK_EV_START_CARD: {
        PkBeat h = mk(b, PK_BK_HALO, c->cause, PK_T_FADE);
        h.from = h.to = PK_ANC_STACK;
        h.suit = e->suit;
        add(b, h);
        if (b->n_seats > 2) {
            PkBeat d = mk(b, PK_BK_FADE, c->cause, PK_T_FADE);
            d.from = d.to = PK_ANC_DIR;
            d.sub = 1;
            add(b, d);
        }
        break;
    }

    /* Grid "Turn moves": the last event of a turn, 25ms after what caused it */
    case PK_EV_TURN_TO: {
        finish_penalty(b);
        s = step_start(b);
        PkBeat x = mk(b, PK_BK_TURN_BAR, s, PK_T_FADE);
        x.from = PK_ANC_SEAT;
        x.to = PK_ANC_SEAT;
        x.from_i = e->other;
        x.to_i = e->seat;
        x.seat = e->seat;
        wait_for(b, add(b, x));
        c->land = c->fx = -1;
        c->draw_prev = -1;
        break;
    }

    /* Grid "Say it": the LAST stamp slams under the sayer's badge */
    case PK_EV_SAY_IT:
        s = step_start(b);
        if (!me(b, e->seat)) wait_for(b, stamp(b, e->seat, PK_STAMP_LAST, s));
        break;

    /* Grid "Call-out": the tapped fan presses and rings */
    case PK_EV_CALL_OUT: {
        s = step_start(b);
        PkBeat r = mk(b, PK_BK_RING, s, PK_T_RING);
        r.from = r.to = PK_ANC_FAN;
        r.from_i = r.to_i = e->seat;
        r.seat = e->seat;
        r.amp = 5;
        wait_for(b, add(b, r));
        break;
    }

    /* Grid "Reshuffle" 1: every under-card slides into the empty deck */
    case PK_EV_RESHUFFLE_GATHER: {
        s = draw_slot(b, c->pen_open);
        int parts = e->n < 1 ? 1 : e->n > PK_BEAT_UNDER ? PK_BEAT_UNDER : e->n;
        PkBeat g = mk(b, PK_BK_GATHER, s, PK_T_GATHER + PK_T_GATHER_STEP * (parts - 1));
        g.part_ms = PK_T_GATHER;
        g.parts = (uint8_t)parts;
        g.stagger_ms = PK_T_GATHER_STEP;
        g.ease = PK_EASE_IN;
        g.bulge = 104;
        g.from = PK_ANC_STACK;
        g.to = PK_ANC_DECK;
        g.card = PK_CARD_HIDDEN;
        t = add(b, g);
        if (!b->done) c->gag_until = (int32_t)t;
        wait_for(b, t);
        break;
    }

    /* Grid "Reshuffle" 2: fatten, then riffle twice; the count snaps */
    case PK_EV_RESHUFFLE_SHUFFLE: {
        s = c->gag_until >= 0 ? (uint32_t)c->gag_until : step_start(b);
        PkBeat f = mk(b, PK_BK_FATTEN, s, PK_T_FATTEN);
        f.ease = PK_EASE_SPRING;
        f.from = f.to = PK_ANC_DECK;
        f.deck_n = e->n;
        s = add(b, f);
        for (int k = 0; k < 2; k++) {
            PkBeat r = mk(b, PK_BK_RIFFLE, s, PK_T_RIFFLE + PK_T_RIFFLE_STEP * (PK_BEAT_FAT_LAYERS - 1));
            r.part_ms = PK_T_RIFFLE;
            r.parts = PK_BEAT_FAT_LAYERS;
            r.stagger_ms = PK_T_RIFFLE_STEP;
            r.from = r.to = PK_ANC_DECK;
            r.amp = 7;
            r.rot1 = 4;
            s = add(b, r);
        }
        if (!b->done) c->gag_until = (int32_t)s;
        wait_for(b, s);
        break;
    }

    case PK_EV_RESHUFFLE_DONE:
        break;

    /* Grid "Draw x1" / "Draw xN" */
    case PK_EV_DRAW:
        s = c->gag_until >= 0 ? (uint32_t)c->gag_until : draw_slot(b, 0);
        c->gag_until = -1;
        c->draw_prev = (int32_t)s;
        wait_for(b, draw_to(b, e->seat, e->card, e->deck_n, s));
        break;

    /* Grid "Play a number": hand -> pile, 500ms, bulge 1.15, lands -3 deg */
    case PK_EV_PLAY: {
        s = step_start(b);
        c->draw_prev = -1;
        const int mine = me(b, e->seat);
        const int wild = pk_is_wild(e->card);
        PkBeat f;
        if (mine && wild && b->mode == PK_BEATS_STAGE && (b->flags & PK_BFL_WILD_PLACED)) {
            /* the picker already put it there: it only leaves the hand */
            f = mk(b, PK_BK_HOLD, s, 0);
            f.from = PK_ANC_HAND;
            f.from_i = e->i;
            f.to = PK_ANC_STACK;
            f.card = e->card;
            f.flags = PK_BF_LEAVES | PK_BF_TOP;
        } else {
            f = mk(b, PK_BK_FLIGHT, s, PK_T_FLIGHT);
            f.ease = PK_EASE_FLIGHT;
            f.bulge = 115;
            f.rot1 = -3;
            f.from = mine ? PK_ANC_HAND : PK_ANC_FAN;
            f.from_i = mine ? e->i : e->seat;
            f.to = PK_ANC_STACK;
            f.card = e->card;
            f.flags = (uint8_t)(PK_BF_TOP | (mine ? PK_BF_LEAVES : PK_BF_FACE_MID));
        }
        f.seat = e->seat;
        f.deck_n = e->deck_n;
        t = add(b, f);
        if (mine && b->hand_n > 0) b->hand_n--;
        if (!wild) {                              /* the halo cross-fades as it lands */
            PkBeat h = mk(b, PK_BK_HALO, t, PK_T_FADE);
            h.from = h.to = PK_ANC_STACK;
            h.suit = e->suit;
            add(b, h);
        }
        if (!b->done) {
            c->land = c->fx = (int32_t)t;
            c->last_play_land = (int32_t)t;
        }
        wait_for(b, t);
        break;
    }

    /* Grid "Play a wild" 3: the band slides up, the halo turns */
    case PK_EV_WILD_SUIT: {
        s = c->land >= 0 ? (uint32_t)c->land : step_start(b);
        if (me(b, e->seat)) {
            PkBeat band = mk(b, PK_BK_BAND, s, PK_T_FADE);
            band.from = band.to = PK_ANC_STACK;
            band.suit = e->suit;
            add(b, band);
        }
        PkBeat h = mk(b, PK_BK_HALO, s, PK_T_FADE);
        h.from = h.to = PK_ANC_STACK;
        h.suit = e->suit;
        add(b, h);
        break;
    }

    /* Grid "Play a skip" / "Play a reverse" at 2 */
    case PK_EV_SKIP:
    case PK_EV_REVERSE_AS_SKIP:
        s = fx_start(b);
        t = skip_mark(b, e->seat, s);
        if (!b->done) { c->fx = (int32_t)t; if (c->land < 0) c->land = (int32_t)s; }
        wait_for(b, t);
        break;

    /* Grid "Play a reverse" B2: TURN on the direction box */
    case PK_EV_REVERSE: {
        s = fx_start(b);
        PkBeat x = mk(b, PK_BK_TURN, s, 2 * PK_T_HALF);
        x.from = x.to = PK_ANC_DIR;
        x.sub = e->dir;
        x.ease = PK_EASE_LINEAR;
        t = add(b, x);
        if (!b->done) { c->fx = (int32_t)t; if (c->land < 0) c->land = (int32_t)s; }
        wait_for(b, t);
        break;
    }

    /* Grid "Play a +2" / "+4" / "Call-out penalty" / "Penalty draw" */
    case PK_EV_PENALTY:
        finish_penalty(b);
        c->pen_open = 1;
        c->pen_victim = e->seat;
        c->pen_why = e->i;
        c->pen_prev = -1;
        c->pen_base = (int32_t)fx_start(b);
        c->pen_end = c->pen_base;
        break;

    case PK_EV_PENALTY_DRAW:
        s = c->gag_until >= 0 ? (uint32_t)c->gag_until : draw_slot(b, 1);
        c->gag_until = -1;
        c->pen_prev = (int32_t)s;
        t = draw_to(b, e->seat, e->card, e->deck_n, s);
        if ((int32_t)t > c->pen_end) c->pen_end = (int32_t)t;
        wait_for(b, t);
        if (e->i >= e->n) finish_penalty(b);
        break;

    /* SHORT: the empty deck shakes once, and the rest is forgiven (D15) */
    case PK_EV_PENALTY_SHORT: {
        s = draw_slot(b, 1);
        PkBeat x = mk(b, PK_BK_SHAKE, s, PK_T_DECK_SHAKE);
        x.ease = PK_EASE_LINEAR;
        x.from = x.to = PK_ANC_DECK;
        x.amp = 3;
        t = add(b, x);
        if ((int32_t)t > c->pen_end) c->pen_end = (int32_t)t;
        wait_for(b, t);
        finish_penalty(b);
        break;
    }

    /* Grid "Pass": my hand dims to .5, or the passer's fan shrugs */
    case PK_EV_PASS: {
        s = step_start(b);
        c->draw_prev = -1;
        PkBeat x;
        if (me(b, e->seat)) {
            x = mk(b, PK_BK_FADE, s, PK_T_FADE);
            x.from = x.to = PK_ANC_HAND;
            x.amp = 50;
        } else {
            x = mk(b, PK_BK_SHRUG, s, PK_T_SHRUG);
            x.from = x.to = PK_ANC_FAN;
            x.from_i = x.to_i = e->seat;
            x.amp = 4;
        }
        x.seat = e->seat;
        wait_for(b, add(b, x));
        break;
    }

    /* Grid "Call-out penalty" B1: the stamp, then its cards */
    case PK_EV_CALL_HIT:
    case PK_EV_CALL_MISS:
        finish_penalty(b);
        s = step_start(b);
        t = me(b, e->seat) ? s : stamp(b, e->seat, e->kind == PK_EV_CALL_HIT ? PK_STAMP_CAUGHT : PK_STAMP_WRONG, s);
        if (!b->done) { c->land = (int32_t)s; c->fx = (int32_t)t; }
        wait_for(b, t);
        break;

    /* Grid "Win" B2: OUT slams into the winner's slot; every turn bar goes */
    case PK_EV_WIN: {
        finish_penalty(b);
        s = c->land >= 0 && c->last_play_land == (int32_t)c->cause ? (uint32_t)c->land : step_start(b);
        t = s;
        if (e->seat != PK_SEAT_NONE) t = stamp(b, e->seat, PK_STAMP_OUT, s);
        if (b->n_seats > 2) {
            PkBeat d = mk(b, PK_BK_FADE, s, PK_T_FADE);
            d.from = d.to = PK_ANC_DIR;
            d.sub = 0;
            add(b, d);
        }
        wait_for(b, t);
        c->reveal_base = (int32_t)t;
        c->reveal_prev = -1;
        c->won = 1;
        break;
    }

    /* Grid "End reveal": every back turns, seat by seat, 60ms apart */
    case PK_EV_REVEAL: {
        if (me(b, e->seat)) break;
        s = c->reveal_prev >= 0 ? (uint32_t)c->reveal_prev + PK_T_REVEAL_STEP : (uint32_t)c->reveal_base;
        c->reveal_prev = (int32_t)s;
        PkBeat x = mk(b, PK_BK_FLIP, s, PK_T_REVEAL_FLIP);
        x.ease = PK_EASE_LINEAR;
        x.from = x.to = PK_ANC_FAN;
        x.from_i = e->seat;
        x.to_i = (uint8_t)(e->i ? e->i - 1 : 0);
        x.seat = e->seat;
        x.card = e->card;
        wait_for(b, add(b, x));
        break;
    }

    default:
        break;
    }
    c->prev_kind = e->kind;
}

/* ---- the cut (5.3.6) ---------------------------------------------------------------- */

enum { EV_PLAYED = 0, EV_DONE, EV_HELD };

static int turn_action(const PkEvent *e)
{
    return e->half == PK_HALF_ACTION &&
           (e->kind == PK_EV_DRAW || e->kind == PK_EV_PLAY || e->kind == PK_EV_PASS);
}

static int same_event(const PkEvent *a, const PkEvent *b)
{
    return a->kind == b->kind && a->half == b->half && a->seat == b->seat && a->other == b->other &&
           a->card == b->card && a->n == b->n && a->i == b->i;
}

/* next[i]: the index of the first turn action after i, or n. match[i]: the
 * same event in the previous tap's plan, or -1; pnext likewise for that plan. */
static int classify(const PkEvent *ev, int n, const uint16_t *next, const int16_t *match,
                    const uint16_t *pnext, int prev_n, int i, int mode)
{
    const PkEvent *e = &ev[i];
    const int settle = e->half == PK_HALF_SETTLE;
    const int released = next[i] < n;              /* a later turn has begun */
    switch (mode) {
    case PK_BEATS_STAGE:
        if (!settle) return match[i] >= 0 ? EV_DONE : EV_PLAYED;
        if (!released) return EV_HELD;
        if (match[i] >= 0 && pnext[match[i]] < prev_n) return EV_DONE;   /* released at an earlier tap */
        return EV_PLAYED;
    case PK_BEATS_SEND:
        if (!settle) return EV_DONE;
        return released ? EV_DONE : EV_PLAYED;
    default:
        return EV_PLAYED;
    }
}

static uint16_t NEXT[PK_BEATS_EVENTS], PNEXT[PK_BEATS_EVENTS];
static int16_t  MATCH[PK_BEATS_EVENTS];

static void next_turn_actions(const PkEvent *ev, int n, uint16_t *next)
{
    int nx = n;
    for (int i = n - 1; i >= 0; i--) {
        next[i] = (uint16_t)nx;
        if (turn_action(&ev[i])) nx = i;
    }
}

int pk_beats_build(const PkEvent *ev, int n, const PkBeatFrame *start, int viewer, int n_seats,
                   int mode, const PkEvent *prev, int prev_n, int flags, PkBeats *out)
{
    memset(out, 0, sizeof *out);
    out->mode = (uint8_t)mode;
    out->viewer = (uint8_t)(viewer >= 0 && viewer < PK_MAX_SEATS ? viewer : PK_SEAT_NONE);
    out->n_seats = (uint8_t)n_seats;
    if (start) out->start = *start; else pk_beats_frame_of(0, n_seats, &out->start);
    out->start.next_ms = PK_BEAT_NEVER;
    if (n < 0 || n > PK_BEATS_EVENTS || (n && !ev)) return -1;
    if (mode != PK_BEATS_STAGE || !prev || prev_n < 0 || prev_n > PK_BEATS_EVENTS) prev_n = 0;

    B b;
    memset(&b, 0, sizeof b);
    b.out = out;
    b.viewer = out->viewer;
    b.n_seats = n_seats;
    b.mode = mode;
    b.flags = flags;
    b.lead = mode == PK_BEATS_OPEN ? PK_T_LEAD_OPEN : PK_T_LEAD_LIVE;
    b.hand_n = out->start.my_n;
    b.c.land = b.c.fx = b.c.draw_prev = b.c.gag_until = -1;
    b.c.pen_prev = b.c.pen_base = b.c.pen_end = -1;
    b.c.reveal_prev = b.c.reveal_base = -1;
    b.c.last_play_land = -1;
    b.c.cause = b.lead;

    next_turn_actions(ev, n, NEXT);
    next_turn_actions(prev, prev_n, PNEXT);
    for (int i = 0, j = 0; i < n; i++) {          /* the previous plan, in order */
        MATCH[i] = -1;
        for (int k = j; k < prev_n; k++)
            if (same_event(&ev[i], &prev[k])) { MATCH[i] = (int16_t)k; j = k + 1; break; }
    }
    for (int i = 0; i < n; i++) if (ev[i].kind == PK_EV_DEAL) b.deal_cards++;

    for (int i = 0; i < n; i++) {
        int k = classify(ev, n, NEXT, MATCH, PNEXT, prev_n, i, mode);
        if (k == EV_HELD) { out->held++; continue; }
        b.ev_i = (uint16_t)i;
        b.ev_kind = ev[i].kind;
        b.done = k == EV_DONE;
        if (b.done) {
            Clock keep = b.c;
            on_event(&b, &ev[i]);
            b.c = keep;
        } else {
            on_event(&b, &ev[i]);
        }
    }
    b.done = 0;
    finish_penalty(&b);
    if (b.c.won) results(&b);
    if (b.overflow) { out->n = 0; return -1; }
    out->settle_ms = PK_T_COLLAPSE_WAIT + out->total_ms + PK_T_COLLAPSE_REST;
    return out->n;
}

/* ---- host motions ---------------------------------------------------------------- */

int pk_beats_host(int what, int a, int bb, const PkBeatFrame *start, int viewer, int n_seats,
                  int append, PkBeats *out)
{
    if (!append) {
        memset(out, 0, sizeof *out);
        out->mode = PK_BEATS_HOST;
        out->viewer = (uint8_t)(viewer >= 0 && viewer < PK_MAX_SEATS ? viewer : PK_SEAT_NONE);
        out->n_seats = (uint8_t)n_seats;
        if (start) out->start = *start; else pk_beats_frame_of(0, n_seats, &out->start);
        out->start.next_ms = PK_BEAT_NEVER;
    }
    B b;
    memset(&b, 0, sizeof b);
    b.out = out;
    b.viewer = out->viewer;
    b.n_seats = n_seats;
    b.mode = PK_BEATS_HOST;
    b.ev_i = PK_BEAT_NO_EVENT;
    b.lead = append ? out->total_ms : PK_T_LEAD_LIVE;
    const uint32_t s = b.lead;
    PkBeat x;
    switch (what) {
    /* Grid "Undo a staged card": after the 16ms beat the card lifts off the
     * pile back to its slot, no bulge; the halo returns */
    case PK_HM_UNDO:
    case PK_HM_PICKER_CANCEL:
    case PK_HM_RETRACT:
        x = mk(&b, PK_BK_FLIGHT, s, PK_T_FLIGHT);
        x.ease = PK_EASE_FLIGHT;
        x.rot0 = -3;
        x.from = PK_ANC_STACK;
        x.to = PK_ANC_HAND;
        x.to_i = (uint8_t)bb;
        x.card = (uint8_t)a;
        x.flags = PK_BF_HIDES | PK_BF_SHOWS | (what == PK_HM_RETRACT ? PK_BF_RETRACT : 0);
        add(&b, x);
        if (what == PK_HM_UNDO || what == PK_HM_RETRACT) {
            PkBeat h = mk(&b, PK_BK_HALO, s, PK_T_FADE);
            h.from = h.to = PK_ANC_STACK;
            h.suit = out->start.suit;
            add(&b, h);
        } else {                                  /* cancel: the tiles collapse first */
            PkBeat c = mk(&b, PK_BK_COLLAPSE, s, PK_T_COLLAPSE);
            c.ease = PK_EASE_IN;
            c.parts = 5;
            c.from = PK_ANC_PICKER;
            c.to = PK_ANC_STACK;
            add(&b, c);
            PkBeat sc = mk(&b, PK_BK_FADE, s, PK_T_FADE);
            sc.from = sc.to = PK_ANC_SCRIM;
            sc.sub = 0;
            add(&b, sc);
        }
        break;
    /* Grid "Undo a staged draw": refused; the newest one shakes three times */
    case PK_HM_REFUSED:
        x = mk(&b, PK_BK_SHAKE, s, 3 * PK_T_SHAKE);
        x.ease = PK_EASE_LINEAR;
        x.from = x.to = PK_ANC_HAND;
        x.from_i = x.to_i = (uint8_t)bb;
        x.parts = 3;
        x.amp = 5;
        add(&b, x);
        break;
    /* Grid "Play a wild" 1-2: the wild flies to the pile face up and waits;
     * the scrim fades in; four tiles (and the x) pop out 30ms apart */
    case PK_HM_PICKER_OPEN: {
        x = mk(&b, PK_BK_FLIGHT, s, PK_T_FLIGHT);
        x.ease = PK_EASE_FLIGHT;
        x.bulge = 115;
        x.rot1 = -3;
        x.from = PK_ANC_HAND;
        x.from_i = (uint8_t)bb;
        x.to = PK_ANC_STACK;
        x.card = (uint8_t)a;
        x.flags = PK_BF_HIDES;
        x.to_i = (uint8_t)bb;
        uint32_t t = add(&b, x);
        PkBeat sc = mk(&b, PK_BK_FADE, t, PK_T_FADE);
        sc.from = sc.to = PK_ANC_SCRIM;
        sc.sub = 1;
        add(&b, sc);
        PkBeat p = mk(&b, PK_BK_POP, t, PK_T_POP + 4 * PK_T_POP_STEP);
        p.ease = PK_EASE_SPRING;
        p.part_ms = PK_T_POP;
        p.parts = 5;
        p.stagger_ms = PK_T_POP_STEP;
        p.from = p.to = PK_ANC_PICKER;
        add(&b, p);
        break;
    }
    /* ... Tap: the tile rings, all four collapse into the card, scrim out */
    case PK_HM_PICKER_PICK: {
        PkBeat r = mk(&b, PK_BK_RING, s, PK_T_RING);
        r.from = r.to = PK_ANC_PICKER;
        r.from_i = r.to_i = (uint8_t)a;
        r.amp = 5;
        uint32_t t = add(&b, r);
        PkBeat c = mk(&b, PK_BK_COLLAPSE, t, PK_T_COLLAPSE);
        c.ease = PK_EASE_IN;
        c.parts = 5;
        c.from = PK_ANC_PICKER;
        c.to = PK_ANC_STACK;
        add(&b, c);
        PkBeat sc = mk(&b, PK_BK_FADE, t, PK_T_FADE);
        sc.from = sc.to = PK_ANC_SCRIM;
        sc.sub = 0;
        add(&b, sc);
        break;
    }
    /* Grid "Un-say / un-call": the ring and the tip fade off the fan */
    case PK_HM_UNCALL:
        x = mk(&b, PK_BK_FADE, s, PK_T_RING);
        x.from = x.to = PK_ANC_FAN;
        x.from_i = x.to_i = (uint8_t)a;
        x.seat = (uint8_t)a;
        x.sub = 0;
        add(&b, x);
        break;
    default:
        return -1;
    }
    if (b.overflow) return -1;
    out->settle_ms = PK_T_COLLAPSE_WAIT + out->total_ms + PK_T_COLLAPSE_REST;
    return out->n;
}

void pk_beats_delay(PkBeats *b, int first, uint32_t ms)
{
    for (int i = first < 0 ? 0 : first; i < b->n; i++) b->beat[i].start_ms += ms;
    if (b->n > first) b->total_ms += ms;
    b->settle_ms = PK_T_COLLAPSE_WAIT + b->total_ms + PK_T_COLLAPSE_REST;
}

/* ---- sampling --------------------------------------------------------------------------- */

static uint32_t mid_of(const PkBeat *b)
{
    return b->start_ms + b->dur_ms / 2;
}

void pk_beats_frame(const PkBeats *bs, uint32_t now, PkBeatFrame *out)
{
    *out = bs->start;
    uint32_t next = PK_BEAT_NEVER;
    int done = 1;
    for (int i = 0; i < bs->n; i++) {
        const PkBeat *b = &bs->beat[i];
        uint32_t s = b->start_ms, m = mid_of(b), e = beat_end(b);
        int edges = 0;
        if (now >= s) edges |= EDGE_START; else if (s < next) next = s;
        if (now >= m) edges |= EDGE_MID; else if (m < next) next = m;
        if (now >= e) edges |= EDGE_END; else { done = 0; if (e < next) next = e; }
        if (edges) commit(out, b, edges);
    }
    out->now_ms = now;
    out->next_ms = next;
    out->done = (uint8_t)done;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

/* A WAAPI keyframe list sampled at eased progress p: offsets o[], values v[]. */
static float keys(const float *o, const float *v, int n, float p)
{
    if (p <= o[0]) return v[0];
    for (int i = 1; i < n; i++)
        if (p <= o[i]) return lerp(v[i - 1], v[i], (p - o[i - 1]) / (o[i] - o[i - 1]));
    return v[n - 1];
}

void pk_beat_sample(const PkBeat *b, uint32_t now, int part, PkBeatSample *out)
{
    memset(out, 0, sizeof *out);
    out->scale = out->scale_x = out->opacity = 1;
    out->face = b->card != PK_CARD_HIDDEN;
    if (part < 0) part = 0;
    if (part >= b->parts) part = b->parts - 1;
    const uint32_t s = b->start_ms + (uint32_t)part * b->stagger_ms;
    /* a lone part runs the whole envelope (a slash stays up while its badge dims) */
    const uint32_t len = b->parts > 1 && b->stagger_ms ? b->part_ms : b->dur_ms;
    if (now < s) { out->state = PK_BS_PENDING; }
    else if (now >= s + len) { out->state = PK_BS_DONE; }
    else out->state = PK_BS_ACTIVE;
    const uint32_t run = b->part_ms ? b->part_ms : 1;
    float u = now <= s ? 0 : (float)(now - s) / (float)run;
    if (u > 1) u = 1;
    if (b->part_ms == 0) u = now >= s ? 1 : 0;
    const float p = pk_ease(b->ease, u);
    out->p = p;
    const int brings_in = b->kind == PK_BK_POP || b->kind == PK_BK_STAMP || (b->kind == PK_BK_FADE && b->sub);
    const int takes_out = b->kind == PK_BK_COLLAPSE || (b->kind == PK_BK_FADE && !b->sub);
    out->apply = (uint8_t)(out->state == PK_BS_ACTIVE || (out->state == PK_BS_PENDING && brings_in) ||
                           (out->state == PK_BS_DONE && takes_out));
    const float bulge = b->bulge / 100.0f;
    const float a = (float)b->amp;

    switch (b->kind) {
    case PK_BK_FLIGHT:
    case PK_BK_GATHER:
        out->scale = p <= .5f ? 1 + (bulge - 1) * p * 2 : bulge - (bulge - 1) * (p - .5f) * 2;
        out->rot = lerp((float)b->rot0, (float)b->rot1, p);
        if (b->card == PK_CARD_HIDDEN) out->face = 0;
        else if (b->flags & PK_BF_FACE_MID) out->face = u >= .5f;
        else out->face = 1;
        break;
    case PK_BK_FLIP:
        if (u < .5f) { out->scale_x = 1 - pk_ease(PK_EASE_IN, u * 2); out->face = 0; }
        else { out->scale_x = pk_ease(PK_EASE_OUT, (u - .5f) * 2); out->face = 1; }
        break;
    case PK_BK_RIFFLE: {
        const float sign = part % 2 ? 1.0f : -1.0f;
        const float v = p < .5f ? p * 2 : (1 - p) * 2;
        out->dx = sign * a * v;
        out->rot = sign * (float)b->rot1 * v;
        break;
    }
    case PK_BK_FATTEN: {
        static const float o[3] = { 0, .6f, 1 }, v[3] = { .7f, 1.14f, 1 };
        out->scale = keys(o, v, 3, p);
        break;
    }
    case PK_BK_HALO:
        out->opacity = lerp(.2f, 1, p);
        break;
    case PK_BK_BAND:
    case PK_BK_TURN_BAR:
        out->opacity = p;
        break;
    case PK_BK_STAMP: {
        static const float o[3] = { 0, .7f, 1 }, sv[3] = { 2.4f, .92f, 1 }, ov[3] = { 0, 1, 1 };
        out->scale = keys(o, sv, 3, p);
        out->opacity = keys(o, ov, 3, p);
        break;
    }
    case PK_BK_SLASH:
        break;                                     /* p is the wipe, 0..1 over part_ms */
    case PK_BK_DIM: {
        const float lo = a / 100.0f;
        static const float o[4] = { 0, 1.0f / 3, 2.0f / 3, 1 };
        const float v[4] = { 1, lo, lo, 1 };
        out->opacity = keys(o, v, 4, p);
        break;
    }
    case PK_BK_TURN:
        if (u < .5f) { out->rot = 90 * pk_ease(PK_EASE_IN, u * 2); out->face = 0; }
        else { out->rot = -90 + 90 * pk_ease(PK_EASE_OUT, (u - .5f) * 2); out->face = 1; }
        break;
    case PK_BK_FADE:
        if (b->sub) out->opacity = p;
        else out->opacity = 1 - (1 - (b->amp ? a / 100.0f : 0)) * p;
        break;
    case PK_BK_SHAKE:
        if (b->parts == 3) {                       /* 0, -5, 5, -4, 4, 0 */
            static const float o[6] = { 0, .2f, .4f, .6f, .8f, 1 };
            const float v[6] = { 0, -a, a, -(a - 1), a - 1, 0 };
            out->dx = keys(o, v, 6, p);
        } else {                                   /* once: 0, -a, a, 0 */
            static const float o[4] = { 0, 1.0f / 3, 2.0f / 3, 1 };
            const float v[4] = { 0, -a, a, 0 };
            out->dx = keys(o, v, 4, p);
        }
        break;
    case PK_BK_SHRUG:
    case PK_BK_PULSE: {
        const float peak = b->kind == PK_BK_SHRUG ? 1 - a / 100.0f : 1 + a / 100.0f;
        static const float o[3] = { 0, .5f, 1 };
        const float v[3] = { 1, peak, 1 };
        out->scale = keys(o, v, 3, p);
        break;
    }
    case PK_BK_RING:
        out->scale = 1 - (a / 100.0f) * p;
        out->opacity = p;
        break;
    case PK_BK_POP:
        out->scale = p;
        out->opacity = p < 1 ? p : 1;
        break;
    case PK_BK_COLLAPSE:
        out->scale = 1 - .8f * p;
        out->opacity = 1 - p;
        break;
    default:
        break;
    }
}

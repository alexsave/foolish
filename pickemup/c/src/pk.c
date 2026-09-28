/* Pick 'Em Up - the rules (RULES_AND_KERNEL.md sections 1 and 3).
 *
 * One apply path. pk_apply, pk_seal and pk_new all run through the static
 * functions below with an optional sink (pk_internal.h): NULL for play, a
 * live one for the animation plan. Undo is a replay of hist[] from the seed,
 * never an unwinding (the uttt_undo rule). */
#include "pk_internal.h"
#include <string.h>

#define BIT(s) ((uint8_t)(1u << (s)))

/* ---- the sink -------------------------------------------------------------- */

static void step(PkSink *k)
{
    if (k) k->step++;
}

/* A DEAL, DRAW or PENALTY_DRAW card is its receiver's alone (5.1); every
 * other card an event names was face up on the table. */
static int masked_kind(int kind)
{
    return kind == PK_EV_DEAL || kind == PK_EV_DRAW || kind == PK_EV_PENALTY_DRAW;
}

static void emit(const PkGame *g, PkSink *k, int kind, int half, int seat, int other,
                 int card, int n, int i)
{
    if (!k) return;
    if ((int)k->bubble <= k->from || (int)k->bubble > k->to) return;
    PkEvent e;
    e.kind   = (uint8_t)kind;
    e.half   = (uint8_t)half;
    e.seat   = (uint8_t)seat;
    e.other  = (uint8_t)other;
    e.card   = (uint8_t)card;
    if (masked_kind(kind) && k->viewer != PK_VIEW_ALL && k->viewer != seat)
        e.card = PK_CARD_HIDDEN;
    e.suit   = g->live_suit;
    e.n      = (uint8_t)(n > 255 ? 255 : n);
    e.i      = (uint8_t)(i > 255 ? 255 : i);
    e.bubble = k->bubble;
    e.step   = k->step;
    e.deck_n = g->deck_n;
    e.dir    = g->dir > 0 ? PK_DIR_CW : PK_DIR_ACW;
    k->count++;
    if (k->fn) k->fn(&e, k->ctx);
}

/* ---- small rules ------------------------------------------------------------ */

int pk_next(const PkGame *g, int from, int k)
{
    int n = g->n;
    int s = (from + k * g->dir) % n;
    return s < 0 ? s + n : s;
}

int pk_can_draw_any(const PkGame *g)
{
    return g->deck_n > 0 || g->stack_n >= 2;
}

/* THE ONE HELPER EVERY HAND CHANGE GOES THROUGH (3.3): a seat not on one card
 * is neither exposed nor stamped, so the bits cannot outlive the truth. */
static void hand_changed(PkGame *g, int s)
{
    if (g->hand_n[s] != 1) {
        g->exposed &= (uint8_t)~BIT(s);
        g->said    &= (uint8_t)~BIT(s);
    }
}

static int matches(const PkGame *g, uint8_t c)
{
    if (pk_is_wild(c)) return 1;
    if (pk_suit(c) == g->live_suit) return 1;
    return pk_rank(c) == pk_rank(g->stack[g->stack_n - 1]);
}

/* 1.10: fewest cards wins; a tie goes to whichever tied seat is next to move,
 * counting from the seat that would move next (itself first). */
static int fewest_from(const PkGame *g, int from)
{
    int best = -1;
    for (int j = 0; j < g->n; j++) {
        int s = pk_next(g, from, j);
        if (best < 0 || g->hand_n[s] < g->hand_n[best]) best = s;
    }
    return best;
}

static void finish(PkGame *g, int over, int winner)
{
    g->over = (uint8_t)over;
    g->winner = (uint8_t)(winner >= 0 ? winner : fewest_from(g, g->turn));
    g->turn = PK_SEAT_NONE;
}

/* ---- the deck --------------------------------------------------------------- */

/* 3.5: every stack card except the top becomes the new deck, shuffled from
 * block r << 32; the top stays with its live suit. */
static void reshuffle(PkGame *g, PkSink *k, int half)
{
    int m = g->stack_n - 1;
    uint8_t top = g->stack[m];
    int r = ++g->reshuffles;
    uint8_t a[PK_DECK];
    memcpy(a, g->stack, (size_t)m);
    g->stack[0] = top;
    g->stack_n = 1;
    memcpy(g->deck, a, (size_t)m);
    g->deck_n = (uint8_t)m;
    emit(g, k, PK_EV_RESHUFFLE_GATHER, half, PK_SEAT_NONE, PK_SEAT_NONE, PK_CARD_NONE, m, 0);
    pk_shuffle(g->deck, m, g->seed, pk_reshuffle_block(r));
    emit(g, k, PK_EV_RESHUFFLE_SHUFFLE, half, PK_SEAT_NONE, PK_SEAT_NONE, PK_CARD_NONE, m, r & 0xFF);
    emit(g, k, PK_EV_RESHUFFLE_DONE, half, PK_SEAT_NONE, PK_SEAT_NONE, PK_CARD_NONE, g->deck_n, 0);
}

/* One card off the top into `s`'s hand, reshuffling first if it must.
 * The card, or -1 when nothing can be drawn. The caller reports it. */
static int draw_card(PkGame *g, int s, PkSink *k, int half)
{
    if (g->deck_n == 0) {
        if (g->stack_n < 2) return -1;
        reshuffle(g, k, half);
    }
    uint8_t c = g->deck[--g->deck_n];
    g->hand[s][g->hand_n[s]++] = c;
    return c;
}

/* A penalty (+2, +4, a catch): automatic, and what cannot be supplied is
 * forgiven (D15). */
static void penalty(PkGame *g, int victim, int owed, int why, int cause_seat,
                    int cause_card, PkSink *k)
{
    emit(g, k, PK_EV_PENALTY, PK_HALF_SETTLE, victim, cause_seat, cause_card, owed, why);
    for (int i = 1; i <= owed; i++) {
        int c = draw_card(g, victim, k, PK_HALF_SETTLE);
        if (c < 0) {
            emit(g, k, PK_EV_PENALTY_SHORT, PK_HALF_SETTLE, victim, PK_SEAT_NONE,
                 PK_CARD_NONE, owed - i + 1, 0);
            break;
        }
        emit(g, k, PK_EV_PENALTY_DRAW, PK_HALF_SETTLE, victim, PK_SEAT_NONE, c, owed, i);
    }
    hand_changed(g, victim);
}

/* ---- the deal (1.4, 3.5) ------------------------------------------------------ */

int pk__new(PkGame *g, const uint8_t seed[32], int n, int starter, PkSink *k)
{
    if (n < 2 || n > PK_MAX_SEATS) return 0;
    memset(g, 0, sizeof *g);
    g->n = (uint8_t)n;
    g->dir = 1;
    g->turn = PK_SEAT_NONE;
    g->winner = PK_SEAT_NONE;
    g->starter = (uint8_t)(starter >= 0 && starter < n ? starter : PK_SEAT_NONE);
    memcpy(g->seed, seed, 32);
    if (k) { k->bubble = 0; k->step = 0; k->turn_draws = 0; }

    emit(g, k, PK_EV_LOBBY_START, PK_HALF_ACTION, g->starter, PK_SEAT_NONE, PK_CARD_NONE, n, 0);
    for (int c = 0; c < PK_DECK; c++) g->deck[c] = (uint8_t)c;
    pk_shuffle(g->deck, PK_DECK, seed, 0);
    g->deck_n = PK_DECK;
    step(k);
    emit(g, k, PK_EV_SHUFFLE, PK_HALF_ACTION, PK_SEAT_NONE, PK_SEAT_NONE, PK_CARD_NONE, PK_DECK, 0);

    /* ONE CARD AT A TIME, ROUND-ROBIN from seat 1, the dealer last (D19). */
    for (int round = 0; round < PK_HAND_SIZE; round++)
        for (int j = 0; j < n; j++) {
            int s = (1 + j) % n;
            uint8_t c = g->deck[--g->deck_n];
            g->hand[s][g->hand_n[s]++] = c;
            step(k);
            emit(g, k, PK_EV_DEAL, PK_HALF_ACTION, s, PK_SEAT_NONE, c, round + 1, round * n + j + 1);
        }

    /* THE START CARD IS A NUMBER (D14): anything else goes face up to the
     * bottom of the deck and the next card is turned. 72 numbers, so it ends;
     * the bound turns a broken bury into a refusal rather than a hang. */
    for (int guard = 0;; guard++) {
        if (guard >= PK_DECK || g->deck_n == 0) return 0;
        uint8_t c = g->deck[--g->deck_n];
        step(k);
        emit(g, k, PK_EV_FLIP, PK_HALF_ACTION, PK_SEAT_NONE, PK_SEAT_NONE, c, 0, 0);
        if (pk_is_number(c)) {
            g->stack[g->stack_n++] = c;
            g->live_suit = (uint8_t)pk_suit(c);
            emit(g, k, PK_EV_START_CARD, PK_HALF_ACTION, PK_SEAT_NONE, PK_SEAT_NONE, c, 0, 0);
            break;
        }
        for (int i = g->deck_n; i > 0; i--) g->deck[i] = g->deck[i - 1];
        g->deck[0] = c;
        g->deck_n++;
        emit(g, k, PK_EV_BURY, PK_HALF_ACTION, PK_SEAT_NONE, PK_SEAT_NONE, c, 0, 0);
    }
    g->turn = 1;
    step(k);
    emit(g, k, PK_EV_TURN_TO, PK_HALF_SETTLE, g->turn, PK_SEAT_NONE, PK_CARD_NONE, 0, 0);
    return 1;
}

int pk_new(PkGame *g, const uint8_t seed[32], int n)
{
    return pk__new(g, seed, n, -1, 0);
}

/* ---- legality (3.6) ------------------------------------------------------------ */

static int last_turn_kind(const PkGame *g)
{
    if (!g->b_open) return 0;
    for (int i = g->hist_n - 1; i > (int)g->b_rec; i--) {
        int kd = g->hist[i].kind;
        if (kd == PK_A_DRAW || kd == PK_A_PLAY || kd == PK_A_PASS) return kd;
    }
    return 0;
}

int pk_turn_ended(const PkGame *g)
{
    if (!g->b_open) return 0;
    if (g->over) return 1;
    int last = last_turn_kind(g);
    if (last != PK_A_PLAY && last != PK_A_PASS) return 0;
    /* D7: the turn came straight back and the sender is not on one card */
    return g->turn != g->b_sender || g->hand_n[g->b_sender] == 1;
}

/* May `seat` act on this draft at all? While one seat has a bubble open, no
 * other seat has anything legal on it. */
static int may_act(const PkGame *g, int seat)
{
    if (seat < 0 || seat >= g->n || g->over) return 0;
    return !g->b_open || g->b_sender == seat;
}

/* The long-game stop needs nothing here: every turn action ends in
 * long_stop, which sets `over` the moment `actions` reaches the cap, and
 * may_act refuses everything once over. ONE OWNER (1.11, 4.4's empty menu). */
static int turn_open(const PkGame *g, int seat)
{
    return may_act(g, seat) && seat == g->turn && !pk_turn_ended(g);
}

static int draw_ok(const PkGame *g, int seat)
{
    return turn_open(g, seat) && pk_can_draw_any(g);
}

static int play_ok(const PkGame *g, int seat, int p, int s)
{
    if (!turn_open(g, seat) || p < 0 || p >= g->hand_n[seat]) return 0;
    uint8_t c = g->hand[seat][p];
    if (!matches(g, c)) return 0;
    if (pk_is_wild(c) && g->hand_n[seat] > 1) return s >= 0 && s < PK_SUITS;
    return s == PK_NO_SUIT;   /* a suited card, or a last-card wild (D17) */
}

int pk_can_play(const PkGame *g, int seat, int p)
{
    return play_ok(g, seat, p, PK_NO_SUIT) || play_ok(g, seat, p, 0);
}

static int any_play(const PkGame *g, int seat)
{
    for (int p = 0; p < g->hand_n[seat]; p++)
        if (pk_can_play(g, seat, p)) return 1;
    return 0;
}

static int pass_ok(const PkGame *g, int seat)
{
    if (!turn_open(g, seat)) return 0;
    return g->t_drew || (!pk_can_draw_any(g) && !any_play(g, seat));
}

static int say_ok(const PkGame *g, int seat)
{
    if (!may_act(g, seat) || g->b_said) return 0;
    uint8_t ref = g->b_open ? (uint8_t)(g->exposed & g->b_exposed_at_open) : g->exposed;
    return (ref & BIT(seat)) != 0;
}

/* CALL_OUT NEVER READS A HAND COUNT OR `exposed` (3.6). If it did, a dimmed
 * fan would tell every player who is on one card. It reads only the LAST
 * stamps, which are public, as they stood when the bubble opened (D32). */
static int call_ok(const PkGame *g, int seat, int t)
{
    if (!may_act(g, seat) || t == seat || t < 0 || t >= g->n) return 0;
    if (g->b_open && g->b_call != PK_SEAT_NONE) return 0;
    uint8_t ref = g->b_open ? g->b_said_at_open : g->said;
    return !(ref & BIT(t));
}

int pk_is_legal(const PkGame *g, int seat, PkAct a)
{
    switch (a.kind) {
    case PK_A_DRAW:     return draw_ok(g, seat);
    case PK_A_PLAY:     return play_ok(g, seat, a.a, a.b);
    case PK_A_PASS:     return pass_ok(g, seat);
    case PK_A_SAY_IT:   return say_ok(g, seat);
    case PK_A_CALL_OUT: return call_ok(g, seat, a.a);
    default:            return 0;
    }
}

static int put(PkAct *out, int cap, int n, int kind, int a, int b)
{
    if (n < cap) {
        out[n].kind = (uint8_t)kind;
        out[n].a = (uint8_t)a;
        out[n].b = (uint8_t)b;
        out[n].c = 0;
    }
    return n + 1;
}

int pk_legal_turn(const PkGame *g, int seat, PkAct *out, int cap)
{
    int n = 0;
    if (!turn_open(g, seat)) return 0;
    if (draw_ok(g, seat)) n = put(out, cap, n, PK_A_DRAW, 0, 0);
    for (int p = 0; p < g->hand_n[seat]; p++) {
        if (play_ok(g, seat, p, PK_NO_SUIT)) n = put(out, cap, n, PK_A_PLAY, p, PK_NO_SUIT);
        for (int s = 0; s < PK_SUITS; s++)
            if (play_ok(g, seat, p, s)) n = put(out, cap, n, PK_A_PLAY, p, s);
    }
    if (pass_ok(g, seat)) n = put(out, cap, n, PK_A_PASS, 0, 0);
    return n < cap ? n : cap;
}

int pk_legal(const PkGame *g, int seat, PkAct *out, int cap)
{
    int n = pk_legal_turn(g, seat, out, cap);
    if (say_ok(g, seat)) n = put(out, cap, n, PK_A_SAY_IT, 0, 0);
    for (int t = 0; t < g->n; t++)
        if (call_ok(g, seat, t)) n = put(out, cap, n, PK_A_CALL_OUT, t, 0);
    return n < cap ? n : cap;
}

int pk_can_seal(const PkGame *g)
{
    if (!g->b_open) return 0;
    int last = last_turn_kind(g);
    if (!last && !g->b_said && g->b_call == PK_SEAT_NONE) return 0;   /* empty */
    if (last == PK_A_DRAW && !g->over) return 0;                      /* mid-turn */
    return 1;
}

/* ---- apply (3.7) ----------------------------------------------------------------- */

static void push_hist(PkGame *g, int kind, int a, int b, int c)
{
    PkAct *r = &g->hist[g->hist_n++];
    r->kind = (uint8_t)kind;
    r->a = (uint8_t)a;
    r->b = (uint8_t)b;
    r->c = (uint8_t)c;
}

static void open_bubble(PkGame *g, int seat, PkSink *k)
{
    g->b_open = 1;
    g->b_sender = (uint8_t)seat;
    g->b_said = 0;
    g->b_call = PK_SEAT_NONE;
    g->b_exposed_at_open = g->exposed;
    g->b_said_at_open = g->said;
    g->b_had_terminal = 0;
    g->b_created = 0;
    g->b_rec = g->hist_n;
    g->b_floor = g->hist_n;
    push_hist(g, PK_A_BUBBLE, seat, 0, PK_SEAT_NONE);
    if (k) { k->bubble = (uint16_t)(g->bubbles + 1); k->turn_draws = 0; }
    step(k);
    emit(g, k, PK_EV_BUBBLE_BEGIN, PK_HALF_ACTION, seat, PK_SEAT_NONE, PK_CARD_NONE, 0, 0);
}

static void long_stop(PkGame *g)
{
    if (!g->over && g->actions >= PK_MAX_ACTIONS) finish(g, PK_OVER_LONG, -1);
}

static void do_play(PkGame *g, int seat, int p, int s, PkSink *k)
{
    uint8_t c = g->hand[seat][p];
    int n = --g->hand_n[seat];
    /* D24: the rest close up, order kept */
    for (int i = p; i < n; i++) g->hand[seat][i] = g->hand[seat][i + 1];
    g->hand[seat][n] = 0;
    g->stack[g->stack_n++] = c;
    g->live_suit = (uint8_t)(pk_is_wild(c) ? (s < PK_SUITS ? s : g->live_suit) : pk_suit(c));
    g->actions++;
    g->turns++;
    g->idle_passes = 0;
    g->t_drew = 0;
    g->b_had_terminal = 1;
    hand_changed(g, seat);
    emit(g, k, PK_EV_PLAY, PK_HALF_ACTION, seat, PK_SEAT_NONE, c, 0, p);
    if (pk_is_wild(c) && s < PK_SUITS)
        emit(g, k, PK_EV_WILD_SUIT, PK_HALF_ACTION, seat, PK_SEAT_NONE, c, 0, 0);
    if (k) k->turn_draws = 0;

    /* 1.7: going out ends it at once; a last card's action does nothing */
    if (n == 0) {
        finish(g, PK_OVER_OUT, seat);
        return;
    }
    if (n == 1) {
        g->exposed |= BIT(seat);
        g->b_created |= BIT(seat);
    }

    int victim = pk_next(g, seat, 1);
    switch (pk_rank(c)) {
    case PK_R_SKIP:
        emit(g, k, PK_EV_SKIP, PK_HALF_SETTLE, victim, seat, PK_CARD_NONE, 0, 0);
        g->turn = (uint8_t)pk_next(g, seat, 2);
        break;
    case PK_R_REVERSE:
        if (g->n > 2) {
            g->dir = (int8_t)-g->dir;
            emit(g, k, PK_EV_REVERSE, PK_HALF_SETTLE, PK_SEAT_NONE, seat, PK_CARD_NONE, 0, 0);
            g->turn = (uint8_t)pk_next(g, seat, 1);
        } else {
            /* D13: at two players a Reverse is a Skip and the word does not turn */
            emit(g, k, PK_EV_REVERSE_AS_SKIP, PK_HALF_SETTLE, victim, seat, PK_CARD_NONE, 0, 0);
            g->turn = (uint8_t)seat;
        }
        break;
    case PK_R_PLUS2:
        penalty(g, victim, 2, PK_PEN_PLUS2, seat, c, k);
        g->turn = (uint8_t)pk_next(g, seat, 2);
        break;
    case PK_R_WILD4:
        penalty(g, victim, 4, PK_PEN_WILD4, seat, c, k);
        g->turn = (uint8_t)pk_next(g, seat, 2);
        break;
    default:
        g->turn = (uint8_t)pk_next(g, seat, 1);
        break;
    }
    emit(g, k, PK_EV_TURN_TO, PK_HALF_SETTLE, g->turn, seat, PK_CARD_NONE, 0, 0);
    long_stop(g);
}

int pk__apply(PkGame *g, int seat, PkAct a, PkSink *k)
{
    if (!pk_is_legal(g, seat, a)) return 0;
    if (!g->b_open) open_bubble(g, seat, k);

    if (a.kind == PK_A_SAY_IT) {
        step(k);
        g->b_said = 1;
        g->exposed &= (uint8_t)~BIT(seat);
        g->said |= BIT(seat);
        g->hist[g->b_rec].b |= PK_BR_SAID;
        emit(g, k, PK_EV_SAY_IT, PK_HALF_ACTION, seat, PK_SEAT_NONE, PK_CARD_NONE, 0, 0);
        return 1;
    }
    if (a.kind == PK_A_CALL_OUT) {
        /* D5d: nothing else happens until seal */
        step(k);
        g->b_call = a.a;
        g->hist[g->b_rec].b |= PK_BR_CALL;
        g->hist[g->b_rec].c = a.a;
        emit(g, k, PK_EV_CALL_OUT, PK_HALF_ACTION, a.a, seat, PK_CARD_NONE, 0, 0);
        return 1;
    }

    /* a turn action; a new turn inside the same bubble is a CONTINUE (D7) */
    if (g->b_had_terminal) {
        int last = g->hist[g->hist_n - 1].kind;
        if (last == PK_A_PLAY || last == PK_A_PASS) push_hist(g, PK_A_CONTINUE, 0, 0, 0);
    }
    push_hist(g, a.kind, a.kind == PK_A_PLAY ? a.a : 0, a.kind == PK_A_PLAY ? a.b : 0, 0);
    step(k);

    switch (a.kind) {
    case PK_A_DRAW: {
        int c = draw_card(g, seat, k, PK_HALF_ACTION);
        g->t_drew = 1;
        g->actions++;
        g->idle_passes = 0;
        hand_changed(g, seat);
        g->b_floor = g->hist_n;          /* D8: a draw is committed */
        if (k && k->turn_draws < 0xFFFF) k->turn_draws++;
        emit(g, k, PK_EV_DRAW, PK_HALF_ACTION, seat, PK_SEAT_NONE, c,
             k ? k->turn_draws : 0, k ? k->turn_draws : 0);
        long_stop(g);
        break;
    }
    case PK_A_PLAY:
        do_play(g, seat, a.a, a.b, k);
        break;
    case PK_A_PASS: {
        int drew = g->t_drew;
        g->actions++;
        g->turns++;
        g->b_had_terminal = 1;
        g->idle_passes = (uint8_t)(drew ? 0 : g->idle_passes + 1);
        g->t_drew = 0;
        emit(g, k, PK_EV_PASS, PK_HALF_ACTION, seat, PK_SEAT_NONE, PK_CARD_NONE,
             k ? k->turn_draws : 0, 0);
        if (k) k->turn_draws = 0;
        g->turn = (uint8_t)pk_next(g, seat, 1);
        emit(g, k, PK_EV_TURN_TO, PK_HALF_SETTLE, g->turn, seat, PK_CARD_NONE, 0, 0);
        if (g->idle_passes >= g->n) finish(g, PK_OVER_STUCK, -1);
        else long_stop(g);
        break;
    }
    }
    return 1;
}

int pk_apply(PkGame *g, int seat, PkAct a)
{
    return pk__apply(g, seat, a, 0);
}

/* ---- seal ----------------------------------------------------------------------- */

int pk__seal(PkGame *g, PkSink *k)
{
    if (!pk_can_seal(g)) return 0;
    step(k);
    int sender = g->b_sender;

    /* 2. the catch, judged against the table as it stood at open (D5d) */
    if (g->b_call != PK_SEAT_NONE) {
        int t = g->b_call;
        int hit = (g->b_exposed_at_open & BIT(t)) != 0;
        if (hit) {
            emit(g, k, PK_EV_CALL_HIT, PK_HALF_SETTLE, t, sender, PK_CARD_NONE, 0, 0);
            g->exposed &= (uint8_t)~BIT(t);
            if (!g->over) penalty(g, t, 2, PK_PEN_CAUGHT, sender, PK_CARD_NONE, k);
        } else {
            emit(g, k, PK_EV_CALL_MISS, PK_HALF_SETTLE, sender, t, PK_CARD_NONE, 0, 0);
            if (!g->over) penalty(g, sender, 1, PK_PEN_WRONG, t, PK_CARD_NONE, k);
        }
    }

    /* 3. the window (D4): a completed turn closes every exposure this bubble
     * did not itself create */
    if (g->b_had_terminal) g->exposed &= g->b_created;

    g->hist[g->b_rec].b |= PK_BR_SEALED;
    g->bubbles++;
    if (g->bubbles >= PK_MAX_BUBBLES && !g->over) finish(g, PK_OVER_LONG, -1);

    if (g->over) {
        int last = g->over == PK_OVER_OUT ? g->stack[g->stack_n - 1] : PK_CARD_NONE;
        emit(g, k, PK_EV_WIN, PK_HALF_SETTLE, g->winner, PK_SEAT_NONE, last, 0, g->over);
        for (int s = 0; s < g->n; s++)
            for (int i = 0; i < g->hand_n[s]; i++)
                emit(g, k, PK_EV_REVEAL, PK_HALF_SETTLE, s, PK_SEAT_NONE, g->hand[s][i],
                     g->hand_n[s], i + 1);
    }
    emit(g, k, PK_EV_BUBBLE_END, PK_HALF_ACTION, sender, PK_SEAT_NONE, PK_CARD_NONE, 0, 0);

    g->b_open = 0;
    g->b_sender = 0;
    g->b_said = 0;
    g->b_call = 0;
    g->b_exposed_at_open = 0;
    g->b_said_at_open = 0;
    g->b_had_terminal = 0;
    g->b_created = 0;
    g->t_drew = 0;
    g->b_floor = 0;
    g->b_rec = 0;
    return 1;
}

int pk_seal(PkGame *g)
{
    return pk__seal(g, 0);
}

/* ---- replay, undo --------------------------------------------------------------- */

int pk__replay(PkGame *out, const PkGame *g, int kk, PkSink *k)
{
    if (kk < 0 || kk > g->hist_n || kk > PK_HIST_CAP) return 0;
    if (!pk__new(out, g->seed, g->n, g->starter == PK_SEAT_NONE ? -1 : g->starter, k)) return 0;
    int rec = -1;
    for (int i = 0; i < kk; i++) {
        PkAct r = g->hist[i];
        switch (r.kind) {
        case PK_A_BUBBLE: {
            if (out->b_open) {
                if (!(g->hist[rec].b & PK_BR_SEALED) || !pk__seal(out, k)) return 0;
            }
            if (r.a >= out->n || (r.b & ~(PK_BR_SAID | PK_BR_CALL | PK_BR_SEALED))) return 0;
            if (out->over) return 0;
            rec = i;
            open_bubble(out, r.a, k);
            if (r.b & PK_BR_SAID) {
                PkAct s = { PK_A_SAY_IT, 0, 0, 0 };
                if (!pk__apply(out, r.a, s, k)) return 0;
            }
            if (r.b & PK_BR_CALL) {
                PkAct c = { PK_A_CALL_OUT, r.c, 0, 0 };
                if (!pk__apply(out, r.a, c, k)) return 0;
            }
            break;
        }
        case PK_A_CONTINUE:
            /* re-appended by the turn action that follows it */
            if (!out->b_open) return 0;
            break;
        case PK_A_DRAW: case PK_A_PLAY: case PK_A_PASS: {
            if (!out->b_open) return 0;
            PkAct a = { r.kind, r.a, r.b, 0 };
            if (!pk__apply(out, out->b_sender, a, k)) return 0;
            break;
        }
        default:
            return 0;
        }
    }
    if (out->b_open && (g->hist[rec].b & PK_BR_SEALED) && !pk__seal(out, k)) return 0;
    /* The rebuilt history must be the one given, record for record: a stray or
     * missing CONTINUE, or a seal bit the rules would not have set, refuses. */
    if (out->hist_n != kk) return 0;
    for (int i = 0; i < kk; i++) {
        PkAct x = out->hist[i], y = g->hist[i];
        if (x.kind != y.kind || x.a != y.a || x.b != y.b || x.c != y.c) return 0;
    }
    return 1;
}

int pk_replay(PkGame *out, const PkGame *g, int k)
{
    return pk__replay(out, g, k, 0);
}

static int has_turn_action(const PkGame *g, int from, int to)
{
    for (int i = from; i < to; i++) {
        int kd = g->hist[i].kind;
        if (kd == PK_A_DRAW || kd == PK_A_PLAY || kd == PK_A_PASS) return 1;
    }
    return 0;
}

/* Rebuild `g` from `src` through k, dropping the open bubble's record too
 * when nothing would be left in it (a draft with nothing in it is no draft). */
static int rebuild(PkGame *g, const PkGame *src, int k)
{
    int rec = g->b_rec;
    if (k > rec && !has_turn_action(src, rec + 1, k)
        && !(src->hist[rec].b & (PK_BR_SAID | PK_BR_CALL)))
        k = rec;
    PkGame tmp;
    if (!pk_replay(&tmp, src, k)) return 0;
    *g = tmp;
    return 1;
}

int pk_undo(PkGame *g)
{
    if (!g->b_open) return 0;
    int j = -1;
    for (int i = g->hist_n - 1; i > (int)g->b_rec; i--) {
        int kd = g->hist[i].kind;
        if (kd == PK_A_DRAW || kd == PK_A_PLAY || kd == PK_A_PASS) { j = i; break; }
    }
    if (j < 0) return 0;
    int k = j;
    if (g->hist[j - 1].kind == PK_A_CONTINUE) k = j - 1;
    if (k < (int)g->b_floor) return 0;      /* D8: never below the last draw */
    return rebuild(g, g, k);
}

static int drop_flag(PkGame *g, int flag)
{
    if (!g->b_open) return 0;
    if (flag == PK_BR_SAID && !g->b_said) return 0;
    if (flag == PK_BR_CALL && g->b_call == PK_SEAT_NONE) return 0;
    PkGame copy = *g;
    copy.hist[copy.b_rec].b &= (uint8_t)~flag;
    if (flag == PK_BR_CALL) copy.hist[copy.b_rec].c = PK_SEAT_NONE;
    return rebuild(g, &copy, copy.hist_n);
}

int pk_unsay(PkGame *g)  { return drop_flag(g, PK_BR_SAID); }
int pk_uncall(PkGame *g) { return drop_flag(g, PK_BR_CALL); }

int pk_floor(const PkGame *g)
{
    return g->b_open ? g->b_floor : g->hist_n;
}

int pk_to_floor(PkGame *g)
{
    if (!g->b_open) return 0;
    PkGame copy = *g;
    copy.hist[copy.b_rec].b &= (uint8_t)~(PK_BR_SAID | PK_BR_CALL);
    copy.hist[copy.b_rec].c = PK_SEAT_NONE;
    return rebuild(g, &copy, g->b_floor);
}

/* ---- hash ----------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *p, int n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (int i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

uint64_t pk_hash(const PkGame *g)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t small[] = {
        g->n, g->turn, (uint8_t)g->dir, g->live_suit, g->deck_n, g->stack_n,
        g->exposed, g->said, g->over, g->winner, g->idle_passes, g->starter,
        g->b_open, g->b_sender, g->b_said, g->b_call, g->b_exposed_at_open,
        g->b_said_at_open, g->b_had_terminal, g->b_created, g->t_drew,
    };
    uint16_t wide[] = { g->reshuffles, g->actions, g->turns, g->bubbles, g->b_floor,
                        g->b_rec, g->hist_n };
    h = fnv(h, small, (int)sizeof small);
    h = fnv(h, wide, (int)sizeof wide);
    h = fnv(h, g->deck, g->deck_n);
    h = fnv(h, g->stack, g->stack_n);
    for (int s = 0; s < g->n; s++) {
        h = fnv(h, &g->hand_n[s], 1);
        h = fnv(h, g->hand[s], g->hand_n[s]);
    }
    h = fnv(h, g->hist, g->hist_n * (int)sizeof(PkAct));
    h = fnv(h, g->seed, 32);
    return h;
}

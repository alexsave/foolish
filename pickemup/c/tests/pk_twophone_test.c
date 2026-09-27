/* Two phones, one game, end to end through the iOS bridge: the steps
 * pickemup/docs/SIM_VERIFICATION.md owes the simulator, played through the
 * same entry points Swift calls (ios/include/pk_api.h), so a flow that works
 * here is the flow the extension drives.
 *
 * TWO PHONES, ONE RESIDENT SLOT. pk_api.c keeps one resident message (the
 * extension's process has one). A phone here is what a device keeps between
 * bubbles: its identity bytes, its nickname and its seat records (the App
 * Group's). Switching phone saves the records of the phone put down and
 * loads the other's, drops the sender fact (it was about the other screen)
 * and then ADOPTS the newest link of the thread with pk_api_read, exactly as
 * a tap on the bubble does. A draft is never carried across a switch: every
 * bubble is staged, written with pk_api_text, committed and only then handed
 * over, which is how Messages delivers it.
 *
 * THE ORACLE IS THIS FILE'S OWN. Every expected event list, count and
 * caption below is derived from RULES_AND_KERNEL.md (5.2, 5.3, 6.2) by the
 * test, from what the phone in hand staged, never read back from the kernel:
 * the captions are composed from the English of section 6 with the 36-column
 * rule, the event kinds from the ordering guarantees of 5.3, the hand sizes
 * from a count of 7 dealt plus every draw and penalty less every play.
 *
 * THE SEED IS FOUND, NOT CHOSEN. The script steers each phone at each step
 * towards the next step still owed (a wild with a suit, a +2, a Skip, a
 * Reverse, a reshuffle, a catch, a "Last card!", a win) and plays a plain
 * number or draws and passes otherwise. A seed that cannot reach every step
 * is skipped silently; the first that can is then played again with every
 * assertion on. The steering reads nothing an assertion checks.
 *
 * The bridge is compiled into this file (the TESTS rule builds the kernel
 * sources alone), so the calls below are the shipped pk_api.c's. */
#include "../ios/pk_api.c"
#include <stdio.h>
#include <stdlib.h>

/* ---- the harness: named steps, every assertion counted ---------------------- */

static int         g_checks, g_fails, g_quiet;
static const char *g_step = "";

#define STEP(name) (g_step = (name))
#define OK(c, ...) do {                                                          \
        int ok_ = (c) ? 1 : 0;                                                   \
        if (g_quiet) break;                                                      \
        g_checks++;                                                              \
        if (!ok_) {                                                              \
            g_fails++;                                                           \
            if (g_fails <= 40) {                                                 \
                fprintf(stderr, "FAIL %s:%d [%s] %s: ", __FILE__, __LINE__,      \
                        g_step, #c);                                             \
                fprintf(stderr, __VA_ARGS__);                                    \
                fputc('\n', stderr);                                             \
            }                                                                    \
        }                                                                        \
    } while (0)

/* A step the seed cannot reach: the script gives up on this seed. */
static const char *g_why;
#define STEER(c, why) do { if (!(c)) { g_why = (why); return 0; } } while (0)

/* ---- cards, decoded here from the id order of 3.2 (not the kernel's) ------ */

enum { R_SKIP = 10, R_REV = 11, R_PLUS2 = 12, R_WILD = 13, R_WILD4 = 14 };

static int rank_of(int c)
{
    if (c >= 100 && c < 104) return R_WILD4;
    if (c >= 96 && c < 100) return R_WILD;
    int k = c % 24;
    return k < 18 ? k / 2 + 1 : k < 20 ? R_SKIP : k < 22 ? R_REV : R_PLUS2;
}
static int suit_of(int c) { return c < 96 ? c / 24 : 4; }
static int is_num(int c)  { return rank_of(c) <= 9; }

static const char *SUITS[4] = { "circles", "triangles", "squares", "diamonds" };

/* 6.1 */
static void card_words(int c, char *out, int cap)
{
    switch (rank_of(c)) {
    case R_WILD:  snprintf(out, (size_t)cap, "wild"); break;
    case R_WILD4: snprintf(out, (size_t)cap, "wild +4"); break;
    case R_SKIP:  snprintf(out, (size_t)cap, "skip on %s", SUITS[suit_of(c)]); break;
    case R_REV:   snprintf(out, (size_t)cap, "reverse on %s", SUITS[suit_of(c)]); break;
    case R_PLUS2: snprintf(out, (size_t)cap, "+2 on %s", SUITS[suit_of(c)]); break;
    default:      snprintf(out, (size_t)cap, "%d of %s", rank_of(c), SUITS[suit_of(c)]); break;
    }
}

/* ---- the phones --------------------------------------------------------------- */

enum { A = 0, B = 1 };                           /* A made the game, B joined it */
static const char *NICK[2] = { "Alex", "Bo" };
static uint8_t recs[2][PK_API_REC_BYTES];
static int     recn[2];
static int     holding = -1;                     /* whose phone is in hand       */

static void pick_up(int p)
{
    if (holding >= 0) recn[holding] = pk_api_seats_save(recs[holding], PK_API_REC_BYTES);
    pk_api_seats_load(recs[p], recn[p]);
    pk_api_sender(NULL, 0, -1);
    uint8_t id[16];
    for (int k = 0; k < 16; k++) id[k] = (uint8_t)(p * 53 + k * 7 + 1);
    pk_api_me(id, 16);
    pk_api_nickname((const uint8_t *)NICK[p], (int)strlen(NICK[p]));
    holding = p;
}

static void reset_phones(void)
{
    holding = -1;
    memset(recn, 0, sizeof recn);
}

/* The thread: its newest bubble and who sent it. */
static char tip[PK_API_TEXT_MAX];
static int  tip_from = -1;

/* Tap the newest bubble on phone p, as its sender or not, in a DM. */
static int open_tip(int p)
{
    pick_up(p);
    pk_api_sender(tip, 1, tip_from == p);
    return pk_api_read(tip);
}

static const PkApiTable *table_(void) { return (const PkApiTable *)pk_api_table(); }
static PkView VM, VA;                             /* copies: the bridge's storage is reused */
static const PkView *vme(void) { VM = *(const PkView *)pk_api_view(PK_API_ME); return &VM; }
static const PkView *vall(void) { VA = *(const PkView *)pk_api_view(PK_API_ALL); return &VA; }

/* ---- the oracle for one bubble -------------------------------------------------- */

typedef struct { uint8_t kind; int seat, n, i; } Ev;   /* -1: not checked */
#define EXP_CAP 1024

static struct {
    int  sender;
    int  said, call, hit;                  /* bubble-level; call -1 for none         */
    Ev   body[EXP_CAP]; int nb;             /* the turns, in the order 5.3.3 says     */
    Ev   all[EXP_CAP];  int n;              /* the whole bubble, built at seal        */
    /* the caption's facts (6.2) */
    int  turn_draws, total_draws, last, last_card, last_drew, wild_suit, reshuffled, next;
    int  over, winner, win_card;
    /* pk_since's */
    int  drawn[2], pen[2], plays[2], resh, skipped;
    int  short_supply;
} X;

static int cnt[2];                          /* hand sizes, counted here             */
static int resh_total;                      /* reshuffles so far in the game        */

static void ev_push(Ev *list, int *n, int kind, int seat, int nn, int i)
{
    if (*n < EXP_CAP) { list[*n].kind = (uint8_t)kind; list[*n].seat = seat; list[*n].n = nn; list[*n].i = i; }
    (*n)++;
}
#define BODY(k, s, n, i) ev_push(X.body, &X.nb, (k), (s), (n), (i))

static void bubble_begin(int sender)
{
    memset(&X, 0, sizeof X);
    X.sender = sender;
    X.call = -1;
    X.next = -1;
    X.winner = -1;
}

/* A reshuffle triple (1.9): all but the top, r-th of the game. */
static void expect_reshuffle(int m)
{
    resh_total++;
    X.resh++;
    X.reshuffled = 1;
    BODY(PK_EV_RESHUFFLE_GATHER, PK_SEAT_NONE, m, -1);
    BODY(PK_EV_RESHUFFLE_SHUFFLE, PK_SEAT_NONE, m, resh_total & 0xFF);
    BODY(PK_EV_RESHUFFLE_DONE, PK_SEAT_NONE, m, -1);
}

/* A penalty (1.6, 1.8): PENALTY, then one PENALTY_DRAW per card, a
 * reshuffle wherever the deck runs dry. */
static void expect_penalty(int victim, int owed, int why, int deck, int stack, Ev *list, int *n)
{
    ev_push(list, n, PK_EV_PENALTY, victim, owed, why);
    for (int i = 1; i <= owed; i++) {
        if (deck == 0) {
            if (stack < 2) { X.short_supply = 1; return; }
            resh_total++;
            X.resh++;
            X.reshuffled = 1;
            ev_push(list, n, PK_EV_RESHUFFLE_GATHER, PK_SEAT_NONE, stack - 1, -1);
            ev_push(list, n, PK_EV_RESHUFFLE_SHUFFLE, PK_SEAT_NONE, stack - 1, resh_total & 0xFF);
            ev_push(list, n, PK_EV_RESHUFFLE_DONE, PK_SEAT_NONE, stack - 1, -1);
            deck = stack - 1;
            stack = 1;
        }
        deck--;
        ev_push(list, n, PK_EV_PENALTY_DRAW, victim, owed, i);
        cnt[victim]++;
        X.pen[victim]++;
    }
}

/* ---- staging, on the phone in hand (its seat is `holding`) ------------------------ */

static int me_(void) { return holding; }

static int do_draw(void)
{
    const PkView *v = vme();
    int deck = v->deck_n, stack = v->stack_n, top = v->top, suit = v->live_suit;
    int r = pk_api_draw();
    OK(r == 1, "a draw on my turn is applied");
    if (r != 1) return 0;
    if (deck == 0) {
        expect_reshuffle(stack - 1);
        v = vme();
        STEP(g_step);
        OK(v->stack_n == 1 && v->top == top && v->live_suit == suit,
           "the reshuffle leaves the pile its top card and suit (stack %d top %d)", v->stack_n, v->top);
        OK(v->deck_n == stack - 2, "the deck refilled from the pile less its top, less the card drawn (%d, want %d)",
           v->deck_n, stack - 2);
    }
    X.turn_draws++;
    X.total_draws++;
    X.drawn[me_()]++;
    cnt[me_()]++;
    BODY(PK_EV_DRAW, me_(), X.turn_draws, X.turn_draws);
    v = vme();
    OK(v->my_n == cnt[me_()] && v->my_hand[v->my_n - 1] != PK_CARD_HIDDEN, "the drawn card is in my hand, face up to me");
    return 1;
}

static int do_play(int pos, int suit)
{
    const PkView *v = vme();
    int c = v->my_hand[pos], deck = v->deck_n, stack = v->stack_n, me = me_(), other = me ^ 1;
    int r = pk_api_play(pos, suit);
    OK(r == 1, "the play is applied (card %d)", c);
    if (r != 1) return 0;
    int before = cnt[me]--;
    X.plays[me]++;
    BODY(PK_EV_PLAY, me, -1, pos);
    if (rank_of(c) >= R_WILD && before > 1) BODY(PK_EV_WILD_SUIT, me, -1, -1);
    X.last = PK_EV_PLAY;
    X.last_card = c;
    X.last_drew = X.turn_draws;
    X.wild_suit = suit;
    X.turn_draws = 0;
    if (cnt[me] == 0) {                            /* 1.7: out, the action does nothing */
        X.over = PK_OVER_OUT;
        X.winner = me;
        X.win_card = c;
        return 1;
    }
    int next = other;
    switch (rank_of(c)) {
    case R_SKIP:  BODY(PK_EV_SKIP, other, -1, -1); X.skipped |= 1 << other; next = me; break;
    case R_REV:   BODY(PK_EV_REVERSE_AS_SKIP, other, -1, -1); X.skipped |= 1 << other; next = me; break;
    case R_PLUS2: expect_penalty(other, 2, PK_PEN_PLUS2, deck, stack + 1, X.body, &X.nb); next = me; break;
    case R_WILD4: expect_penalty(other, 4, PK_PEN_WILD4, deck, stack + 1, X.body, &X.nb); next = me; break;
    default: break;
    }
    BODY(PK_EV_TURN_TO, next, -1, -1);
    X.next = next;
    v = vme();
    OK(v->turn == next, "the turn goes where 1.6 sends it (%d, want %d)", v->turn, next);
    OK(v->my_n == cnt[me], "my hand is one card smaller (%d, want %d)", v->my_n, cnt[me]);
    return 1;
}

static int do_pass(void)
{
    int r = pk_api_pass();
    OK(r == 1, "the pass is applied");
    if (r != 1) return 0;
    BODY(PK_EV_PASS, me_(), X.turn_draws, -1);
    X.last = PK_EV_PASS;
    X.last_drew = X.turn_draws;
    X.turn_draws = 0;
    X.next = me_() ^ 1;
    BODY(PK_EV_TURN_TO, X.next, -1, -1);
    return 1;
}

/* The whole bubble's events, 5.3.3: BEGIN, SAY_IT, CALL_OUT, the turns, the
 * catch's outcome at seal, WIN and the REVEALs, END. */
static void bubble_seal(int deck, int stack)
{
    X.n = 0;
    ev_push(X.all, &X.n, PK_EV_BUBBLE_BEGIN, X.sender, -1, -1);
    if (X.said) ev_push(X.all, &X.n, PK_EV_SAY_IT, X.sender, -1, -1);
    if (X.call >= 0) ev_push(X.all, &X.n, PK_EV_CALL_OUT, X.call, -1, -1);
    for (int i = 0; i < X.nb && X.n < EXP_CAP; i++) X.all[X.n++] = X.body[i];
    if (X.call >= 0) {
        if (X.hit) {
            ev_push(X.all, &X.n, PK_EV_CALL_HIT, X.call, -1, -1);
            if (!X.over) expect_penalty(X.call, 2, PK_PEN_CAUGHT, deck, stack, X.all, &X.n);
        } else {
            ev_push(X.all, &X.n, PK_EV_CALL_MISS, X.sender, -1, -1);
            if (!X.over) expect_penalty(X.sender, 1, PK_PEN_WRONG, deck, stack, X.all, &X.n);
        }
    }
    if (X.over) {
        ev_push(X.all, &X.n, PK_EV_WIN, X.winner, -1, PK_OVER_OUT);
        for (int s = 0; s < 2; s++)
            for (int i = 1; i <= cnt[s]; i++) ev_push(X.all, &X.n, PK_EV_REVEAL, s, cnt[s], i);
    }
    ev_push(X.all, &X.n, PK_EV_BUBBLE_END, X.sender, -1, -1);
}

/* ---- the caption, composed from section 6's English ------------------------------ */

enum { CAPTION_MAX = 36 };

static void append(char *out, const char *clause, int *stop)
{
    if (*stop || !clause[0]) return;
    size_t n = strlen(out);
    if (!n) { strcpy(out, clause); return; }
    const char *join = out[n - 1] == '!' || out[n - 1] == '?' ? " " : ". ";
    if (n + strlen(join) + strlen(clause) > CAPTION_MAX) { *stop = 1; return; }
    strcat(out, join);
    strcat(out, clause);
}

static void expected_caption(char *out)
{
    char c[160], card[64];
    const char *who = NICK[X.sender];
    int stop = 0;
    out[0] = 0;
    if (X.over) {
        snprintf(c, sizeof c, rank_of(X.win_card) >= R_WILD ? "%s went out on a wild and wins" : "%s is out and wins",
                 NICK[X.winner]);
        append(out, c, &stop);
    }
    if (X.call >= 0) {
        if (X.hit) snprintf(c, sizeof c, "%s caught %s. %s draws two", who, NICK[X.call], NICK[X.call]);
        else       snprintf(c, sizeof c, "%s called %s wrong and draws one", who, NICK[X.call]);
        append(out, c, &stop);
    }
    if (X.said) { snprintf(c, sizeof c, "%s: Last card!", who); append(out, c, &stop); }
    if (X.over != PK_OVER_OUT) {
        int drew = X.last ? X.last_drew : X.total_draws;
        const char *target = NICK[X.sender ^ 1];
        c[0] = 0;
        if (X.last == PK_EV_PASS) {
            if (drew) snprintf(c, sizeof c, "%s drew %d and passed", who, drew);
            else      snprintf(c, sizeof c, "%s passed", who);
        } else if (X.last == PK_EV_PLAY) {
            card_words(X.last_card, card, sizeof card);
            switch (rank_of(X.last_card)) {
            case R_WILD4: snprintf(c, sizeof c, "%s played wild +4, now %s. %s draws four", who, SUITS[X.wild_suit], target); break;
            case R_PLUS2: snprintf(c, sizeof c, "%s played +2. %s draws two", who, target); break;
            case R_SKIP:  snprintf(c, sizeof c, "%s skipped %s", who, target); break;
            case R_REV:   snprintf(c, sizeof c, "%s reversed and goes again", who); break;
            case R_WILD:
                if (drew) snprintf(c, sizeof c, "%s drew %d and played wild", who, drew);
                else      snprintf(c, sizeof c, "%s played wild, now %s", who, SUITS[X.wild_suit]);
                break;
            default:
                if (drew) snprintf(c, sizeof c, "%s drew %d and played %s", who, drew, card);
                else      snprintf(c, sizeof c, "%s played %s", who, card);
            }
        } else if (drew == 1) snprintf(c, sizeof c, "%s drew a card", who);
        else if (drew)        snprintf(c, sizeof c, "%s drew %d", who, drew);
        append(out, c, &stop);
    }
    if (X.reshuffled) append(out, "The pile went back in the deck", &stop);
    if (!X.over && X.next >= 0) { snprintf(c, sizeof c, "%s to play", NICK[X.next]); append(out, c, &stop); }
}

/* ---- the event checks ------------------------------------------------------------ */

static int half_of(int kind)
{
    switch (kind) {
    case PK_EV_SAY_IT: case PK_EV_CALL_OUT: case PK_EV_DRAW: case PK_EV_PLAY: case PK_EV_WILD_SUIT:
    case PK_EV_PASS: case PK_EV_BUBBLE_BEGIN: case PK_EV_BUBBLE_END:
        return PK_HALF_ACTION;
    case PK_EV_RESHUFFLE_GATHER: case PK_EV_RESHUFFLE_SHUFFLE: case PK_EV_RESHUFFLE_DONE:
        return -1;                                  /* the draw's own half (5.3.5) */
    default:
        return PK_HALF_SETTLE;
    }
}

/* `got` against `want`, for viewer `viewer`; `n_want` of them. */
static void check_events(const PkApiEvents *got, const Ev *want, int n_want, int viewer, int bubble, const char *what)
{
    OK(got != 0, "%s: a plan", what);
    if (!got) return;
    OK(got->n == n_want, "%s: %d events, want %d", what, got->n, n_want);
    int bad = -1, masked_bad = -1, half_bad = -1;
    for (int i = 0; i < got->n && i < n_want; i++) {
        const PkEvent *e = &got->ev[i];
        const Ev *w = &want[i];
        if (bad < 0 && (e->kind != w->kind || (w->seat >= 0 && e->seat != w->seat)
                        || (w->n >= 0 && e->n != w->n) || (w->i >= 0 && e->i != w->i)
                        || (bubble >= 0 && e->bubble != bubble)))
            bad = i;
        int secret = e->kind == PK_EV_DEAL || e->kind == PK_EV_DRAW || e->kind == PK_EV_PENALTY_DRAW;
        if (secret && masked_bad < 0 && ((e->seat == viewer) == (e->card == PK_CARD_HIDDEN))) masked_bad = i;
        if (!secret && e->card == PK_CARD_HIDDEN && masked_bad < 0) masked_bad = i;
        int h = half_of(e->kind);
        if (h < 0 && i + 1 < got->n) {              /* a triple rides with the draw it serves */
            int j = i;
            while (j < got->n && half_of(got->ev[j].kind) < 0) j++;
            h = j < got->n ? got->ev[j].half : e->half;
        }
        if (half_bad < 0 && e->kind != PK_EV_LOBBY_START && e->kind != PK_EV_SHUFFLE && e->kind != PK_EV_DEAL
            && e->kind != PK_EV_FLIP && e->kind != PK_EV_BURY && e->kind != PK_EV_START_CARD && e->half != h)
            half_bad = i;
    }
    OK(bad < 0, "%s: event %d is kind %d seat %d n %d i %d, want kind %d seat %d n %d i %d", what, bad,
       bad >= 0 ? got->ev[bad].kind : 0, bad >= 0 ? got->ev[bad].seat : 0, bad >= 0 ? got->ev[bad].n : 0,
       bad >= 0 ? got->ev[bad].i : 0, bad >= 0 ? want[bad].kind : 0, bad >= 0 ? want[bad].seat : 0,
       bad >= 0 ? want[bad].n : 0, bad >= 0 ? want[bad].i : 0);
    OK(masked_bad < 0, "%s: event %d (kind %d, seat %d) shows a card only its receiver may see, or hides a public one",
       what, masked_bad, masked_bad >= 0 ? got->ev[masked_bad].kind : 0, masked_bad >= 0 ? got->ev[masked_bad].seat : 0);
    OK(half_bad < 0, "%s: event %d (kind %d) is on the wrong side of the cut", what, half_bad,
       half_bad >= 0 ? got->ev[half_bad].kind : 0);
}

/* ---- one bubble, sent and received ------------------------------------------------ */

static char staged_caption[256];

/* The phone in hand has staged its bubble: check the draft, write the link,
 * commit, and hand it to the other phone, which checks everything it gets. */
static int send_and_receive(void)
{
    const int P = holding, Q = P ^ 1;
    const PkApiTable *t = table_();
    OK(t->draft && t->can_send, "the staged bubble can be sent");
    STEER(t->can_send, "a bubble that cannot be sent");
    /* the draft's own plan (channel A): everything but the seal, and never
     * the catch's outcome (D5d). The seed search skips the replays. */
    if (!g_quiet) {
        const PkApiEvents *d = (const PkApiEvents *)pk_api_plan_draft(PK_API_ME);
        static Ev head[EXP_CAP];
        int n = 0;
        ev_push(head, &n, PK_EV_BUBBLE_BEGIN, X.sender, -1, -1);
        if (X.said) ev_push(head, &n, PK_EV_SAY_IT, X.sender, -1, -1);
        if (X.call >= 0) ev_push(head, &n, PK_EV_CALL_OUT, X.call, -1, -1);
        for (int i = 0; i < X.nb && n < EXP_CAP; i++) head[n++] = X.body[i];
        check_events(d, head, n, P, -1, "the sender's draft plan");
    }
    const PkView *v = vme();
    bubble_seal(v->deck_n, v->stack_n);
    STEER(!X.short_supply, "a penalty the deck could not supply");
    char want[256];
    expected_caption(want);
    int cl = g_quiet ? 0 : pk_api_words(PK_API_W_STAGED_CAPTION, 0, staged_caption, sizeof staged_caption);
    OK(cl > 0 && !strcmp(staged_caption, want), "the staged caption: \"%s\", want \"%s\"", staged_caption, want);

    static char link[PK_API_TEXT_MAX];
    int n = pk_api_text(link, sizeof link);
    OK(n > 0, "the bubble writes (%d)", n);
    STEER(n > 0, "a bubble that does not write");
    OK(table_()->draft, "writing the link leaves the draft staged");
    OK(pk_api_commit() == 1 && !table_()->draft, "sent: the draft is sealed");
    const int bubble = table_()->bubbles;
    memcpy(tip, link, (size_t)n + 1);
    tip_from = P;

    /* ---- the other phone ---- */
    OK(open_tip(Q) == 0, "the other phone reads the bubble");
    t = table_();
    OK(t->me == Q && t->by == PK_BY_RECORD, "the receiver resolves to its own seat by its record (me %d by %d)",
       t->me, t->by);
    OK(t->bubbles == bubble && t->sender == P, "the receiver holds the sent bubble (%d from %d)", t->bubbles, t->sender);
    if (g_quiet) return 1;
    v = vme();
    const PkView *all = vall();
    OK(v->my_n == cnt[Q], "the receiver's hand is the count kept here (%d, want %d)", v->my_n, cnt[Q]);
    OK(!memcmp(v->my_hand, all->reveal[Q].card, v->my_n), "the receiver's hand is its own");
    int leaked = 0;
    for (int i = 0; i < all->reveal[P].n; i++)
        for (int j = 0; j < v->my_n; j++) leaked |= v->my_hand[j] == all->reveal[P].card[i];
    OK(!leaked, "the receiver's view holds none of the sender's cards");
    if (!v->over) {
        int counts = 0;
        for (int s = 0; s < PK_MAX_SEATS; s++) counts |= v->reveal[s].n;
        OK(!counts, "no seat's card count reaches the receiver while the game is played (D22)");
    }
    check_events((const PkApiEvents *)pk_api_plan(PK_API_ME, bubble - 1, bubble), X.all, X.n, Q, bubble,
                 "the receiver's plan");
    const PkSince *s = (const PkSince *)pk_api_since(bubble - 1, bubble);
    OK(s != 0, "since the last bubble");
    if (s) {
        int ok = 1;
        for (int k = 0; k < 2; k++)
            ok &= s->drawn[k] == X.drawn[k] && s->penalty[k] == X.pen[k] && s->plays[k] == X.plays[k];
        OK(ok, "pk_since counts the draws, penalty cards and plays (drawn %d/%d pen %d/%d plays %d/%d)",
           s->drawn[0], s->drawn[1], s->penalty[0], s->penalty[1], s->plays[0], s->plays[1]);
        OK(s->reshuffles == X.resh, "pk_since counts the reshuffles (%d, want %d)", s->reshuffles, X.resh);
        OK(s->said == (X.said ? 1 << P : 0) && s->skipped == X.skipped, "pk_since: who said it, who was skipped");
        int hit = X.call >= 0 && X.hit, miss = X.call >= 0 && !X.hit;
        OK(s->caught == (hit ? X.call : PK_SEAT_NONE) && s->caught_by == (hit ? P : PK_SEAT_NONE)
           && s->wrong == (miss ? P : PK_SEAT_NONE), "pk_since: the catch");
    }
    char got[256];
    int gl = pk_api_words(PK_API_W_CAPTION, bubble, got, sizeof got);
    OK(gl > 0 && !strcmp(got, want), "the receiver's caption of bubble %d: \"%s\", want \"%s\"", bubble, got, want);
    if (strcmp(g_step, "filler")) printf("  bubble %3d  %-34s \"%s\"\n", bubble, g_step, got);
    return 1;
}

/* ---- the script ------------------------------------------------------------------- */

enum {
    NEED_UNDO_EMPTY = 1, NEED_WILD = 2, NEED_PLUS2 = 4, NEED_SKIP = 8, NEED_REVERSE = 16,
    NEED_EARLY = 31,
};
enum { PH_EARLY, PH_RESHUFFLE, PH_EXPOSE, PH_CATCH, PH_EXPOSE2, PH_SAY, PH_WIN };

/* The first playable card of rank `r` (a number: any 1..9) leaving at least
 * `keep` cards, or -1. */
static int find(const PkView *v, int r, int keep)
{
    if (v->my_n - 1 < keep) return -1;
    for (int p = 0; p < v->my_n; p++) {
        if (!v->my_playable[p]) continue;
        int k = rank_of(v->my_hand[p]);
        if (r == 0 ? k <= 9 : k == r) return p;
    }
    return -1;
}

/* The shedder's pick: a number, else any action card, else a wild (on the
 * suit after the live one), leaving `keep`; -1 for none. */
static int find_any(const PkView *v, int keep, int *suit)
{
    int p = find(v, 0, keep);
    *suit = 4;
    for (int r = R_SKIP; p < 0 && r <= R_WILD4; r++) p = find(v, r, keep);
    if (p >= 0 && rank_of(v->my_hand[p]) >= R_WILD) *suit = (v->live_suit + 1) % 4;
    return p;
}

/* A plain turn: a number card if one plays and leaves `keep`, else one draw
 * and then that or a pass. `any`: the shedder, who plays whatever plays. */
static int filler_(int keep, int any)
{
    const PkView *v = vme();
    int suit = 4;
    int p = any ? find_any(v, keep, &suit) : find(v, 0, keep);
    if (p >= 0) return do_play(p, suit);
    STEER(v->can_draw, "nothing to draw and nothing plain to play");
    if (!do_draw()) return 0;
    v = vme();
    p = any ? find_any(v, keep, &suit) : find(v, 0, keep);
    if (p >= 0) return do_play(p, suit);
    return do_pass();
}
static int filler(int keep) { return filler_(keep, 0); }

static int play_game(const uint8_t seed[32])
{
    reset_phones();
    resh_total = 0;
    cnt[A] = cnt[B] = 7;
    static char lobby[PK_API_TEXT_MAX];
    char line[256];

    /* ---- the lobby: Alex makes it, Bo joins, and the join fills the table ---- */
    STEP("S1 lobby");
    pick_up(A);
    OK(pk_api_new(seed, 1) == 0, "Alex makes a lobby");
    const PkApiTable *t = table_();
    OK(t->phase == PK_PHASE_WAITING && t->me == A && t->n_seats == 1 && t->offered == PK_LOBBY_WAITING,
       "Alex sits alone in seat 0, waiting");
    OK(pk_api_words(PK_API_W_INVITE, 0, line, sizeof line) > 0
       && !strcmp(line, "Alex wants a game of Pick 'Em Up. Tap to join"), "the invitation's caption: \"%s\"", line);
    OK(pk_api_words(PK_API_W_STAGED_CAPTION, 0, line, sizeof line) == 0, "a lobby bubble has no game caption");
    int n = pk_api_text(lobby, sizeof lobby);
    OK(n > 0, "the invitation writes");
    STEER(n > 0, "no invitation");
    memcpy(tip, lobby, (size_t)n + 1);
    tip_from = A;

    STEP("S2 join starts the game");
    OK(open_tip(B) == 0, "Bo opens the invitation");
    t = table_();
    OK(t->me == PK_SEAT_NONE && t->offered == PK_LOBBY_JOIN && t->can_join_start,
       "Bo is not seated and is offered join-and-start (4.6.5)");
    OK(pk_api_join_start() == B, "Bo joins and starts in one bubble");
    t = table_();
    OK(t->phase == PK_PHASE_LIVE && t->me == B && t->starter == B && t->n_seats == 2 && t->bubbles == 0,
       "a live game, Bo in seat 1 and the starter");
    OK(pk_api_words(PK_API_W_CAPTION, 0, line, sizeof line) > 0 && !strcmp(line, "Cards dealt. Bo goes first"),
       "the deal's caption: \"%s\"", line);
    const PkView *v = vme();
    OK(v->my_n == 7 && v->turn == B, "Bo holds seven and moves first (D19, D28)");
    static uint8_t deal_a[7];
    memcpy(deal_a, vall()->reveal[A].card, 7);

    /* ---- Bo's first turn, in the start bubble: three draws, then a play ---- */
    STEP("S4 three draws then a play");
    bubble_begin(B);
    for (int k = 0; k < 3; k++) {
        if (!do_draw()) return 0;
        OK(!table_()->can_send, "mid-turn: the draft cannot be sent");
        OK(pk_api_undo() == 0, "a draw does not come back (D8)");
    }
    v = vme();
    uint8_t drawn[3];
    memcpy(drawn, v->my_hand + 7, 3);
    int p = find(v, 0, 2);
    STEER(p >= 0, "no plain card after three draws");
    STEP("S4b undo a staged play");
    {
        int c = v->my_hand[p];
        OK(pk_api_play(p, 4) == 1, "Bo stages the play");
        OK(vme()->my_n == 9 && vme()->top == c, "the card is on the pile");
        OK(pk_api_undo() == 1, "the staged play comes back");
        v = vme();
        OK(v->my_n == 10 && !memcmp(v->my_hand + 7, drawn, 3) && v->my_hand[p] == c,
           "the play came home and the three drawn cards stayed (D8)");
        OK(table_()->draft && !table_()->can_send, "the draft still holds the draws");
        OK(pk_api_words(PK_API_W_SUBLINE, 0, line, sizeof line) >= 0, "a subline");
    }
    STEP("S4 three draws then a play");
    if (!do_play(p, 4)) return 0;
    {
        const PkApiEvents *d = (const PkApiEvents *)pk_api_plan_draft(PK_API_ME);
        int ok = d && d->n == 6 && d->ev[0].kind == PK_EV_BUBBLE_BEGIN && d->ev[4].kind == PK_EV_PLAY;
        for (int k = 1; ok && k <= 3; k++) ok = d->ev[k].kind == PK_EV_DRAW && d->ev[k].card == drawn[k - 1];
        OK(ok, "the strip: three backs, then the played card (chips_after_draws)");
    }
    if (!send_and_receive()) return 0;

    /* Alex's phone also saw the deal: bubble 0, round robin from seat 1 */
    STEP("S2 the deal on the receiver");
    {
        const PkApiEvents *d = (const PkApiEvents *)pk_api_plan(PK_API_ME, -1, 0);
        OK(d && d->n > 18, "the deal's plan");
        if (d && d->n > 18) {
            OK(d->ev[0].kind == PK_EV_LOBBY_START && d->ev[0].seat == B && d->ev[0].n == 2, "LOBBY_START names Bo");
            OK(d->ev[1].kind == PK_EV_SHUFFLE && d->ev[1].n == PK_DECK, "one SHUFFLE of 104");
            int ok = 1, mine = 0, bad = -1;
            for (int j = 0; j < 14; j++) {
                const PkEvent *e = &d->ev[2 + j];
                int seat = (1 + j) % 2;
                if (e->kind != PK_EV_DEAL || e->seat != seat || e->n != j / 2 + 1 || e->i != j + 1) { ok = 0; if (bad < 0) bad = j; }
                if (seat == A) { if (e->card != deal_a[mine++]) ok = 0; }
                else if (e->card != PK_CARD_HIDDEN) ok = 0;
            }
            OK(ok, "fourteen DEALs, one card at a time from seat 1, Bo's hidden, Alex's in hand order (first wrong %d)", bad);
            int k = 16;
            while (k + 1 < d->n && d->ev[k].kind == PK_EV_FLIP && d->ev[k + 1].kind == PK_EV_BURY) k += 2;
            OK(k + 2 < d->n && d->ev[k].kind == PK_EV_FLIP && d->ev[k + 1].kind == PK_EV_START_CARD
               && d->ev[k + 1].card == d->ev[k].card && is_num(d->ev[k].card)
               && d->ev[k + 2].kind == PK_EV_TURN_TO && d->ev[k + 2].seat == B && k + 3 == d->n,
               "FLIP and BURY pairs, a number START_CARD, TURN_TO Bo");
        }
    }

    int need = NEED_EARLY, phase = PH_EARLY, x = -1;
    static char exposing[PK_API_TEXT_MAX];
    for (int round = 0; round < 200; round++) {
        /* who sends next: the turn seat, or an exposed player saying it */
        OK(open_tip(A) == 0 && table_()->me == A, "Alex's phone resolves to seat 0");
        int turn = vme()->turn;
        int sender = turn;
        if (phase == PH_SAY) sender = x;
        if (sender != A) {
            OK(open_tip(sender) == 0 && table_()->me == sender, "the sender's phone resolves to its own seat");
        }
        v = vme();
        bubble_begin(sender);

        if (phase == PH_SAY) {
            STEP("S9 Last card! in a later bubble");
            OK(v->my_exposed && v->turn != sender, "exposed, out of turn, it may be said now");
            OK(pk_api_say_it() == 1, "Last card! is staged");
            X.said = 1;
            OK(pk_api_unsay() == 1 && !vme()->draft_said && pk_api_say_it() == 1 && vme()->draft_said,
               "unsay and say again");
            {
                /* ---- a rival: the other catches off the same parent, unsent ---- */
                STEP("S11 Rule P");
                static char said_link[PK_API_TEXT_MAX], rival[PK_API_TEXT_MAX];
                int ns = pk_api_text(said_link, sizeof said_link);
                OK(ns > 0, "the say bubble writes");
                pick_up(sender ^ 1);
                pk_api_sender(exposing, 1, 0);
                OK(pk_api_read(exposing) == 0 && (vme()->can_call & (1 << sender)), "the other may catch the exposed");
                OK(pk_api_catch(sender) == 1, "the other stages a catch off the same bubble");
                int nr = pk_api_text(rival, sizeof rival);
                OK(nr > 0, "the rival writes");
                OK(pk_api_prefer(said_link, rival) < 0 && pk_api_prefer(rival, said_link) > 0,
                   "saying it beats a catch that answered the same bubble (4.8.5, D26)");
                OK(pk_api_cancel() == 1 && !table_()->draft, "the rival catch is cancelled");
                OK(open_tip(sender) == 0, "back to the exposed player's phone");
                OK(pk_api_say_it() == 1, "the say is staged again");
                STEP("S9 Last card! in a later bubble");
            }
            if (!send_and_receive()) return 0;
            v = vme();
            OK((v->said & (1 << sender)) && !(v->can_call & (1 << sender)),
               "the LAST stamp shows and the fan cannot be caught (D5c)");
            OK(pk_api_words(PK_API_W_SUBLINE, 0, line, sizeof line) >= 0, "a subline");
            {
                /* ---- a stale bubble tapped after a newer one ---- */
                STEP("S11 Rule P");
                OK(pk_api_check(exposing) == 0, "the older bubble still reads");
                OK(pk_api_prefer(tip, exposing) < 0 && pk_api_prefer(exposing, tip) > 0,
                   "Rule P keeps the newer bubble, whichever way round");
                OK(pk_api_common(tip, exposing) == table_()->bubbles - 1, "the two share every bubble of the older");
                int newest = table_()->bubbles, keep = pk_api_prefer(tip, exposing) < 0;
                OK(pk_api_read(keep ? tip : exposing) == 0 && table_()->bubbles == newest && table_()->tip_said,
                   "the host adopts the newer: its tip said it");
            }
            phase = PH_WIN;
            continue;
        }

        if (phase == PH_EARLY) {
            int done = 0;
            if (!done && (need & NEED_UNDO_EMPTY) && (p = find(v, 0, 3)) >= 0) {
                STEP("S5 an undo that empties the draft");
                OK(pk_api_play(p, 4) == 1 && table_()->draft, "a play is staged with nothing before it");
                OK(pk_api_undo() == 1, "and undone");
                const PkApiEvents *d = (const PkApiEvents *)pk_api_plan_draft(PK_API_ME);
                OK(!table_()->draft && !table_()->can_send && vme()->my_n == cnt[sender] && d && d->n == 0,
                   "the draft is empty: no draft, nothing to send, the card home");
                OK(pk_api_words(PK_API_W_STAGED_CAPTION, 0, line, sizeof line) > 0, "the staged caption falls back to the tip");
                if (!do_play(p, 4)) return 0;
                need &= ~NEED_UNDO_EMPTY;
                done = 1;
            }
            if (!done && (need & NEED_WILD) && (p = find(v, R_WILD, 2)) >= 0) {
                STEP("S6 a wild with a suit");
                OK(pk_api_is_wild(p), "the host is told the wild needs a suit");
                OK(pk_api_play(p, 4) == 0, "a wild with no suit is not a play (D17)");
                int s = (v->live_suit + 2) % 4, c = v->my_hand[p];
                if (!do_play(p, s)) return 0;
                OK(vme()->live_suit == s && vme()->top == c, "the chosen suit is live");
                need &= ~NEED_WILD;
                done = 1;
            }
            if (!done && (need & NEED_PLUS2) && (p = find(v, R_PLUS2, 2)) >= 0) {
                STEP("S7 a +2");
                if (!do_play(p, 4)) return 0;
                OK(vme()->turn == sender, "at two players the +2's sender goes again (1.6)");
                need &= ~NEED_PLUS2;
                done = 1;
            }
            if (!done && (need & NEED_SKIP) && (p = find(v, R_SKIP, 2)) >= 0) {
                STEP("S7 a Skip");
                if (!do_play(p, 4)) return 0;
                need &= ~NEED_SKIP;
                done = 1;
            }
            if (!done && (need & NEED_REVERSE) && (p = find(v, R_REV, 2)) >= 0) {
                STEP("S7 a Reverse");
                int dir = v->dir;
                if (!do_play(p, 4)) return 0;
                OK(vme()->dir == dir && !vme()->show_dir, "a Reverse at two is a Skip: no direction change, no word (D13)");
                need &= ~NEED_REVERSE;
                done = 1;
            }
            if (!done) {
                STEP("filler");
                if (!filler(2)) return 0;
            }
            if (!send_and_receive()) return 0;
            if (!need) phase = PH_RESHUFFLE;
            continue;
        }

        if (phase == PH_RESHUFFLE) {
            if (v->stack_n < 4) {
                STEP("filler");
                if (!filler(2)) return 0;
                if (!send_and_receive()) return 0;
                continue;
            }
            STEP("S8 reshuffle");
            int guard = 0;
            while (vme()->deck_n > 0 && guard++ < PK_DECK)
                if (!do_draw()) return 0;
            OK(vme()->deck_n == 0, "the deck is empty");
            int before = resh_total;
            if (!do_draw()) return 0;
            OK(resh_total == before + 1, "one more draw reshuffled the pile");
            char deck_line[64];
            OK(pk_api_words(PK_API_W_DECK_LEFT, 0, deck_line, sizeof deck_line) > 0, "the deck count line");
            p = find(vme(), 0, 2);
            if (p >= 0) { if (!do_play(p, 4)) return 0; }
            else if (!do_pass()) return 0;
            if (!send_and_receive()) return 0;
            x = sender ^ 1;                 /* the other seat plays down to one */
            phase = PH_EXPOSE;
            continue;
        }

        /* the endgame: x sheds, the other plays plain */
        if (sender != x) {
            if (phase == PH_CATCH) {
                STEP("S8c Caught you! staged");
                OK(v->can_call & (1 << x), "the fan can be tapped");
                OK(pk_api_catch(x) == 1, "the fan is tapped");
                const PkView *w = vme();
                const PkApiEvents *d = (const PkApiEvents *)pk_api_plan_draft(PK_API_ME);
                OK(w->draft_open && w->draft_call == x && !(w->can_call & (1 << x)) && table_()->can_send,
                   "staged: the catch is in the draft, one per bubble, and it could be sent alone");
                OK(d && d->n == 2 && d->ev[1].kind == PK_EV_CALL_OUT && d->ev[1].seat == x && d->ev[1].other == sender,
                   "the draft shows the tap, not its outcome (D5d)");
                OK(vall()->reveal[x].n == 1, "nothing is drawn before Send");
                OK(pk_api_uncall() == 1 && vme()->draft_call == PK_SEAT_NONE && pk_api_catch(x) == 1,
                   "the tap can be taken back and made again");
                X.call = x;
                X.hit = 1;
                STEP("filler");
                if (!filler(2)) return 0;
                STEP("S8c Caught you! staged");
                d = (const PkApiEvents *)pk_api_plan_draft(PK_API_ME);
                int outcome = 0;
                for (int k = 0; d && k < d->n; k++)
                    outcome |= d->ev[k].kind == PK_EV_CALL_HIT || d->ev[k].kind == PK_EV_CALL_MISS;
                OK(!outcome && vall()->reveal[x].n == 1, "with the turn staged too, still no outcome before Send");
                if (!send_and_receive()) return 0;
                STEP("S8d Caught you! after Send");
                OK(vme()->my_n == 3 && cnt[x] == 3, "the caught player drew two (1.8): %d", vme()->my_n);
                phase = PH_EXPOSE2;
                continue;
            }
            STEP("filler");
            if (!filler(2)) return 0;
            if (!send_and_receive()) return 0;
            continue;
        }

        /* x's turn */
        if (phase == PH_WIN && cnt[x] == 1) {
            p = -1;
            for (int k = 0; k < v->my_n; k++) if (v->my_playable[k]) p = k;
            STEER(p >= 0, "the last card does not play");
            STEP("S12 the win");
            if (!do_play(p, 4)) return 0;
            if (!send_and_receive()) return 0;
            goto won;
        }
        STEP("filler");
        const int shed = phase == PH_EXPOSE || phase == PH_EXPOSE2;
        if (shed && cnt[x] == 2 && (p = find(v, 0, 1)) >= 0) {
            STEP(phase == PH_EXPOSE ? "S8b down to one" : "S9b down to one again");
            if (!do_play(p, 4)) return 0;
            OK(pk_api_say_it() == 0 && !vme()->my_exposed, "not in the bubble that exposed me (D3)");
            OK(!vme()->can_draw && table_()->can_send && vme()->turn == (x ^ 1), "a play to one card ends the bubble");
            if (!send_and_receive()) return 0;
            if (phase == PH_EXPOSE) phase = PH_CATCH;
            else { phase = PH_SAY; memcpy(exposing, tip, strlen(tip) + 1); }
            continue;
        }
        if (!filler_(2, shed)) return 0;
        if (!send_and_receive()) return 0;
    }
    g_why = "the game ran past 200 bubbles";
    return 0;

won:
    STEP("S12 the win");
    {
        v = vme();
        const PkApiTable *tt = table_();
        OK(tt->phase == PK_PHASE_FINISHED && v->over == PK_OVER_OUT && v->winner == x, "the game is over, the shedder won");
        int ok = 1;
        for (int s = 0; s < 2; s++) ok &= v->reveal[s].n == cnt[s];
        OK(ok && v->reveal[x].n == 0 && v->reveal[x ^ 1].n > 0, "every hand is revealed: %d and %d",
           v->reveal[0].n, v->reveal[1].n);
        OK(!memcmp(v->reveal[holding].card, v->my_hand, v->my_n), "my own row of the reveal is my hand");
        uint8_t rank[8];
        OK(pk_api_ranks(rank) == 2 && rank[0] == x && rank[1] == (x ^ 1), "the ranking: the winner, then the other");
        char want[64];
        snprintf(want, sizeof want, "1. %s", NICK[x]);
        OK(pk_api_words(PK_API_W_RANK_ROW, 0, line, sizeof line) > 0 && !strcmp(line, want), "results row 1: \"%s\"", line);
        snprintf(want, sizeof want, "2. %s (You)", NICK[x ^ 1]);
        OK(pk_api_words(PK_API_W_RANK_ROW, 1, line, sizeof line) > 0 && !strcmp(line, want), "results row 2: \"%s\"", line);
        snprintf(want, sizeof want, "%s wins", NICK[x]);
        OK(pk_api_words(PK_API_W_HEADLINE, 0, line, sizeof line) > 0 && !strcmp(line, want), "the loser's headline: \"%s\"", line);
        OK(open_tip(x) == 0 && pk_api_words(PK_API_W_HEADLINE, 0, line, sizeof line) > 0 && !strcmp(line, "You win"),
           "the winner's headline: \"%s\"", line);
    }

    /* ---- the seat resolver with the records gone (a reinstall): the tag ---- */
    STEP("S13 seat resolver");
    for (int ph = 0; ph < 2; ph++) {
        pick_up(ph);
        pk_api_seats_load(0, 0);
        OK(pk_api_read(tip) == 0 && table_()->me == ph && table_()->by == PK_BY_TAG,
           "phone %d with no records finds its own seat by its tag", ph);
        pk_api_seats_load(recs[ph], recn[ph]);
        OK(pk_api_read(tip) == 0 && table_()->me == ph && table_()->by == PK_BY_RECORD,
           "phone %d with its records finds it by the record", ph);
    }
    return 1;
}

static void seed_k(uint8_t seed[32], int k)
{
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 31 + k * 7 + (k >> 8) * 13 + 5);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    /* The shipped kernel finds seed k=1 on the second try. The bound keeps a
     * broken kernel from searching for minutes; with no seed found, seed 0 is
     * played with the assertions on anyway, so the red names what broke. */
    int tries = argc > 1 ? atoi(argv[1]) : 100;
    uint8_t seed[32];
    int found = -1;
    g_quiet = 1;
    for (int k = 0; k < tries && found < 0; k++) {
        seed_k(seed, k);
        g_why = 0;
        if (play_game(seed)) found = k;
    }
    g_quiet = 0;
    STEP("seed");
    OK(found >= 0, "a seed that reaches every step within %d tries (last refusal: %s)", tries, g_why ? g_why : "none");
    int k = found >= 0 ? found : 0;
    seed_k(seed, k);
    g_why = 0;
    int ok = play_game(seed);
    STEP("seed");
    OK(ok, "the seed plays every step again with the assertions on (%s)", g_why ? g_why : "");
    printf("twophone: seed k=%d, %d bubbles, %d reshuffles\n", k, table_()->bubbles, resh_total);
    printf("twophone: %d assertions, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

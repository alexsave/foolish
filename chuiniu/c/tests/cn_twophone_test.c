/* One whole three-seat game phone to phone through the bridge entry points
 * only (ios/include/cn_api.h), the way the extension drives them: Alex makes
 * a lobby in a group, Bo and Cy join, Alex starts, and the three play to the
 * end. Every bubble is staged, written with cn_api_text, committed, and then
 * adopted by every phone from its link.
 *
 * TWO PHONES, ONE RESIDENT SLOT (pickemup's rule): a phone here is what a
 * device keeps between bubbles, its identity, nickname and seat records;
 * switching phone saves one and loads the other and drops the sender fact.
 *
 * THE ORACLE IS THIS FILE'S OWN. The test is an omniscient referee: it reads
 * every die once per round through CN_API_ALL (the view tests are given) and
 * from there works out by itself, from DECISIONS R1 to R7, who is on turn,
 * the lowest legal raise, every seat's count, who a call costs a die, who is
 * out and who wins; each caption and headline is composed here from the
 * English, never read back from the kernel. The steering reads nothing an
 * assertion checks except the phone's own dice.
 *
 * The bridge is compiled into this file, so the calls are the shipped
 * cn_api.c's. */
#include "../ios/cn_api.c"
#include "../../../shared/c/test/twophone.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- the phones ----------------------------------------------------------------- */

#define PHONES 3
static const char *NICK[PHONES] = { "Alex", "Bo", "Cy" };
static uint8_t recs[PHONES][CN_API_REC_BYTES];
static int     recn[PHONES];
static int     who = -1;
static char    link[CN_API_TEXT_MAX], older[CN_API_TEXT_MAX];
static char    line[512], want[512];

static void be(int i)
{
    if (who >= 0) recn[who] = cn_api_seats_save(recs[who], CN_API_REC_BYTES);
    cn_api_seats_load(recs[i], recn[i]);
    cn_api_sender(NULL, 0, -1);
    uint8_t id[16];
    for (int k = 0; k < 16; k++) id[k] = (uint8_t)(i * 29 + k + 3);
    cn_api_me(id, 16);
    cn_api_nickname((const uint8_t *)NICK[i], (int)strlen(NICK[i]));
    who = i;
}

static const CnApiTable *table(void) { return (const CnApiTable *)cn_api_table(); }
static const CnView *me_view(void) { return (const CnView *)cn_api_view(CN_API_ME); }
static const char *words(int what, int arg)
{
    line[0] = 0;
    cn_api_words(what, arg, line, sizeof line);
    return line;
}

/* Tap the newest link as phone i. */
static void open_as(int i)
{
    be(i);
    OK(cn_api_adopt(link) == CN_EOK, "%s opens the link", NICK[i]);
}

/* Stage-then-send on the phone in hand: the link it writes becomes the
 * thread's newest. */
static void send(void)
{
    memcpy(older, link, sizeof older);
    int n = cn_api_text(link, sizeof link);
    OK(n > 0 && n < 5000, "a link of %d characters", n);
    OK(cn_api_commit() == 1 || table()->phase == CN_PHASE_WAITING || table()->moves == 0, "committed");
}

/* ---- the referee's English ------------------------------------------------------ */

static const char *WORD[13] = { "", "one", "two", "three", "four", "five", "six", "seven", "eight",
                                "nine", "ten", "eleven", "twelve" };

static void say_bid(char *out, int q, int f, int initial)
{
    char w[16];
    if (q <= 12) snprintf(w, sizeof w, "%s", WORD[q]); else snprintf(w, sizeof w, "%d", q);
    if (initial && q <= 12) w[0] = (char)(w[0] - 'a' + 'A');   /* digits stay digits */
    snprintf(out, 64, q == 1 ? "%s %d" : "%s %ds", w, f);
}

/* ---- the referee's table ----------------------------------------------------------- */

static int dice_left[PHONES], dice[PHONES][5], turn, bid_q, bid_f, bidder, total;

static int live_after(int s)
{
    for (int i = 1; i <= PHONES; i++) if (dice_left[(s + i) % PHONES]) return (s + i) % PHONES;
    return -1;
}

static int live_count(void)
{
    int k = 0;
    for (int s = 0; s < PHONES; s++) k += dice_left[s] > 0;
    return k;
}

/* The referee reads this round's dice once, as an observer. */
static void read_round(void)
{
    const CnView *all = (const CnView *)cn_api_view(CN_API_ALL);
    for (int s = 0; s < PHONES; s++)
        for (int i = 0; i < 5; i++) dice[s][i] = all->all[s * 5 + i];
}

/* The lowest legal raise by R2 and R7 (the referee's own ranking). */
static int min_raise(int *q, int *f)
{
    if (!bid_q) { *q = 1; *f = 2; return 1; }
    *q = bid_q; *f = bid_f + 1;
    if (*f > 6) { *q = bid_q + 1; *f = 2; }
    return *q <= total;
}

/* ---- the game ------------------------------------------------------------------------- */

static void lobby(void)
{
    STEP("the lobby");
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 11);
    be(0);
    OK(cn_api_new(seed, 0) == CN_EOK, "Alex makes a lobby in a group");
    OK(!strcmp(words(CN_API_W_STAGED_CAPTION, 0), "Alex wants a game of Chui Niu. Tap to join"), "%s", line);
    send();
    for (int p = 1; p < PHONES; p++) {
        open_as(p);
        OK(table()->me == CN_SEAT_NONE && table()->offered == CN_LOBBY_JOIN, "%s is offered Join", NICK[p]);
        OK(cn_api_join() == p, "%s takes seat %d", NICK[p], p);
        snprintf(want, sizeof want, "%s joined", NICK[p]);
        OK(!strcmp(words(CN_API_W_STAGED_CAPTION, 0), want), "%s", line);
        snprintf(want, sizeof want, "%d. %s (You)", p + 1, NICK[p]);
        OK(!strcmp(words(CN_API_W_LOBBY_ROW, p), want), "%s", line);
        send();
    }
    open_as(0);
    OK(table()->me == 0 && table()->by == CN_BY_RECORD && table()->offered == CN_LOBBY_START, "Alex may start");
    OK(cn_api_start() == CN_EOK, "Alex starts");
    OK(!strcmp(words(CN_API_W_STAGED_CAPTION, 0), "Dice rolled. Alex bids first"), "%s", line);
    send();
    OK(cn_api_beats_now() == 0, "the sender's own start: no motion until it adopts");
}

static void check_phone(int p)
{
    const CnApiTable *t = table();
    OK(t->me == p && t->phase == CN_PHASE_LIVE, "%s is seat %d in a live game", NICK[p], p);
    const CnView *v = me_view();
    OK(v && v->my_n == dice_left[p], "%s holds %d dice (%d)", NICK[p], dice_left[p], v ? v->my_n : -1);
    for (int s = 0; s < PHONES; s++) OK(v->dice_n[s] == dice_left[s], "%s sees %s on %d", NICK[p], NICK[s], dice_left[s]);
    /* my own dice, sorted, are the ones the referee saw; nobody else's */
    int mine[5], n = dice_left[p];
    memcpy(mine, dice[p], sizeof mine);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && mine[j - 1] > mine[j]; j--) { int x = mine[j]; mine[j] = mine[j - 1]; mine[j - 1] = x; }
    for (int i = 0; i < n; i++) OK(v->my_dice[i] == mine[i], "%s's die %d", NICK[p], i);
    for (int i = 0; i < CN_MAX_DICE; i++) OK(v->all[i] == 0, "%s's view carries nobody's cup", NICK[p]);
    OK(v->turn == turn && v->bid_q == bid_q && v->bid_f == bid_f, "%s sees the table as it is", NICK[p]);
    if (p == turn) {
        int q, f;
        int can = min_raise(&q, &f);
        OK(v->my_turn && v->can_call == (bid_q > 0) && v->can_raise == can, "the menu");
        if (can) OK(v->min_q == q && v->min_f == f, "the lowest raise %d %d (%d %d)", q, f, v->min_q, v->min_f);
        const char *h = !bid_q ? "Your turn: open the bidding" : can ? "Your turn: raise or call Liar" : "Your turn: call Liar";
        OK(!strcmp(words(CN_API_W_HEADLINE, 0), h), "%s: %s", NICK[p], line);
    } else {
        OK(!v->my_turn && !v->can_call && !v->can_raise, "no menu off turn");
        if (dice_left[p]) snprintf(want, sizeof want, "%s's turn", NICK[turn]);
        else snprintf(want, sizeof want, "You're out");
        OK(!strcmp(words(CN_API_W_HEADLINE, 0), want), "%s: %s", NICK[p], line);
    }
}

static int play(void)
{
    int moves = 0, calls = 0, rounds = 1;
    turn = 0; bid_q = bid_f = 0; bidder = -1; total = 15;
    for (int s = 0; s < PHONES; s++) dice_left[s] = 5;
    open_as(0);
    read_round();
    while (live_count() > 1) {
        STEP("a bubble");
        /* every phone opens the newest link and sees its own */
        for (int p = 0; p < PHONES; p++) {
            open_as(p);
            if (moves > 0) OK(cn_api_prefer(older, link) > 0 && cn_api_prefer(link, older) < 0, "the newer bubble wins");
            check_phone(p);
        }
        open_as(turn);
        /* the steering: call a bid that is more than a third of the table
         * plus one, else raise to my most common face */
        int q, f;
        int can = min_raise(&q, &f);
        int best = 2, most = -1;
        for (int face = 2; face <= 6; face++) {
            int c = 0;
            for (int i = 0; i < dice_left[turn]; i++) c += dice[turn][i] == face || dice[turn][i] == 1;
            if (c > most) { most = c; best = face; }
        }
        if (bid_q && (!can || bid_q > total / 3 + 1)) {
            STEP("a call");
            OK(cn_api_call() == 1, "%s calls", NICK[turn]);
            char b[64];
            say_bid(b, bid_q, bid_f, 0);
            snprintf(want, sizeof want, "%s calls %s", NICK[turn], b);
            OK(!strcmp(words(CN_API_W_STAGED_CAPTION, 0), want), "%s", line);
            snprintf(want, sizeof want, "Send to call Liar on %s", b);
            OK(!strcmp(words(CN_API_W_HEADLINE, 0), want), "%s", line);
            OK(!me_view()->revealed, "nothing lifts before it is sent");
            send();
            moves++; calls++;
            /* the referee judges it */
            int count = 0;
            for (int s = 0; s < PHONES; s++)
                for (int i = 0; i < dice_left[s]; i++) count += dice[s][i] == bid_f || dice[s][i] == 1;
            int caller = turn, loser = count >= bid_q ? caller : bidder;
            const CnView *v = me_view();
            OK(v->revealed && v->call_count == count && v->call_loser == loser, "the reveal: %d counted, %s loses",
               count, NICK[loser]);
            for (int s = 0; s < PHONES; s++) OK(v->shown_n[s] == dice_left[s], "every cup shown");
            char cap[64];
            say_bid(cap, bid_q, bid_f, 1);
            snprintf(want, sizeof want, "%s calls. %s was %s, %s loses a die", NICK[caller], cap,
                     loser == caller ? "true" : "false", NICK[loser]);
            dice_left[loser]--;
            total--;
            if (!dice_left[loser] && live_count() > 1) snprintf(want + strlen(want), 64, ". %s is out", NICK[loser]);
            if (live_count() == 1) snprintf(want + strlen(want), 64, ". %s wins", NICK[live_after(loser)]);
            OK(!strcmp(words(CN_API_W_OUTCOME, 0), want), "%s", line);
            bid_q = bid_f = 0; bidder = -1;
            if (live_count() == 1) break;
            turn = dice_left[loser] ? loser : live_after(loser);
            rounds++;
            open_as(turn);
            read_round();
        } else {
            STEP("a bid");
            int rq = q, rf = best;
            if (rf < f && rq == q) rq = q + (bid_q ? 1 : 0);   /* the same quantity needs a higher face */
            if (!bid_q) rq = 1 + dice_left[turn] / 2;
            if (rq > total) { rq = q; rf = f; }
            OK(cn_api_can_raise(rq, rf) == 1, "%s may bid %d %d", NICK[turn], rq, rf);
            OK(cn_api_can_raise(bid_q, bid_f) == 0, "the standing bid is not a raise");
            OK(cn_api_raise(6, 6) == (6 * 5 > (bid_q - 1) * 5 + bid_f - 2 && 6 <= total), "a first try");
            OK(cn_api_raise(rq, rf) == 1, "%s stages %d %d, replacing it", NICK[turn], rq, rf);
            char b[64];
            say_bid(b, rq, rf, 0);
            snprintf(want, sizeof want, "%s bid %s", NICK[turn], b);
            OK(!strcmp(words(CN_API_W_STAGED_CAPTION, 0), want), "%s", line);
            send();
            moves++;
            OK(!strcmp(words(CN_API_W_CAPTION, moves), want), "the sent caption: %s", line);
            /* ADOPTING: the same bubble again moves nothing; a phone a bubble
             * behind plays exactly the one bid */
            OK(cn_api_adopt(link) == CN_EOK && cn_api_beats_now() == 0, "the same bubble: no motion");
            OK(cn_api_read(older) == CN_EOK && cn_api_adopt(link) == CN_EOK, "one bubble behind, then the newest");
            const CnBeats *bb = (const CnBeats *)cn_api_beats_now();
            OK(bb && bb->n == 1 && bb->beat[0].kind == CN_BK_BID && bb->beat[0].q == rq && bb->beat[0].f == rf,
               "one bid pops");
            bid_q = rq; bid_f = rf; bidder = turn;
            turn = live_after(turn);
        }
    }
    STEP("the end");
    int winner = -1;
    for (int s = 0; s < PHONES; s++) if (dice_left[s]) winner = s;
    for (int p = 0; p < PHONES; p++) {
        open_as(p);
        OK(table()->phase == CN_PHASE_FINISHED && table()->game_phase == CN_PH_OVER, "%s sees it finished", NICK[p]);
        const CnView *v = me_view();
        OK(v->winner == winner && v->revealed, "the winner and the last reveal");
        if (p == winner) snprintf(want, sizeof want, "You win");
        else snprintf(want, sizeof want, "%s wins", NICK[winner]);
        OK(!strcmp(words(CN_API_W_HEADLINE, 0), want), "%s: %s", NICK[p], line);
        OK(cn_api_raise(1, 2) == 0 && cn_api_call() == 0, "nothing more to play");
        int m = table()->moves;
        const CnBeats *b = (const CnBeats *)cn_api_beats(m - 1, m);
        OK(b && b->n > 0 && b->beat[b->n - 1].kind == CN_BK_WIN, "the last move's motion ends on the win");
        const CnBeatFrame *fr = (const CnBeatFrame *)cn_api_beats_frame(b->total_ms);
        OK(fr && fr->done && fr->winner == winner && fr->cups_up, "and its last frame: the winner, the cups up");
    }
    printf("cn_twophone_test: %d moves, %d calls, %d rounds, %s wins\n", moves, calls, rounds, NICK[winner]);
    return moves;
}

/* ---- THE LOBBY'S RULES that the group game above never meets
 * (shared/c/msg_lobby_roster): a creator left alone by a leaver is not
 * offered Start, the join that fills a table starts it, and nobody leaves
 * once the game is live. A two-person chat after the game, on Alex's and
 * Bo's phones. */
static void lobby_rules(void)
{
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 13 + 5);
    char start[CN_API_TEXT_MAX], after[CN_API_TEXT_MAX];

    STEP("a lone creator is not offered Start");
    be(0);
    OK(cn_api_new(seed, 1) == CN_EOK, "Alex makes a lobby in a two-person chat");
    OK(table()->n_seats == 1 && table()->offered == CN_LOBBY_WAITING && !table()->can_exit,
       "alone, the newest sender: waiting, offered %d", table()->offered);
    OK(cn_api_start() != CN_EOK && table()->phase == CN_PHASE_WAITING, "Alex cannot roll alone");
    OK(cn_api_text(link, sizeof link) > 0, "the invitation writes");

    STEP("a join that fills the table, then a leave");
    open_as(1);
    OK(cn_api_join() == 1, "Bo joins with a plain join");
    OK(table()->n_seats == 2 && table()->offered == CN_LOBBY_START && table()->can_exit,
       "the table is full: Bo, the newest sender, is offered Start (%d) or a leave", table()->offered);
    OK(!strcmp(words(CN_API_W_LEFT, 1), "Bo left"), "the leave's caption: %s", line);
    OK(cn_api_leave() == CN_EOK && table()->n_seats == 1 && table()->me == CN_SEAT_NONE, "Bo leaves");
    OK(cn_api_text(link, sizeof link) > 0, "the leave writes");
    open_as(0);
    OK(table()->me == 0 && table()->n_seats == 1 && table()->offered == CN_LOBBY_INVITE,
       "alone again, the newest bubble not his: Alex is offered Invite (%d), not Start", table()->offered);
    OK(cn_api_start() != CN_EOK && table()->phase == CN_PHASE_WAITING && table()->n_seats == 1, "and cannot roll alone");

    STEP("the join that fills the table starts it");
    open_as(1);
    OK(table()->offered == CN_LOBBY_JOIN && table()->can_join_start, "Bo is offered join-and-start");
    OK(cn_api_join_start() == 1, "Bo joins and starts in one bubble");
    OK(table()->phase == CN_PHASE_LIVE && table()->n_seats == 2 && table()->starter == 1, "a live game of two");
    int n = cn_api_text(start, sizeof start);
    OK(n > 0, "the start writes");
    memcpy(link, start, sizeof link);

    STEP("a leave once live is refused");
    for (int p = 1; p >= 0; p--) {
        open_as(p);
        OK(table()->phase == CN_PHASE_LIVE && table()->me == p, "%s is in the live game", NICK[p]);
        uint16_t rev = table()->lobby_rev;
        OK(!table()->can_exit, "%s is offered no leave once live", NICK[p]);
        OK(cn_api_leave() != CN_EOK, "%s's leave is refused", NICK[p]);
        OK(table()->phase == CN_PHASE_LIVE && table()->n_seats == 2 && table()->me == p && table()->lobby_rev == rev,
           "the roster is unchanged: %d seats, me %d", table()->n_seats, table()->me);
        OK(cn_api_text(after, sizeof after) == n && !strcmp(after, start), "the bubble encodes no departure");
    }
}

int main(void)
{
    lobby();
    play();
    lobby_rules();
    return report("cn_twophone_test");
}

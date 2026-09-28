/* msg_lobby_roster_test.c - the lobby's rules (msg_lobby_roster.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror msg_lobby_roster.c msg_lobby_roster_test.c -o msg_lobby_roster_test && ./msg_lobby_roster_test
 *
 * No -I: the header is beside this file. Exits 1 on any failure. Capacity in
 * a two-person chat and a group, a new lobby that starts nothing, joins up to
 * the cap, the one control per state, the filling join as the only join that
 * may start, leaving before and after the start, and the roster's changes. */
#include <stdio.h>
#include <string.h>
#include "msg_lobby_roster.h"

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

/* The stand-in game constructor: counts its calls and records what it saw. */
typedef struct { int calls, n, starter; uint8_t seed0; int refuse; } Game;
static int start_game(void *game, const MsgLobbyRoster *l, int starter, const uint8_t seed[32])
{
    Game *g = game;
    g->calls++;
    if (g->refuse) return 0;
    g->n = l->n_seats;
    g->starter = starter;
    g->seed0 = seed[0];
    return 1;
}
static const uint8_t SEED[32] = { 42 };

static MsgLobbyRoster seated(int dm, int cap, int n)
{
    MsgLobbyRoster l;
    msg_lobby_roster_new(&l, dm, cap, 100);
    for (int s = 1; s < n; s++) msg_lobby_roster_join(&l, (uint16_t)(100 + s));
    return l;
}

static void test_cap_and_new(void)
{
    for (int cap = 2; cap <= MSG_LOBBY_ROSTER_MAX_SEATS; cap++) {
        MsgLobbyRoster d, g;
        msg_lobby_roster_new(&d, 1, cap, 7);
        msg_lobby_roster_new(&g, 0, cap, 7);
        OK(msg_lobby_roster_cap(&d) == MSG_LOBBY_ROSTER_DM_CAP, "a two-person chat seats two, whatever the group cap");
        OK(msg_lobby_roster_cap(&g) == cap, "a group seats the group cap");
    }
    MsgLobbyRoster l;
    memset(&l, 0xEE, sizeof l);
    msg_lobby_roster_new(&l, 5, 6, 9);
    OK(l.dm == 1, "any nonzero dm is a two-person chat");
    OK(l.n_seats == 1 && l.who[0] == 9 && l.newest == 0 && l.rev == 0, "the creator alone in seat 0, the newest sender");
    OK(!l.started, "CREATING NEVER DEALS: a new lobby is not started");
    for (int s = 1; s < MSG_LOBBY_ROSTER_MAX_SEATS; s++) OK(l.who[s] == 0, "every other row is clear");
    OK(msg_lobby_roster_offered(&l, 0) == MSG_LOBBY_ROSTER_WAITING, "the creator, who sent the invitation, waits");
    OK(msg_lobby_roster_offered(&l, -1) == MSG_LOBBY_ROSTER_JOIN, "an outsider may join");
    /* the creator alone may not start, in a group or a two-person chat */
    Game g = { 0 };
    for (int dm = 0; dm < 2; dm++) {
        msg_lobby_roster_new(&l, dm, 8, 1);
        OK(!msg_lobby_roster_start(&l, 0, SEED, &g, start_game), "the creator alone cannot start");
        l.newest = MSG_LOBBY_ROSTER_NO_SEAT;
        OK(msg_lobby_roster_offered(&l, 0) == MSG_LOBBY_ROSTER_INVITE, "alone, and the newest bubble is not mine: invite");
        OK(!msg_lobby_roster_start(&l, 0, SEED, &g, start_game), "not even then");
    }
    OK(g.calls == 0, "CREATING NEVER DEALS: no game constructor ran for a lobby of one");
}

static void test_join(void)
{
    for (int dm = 0; dm < 2; dm++)
        for (int gc = 2; gc <= MSG_LOBBY_ROSTER_MAX_SEATS; gc++) {
            MsgLobbyRoster l;
            msg_lobby_roster_new(&l, dm, gc, 100);
            int cap = msg_lobby_roster_cap(&l);
            OK(msg_lobby_roster_join(&l, 100) == -1 && l.n_seats == 1 && l.rev == 0, "the creator cannot join twice");
            for (int s = 1; s < cap; s++) {
                OK(msg_lobby_roster_join(&l, (uint16_t)(100 + s)) == s, "the lowest free seat");
                OK(l.n_seats == s + 1 && l.newest == s && l.rev == s && l.who[s] == 100 + s, "seated, newest, counted");
                OK(msg_lobby_roster_seat_of(&l, (uint16_t)(100 + s)) == s, "found at the seat");
                OK(msg_lobby_roster_join(&l, (uint16_t)(100 + s)) == -1, "already seated");
            }
            OK(msg_lobby_roster_join(&l, 999) == -1 && l.n_seats == cap && l.rev == cap - 1, "full: refused, nothing moves");
            OK(msg_lobby_roster_seat_of(&l, 999) == -1, "a stranger holds no seat");
            OK(msg_lobby_roster_offered(&l, -1) == MSG_LOBBY_ROSTER_FULL, "an outsider at a full table: full");
        }
    MsgLobbyRoster l = seated(0, 8, 3);
    Game g = { 0 };
    OK(msg_lobby_roster_start(&l, 0, SEED, &g, start_game), "started");
    OK(msg_lobby_roster_join(&l, 555) == -1 && l.n_seats == 3, "no join once started");
}

static void test_offered(void)
{
    for (int dm = 0; dm < 2; dm++)
        for (int gc = 2; gc <= MSG_LOBBY_ROSTER_MAX_SEATS; gc++) {
            int cap = dm ? MSG_LOBBY_ROSTER_DM_CAP : gc;
            for (int n = 1; n <= cap; n++)
                for (int newest = -1; newest < n; newest++) {
                    MsgLobbyRoster l = seated(dm, gc, n);
                    l.newest = newest < 0 ? MSG_LOBBY_ROSTER_NO_SEAT : (uint8_t)newest;
                    int starters = 0;
                    for (int me = -1; me <= n; me++) {
                        int o = msg_lobby_roster_offered(&l, me), want;
                        if (me < 0 || me >= n) want = n < cap ? MSG_LOBBY_ROSTER_JOIN : MSG_LOBBY_ROSTER_FULL;
                        else if (n >= 2 && (me != newest || n == cap)) want = MSG_LOBBY_ROSTER_START;
                        else if (n == 1 && me != newest) want = MSG_LOBBY_ROSTER_INVITE;
                        else want = MSG_LOBBY_ROSTER_WAITING;
                        OK(o == want, "one control per state, as the rule says");
                        starters += o == MSG_LOBBY_ROSTER_START;
                        if (me >= 0 && me < n && n == cap && n >= 2)
                            OK(o == MSG_LOBBY_ROSTER_START, "a full table: everyone may start");
                    }
                    if (n >= 2) OK(starters >= 1, "somebody can start");
                    OK(msg_lobby_roster_can_join_and_start(&l) == (n + 1 == cap),
                       "join-and-start exactly when the join fills the table");
                    l.started = 1;
                    for (int me = -1; me <= n; me++) OK(msg_lobby_roster_offered(&l, me) == 0, "nothing offered once started");
                    OK(!msg_lobby_roster_can_join_and_start(&l), "no join-and-start once started");
                }
        }
}

/* THE LAST JOINER STARTS ONLY WHEN THE JOIN FILLS THE TABLE: walk every group
 * cap, join one at a time, and try to start from the joiner's seat. */
static void test_filling_join(void)
{
    for (int dm = 0; dm < 2; dm++)
        for (int gc = 2; gc <= MSG_LOBBY_ROSTER_MAX_SEATS; gc++) {
            MsgLobbyRoster l;
            msg_lobby_roster_new(&l, dm, gc, 100);
            int cap = msg_lobby_roster_cap(&l);
            for (int s = 1; s < cap; s++) {
                int could = msg_lobby_roster_can_join_and_start(&l);
                int j = msg_lobby_roster_join(&l, (uint16_t)(100 + s));
                Game g = { 0 };
                MsgLobbyRoster t = l;
                int started = msg_lobby_roster_start(&t, j, SEED, &g, start_game);
                OK(started == (s + 1 == cap), "the joiner may start exactly when the join filled the table");
                OK(could == started, "and join-and-start said so before the join");
                OK(g.calls == started, "the game constructor ran only for the filling join");
                OK(t.started == started, "started only then");
                if (started) OK(g.n == cap && g.starter == j && g.seed0 == 42, "the whole table, the joiner as starter, the seed");
            }
        }
    /* a two-person chat: the joiner fills it, and both may start */
    MsgLobbyRoster d;
    msg_lobby_roster_new(&d, 1, 8, 7);
    OK(msg_lobby_roster_can_join_and_start(&d), "a two-person chat: join-and-start offered");
    OK(msg_lobby_roster_join(&d, 8) == 1, "the joiner takes seat 1");
    OK(msg_lobby_roster_offered(&d, 1) == MSG_LOBBY_ROSTER_START && msg_lobby_roster_offered(&d, 0) == MSG_LOBBY_ROSTER_START,
       "both offered START");
    OK(msg_lobby_roster_join(&d, 9) < 0, "the chat is full");
}

static void test_start(void)
{
    MsgLobbyRoster l = seated(0, 8, 3);
    Game g = { 0 };
    OK(!msg_lobby_roster_start(&l, 2, SEED, &g, start_game) && g.calls == 0, "the newest joiner with room: refused, no game");
    OK(!msg_lobby_roster_start(&l, -1, SEED, &g, start_game) && !msg_lobby_roster_start(&l, 3, SEED, &g, start_game)
       && g.calls == 0, "an outsider: refused, no game");
    g.refuse = 1;
    OK(!msg_lobby_roster_start(&l, 0, SEED, &g, start_game) && g.calls == 1 && !l.started,
       "a constructor that says no leaves the lobby waiting");
    g.refuse = 0;
    OK(msg_lobby_roster_start(&l, 1, SEED, &g, start_game) && g.calls == 2 && l.started, "seat 1 starts");
    OK(g.n == 3 && g.starter == 1, "at n_seats, starter recorded");
    OK(!msg_lobby_roster_start(&l, 0, SEED, &g, start_game) && g.calls == 2, "no second start");
}

static void test_leave(void)
{
    MsgLobbyRoster l = seated(0, 8, 4);    /* who 100..103 */
    OK(!msg_lobby_roster_can_exit(&l, -1) && !msg_lobby_roster_can_exit(&l, 4), "not seated: no exit");
    for (int s = 0; s < 4; s++) OK(msg_lobby_roster_can_exit(&l, s), "every seat may leave a waiting lobby");
    OK(!msg_lobby_roster_leave(&l, 4) && l.n_seats == 4 && l.rev == 3, "a seat nobody holds cannot leave");
    OK(msg_lobby_roster_leave(&l, 1), "seat 1 leaves");
    OK(l.n_seats == 3 && l.who[0] == 100 && l.who[1] == 102 && l.who[2] == 103 && l.who[3] == 0,
       "later rows moved down, the freed row cleared");
    OK(l.newest == MSG_LOBBY_ROSTER_NO_SEAT && l.rev == 4, "the leaver sent the newest bubble, and has no seat");
    OK(msg_lobby_roster_offered(&l, 2) == MSG_LOBBY_ROSTER_START, "so the last row may start");
    OK(msg_lobby_roster_leave(&l, 0) && l.who[0] == 102, "the new seat 0 goes first");
    OK(msg_lobby_roster_join(&l, 101) == 2 && l.newest == 2, "the leaver may come back, at the end");
    OK(msg_lobby_roster_leave(&l, 2) && msg_lobby_roster_leave(&l, 1) && l.n_seats == 1, "down to one");
    OK(!msg_lobby_roster_can_exit(&l, 0) && !msg_lobby_roster_leave(&l, 0) && l.n_seats == 1,
       "the last one cannot leave");
    OK(msg_lobby_roster_offered(&l, 0) == MSG_LOBBY_ROSTER_INVITE, "and is offered INVITE");
    /* after the start */
    l = seated(0, 8, 3);
    Game g = { 0 };
    msg_lobby_roster_start(&l, 0, SEED, &g, start_game);
    for (int s = -1; s <= 3; s++) OK(!msg_lobby_roster_can_exit(&l, s), "nobody may leave a started game");
    OK(!msg_lobby_roster_leave(&l, 1) && l.n_seats == 3 && l.who[1] == 101, "a leave after the start moves nothing");
}

static void test_plan(void)
{
    MsgLobbyRosterChange ch[MSG_LOBBY_ROSTER_MAX_CHANGES];
    MsgLobbyRoster a = seated(0, 8, 3), b;    /* 100 101 102 */
    OK(msg_lobby_roster_plan(&a, &a, ch) == 0, "no change, no changes");
    b = a;
    msg_lobby_roster_leave(&b, 1);
    msg_lobby_roster_join(&b, 200);           /* 100 102 200 */
    OK(msg_lobby_roster_plan(&a, &b, ch) == 2, "a leave and a join");
    OK(ch[0].kind == MSG_LOBBY_ROSTER_LEFT && ch[0].seat == 1, "the leave first, at the old seat");
    OK(ch[1].kind == MSG_LOBBY_ROSTER_JOINED && ch[1].seat == 2, "then the join, at the new seat");
    OK(msg_lobby_roster_plan(&b, &a, ch) == 2 && ch[0].kind == MSG_LOBBY_ROSTER_LEFT && ch[0].seat == 2
       && ch[1].kind == MSG_LOBBY_ROSTER_JOINED && ch[1].seat == 1, "and backwards");
    /* the whole table replaced: every leave before any join, each in seat order */
    MsgLobbyRoster x = seated(0, 8, 8), y;
    msg_lobby_roster_new(&y, 0, 8, 900);
    for (int s = 1; s < 8; s++) msg_lobby_roster_join(&y, (uint16_t)(900 + s));
    int n = msg_lobby_roster_plan(&x, &y, ch);
    OK(n == MSG_LOBBY_ROSTER_MAX_CHANGES, "eight gone, eight come: the most there can be");
    int order = 1;
    for (int i = 0; i < 8; i++) order &= ch[i].kind == MSG_LOBBY_ROSTER_LEFT && ch[i].seat == i
                                        && ch[8 + i].kind == MSG_LOBBY_ROSTER_JOINED && ch[8 + i].seat == i;
    OK(order, "leaves in seat order, then joins in seat order");
    /* a leave that moves rows down is not a change for those who stayed */
    b = a;
    msg_lobby_roster_leave(&b, 0);
    OK(msg_lobby_roster_plan(&a, &b, ch) == 1 && ch[0].kind == MSG_LOBBY_ROSTER_LEFT && ch[0].seat == 0,
       "only the leaver changes, though every row moved");
}

/* OUT OF CONTRACT, HELD: a group_cap past MAX or below 2, and a hand-built
 * roster whose n_seats is past MAX. The roster sits in a block with a guard
 * tail, so a write past who[] shows as a changed guard
 * (SECURITY_REVIEW_LIFTED.md: a group_cap of 9 wrote who[8]). */
static void test_bounds(void)
{
    struct { MsgLobbyRoster l; uint16_t guard[16]; } b;
    memset(&b, 0, sizeof b);
    memset(b.guard, 0xA5, sizeof b.guard);
    msg_lobby_roster_new(&b.l, 0, 200, 1);
    OK(msg_lobby_roster_cap(&b.l) == MSG_LOBBY_ROSTER_MAX_SEATS, "a group_cap of 200 seats MAX");
    int joined = 0;
    for (int w = 2; w < 40; w++) joined += msg_lobby_roster_join(&b.l, (uint16_t)w) >= 0;
    OK(joined == MSG_LOBBY_ROSTER_MAX_SEATS - 1 && b.l.n_seats == MSG_LOBBY_ROSTER_MAX_SEATS, "joins stop at MAX");
    int intact = 1;
    for (int i = 0; i < 16; i++) intact &= b.guard[i] == 0xA5A5;
    OK(intact, "nothing past who[] was written");

    MsgLobbyRoster z;
    msg_lobby_roster_new(&z, 0, 0, 1);
    OK(msg_lobby_roster_cap(&z) == 2, "a group_cap of 0 seats two");
    msg_lobby_roster_new(&z, 0, 1, 1);
    OK(msg_lobby_roster_cap(&z) == 2, "a group_cap of 1 seats two");

    /* n_seats past MAX: no leave, no walk past who[] */
    struct { MsgLobbyRoster l; uint16_t guard[256]; } h, g;
    memset(&h, 0xA5, sizeof h);
    memset(&g, 0, sizeof g);
    h.l.started = 0;
    h.l.n_seats = 200;
    g.l.n_seats = 0;
    OK(!msg_lobby_roster_leave(&h.l, 150) && !msg_lobby_roster_can_exit(&h.l, 3), "a roster past MAX cannot be left");
    for (int i = 0; i < MSG_LOBBY_ROSTER_MAX_SEATS; i++) h.l.who[i] = (uint16_t)(100 + i);
    OK(msg_lobby_roster_seat_of(&h.l, 107) == 7, "seat_of finds the last handle in who[]");
    OK(msg_lobby_roster_seat_of(&h.l, 0xA5A5) == -1, "seat_of looks no further than who[]");
    MsgLobbyRosterChange ch[512];   /* room for what an unbounded walk would write */
    OK(msg_lobby_roster_plan(&g.l, &h.l, ch) == MSG_LOBBY_ROSTER_MAX_SEATS, "plan counts at most MAX joins");
    OK(msg_lobby_roster_plan(&h.l, &g.l, ch) == MSG_LOBBY_ROSTER_MAX_SEATS, "plan counts at most MAX leaves");
}

int main(void)
{
    test_cap_and_new();
    test_join();
    test_offered();
    test_filling_join();
    test_start();
    test_leave();
    test_plan();
    test_bounds();
    printf("msg_lobby_roster: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

/* The bridge, proved WITHOUT a Mac: every entry point Swift will call, run
 * against real games passed between simulated phones as the link text
 * Messages carries, so a broken boundary fails here rather than in Xcode.
 *
 *     make -C pickemup/c ios-smoke
 *
 * It reads the returned structs through pk_api_layout.h because it is C; the
 * Swift host reads the same pointers through the generated readers. */
#include "include/pk_api.h"
#include "pk_api_layout.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

/* ONE KERNEL, SEVERAL PHONES: each person's identity, nickname and seat
 * records are their own device's, so switching person swaps them - and drops
 * the sender fact, which was about the other phone's screen. */
static uint8_t recs[4][PK_API_REC_BYTES];
static int     recn[4];
static int     who = -1;
static const char *NICK[4] = { "Alex", "Bo", "Cleo", "Dev" };

static void be(int i)
{
    if (who >= 0) recn[who] = pk_api_seats_save(recs[who], PK_API_REC_BYTES);
    pk_api_seats_load(recs[i], recn[i]);
    pk_api_sender(NULL, 0, -1);
    uint8_t id[16];
    for (int k = 0; k < 16; k++) id[k] = (uint8_t)(i * 37 + k);
    pk_api_me(id, 16);
    pk_api_nickname((const uint8_t *)NICK[i], (int)strlen(NICK[i]));
    who = i;
}

static const PkApiTable *table(void) { return (const PkApiTable *)pk_api_table(); }
static const PkView *me_view(void) { return (const PkView *)pk_api_view(PK_API_ME); }

static char links[8][PK_API_TEXT_MAX];

/* Tap `text` as person i, who did (i_sent 1) or did not send it, in a DM. */
static int open_as(int i, const char *text, int dm, int i_sent)
{
    be(i);
    pk_api_sender(text, dm, i_sent);
    return pk_api_read(text);
}

/* One bubble by the seat whose phone is in hand: "Last card!" when it may,
 * then its turn if it is its turn - the first card it can play, else a draw
 * and then that or a pass. */
static void one_bubble(void)
{
    const PkView *v = me_view();
    if (v->my_exposed) pk_api_say_it();
    v = me_view();
    if (v->turn != v->me) return;
    for (int guard = 0; guard < 200; guard++) {
        v = me_view();
        int played = 0;
        for (int p = 0; p < v->my_n && !played; p++)
            if (v->my_playable[p]) played = pk_api_play(p, pk_api_is_wild(p) ? 0 : 4);
        if (played) {
            v = me_view();
            if (v->turn != v->me || v->over || !v->can_draw) return;
            continue;                                /* the turn came back (D7) */
        }
        if (v->can_draw && !pk_api_draw()) return;
        v = me_view();
        int again = 0;
        for (int p = 0; p < v->my_n && !again; p++) again = v->my_playable[p];
        if (!again) { pk_api_pass(); return; }
    }
}

/* ---- the layout (pk_lay.c): UI.html's thresholds, pinned ---- */
static int near(float a, float b) { return a > b - 0.05f && a < b + 0.05f; }

static void layout(void)
{
    const float W = 358;                     /* a 390pt phone: the 374 board less 2 x 8 */
    float cw, step, content, box, x, y;
    int rows, top;

    OK(pk_lay_collapse(340) == 1 && pk_lay_collapse(440) == 0 && near(pk_lay_collapse(390), 0.5f),
       "collapse runs 440 -> 340");
    OK(pk_lay_max_rows(340) == 1 && pk_lay_max_rows(718) == 2, "the drawer keeps one row (U7)");

    /* expanded: foolish's rows */
    OK(pk_lay_hand(7, W, 2, &cw, &step, &rows, &top, &content, &box) == PK_LAY_FLAT && rows == 1
       && near(cw, 46.571f) && near(box, 80), "seven: one flat row of 46.6 (Rulers 01)");
    OK(pk_lay_hand(9, W, 2, &cw, 0, &rows, 0, 0, 0) == PK_LAY_FLAT && rows == 1, "nine: still one row");
    OK(pk_lay_hand(10, W, 2, &cw, 0, &rows, &top, 0, &box) == PK_LAY_FLAT && rows == 2 && top == 5
       && near(box, 166), "ten: two rows, the 166 box (table 04)");
    OK(pk_lay_hand(13, W, 2, 0, 0, &rows, &top, 0, 0) == PK_LAY_FLAT && rows == 2 && top == 6,
       "thirteen: the smaller half on top");
    OK(pk_lay_hand(26, W, 2, &cw, 0, &rows, 0, 0, 0) == PK_LAY_FLAT && rows == 2 && cw >= 22,
       "twenty-six: two flat rows at the floor (table 05)");
    OK(pk_lay_hand(27, W, 2, &cw, &step, &rows, 0, 0, 0) == PK_LAY_OVERLAP && rows == 2 && cw == 40,
       "twenty-seven: overlapped, 40pt faces (U8)");
    OK(pk_lay_hand(40, W, 2, 0, &step, 0, 0, &content, 0) == PK_LAY_OVERLAP && step >= 16 && content == W,
       "forty: overlap down to a 16pt strip (table 06)");
    OK(pk_lay_hand(42, W, 2, 0, &step, 0, 0, &content, 0) == PK_LAY_SCROLL && step == 16 && content > W,
       "forty-two: the rows scroll (table 07)");

    /* the drawer: one row, flat to overlap to scroll (U7) */
    OK(pk_lay_hand(12, W, 1, &cw, 0, &rows, 0, 0, &box) == PK_LAY_FLAT && rows == 1 && cw < 40 && near(box, 80),
       "twelve in the drawer: one thin flat row");
    OK(pk_lay_hand(14, W, 1, &cw, &step, &rows, 0, 0, 0) == PK_LAY_OVERLAP && rows == 1 && near(step, 23.846f),
       "fourteen in the drawer: overlapped at a 24pt strip (collapsed 02)");
    OK(pk_lay_hand(30, W, 1, 0, &step, 0, 0, &content, 0) == PK_LAY_SCROLL && step == 16,
       "thirty in the drawer: scrolling (collapsed 03)");

    OK(pk_lay_hand_slot(7, W, 2, 0, &x, &y) == 0 && near(x, (W - 7 * 46.571f - 6 * 4) / 2) && near(y, 4),
       "the first card of a flat row is centred");
    OK(pk_lay_hand_slot(10, W, 2, 5, &x, &y) == 0 && near(y, 8 + 78), "card 5 of ten opens the lower row");
    OK(pk_lay_hand_slot(42, W, 2, 21, &x, 0) == 0 && near(x, 4), "a scrolling row starts at the gap");
    OK(pk_lay_hand_slot(7, W, 2, 7, &x, &y) == -1, "a slot off the end");

    /* the ring: me at the bottom, the rest round */
    pk_lay_seat(2, 2, 4, 374, 700, 0, &x, &y);
    OK(near(x, 187) && near(y, 350 + 0.35f * 700), "my own seat is at the bottom");
    pk_lay_seat(3, 2, 4, 374, 700, 0, &x, &y);
    OK(x < 187 * 0.2f && near(y, 350), "the next seat sits on my left");
    pk_lay_seat(0, 2, 4, 374, 322, 1, &x, &y);
    OK(near(x, 187) && near(y, 161 - 0.38f * 322), "across the table, on the collapsed ellipse");

    OK(pk_lay_fan_step(3) == 10 && near(pk_lay_fan_step(12), 68.0f / 11) && pk_lay_fan_step(40) == 3,
       "the fan steps 10, compressing to fit 96 (U6)");
    OK(pk_lay_deck_layers(0) == 0 && pk_lay_deck_layers(6) == 6 && pk_lay_deck_layers(7) == 7
       && pk_lay_deck_layers(12) == 8 && pk_lay_deck_layers(104) == 8, "the deck's layers");

    float cx, cy;
    pk_lay_pile(374, 322, 1, &cx, &cy);
    OK(near(cx, 187) && near(cy, 161 - 24), "the pile lifts 24 in the drawer (U2)");
    pk_lay_pile(374, 700, 0, &cx, &cy);
    OK(near(cy, 350), "and sits on the centre expanded");
    pk_lay_deck(374, 700, 0, &x, &y);
    OK(near(x, 187 - 41 - 10 - 50) && near(y, 350 - 35), "the deck 10pt left of the pile, on its line (U3)");

    int tr, ld;
    pk_lay_pills(1, 1, 0, 0, 0, &tr, &ld);
    OK(tr == PK_PILL_DRAW && ld == PK_PILL_NONE, "my turn: Draw alone, trailing");
    pk_lay_pills(1, 1, 1, 1, 0, &tr, &ld);
    OK(tr == PK_PILL_DRAW && ld == PK_PILL_PLAY, "a card selected: Play beside Draw");
    pk_lay_pills(1, 1, 0, 1, 0, &tr, &ld);
    OK(tr == PK_PILL_DRAW && ld == PK_PILL_PASS, "drew: Pass beside Draw (D10)");
    pk_lay_pills(0, 1, 0, 0, 1, &tr, &ld);
    OK(tr == PK_PILL_UNDO && ld == PK_PILL_NONE, "a play staged: Undo alone takes the trailing slot");
    pk_lay_pills(0, 0, 1, 0, 0, &tr, &ld);
    OK(tr == PK_PILL_NONE && ld == PK_PILL_NONE, "not my turn: a selection offers no Play");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);       /* a crash still shows what went red */
    layout();
    char buf[PK_API_TEXT_MAX], line[256];
    OK(pk_api_layout_hash() == 0, "the smoke build is not stamped (ios-lib stamps the shipped one)");
    OK(pk_api_name_verdict((const uint8_t *)"Alex", 4) == 0 && pk_api_name_verdict((const uint8_t *)"", 0) == 1
       && pk_api_name_verdict((const uint8_t *)"seventeen chars!!", 17) == 2, "nickname verdicts");

    /* ---- a DM: Alex invites, Bo joins and starts in one bubble ---- */
    be(0);
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 29 + 7);
    OK(pk_api_new(seed, 1) == 0, "Alex makes a lobby");
    const PkApiTable *t = table();
    OK(t->readable && t->phase == 0 && t->me == 0 && t->n_seats == 1 && t->offered == PK_LOBBY_WAITING,
       "Alex waits in seat 0");
    OK(pk_api_seats_dirty(), "the new game is recorded");
    int n = pk_api_text(links[0], PK_API_TEXT_MAX);
    OK(n > 3 && !strncmp(links[0], "?m=", 3), "the invitation is a link");
    printf("  invitation: %d characters\n", n);
    OK(pk_api_words(PK_API_W_INVITE, 0, line, sizeof line) > 0, "the invitation's caption");
    printf("  \"%s\"\n", line);
    OK(pk_api_words(PK_API_W_LOBBY_ROW, 0, line, sizeof line) > 0 && !strcmp(line, "1. Alex (You)"),
       "my own roster row says so");
    OK(pk_api_words(PK_API_W_LOBBY_DEALER, 0, line, sizeof line) > 0 && !strcmp(line, "Alex deals"),
       "seat 0 deals");
    OK(pk_api_words(PK_API_W_PUBLIC_ROW, 0, line, sizeof line) > 0 && !strcmp(line, "1. Alex"),
       "the bubble's roster says nobody is you");
    OK(pk_api_words(PK_API_W_STAGED_CAPTION, 0, line, sizeof line) == 0, "a lobby is captioned by its lobby line");

    OK(open_as(1, links[0], 1, 0) == 0, "Bo opens it");
    t = table();
    OK(t->me == 0xFF && t->offered == PK_LOBBY_JOIN && t->can_join_start, "Bo is offered join-and-start");
    OK(pk_api_join_start() == 1, "Bo joins and starts");
    t = table();
    OK(t->phase == 2 && t->me == 1 && t->starter == 1 && t->n_seats == 2, "a live game, Bo in seat 1");
    OK(pk_api_words(PK_API_W_CAPTION, 0, line, sizeof line) > 0, "the deal's caption");
    printf("  \"%s\"\n", line);
    {
        char staged[256];
        OK(pk_api_words(PK_API_W_STAGED_CAPTION, 0, staged, sizeof staged) > 0 && !strcmp(staged, line),
           "right after Start the staged caption is the deal's");
        OK(pk_api_words(PK_API_W_LOBBY_ROW, 0, staged, sizeof staged) > 0 && !strcmp(staged, "1. Alex"),
           "somebody else's roster row");
    }
    const PkView *v = me_view();
    OK(v->me == 1 && v->my_n == 7 && v->turn == 1, "Bo holds seven and moves first (D28)");
    OK(pk_api_draw() == 1, "Bo draws in the start bubble");
    OK(pk_api_undo() == 0, "a draw does not come back (D8)");
    t = table();
    OK(t->draft && !t->can_send, "mid-turn: a draft that cannot be sent");
    const PkApiEvents *ev = (const PkApiEvents *)pk_api_plan_draft(PK_API_ME);
    OK(ev && ev->n > 0 && ev->ev[0].kind == PK_EV_BUBBLE_BEGIN, "the draft's own plan");
    int kinds = 0;
    for (int i = 0; ev && i < ev->n; i++) kinds |= ev->ev[i].kind == PK_EV_DRAW && ev->ev[i].card != PK_CARD_HIDDEN;
    OK(kinds, "Bo sees the card Bo drew");
    OK(pk_api_text(buf, sizeof buf) < 0, "a draft mid-turn cannot be written");
    one_bubble();
    t = table();
    OK(t->can_send, "the turn is done: it can be sent");
    n = pk_api_text(links[1], PK_API_TEXT_MAX);
    OK(n > 0, "the start bubble is a link");
    if (n <= 0) { printf("bridge: %d checks, %d failed\n", checks, fails); return 1; }
    OK(pk_api_check(links[1]) == 0, "and it reads");
    OK(table()->draft, "writing the link left the draft staged");
    {
        char staged[256];
        OK(pk_api_words(PK_API_W_STAGED_CAPTION, 0, staged, sizeof staged) > 0, "the staged bubble's caption");
        OK(pk_api_commit() == 1 && !table()->draft, "sent: the draft is sealed");
        OK(pk_api_words(PK_API_W_CAPTION, table()->bubbles, line, sizeof line) > 0 && !strcmp(staged, line),
           "the staged caption is the sent bubble's");
        printf("  staged: \"%s\"\n", staged);
    }
    OK(pk_api_seats_dirty(), "Bo's seat is recorded");

    /* ---- play it out, phone to phone: the other phone reads each bubble,
     * which at two players is always the turn seat's ---- */
    int from = 1, reads = 0;
    char *cur = links[1];
    static char next[PK_API_TEXT_MAX];
    for (int round = 0; round < 3000; round++) {
        OK(open_as(from ^ 1, cur, 1, 0) == 0, "the other phone reads the bubble");
        reads++;
        const PkApiTable *tt = table();
        OK(tt->me == (from ^ 1) && (tt->by == PK_BY_RECORD || tt->by == PK_BY_SENDER), "seated by record or sender");
        if (tt->phase == 3) break;
        v = me_view();
        OK(v->my_n > 0 && v->me == tt->me, "my own hand");
        if (v->turn != v->me) {
            /* a play to one card ended the sender's bubble with the turn
             * still theirs (D7): their own phone goes on */
            OK(open_as(from, cur, 1, 1) == 0 && table()->me == from, "the sender's phone reads its own bubble");
            v = me_view();
        }
        OK(v->turn == v->me, "the phone in hand is the turn seat's");
        tt = table();
        OK(pk_api_since(tt->bubbles - 1, tt->bubbles) != 0, "since the last bubble");
        OK(pk_api_words(PK_API_W_CAPTION, tt->bubbles, line, sizeof line) > 0
           && pk_api_words(PK_API_W_HEADLINE, 0, line, sizeof line) >= 0
           && pk_api_words(PK_API_W_DECK_LEFT, 0, line, sizeof line) > 0, "the words");
        {
            uint8_t rank[8];
            OK(pk_api_ranks(rank) == 0, "a live game ranks nobody (D22)");
        }
        one_bubble();
        OK(table()->can_send, "a bubble that can be sent");
        n = pk_api_text(next, sizeof next);
        OK(n > 0, "it writes");
        if (n <= 0) break;
        OK(pk_api_prefer(next, cur) < 0 && pk_api_prefer(cur, next) > 0, "the child beats its parent");
        OK(pk_api_common(next, cur) == table()->bubbles, "they share every bubble of the parent");
        pk_api_commit();
        memcpy(links[2], next, (size_t)n + 1);
        cur = links[2];
        from = table()->me;
    }
    t = table();
    OK(t->phase == 3, "the game ended");
    v = (const PkView *)pk_api_view(PK_API_ME);
    OK(v->over && v->reveal[0].n + v->reveal[1].n > 0, "the end reveals every hand");
    {
        uint8_t rank[8];
        int loser = v->winner ^ 1;
        OK(pk_api_ranks(rank) == 2 && rank[0] == v->winner && rank[1] == loser, "the winner ranks first");
        OK(v->reveal[v->winner].n <= v->reveal[loser].n, "the winner holds the fewest");
        OK(pk_api_words(PK_API_W_RANK_ROW, 0, line, sizeof line) > 0 && line[0] == '1'
           && pk_api_words(PK_API_W_RANK_ROW, 2, line, sizeof line) == -1, "the results rows");
    }
    OK(pk_api_words(PK_API_W_CAPTION, t->bubbles, line, sizeof line) > 0, "the last caption");
    printf("  a 2p game in %d bubbles over %d reads; last: \"%s\" (%d characters)\n", t->bubbles, reads, line,
           (int)strlen(cur));
    ev = (const PkApiEvents *)pk_api_plan(PK_API_ALL, t->bubbles - 1, t->bubbles);
    OK(ev && ev->n > 0 && ev->ev[ev->n - 1].kind == PK_EV_BUBBLE_END, "the last bubble's plan");

    /* ---- a rotated id: no record, no tag, only the sender fact or the name ---- */
    uint8_t other[16] = { 9, 9, 9 };
    pk_api_seats_load(0, 0);
    pk_api_me(other, 16);
    pk_api_sender(cur, 1, 1);
    OK(pk_api_read(cur) == 0 && table()->by == PK_BY_SENDER, "a rotated id in a DM: seated by the sender fact");
    pk_api_seats_load(0, 0);
    pk_api_sender(NULL, 0, -1);
    pk_api_nickname((const uint8_t *)"Alex", 4);
    OK(pk_api_read(cur) == 0 && table()->me == 0 && table()->by == PK_BY_NAME, "...or by the nickname");
    pk_api_seats_load(0, 0);
    pk_api_nickname((const uint8_t *)"Zed", 3);
    OK(pk_api_read(cur) == 0 && table()->me == 0xFF, "nobody I know: a spectator");
    OK(pk_api_draw() == 0, "a spectator stages nothing");

    /* ---- a group lobby: three join, one leaves, the lobby plan ---- */
    be(0);
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 13 + 1);
    pk_api_new(seed, 0);
    pk_api_text(links[3], PK_API_TEXT_MAX);
    open_as(1, links[3], 0, 0);
    OK(pk_api_join() == 1, "Bo joins");
    pk_api_text(links[4], PK_API_TEXT_MAX);
    open_as(2, links[4], 0, 0);
    OK(table()->offered == PK_LOBBY_JOIN, "Cleo may join");
    OK(pk_api_join() == 2, "Cleo joins");
    OK(pk_api_words(PK_API_W_LOBBY_DEALER, 0, line, sizeof line) > 0 && !strcmp(line, "Alex deals"),
       "seat 0 deals, whoever joined last");
    pk_api_text(links[5], PK_API_TEXT_MAX);
    ev = (const PkApiEvents *)pk_api_plan_lobby(links[4]);
    OK(ev && ev->n == 1 && ev->ev[0].kind == PK_EV_LOBBY_JOIN && ev->ev[0].seat == 2, "the lobby plan: Cleo arrives");
    OK(table()->offered == PK_LOBBY_WAITING, "the newest joiner waits while there is room");
    open_as(1, links[5], 0, 0);
    OK(table()->me == 1 && table()->offered == PK_LOBBY_START && table()->can_exit, "Bo may start, or leave");
    OK(pk_api_words(PK_API_W_LEFT, 1, line, sizeof line) > 0, "Bo's leave is captioned before it");
    OK(pk_api_leave() == 0 && table()->me == 0xFF && table()->n_seats == 2, "Bo leaves");
    pk_api_text(links[6], PK_API_TEXT_MAX);
    open_as(2, links[6], 0, 0);
    OK(table()->me == 1 && table()->by == PK_BY_RECORD, "Cleo's record finds her in the row she moved down to");
    OK(pk_api_start() == 0 && table()->phase == 2 && table()->starter == 1, "Cleo starts");
    ev = (const PkApiEvents *)pk_api_plan(PK_API_ME, -1, 0);
    OK(ev && ev->n > 7 && ev->ev[0].kind == PK_EV_LOBBY_START && ev->ev[0].seat == 1, "the deal's plan names its starter");

    /* ---- the name Bo freed is taken by somebody else: Bo's phone, which
     * sat here and left, is not handed the namesake's seat (D51) ---- */
    open_as(3, links[6], 0, 0);
    pk_api_nickname((const uint8_t *)"Bo", 2);
    OK(pk_api_read(links[6]) == 0 && table()->me == 0xFF && pk_api_join() == 2, "another Bo joins at seat 2");
    pk_api_text(links[7], PK_API_TEXT_MAX);
    OK(open_as(1, links[7], 0, 0) == 0 && table()->me == 0xFF && table()->offered == PK_LOBBY_JOIN,
       "the first Bo, who left, is not seated by the name");

    /* ---- the buried start cards (D14, U16): there until drawn down to ---- */
    {
        int buried_at_deal = 0, found = 0;
        for (int k = 0; k < 400 && !found; k++) {
            be(0);
            for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 11 + k * 5 + 1);
            pk_api_new(seed, 1);
            pk_api_text(links[3], PK_API_TEXT_MAX);
            open_as(1, links[3], 1, 0);
            pk_api_join_start();
            ev = (const PkApiEvents *)pk_api_plan(PK_API_ALL, -1, 0);
            buried_at_deal = 0;
            for (int i = 0; ev && i < ev->n; i++) buried_at_deal += ev->ev[i].kind == PK_EV_BURY;
            found = buried_at_deal > 1;         /* two, so one can go before the other */
        }
        OK(found, "a deal that buries two start cards");
        uint8_t under[8];
        OK(pk_api_buried(under) == buried_at_deal, "every buried card is under the deck after the deal");
        OK(!pk_api_card_rank(under[0]) || pk_api_card_rank(under[0]) > 9, "a buried card is not a number");
        /* draw and pass, phone to phone, until the deck is empty: no play, so
         * the pile is one card and nothing can reshuffle */
        n = pk_api_text(links[6], PK_API_TEXT_MAX);
        int mid_checked = 0;
        for (int round = 0; round < 400 && n > 0; round++) {
            v = me_view();
            if (!mid_checked && v->deck_n > 20) {
                OK(pk_api_buried(under) == buried_at_deal, "still buried with the deck half drawn");
                mid_checked = 1;
            }
            int want = buried_at_deal < v->deck_n ? buried_at_deal : v->deck_n;
            OK(pk_api_buried(under) == want, "the buried cards are the deck's bottom ones");
            if (v->deck_n == 0 || v->over) break;
            int turn = v->turn;
            open_as(turn, links[6], 1, 0);
            if (!pk_api_draw() || !pk_api_pass()) break;
            n = pk_api_text(links[6], PK_API_TEXT_MAX);
            pk_api_commit();
        }
        OK(me_view()->deck_n == 0, "the deck was drawn down");
        OK(pk_api_buried(under) == 0, "drawn down to, the buried cards are in hands now");
    }

    /* ---- three players to the end: the results order ---- */
    {
        be(0);
        for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 3);
        pk_api_new(seed, 0);
        pk_api_text(links[3], PK_API_TEXT_MAX);
        open_as(1, links[3], 0, 0);
        pk_api_join();
        pk_api_text(links[4], PK_API_TEXT_MAX);
        open_as(2, links[4], 0, 0);
        pk_api_join();
        pk_api_text(links[5], PK_API_TEXT_MAX);
        open_as(0, links[5], 0, 0);
        OK(pk_api_start() == 0 && table()->n_seats == 3, "a three-seat game");
        n = pk_api_text(links[6], PK_API_TEXT_MAX);
        char *at = links[6];
        for (int round = 0; round < 3000 && n > 0; round++) {
            open_as(0, at, 0, 0);
            if (table()->phase == 3) break;
            int turn = me_view()->turn;
            open_as(turn, at, 0, 0);
            one_bubble();
            n = pk_api_text(links[7], PK_API_TEXT_MAX);
            pk_api_commit();
            memcpy(links[6], links[7], (size_t)(n > 0 ? n : 0) + 1);
        }
        OK(table()->phase == 3, "the three-seat game ended");
        uint8_t rank[8];
        const PkView *all = (const PkView *)pk_api_view(PK_API_ALL);
        OK(pk_api_ranks(rank) == 3 && rank[0] == all->winner, "three ranked, the winner first");
        OK(all->reveal[rank[1]].n <= all->reveal[rank[2]].n
           && (all->reveal[rank[1]].n < all->reveal[rank[2]].n || rank[1] < rank[2]),
           "then fewest cards first, ties in seat order");
    }

    /* ---- refusals ---- */
    OK(pk_api_words(PK_API_W_ERROR, PK_EFORMAT, line, sizeof line) > 0
       && !strcmp(line, "That game came from a newer version of the app"), "a newer format says so");
    OK(pk_api_words(PK_API_W_ERROR, PK_ECHECK, line, sizeof line) > 0
       && !strcmp(line, "This game link is damaged"), "any other refusal is a damaged link");
    OK(pk_api_words(PK_API_W_ERROR, 0, line, sizeof line) == -1, "PK_EOK is not an error");
    OK(pk_api_card_suit(0) == 0 && pk_api_card_rank(0) == 1 && pk_api_card_rank(17) == 9
       && pk_api_card_suit(47) == 1 && pk_api_card_rank(47) == PK_R_PLUS2 && pk_api_card_rank(42) == PK_R_SKIP
       && pk_api_card_rank(44) == PK_R_REVERSE, "a suited card's suit and rank (3.2)");
    OK(pk_api_card_suit(96) == PK_NO_SUIT && pk_api_card_rank(99) == PK_R_WILD && pk_api_card_rank(100) == PK_R_WILD4,
       "the wilds");
    OK(pk_api_card_suit(PK_CARD_HIDDEN) == -1 && pk_api_card_rank(104) == -1 && pk_api_card_rank(-1) == -1,
       "an id off the deck is nothing");
    OK(pk_api_read("hello") < 0 && pk_api_check("?m=AAAA") < 0, "a link that is not a game");
    OK(pk_api_prefer("junk", links[6]) > 0 && pk_api_prefer(links[6], "junk") < 0, "the unreadable one loses");
    OK(pk_api_same_game(links[3], links[6]) && !pk_api_same_game(links[0], links[6]), "same game by seed");
    OK(pk_api_words(PK_API_W_COUNT, 0, line, sizeof line) == -1 && pk_api_string(-1, line, sizeof line) == -1,
       "off the end of the words");
    OK(pk_api_string(0, line, sizeof line) > 0, "the game's name by key");
    OK(pk_api_seats_save((uint8_t *)buf, 3) == -1, "a short records buffer is refused");

    printf("bridge: %d checks, %d failed\n", checks, fails);
    if (!fails) printf("bridge ok\n");
    return fails ? 1 : 0;
}

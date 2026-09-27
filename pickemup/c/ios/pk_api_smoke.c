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
    pk_lay_pile(374, 390, 0.5f, &cx, &cy);
    OK(near(cx, 187) && near(cy, 195 - 24), "the pile lifts 24 in the drawer (U2)");
    pk_lay_pile(374, 700, 0, &cx, &cy);
    OK(near(cy, 350), "and sits on the centre expanded");
    pk_lay_deck(374, 700, 0, &x, &y);
    OK(near(x, 187 - 41 - 10 - 50) && near(y, 350 - 35), "the deck 10pt left of the pile, on its line (U3)");
    OK(pk_lay_table_scale(700, 0) == 1 && pk_lay_subline(0) == 1, "expanded: the table at full size, the sub-line shown");

    /* O10, the compact drawer: the pile and the deck in the band between the
     * top fan's foot and the pill row, 4 clear of each, scaled to fit it. The
     * band's two edges are read back from the ring and the pill zone, so a
     * change to either moves the pile with it. */
    {
        static const struct { float view_h, scale, cy; const char *what; } drawer[] = {
            { 340, 1.0f, 127.0f, "a 340pt drawer (322 of board): full size, the lift held 67 over the pills" },
            { 299, 0.80977f, 98.745f, "the iPhone 17e's 299pt drawer (281 of board): 0.81, filling the band" },
        };
        for (int k = 0; k < 2; k++) {
            float bh = drawer[k].view_h - PK_LAY_INSET_T - PK_LAY_INSET_B, c = pk_lay_collapse(drawer[k].view_h);
            float s = pk_lay_table_scale(bh, c), fx, fy, px, py, pw, ph;
            pk_lay_pile(374, bh, c, &cx, &cy);
            OK(c == 1 && near(s, drawer[k].scale) && near(cx, 187) && near(cy, drawer[k].cy), drawer[k].what);
            pk_lay_seat(1, 0, 2, 374, bh, c, &fx, &fy);
            pk_lay_zone(PK_ZONE_PILLS, 374, bh, c, PK_LAY_ROW_H, &px, &py, &pw, &ph);
            OK(cy - 64 * s >= fy + 9.2f + 4 - 0.01f, "the pile's reach clears the top fan's foot by 4");
            OK(cy + 67 * s <= py - 4 + 0.01f, "and the pill row by 4: no pill over the pile");
            pk_lay_deck(374, bh, c, &x, &y);
            OK(near(x, cx - (41 + 10 + 50) * s) && near(y, cy - 35 * s), "the deck beside it, scaled with it, on its line");
            OK(pk_lay_zone(PK_ZONE_PILE_DROP, 374, bh, c, PK_LAY_ROW_H, &px, &py, &pw, &ph) == 0
               && near(px, cx - 41 * s - 8) && near(py, cy - 57.5f * s - 8) && near(pw, 82 * s + 16) && near(ph, 115 * s + 16),
               "the drop zone is the drawn pile, 8 all round");
            OK(pk_lay_subline(c) == 0, "the drawer drops the status corner's sub-line");
        }
        OK(pk_lay_table_scale(100, 1) == 0.5f, "never under half size");
        pk_lay_picker(0, 187, 98.745f, &x, &y);
        OK(near(x, 187) && near(y, 30), "the north tile stays whole on the board");
        pk_lay_picker(2, 187, 98.745f, &x, &y);
        OK(near(y, 98.745f + 104), "the south tile keeps its reach");
    }

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

    /* the zones (I31): a 374 x 700 board with a one-row hand (80) */
    float zw, zh;
    OK(pk_lay_zone(PK_ZONE_DRAW_BAND, 374, 700, 0, 80, &x, &y, &zw, &zh) == 0
       && near(x, 8) && near(y, 620 - 64) && near(zw, 358) && near(zh, 80 + 88), "U24: the hand band, 64 up and 24 down");
    OK(pk_lay_zone(PK_ZONE_PILE_DROP, 374, 322, 1, 80, &x, &y, &zw, &zh) == 0
       && near(x, 187 - 41 - 8) && near(y, 127 - 57.5f - 8) && near(zw, 98) && near(zh, 131),
       "the pile's drop target follows its lift, 8 all round");
    OK(pk_lay_zone(PK_ZONE_PILLS, 374, 700, 0, 166, &x, &y, &zw, &zh) == 0 && near(x, 0) && near(y, 534 - 44)
       && near(zw, 374) && near(zh, 40), "the pill row 4 above a two-row hand");
    OK(pk_lay_zone(PK_ZONE_TOAST, 374, 700, 0, 80, &x, &y, &zw, &zh) == 0 && near(x, 187) && near(y, 556)
       && zw == 0 && zh == 0, "the toast's centre");
    OK(pk_lay_zone(PK_ZONE_DIR, 374, 700, 0, 80, &x, &y, &zw, &zh) == 0 && near(x, 296) && near(y, -3)
       && near(zw, 78) && near(zh, 68), "the direction box, top right");
    OK(pk_lay_zone(PK_ZONE_N, 374, 700, 0, 80, &x, &y, &zw, &zh) == -1, "a zone off the list");
    OK(pk_lay_zone(PK_ZONE_DRAW_BAND, 10, 700, 0, 80, 0, 0, &zw, 0) == 0 && zw == 0, "a band never goes negative");

    /* A14: the auto-collapse's push, uttt's curve on this kernel's numbers:
     * the whole travel at the flip, the host's critically damped spring
     * (at half its 338ms response, (1 + pi) e^-pi of the travel is left), and
     * exactly nothing from 600ms on, reached without a step */
    {
        int mono = 1;
        float prev = pk_lay_collapse_push(500, 0);
        for (int t = 1; t <= PK_LAY_COLLAPSE_MS; t++) {
            float p = pk_lay_collapse_push(500, t);
            if (p > prev + 1e-4f || p < -1e-4f) mono = 0;
            prev = p;
        }
        OK(pk_lay_collapse_push(500, 0) == 500 && pk_lay_collapse_push(500, -5) == 500, "the whole travel at the flip");
        OK(mono, "the push only ever falls, and never below zero");
        OK(near(pk_lay_collapse_push(500, 169), 500 * 0.178976f), "the host's spring at half its response");
        OK(pk_lay_collapse_push(500, PK_LAY_COLLAPSE_MS) == 0 && pk_lay_collapse_push(500, 5000) == 0
           && pk_lay_collapse_push(500, PK_LAY_COLLAPSE_MS - 1) < .05f, "nothing left at 600ms, and no step to it");
        OK(PK_LAY_COLLAPSE_MS == 600 && PK_LAY_COLLAPSE_STEPS == 120 && PK_LAY_COLLAPSE_FLIP == 60.0f
           && PK_LAY_DRAWER_RESPONSE_MS == 338, "uttt's numbers, so the two games collapse alike");
    }
}

/* ---- adopting with its motion (I29) and the fan's tap (I30) ---- */
static void adopt_and_fan(void)
{
    static char l[4][PK_API_TEXT_MAX];
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 17 + 9);
    be(0);
    pk_api_new(seed, 1);
    pk_api_text(l[0], PK_API_TEXT_MAX);
    open_as(1, l[0], 1, 0);
    pk_api_join_start();
    static char start[PK_API_TEXT_MAX];
    OK(pk_api_text(start, PK_API_TEXT_MAX) > 0, "the start bubble");
    one_bubble();
    OK(pk_api_text(l[1], PK_API_TEXT_MAX) > 0 && pk_api_commit() == 1, "Bo's first bubble");

    be(0);
    {
        uint32_t serial = pk_api_beats_serial();
        const void *playing = pk_api_beats_now();
        OK(pk_api_adopt("junk", 0) < 0 && pk_api_beats_serial() == serial && pk_api_beats_now() == playing,
           "an unreadable link: refused, the playing plan kept");
    }
    OK(pk_api_adopt(l[0], 0) == 0 && pk_api_beats_now() == 0, "a lobby: no motion");
    const PkBeats *b = (const PkBeats *)pk_api_beats_now();
    OK(pk_api_adopt(l[1], 1) == 0 && (b = (const PkBeats *)pk_api_beats_now()) != 0 && b->n > 0
       && b->mode == PK_BEATS_ARRIVAL, "from the lobby it was dealt from: the newest bubble, arriving");
    OK(b && ((const PkBeatFrame *)pk_api_beats_frame(0))->deck_n < PK_DECK,
       "the newest bubble only: it starts from the board after the deal");
    OK(pk_api_adopt(l[0], 0) == 0 && pk_api_adopt(l[1], 0) == 0 && (b = (const PkBeats *)pk_api_beats_now()) != 0
       && b->mode == PK_BEATS_OPEN, "opened, not arriving: the opened lead");
    OK(pk_api_adopt(l[1], 1) == 0 && pk_api_beats_now() == 0, "the same bubble again moves nothing");
    OK(pk_api_adopt(l[0], 0) == 0 && pk_api_adopt(start, 0) == 0 && pk_api_beats_now() != 0
       && ((const PkBeatFrame *)pk_api_beats_frame(0))->deck_n == PK_DECK, "the start bubble plays its deal");
    OK(pk_api_adopt(l[1], 0) == 0 && table()->bubbles == 1, "back on Bo's bubble");
    OK(pk_api_adopt("junk", 1) < 0 && table()->bubbles == 1, "a refused adopt leaves the resident");

    /* Alex moves; Bo's phone, showing bubble 1, adopts bubble 2 */
    one_bubble();
    OK(pk_api_text(l[2], PK_API_TEXT_MAX) > 0 && pk_api_commit() == 1, "Alex's bubble");
    open_as(1, l[1], 1, 1);
    OK(pk_api_adopt(l[2], 1) == 0 && (b = (const PkBeats *)pk_api_beats_now()) != 0
       && b->mode == PK_BEATS_ARRIVAL && b->n > 0, "further on in the same game: an arrival");
    {
        /* the plan is exactly bubbles (1, 2]: no deal in it */
        int dealt = 0;
        for (int i = 0; b && i < b->n; i++) dealt |= b->beat[i].ev_kind == PK_EV_DEAL;
        OK(!dealt, "from the bubble on screen, not from the deal");
    }
    OK(pk_api_adopt(l[2], 0) == 0 && pk_api_beats_now() == 0, "opened again: nothing new");

    /* a staged play, then a chain without it: the retraction leads */
    int staged = 0;
    const PkView *v = me_view();
    for (int p = 0; p < v->my_n && !staged; p++)
        if (v->my_playable[p] && !pk_api_is_wild(p)) staged = pk_api_play(p, 4);
    OK(staged, "Bo has a plain card to stage");
    if (staged) {
        OK(table()->draft && table()->can_send, "Bo's play is staged");
        OK(pk_api_adopt(l[1], 0) == 0 && (b = (const PkBeats *)pk_api_beats_now()) != 0 && b->n > 0
           && (b->beat[0].flags & PK_BF_RETRACT), "the staged card flies home first");
    }

    /* three seats: the fan's tap */
    be(0);
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 5 + 77);
    pk_api_new(seed, 0);
    pk_api_text(l[0], PK_API_TEXT_MAX);
    open_as(1, l[0], 0, 0);
    pk_api_join();
    pk_api_text(l[1], PK_API_TEXT_MAX);
    open_as(2, l[1], 0, 0);
    pk_api_join();
    pk_api_text(l[2], PK_API_TEXT_MAX);
    open_as(0, l[2], 0, 0);
    OK(pk_api_start() == 0 && table()->n_seats == 3, "a three-seat game for the fans");
    int me = table()->me, a = (me + 1) % 3, c = (me + 2) % 3;
    OK(pk_api_tap_fan(me) == PK_API_FAN_REFUSED, "my own seat is no catch");
    OK(pk_api_tap_fan(a) == PK_API_FAN_CALLED && me_view()->draft_call == a, "a tap calls that seat");
    OK(pk_api_tap_fan(c) == PK_API_FAN_MOVED && me_view()->draft_call == c, "another seat's tap moves the call");
    OK(pk_api_tap_fan(me) == PK_API_FAN_REFUSED && me_view()->draft_call == c,
       "a refused move keeps the call it would have replaced");
    OK(pk_api_tap_fan(c) == PK_API_FAN_UNCALLED && me_view()->draft_call == PK_SEAT_NONE, "a second tap takes it back");
    OK(pk_api_tap_fan(9) == PK_API_FAN_REFUSED, "a seat off the table");
}

/* ---- my own arrangement of my hand (O9, I38): the entry points and the drag's layout ---- */
static void arrange(void)
{
    /* the slot a dragged card asks for */
    float w = 343, x, y, cw;
    int n = 7;
    pk_lay_hand(n, w, 2, &cw, 0, 0, 0, 0, 0);
    int ok = 1;
    for (int i = 0; i < n; i++) {
        pk_lay_hand_slot(n, w, 2, i, &x, &y);
        ok &= pk_lay_hand_nearest(n, w, 2, x + cw / 2, y + PK_LAY_CARD_H / 2) == i;
        ok &= pk_lay_hand_nearest(n, w, 2, x + cw / 2 + cw * 0.4f, y + 10) == i;
        ok &= pk_lay_hand_nearest(n, w, 2, x + cw / 2 - cw * 0.4f, y + 10) == i;
    }
    OK(ok, "a card over a slot asks for that slot");
    float x0, x1;
    pk_lay_hand_slot(n, w, 2, 2, &x0, &y);
    pk_lay_hand_slot(n, w, 2, 3, &x1, &y);
    OK(pk_lay_hand_nearest(n, w, 2, (x0 + x1) / 2 + cw / 2, y + PK_LAY_CARD_H / 2) == 2, "a tie goes to the lower slot");
    OK(pk_lay_hand_nearest(0, w, 2, 10, 10) == -1, "no slot in an empty hand");
    OK(pk_lay_hand_nearest(n, w, 2, -500, 40) == 0 && pk_lay_hand_nearest(n, w, 2, 900, 40) == n - 1,
       "past either end: the end slot");
    pk_lay_hand(20, w, 2, &cw, 0, 0, 0, 0, 0);
    float xb, yb;
    pk_lay_hand_slot(20, w, 2, 15, &xb, &yb);
    OK(pk_lay_hand_nearest(20, w, 2, xb + cw / 2, yb + PK_LAY_CARD_H / 2) == 15, "the other row's slots are reachable");

    /* where a release lands: the row first */
    float bw = 360, bh = 420, box, px, py, pw, ph;
    pk_lay_hand(7, bw - 2 * PK_LAY_HAND_PAD, 2, 0, 0, 0, 0, 0, &box);
    pk_lay_zone(PK_ZONE_PILE_DROP, bw, bh, 0, box, &px, &py, &pw, &ph);
    OK(pk_lay_drop(bw, bh, 0, box, bw / 2, bh - box / 2) == PK_DROP_HAND, "a release in the row rearranges");
    OK(pk_lay_drop(bw, bh, 0, box, px + pw / 2, py + ph / 2) == PK_DROP_PILE, "a release on the pile plays");
    OK(pk_lay_drop(bw, bh, 0, box, 4, 40) == PK_DROP_NONE, "a release on the felt does nothing");
    OK(pk_lay_drop(bw, bh, 0, box, bw / 2, bh - box - 1) != PK_DROP_HAND, "just above the row is not the row");
    OK(pk_lay_drop(bw, bh, 0, box, PK_LAY_HAND_PAD - 1, bh - 2) != PK_DROP_HAND, "the side padding is not the row");
    float tall = bh - (py + ph) + 20;       /* a row that reaches up into the pile's zone */
    OK(pk_lay_drop(bw, bh, 0, tall, px + pw / 2, py + ph - 5) == PK_DROP_HAND,
       "a release on the cards never plays, even inside the pile's zone");

    /* the bridge: a drag, the view, a play by slot, the record */
    static char l[3][PK_API_TEXT_MAX];
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 29 + 3);
    be(0);
    pk_api_new(seed, 1);
    OK(pk_api_arrange_move(0, 1) == 0 && pk_api_arranged_pos(0) == -1, "a lobby has no hand to arrange");
    pk_api_text(l[0], PK_API_TEXT_MAX);
    open_as(1, l[0], 1, 0);
    pk_api_join_start();
    pk_api_text(l[1], PK_API_TEXT_MAX);
    pk_api_commit();
    open_as(0, l[1], 1, 0);
    const PkView *v = me_view();
    uint8_t first = v->my_hand[0];
    OK(v->my_slot[0] == 0 && v->my_slot[6] == 6, "the deal reads in acquisition order");
    OK(pk_api_arrange_move(0, 6) == 1, "a drag of slot 0 to slot 6");
    v = me_view();
    OK(v->my_hand[0] == first && v->my_slot[0] == 6 && v->my_slot[1] == 0, "the view: the same hand, drawn in the new order");
    OK(pk_api_arranged_pos(6) == 0 && pk_api_arranged_pos(0) == 1 && pk_api_arranged_pos(7) == -1,
       "the position drawn at a slot");
    OK(pk_api_arrange_move(2, 2) == 0 && pk_api_arrange_move(0, 7) == 0, "no move to itself or off the hand");
    OK(pk_api_arrange_move(3, 1) == 1 && pk_api_arranged_pos(1) == 4 && pk_api_arranged_pos(2) == 2
       && pk_api_arranged_pos(3) == 3, "a drag to a middle slot lands in that slot");
    OK(pk_api_arrange_move(1, 3) == 1 && pk_api_arranged_pos(3) == 4 && pk_api_arranged_pos(4) == 5,
       "and back again");
    OK(pk_api_play_slot(-1, 4) == 0, "no play off the hand");
    OK(pk_api_seats_dirty(), "a drag is saved with the seat records");
    int rn = pk_api_seats_save((uint8_t *)l[2], PK_API_REC_BYTES);
    OK(rn > PK_API_ARR_BYTES && (rn - PK_API_ARR_BYTES) % 17 == 0, "the records, then the arrangements' block");
    pk_api_seats_load(0, 0);
    OK(pk_api_read(l[1]) == 0 && me_view()->my_slot[0] == 0, "a phone without the record: acquisition order");
    pk_api_seats_load((const uint8_t *)l[2], rn);
    OK(pk_api_read(l[1]) == 0 && me_view()->my_slot[0] == 6, "the record back: the drag is back");
    const PkBeatFrame *f = (const PkBeatFrame *)pk_api_beats_frame(0);
    OK(pk_api_beats_host(PK_HM_REFUSED, 0, 6) && (f = (const PkBeatFrame *)pk_api_beats_frame(0)) != 0
       && f->my_slot[0] == 6 && f->my_slot[1] == 0, "a plan's frame is drawn by the arrangement too");
    who = -1;
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
    {
        /* THE MOTION: the deal for Bo on the start bubble, and nothing at all
         * for a range with no bubble in it */
        const PkBeats *b = (const PkBeats *)pk_api_beats(PK_API_ME, -1, 0, PK_BEATS_OPEN);
        OK(b && b->n >= 14 + 7 + 2 && b->total_ms > 3000 && b->viewer == 1, "the deal is a plan for Bo");
        uint32_t serial = b ? b->serial : 0;
        const PkBeatFrame *f = (const PkBeatFrame *)pk_api_beats_frame(0);
        OK(f->deck_n == PK_DECK && f->my_n == 0, "it starts from a full deck");
        f = (const PkBeatFrame *)pk_api_beats_frame(b ? b->total_ms : 0);
        OK(f->done && f->my_n == 7 && !memcmp(f->my_hand, v->my_hand, 7), "and ends on Bo's seven");
        OK(pk_api_beat_sample(0, 0, 0) != 0 && pk_api_beat_sample(b ? b->n : 0, 0, 0) == 0, "a sample per beat");
        b = (const PkBeats *)pk_api_beats(PK_API_ME, 0, 0, PK_BEATS_ARRIVAL);
        OK(b && b->n == 0 && b->total_ms == 0 && b->serial == serial + 1, "from == to: no motion, a new plan");
        OK(pk_api_beats(PK_API_ME, 0, 0, 7) == 0, "a mode that is not a range's is refused");
    }
    OK(pk_api_draw() == 1, "Bo draws in the start bubble");
    {
        const PkBeats *b = (const PkBeats *)pk_api_beats_stage(0);
        OK(b && b->mode == PK_BEATS_STAGE && b->n == 3 && b->beat[0].kind == PK_BK_FLIGHT &&
           b->beat[0].to == PK_ANC_HAND && b->beat[0].to_i == 7, "channel A: Bo's draw flies to the eighth slot");
        b = (const PkBeats *)pk_api_beats_stage(0);
        OK(b && b->n == 0, "asked again with nothing new: nothing moves");
    }
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
        const PkBeats *b = (const PkBeats *)pk_api_beats_send();
        OK(b && b->mode == PK_BEATS_SEND && b->held == 0, "channel B: what staging held");
        int turned = 0;
        for (int i = 0; b && i < b->n; i++) turned |= b->beat[i].kind == PK_BK_TURN_BAR;
        OK(turned, "the turn bar moves at Send");
        OK(pk_api_words(PK_API_W_CAPTION, table()->bubbles, line, sizeof line) > 0 && !strcmp(staged, line),
           "the staged caption is the sent bubble's");
        printf("  staged: \"%s\"\n", staged);
    }
    OK(pk_api_seats_dirty(), "Bo's seat is recorded");

    /* ---- play it out, phone to phone: the other phone reads each bubble,
     * which at two players is always the turn seat's ---- */
    int from = 1, reads = 0, won_with_call = 0;
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
        if (round == 0) {
            char a[64], b[64];
            pk_api_words(PK_API_W_DECK_LEFT, 0, a, sizeof a);
            pk_api_words(PK_API_W_DECK_N, me_view()->deck_n, b, sizeof b);
            OK(!strcmp(a, b), "a plan's count reads as the settled one");
            OK(pk_api_words(PK_API_W_DECK_N, 27, b, sizeof b) > 0 && !strcmp(b, "27 left"), "any count");
            OK(pk_api_words(PK_API_W_DECK_N, 105, b, sizeof b) < 0, "no count past the deck");
            OK(pk_api_words(PK_API_W_DIR_OF, PK_DIR_ACW, b, sizeof b) == 0, "two players: no direction word (D13)");
        }
        {
            uint8_t rank[8];
            OK(pk_api_ranks(rank) == 0, "a live game ranks nobody (D22)");
        }
        /* one wrong call, early (I37): the caller's stamp, in the kernel's order */
        const int caller = table()->me, callee = caller ^ 1;
        if (round == 3) {
            OK((me_view()->can_call & (1 << callee)) && pk_api_catch(callee) == 1, "a call on a seat that is not exposed");
            OK(pk_api_collapses(PK_API_TOUCH_CALL) == 0, "a call does not collapse the drawer");
        }
        /* the winning bubble carries a wrong call too: OUT outranks it (I37) */
        const int last_call = me_view()->my_n == 1 && me_view()->my_playable[0]
                              && (me_view()->can_call & (1 << callee)) && pk_api_catch(callee) == 1;
        one_bubble();
        OK(table()->can_send, "a bubble that can be sent");
        if (round == 3) {
            OK(pk_api_collapses(PK_API_TOUCH_SAY) == 0, "a bubble with a call is not a lone Last card!");
            OK(pk_api_collapses(PK_API_TOUCH_PLAY) == 1 && pk_api_collapses(PK_API_TOUCH_PASS) == 1
               && pk_api_collapses(PK_API_TOUCH_DRAW) == 0 && pk_api_collapses(PK_API_TOUCH_UNDO) == 0
               && pk_api_collapses(PK_API_TOUCH_UNSAY) == 0 && pk_api_collapses(PK_API_TOUCH_UNCALL) == 0
               && pk_api_collapses(0) == 0, "a play or a pass collapses; a draw, an undo, an un-say, an un-call do not");
        }
        n = pk_api_text(next, sizeof next);
        OK(n > 0, "it writes");
        if (n <= 0) break;
        OK(pk_api_prefer(next, cur) < 0 && pk_api_prefer(cur, next) > 0, "the child beats its parent");
        OK(pk_api_common(next, cur) == table()->bubbles, "they share every bubble of the parent");
        pk_api_commit();
        if (round == 3) {
            OK(pk_api_stamp(caller) == PK_STAMP_WRONG && pk_api_stamp(callee) != PK_STAMP_CAUGHT,
               "Wrong call under the caller, nothing caught on the other");
            OK(pk_api_collapses(PK_API_TOUCH_PLAY) == 0, "no draft open: nothing to collapse for");
        }
        if (last_call && table()->phase == PK_PHASE_FINISHED) {
            const PkSince *ls = (const PkSince *)pk_api_since(table()->bubbles - 1, table()->bubbles);
            OK(ls && ls->wrong == caller, "the winning bubble's call was judged");
            OK(pk_api_stamp(caller) == PK_STAMP_OUT && pk_api_stamp(callee) == 0,
               "once it is over OUT outranks the newest bubble's verdict");
            won_with_call = 1;
        }
        memcpy(links[2], next, (size_t)n + 1);
        cur = links[2];
        from = table()->me;
    }
    t = table();
    OK(t->phase == 3, "the game ended");
    OK(won_with_call, "the game was won by a bubble that also called");
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
    {
        /* grid "Join" (A13): the new row fades up, 220ms, one 16ms beat after
         * the tap, and is unseen until then (fill backwards) */
        const PkBeats *lb = (const PkBeats *)pk_api_beats_lobby();
        OK(lb && lb->n == 1 && lb->beat[0].kind == PK_BK_FADE && lb->beat[0].sub == 1
           && lb->beat[0].to == PK_ANC_ROW && lb->beat[0].to_i == 2 && lb->beat[0].start_ms == 16
           && lb->beat[0].dur_ms == 220 && lb->total_ms == 236, "Join: Cleo's row fades up, 220ms after a 16ms beat");
        const PkBeatSample *ls = (const PkBeatSample *)pk_api_beat_sample(0, 0, 0);
        OK(ls && ls->apply && ls->opacity == 0, "Join: the row is unseen before its fade");
        OK(pk_api_words(PK_API_W_LOBBY_GONE, 2, line, sizeof line) < 0, "Join: no row went");
    }
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
    {
        /* grid "Leave" (A13): the row fades out, 220ms, then the rows below
         * close up on the card spring, 320ms */
        const PkBeats *lb = (const PkBeats *)pk_api_beats_lobby();
        OK(lb && lb->n == 2 && lb->beat[0].kind == PK_BK_FADE && lb->beat[0].sub == 0 && lb->beat[0].to == PK_ANC_ROW
           && lb->beat[0].to_i == 1 && lb->beat[0].start_ms == 16 && lb->beat[0].dur_ms == 220,
           "Leave: Bo's row fades out, 220ms after a 16ms beat");
        OK(lb && lb->n == 2 && lb->beat[1].kind == PK_BK_HOLD && lb->beat[1].to == PK_ANC_ROW && lb->beat[1].to_i == 1
           && lb->beat[1].start_ms == 236 && lb->beat[1].dur_ms == 320 && lb->beat[1].ease == PK_EASE_SPRING
           && lb->total_ms == 556, "Leave: then the rows below close up, 320ms on the card spring");
        OK(pk_api_words(PK_API_W_LOBBY_GONE, 1, line, sizeof line) > 0 && !strcmp(line, "2. Bo (You)"),
           "Leave: the row that went, as it read to Bo");
        static char bo_left[PK_API_TEXT_MAX];
        memcpy(bo_left, links[6], sizeof bo_left);
        OK(pk_api_read(bo_left) == 0 && !pk_api_beats_lobby() && pk_api_words(PK_API_W_LOBBY_GONE, 1, line, sizeof line) < 0,
           "a read forgets my lobby action");
        /* Cleo sees the same leave arrive over her lobby, and opened, and again */
        be(2);
        OK(pk_api_read(links[5]) == 0 && pk_api_adopt(links[6], 1) == 0, "Cleo adopts the leave over her lobby");
        lb = (const PkBeats *)pk_api_beats_now();
        OK(lb && lb->n == 2 && lb->beat[0].sub == 0 && lb->beat[0].to_i == 1 && lb->beat[0].start_ms == 16
           && lb->beat[1].start_ms == 236, "an arrival: the same two beats");
        OK(pk_api_words(PK_API_W_LOBBY_GONE, 1, line, sizeof line) > 0 && !strcmp(line, "2. Bo"),
           "the row that went, as it read to Cleo");
        OK(pk_api_read(links[5]) == 0 && pk_api_adopt(links[6], 0) == 0 && pk_api_beats_now()
           && ((const PkBeats *)pk_api_beats_now())->beat[0].start_ms == 100, "opened: the 100ms lead");
        OK(pk_api_adopt(links[6], 1) == 0 && !pk_api_beats_now(), "the same lobby again moves nothing");
        uint8_t seed2[32];
        for (int i = 0; i < 32; i++) seed2[i] = (uint8_t)(i * 7 + 3);
        pk_api_new(seed2, 0);
        OK(!pk_api_beats_lobby(), "a new lobby is no roster change");
        OK(pk_api_adopt(links[6], 1) == 0 && !pk_api_beats_now(), "another game's lobby, cold: nothing moves");
        OK(!pk_api_beats_lobby(), "a read is no lobby action of mine");
    }
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

    adopt_and_fan();
    arrange();

    /* ---- the corner index and the strip's count (I33) ---- */
    OK(pk_api_words(PK_API_W_INDEX, 0, line, sizeof line) > 0 && !strcmp(line, "1"), "a number's index");
    OK(pk_api_words(PK_API_W_INDEX, 47, line, sizeof line) > 0 && !strcmp(line, "+2"), "+2's index");
    OK(pk_api_words(PK_API_W_INDEX, 100, line, sizeof line) > 0 && !strcmp(line, "+4"), "a Wild +4's index");
    OK(pk_api_words(PK_API_W_INDEX, 42, line, sizeof line) == 0 && pk_api_words(PK_API_W_INDEX, 99, line, sizeof line) == 0,
       "a skip and a wild print a glyph, not an index");
    OK(pk_api_words(PK_API_W_INDEX, PK_CARD_HIDDEN, line, sizeof line) == -1, "a hidden card has no index");
    OK(pk_api_words(PK_API_W_STRIP_DRAWS, 3, line, sizeof line) > 0 && !strcmp(line, "\xc3\x97" "3"), "my draws, counted");
    OK(pk_api_words(PK_API_W_STRIP_DRAWS, 0, line, sizeof line) == -1, "no draws, no chip");

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

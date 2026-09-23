/* The bridge, proved WITHOUT a Mac: every entry point Swift will call, run
 * against a real game, so a broken boundary fails in CI rather than in Xcode.
 *
 *     make -C uttt/c ios-smoke
 */
#include "include/uttt_api.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int cond, const char *what)
{
    if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

int main(void)
{
    uti_new(77);
    ok(uti_n_plies() == 0, "a new game has no plies");
    ok(uti_over() == 0, "a new game is not over");
    ok(uti_forced() == 255, "a new game is unforced");

    uint8_t list[81];
    ok(uti_legal(list) == 81, "81 legal moves at the start");

    static const int mv[] = { 34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,
        70,71,72,3,29,19,17,73,14,50,45,6,59,47,23,53,75,30,35,74,22,42,60,54,
        0,2,24,58,38,18,1,9,5,77 };
    const int N = (int)(sizeof mv / sizeof *mv);
    /* A move can point at a block that is already won or full, and then the
     * reply goes anywhere - which uti_forced() does NOT say, because it is
     * the raw rule. The bubble's place line believes uti_active(), so the
     * difference between the two has to show up somewhere. */
    int sent_anywhere = 0;
    for (int i = 0; i < N; i++) {
        ok(uti_play(mv[i]), "a recorded move is legal");
        if (uti_forced() != 255 && uti_active() == 9) sent_anywhere++;
    }
    ok(sent_anywhere == 1, "one move in that game points at a decided block");
    ok(uti_n_plies() == N, "every move was played");

    /* UNDO IS THE PRODUCT'S, not a test hook: a staged bubble is a draft and
     * the player has to be able to change their mind before they send. */
    {
        int was_over = uti_over(), last = uti_move_at(N - 1);
        ok(uti_undo(), "the last move comes back");
        ok(uti_n_plies() == N - 1, "and the history is one shorter");
        ok(uti_cell(last) == 0, "the square it was on is empty again");
        ok(uti_over() == 0, "a game undone below its winning move is not over");
        ok(uti_play(last), "and the same move can be played again");
        ok(uti_over() == was_over && uti_n_plies() == N, "back where we were");
    }
    ok(uti_over() == 2, "O wins that game");

    uint8_t buf[64];
    int len = uti_encode(buf, sizeof buf);
    ok(len > 0 && len < 32, "the game codes to under 32 bytes");
    printf("  coded in %d bytes\n", len);

    int plies = uti_n_plies();
    ok(uti_decode(buf, len, 77), "it decodes");
    ok(uti_n_plies() == plies, "the decode is the same length");
    ok(uti_over() == 2, "and the same result");
    ok(uti_active() == -1, "a finished game sends nobody anywhere");

    int np = uti_draw(-1, mv[N-1], 1.f, 1.f);
    ok(np > 1000, "a finished board is thousands of polygons");
    ok(uti_point_count() > np, "with more points than polygons");
    ok(!uti_draw_overflow(), "and it fits in the buffer");
    printf("  %d polygons, %d points\n", np, uti_point_count());

    /* THE WORST BOARD IS NOT THE ONE ABOVE. That game ended in 50 plies with
     * half the sheet still blank; the buffers have to hold eighty-one marks
     * and every won block's grid underneath them. Play a full board out and
     * ask again, because the failure is silent. */
    {
        /* The FIRST legal square every time, which fills the sheet without
         * asking a bot anything - the bots are research and do not cross this
         * boundary. */
        uti_new(77);
        uint8_t list[81];
        int full = 0;
        while (!uti_over()) {
            if (uti_legal(list) <= 0 || !uti_play(list[0])) break;
            full++;
        }
        int wp = uti_draw(-1, -1, 1.f, 1.f);
        ok(!uti_draw_overflow(), "and so does a board played to the end");
        printf("  worst board: %d plies, %d polygons, %d points\n",
               full, wp, uti_point_count());
    }

    /* every polygon has to be inside the unit square, or a renderer that
     * trusts the kernel will draw outside its own view */
    const float *p = uti_points();
    int out_of_range = 0;
    for (int i = 0; i < uti_point_count() * 2; i++)
        if (p[i] < -0.25f || p[i] > 1.25f) out_of_range++;
    ok(out_of_range == 0, "no point escapes the board");
    if (out_of_range) printf("    %d stray coordinates\n", out_of_range);

    /* the display list must be deterministic - two phones, one sheet */
    int a = uti_draw(-1, mv[N-1], 1.f, 1.f);
    float first = uti_points()[0];
    int b = uti_draw(-1, mv[N-1], 1.f, 1.f);
    ok(a == b && first == uti_points()[0], "the same seed draws the same board");

    uti_new(77);

    /* ---- the bubble frame. 300x195 is somebody else's number, so the only
     * thing worth asserting is that nothing the kernel hands back falls
     * outside it - a renderer that trusts these draws off the image. */
    float bw = 0, bh = 0, x = 0, y = 0, s = 0, tw = 0, th = 0;
    uti_bubble_size(&bw, &bh);
    ok(bw == 300.f && bh == 195.f, "the bubble is 300 by 195");
    uti_bubble_board(&x, &y, &s);
    ok(s == 181.f, "a square tops out at 181 points");
    ok(x >= 0 && y >= 0 && x + s <= bw && y + s <= bh, "the board is inside the frame");
    uti_bubble_text(&x, &y, &tw, &th);
    ok(x >= 0 && x + tw <= bw && y + th <= bh, "the text column is inside the frame");
    ok(tw == 87.f, "and 87 points wide, which is why the place line wraps");
    /* MESSAGES STAMPS THE APP LOGO INTO THE TOP-LEFT CORNER of every bubble,
     * over whatever is under it. The board cannot live there or the badge
     * sits on its first block for the whole game, so the text column does. */
    {
        float bx = 0, by = 0, bs = 0;
        uti_bubble_board(&bx, &by, &bs);
        ok(x + tw <= bx, "the text column is left of the board, clear of the badge");
    }
    printf("  bubble %gx%g: board %g, text %g wide\n", bw, bh, s, tw);
    ok(uti_bubble_type(0) > 0 && uti_bubble_type(1) > 0, "both lines have a size");
    ok((uti_bubble_ink(0) & 0xffu) == 0xffu, "the headline ink is opaque");
    ok(uti_bubble_ink(0) != uti_bubble_ink(1), "the place is a different colour");

    /* ---- the place line. A new game is unforced, so it is "anywhere". */
    ok(uti_active() == 9, "a new game may be played anywhere");
    ok(!strcmp(uti_place_name(uti_active(), 0), "anywhere"), "and it is named so");
    ok(uti_play(40), "a move in the centre of the centre");
    ok(uti_active() == 4, "sends the reply to the centre block");
    ok(!strcmp(uti_place_name(4, 0), "centre"), "the place line says centre");
    ok(!strcmp(uti_place_name(7, 0), "bottom middle"), "block 7 is bottom middle");
    ok(!strcmp(uti_place_name(7, 1), "bottom-middle"), "and bottom-middle in a sentence");
    ok(uti_place_name(-1, 0)[0] == '\0' && uti_place_name(10, 0)[0] == '\0',
       "an impossible block names nothing");
    for (int b = 0; b <= 9; b++)
        ok(uti_place_name(b, 0)[0] && uti_place_name(b, 1)[0], "every block is named");

    /* ---- the rulebook door. Its shape is rough.js's and its numbers are
     * docs/UI.html's, so what is worth asserting is the handful of things a
     * renderer would silently get wrong if the geometry ever came back to
     * Swift: how many strokes there are, that they are the document's
     * colours, that the fill is painted under its own outline, and that the
     * button is NOT a scale-free drawing. */
    {
        int np = uti_draw_rulebook(54, 54);
        /* 17 hachure lines in the square and 7 in each leaf, every one of
         * them drawn twice, plus 4 edges drawn twice around each of the
         * three shapes: (17 + 7 + 7 + 4 + 4 + 4) * 2. */
        ok(np == 86, "the button is 86 strokes at 54 points");
        printf("  rulebook: %d strokes, %d points\n", np, uti_point_count());

        /* ONE POLYGON PER STROKE, which is what a canvas does when it strokes
         * a path - a stroke laid as overlapping quads blends with itself and
         * a 55% page comes out at 80%. */
        int ribbons = 0;
        for (int i = 0; i < np; i++)
            if (uti_poly_n()[i] >= 4 && uti_poly_n()[i] % 2 == 0) ribbons++;
        ok(ribbons == np, "each one is a single ribbon polygon");

        const uint32_t *c = uti_poly_rgba();
        int ink = 0, edge = 0, book = 0, other = 0;
        uint32_t ink_rgba = 0, book_rgba = 0;
        for (int i = 0; i < np; i++) {
            uint32_t rgb = c[i] >> 8;
            if (rgb == 0x25376bu)      { ink++;  ink_rgba = c[i]; }
            else if (rgb == 0x1b2a52u) {
                /* The outline and the book are the SAME ink at different
                 * strengths, so they are told apart by alpha rather than by
                 * hue - which is the point: one pen, one colour. */
                if ((c[i] & 0xffu) == 255) edge++; else { book++; book_rgba = c[i]; }
            } else other++;
        }
        ok(other == 0, "every stroke is one of the document's two inks");
        ok(ink == 34 && edge == 8, "the square is filled in ink and edged darker");
        ok(book == 44, "and the book is one colour, both leaves and both edges");

        /* THE FIGURE READS AGAINST THE GROUND, not into it. At full strength
         * the hachure closes into a solid navy block at 54 points and a pale
         * glyph on top of it vanishes - which is what the design document did
         * and what Chrome renders from it. So the fill is thin enough to see
         * the paper through and the book is the dark one. */
        ok((ink_rgba & 0xffu) < 128, "the square's fill lets the paper through");
        ok((book_rgba & 0xffu) > 200, "and the book is nearly solid on top of it");
        ok((book_rgba & 0xffu) > (ink_rgba & 0xffu),
           "the glyph is darker than the field it lies on");

        /* the fill is laid first so the outline lands ON it */
        ok((c[0] >> 8) == 0x25376bu, "the first stroke down is fill, not outline");

        const float *p = uti_points();
        int stray = 0;
        for (int i = 0; i < uti_point_count() * 2; i++)
            if (p[i] < -0.05f || p[i] > 1.05f) stray++;
        ok(stray == 0, "no stroke leaves the button");

        /* EVERY coordinate, not the first one: the first hachure line is the
         * scan through the top corner, which is a point rather than a line,
         * and a line of no length is jittered by nothing - so p[0] is the
         * same number whatever the seed does and would witness nothing. */
        double sum = 0;
        for (int i = 0; i < uti_point_count() * 2; i++) sum += p[i];
        int pc = uti_point_count();
        int again = uti_draw_rulebook(54, 54);
        double sum2 = 0;
        for (int i = 0; i < uti_point_count() * 2; i++) sum2 += uti_points()[i];
        ok(again == np && uti_point_count() == pc && sum2 == sum,
           "the same button draws the same way twice");

        /* NOT SCALE-FREE, and this is the whole reason the entry takes a
         * size: rough.js rounds a hachure gap to a whole unit, so a bigger
         * button is filled with MORE lines rather than the same lines
         * stretched. Draw it at 54 and hand a renderer a scaled copy and the
         * fill is wrong at every other size. */
        int big = uti_draw_rulebook(108, 108);
        ok(big > np, "a bigger button gets more hachure, not bigger hachure");
        printf("  rulebook at 108 points: %d strokes\n", big);
    }

    /* ---- the message, end to end as the host drives it: two devices are two
     * identities, and the only thing that crosses between them is text. */
    {
        static const uint8_t alex[16] = { 0xa1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        static const uint8_t vera[16] = { 0xb2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        static const uint8_t cleo[16] = { 0xc3, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        char inv[160], join[160], reply[160], other[160];

        uti_me(alex, 16);
        ok(uti_msg_open(1726990000) == 1, "alex opens an invitation");
        ok(uti_msg_seat() == UTI_SEAT_WAITING, "and waits on it");
        ok(uti_msg_mark() == 0 && !uti_msg_can_move(), "with no mark and no move");
        ok(!strcmp(uti_say(UTI_SAY_CAPTION), "New Ultimate Tic Tac Toe game"), "the invitation's caption");
        ok(uti_msg_text(inv, sizeof inv) > 3 && !strncmp(inv, "?m=", 3), "the invitation is a bare query");
        ok(uti_msg_door(0) == UTI_DOOR_NONE && uti_msg_door(1) == UTI_DOOR_TAKE_BACK,
           "a draft has no door; sent, it can be taken back");
        {
            char back[160];
            ok(uti_msg_take_back() && uti_msg_seat() == UTI_SEAT_CLOSED, "alex takes it back");
            ok(!strcmp(uti_say(UTI_SAY_BUBBLE_HEADLINE), "No game"), "and the bubble says so");
            ok(uti_msg_text(back, sizeof back) > 0 && uti_msg_prefer(back, inv) < 0,
               "the take-back is a message that outranks the invitation");
            ok(uti_msg_undo() && uti_msg_seat() == UTI_SEAT_WAITING, "cancelling it gives the invitation back");
            uti_me(vera, 16);
            ok(uti_msg_read(back) == 0 && uti_msg_seat() == UTI_SEAT_CLOSED && !uti_msg_play(40),
               "vera cannot sit down at a taken-back board");
            uti_me(alex, 16);
            ok(uti_msg_read(inv) == 0, "alex is back on the invitation");
        }

        uti_me(vera, 16);
        ok(uti_msg_read(inv) == 0, "vera reads it");
        ok(uti_msg_seat() == UTI_SEAT_OPEN && uti_msg_mark() == 1, "the seat is hers, as X");
        ok(!strcmp(uti_say(UTI_SAY_HEADLINE_PRE), "Your move"), "and the first move is hers");
        int mv = uti_hit(.5f, .5f);
        ok(mv == 40, "a tap in the middle is the centre of the centre");
        ok(uti_msg_play(mv) && uti_msg_sealed(), "her first move takes the seat");
        ok(uti_msg_seat() == UTI_SEAT_X && !uti_msg_can_move(), "she is X and it is O's turn");
        ok(uti_msg_text(join, sizeof join) > 0, "the join is one message");
        ok(!strcmp(uti_say(UTI_SAY_CAPTION), "Sent to the centre board."), "carrying her move");

        uti_me(alex, 16);
        ok(uti_msg_read(join) == 0 && uti_msg_seat() == UTI_SEAT_O, "alex opens it as O");
        ok(uti_msg_can_move() && uti_msg_play(36), "and answers");
        ok(uti_msg_text(reply, sizeof reply) > 0, "the reply");
        ok(uti_msg_prefer(reply, join) < 0 && uti_msg_prefer(join, reply) > 0, "the reply outranks the join");
        ok(uti_msg_same_game(reply, inv), "all one game");

        uti_me(cleo, 16);
        ok(uti_msg_read(reply) == 0 && uti_msg_seat() == UTI_SEAT_SPECTATOR, "cleo watches");
        ok(!uti_msg_play(0) && !uti_msg_undo(), "and cannot touch it");
        ok(!strcmp(uti_say(UTI_SAY_WATCH_LINE), "X to play"), "the spectator's line");

        ok(uti_msg_check(join) == 0 && uti_msg_check("?v=1&s=2") < 0, "check reads without adopting");
        ok(uti_msg_seat() == UTI_SEAT_SPECTATOR && uti_n_plies() == 2, "so cleo is still on the reply");
        ok(uti_msg_read("?v=1&s=2") < 0, "an old-format link is refused");
        ok(uti_msg_seat() == UTI_SEAT_SPECTATOR && uti_n_plies() == 2, "and changes nothing");
        ok(uti_msg_prefer("garbage", reply) > 0 && uti_msg_prefer(reply, "garbage") < 0,
           "an unreadable bubble always loses");

        uti_me(alex, 16);
        uti_new(99);
        ok(uti_play(4) && uti_play(40), "a seeded position");
        ok(uti_msg_seat_ids(vera, 16, alex, 16), "seated from two identities");
        ok(uti_msg_seat() == UTI_SEAT_X && uti_msg_can_move(), "alex is X and on move");
        ok(!uti_msg_seat_ids(vera, 16, vera, 16), "nobody plays themselves");
        ok(uti_msg_text(other, sizeof other) > 0, "and it has a link");
        ok(uti_say(12345)[0] == '\0', "an unknown sentence is empty, never NULL");
    }

    printf(fails ? "\n%d FAILED\n" : "\nbridge ok\n", fails);
    return fails ? 1 : 0;
}

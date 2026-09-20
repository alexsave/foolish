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
        uti_new(77);
        int full = 0;
        while (!uti_over()) {
            int m = uti_bot_move(1);
            if (m < 0 || !uti_play(m)) break;
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
    int m = uti_bot_move(20);
    ok(m >= 0 && m <= 80, "nib returns a move");

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
        int ink = 0, edge = 0, page = 0, other = 0;
        uint32_t page_rgba = 0;
        for (int i = 0; i < np; i++) {
            switch (c[i] >> 8) {
            case 0x25376bu: ink++;  break;
            case 0x1b2a52u: edge++; break;
            case 0xe2e8f4u: page++; page_rgba = c[i]; break;
            default:        other++;
            }
        }
        ok(other == 0, "every stroke is one of the document's three colours");
        ok(ink == 34 && edge == 8, "the square is filled in ink and edged darker");
        ok(page == 44, "and the book is one colour, both leaves and both edges");
        /* the glyph lies ON a hachured square, so it is a wash and not a
         * bright shape fighting the fill underneath it */
        ok((page_rgba & 0xffu) == 140, "the page is 55 percent, not solid");
        ok((c[0] & 0xffu) == 255, "while the square itself is opaque");

        /* the fill is laid first so the outline lands ON it */
        ok(c[0] == 0x25376bffu, "the first stroke down is fill, not outline");

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

    printf(fails ? "\n%d FAILED\n" : "\nbridge ok\n", fails);
    return fails ? 1 : 0;
}

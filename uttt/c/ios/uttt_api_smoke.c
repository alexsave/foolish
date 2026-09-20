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
    for (int i = 0; i < N; i++)
        ok(uti_play(mv[i]), "a recorded move is legal");
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
    printf("  %d polygons, %d points\n", np, uti_point_count());

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

    printf(fails ? "\n%d FAILED\n" : "\nbridge ok\n", fails);
    return fails ? 1 : 0;
}

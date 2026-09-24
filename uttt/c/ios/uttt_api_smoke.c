/* The bridge, proved WITHOUT a Mac: every entry point Swift will call, run
 * against a real game, so a broken boundary fails in CI rather than in Xcode.
 *
 *     make -C uttt/c ios-smoke
 */
#include "include/uttt_api.h"
#include <math.h>
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

    /* A WHOLE BOARD IS HANDED OVER, NOT COPIED: the host fills it off the
     * main thread from the kernel's own memory, and the kernel keeps nothing
     * of it resident (TESTFLIGHT_PLAN.md 12, memory). */
    {
        int npt = uti_point_count();
        UtiTaken t = uti_take();
        ok(t.n_polys == b && t.n_points == npt && t.points[0] == first
           && t.polys[b - 1].first + t.polys[b - 1].n <= npt, "a taken board is the draw, whole");
        ok(uti_point_count() == 0 && uti_points() == NULL, "and the kernel keeps none of it");
        int c = uti_draw(-1, mv[N-1], 1.f, 1.f);
        ok(c == b && uti_points() != t.points && uti_points()[0] == t.points[0]
           && uti_polys()[c - 1].n == t.polys[b - 1].n, "the next draw starts on buffers of its own");
        uti_taken_free(t);
        UtiTaken none = uti_take();
        uti_draw_last(0.f);
        ok(uti_point_count() >= 0 && !uti_draw_overflow(), "a draw after a take still has room");
        uti_taken_free(none);
    }

    /* THE COMPOSITOR'S PAPER IS THE SAME PAPER: every pixel of the BGRA
     * rows is the RGBA paper's with red and blue swapped, and the padding a
     * surface's stride leaves past each row is not written. */
    {
        enum { PW = 37, PH = 23, STRIDE = PW * 4 + 12 };
        static uint8_t rgba[PW * PH * 4], bgra[STRIDE * PH];
        memset(bgra, 0xab, sizeof bgra);
        uti_paper(rgba, PW, PH);
        uti_paper_bgra(bgra, PW, PH, STRIDE);
        int same = 1, pad = 1;
        for (int y = 0; y < PH; y++) {
            for (int x = 0; x < PW; x++) {
                const uint8_t *s = rgba + (y * PW + x) * 4, *d = bgra + y * STRIDE + x * 4;
                if (d[0] != s[2] || d[1] != s[1] || d[2] != s[0] || d[3] != s[3]) same = 0;
            }
            for (int k = PW * 4; k < STRIDE; k++) if (bgra[y * STRIDE + k] != 0xab) pad = 0;
        }
        ok(same, "the BGRA paper is the RGBA paper, red and blue swapped");
        ok(pad, "and the stride's padding is left alone");
    }

    uti_new(77);

    /* ---- the bubble frame. 300x195 is somebody else's number, so the only
     * thing worth asserting is that nothing the kernel hands back falls
     * outside it - a renderer that trusts these draws off the image. */
    float bw = 0, bh = 0, x = 0, y = 0, s = 0, tw = 0, th = 0;
    uti_bubble_size(&bw, &bh);
    ok(bw == 300.f && bh == 195.f, "the bubble is 300 by 195");
    ok(uti_bubble_scale(2.f) == 2.f && uti_bubble_scale(3.f) == 3.f && uti_bubble_scale(1.f) == 2.f,
       "the bubble bakes at the sender's scale, 2 to 3");
    /* THE LINES STOP ON THE PAPER, and none runs under the badge Messages
     * stamps into the top-left corner: every point of the bubble's board,
     * drawn with its own reach, lies 3 points or more inside the frame and
     * outside the badge's corner. The drawer's reach would run 23 past. */
    float ink[4];                                   /* left top right bottom */
    int under_badge = 0;
    #define BUBBLE_INK() do { \
        float bx = 0, by = 0, bs = 0; \
        uti_bubble_board(&bx, &by, &bs); \
        int np = uti_draw_bubble(4, -1); \
        const float *pt = uti_points(); \
        const UtiPoly *pq = uti_polys(); \
        ink[0] = ink[1] = 1e9f; ink[2] = ink[3] = -1e9f; under_badge = 0; \
        for (int i = 0; i < np; i++) \
            for (int k = 0; k < pq[i].n; k++) { \
                float px = bx + pt[(pq[i].first + k) * 2] * bs, py = by + pt[(pq[i].first + k) * 2 + 1] * bs; \
                if (px < ink[0]) ink[0] = px; \
                if (py < ink[1]) ink[1] = py; \
                if (px > ink[2]) ink[2] = px; \
                if (py > ink[3]) ink[3] = py; \
                if (px < 31.f && py < 24.f) under_badge = 1; \
            } \
        ok(np > 0, "the bubble's board draws"); \
    } while (0)

    /* a game in play: the board alone, centred, as large as the frame's
     * height allows */
    uti_bubble_board(&x, &y, &s);
    ok(s == 168.f, "the board is 168 points, so its lines (both ends drawn) stop on the frame");
    ok(fabsf(x + s / 2 - bw / 2) < .01f && fabsf(y + s / 2 - bh / 2) < .01f,
       "a game in play: the board is centred in the frame");
    uti_bubble_text(&x, &y, &tw, &th);
    ok(tw == 0.f && th == 0.f, "and has no words");
    BUBBLE_INK();
    printf("  bubble board ink (in play): left %.1f top %.1f right %.1f bottom %.1f\n",
           ink[0], ink[1], bw - ink[2], bh - ink[3]);
    ok(ink[0] >= 3.f && ink[1] >= 3.f && bw - ink[2] >= 3.f && bh - ink[3] >= 3.f,
       "a game in play: the bubble's lines stop 3 points inside the frame");
    ok(!under_badge, "a game in play: no line runs under Messages' badge");

    /* a finished game: the words in a column left of the board */
    {
        uint8_t list[81]; int guard = 0;
        while (!uti_over() && guard++ < 81 && uti_legal(list) > 0) uti_play(list[0]);
        ok(uti_over() != 0, "a game played out is over");
        uti_bubble_board(&x, &y, &s);
        float bx = x;
        uti_bubble_text(&x, &y, &tw, &th);
        ok(x >= 0 && x + tw <= bw && y + th <= bh, "a finished game: the text column is inside the frame");
        ok(tw >= 90.f, "and at least 90 points wide");
        ok(x + tw <= bx, "and left of the board");
        BUBBLE_INK();
        ok(ink[1] >= 3.f && bw - ink[2] >= 3.f && bh - ink[3] >= 3.f,
           "a finished game: the bubble's lines stop 3 points inside the frame");
        ok(!under_badge, "a finished game: no line runs under Messages' badge");
        printf("  bubble %gx%g: board %g, text %g wide\n", bw, bh, s, tw);
    }
    #undef BUBBLE_INK
    ok(uti_bubble_type(0) > 0 && uti_bubble_type(1) > 0, "both lines have a size");
    ok((uti_bubble_ink(0) & 0xffu) == 0xffu, "the headline ink is opaque");
    ok(uti_bubble_ink(0) != uti_bubble_ink(1), "the place is a different colour");
    uti_new(77);

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

    /* ---- THE "YOU ARE" O STAYS A RING (owner, TestFlight 1.0(6): "the drawn
     * O sometimes draws lines right through the circle"). Off the display
     * list the host fills: every quad's two ends are samples of the stroke,
     * and each must sit within 20% of the O's own mean distance from its
     * centre - a tail cutting a chord across the inside lands at half of it.
     * Over 20,000 game seeds, as the host draws them (model.seed &+ 4). */
    {
        int ring = 1, worst_seed = 0;
        float worst = 0.f;
        const float cx = .06f + (50.f + .3f * 4.f) * .0088f, cy = .06f + (50.f - .3f * 3.f) * .0088f;
        const float rx = (78.f - .3f * 10.f) * .0044f, ry = (76.f + .3f * 8.f) * .0044f;
        for (int32_t g = 1; g <= 20000; g++) {
            int m = uti_draw_mark(2, g + 4);
            const UtiPoly *q = uti_polys();
            const float *pt = uti_points();
            static float rho[4000];
            int n = 0;
            for (int i = 0; i < m && n + 2 <= 4000; i++) {
                if (q[i].n != 4) continue;                       /* the quads */
                const float *a = &pt[q[i].first * 2];
                float sx[2] = { (a[0] + a[2]) * .5f, (a[4] + a[6]) * .5f };
                float sy[2] = { (a[1] + a[3]) * .5f, (a[5] + a[7]) * .5f };
                for (int e = 0; e < 2; e++) {
                    float dx = (sx[e] - cx) / rx, dy = (sy[e] - cy) / ry;
                    rho[n++] = sqrtf(dx * dx + dy * dy);
                }
            }
            float lo = 1e9f, hi = 0.f, sum = 0.f;
            for (int i = 0; i < n; i++) { lo = fminf(lo, rho[i]); hi = fmaxf(hi, rho[i]); sum += rho[i]; }
            float mid = n ? sum / n : 1.f;
            float off = fmaxf(1.f - lo / mid, hi / mid - 1.f);
            if (off > worst) { worst = off; worst_seed = g; }
            if (n < 20 || off > .20f) ring = 0;
        }
        printf("  you-are O: worst stray %.3f of its radius (game seed %d)\n", worst, worst_seed);
        ok(ring, "the you-are O never cuts across itself, over 20,000 game seeds");
        int a = uti_draw_mark(2, 12345);
        double s1 = 0; for (int i = 0; i < uti_point_count() * 2; i++) s1 += uti_points()[i];
        int b = uti_draw_mark(2, 12345);
        double s2 = 0; for (int i = 0; i < uti_point_count() * 2; i++) s2 += uti_points()[i];
        ok(a == b && s1 == s2, "and one seed draws one O, every time");
    }

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
            if (uti_polys()[i].n >= 4 && uti_polys()[i].n % 2 == 0) ribbons++;
        ok(ribbons == np, "each one is a single ribbon polygon");

        const UtiPoly *cq = uti_polys();
        int ink = 0, edge = 0, book = 0, other = 0;
        uint32_t ink_rgba = 0, book_rgba = 0;
        for (int i = 0; i < np; i++) {
            uint32_t rgb = cq[i].rgba >> 8;
            if (rgb == 0x25376bu)      { ink++;  ink_rgba = cq[i].rgba; }
            else if (rgb == 0x1b2a52u) {
                /* The outline and the book are the SAME ink at different
                 * strengths, so they are told apart by alpha rather than by
                 * hue - which is the point: one pen, one colour. */
                if ((cq[i].rgba & 0xffu) == 255) edge++; else { book++; book_rgba = cq[i].rgba; }
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
        ok((cq[0].rgba >> 8) == 0x25376bu, "the first stroke down is fill, not outline");

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

    /* ---- the Again door. The bar is the rulebook square's pen stretched to
     * the width a phone gives it, so what matters is that every width a
     * phone can give fits the display list, that it is the rulebook's two
     * inks with the fill under the outline, and that it is the same bar on
     * every draw. */
    {
        int fits = 1, lo = 1 << 30, hi = 0;
        for (float w = 200; w <= 430; w += 10) {
            int np = uti_draw_door(w, 46);
            if (uti_draw_overflow() || np <= 0) fits = 0;
            if (np < lo) lo = np;
            if (np > hi) hi = np;
        }
        ok(fits, "the door fits at every width from 200 to 430 points");
        printf("  door: %d to %d strokes\n", lo, hi);

        int np = uti_draw_door(310, 46);
        const UtiPoly *cq = uti_polys();
        int fill = 0, edge = 0, other = 0, last_fill = -1, first_edge = np;
        for (int i = 0; i < np; i++) {
            if (cq[i].rgba == 0x25376b66u)      { fill++; last_fill = i; }
            else if (cq[i].rgba == 0x1b2a52ffu) { edge++; if (i < first_edge) first_edge = i; }
            else other++;
        }
        ok(other == 0 && fill > 0, "the door is the rulebook's two inks, hachured");
        ok(edge == 8, "and edged by four sides drawn twice");
        ok(last_fill < first_edge, "the fill is laid before the outline, so the edge is on top");

        const float *p = uti_points();
        int stray = 0, pc = uti_point_count();
        double sum = 0;
        for (int i = 0; i < pc * 2; i++) {
            if (p[i] < -0.05f || p[i] > 1.05f) stray++;
            sum += p[i];
        }
        ok(stray == 0, "no stroke leaves the bar");
        int again = uti_draw_door(310, 46);
        double sum2 = 0;
        for (int i = 0; i < uti_point_count() * 2; i++) sum2 += uti_points()[i];
        ok(again == np && uti_point_count() == pc && sum2 == sum,
           "the same door draws the same way twice");
        /* THE GAP IS A LENGTH IN POINTS, so twice the bar is nearly twice
         * the hachure (the diagonal's height share does not double, hence
         * 1.6 rather than 2) - not the same lines spread further apart. */
        int narrow = uti_draw_door(200, 46) - 8, wide = uti_draw_door(400, 46) - 8;
        ok(wide * 10 >= narrow * 16,
           "a wider door gets more hachure at the same gap, not wider hachure");

        /* OPPOSITE EDGES ARE THE SAME WEIGHT (owner, 2026-09-23: the top and
         * the bottom of Again read as different widths). An edge's weight is
         * how deep its ink band is across the edge: the spread of every
         * outline point on that side, in points, whatever the wobble did. */
        for (float w = 200; w <= 430; w += 23) {
            const float H = 46;
            int m = uti_draw_door(w, H);
            const UtiPoly *q = uti_polys();
            const float *pt = uti_points();
            float lo[4] = { 1e9f, 1e9f, 1e9f, 1e9f }, hi[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
            for (int i = 0; i < m; i++) {
                if (q[i].rgba != 0x1b2a52ffu) continue;
                double cx = 0, cy = 0;
                for (int k = 0; k < q[i].n; k++) {
                    cx += pt[(q[i].first + k) * 2]; cy += pt[(q[i].first + k) * 2 + 1];
                }
                cx /= q[i].n; cy /= q[i].n;
                /* 0 top, 1 bottom, 2 left, 3 right, by where the stroke sits */
                double dx = fmin(cx, 1 - cx) * w, dy = fmin(cy, 1 - cy) * H;
                int side = dy < dx ? (cy < .5 ? 0 : 1) : (cx < .5 ? 2 : 3);
                for (int k = 0; k < q[i].n; k++) {
                    float v = side < 2 ? pt[(q[i].first + k) * 2 + 1] * H
                                       : pt[(q[i].first + k) * 2] * w;
                    if (side == 1) v = H - v;
                    if (side == 3) v = w - v;
                    if (v < lo[side]) lo[side] = v;
                    if (v > hi[side]) hi[side] = v;
                }
            }
            float top = hi[0] - lo[0], bot = hi[1] - lo[1];
            float lft = hi[2] - lo[2], rgt = hi[3] - lo[3];
            if (w == 200)
                printf("  door edges at %g: top %.2f bottom %.2f left %.2f right %.2f\n",
                       w, top, bot, lft, rgt);
            if (fabsf(top - bot) > .05f || fabsf(lft - rgt) > .05f) {
                printf("  door %g: top %.2f bottom %.2f left %.2f right %.2f\n",
                       w, top, bot, lft, rgt);
                ok(0, "the door's opposite edges are the same weight");
                break;
            }
            if (w + 23 > 430) ok(1, "the door's opposite edges are the same weight");
        }

        /* ALL FOUR EDGES ARE ONE THICKNESS AS DRAWN (owner, four times, last
         * on TestFlight 1.0(6): "horizontal borders appear less thick than
         * vertical"). The host stretches the door's 0..1 to w by H, so this
         * takes the edge ribbons into POINTS the same way and cuts each side
         * straight across at nine places along it: the ink there is the
         * union of its two passes' spans on the cut. Every side's mean must
         * be within half a pixel at 3x of every other's - measured off the
         * geometry, never off a constant, so a width that is right in one
         * coordinate system and squashed in the other cannot pass. */
        {
            int even = 1;
            float worst = 0.f, worst_w = 0.f;
            for (float w = 100; w <= 430; w += 3.5f) {
                const float H = 46;
                int m = uti_draw_door(w, H);
                const UtiPoly *q = uti_polys();
                const float *pt = uti_points();
                int rib[4][2], nr[4] = { 0 };
                for (int i = 0; i < m; i++) {
                    if (q[i].rgba != 0x1b2a52ffu) continue;
                    double cx = 0, cy = 0;
                    for (int k = 0; k < q[i].n; k++) {
                        cx += pt[(q[i].first + k) * 2]; cy += pt[(q[i].first + k) * 2 + 1];
                    }
                    cx /= q[i].n; cy /= q[i].n;
                    double dx = fmin(cx, 1 - cx) * w, dy = fmin(cy, 1 - cy) * H;
                    int side = dy < dx ? (cy < .5 ? 0 : 1) : (cx < .5 ? 2 : 3);
                    if (nr[side] < 2) rib[side][nr[side]++] = i;
                }
                float mean[4];
                for (int sd = 0; sd < 4; sd++) {
                    float tot = 0.f;
                    int horiz = sd < 2;
                    for (int c = 1; c <= 9; c++) {
                        float at = (horiz ? w : H) * c / 10.f;
                        float iv[64][2]; int ni = 0;
                        for (int r = 0; r < nr[sd]; r++) {
                            const UtiPoly *P = &q[rib[sd][r]];
                            float xs[256]; int nx = 0;
                            for (int k = 0, j = P->n - 1; k < P->n; j = k++) {
                                /* a = along the side, b = across it, in points */
                                float ak = horiz ? pt[(P->first + k) * 2] * w : pt[(P->first + k) * 2 + 1] * H;
                                float bk = horiz ? pt[(P->first + k) * 2 + 1] * H : pt[(P->first + k) * 2] * w;
                                float aj = horiz ? pt[(P->first + j) * 2] * w : pt[(P->first + j) * 2 + 1] * H;
                                float bj = horiz ? pt[(P->first + j) * 2 + 1] * H : pt[(P->first + j) * 2] * w;
                                if ((ak > at) == (aj > at) || nx >= 256) continue;
                                xs[nx++] = bk + (bj - bk) * (at - ak) / (aj - ak);
                            }
                            for (int a = 1; a < nx; a++)
                                for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) {
                                    float t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t;
                                }
                            for (int a = 0; a + 1 < nx && ni < 64; a += 2) {
                                iv[ni][0] = xs[a]; iv[ni][1] = xs[a + 1]; ni++;
                            }
                        }
                        for (int a = 1; a < ni; a++)                 /* union */
                            for (int b = a; b > 0 && iv[b - 1][0] > iv[b][0]; b--) {
                                float t0 = iv[b][0], t1 = iv[b][1];
                                iv[b][0] = iv[b - 1][0]; iv[b][1] = iv[b - 1][1];
                                iv[b - 1][0] = t0; iv[b - 1][1] = t1;
                            }
                        float len = 0.f, lo = -1e9f, hi = -1e9f;
                        for (int a = 0; a < ni; a++) {
                            if (iv[a][0] > hi) { if (hi > lo) len += hi - lo; lo = iv[a][0]; hi = iv[a][1]; }
                            else if (iv[a][1] > hi) hi = iv[a][1];
                        }
                        if (hi > lo) len += hi - lo;
                        tot += len;
                    }
                    mean[sd] = tot / 9.f;
                }
                float mn = fminf(fminf(mean[0], mean[1]), fminf(mean[2], mean[3]));
                float mx = fmaxf(fmaxf(mean[0], mean[1]), fmaxf(mean[2], mean[3]));
                if ((mx - mn) * 3.f > worst) { worst = (mx - mn) * 3.f; worst_w = w; }
                if ((mx - mn) * 3.f > .5f || mn < 2.f) {
                    if (even) printf("  door %g: top %.2f bottom %.2f left %.2f right %.2f pt\n",
                                     w, mean[0], mean[1], mean[2], mean[3]);
                    even = 0;
                }
            }
            printf("  door edges: widest spread %.2f px at 3x (door %g)\n", worst, worst_w);
            ok(even, "all four door edges are one thickness, within half a pixel at 3x, at every width");
        }
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
        ok(!strcmp(uti_say_by(UTI_SAY_CAPTION, "$A1"), "$A1 wants a game. Tap to take it"),
           "the invitation's caption names its sender");
        ok(uti_say_bubble_mark() == 0, "and its bubble draws no mark");
        ok(uti_msg_text(inv, sizeof inv) > 3 && !strncmp(inv, "?m=", 3), "the invitation is a bare query");
        ok(uti_msg_door() == UTI_DOOR_NONE, "an invitation has no door");

        uti_me(vera, 16);
        ok(uti_msg_read(inv) == 0, "vera reads it");
        ok(uti_msg_seat() == UTI_SEAT_OPEN && uti_msg_mark() == 1, "the seat is hers, as X");
        ok(!strcmp(uti_say(UTI_SAY_HEADLINE_PRE), "Your move"), "and the first move is hers");
        int mv = uti_hit(.5f, .5f);
        ok(mv == 40, "a tap in the middle is the centre of the centre");
        ok(uti_msg_play(mv) && uti_msg_sealed(), "her first move takes the seat");
        ok(uti_msg_seat() == UTI_SEAT_X && !uti_msg_can_move(), "she is X and it is O's turn");
        ok(uti_msg_text(join, sizeof join) > 0, "the join is one message");
        ok(!strcmp(uti_say(UTI_SAY_CAPTION), "O to play, centre board"), "carrying her move");

        uti_me(alex, 16);
        ok(uti_msg_read(join) == 0 && uti_msg_seat() == UTI_SEAT_O, "alex opens it as O");
        ok(uti_msg_can_move() && uti_msg_play(36), "and answers");
        ok(uti_msg_can_replace(37) && !uti_msg_can_replace(36) && !uti_msg_can_replace(40)
           && !uti_msg_can_replace(0), "a change of mind: another free square of the centre only");
        ok(uti_draw_outline(4, 1.f) > 0 && uti_draw_outline(-1, 1.f) == 0,
           "the promise draws round a block, and nothing for none");
        ok(uti_msg_text(reply, sizeof reply) > 0, "the reply");
        ok(uti_msg_prefer(reply, join) < 0 && uti_msg_prefer(join, reply) > 0, "the reply outranks the join");
        ok(uti_msg_same_game(reply, inv), "all one game");

        uti_me(cleo, 16);
        ok(uti_msg_read(reply) == 0 && uti_msg_seat() == UTI_SEAT_SPECTATOR, "cleo watches");
        ok(!uti_msg_play(0) && !uti_msg_undo(), "and cannot touch it");
        ok(!strcmp(uti_say(UTI_SAY_WATCH_LINE), "X to play"), "the spectator's line");
        ok(!strcmp(uti_say_cell(40), "Centre board, centre square, X")
           && !strcmp(uti_say_cell(36), "Centre board, top left square, O")
           && !strcmp(uti_say_cell(0), "Top left board, top left square, empty"),
           "VoiceOver reads each square by block, square and mark");
        {
            float r[4];
            ok(uti_cell_rect(40, r) && uti_hit(r[0] + r[2] / 2, r[1] + r[3] / 2) == 40
               && !uti_cell_rect(81, r), "and its rectangle is the one a tap hits");
        }

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
        ok(!strcmp(uti_say(UTI_SAY_DOOR_SEND), "Send a board"), "the send door's words cross the bridge");
        ok(uti_insert_silence(1, 1) == UTI_INSERT_RETRY && uti_insert_silence(1, 0) == UTI_INSERT_LISTEN
           && uti_insert_silence(uti_insert_silence_ms() / 50, 1) == UTI_INSERT_DOOR,
           "an insert's silence crosses the bridge");
        ok(uti_send_hint_ms() == 3000, "the send hint's fuse crosses the bridge");
        ok(uti_drawer_up(667, 647, 1) && !uti_drawer_up(932, 932, 0) && !uti_drawer_up(667, 647, 0),
           "whether the drawer is up crosses the bridge");
    }

    {   /* one layout through the bridge: the waiting strip's board is the
         * sheet's centre, as tall as the strip allows, its words beside it,
         * and every field crosses intact */
        UtiSheet L = uti_sheet((UtiSheetIn){ .w = 440.f, .h = 280.f, .kind = UTI_SHEET_WAIT, .words = 1 });
        ok(fabsf(L.board[0] + L.board[2] / 2 - 220.f) < 1e-3f
           && fabsf(L.board[1] + L.board[2] / 2 - 140.f) < 1e-3f, "the waiting board is centred on the strip");
        ok(fabsf(L.board[2] - (280.f - 2.f * L.vpad)) < 1e-3f, "and as tall as the strip allows");
        ok(L.words_side == 1 && L.words[0] == L.hpad && L.words[0] + L.words[2] < L.board[0],
           "with its words in the column beside it");
        ok(L.t == 0.f && L.words_alpha == 1.f && L.door_alpha == 0.f,
           "shown, and no door");
        printf("  sheet: waiting strip 440x280 board %.1f at %.1f,%.1f, words %.1f wide\n",
               L.board[2], L.board[0], L.board[1], L.words[2]);
    }

    printf(fails ? "\n%d FAILED\n" : "\nbridge ok\n", fails);
    return fails ? 1 : 0;
}

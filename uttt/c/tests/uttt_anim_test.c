/* The motion plan (src/uttt_anim.c) and the two draws it composites:
 * docs/UI.html's timings, the order of the beats, and that the board a host
 * caches plus the stroke it draws over it is the whole board. */
#include "../src/uttt.h"
#include "../src/uttt_anim.h"
#include "../src/uttt_draw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static int same_rect(const float *a, const float *b)
{
    for (int i = 0; i < 4; i++) if (fabsf(a[i] - b[i]) > 1e-5f) return 0;
    return 1;
}

static UtttPt PT[400000];
static UtttPoly PO[60000];

/* A display list's ribbon width: each polygon's area over half its perimeter,
 * weighted by area, over the polygons at or above the `top` share of the
 * widest (0: all of them) - the heavy strokes of a sheet. */
static double ribbon_width(const UtttDL *d, float top)
{
    double wmax = 0, sa = 0, sw = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < d->n_poly; i++) {
            const UtttPoly *p = &d->poly[i];
            double a = 0, per = 0;
            for (int k = 0; k < p->n; k++) {
                const UtttPt *u = &d->pt[p->first + k], *v = &d->pt[p->first + (k + 1) % p->n];
                a += (double)u->x * v->y - (double)v->x * u->y;
                per += hypot(v->x - u->x, v->y - u->y);
            }
            a = fabs(a) / 2;
            if (per <= 0) continue;
            double wd = 2 * a / per;
            if (pass == 0) { if (wd > wmax) wmax = wd; continue; }
            if (wd < top * wmax) continue;
            sa += a; sw += a * wd;
        }
    return sa > 0 ? sw / sa : 0;
}

int main(void)
{
    UtttGame g; uttt_init(&g);
    UtttFrame f;
    float r4[4], r0[4], r9[4], a;

    /* nothing played: at rest */
    UtttMotion m = uttt_motion(&g, UTTT_CH_STAGE);
    uttt_motion_at(&m, 0, &f);
    OK(m.ch == UTTT_CH_STILL && !f.running && f.mark_t == 1.f, "an empty board does not move");

    /* X in the centre of the centre: from anywhere (9) to the centre (4).
     * PRE (stage): the ink draws, the highlighter stays on the sheet it was
     * played in, and the centre block is outlined once the ink is down. */
    uttt_play(&g, 4 * 9 + 4);
    m = uttt_motion(&g, UTTT_CH_STAGE);
    uttt_wash_rect(4, r4, &a); uttt_wash_rect(9, r9, NULL); uttt_wash_rect(0, r0, NULL);
    OK(m.mark == UTTT_X && m.ink_ms == 260, "an X inks in 260 ms");
    OK(m.from == 9 && m.to == 9, "pre: the wash stays where the move was played");
    OK(m.outline == 4 && m.outline_at == 260 && !m.outline_fade,
       "pre: the block it sends to is outlined once the ink lands");
    OK(m.end_ms == 260 + UTTT_MS_OUTLINE, "pre: it rests once the outline is drawn round");

    uttt_motion_at(&m, 0, &f);
    OK(f.mark_t == 0.f && !f.landed && f.running, "at 0 nothing is drawn yet");
    OK(same_rect(f.wash, r9), "and the wash is still where it was");
    OK(f.outline_t == 0.f, "and no outline yet");
    float prev = 0.f; int mono = 1, still_wash = 1;
    for (int t = 0; t <= m.end_ms + 100; t += 4) {
        uttt_motion_at(&m, t, &f);
        if (t <= 260 && f.mark_t + 1e-6f < prev) mono = 0;
        if (t <= 260) prev = f.mark_t;
        if (!same_rect(f.wash, r9)) still_wash = 0;
    }
    OK(mono, "the ink only moves forward");
    OK(still_wash, "pre: the highlighter never moves before Send");
    uttt_motion_at(&m, 130, &f);
    OK(f.mark_t > .5f && f.mark_t < 1.f, "halfway through, the ease-out curve is past half");
    uttt_motion_at(&m, 259, &f);
    OK(!f.landed && f.outline_t == 0.f, "not landed a millisecond early, and no outline");
    uttt_motion_at(&m, 260, &f);
    OK(f.landed && f.mark_t == 1.f, "landed at 260");
    uttt_motion_at(&m, 260 + UTTT_MS_OUTLINE / 2, &f);
    OK(f.outline == 4 && f.outline_t > .3f && f.outline_t < 1.f && f.outline_a == 1.f,
       "pre: the outline is drawn round after the ink");
    OK(!f.settled && f.running, "not settled while the outline draws");
    uttt_motion_at(&m, m.end_ms, &f);
    OK(!f.running && f.settled && f.outline_t == 1.f && same_rect(f.wash, r9),
       "at the end: the mark, the wash where it was, the whole outline");

    /* A DRAFT SHOWN AGAIN is that last frame, still */
    {
        UtttMotion md = uttt_motion(&g, UTTT_CH_DRAFT);
        UtttFrame fd; uttt_motion_at(&md, 0, &fd);
        OK(!fd.running && fd.mark_t == 1.f && fd.outline == 4 && fd.outline_t == 1.f
           && fd.outline_a == 1.f && same_rect(fd.wash, r9) && !memcmp(fd.wash, f.wash, sizeof f.wash),
           "draft: the stage's last frame, still");
    }

    /* POST (Send): only the highlighter moves, to the outlined block, and the
     * outline fades as the tint arrives. */
    {
        UtttMotion mb = uttt_motion(&g, UTTT_CH_SETTLE);
        UtttFrame fb;
        uttt_motion_at(&mb, 0, &fb);
        OK(fb.running && fb.mark_t == 1.f && same_rect(fb.wash, r9) && fb.outline == 4
           && fb.outline_t == 1.f && fb.outline_a == 1.f,
           "post: at Send the mark is down, the wash where it was, the outline whole");
        uttt_motion_at(&mb, UTTT_MS_WASH_MINE / 2, &fb);
        OK(!same_rect(fb.wash, r9) && !same_rect(fb.wash, r4) && fb.wash[2] < r9[2] && fb.wash[2] > r4[2],
           "post: mid-travel the rect is between the sheet and the block");
        OK(fb.outline_a > 0.f && fb.outline_a < 1.f && fb.mark_t == 1.f, "post: the outline fading, nothing else");
        uttt_motion_at(&mb, mb.end_ms, &fb);
        OK(!fb.running && same_rect(fb.wash, r4) && fb.wash_rgba == uttt_wash_rgba(a) && fb.outline < 0,
           "post: the tint where the outline was, and the outline gone");
        OK(mb.end_ms == UTTT_MS_WASH_MINE, "post: it lasts the wash's travel");
    }

    /* THE OUTLINE IS THE TINT'S RECT AND COLOUR, drawn round by the pen */
    {
        UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_outline(&d, 4, 7, 1.f);
        int whole = d.n_poly, ink = whole > 0;
        float lo[2] = { 9, 9 }, hi[2] = { -9, -9 };
        for (int i = 0; i < d.n_poly; i++)
            if ((d.poly[i].rgba >> 8) != (uttt_wash_rgba(1.f) >> 8)) ink = 0;
        for (int i = 0; i < d.n_pt; i++) {
            if (d.pt[i].x < lo[0]) lo[0] = d.pt[i].x;
            if (d.pt[i].y < lo[1]) lo[1] = d.pt[i].y;
            if (d.pt[i].x > hi[0]) hi[0] = d.pt[i].x;
            if (d.pt[i].y > hi[1]) hi[1] = d.pt[i].y;
        }
        OK(ink, "outline: every stroke is the highlighter's colour");
        OK(fabsf(lo[0] - r4[0]) < .012f && fabsf(lo[1] - r4[1]) < .012f
           && fabsf(hi[0] - (r4[0] + r4[2])) < .012f && fabsf(hi[1] - (r4[1] + r4[3])) < .012f,
           "outline: it runs round the tint's own rect");
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_outline(&d, 4, 7, .5f);
        int half = d.n_poly;
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_outline(&d, 4, 7, 0.f);
        int none = d.n_poly;
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_outline(&d, -1, 7, 1.f);
        OK(half > 0 && half < whole && none == 0 && d.n_poly == 0,
           "outline: drawn round to t, nothing at 0 or for no block");
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_outline(&d, 4, 7, 1.f);
        UtttPt p0 = d.pt[d.n_pt / 2];
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_outline(&d, 4, 7, 1.f);
        OK(d.n_poly == whole && d.pt[d.n_pt / 2].x == p0.x && d.pt[d.n_pt / 2].y == p0.y,
           "outline: the same seed draws the same wobble on both phones");
    }

    /* O answers into the top-left: centre (4) to top-left (0) */
    uttt_play(&g, 4 * 9 + 0);
    m = uttt_motion(&g, UTTT_CH_REPLAY);
    OK(m.mark == UTTT_O && m.ink_ms == 340, "an O inks in 340 ms");
    OK(m.end_ms == 340 + 340, "my own replay rests once my wash arrives");
    OK(m.outline < 0, "my own bubble reopened: no promise, the wash moves");
    uttt_motion_at(&m, 170, &f);
    OK(f.mark_t > .5f && f.mark_t < 1.f, "an O eases out too");
    m = uttt_motion(&g, UTTT_CH_THEIRS);
    OK(m.wash_ms == 420 && m.end_ms == 340 + 420, "their move: the wash takes longer, and nothing rings after it");
    OK(m.from == 4 && m.to == 0, "from the centre to the top left");
    OK(m.outline < 0 && m.wash_at == 340, "their move: no outline, the wash moves after the ink");
    uttt_motion_at(&m, 340 + 420, &f);
    OK(same_rect(f.wash, r0), "and lands on it");
    m = uttt_motion(&g, UTTT_CH_ARRIVAL);
    OK(m.wash_ms == 420 && m.end_ms == 340 + 420, "an arrival is their move too, with no ring");
    m = uttt_motion(&g, UTTT_CH_STILL);
    uttt_motion_at(&m, 0, &f);
    OK(!f.running && f.mark_t == 1.f && same_rect(f.wash, r0), "STILL is the resting board");

    /* THE CACHE AND THE STROKE ARE THE WHOLE BOARD: under + last == board */
    {
        UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 60000);
        UtttDrawOpts o = uttt_draw_opts(7);
        o.last = 4 * 9 + 0;
        uttt_draw_board(&d, &g, &o);
        int whole = d.n_poly;
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        o.mark_t = 0.f;
        uttt_draw_board(&d, &g, &o);
        int under = d.n_poly;
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_last(&d, &g, 7, 1.f);
        OK(under + d.n_poly == whole && d.n_poly > 0, "the cached board plus the last mark is the board");
    }

    /* THE SETTLEMENT (UI.html 04, 05): the final move of the diagonal
     * fixture wins the bottom-right block and the game. At stage only the
     * mark draws and the big mark and the line wait for Send; at Send (B)
     * the big mark falls, then the line; an opened or arrived bubble plays
     * both halves after the ink; and the cache plus the stroke plus the
     * settlement is the whole board. */
    {
        static const uint8_t diag[] = { 79, 63, 5, 45, 8, 76, 42, 61, 70, 71, 78, 55, 15,
                                        58, 36, 1, 11, 24, 4, 40, 39, 31, 80, 35, 0 };
        UtttGame w; uttt_init(&w);
        for (unsigned i = 0; i < sizeof diag && !w.over; i++) uttt_play(&w, diag[i]);
        UtttMotion ms = uttt_motion(&w, UTTT_CH_STAGE);
        float rf[4]; uttt_wash_rect(ms.from, rf, NULL);
        OK(ms.fall_at == ms.ink_ms && ms.line_at == ms.ink_ms + UTTT_MS_FALL
           && ms.end_ms == ms.line_at + UTTT_MS_LINE,
           "pre: the winning move draws its mark, then the big mark, then the line");
        OK(ms.outline < 0, "pre: a game it ended sends nobody anywhere - no outline");
        uttt_motion_at(&ms, ms.ink_ms - 1, &f);
        OK(f.fall_t == 0.f && f.line_t == 0.f, "pre: nothing of the settlement before the ink lands");
        uttt_motion_at(&ms, ms.fall_at + UTTT_MS_FALL / 2, &f);
        OK(f.mark_t == 1.f && f.fall_t > .3f && f.fall_t < 1.f && f.line_t == 0.f,
           "pre: the big mark falls after the small one");
        uttt_motion_at(&ms, ms.line_at + UTTT_MS_LINE / 2, &f);
        OK(f.fall_t == 1.f && f.line_t > .3f && f.line_t < 1.f, "pre: then the line");
        OK(same_rect(f.wash, rf), "pre: the wash still on the block the move was played in");
        uttt_motion_at(&ms, ms.end_ms, &f);
        OK(!f.running && f.fall_t == 1.f && f.line_t == 1.f && same_rect(f.wash, rf),
           "pre: at rest, the whole settlement and the wash unmoved");
        UtttMotion mb = uttt_motion(&w, UTTT_CH_SETTLE);
        uttt_motion_at(&mb, 0, &f);
        OK(f.mark_t == 1.f && f.fall_t == 1.f && f.line_t == 1.f && f.running,
           "post: at Send the whole settlement is already down");
        uttt_motion_at(&mb, mb.end_ms / 2, &f);
        OK(f.fall_t == 1.f && f.line_t == 1.f && same_rect(f.wash, rf)
           && (f.wash_rgba & 0xff) < (uttt_wash_rgba(.3f) & 0xff),
           "post: the game is over, so the wash leaves - nothing else moves");
        UtttMotion md = uttt_motion(&w, UTTT_CH_THEIRS);
        uttt_motion_at(&md, md.ink_ms - 1, &f);
        OK(f.fall_t == 0.f && md.fall_at == md.ink_ms && md.line_at == md.ink_ms + UTTT_MS_FALL
           && md.wash_at == md.line_at + UTTT_MS_LINE && md.outline < 0,
           "theirs: small mark, big mark, line, then the highlighter");
        UtttMotion mr = uttt_motion(&w, UTTT_CH_DRAFT);
        uttt_motion_at(&mr, 0, &f);
        OK(!f.running && f.mark_t == 1.f && f.fall_t == 1.f && f.line_t == 1.f && same_rect(f.wash, rf),
           "draft: the winning move staged, shown again, has its settlement and the wash unmoved");
        /* a move that took a block and did not end the game: a prefix of
         * the fixture whose last move won its block */
        int took = 0;
        for (unsigned k = 1; k < sizeof diag - 1 && !took; k++) {
            UtttGame p; uttt_init(&p);
            for (unsigned i = 0; i < k; i++) uttt_play(&p, diag[i]);
            int b = diag[k - 1] / 9;
            if (p.over || (uttt_block(&p, b) != UTTT_X && uttt_block(&p, b) != UTTT_O)) continue;
            took = 1;
            UtttMotion mps = uttt_motion(&p, UTTT_CH_STAGE);
            OK(mps.fall_at == mps.ink_ms && mps.line_at < 0 && mps.outline == uttt_active(&p)
               && mps.outline_at == mps.ink_ms + UTTT_MS_FALL,
               "pre: a block-taking move draws its big mark, then the outline");
            UtttMotion mp = uttt_motion(&p, UTTT_CH_DRAFT);
            UtttFrame fs;
            uttt_motion_at(&mp, 0, &f);
            uttt_motion_at(&mps, mps.end_ms, &fs);
            OK(!f.running && f.fall_t == 1.f && f.mark_t == 1.f && f.outline == fs.outline
               && !memcmp(f.wash, fs.wash, sizeof f.wash) && f.wash_rgba == fs.wash_rgba,
               "draft: a block-taking move staged, shown again, is the stage's last frame");
        }
        OK(took, "draft: the fixture has a block-taking move before the last");

        UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 60000);
        UtttDrawOpts o = uttt_draw_opts(7);
        o.last = w.move[w.n_plies - 1];
        uttt_draw_board(&d, &w, &o);
        int whole = d.n_poly;
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        o.mark_t = 0.f; o.fall_t = 0.f; o.meta_t = 0.f;
        uttt_draw_board(&d, &w, &o);
        int under = d.n_poly;
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_last(&d, &w, 7, 1.f);
        int last = d.n_poly;
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_settle(&d, &w, 7, 1.f, 1.f);
        OK(under + last + d.n_poly == whole && d.n_poly > 0,
           "settle: the cache, the last mark and the settlement are the whole board");
    }

    /* THE WIN LINE IS THE HEAVIEST INK ON THE SHEET (owner, 2026-09-23:
     * "winning diagonal needs to be thicker"): its width, area over half the
     * perimeter of each ribbon and weighted by area, is at least twice the
     * major grid lines' - the widest ribbons of an empty board. */
    {
        static const uint8_t diag[] = { 79, 63, 5, 45, 8, 76, 42, 61, 70, 71, 78, 55, 15,
                                        58, 36, 1, 11, 24, 4, 40, 39, 31, 80, 35, 0 };
        UtttGame w; uttt_init(&w);
        for (unsigned i = 0; i < sizeof diag && !w.over; i++) uttt_play(&w, diag[i]);
        UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 60000);
        uttt_draw_settle(&d, &w, 7, 0.f, 1.f);
        double line = ribbon_width(&d, 0.f);
        UtttGame e; uttt_init(&e);
        uttt_dl_init(&d, PT, 400000, PO, 60000);
        UtttDrawOpts o = uttt_draw_opts(7);
        o.active = -1;
        uttt_draw_board(&d, &e, &o);
        double major = ribbon_width(&d, .9f);
        printf("  win line %.5f, major grid %.5f (x%.2f)\n", line, major, line / major);
        OK(line > 0 && major > 0 && line >= 2.0 * major, "the win line is at least twice the major grid line");
    }

    /* THE MAIN LINES STOP NEAR THE BOARD, as UI.html draws them (5%) */
    {
        UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 60000);
        UtttDrawOpts o = uttt_draw_opts(7);
        uttt_draw_board(&d, &g, &o);
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < d.n_pt; i++) {
            if (d.pt[i].x < lo) lo = d.pt[i].x;
            if (d.pt[i].x > hi) hi = d.pt[i].x;
        }
        printf("  drawer board ink: x %.3f .. %.3f\n", lo, hi);
        OK(lo > -.07f && hi < 1.07f, "the drawer's lines run no more than 7% past the board");
    }

    /* THE RULEBOOK DOOR IS ONE SIZE AT EVERY HEIGHT (owner, 2026-09-23): a
     * bit bigger than it was collapsed (38), a bit smaller than expanded
     * (54) - the midpoint, 46 - and on the strip its column is wide enough
     * for it, so it never sits on the board's ink. */
    {
        int one = 1, fits = 1;
        for (float h = 220.f; h <= 900.f; h += .25f) {
            UtttSheet o;
            uttt_sheet(&(UtttSheetIn){ .w = 440.f, .h = h, .kind = UTTT_SHEET_PLAY }, &o);
            if (o.door != 46.f) one = 0;
            if (o.t == 0.f && o.col < o.door) fits = 0;
        }
        OK(one, "the rulebook door is 46 points at every drawer height");
        OK(fits, "on the strip the door's column is as wide as the door");
    }

    /* THE FOUR MAIN LINES ARE CENTRED ON THE GRID (owner, 2026-09-23: "major
     * grid lines aren't centered on the grid"). Measured on the INK, not on
     * the endpoints asked for: those were always symmetric, and the pen still
     * ran long at the start and short at the end. For each main line, the ink
     * within a few percent of its 1/3 or 2/3 track is that line; its overshoot
     * past the board at each end, averaged over many games, must match, and
     * its track must sit on the block boundary. */
    {
        UtttGame e; uttt_init(&e);
        double bias[4] = { 0 }, track[4] = { 0 }, worst = 0;
        int nan = 0;
        const int SEEDS = 60;
        for (int sd = 1; sd <= SEEDS; sd++) {
            UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 60000);
            UtttDrawOpts o = uttt_draw_opts(sd * 7919);
            uttt_draw_board(&d, &e, &o);
            for (int i = 0; i < d.n_pt; i++)
                if (isnan(d.pt[i].x) || isnan(d.pt[i].y)) nan++;
            for (int L = 0; L < 4; L++) {
                /* 0,1 vertical at x = 1/3, 2/3; 2,3 horizontal at y = 1/3, 2/3 */
                float at = (L % 2 + 1) / 3.f, lo = 1e9f, hi = -1e9f;
                double sum = 0; int n = 0;
                for (int i = 0; i < d.n_pt; i++) {
                    float across = L < 2 ? d.pt[i].x : d.pt[i].y;
                    float along  = L < 2 ? d.pt[i].y : d.pt[i].x;
                    if (isnan(across) || isnan(along) || fabsf(across - at) > .03f) continue;
                    if (along > -.005f && along < 1.005f) continue;  /* only past the board */
                    if (along < lo) lo = along;
                    if (along > hi) hi = along;
                    sum += across; n++;
                }
                double b = (-lo) - (hi - 1.f);
                bias[L] += b / SEEDS;
                track[L] += (n ? sum / n - at : 1) / SEEDS;
                if (fabs(b) > worst) worst = fabs(b);
            }
        }
        printf("  main lines, start minus end overshoot: %.4f %.4f %.4f %.4f (worst game %.3f)\n",
               bias[0], bias[1], bias[2], bias[3], worst);
        printf("  main lines, track off 1/3 or 2/3: %.4f %.4f %.4f %.4f\n",
               track[0], track[1], track[2], track[3]);
        int even = 1, on = 1;
        for (int L = 0; L < 4; L++) {
            if (fabs(bias[L]) > .004) even = 0;
            if (fabs(track[L]) > .003) on = 0;
        }
        OK(nan == 0, "no point of the board is NaN (a lifted stroke's last segment was)");
        OK(even, "each main line overshoots the board by the same at both ends");
        OK(on, "each main line sits on its block boundary");
        OK(worst < .03, "no game's main line is more than 3% longer at one end");
    }

    /* ONE LAYOUT, EVERY SCREEN: the board's centre is the sheet's centre at
     * every height, its side never steps as the drawer moves, it is as large
     * as the sheet allows, and the words sit clear of it (docs/UI.html "What
     * holds which edge": the board holds the centre and is the only thing
     * that resizes; owner 2026-09-23: the board as large as possible). */
    {
        static const float W[] = { 375.f, 393.f, 440.f };
        static const struct { int kind, words; } K[] = {
            { UTTT_SHEET_PLAY, 0 },           /* a live game                  */
            { UTTT_SHEET_PLAY, 1 },           /* the end: the verdict         */
            { UTTT_SHEET_WATCH, 1 },          /* the spectator's line         */
            { UTTT_SHEET_WAIT, 1 },           /* the waiting words            */
        };
        float reach = .135f * UTTT_REACH;
        int centred = 1, clear = 1, onsheet = 1, grows = 1, squeezed = 0, jumps = 0;
        float worst = 0.f;
        for (int wi = 0; wi < 3; wi++)
        for (int ki = 0; ki < 4; ki++) {
            UtttSheetIn in = { .w = W[wi], .kind = K[ki].kind, .words = K[ki].words };
            float prev = -1.f, pa = -1.f, pb = -1.f;
            for (float h = 220.f; h <= 900.f; h += .25f) {
                UtttSheet o;
                in.h = h;
                uttt_sheet(&in, &o);
                float s = o.board[2];
                if (fabsf(o.board[0] + s / 2 - in.w / 2) > 1e-3f
                    || fabsf(o.board[1] + s / 2 - h / 2) > 1e-3f) centred = 0;
                if (prev >= 0.f && fabsf(s - prev) > worst) worst = fabsf(s - prev);
                if (prev >= 0.f && s < prev - 1e-3f && o.t == 0.f) grows = 0;
                prev = s;
                if (s * (1.f + 2.f * reach) > in.w - 2.f * o.hpad + 1e-3f || s > h - 2.f * o.vpad + 1e-3f)
                    onsheet = 0;
                /* the words' box: beside the ink on the strip, above the
                 * square in the band, and on the sheet either way */
                float il = (in.w - s * (1.f + 2.f * reach)) / 2, ir = in.w - il;
                const float *b = o.words, *bb = o.band;
                if (b[0] < o.hpad - 1e-3f || b[0] + b[2] > in.w - o.hpad + 1e-3f || b[2] < 0.f) clear = 0;
                if (o.words_alpha > 0.f && b[0] + b[2] > il + 1e-3f && b[0] < ir - 1e-3f) clear = 0;
                if (o.band_alpha > 0.f && bb[1] + bb[3] > o.board[1] + 1e-3f) clear = 0;
                /* NEVER SQUEEZED: a copy that shows has the room it needs,
                 * and the two copies fade rather than jump between heights */
                if (o.words_alpha > 0.f && b[2] < 30.f) squeezed = 1;
                if (o.band_alpha > 0.f && bb[3] < 38.f) squeezed = 1;
                if (pa >= 0.f && (fabsf(o.words_alpha - pa) > .02f || fabsf(o.band_alpha - pb) > .02f))
                    jumps = 1;
                pa = o.words_alpha; pb = o.band_alpha;
            }
        }
        printf("  sheet: largest side step per quarter point of drawer %.3f pt\n", worst);
        OK(centred, "every screen's board is centred on the sheet at every height");
        OK(worst < .6f, "and its side never steps as the drawer moves");
        OK(grows, "on the strip a taller drawer never gives a smaller board");
        OK(onsheet, "its lines stay on the sheet");
        OK(clear, "and its words sit beside it on the strip and above it in the band");
        OK(!squeezed, "a copy of the words shows only in a box with the room it needs");
        OK(!jumps, "and the words crossfade between column and band, never switch in a step");

        /* AS LARGE AS THE SHEET ALLOWS, on every screen, words or none: on
         * the strip the drawer's height less the grab handle's margins (or
         * the width less the columns, on a narrow phone), expanded the width
         * less the margins. Worked out here from the sheet's own numbers,
         * not from the layout's arithmetic. */
        static const struct { float w, h; } D[] = {
            { 440.f, 274.f }, { 440.f, 340.f }, { 393.f, 300.f }, { 375.f, 250.f },   /* compact  */
            { 440.f, 800.f }, { 393.f, 700.f }, { 375.f, 541.f },                     /* expanded */
        };
        int biggest = 1, words_room = 1;
        for (int di = 0; di < 7; di++)
        for (int ki = 0; ki < 4; ki++) {
            UtttSheet o;
            uttt_sheet(&(UtttSheetIn){ .w = D[di].w, .h = D[di].h, .kind = K[ki].kind,
                                       .words = K[ki].words }, &o);
            int compact = di < 4;
            float want = compact
                ? fminf(D[di].h - 2.f * 13.f, (D[di].w - 2.f * 13.f - 2.f * 46.f) / (1.f + 2.f * reach))  /* columns: the 46-point door */
                : (D[di].w - 2.f * 16.f) / (1.f + 2.f * reach);
            if (fabsf(o.board[2] - want) > 1e-2f) {
                biggest = 0;
                printf("  sheet %.0fx%.0f kind %d words %d: board %.2f, the sheet allows %.2f\n",
                       D[di].w, D[di].h, K[ki].kind, K[ki].words, o.board[2], want);
            }
            if (compact && o.words[2] < 32.f - 1e-3f) words_room = 0;
        }
        OK(biggest, "every screen's board is as large as the sheet allows, compact and expanded");
        OK(words_room, "and on every strip the words get a column of at least 32 points");

        UtttSheet c, e;
        uttt_sheet(&(UtttSheetIn){ .w = 375.f, .h = 340.f, .kind = UTTT_SHEET_PLAY }, &c);
        uttt_sheet(&(UtttSheetIn){ .w = 375.f, .h = 541.f, .kind = UTTT_SHEET_PLAY }, &e);
        printf("  sheet: SE compact board %.1f, expanded %.1f\n", c.board[2], e.board[2]);
        OK(c.t == 0.f && e.t == 1.f, "340 is the compact end and 541 the expanded one");
        OK(c.words_alpha == 0.f && c.band_alpha == 0.f && e.band_alpha == 1.f && e.words_alpha == 0.f,
           "a live strip hides the headline, the band shows it");

        /* THE SEND HINT'S CORNER (sheet 6: its ring over "You win"): with a
         * bubble in the field the strip's right column starts under the
         * hint, and without one it starts at the margin. */
        UtttSheet hn, h0;
        uttt_sheet(&(UtttSheetIn){ .w = 440.f, .h = 274.f, .kind = UTTT_SHEET_PLAY, .words = 1, .hint = 1 }, &hn);
        uttt_sheet(&(UtttSheetIn){ .w = 440.f, .h = 274.f, .kind = UTTT_SHEET_PLAY, .words = 1 }, &h0);
        OK(hn.words[1] >= 14.f + 29.f + 3.f + 9.f && h0.words[1] == h0.vpad
           && fabsf(hn.words[1] + hn.words[3] - h0.words[1] - h0.words[3]) < 1e-3f && hn.words_alpha == 1.f,
           "a staged bubble's hint never stands over the strip's verdict");
    }

    /* THE SLIDE: the whole travel at the start, nothing at the end, never
     * back up, and on the host's own curve (a critically damped spring on
     * the drawer's response) to within the tail it gives away. */
    {
        int mono = 1, host = 1;
        float prev = 1e9f;
        for (int t = 0; t <= UTTT_COLLAPSE_MS; t++) {
            float p = uttt_collapse_push(500.f, t);
            if (p > prev + 1e-4f) mono = 0;
            prev = p;
            double w = 2.0 * 3.14159265358979 / UTTT_DRAWER_RESPONSE_MS;
            double want = 500.0 * (1.0 + w * t) * exp(-w * t);
            if (fabs(p - want) > 1.5) host = 0;
        }
        OK(uttt_collapse_push(500.f, 0) == 500.f && uttt_collapse_push(500.f, UTTT_COLLAPSE_MS) == 0.f
           && fabsf(uttt_collapse_push(500.f, UTTT_COLLAPSE_MS - 1)) < .05f,
           "the slide starts at the whole travel and ends at nothing, with no step at the release");
        OK(mono, "and never pushes back up");
        OK(host, "and rides the host's spring to within a point and a half");
    }

    printf("uttt_anim: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

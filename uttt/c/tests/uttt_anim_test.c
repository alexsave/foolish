/* The motion plan (src/uttt_anim.c) and the two draws it composites:
 * docs/UI.html's timings, the order of the beats, and that the board a host
 * caches plus the stroke it draws over it is the whole board. */
#include "../src/uttt.h"
#include "../src/uttt_anim.h"
#include "../src/uttt_draw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void)
{
    UtttGame g; uttt_init(&g);
    UtttFrame f;
    float r4[4], r0[4], r9[4], a;

    /* nothing played: at rest */
    UtttMotion m = uttt_motion(&g, UTTT_CH_STAGE);
    uttt_motion_at(&m, 0, &f);
    OK(m.ch == UTTT_CH_STILL && !f.running && f.mark_t == 1.f, "an empty board does not move");

    /* X in the centre of the centre: from anywhere (9) to the centre (4) */
    uttt_play(&g, 4 * 9 + 4);
    m = uttt_motion(&g, UTTT_CH_STAGE);
    uttt_wash_rect(4, r4, &a); uttt_wash_rect(9, r9, NULL); uttt_wash_rect(0, r0, NULL);
    OK(m.mark == UTTT_X && m.ink_ms == 260, "an X inks in 260 ms");
    OK(m.from == 9 && m.to == 4, "the wash goes from anywhere to the block it sends to");
    OK(m.wash_at == 260 && m.wash_ms == 340, "the wash moves after the ink lands, for 340 ms");
    OK(m.pulse_at == 560, "the pulse is 300 ms after the ink lands");
    OK(m.end_ms == 560 + 2 * 620, "two rings, then rest");

    uttt_motion_at(&m, 0, &f);
    OK(f.mark_t == 0.f && !f.landed && f.running, "at 0 nothing is drawn yet");
    OK(same_rect(f.wash, r9), "and the wash is still where it was");
    float prev = 0.f; int mono = 1;
    for (int t = 0; t <= 260; t += 4) {
        uttt_motion_at(&m, t, &f);
        if (f.mark_t + 1e-6f < prev) mono = 0;
        prev = f.mark_t;
        if (t < 260 && !same_rect(f.wash, r9)) mono = 0;
    }
    OK(mono, "the ink only moves forward, and the wash waits for it");
    uttt_motion_at(&m, 130, &f);
    OK(f.mark_t > .5f && f.mark_t < 1.f, "halfway through, the ease-out curve is past half");
    uttt_motion_at(&m, 259, &f);
    OK(!f.landed, "not landed a millisecond early");
    uttt_motion_at(&m, 260, &f);
    OK(f.landed && f.mark_t == 1.f, "landed at 260");
    uttt_motion_at(&m, 260 + 170, &f);
    OK(!same_rect(f.wash, r9) && !same_rect(f.wash, r4), "mid-travel the rect is between");
    OK(f.wash[2] < r9[2] && f.wash[2] > r4[2], "and shrinking from the sheet to the block");
    uttt_motion_at(&m, 600, &f);
    OK(same_rect(f.wash, r4) && f.wash_rgba == uttt_wash_rgba(a), "arrived at 600");
    OK(f.settled, "settled once the wash arrives");
    uttt_motion_at(&m, 599, &f);
    OK(!f.settled && f.landed, "not settled while the wash travels");
    uttt_motion_at(&m, 559, &f);
    OK(f.pulse_rgba == 0, "no ring before 560");
    uttt_motion_at(&m, 600, &f);
    OK((f.pulse_rgba & 0xff) > 0 && f.pulse_spread > 0.f && same_rect(f.pulse, r4),
       "a ring round the destination at 600");
    uttt_motion_at(&m, 560 + 620 + 40, &f);
    OK((f.pulse_rgba & 0xff) > 0, "and a second one");
    uttt_motion_at(&m, m.end_ms - 1, &f);
    OK(f.running, "still running a millisecond before the end");
    uttt_motion_at(&m, m.end_ms, &f);
    OK(!f.running && f.pulse_rgba == 0 && same_rect(f.wash, r4) && f.mark_t == 1.f,
       "at the end: the resting board");

    /* O answers into the top-left: centre (4) to top-left (0) */
    uttt_play(&g, 4 * 9 + 0);
    m = uttt_motion(&g, UTTT_CH_REPLAY);
    OK(m.mark == UTTT_O && m.ink_ms == 340, "an O inks in 340 ms");
    OK(m.pulse_at == -1 && m.end_ms == 340 + 340, "my own replay does not pulse");
    uttt_motion_at(&m, 170, &f);
    OK(f.mark_t > .5f && f.mark_t < 1.f, "an O eases out too");
    m = uttt_motion(&g, UTTT_CH_THEIRS);
    OK(m.wash_ms == 420 && m.pulse_at == 640, "their move: the wash takes longer, and pulses");
    OK(m.from == 4 && m.to == 0, "from the centre to the top left");
    uttt_motion_at(&m, 340 + 420, &f);
    OK(same_rect(f.wash, r0), "and lands on it");
    m = uttt_motion(&g, UTTT_CH_ARRIVAL);
    OK(m.pulse_at == 640 && m.wash_ms == 420, "an arrival is their move too");
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

    /* THE DRAWER: the layout height never steps, and a finger is followed */
    {
        UtttDrawer d = {0};
        int32_t mv;
        uttt_drawer_report(&d, 840.f, 0);
        OK(uttt_drawer_at(&d, 5, &mv) == 840.f && !mv, "the first height is laid out as handed");
        uttt_drawer_report(&d, 820.f, 16);
        OK(uttt_drawer_at(&d, 16, &mv) == 820.f && !mv, "a finger's small step is followed at once");
        /* an auto-collapse: one height, far away */
        uttt_drawer_report(&d, 289.f, 1000);
        OK(uttt_drawer_at(&d, 1000, &mv) == 820.f && mv, "a far height does not step the layout");
        OK(uttt_drawer_at(&d, 1000 + UTTT_DRAWER_LEAD_MS, NULL) == 820.f, "it waits out the lead");
        float prev = 820.f, step = 0.f;
        int mono = 1;
        for (int32_t t = 1000; t <= 2200; t += 4) {
            float h = uttt_drawer_at(&d, t, NULL);
            if (h > prev + 1e-3f || h < 289.f - 1e-3f) mono = 0;
            if (prev - h > step) step = prev - h;
            prev = h;
        }
        printf("  drawer: largest 4 ms step of a 531 pt collapse %.1f pt\n", step);
        OK(mono, "from rest it runs one way and never overshoots");
        /* the peak of a critically damped run is x0 w / e: 3.63 pt/ms here */
        OK(step < 15.f, "and no 4 ms of it moves more than the spring's peak, 15 points");
        OK(uttt_drawer_at(&d, 1000 + UTTT_DRAWER_LEAD_MS + 3 * UTTT_DRAWER_RESPONSE_MS, &mv) == 289.f && !mv,
           "three responses in it is at rest on the target");
        /* a release: sparse heights while it is still moving */
        UtttDrawer e = {0};
        uttt_drawer_report(&e, 516.f, 0);
        uttt_drawer_report(&e, 334.f, 0);
        float a = uttt_drawer_at(&e, 200, NULL), a0 = uttt_drawer_at(&e, 196, NULL);
        uttt_drawer_report(&e, 289.f, 200);
        float b = uttt_drawer_at(&e, 200, NULL), b1 = uttt_drawer_at(&e, 204, NULL);
        OK(fabsf(a - b) < 1e-3f, "a new height mid-spring keeps the position");
        OK(fabsf((a - a0) - (b1 - b)) < .5f, "and the velocity");
        uttt_drawer_report(&e, 300.f, 210);
        OK(fabsf(uttt_drawer_at(&e, 210, NULL) - uttt_drawer_at(&e, 209, NULL)) < 6.f,
           "a small height mid-spring re-aims it rather than jumping to it");
    }

    /* ONE LAYOUT, EVERY SCREEN: the board's centre is the sheet's centre at
     * every height, its side never steps as the drawer moves, and it stays
     * clear of the words and on the sheet (docs/UI.html "What holds which
     * edge": the board holds the centre and is the only thing that resizes). */
    {
        static const float W[] = { 375.f, 393.f, 440.f };
        static const struct { int kind; float ww, wh; } K[] = {
            { UTTT_SHEET_PLAY, 0, 0 },        /* a live game                  */
            { UTTT_SHEET_PLAY, 82, 50 },      /* the end: "You win / Diagonal" */
            { UTTT_SHEET_WATCH, 0, 28 },      /* the spectator's line         */
            { UTTT_SHEET_WAIT, 168, 50 },     /* the waiting words            */
        };
        float reach = .135f * UTTT_REACH;
        int centred = 1, clear = 1, onsheet = 1, grows = 1;
        float worst = 0.f;
        for (int wi = 0; wi < 3; wi++)
        for (int ki = 0; ki < 4; ki++) {
            UtttSheetIn in = { .w = W[wi], .words_w = K[ki].ww, .words_h = K[ki].wh, .kind = K[ki].kind };
            float prev = -1.f;
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
                /* clear of the strip's words: beside them or under them */
                if (o.t == 0.f && in.words_h > 0.f) {
                    float bx = in.words_w > 0.f ? in.words_w : in.w - 2.f * o.hpad;
                    int beside = o.board[0] + s * (1.f + reach) <= in.w - o.hpad - bx + 1e-3f;
                    int under  = o.board[1] >= o.vpad + in.words_h - 1e-3f;
                    if (!beside && !under) clear = 0;
                }
            }
        }
        printf("  sheet: largest side step per quarter point of drawer %.3f pt\n", worst);
        OK(centred, "every screen's board is centred on the sheet at every height");
        OK(worst < .6f, "and its side never steps as the drawer moves");
        OK(grows, "on the strip a taller drawer never gives a smaller board");
        OK(onsheet, "its lines stay on the sheet");
        OK(clear, "and it clears the strip's words, beside or under them");
        UtttSheet c, e;
        uttt_sheet(&(UtttSheetIn){ .w = 375.f, .h = 340.f, .kind = UTTT_SHEET_PLAY }, &c);
        uttt_sheet(&(UtttSheetIn){ .w = 375.f, .h = 541.f, .kind = UTTT_SHEET_PLAY }, &e);
        printf("  sheet: SE compact board %.1f, expanded %.1f\n", c.board[2], e.board[2]);
        OK(c.t == 0.f && e.t == 1.f, "340 is the compact end and 541 the expanded one");
        OK(c.words_alpha == 0.f && e.words_alpha == 1.f, "a live strip hides the headline, the bar shows it");
    }

    printf("uttt_anim: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

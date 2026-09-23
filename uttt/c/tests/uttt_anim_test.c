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

    printf("uttt_anim: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

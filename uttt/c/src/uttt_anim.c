#include "uttt_anim.h"
#include "uttt_draw.h"
#include <math.h>

#define BL (1.f / 3.f)
#define WASH_RGB 0xd6a83600u        /* rgba(214,168,54) - UI.html's wash */

int uttt_wash_rect(int block, float r[4], float *alpha)
{
    if (block == 9) {
        r[0] = .01f; r[1] = .01f; r[2] = 1.f - .02f; r[3] = 1.f - .02f;
        if (alpha) *alpha = .17f;
        return 1;
    }
    if (block < 0 || block > 8) {
        r[0] = r[1] = r[2] = r[3] = 0.f;
        if (alpha) *alpha = 0.f;
        return 0;
    }
    r[0] = (block % 3) * BL + .012f; r[1] = (block / 3) * BL + .012f;
    r[2] = BL - .024f;               r[3] = BL - .024f;
    if (alpha) *alpha = .30f;
    return 1;
}

uint32_t uttt_wash_rgba(float a)
{
    if (a < 0.f) a = 0.f;
    if (a > 1.f) a = 1.f;
    return WASH_RGB | (uint32_t)lroundf(a * 255.f);
}

UtttMotion uttt_motion(const UtttGame *g, int ch)
{
    UtttMotion m = { 0 };
    m.ch = ch; m.mv = -1; m.pulse_at = -1;
    m.to = m.from = uttt_active(g);
    if (ch == UTTT_CH_STILL || g->n_plies == 0) { m.ch = UTTT_CH_STILL; return m; }

    UtttGame before = *g;
    uttt_undo(&before);
    m.mv     = g->move[g->n_plies - 1];
    m.mark   = uttt_cell(g, m.mv);
    m.from   = uttt_active(&before);
    m.ink_ms = m.mark == UTTT_O ? UTTT_MS_INK_O : UTTT_MS_INK_X;

    /* THE WASH MOVES AFTER THE INK LANDS, never with it: two facts - what I
     * played, then where you go - and shown together they teach nothing. */
    m.wash_at = m.ink_ms;
    m.wash_ms = (ch == UTTT_CH_STAGE || ch == UTTT_CH_REPLAY)
              ? UTTT_MS_WASH_MINE : UTTT_MS_WASH_THEIRS;
    m.end_ms  = m.wash_at + m.wash_ms;

    /* THE PULSE is the only thing that tells you where you have been sent,
     * so it plays on every channel but my own replay, which "should not
     * surprise me". It rings round the destination - nothing at game end. */
    if (ch != UTTT_CH_REPLAY && m.to >= 0) {
        m.pulse_at = m.ink_ms + UTTT_MS_PULSE_AT;
        int e = m.pulse_at + UTTT_PULSES * UTTT_MS_PULSE;
        if (e > m.end_ms) m.end_ms = e;
    }
    return m;
}

/* CSS cubic-bezier(x1, y1, x2, y2) at x, solved for the curve's parameter
 * by Newton then bisection, as a browser does. */
static float bezier(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0.f) return 0.f;
    if (x >= 1.f) return 1.f;
    const float cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
    const float cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    float u = x;
    for (int i = 0; i < 8; i++) {
        float fx = ((ax * u + bx) * u + cx) * u - x;
        float d  = (3 * ax * u + 2 * bx) * u + cx;
        if (fabsf(fx) < 1e-6f) goto done;
        if (fabsf(d) < 1e-6f) break;
        u -= fx / d;
    }
    {
        float lo = 0.f, hi = 1.f; u = x;
        for (int i = 0; i < 40; i++) {
            float fx = ((ax * u + bx) * u + cx) * u;
            if (fabsf(fx - x) < 1e-6f) break;
            if (fx < x) lo = u; else hi = u;
            u = (lo + hi) * .5f;
        }
    }
done:
    return ((ay * u + by) * u + cy) * u;
}

static float smooth(float t)
{
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t * t * (3.f - 2.f * t);
}

void uttt_motion_at(const UtttMotion *m, int32_t now, UtttFrame *f)
{
    *f = (UtttFrame){ 0 };
    int still = m->ch == UTTT_CH_STILL || now >= m->end_ms;

    /* the ink, on UI.html's own curves: .32,.72,.4,1 for an X and
     * .3,.66,.36,1 for the one slower stroke of an O */
    if (m->mv < 0 || still || now >= m->ink_ms) f->mark_t = 1.f;
    else {
        float x = (float)now / (float)m->ink_ms;
        f->mark_t = m->mark == UTTT_O ? bezier(.3f, .66f, .36f, 1.f, x)
                                      : bezier(.32f, .72f, .4f, 1.f, x);
    }
    f->landed = f->mark_t >= 1.f;

    /* ONE RECT, TRAVELLING. From block to block it slides and resizes; to
     * "anywhere" the same interpolation grows it to the sheet, so being
     * freed is the same gesture as being sent, only bigger. */
    float a[4], b[4], aa, ba;
    int has_to = uttt_wash_rect(m->to, b, &ba);
    int has_from = uttt_wash_rect(m->from, a, &aa);
    float p = still ? 1.f : smooth((float)(now - m->wash_at) / (float)m->wash_ms);
    if (!has_to) {
        /* the game ended: the wash leaves with the ink, it does not travel */
        if (has_from && p < 1.f) {
            for (int i = 0; i < 4; i++) f->wash[i] = a[i];
            f->wash_rgba = uttt_wash_rgba(aa * (1.f - p));
        }
    } else {
        if (!has_from) {                 /* no origin: grow from its centre */
            a[0] = b[0] + b[2] / 2; a[1] = b[1] + b[3] / 2; a[2] = a[3] = 0;
            aa = ba;
        }
        for (int i = 0; i < 4; i++) f->wash[i] = a[i] + (b[i] - a[i]) * p;
        f->wash_rgba = uttt_wash_rgba(aa + (ba - aa) * p);
    }

    /* the ring: out to 13 points and gone by 70%, then back to nothing,
     * ease-out on each leg - UI.html's cellpulse keyframes */
    if (!still && m->pulse_at >= 0 && now >= m->pulse_at && has_to) {
        int32_t k = now - m->pulse_at;
        if (k < UTTT_PULSES * UTTT_MS_PULSE) {
            float q = (float)(k % UTTT_MS_PULSE) / (float)UTTT_MS_PULSE;
            float s, al;
            if (q < .7f) {
                float e = bezier(0.f, 0.f, .58f, 1.f, q / .7f);
                s = UTTT_PULSE_REACH * e; al = UTTT_PULSE_ALPHA * (1.f - e);
            } else {
                float e = bezier(0.f, 0.f, .58f, 1.f, (q - .7f) / .3f);
                s = UTTT_PULSE_REACH * (1.f - e); al = 0.f;
            }
            if (al > 0.f) {
                for (int i = 0; i < 4; i++) f->pulse[i] = b[i];
                f->pulse_spread = s;
                f->pulse_rgba = UTTT_PULSE_RGB | (uint32_t)lroundf(al * 255.f);
            }
        }
    }
    f->settled = f->landed && (still || now >= m->wash_at + m->wash_ms);
    f->running = !still;
}

/* ---- the drawer's layout height (uttt_anim.h) ---------------------------- */

/* A critically damped spring, `t` ms after it started `x0` from its target
 * moving at `v0`: x(t) = (x0 + (v0 + w x0) t) e^(-w t). */
static void drawer_spring(const UtttDrawer *d, int32_t now_ms, float *x, float *v)
{
    double t = (double)(now_ms - d->t0);
    if (t < 0) { *x = d->from; *v = 0.f; return; }       /* the lead */
    double w = 2.0 * 3.14159265358979 / UTTT_DRAWER_RESPONSE_MS;
    double b = d->vel + w * d->from, e = exp(-w * t);
    *x = (float)((d->from + b * t) * e);
    *v = (float)((d->vel - w * b * t) * e);
}

/* The spring is over once it cannot move a hundredth of a point again:
 * three responses in, (1 + w t) e^(-w t) is below 1e-7. */
static int drawer_done(const UtttDrawer *d, int32_t now_ms)
{
    return !d->moving || now_ms - d->t0 >= 3 * UTTT_DRAWER_RESPONSE_MS;
}

void uttt_drawer_report(UtttDrawer *d, float h, int32_t now_ms)
{
    if (!d->seen) {
        *d = (UtttDrawer){ .target = h, .seen = 1 };
        return;
    }
    if (drawer_done(d, now_ms)) {
        if (fabsf(h - d->target) <= UTTT_DRAWER_FOLLOW_PT) {
            *d = (UtttDrawer){ .target = h, .seen = 1 };  /* a finger: follow */
            return;
        }
        d->from = d->target - h;
        d->vel = 0.f;
        d->t0 = now_ms + UTTT_DRAWER_LEAD_MS;
    } else {
        float x, v;
        drawer_spring(d, now_ms, &x, &v);
        d->from = d->target + x - h;
        d->vel = v;
        d->t0 = now_ms;
    }
    d->target = h;
    d->moving = 1;
}

float uttt_drawer_at(const UtttDrawer *d, int32_t now_ms, int32_t *moving)
{
    if (drawer_done(d, now_ms)) {
        if (moving) *moving = 0;
        return d->target;
    }
    float x, v;
    drawer_spring(d, now_ms, &x, &v);
    if (moving) *moving = 1;
    return d->target + x;
}

/* ---- one layout for every screen (uttt_anim.h UtttSheet) ---------------- */

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* The numbers the two ends are drawn with. The margin is the grab handle's
 * above and below (Messages draws its handle over the top 13 points), and
 * the sides'; the columns are one either side on the strip so the board sits
 * in the middle of the SHEET and not of what is left over ("you are" in the
 * left, the rulebook in the right); the header band is "you are" over a
 * 46-point mark. */
#define SHEET_MARGIN     13.f
#define SHEET_COLUMN     38.f
#define SHEET_GUTTER      3.f
#define SHEET_DOOR_LO    38.f
#define SHEET_DOOR_HI    54.f     /* UtttRulebookButton.expandedSide          */
#define SHEET_DOOR_GAP    6.f
#define SHEET_BAR        72.f
#define SHEET_WORDS_AIR   6.f     /* between a box of words and the ink       */

void uttt_sheet(const UtttSheetIn *in, UtttSheet *o)
{
    float x = clampf((in->h - UTTT_SHEET_LO) / (UTTT_SHEET_HI - UTTT_SHEET_LO), 0.f, 1.f);
    float t = x * x * (3.f - 2.f * x);              /* smoothstep: the ends settle */
    float reach = .135f * UTTT_REACH;               /* UI.html's 5% overshoot     */
    int seat = in->kind == UTTT_SHEET_PLAY, doors = in->kind != UTTT_SHEET_WAIT;

    *o = (UtttSheet){ .t = t };
    o->hpad = SHEET_MARGIN;
    o->vpad = SHEET_MARGIN;
    /* EVERY SCREEN KEEPS ITS COLUMNS on the strip, words or doors: a board
     * that took the whole width would leave its words no room at all (a
     * 340 drawer on a 375 phone left the waiting words 0 points). */
    o->col  = lerpf(SHEET_COLUMN, 0.f, t);
    o->door = lerpf(SHEET_DOOR_LO, SHEET_DOOR_HI, t);
    o->foot = doors ? lerpf(0.f, SHEET_DOOR_HI + SHEET_DOOR_GAP, t) : 0.f;
    o->icon = lerpf(34.f, 46.f, t);
    o->icon_lead = lerpf(3.f, 4.f, t);
    o->icon_top  = lerpf(4.f, 0.f, t);
    o->door_alpha = doors ? clampf((t - .5f) * 2.f, 0.f, 1.f) : 0.f;
    /* The header band, which the door row mirrors at the bottom so the
     * centre stays the centre: nothing on the strip, SHEET_BAR open. */
    o->bar = lerpf(0.f, SHEET_BAR, t);
    /* A live seat's strip says nothing - the wash says it - and its headline
     * fades in with the band; an ended seat's verdict, the waiting words and
     * the spectator's line are shown at every height. */
    o->words_alpha = (seat && !in->words) ? t : 1.f;

    /* THE SIDE: the width less the columns, leaving the main lines room to
     * run 5% past the board; the height less the margins and the bands.
     * Nothing else. */
    float gut  = lerpf(0.f, SHEET_GUTTER, t);
    float wide = (in->w - 2.f * (SHEET_MARGIN + gut) - 2.f * o->col) / (1.f + 2.f * reach);
    float tall = in->h - 2.f * o->vpad - 2.f * fmaxf(o->bar, o->foot);
    float side = fmaxf(fminf(wide, tall), 0.f);

    /* THE WORDS. Beside the ink on the strip - the play screen's verdict in
     * the right column over the rulebook, where the expanded band puts its
     * headline; the waiting words and the spectator's line in the left - and
     * in the band across the top once the sheet is half open. */
    float ink_l = (in->w - side * (1.f + 2.f * reach)) * .5f;
    float ink_r = in->w - ink_l;
    if (t < .5f) {
        o->words_side = 1;
        o->words[1] = o->vpad;
        if (seat) {
            o->words[0] = ink_r + SHEET_WORDS_AIR;
            o->words[2] = in->w - SHEET_MARGIN - o->words[0];
            o->words[3] = in->h - 2.f * o->vpad - o->door - SHEET_DOOR_GAP;
        } else {
            o->words[0] = SHEET_MARGIN;
            o->words[2] = ink_l - SHEET_WORDS_AIR - SHEET_MARGIN;
            o->words[3] = in->h - 2.f * o->vpad;
        }
        o->words[2] = fmaxf(o->words[2], 0.f);
    } else {
        o->words_side = 0;
        o->words[0] = SHEET_MARGIN;
        o->words[1] = o->vpad;
        o->words[2] = in->w - 2.f * SHEET_MARGIN;
        o->words[3] = o->bar;
    }

    o->board[0] = (in->w - side) * .5f;
    o->board[1] = (in->h - side) * .5f;
    o->board[2] = side;
}

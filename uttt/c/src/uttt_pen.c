#include "uttt_pen.h"
#include <math.h>
#include <string.h>

/* ======================= rough.js, transcribed ========================= */

/* Its randomiser is a Lehmer generator with the MINSTD multiplier, and the
 * seed advances in place - which is why the ORDER of the random draws below
 * matters as much as the arithmetic. Reordering two of them would give a
 * different, equally plausible-looking sheet, and the document would stop
 * matching the app. */
static float rnd(UtttRough *o)
{
    o->seed = (int32_t)((uint32_t)o->seed * 48271u);
    return (float)((double)(o->seed & 0x7fffffff) / 2147483648.0);
}
static float ofs(float mn, float mx, UtttRough *o, float gain)
{
    return o->roughness * gain * (rnd(o) * (mx - mn) + mn);
}
static float ofs1(float x, UtttRough *o, float gain)
{
    return ofs(-x, x, o, gain);
}

UtttRough uttt_rough_default(int32_t seed)
{
    UtttRough o;
    o.roughness = 1.f; o.bowing = 1.f; o.max_offset = 2.f;
    o.curve_tightness = 0.f; o.curve_fitting = .95f;
    o.curve_step_count = 9; o.single = 0;
    o.seg_line = 22; o.seg_curve = 14;
    o.seed = seed ? seed : 1;
    return o;
}

/* ------------------------------------------------------------- flattening */
static void push(UtttPt *pts, int cap, int *n, float x, float y)
{
    if (*n < cap) { pts[*n].x = x; pts[*n].y = y; (*n)++; }
}
static void bez(UtttPt *pts, int cap, int *n, float x0, float y0,
                float x1, float y1, float x2, float y2, float x3, float y3,
                int steps)
{
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps, u = 1 - t;
        push(pts, cap, n,
             u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3,
             u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3);
    }
}

/* rough.js `_line`: both endpoints jittered, two control points at the
 * diverge point and twice it, and a gain that straightens long lines. */
static void rline(UtttRough *o, float x1, float y1, float x2, float y2,
                  int overlay, UtttPt *pts, int cap, int *n, UtttSpan *sp)
{
    float lsq = (x1-x2)*(x1-x2) + (y1-y2)*(y1-y2);
    float len = sqrtf(lsq);
    float gain = len < 200.f ? 1.f
               : len > 500.f ? .4f
               : -0.0016668f * len + 1.233334f;

    float off = o->max_offset;
    if (off * off * 100.f > lsq) off = len / 10.f;
    float half = off / 2.f;
    float diverge = .2f + .2f * rnd(o);

    float mdx = o->bowing * o->max_offset * (y2 - y1) / 200.f;
    float mdy = o->bowing * o->max_offset * (x1 - x2) / 200.f;
    mdx = ofs1(mdx, o, gain);
    mdy = ofs1(mdy, o, gain);

    float r = overlay ? half : off;
    #define J() ofs1(r, o, gain)
    float sx = x1 + J(), sy = y1 + J();
    sp->first = *n;
    push(pts, cap, n, sx, sy);
    float c1x = mdx + x1 + (x2-x1)*diverge + J();
    float c1y = mdy + y1 + (y2-y1)*diverge + J();
    float c2x = mdx + x1 + 2*(x2-x1)*diverge + J();
    float c2y = mdy + y1 + 2*(y2-y1)*diverge + J();
    float ex  = x2 + J(), ey = y2 + J();
    #undef J
    bez(pts, cap, n, sx, sy, c1x, c1y, c2x, c2y, ex, ey, o->seg_line);
    sp->n = *n - sp->first;
}

int uttt_rough_line(UtttRough *o, float x1, float y1, float x2, float y2,
                    UtttPt *pts, int cap, int *n_pts,
                    UtttSpan *out, int out_cap)
{
    int k = 0;
    if (out_cap > 0) { rline(o, x1, y1, x2, y2, 0, pts, cap, n_pts, &out[k]); k++; }
    if (!o->single && out_cap > 1) {
        rline(o, x1, y1, x2, y2, 1, pts, cap, n_pts, &out[k]); k++;
    }
    return k;
}

/* rough.js `curve`: a Catmull-Rom through the sampled points, emitted as
 * cubics. The first and last points are control only, which is why its
 * ellipse generator pads the ring at both ends. */
static void rcurve(UtttRough *o, const UtttPt *p, int np,
                   UtttPt *pts, int cap, int *n, UtttSpan *sp)
{
    sp->first = *n;
    if (np > 3) {
        float h = 1.f - o->curve_tightness;
        push(pts, cap, n, p[1].x, p[1].y);
        for (int i = 1; i + 2 < np; i++) {
            float b1x = p[i].x + (h*p[i+1].x - h*p[i-1].x) / 6.f;
            float b1y = p[i].y + (h*p[i+1].y - h*p[i-1].y) / 6.f;
            float b2x = p[i+1].x + (h*p[i].x - h*p[i+2].x) / 6.f;
            float b2y = p[i+1].y + (h*p[i].y - h*p[i+2].y) / 6.f;
            bez(pts, cap, n, p[i].x, p[i].y, b1x, b1y, b2x, b2y,
                p[i+1].x, p[i+1].y, o->seg_curve);
        }
    } else if (np == 3) {
        push(pts, cap, n, p[1].x, p[1].y);
        push(pts, cap, n, p[2].x, p[2].y);
    } else if (np == 2) {
        push(pts, cap, n, p[0].x, p[0].y);
        push(pts, cap, n, p[1].x, p[1].y);
    }
    sp->n = *n - sp->first;
}

/* rough.js `computeEllipsePoints`, the roughness>0 branch. */
static int ell_points(UtttRough *o, float cx, float cy, float rx, float ry,
                      float inc, float off, float overlap, UtttPt *buf, int cap)
{
    int n = 0;
    float start = ofs1(.5f, o, 1.f) - (float)M_PI / 2.f;
    #define P(ang, sc) do { if (n < cap) { \
        buf[n].x = ofs1(off,o,1.f) + cx + (sc) * rx * cosf(ang); \
        buf[n].y = ofs1(off,o,1.f) + cy + (sc) * ry * sinf(ang); n++; } } while (0)
    P(start - inc, .9f);
    float end = 2.f * (float)M_PI + start - .01f;
    for (float a = start; a < end; a += inc) P(a, 1.f);
    P(start + 2.f * (float)M_PI + overlap * .5f, 1.f);
    P(start + overlap, .98f);
    P(start + overlap * .5f, .9f);
    #undef P
    return n;
}

int uttt_rough_ellipse(UtttRough *o, float cx, float cy, float w, float h,
                       UtttPt *pts, int cap, int *n_pts,
                       UtttSpan *out, int out_cap)
{
    /* ellipseParams */
    float ps = sqrtf(2.f * (float)M_PI *
                     sqrtf((w*w/4.f + h*h/4.f) / 2.f));
    float sc = (float)o->curve_step_count;
    int   steps = (int)ceilf(fmaxf(sc, sc / sqrtf(200.f) * ps));
    float inc = 2.f * (float)M_PI / steps;
    float rx = fabsf(w / 2.f), ry = fabsf(h / 2.f);
    float fit = 1.f - o->curve_fitting;
    rx += ofs1(rx * fit, o, 1.f);
    ry += ofs1(ry * fit, o, 1.f);

    UtttPt buf[256];
    int k = 0;
    float ov = inc * ofs(.1f, ofs(.4f, 1.f, o, 1.f), o, 1.f);
    int np = ell_points(o, cx, cy, rx, ry, inc, 1.f, ov, buf, 256);
    if (out_cap > 0) { rcurve(o, buf, np, pts, cap, n_pts, &out[k]); k++; }
    if (!o->single && o->roughness != 0.f && out_cap > 1) {
        np = ell_points(o, cx, cy, rx, ry, inc, 1.5f, 0.f, buf, 256);
        rcurve(o, buf, np, pts, cap, n_pts, &out[k]); k++;
    }
    return k;
}

/* ============================== the pen ================================ */

static uint32_t h2(int x, int y, int s)
{
    uint32_t h = (uint32_t)x * 374761393u ^ (uint32_t)y * 668265263u
               ^ (uint32_t)s * 362437u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float smooth(float t) { return t * t * (3.f - 2.f * t); }

float uttt_grain(float x, float y, float fx, float fy, int seed)
{
    float gx = x * fx, gy = y * fy;
    int xi = (int)floorf(gx), yi = (int)floorf(gy);
    float u = smooth(gx - xi), v = smooth(gy - yi);
    float a = h2(xi, yi, seed) / 4294967295.f;
    float b = h2(xi + 1, yi, seed) / 4294967295.f;
    float c = h2(xi, yi + 1, seed) / 4294967295.f;
    float d = h2(xi + 1, yi + 1, seed) / 4294967295.f;
    return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}

UtttPen uttt_pen_92(void)
{
    UtttPen p;
    p.w = 2.7f; p.a = .8f;
    p.vel = .5f; p.lift = .45f; p.liftp = .6f;
    p.press = 0; p.grain = .45f; p.dir = 0; p.chat = 0;
    p.agrain = .4f; p.gfx = .16f; p.gfy = .006f;
    p.ink = 0x25376bffu;
    return p;
}

static float pen_w(const UtttPen *p, float x, float y, float t)
{
    float k = p->w;
    if (p->vel)   k *= 1.f + p->vel * (.5f - sinf((float)M_PI * t));
    if (p->lift)  k *= (1.f - p->lift)
                     + p->lift * powf(sinf((float)M_PI * t), p->liftp);
    if (p->press) k *= 1.f + p->press * (.5f - t);
    if (p->grain) k *= (1.f - p->grain)
                     + 2.f * p->grain * uttt_grain(x, y, p->gfx, p->gfy, 3);
    /* relative to the pen, not to a coordinate system this file
     * does not get to assume */
    float floor_w = p->w * .08f;
    return k < floor_w ? floor_w : k;
}
static float pen_a(const UtttPen *p, float x, float y)
{
    float k = p->a;
    if (p->agrain) k *= (1.f - p->agrain)
                      + 2.f * p->agrain * uttt_grain(x, y, p->gfx, p->gfy, 3);
    return k < .04f ? .04f : (k > 1.f ? 1.f : k);
}

void uttt_dl_init(UtttDL *d, UtttPt *pt, int cap_pt,
                  UtttPoly *poly, int cap_poly)
{
    d->pt = pt; d->cap_pt = cap_pt; d->n_pt = 0;
    d->poly = poly; d->cap_poly = cap_poly; d->n_poly = 0;
}
void uttt_dl_reset(UtttDL *d) { d->n_pt = 0; d->n_poly = 0; }

static void quad(UtttDL *d, UtttPt a, UtttPt b, UtttPt c, UtttPt e, uint32_t rgba)
{
    if (d->n_poly >= d->cap_poly || d->n_pt + 4 > d->cap_pt) return;
    UtttPoly *p = &d->poly[d->n_poly++];
    p->first = d->n_pt; p->n = 4; p->rgba = rgba;
    d->pt[d->n_pt++] = a; d->pt[d->n_pt++] = b;
    d->pt[d->n_pt++] = c; d->pt[d->n_pt++] = e;
}
static void disc(UtttDL *d, float x, float y, float r, uint32_t rgba)
{
    enum { SEG = 9 };
    if (d->n_poly >= d->cap_poly || d->n_pt + SEG > d->cap_pt) return;
    UtttPoly *p = &d->poly[d->n_poly++];
    p->first = d->n_pt; p->n = SEG; p->rgba = rgba;
    for (int i = 0; i < SEG; i++) {
        float a = 2.f * (float)M_PI * i / SEG;
        d->pt[d->n_pt].x = x + cosf(a) * r;
        d->pt[d->n_pt].y = y + sinf(a) * r;
        d->n_pt++;
    }
}

void uttt_ink(UtttDL *d, const UtttPt *pts, int n, const UtttPen *p)
{
    if (n < 2) return;
    for (int i = 0; i < n - 1; i++) {
        float t0 = (float)i / (n - 1), t1 = (float)(i + 1) / (n - 1);
        const UtttPt *q0 = &pts[i], *q1 = &pts[i + 1];
        float dx = q1->x - q0->x, dy = q1->y - q0->y;
        float L = sqrtf(dx*dx + dy*dy);
        if (L < 1e-5f) continue;
        dx /= L; dy /= L;
        float w0 = pen_w(p, q0->x, q0->y, t0) / 2.f;
        float w1 = pen_w(p, q1->x, q1->y, t1) / 2.f;
        float a  = (pen_a(p, q0->x, q0->y) + pen_a(p, q1->x, q1->y)) / 2.f;
        uint32_t rgba = (p->ink & 0xffffff00u) | (uint32_t)(a * 255.f);
        UtttPt A = { q0->x - dy*w0, q0->y + dx*w0 };
        UtttPt B = { q0->x + dy*w0, q0->y - dx*w0 };
        UtttPt C = { q1->x + dy*w1, q1->y - dx*w1 };
        UtttPt E = { q1->x - dy*w1, q1->y + dx*w1 };
        quad(d, A, B, C, E, rgba);
        disc(d, q1->x, q1->y, w1, rgba);
    }
}

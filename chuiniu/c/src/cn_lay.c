/* Chui Niu - the table screen's layout. See cn_lay.h.
 *
 * The study's screen(), ringSeats and rowSeats (chuiniu/docs/UI.html), in
 * doubles, in the study's order; tests/cn_lay_test.c pins them to the numbers
 * headless Chromium computed from the study itself (tools/study_layout.txt). */
#include "cn_lay.h"
#include <string.h>

#define MAXS CN_LAY_SEATS

static double dmax(double a, double b) { return a > b ? a : b; }
static double dmin(double a, double b) { return a < b ? a : b; }
static double dabs(double a) { return a < 0 ? -a : a; }
static double hyp(double a, double b) { return cn_m_sqrt(a * a + b * b); }

/* a die's station in the ring of five under a cup: 72 degrees apart from the
 * top, each a little off its station and turned a little, by the seat */
static void die_spot(int seat, int k, double cx, double cy, double ring, double *x, double *y, double *yaw)
{
    const int32_t s = 977 + seat;
    const double a = -CN_PI / 2 + k * CN_PI * 2 / 5 + (cn_geom_hash(k, 71, s) - .5) * .18;
    const double rr = ring * (1 + (cn_geom_hash(k, 72, s) - .5) * .1);
    *x = cx + rr * cn_m_cos(a);
    *y = cy + rr * cn_m_sin(a);
    *yaw = (cn_geom_hash(k, 73, s) - .5) * .5;
}

/* the ring of five a die of side d sits in: turned dice never touch (centre spacing 1.7 of a side) */
static double ring_of(double d) { return 1.7 * d / (2 * cn_m_sin(CN_PI / 5)); }

/* ---- what the eye sees of a cup, on the glass ------------------------------------- */

/* the drawer's points a picture reaches: its top, its left, its right and its foot */
typedef struct { double y0, x0, x1, y1; } Reach;
static Reach reach_none(void) { Reach r = { 1e30, 1e30, -1e30, -1e30 }; return r; }
static int reach_inside(const Reach *r, double w) { return r->y0 >= CN_LAY_EDGE && r->x0 >= CN_LAY_EDGE && r->x1 <= w - CN_LAY_EDGE; }
/* a picture's box within g of a rectangle (x y w h, glass points) */
static int over_rect(const Reach *r, const float *rc, double g)
{
    return r->x0 < rc[0] + rc[2] + g && r->x1 > rc[0] - g && r->y0 < rc[1] + rc[3] + g && r->y1 > rc[1] - g;
}

/* A cup placed as o, as cn_geom_emit places its mesh: the mouth's rim and the
 * crown's (the mesh's own corners; the fillet and the inside lie within them),
 * through the eye, onto the board's place in the drawer, through the turn. */
static void cup_reach(const CnLay *L, const CnObj *o, Reach *r)
{
    const double R = o->R, rc = R * CN_CUP_RC, h = o->h, ta = o->tilt_angle;
    const double ct = cn_m_cos(ta), st = cn_m_sin(ta), cy = cn_m_cos(o->yaw), sy = cn_m_sin(o->yaw);
    for (int i = 0; i < CN_CUP_SEGS; i++) {
        const double ca = L->seg_c[i], sa = L->seg_s[i];
        for (int top = 0; top < 2; top++) {
            const double rr = top ? rc : R, p[3] = { rr * ca, rr * sa, top ? h : 0 };
            double q[3];
            if (o->has_rot) for (int k = 0; k < 3; k++) q[k] = o->rot[k] * p[0] + o->rot[3 + k] * p[1] + o->rot[6 + k] * p[2];
            else { q[0] = p[0] * cy - p[1] * sy; q[1] = p[0] * sy + p[1] * cy; q[2] = p[2]; }
            if (ta != 0) {
                const double y = q[1] - o->tilt_hinge_y, z = q[2];
                q[1] = y * ct - z * st + o->tilt_hinge_y - o->tilt_back; q[2] = y * st + z * ct + o->tilt_lift;
            }
            float px, py, gx, gy;
            cn_cam_project(&L->cam, (float)(q[0] + o->x), (float)(q[1] + o->y), (float)(q[2] + o->lift), &px, &py);
            cn_cam_map(&L->cam, L->board_x + px, L->board_y + py, &gx, &gy);
            if (gy < r->y0) r->y0 = gy;
            if (gy > r->y1) r->y1 = gy;
            if (gx < r->x0) r->x0 = gx;
            if (gx > r->x1) r->x1 = gx;
        }
    }
}

/* A CUP'S PICTURE AS A POLYGON: the convex hull of its two rims on the glass (a cone's
 * picture is the hull of its two ends), for the tests a box would answer wrongly at
 * the corners (a name under a round crown). */
#define HULL_MAX (2 * CN_CUP_SEGS)
typedef struct { int n; double x[HULL_MAX + 1], y[HULL_MAX + 1]; } Hull;
static void cup_points(const CnLay *L, const CnObj *o, double *X, double *Y)
{
    const double R = o->R, rc = R * CN_CUP_RC, h = o->h, ta = o->tilt_angle;
    const double ct = cn_m_cos(ta), st = cn_m_sin(ta), cy = cn_m_cos(o->yaw), sy = cn_m_sin(o->yaw);
    for (int i = 0; i < CN_CUP_SEGS; i++) {
        const double ca = L->seg_c[i], sa = L->seg_s[i];
        for (int top = 0; top < 2; top++) {
            const double rr = top ? rc : R, p[3] = { rr * ca, rr * sa, top ? h : 0 };
            double q[3];
            if (o->has_rot) for (int k = 0; k < 3; k++) q[k] = o->rot[k] * p[0] + o->rot[3 + k] * p[1] + o->rot[6 + k] * p[2];
            else { q[0] = p[0] * cy - p[1] * sy; q[1] = p[0] * sy + p[1] * cy; q[2] = p[2]; }
            if (ta != 0) {
                const double y = q[1] - o->tilt_hinge_y, z = q[2];
                q[1] = y * ct - z * st + o->tilt_hinge_y - o->tilt_back; q[2] = y * st + z * ct + o->tilt_lift;
            }
            float px, py, gx, gy;
            cn_cam_project(&L->cam, (float)(q[0] + o->x), (float)(q[1] + o->y), (float)(q[2] + o->lift), &px, &py);
            cn_cam_map(&L->cam, L->board_x + px, L->board_y + py, &gx, &gy);
            X[2 * i + top] = gx; Y[2 * i + top] = gy;
        }
    }
}
static double cross3(double ox, double oy, double ax, double ay, double bx, double by) { return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox); }
static void cup_hull(const CnLay *L, const CnObj *o, Hull *H)
{
    double X[HULL_MAX], Y[HULL_MAX];
    int idx[HULL_MAX];
    cup_points(L, o, X, Y);
    for (int i = 0; i < HULL_MAX; i++) idx[i] = i;
    for (int i = 1; i < HULL_MAX; i++) for (int j = i; j > 0 && (X[idx[j]] < X[idx[j - 1]] || (X[idx[j]] == X[idx[j - 1]] && Y[idx[j]] < Y[idx[j - 1]])); j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    int k = 0, st[2 * HULL_MAX];
    for (int i = 0; i < HULL_MAX; i++) {   /* the lower chain, then the upper (monotone chain) */
        while (k >= 2 && cross3(X[st[k - 2]], Y[st[k - 2]], X[st[k - 1]], Y[st[k - 1]], X[idx[i]], Y[idx[i]]) <= 0) k--;
        st[k++] = idx[i];
    }
    for (int i = HULL_MAX - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && cross3(X[st[k - 2]], Y[st[k - 2]], X[st[k - 1]], Y[st[k - 1]], X[idx[i]], Y[idx[i]]) <= 0) k--;
        st[k++] = idx[i];
    }
    H->n = k - 1;
    for (int i = 0; i < H->n; i++) { H->x[i] = X[st[i]]; H->y[i] = Y[st[i]]; }
}
/* does a cup's picture come within g of a box (separating axes: the box's two and each hull edge's normal) */
static int hull_meets(const Hull *H, const Reach *b, double g)
{
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (int i = 0; i < H->n; i++) { x0 = dmin(x0, H->x[i]); x1 = dmax(x1, H->x[i]); y0 = dmin(y0, H->y[i]); y1 = dmax(y1, H->y[i]); }
    if (x1 < b->x0 - g || x0 > b->x1 + g || y1 < b->y0 - g || y0 > b->y1 + g) return 0;
    for (int i = 0; i < H->n; i++) {
        const int j = (i + 1) % H->n;
        const double nx = H->y[j] - H->y[i], ny = H->x[i] - H->x[j], nl = hyp(nx, ny);
        if (!(nl > 0)) continue;
        /* the hull lies on the side of this edge its winding puts it; the box past it by more than g separates */
        const double c = (nx * H->x[i] + ny * H->y[i]) / nl;
        double bmin = 1e30;
        for (int q = 0; q < 4; q++) bmin = dmin(bmin, (nx * (q & 1 ? b->x1 : b->x0) + ny * (q & 2 ? b->y1 : b->y0)) / nl);
        double hmax = -1e30;
        for (int q = 0; q < H->n; q++) hmax = dmax(hmax, (nx * H->x[q] + ny * H->y[q]) / nl);
        (void)c;
        if (bmin > hmax + g) return 0;
        double bmax = -1e30, hmin = 1e30;
        for (int q = 0; q < 4; q++) bmax = dmax(bmax, (nx * (q & 1 ? b->x1 : b->x0) + ny * (q & 2 ? b->y1 : b->y0)) / nl);
        for (int q = 0; q < H->n; q++) hmin = dmin(hmin, (nx * H->x[q] + ny * H->y[q]) / nl);
        if (bmax < hmin - g) return 0;
    }
    return 1;
}

/* a cup tipped by `a` of its full lift `full`, as cn_stage tips it */
static void tip(CnObj *o, float a, float full)
{
    const CnPeek p = cn_cam_peek_tilt(o->R, a, full);
    o->tilt_angle = p.angle; o->tilt_hinge_y = p.hinge_y; o->tilt_back = p.back; o->tilt_lift = p.lift;
}

/* A CUP ON ITS WAY UP. The reveal tips a cup from standing to `full` (the stage's
 * lift, by the LIFT beat's progress), and the crown is highest part way up (at
 * atan(R / h), about 25 degrees, it is sqrt(R^2 + h^2) over the table, past the
 * standing h), so a picture past the side can come and go mid lift: the reach is
 * every LIFT_STEPS'th of the way, the standing cup and the full tip with them. */
#define LIFT_STEPS 8
static void tipped_reach(const CnLay *L, const CnObj *rest, float full, Reach *r)
{
    for (int k = 0; k <= LIFT_STEPS; k++) {
        CnObj o = *rest;
        if (k && full > 0) tip(&o, full * k / LIFT_STEPS, full);
        cup_reach(L, &o, r);
        if (!(full > 0)) break;
    }
}

/* a standing cup at the reveal, through its lift, as cn_stage tips it (DECISIONS I23): to the least tip
 * that shows the dice die_spot puts under it (a seat whose cup did not throw has its dice there) */
static void lift_reach(const CnLay *L, const CnObj *rest, int seat, int ndice, double ring, double d, Reach *r)
{
    float full = 0;
    if (!rest->out && ndice) {
        float dy[CN_LAY_DICE], dd[CN_LAY_DICE];
        for (int k = 0; k < ndice; k++) { double x, y, yaw; die_spot(seat, k, rest->x, rest->y, ring, &x, &y, &yaw); dy[k] = (float)y; dd[k] = (float)d; }
        full = cn_cam_peek_angle(&L->cam, rest->R, rest->y, dy, dd, ndice);
    }
    tipped_reach(L, rest, full, r);
}

/* a name's box (cn_lay.h's CN_NAME_BOX: w by h, centred on x, its top 8 above y) on the glass: the names
 * are drawn flat and turned with the planks, so the box's corners go through the turn */
static Reach name_box(const CnLay *L, int s, double w, double h)
{
    Reach r = reach_none();
    const double x = L->board_x + L->name_x[s], y = L->board_y + L->name_y[s];
    for (int c = 0; c < 4; c++) {
        float gx, gy;
        cn_cam_map(&L->cam, (float)(x + (c & 1 ? 1 : -1) * w / 2), (float)(y - CN_LAY_NAME_UP + (c & 2 ? h : 0)), &gx, &gy);
        r.x0 = dmin(r.x0, gx); r.x1 = dmax(r.x1, gx); r.y0 = dmin(r.y0, gy); r.y1 = dmax(r.y1, gy);
    }
    return r;
}

/* The throw a seat's cup at (x, y) makes, held at reach k of the study's: its
 * cup at every 60 Hz frame from the start until it is home (the frames a bake
 * writes; the host's in-between poses are within a hair of them). */
static void throw_reach(const CnLay *L, const CnThrow *t0, double k, Reach *r)
{
    CnThrow t = *t0;
    const float kf = (float)k;
    t.scale = kf * t.cup_r / CN_THROW_REF_R;   /* in floats, as cn_lay_throws writes it */
    const double span = cn_roll_cup_span(&t);
    float fr[CN_ROLL_FRAME_FLOATS];
    memset(fr, 0, sizeof fr);
    CnObj o;
    cn_geom_cup_obj(&o, t.cup_r, 0, 0, t.cup_x, t.cup_y, 0, 0);
    for (int f = 0; f <= (int)cn_m_ceil(span * CN_ROLL_HZ); f++) {
        CnPose p;
        cn_roll_cup_pose(&t, (double)f / CN_ROLL_HZ, fr);
        cn_geom_pose_at(fr, 0, 1, 0, 0, &p);
        o.has_rot = 1; memcpy(o.rot, p.rot, sizeof o.rot);
        o.x = p.p[0]; o.y = p.p[1]; o.lift = p.p[2];
        cup_reach(L, &o, r);
    }
}

/* my throw's reach: the study's when my held cup stays inside the drawer, else
 * the largest that keeps it there, never under CN_LAY_REACH_MIN */
static double fit_reach(const CnLay *L, const CnThrow *t)
{
    Reach r = reach_none();
    throw_reach(L, t, 1, &r);
    if (reach_inside(&r, L->w)) return 1;
    double lo = CN_LAY_REACH_MIN, hi = 1;
    r = reach_none(); throw_reach(L, t, lo, &r);
    if (!reach_inside(&r, L->w)) return lo;
    for (int i = 0; i < 10; i++) {
        const double m = (lo + hi) / 2;
        r = reach_none(); throw_reach(L, t, m, &r);
        if (reach_inside(&r, L->w)) lo = m; else hi = m;
    }
    return lo;
}

/* a cup's picture as seven discs from its mouth (on the table) to its crown
 * (leaning up and out from the eye's foot), for the apart test */
typedef struct { double x, y, r; } Disc;
typedef struct { Disc d[7]; } Pillar;
static Pillar pillar(double x, double y, double r, double mcx, double Lf, double lean_f, double ey)
{
    Pillar p;
    const double xc = mcx + (x - mcx) * Lf, yc2 = y - (ey - y) * lean_f, rc = CN_CUP_RC * r * Lf;
    for (int k = 0; k <= 6; k++) {
        const double u = k / 6.0;
        p.d[k].x = x + (xc - x) * u; p.d[k].y = y + (yc2 - y) * u; p.d[k].r = r + (rc - r) * u;
    }
    return p;
}

/* THE RING, FITTED TO THE CAMERA (DECISIONS K14, round twenty-two). Every other
 * seat sits on an ellipse through my cup at equal angles, as tall as the glass
 * allows (the farthest cup's crown, leaning up the screen from the fixed eye,
 * just under the HUD's plate after the turn; at most twice as tall as wide) and
 * as wide as the screen allows after the turn. Cups that would touch are all
 * made smaller, two points at a time, until none do. A lying cup is drawn in
 * toward the centre as far as it must to stay on the screen.
 *
 * THEN FITTED TO THE GLASS (package V2). The study fits the ring with its own map
 * (the zoom after the divide), which is up to 36 points off the painted one far up
 * a tall drawer, so a side cup's crown left the drawer above 760; and at the
 * reveal every standing cup is tipped (I23), which swings its crown up the screen
 * past the plate. So each seat's cup as the eye sees it (standing, lying, or at
 * the reveal tipped to its full lift) is kept CN_LAY_EDGE inside the drawer and
 * off the plate: the ring is made a point shorter while a cup is past the top or
 * over the plate, a point narrower while one is past a side, and its cups two
 * points smaller when it would be shorter or narrower than a cup. Where the
 * study's ring fits, nothing moves. */
static void ring_place(const CnLayIn *in, const CnLay *L, const double *ang, double mcx, double mcy, double rx, double ry,
                       double R, double h, double half, double *ys, double *sx, double *sy)
{
    const int n = in->seats, me = in->me;
    const double hc = CN_CAM_VIEW_HC;
    for (int j = 0; j < n - 1; j++) ys[j] = mcy - ry * (1 - cn_m_cos(ang[j]));
    for (int j = 0; j < n - 1; j++) { int i = (me + 1 + j) % n; sx[i] = mcx - cn_m_sin(ang[j]) * rx; sy[i] = ys[j]; }
    /* a seat that is out has its cup lying across it, its height along the table */
    const double Ll = hc / (hc - R);
    for (int j = 0; j < n - 1; j++) {
        int i = (me + 1 + j) % n;
        if (!(in->out_mask >> i & 1)) continue;
        float sm;
        cn_cam_to_screen(&L->cam, (float)(sy[i] - mcy), &sm);
        const double over = (dabs(sx[i] - mcx) + h / 2 + CN_CUP_RC * R) * Ll - (half / sm - 4);
        const double sg = sx[i] > mcx ? 1 : sx[i] < mcx ? -1 : 0;
        if (over > 0) sx[i] = sx[i] - sg * over / Ll;
    }
}

static void ring_seats(const CnLayIn *in, CnLay *L, double W, double H, double myR, double mcx, double mcy, double hudB)
{
    const int n = in->seats, me = in->me;
    const double hc = CN_CAM_VIEW_HC, ey = H / 2 + CN_CAM_VIEW_DOWN * H, half = (W + 32) / 2 - 6;
    double ang[MAXS], stretch = 0;
    for (int j = 0; j < n - 1; j++) { ang[j] = CN_PI * 2 * (j + 1) / n; stretch = dmax(stretch, 1 - cn_m_cos(ang[j])); }
    const CnCam *cam = &L->cam;
    for (double R = myR; ; R -= 2) {
        const double h = R * CN_CUP_TALL, Lf = hc / (hc - h), f = Lf - 1, crown = CN_CUP_RC * R * Lf;
        /* the top seat: the board y whose crown top lands at the HUD's foot, then the table y whose crown that is */
        const double yc = mcy + cn_cam_from_screen(cam, (float)(hudB + 6)), yTop = (yc + ey * f + crown) / (1 + f);
        double ry = dmax(R, (mcy - yTop) / stretch), ys[MAXS], rx = W;
        for (int j = 0; j < n - 1; j++) ys[j] = mcy - ry * (1 - cn_m_cos(ang[j]));
        /* the width: no crown and no mouth past the screen's side after the turn, for any seat */
        for (int j = 0; j < n - 1; j++) {
            const double sn = dabs(cn_m_sin(ang[j]));
            if (sn < 1e-6) continue;
            float sc, sm;
            cn_cam_to_screen(cam, (float)(ys[j] - (ey - ys[j]) * f - mcy), &sc);
            cn_cam_to_screen(cam, (float)(ys[j] - mcy), &sm);
            rx = dmin(rx, dmin((half / sc - crown) / (sn * Lf), (half / sm - R) / sn));
        }
        /* a ring is at most twice as tall as it is wide */
        if (ry > 2 * rx) ry = 2 * rx;
        /* fitted to the glass: every cup as the eye sees it on this screen, inside and off the plate */
        const double sd = L->d * R / myR, sring = ring_of(sd);
        double sx[MAXS], sy[MAXS];
        int fits = 0;
        for (int it = 0; it < 2000; it++) {
            ring_place(in, L, ang, mcx, mcy, rx, ry, R, h, half, ys, sx, sy);
            /* how far past: the top (or into the plate) and a side, on the glass; a step is half of it, a point
             * at least (a point of ring moves a picture about a point, so half never steps past the fit) */
            double top = 0, side = 0;
            for (int j = 0; j < n - 1; j++) {
                const int i = (me + 1 + j) % n, out = in->out_mask >> i & 1;
                CnObj o;
                cn_geom_cup_obj(&o, (float)R, 0, (uint32_t)(i * 7), (float)sx[i], (float)sy[i], out, (float)(W / 2));
                Reach r = reach_none();
                if (in->reveal) lift_reach(L, &o, i, in->dice[i], sring, sd, &r);
                else cup_reach(L, &o, &r);
                if (r.y0 < CN_LAY_EDGE) top = dmax(top, CN_LAY_EDGE - r.y0);
                if (L->has_plate && over_rect(&r, L->plate, 2)) top = dmax(top, L->plate[1] + L->plate[3] + 2 - r.y0);
                if (r.x0 < CN_LAY_EDGE) side = dmax(side, CN_LAY_EDGE - r.x0);
                if (r.x1 > L->w - CN_LAY_EDGE) side = dmax(side, r.x1 - (L->w - CN_LAY_EDGE));
            }
            if (!(top > 0) && !(side > 0)) { fits = 1; break; }
            if (top > 0) ry -= dmax(1, cn_m_floor(top / 2));
            if (side > 0) rx -= dmax(1, cn_m_floor(side / 2));
            if (ry < R || rx < R) break;
        }
        if (!fits && R - 2 >= CN_LAY_ROW_MIN_R) continue;
        /* apart: no cup's picture within a few points of another's */
        Pillar all[MAXS];
        all[0] = pillar(mcx, mcy, myR, mcx, Lf, f, ey);
        for (int j = 0; j < n - 1; j++) all[1 + j] = pillar(sx[(me + 1 + j) % n], ys[j], R, mcx, Lf, f, ey);
        int apart = 1;
        for (int a = 0; a < n && apart; a++) for (int b = a + 1; b < n && apart; b++)
            for (int p = 0; p < 7 && apart; p++) for (int q = 0; q < 7; q++) {
                const Disc *P = &all[a].d[p], *Q = &all[b].d[q];
                if (hyp(P->x - Q->x, P->y - Q->y) < P->r + Q->r + 6) { apart = 0; break; }
            }
        if (!apart && R > 30) continue;
        /* the canvas: up to the glass's top (untilted), and out to the widest crown
         * or a lying cup's length along the table */
        double reach = W / 2;
        for (int j = 0; j < n - 1; j++) {
            const double sn = dabs(cn_m_sin(ang[j]));
            reach = dmax(reach, dmax(sn * rx * Lf + crown + 4, sn * rx + h + 4));
        }
        /* the name: under the mouth, unless a nearer cup leans over that spot; then
         * beside the mouth, on the side toward the ring's centre */
        for (int j = 0; j < n - 1; j++) {
            const int i = (me + 1 + j) % n;
            const double x = sx[i], y = sy[i], side = x < mcx - 1 ? 1 : x > mcx + 1 ? -1 : 0;
            const double spots[3][2] = { { x, y + R + 14 }, { x + side * R * .75, y + R * .75 + 12 }, { x + side * (R + 30), y } };
            int pick = 0;
            if (side != 0) {
                pick = -1;
                for (int s = 0; s < 3 && pick < 0; s++) {
                    int free = 1;
                    for (int k = 0; k < n && free; k++) {
                        if (k == 1 + j) continue;
                        for (int q = 0; q < 7; q++) {
                            const Disc *Q = &all[k].d[q];
                            if (!(hyp(spots[s][0] - Q->x, spots[s][1] - Q->y) > Q->r + 18)) { free = 0; break; }
                        }
                    }
                    if (free) pick = s;
                }
                if (pick < 0) pick = 0;
            }
            L->name_x[i] = (float)spots[pick][0]; L->name_y[i] = (float)spots[pick][1]; L->name_how[i] = CN_NAME_BOX;
        }
        for (int j = 0; j < n - 1; j++) { int i = (me + 1 + j) % n; L->cup_x[i] = (float)sx[i]; L->cup_y[i] = (float)sy[i]; }
        L->cup_r = (float)R;
        L->ring_cy = (float)(mcy - ry); L->ring_rx = (float)rx; L->ring_ry = (float)ry;
        L->pad = (float)dmax(40, cn_m_ceil(-(mcy + cn_cam_from_screen(cam, 0)) + 8));
        L->pad_x = (float)cn_m_ceil(reach - W / 2);
        return;
    }
}

/* a short board: the other seats in a row along the top, their cups as big as
 * the row allows (my cup's size at most), then a point smaller at a time until
 * every crown in the row, standing or lying, is inside the drawer (the study's
 * row is; below a 323 drawer the study's crowns lean past its top) */
/* THE ROW'S STEP AND THE PLATE BESIDE IT (package S). The study's step (a badge and a margin, less when the
 * row is crowded), and the plate the room right of the row (CN_LAY_PLATE_W at most). At four seats that room
 * is 102 points, where a bid beside its die took two lines ("three / 6s", the verifier, and the study too):
 * so when the plate would be under CN_LAY_PLATE_SHORT_W the row's step narrows to leave it that, as long as
 * the step stays CN_LAY_ROW_STEP_MIN (five seats and more keep the study's row and have no plate). The plate's
 * width, 0 for none (under CN_LAY_PLATE_MIN). */
static double row_step(int others, double W, double *plate_w)
{
    double step = dmin(CN_LAY_BADGE + CN_LAY_MARGIN, (W - 2 * CN_LAY_MARGIN) / others);
    double w = dmin(CN_LAY_PLATE_W, W - 2 * CN_LAY_MARGIN - others * step);
    if (w < CN_LAY_PLATE_SHORT_W) {
        const double s2 = (W - 2 * CN_LAY_MARGIN - CN_LAY_PLATE_SHORT_W) / others;
        if (s2 >= CN_LAY_ROW_STEP_MIN) { step = s2; w = CN_LAY_PLATE_SHORT_W; }
    }
    if (plate_w) *plate_w = w >= CN_LAY_PLATE_MIN ? w : 0;
    return step;
}

static void row_seats(const CnLayIn *in, CnLay *L, double W, double H, double myR, double mcy)
{
    const int n = in->seats, me = in->me, others = n - 1;
    const double step = row_step(others, W, 0), bandTop = H - CN_LAY_SHORT_BAND;
    double R = dmin(myR, step / 2 - 4), cy;
    for (;; R -= 1) {
        cy = dmax(R + 2, (bandTop - (2 * R + 24)) / 2 + R);
        if (R - 1 < CN_LAY_ROW_MIN_R) break;
        Reach r = reach_none();
        for (int j = 0; j < others; j++) {
            const int i = (me + 1 + j) % n;
            CnObj o;
            cn_geom_cup_obj(&o, (float)R, 0, (uint32_t)(i * 7), (float)(CN_LAY_MARGIN + j * step + step / 2), (float)cy, in->out_mask >> i & 1, (float)(W / 2));
            cup_reach(L, &o, &r);
        }
        if (reach_inside(&r, L->w)) break;
    }
    for (int j = 0; j < others; j++) {
        const int i = (me + 1 + j) % n;
        L->cup_x[i] = (float)(CN_LAY_MARGIN + j * step + step / 2); L->cup_y[i] = (float)cy;
        L->name_x[i] = L->cup_x[i]; L->name_y[i] = (float)(cy + R + 14); L->name_how[i] = CN_NAME_BOX;
    }
    L->cup_r = (float)R;
    L->ring_cy = (float)cy; L->ring_rx = 0; L->ring_ry = 0;
    L->pad = (float)dmax(40, cn_m_ceil(-(mcy + cn_cam_from_screen(&L->cam, 0)) + 8));
    L->pad_x = 40;
}

/* THE REVEAL ON A SHORT BOARD: EVERY SEAT IN ONE ROW (package V2, DECISIONS I29).
 * The short board's row has no room above it: its crowns are fitted to the
 * drawer's top at rest, and the reveal's tip (I23) swings each crown about two of
 * its radii further up, so the study's reveal there left the drawer by 42 to 69
 * points. And my cup, tipped, swung up over the middle seat's name and dice. So
 * the reveal lays the short board as the study's own reveal lists the seats: one
 * row, mine first and the others after it in seat order, on the board the
 * reveal's shelf leaves. Its cups are as big as the row allows (my table cup's
 * size at most) and then a point smaller at a time until the row, as high as it
 * can go, has every cup tipped to its full lift CN_LAY_EDGE inside the drawer
 * with every name on the board. Every seat's dice are its own ring under its
 * cup, one size; no cup throws (the reveal is still).
 * The tally's plate (CN_LAY_PLATE_REVEAL_W at most, CN_LAY_PLATE_MIN at least) goes
 * beside the row, the row from the left as the table's is, when there is room
 * right of every lifted cup and name; else the row is centred and the plate goes
 * under the names when it and the outcome line fit over the shelf; else there is
 * none (as a short board's row of five has none). */
static void row_fit(const CnLayIn *in, const CnLay *L, double W, double H, double d0, double myR0, double x0, double step,
                    double *R_out, double *cy_out, Reach *ext)
{
    const int n = in->seats, me = in->me;
    /* under each mouth its name and the loser's stamp, all over the outcome line over the shelf */
    const double below = 14 + CN_LAY_STAMP_FOOT, foot = dmin(H, L->shelf[1] - 6 - CN_LAY_OUTCOME_H - L->board_y);
    double R = dmin(myR0, step / 2 - 4), cy = R + 2;
    Reach r;
    for (;; R -= 1) {
        const double sd = d0 * R / myR0, sring = ring_of(sd);
        int ok = 0;
        for (cy = R + 2; cy + R + below <= foot; cy += 1) {
            r = reach_none();
            for (int v = 0; v < n; v++) {
                const int i = (me + v) % n, out = in->out_mask >> i & 1;
                CnObj o;
                cn_geom_cup_obj(&o, (float)R, 0, (uint32_t)(i * 7), (float)(x0 + v * step), (float)cy, out, (float)(W / 2));
                lift_reach(L, &o, i, in->dice[i], sring, sd, &r);
            }
            if ((ok = reach_inside(&r, L->w))) break;
        }
        if (ok || R - 1 < CN_LAY_ROW_MIN_R) break;
    }
    if (cy + R + below > foot) cy = R + 2;
    *R_out = R; *cy_out = cy; *ext = r;
}

static void one_row(const CnLayIn *in, CnLay *L, double W, double H, double d0, double myR0)
{
    const int n = in->seats, me = in->me;
    const double step = dmin(CN_LAY_BADGE + CN_LAY_MARGIN, (W - 2 * CN_LAY_MARGIN) / n), below = 14 + CN_LAY_STAMP_FOOT;
    const double right = CN_LAY_SIDE + W - CN_LAY_MARGIN, lowest = L->shelf[1] - 6 - CN_LAY_OUTCOME_H;
    double R, cy, w = 0;
    Reach ext;
    /* from the left, the plate beside: right of every lifted cup and every name's letters */
    double x0 = CN_LAY_MARGIN + step / 2;
    row_fit(in, L, W, H, d0, myR0, x0, step, &R, &cy, &ext);
    for (int v = 0; v < n; v++) {
        const int i = (me + v) % n;
        L->name_x[i] = (float)(x0 + v * step); L->name_y[i] = (float)(cy + R + 14);
        const Reach nb = name_box(L, i, CN_LAY_NAME_TEXT_W, CN_LAY_NAME_H);
        ext.x1 = dmax(ext.x1, nb.x1);
    }
    w = dmin(CN_LAY_PLATE_REVEAL_W, right - (ext.x1 + 2));
    const int beside = w >= CN_LAY_PLATE_MIN;
    if (!beside) { x0 = (W - n * step) / 2 + step / 2; row_fit(in, L, W, H, d0, myR0, x0, step, &R, &cy, &ext); }
    const double sd = d0 * R / myR0, sring = ring_of(sd);
    for (int v = 0; v < n; v++) {
        const int i = (me + v) % n;
        L->cup_x[i] = (float)(x0 + v * step); L->cup_y[i] = (float)cy;
        L->name_x[i] = L->cup_x[i]; L->name_y[i] = (float)(cy + R + 14); L->name_how[i] = CN_NAME_BOX;
    }
    L->cup_r = L->my_r = (float)R;
    L->d = L->sd = (float)sd; L->ring = L->sring = (float)sring;
    L->ring_cy = (float)cy; L->ring_rx = 0; L->ring_ry = 0;
    L->pad = (float)dmax(40, cn_m_ceil(-(L->cam.origin_y - L->board_y + cn_cam_from_screen(&L->cam, 0)) + 8));
    L->pad_x = 40;
    L->outcome[0] = 0; L->outcome[1] = (float)lowest; L->outcome[2] = (float)L->w; L->outcome[3] = CN_LAY_OUTCOME_H;
    if (beside) {
        /* level with the row's mouths, never over the outcome line */
        const double y = dmax(CN_LAY_EDGE, dmin(L->board_y + cy - CN_LAY_PLATE_H / 2, lowest - CN_LAY_PLATE_H));
        L->has_plate = 1;
        L->plate[0] = (float)(right - w); L->plate[1] = (float)y; L->plate[2] = (float)w; L->plate[3] = CN_LAY_PLATE_H;
        return;
    }
    const double top = L->board_y + cy + R + below + 4;
    w = dmin(CN_LAY_PLATE_REVEAL_W, W);
    if (top + CN_LAY_PLATE_H <= lowest) {
        L->has_plate = 1;
        L->plate[0] = (float)(L->w / 2 - w / 2); L->plate[1] = (float)top; L->plate[2] = (float)w; L->plate[3] = CN_LAY_PLATE_H;
    }
}

/* THE REVEAL ON A TALL BOARD: THE STUDY'S LIST, IN ROWS (package S). The ring
 * puts a seat behind another (six seats: Cy behind Bo, Ed behind Fay; four: the
 * top seat behind my cup), and a cup tipped up to show its own dice swings its
 * crown up the screen over the dice of the seat behind it: the verifier saw a
 * counting die under another seat's cup at four and six seats. The study's
 * reveal (UI.html, `spec.state === 'reveal'`) is a list of the seats, mine
 * first, every die in sight. So a tall board's reveal lays the seats as that
 * list: rows of `per` seats, mine first, each row centred, each seat's dice
 * its ring under its cup at the stations (no cup throws: the reveal is still,
 * as one_row's is). The rows go down the board, the first as high as lets its
 * cups tip in full off the plate, each next one as high as keeps its tipped
 * cups off every die, name and stamp of the rows behind it; the outcome line
 * under the last row's stamps. Every cup the same size, the largest any count
 * of rows allows (fewer rows on a tie): six seats at 390 by 718 are two rows of
 * three. */
#define LIST_STAMP_W 120.0   /* the loser's stamp ("LOSES A DIE" in the small caps at 12, tracked, its frame) */
/* a die's picture at a station: the box round its eight corners on the glass */
static Reach die_box(const CnLay *L, double x, double y, double d)
{
    Reach r = reach_none();
    const double e = d * .71;
    for (int c = 0; c < 8; c++) {
        float px, py, gx, gy;
        cn_cam_project(&L->cam, (float)(x + (c & 1 ? e : -e)), (float)(y + (c & 2 ? e : -e)), (float)(c & 4 ? d : 0), &px, &py);
        cn_cam_map(&L->cam, L->board_x + px, L->board_y + py, &gx, &gy);
        r.x0 = dmin(r.x0, gx); r.x1 = dmax(r.x1, gx); r.y0 = dmin(r.y0, gy); r.y1 = dmax(r.y1, gy);
    }
    return r;
}
/* the flat box under a name's anchor a stamp takes (centred, CN_LAY_STAMP_FOOT down), on the glass */
static Reach stamp_box(const CnLay *L, double x, double y)
{
    Reach r = reach_none();
    for (int c = 0; c < 4; c++) {
        float gx, gy;
        cn_cam_map(&L->cam, (float)(L->board_x + x + (c & 1 ? 1 : -1) * LIST_STAMP_W / 2), (float)(L->board_y + y + (c & 2 ? CN_LAY_STAMP_FOOT : 14)), &gx, &gy);
        r.x0 = dmin(r.x0, gx); r.x1 = dmax(r.x1, gx); r.y0 = dmin(r.y0, gy); r.y1 = dmax(r.y1, gy);
    }
    return r;
}
/* a seat's cup at (x, y) tipped to the full lift that shows its dice at their stations */
static void list_cup(const CnLayIn *in, const CnLay *L, int i, double R, double x, double y, double sd, double sring, CnObj *o)
{
    cn_geom_cup_obj(o, (float)R, 0, (uint32_t)(i * 7), (float)x, (float)y, in->out_mask >> i & 1, (float)(L->board_w / 2));
    if (o->out || !in->dice[i]) return;
    float dy[CN_LAY_DICE], dd[CN_LAY_DICE];
    for (int k = 0; k < in->dice[i]; k++) { double dx, ddy, yaw; die_spot(i, k, x, y, sring, &dx, &ddy, &yaw); dy[k] = (float)ddy; dd[k] = (float)sd; }
    const float full = cn_cam_peek_angle(&L->cam, (float)R, (float)y, dy, dd, in->dice[i]);
    tip(o, full, full);
}

typedef struct { double R, cy[CN_LAY_SEATS], x[CN_LAY_SEATS], y[CN_LAY_SEATS]; int per, rows; } List;

/* the list with `per` seats a row and cups of radius R: 1 when it fits over the outcome line */
static int list_try(const CnLayIn *in, const CnLay *L, double W, double d0, double myR0, int per, double R, List *out)
{
    const int n = in->seats, me = in->me, rows = (n + per - 1) / per;
    const double step = (W - 2 * CN_LAY_MARGIN) / per, sd = d0 * R / myR0, sring = ring_of(sd);
    const double below = 14 + CN_LAY_STAMP_FOOT, foot = L->shelf[1] - 6 - CN_LAY_OUTCOME_H - L->board_y;
    Reach behind[CN_LAY_SEATS * (CN_LAY_DICE + 2)];
    int nb = 0;
    double cy = R + 2;
    int fail = 0;
    out->R = R; out->per = per; out->rows = rows;
    for (int r = 0; r < rows; r++) {
        const int first = r * per, m = n - first < per ? n - first : per;
        const double x0 = (W - m * step) / 2 + step / 2;
        int ok = 0;
        for (; cy + R + below <= foot; cy += 1) {
            Reach ext = reach_none();
            ok = 1;
            for (int c = 0; c < m && ok; c++) {
                const int i = (me + first + c) % n;
                CnObj o;
                cn_geom_cup_obj(&o, (float)R, 0, (uint32_t)(i * 7), (float)(x0 + c * step), (float)cy, in->out_mask >> i & 1, (float)(W / 2));
                lift_reach(L, &o, i, in->dice[i], sring, sd, &ext);
                if (!nb) continue;
                /* the rows behind: no die, name or stamp of theirs under this row's tipped cups */
                list_cup(in, L, i, R, x0 + c * step, cy, sd, sring, &o);
                Hull Hh;
                cup_hull(L, &o, &Hh);
                for (int b = 0; b < nb && ok; b++) ok = !hull_meets(&Hh, &behind[b], 1);
            }
            if (ok) ok = reach_inside(&ext, L->w) && !(L->has_plate && over_rect(&ext, L->plate, 2));
            if (ok) break;
        }
        if (!ok) { fail = 1; cy = dmax(R + 2, dmin(cy, foot - R - below)); }   /* placed anyway: the fallback's */
        out->cy[r] = cy;
        for (int c = 0; c < m; c++) {
            const int i = (me + first + c) % n;
            const double x = x0 + c * step;
            out->x[i] = x; out->y[i] = cy;
            if (!(in->out_mask >> i & 1))
                for (int k = 0; k < in->dice[i]; k++) { double dx, dy, yaw; die_spot(i, k, x, cy, sring, &dx, &dy, &yaw); behind[nb++] = die_box(L, dx, dy, sd); }
            Reach nbx = reach_none();
            for (int q = 0; q < 4; q++) {
                float gx, gy;
                cn_cam_map(&L->cam, (float)(L->board_x + x + (q & 1 ? 1 : -1) * CN_LAY_NAME_TEXT_W / 2), (float)(L->board_y + cy + R + 14 - CN_LAY_NAME_UP + (q & 2 ? CN_LAY_NAME_H : 0)), &gx, &gy);
                nbx.x0 = dmin(nbx.x0, gx); nbx.x1 = dmax(nbx.x1, gx); nbx.y0 = dmin(nbx.y0, gy); nbx.y1 = dmax(nbx.y1, gy);
            }
            behind[nb++] = nbx;
            behind[nb++] = stamp_box(L, x, cy + R + 14);
        }
        cy += 2 * R;   /* the next row's mouths at least a cup's width under these */
    }
    return !fail;
}

static void list_rows(const CnLayIn *in, CnLay *L, double W, double d0, double myR0)
{
    const int n = in->seats;
    List best, t;
    best.R = 0;
    for (int per = n; per >= 1; per--) {
        if (per > 1 && (n + per - 1) / per == (n + per - 2) / (per - 1)) continue;   /* as many rows as one fewer a row, which is wider */
        const double step = (W - 2 * CN_LAY_MARGIN) / per;
        for (double R = cn_m_floor(dmin(myR0, step / 2 - 4)); R >= CN_LAY_ROW_MIN_R && R > best.R; R -= 1)
            if (list_try(in, L, W, d0, myR0, per, R, &t)) { best = t; break; }
    }
    if (!(best.R > 0)) list_try(in, L, W, d0, myR0, (n + 1) / 2, CN_LAY_ROW_MIN_R, &best);
    const double R = best.R, sd = d0 * R / myR0, sring = ring_of(sd);
    double last = 0;
    for (int i = 0; i < n; i++) {
        L->cup_x[i] = (float)best.x[i]; L->cup_y[i] = (float)best.y[i];
        L->name_x[i] = L->cup_x[i]; L->name_y[i] = (float)(best.y[i] + R + 14); L->name_how[i] = CN_NAME_BOX;
        last = dmax(last, best.y[i]);
    }
    L->list_rows = (uint8_t)best.rows;
    L->cup_r = L->my_r = (float)R;
    L->d = L->sd = (float)sd; L->ring = L->sring = (float)sring;
    L->ring_cy = (float)best.y[in->me]; L->ring_rx = 0; L->ring_ry = 0;
    L->pad = (float)dmax(40, cn_m_ceil(-(L->cam.origin_y - L->board_y + cn_cam_from_screen(&L->cam, 0)) + 8));
    L->pad_x = 40;
    /* the outcome line under the last row's stamps */
    const double lowest = L->shelf[1] - 6 - CN_LAY_OUTCOME_H;
    L->outcome[0] = 0; L->outcome[1] = (float)dmin(lowest, L->board_y + last + R + 14 + CN_LAY_STAMP_FOOT + 4);
    L->outcome[2] = (float)L->w; L->outcome[3] = CN_LAY_OUTCOME_H;
}

static int valid(const CnLayIn *in)
{
    if (in->seats < 2 || in->seats > CN_LAY_SEATS || in->me >= in->seats || in->turn >= in->seats) return 0;
    for (int s = 0; s < in->seats; s++) if (in->dice[s] > CN_LAY_DICE) return 0;
    for (int k = 0; k < in->dice[in->me]; k++) if (in->my_faces[k] < 1 || in->my_faces[k] > 6) return 0;
    if (!(in->w > 2 * CN_LAY_SIDE + 4 * CN_LAY_MARGIN) || !(in->h > 0)) return 0;
    return 1;
}

/* seat v's throw (v counts round from me), as cn_lay_throws writes it */
static void seat_throw(const CnLayIn *in, const CnLay *L, int v, int kind, CnLayThrow *t)
{
    const int i = (L->me + v) % L->seats;
    const double W = L->board_w, H = L->board_h, mcx = L->cup_x[i], mcy = L->cup_y[i];
    const uint32_t seed = in->seed;
    memset(t, 0, sizeof *t);
    const float R = v ? L->cup_r : L->my_r, d = v ? L->sd : L->d, ring = v ? L->sring : L->ring;
    cn_throw_default(&t->t, kind, (float)mcx, (float)mcy, R, d, ring);
    /* the dice the seat holds: the study baked five for every seat and drew
     * only the seat's; a die nobody draws must not knock the others about */
    t->t.dice = in->dice[i];
    /* mine held as high as the drawer allows (the default, R / CN_THROW_REF_R, when the study's fits) */
    if (v == 0 && kind == CN_THROW_CUP && L->my_reach < 1) t->t.scale = L->my_reach * R / CN_THROW_REF_R;
    t->t.band_x0 = (float)dmax(20, mcx - 160); t->t.band_x1 = (float)dmin(W - 20, mcx + 160);
    t->t.band_y0 = (float)dmax(20, mcy - 120); t->t.band_y1 = (float)dmin(H - 10, mcy + 60);
    t->t.shake_s = v ? (float)(1.5 + cn_geom_hash(v, 81, (int32_t)seed) * .8) : 0;
    t->delay = v ? (float)(.1 + cn_geom_hash(v, 82, (int32_t)seed) * .5) : 0;
    t->seed = (uint64_t)seed + (uint64_t)v * 1000;
    t->seat = (uint8_t)i;
}

/* MY DICE FIT MY CUP'S PICTURE. My five sit in a ring that keeps turned dice from
 * touching, under a cup whose mouth just covers the ring. The dice are the
 * study's (16 on a short board, 24 on a tall one), then half a point smaller at a
 * time until my cup, tipped as far as any peek tips it (its farthest die at the
 * ring's top, pushed out by the most the jitter does), stays inside the drawer:
 * tipped about the far edge of its mouth, its crown swings up the screen, past
 * the top under a 310 drawer and on a tall one under 450. On a short board my
 * cup standing also stays off the row's names (at six seats its crown leaned
 * over the middle seat's). */
static double fit_dice(const CnLayIn *in, const CnLay *L, int shrt, double mcx, double mcy, double *ring_out, double *myR_out)
{
    double d = shrt ? 16 : 24, ring, myR;
    for (;; d -= .5) {
        ring = ring_of(d); myR = ring + d * .8 + 5;
        if (d - .5 < CN_LAY_MY_D_MIN) break;
        const float fy = (float)(mcy - ring * 1.05), fd = (float)d;
        const float a = cn_cam_peek_angle(&L->cam, (float)myR, (float)mcy, &fy, &fd, 1);
        CnObj o;
        cn_geom_cup_obj(&o, (float)myR, 0, 3, (float)mcx, (float)mcy, 0, 0);
        Hull stand;
        cup_hull(L, &o, &stand);
        tip(&o, a, a);
        Reach r = reach_none();
        cup_reach(L, &o, &r);
        int ok = reach_inside(&r, L->w);
        if (shrt) for (int s = 0; s < in->seats && ok; s++) {
            if (s == in->me) continue;
            const Reach nb = name_box(L, s, CN_LAY_NAME_TEXT_W, CN_LAY_NAME_TEXT_H);
            ok = !hull_meets(&stand, &nb, 0);
        }
        if (ok) break;
    }
    *ring_out = ring; *myR_out = myR;
    return d;
}

static int make(const CnLayIn *in, CnLay *L, int fit);

int cn_lay_make(const CnLayIn *in, CnLay *L) { return make(in, L, 1); }

/* THE TOP MARGIN grows from 8 to 30 over the 22 points above 400, a point a point,
 * where the study stepped it at 400: a 400 drawer was tall (its picker-up board
 * exactly 280) and 401 to 421 short again (259 to 279). Grown a point a point, the
 * picker-up board is 280 from 400 to 422 and then grows, so short and tall is one
 * threshold on the drawer (under 400 short) and the board never shrinks as the
 * drawer grows (package V2). */
static double top_margin(double H) { return H <= 400 ? 8 : dmin(30, H - 392); }

/* The board and my cup's centre on it: what the camera is made from (cn_lay_cam)
 * and what the layout starts from (make), one derivation for both. */
typedef struct {
    double W, H, inner, shelf_h, topM, pickerBoard, boardH, band, mcx, mcy;
    int rolling, shelf, shrt, row1;
} Board;

static int board_of(const CnLayIn *in, Board *b)
{
    b->W = in->w; b->H = in->h; b->inner = b->W - 2 * CN_LAY_SIDE;
    b->rolling = in->rolling || in->reveal;
    b->shelf = in->turn == in->me || b->rolling;
    b->shelf_h = b->rolling ? CN_LAY_ROLL_H : CN_LAY_PICKER_H;
    /* short or tall reads the drawer: the board it has with the picker up. A
     * short board IS that board on every screen, so nothing on it moves when
     * the turn comes round, and the room the picker takes on my turn is the
     * room my cup's throw comes down through on theirs. The reveal on a short
     * board is the one exception: one row on the board its shelf leaves. */
    b->topM = top_margin(b->H); b->pickerBoard = b->H - b->topM - 12 - (CN_LAY_PICKER_H + 10);
    b->shrt = b->pickerBoard < CN_LAY_SHORT_H; b->row1 = b->shrt && in->reveal;
    b->boardH = b->shrt && !b->row1 ? b->pickerBoard : b->H - b->topM - 12 - (b->shelf ? b->shelf_h + 10 : 0);
    if (!(b->boardH > CN_LAY_SHORT_BAND)) return 0;
    b->band = b->shrt ? CN_LAY_SHORT_BAND : CN_LAY_MY_BAND;
    b->mcx = b->inner / 2; b->mcy = (b->row1 ? b->pickerBoard : b->boardH) - b->band + (b->shrt ? 30 : 34);
    return 1;
}

static void board_cam(const Board *b, CnCam *c)
{
    cn_cam_make(c, (float)b->inner, (float)b->boardH, (float)(CN_LAY_SIDE + b->mcx), (float)(b->topM + b->mcy), 1);
}

int cn_lay_cam(const CnLayIn *in, CnCam *c)
{
    memset(c, 0, sizeof *c);
    Board b;
    if (!in || !valid(in) || !board_of(in, &b)) return 0;
    board_cam(&b, c);
    return 1;
}

static int make(const CnLayIn *in, CnLay *L, int fit)
{
    memset(L, 0, sizeof *L);
    Board b;
    if (!in || !valid(in) || !board_of(in, &b)) return 0;
    const double W = b.W, H = b.H, inner = b.inner;
    const int shelf = b.shelf, shrt = b.shrt, row1 = b.row1, list = !shrt && in->reveal;
    const double shelf_h = b.shelf_h, topM = b.topM, boardH = b.boardH;
    const double band = b.band, mcx = b.mcx, mcy = b.mcy;
    const double hudB = shrt ? 0 : CN_LAY_HUD_TOP + CN_LAY_PLATE_H + 6;

    L->w = (float)W; L->h = (float)H;
    for (int i = 0; i < CN_CUP_SEGS; i++) { const double a = (double)i / CN_CUP_SEGS * CN_PI * 2; L->seg_c[i] = cn_m_cos(a); L->seg_s[i] = cn_m_sin(a); }
    L->board_x = CN_LAY_SIDE; L->board_y = (float)topM; L->board_w = (float)inner; L->board_h = (float)boardH;
    L->top_m = (float)topM; L->seats = in->seats; L->me = in->me; L->short_board = (uint8_t)shrt; L->one_row = (uint8_t)row1;
    L->my_band[0] = CN_LAY_MARGIN; L->my_band[1] = (float)(boardH - band); L->my_band[2] = (float)(inner - 2 * CN_LAY_MARGIN); L->my_band[3] = (float)band;
    L->pad_below = CN_LAY_PAD_BELOW;
    board_cam(&b, &L->cam);
    if (shelf) {
        L->has_shelf = 1;
        L->shelf[0] = 0; L->shelf[1] = (float)(H - shelf_h); L->shelf[2] = (float)W; L->shelf[3] = (float)shelf_h;
    }
    const int me = in->me;
    double d, ring, myR;

    if (row1) {
        /* my dice's size is the short table's (16), the row's cups and dice scaled from it */
        d = 16; ring = ring_of(d); myR = ring + d * .8 + 5;
        L->d = (float)d; L->ring = (float)ring; L->my_r = (float)myR;
        one_row(in, L, inner, boardH, d, myR);
    } else if (shrt) {
        /* the row first (it is as big as my cup at most: the study's dice's cup), then my dice, which stay off its names */
        d = 16; ring = ring_of(d); myR = ring + d * .8 + 5;
        row_seats(in, L, inner, boardH, myR, mcy);
        d = fit_dice(in, L, 1, mcx, mcy, &ring, &myR);
        if (L->cup_r > myR) row_seats(in, L, inner, boardH, myR, mcy);
        L->d = (float)d; L->ring = (float)ring; L->my_r = (float)myR;
        /* the plate beside the row (not drawn when the room left is under 100); cn_stage_test's HUD test
         * finds it clear of every cup and name at every height and seat count (package V2) */
        double w;
        row_step(in->seats - 1, inner, &w);
        if (w > 0) {
            L->has_plate = 1;
            L->plate[0] = (float)(CN_LAY_SIDE + inner - CN_LAY_MARGIN - w);
            L->plate[1] = (float)(topM + dmax(0, (boardH - CN_LAY_SHORT_BAND - CN_LAY_PLATE_H) / 2));
            L->plate[2] = (float)w; L->plate[3] = CN_LAY_PLATE_H;
        }
    } else if (list) {
        /* the plate at the top of the glass, the tally's width, then the list under it at the tall table's dice */
        L->has_plate = 1;
        L->plate[0] = (float)(W / 2 - CN_LAY_PLATE_REVEAL_W / 2); L->plate[1] = CN_LAY_HUD_TOP;
        L->plate[2] = CN_LAY_PLATE_REVEAL_W; L->plate[3] = CN_LAY_PLATE_H;
        d = 24; ring = ring_of(d); myR = ring + d * .8 + 5;
        list_rows(in, L, inner, d, myR);
    } else {
        /* the plate at the top of the glass, then my dice, then the ring */
        L->has_plate = 1;
        L->plate[0] = (float)(W / 2 - CN_LAY_PLATE_W / 2); L->plate[1] = CN_LAY_HUD_TOP;
        L->plate[2] = CN_LAY_PLATE_W; L->plate[3] = CN_LAY_PLATE_H;
        d = fit_dice(in, L, 0, mcx, mcy, &ring, &myR);
        L->d = (float)d; L->ring = (float)ring; L->my_r = (float)myR;
        if (fit) ring_seats(in, L, inner, boardH, myR, mcx, mcy, hudB);   /* (a reach fit wants my cup and the camera only) */
    }
    d = L->d; ring = L->ring; myR = L->my_r;
    if (!row1 && !list) {
        L->cup_x[me] = (float)mcx; L->cup_y[me] = (float)mcy;
        if (shrt) { L->name_x[me] = (float)(mcx - (myR + 64)); L->name_y[me] = (float)(boardH - band / 2); L->name_how[me] = CN_NAME_LEFT; }
        else { L->name_x[me] = (float)mcx; L->name_y[me] = (float)(boardH - 4); L->name_how[me] = CN_NAME_FOOT; }
        L->sd = (float)(d * L->cup_r / myR);
        L->sring = (float)ring_of(L->sd);
    }

    /* the peek: the least tip that shows every one of my dice from the eye */
    float dy[CN_LAY_DICE], dd[CN_LAY_DICE];
    int nd = in->dice[me];
    for (int k = 0; k < nd; k++) { double x, y, yaw; die_spot(me, k, L->cup_x[me], L->cup_y[me], ring, &x, &y, &yaw); dy[k] = (float)y; dd[k] = (float)d; }
    L->peek_target = nd ? cn_cam_peek_angle(&L->cam, (float)myR, L->cup_y[me], dy, dd, nd) : 0;
    float pk = in->peek < 0 ? 0 : in->peek > 1 ? 1 : in->peek;
    L->peek = cn_cam_peek_tilt((float)myR, L->peek_target * pk, L->peek_target);

    /* a die's side on the glass at each seat (the turn's own scale there) */
    for (int s = 0; s < in->seats; s++) {
        const double side = s == me ? d : L->sd, x = L->board_x + L->cup_x[s], y = L->board_y + L->cup_y[s];
        float ax, ay, bx, by;
        cn_cam_map(&L->cam, (float)(x - side / 2), (float)y, &ax, &ay);
        cn_cam_map(&L->cam, (float)(x + side / 2), (float)y, &bx, &by);
        L->die_g[s] = (float)hyp(bx - ax, by - ay);
    }

    /* MY THROW'S REACH is one on every screen of the drawer: the least of the reach that fits each screen's own
     * board (a short board is one board on every screen, the picker-up one; a tall one is another board on my turn
     * and on theirs, and a reach fitted on the picker-up board alone left 400 to 420 on their turn by 11 points,
     * package V2). The reveal throws nothing (its list lays the dice at their stations, package S). */
    L->my_reach = 1;
    if (fit && nd && !row1 && !list) {
        double k = 1;
        for (int sc = 0; sc < (shrt ? 1 : 2); sc++) {
            CnLayIn p = *in;
            p.turn = sc == 1 ? (uint8_t)((p.me + 1) % p.seats) : p.me; p.rolling = 0; p.reveal = 0;
            CnLay P;
            if (!make(&p, &P, 0)) continue;
            CnLayThrow t;
            seat_throw(&p, &P, 0, CN_THROW_CUP, &t);
            k = dmin(k, fit_reach(&P, &t.t));
        }
        L->my_reach = (float)k;
    }
    /* THE PLATE AND MY THROW (package S): the plate is flat on the glass, over the picture, so my held cup
     * passing under it is hidden by it; where my throw's cup, at its reach, comes within a point of the plate,
     * the host stands the plate down while my throw is in the air */
    if (fit && nd && !row1 && !list && L->has_plate) {
        CnLayThrow t;
        seat_throw(in, L, 0, CN_THROW_CUP, &t);
        Reach r = reach_none();
        throw_reach(L, &t.t, L->my_reach, &r);
        L->plate_throw = (uint8_t)over_rect(&r, L->plate, 1);
    }
    /* WHO THROWS (package V2). Mine, unless it is the reveal (one row or the list). On a tall board
     * each other seat's whose held cup stays CN_LAY_EDGE inside the drawer at the
     * study's reach, through the whole of its own throw (its seat, size and shake on
     * this screen); a far cup held as a throw holds it rises about four of its radii
     * up the screen, and on a tall board a top or side seat's study throw left the
     * drawer by 25 to 110 points, so it stays down, as every far cup does on a short
     * board (a cup that stays down hides its dice as a thrown one does). */
    if (nd && !row1 && !list) L->throw_mask = (uint8_t)(1 << me);
    if (fit && !shrt && !list)
        for (int v = 1; v < in->seats; v++) {
            const int i = (me + v) % in->seats;
            if ((in->out_mask >> i & 1) || !in->dice[i]) continue;
            CnLayThrow t;
            seat_throw(in, L, v, CN_THROW_CUP, &t);
            Reach r = reach_none();
            throw_reach(L, &t.t, 1, &r);
            if (reach_inside(&r, L->w)) L->throw_mask |= (uint8_t)(1 << i);
        }
    return 1;
}

int cn_lay_objects(const CnLayIn *in, const CnLay *L, CnObj *objs, int cap)
{
    const int n = L->seats, me = L->me;
    int k = 0;
    for (int j = 0; j < n - 1; j++) {
        const int i = (me + 1 + j) % n, out = in->out_mask >> i & 1;
        if (!out) for (int f = 0; f < in->dice[i]; f++) {
            double x, y, yaw;
            if (k >= cap) return -1;
            die_spot(i, f, L->cup_x[i], L->cup_y[i], L->sring, &x, &y, &yaw);
            cn_geom_die_obj(&objs[k], L->sd, 1, (uint32_t)(60 + i * 10 + f), (float)x, (float)y, (float)yaw);
            objs[k].seat = (uint8_t)i; k++;
        }
        if (k >= cap) return -1;
        /* every cup has an inside: a far cup is looked into while it is shaken */
        cn_geom_cup_obj(&objs[k], L->cup_r, in->dice[i], (uint32_t)(i * 7), L->cup_x[i], L->cup_y[i], out, (float)(L->board_w / 2));
        objs[k].seat = (uint8_t)i; objs[k].ring = L->sring; k++;
    }
    for (int f = 0; f < in->dice[me]; f++) {
        double x, y, yaw;
        if (k >= cap) return -1;
        die_spot(me, f, L->cup_x[me], L->cup_y[me], L->ring, &x, &y, &yaw);
        cn_geom_die_obj(&objs[k], L->d, in->my_faces[f], (uint32_t)(60 + me * 10 + f), (float)x, (float)y, (float)yaw);
        objs[k].seat = (uint8_t)me; objs[k].mine = 1; k++;
    }
    if (k >= cap) return -1;
    CnObj *c = &objs[k++];
    cn_geom_cup_obj(c, L->my_r, in->dice[me], 3, L->cup_x[me], L->cup_y[me], 0, 0);
    c->seat = (uint8_t)me; c->mine = 1; c->ring = L->ring;
    c->tilt_angle = L->peek.angle; c->tilt_hinge_y = L->peek.hinge_y; c->tilt_back = L->peek.back; c->tilt_lift = L->peek.lift;
    return k;
}

int cn_lay_throws(const CnLayIn *in, const CnLay *L, int kind, CnLayThrow *out, int cap)
{
    const int n = L->seats, me = L->me;
    int k = 0;
    for (int v = 0; v < n; v++) {
        const int i = (me + v) % n;
        /* who throws is the layout's (throw_mask); the table roll is mine only */
        if (!(L->throw_mask >> i & 1) || (v > 0 && kind != CN_THROW_CUP)) continue;
        if (k >= cap) return -1;
        seat_throw(in, L, v, kind, &out[k++]);
    }
    return k;
}

float cn_lay_lift_fit(const CnLay *L, const CnObj *cup, float full)
{
    if (!(full > 0)) return 0;
    /* the stage tips by the fitted angle as its full one (back and lift with it), the whole way up */
    Reach r = reach_none();
    tipped_reach(L, cup, full, &r);
    if (reach_inside(&r, L->w) && !(L->has_plate && over_rect(&r, L->plate, 0))) return full;
    float lo = 0, hi = full;
    for (int i = 0; i < 14; i++) {
        const float m = (lo + hi) / 2;
        r = reach_none(); tipped_reach(L, cup, m, &r);
        if (reach_inside(&r, L->w) && !(L->has_plate && over_rect(&r, L->plate, 0))) lo = m; else hi = m;
    }
    return lo;
}

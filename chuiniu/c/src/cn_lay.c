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

/* ---- what the eye sees of a cup, on the glass ------------------------------------- */

/* the drawer's points a picture reaches: its top, its left and its right */
typedef struct { double y0, x0, x1; } Reach;
static Reach reach_none(void) { Reach r = { 1e30, 1e30, -1e30 }; return r; }
static int reach_inside(const Reach *r, double w) { return r->y0 >= CN_LAY_EDGE && r->x0 >= CN_LAY_EDGE && r->x1 <= w - CN_LAY_EDGE; }

/* A cup placed as o, as cn_geom_emit places its mesh: the mouth's rim and the
 * crown's (the mesh's own corners; the fillet and the inside lie within them),
 * through the eye, onto the board's place in the drawer, through the turn. */
static void cup_reach(const CnLay *L, const CnObj *o, Reach *r)
{
    const double R = o->R, rc = R * CN_CUP_RC, h = o->h, ta = o->tilt_angle;
    const double ct = cn_m_cos(ta), st = cn_m_sin(ta), cy = cn_m_cos(o->yaw), sy = cn_m_sin(o->yaw);
    for (int i = 0; i < CN_CUP_SEGS; i++) {
        const double a = (double)i / CN_CUP_SEGS * CN_PI * 2, ca = cn_m_cos(a), sa = cn_m_sin(a);
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
            if (gx < r->x0) r->x0 = gx;
            if (gx > r->x1) r->x1 = gx;
        }
    }
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
 * toward the centre as far as it must to stay on the screen. */
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
        if (ry > 2 * rx) { ry = 2 * rx; for (int j = 0; j < n - 1; j++) ys[j] = mcy - ry * (1 - cn_m_cos(ang[j])); }
        double sx[MAXS], sy[MAXS];
        for (int j = 0; j < n - 1; j++) { int i = (me + 1 + j) % n; sx[i] = mcx - cn_m_sin(ang[j]) * rx; sy[i] = ys[j]; }
        /* a seat that is out has its cup lying across it, its height along the table */
        const double Ll = hc / (hc - R);
        for (int j = 0; j < n - 1; j++) {
            int i = (me + 1 + j) % n;
            if (!(in->out_mask >> i & 1)) continue;
            float sm;
            cn_cam_to_screen(cam, (float)(sy[i] - mcy), &sm);
            const double over = (dabs(sx[i] - mcx) + h / 2 + CN_CUP_RC * R) * Ll - (half / sm - 4);
            const double sg = sx[i] > mcx ? 1 : sx[i] < mcx ? -1 : 0;
            if (over > 0) sx[i] = sx[i] - sg * over / Ll;
        }
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
static void row_seats(const CnLayIn *in, CnLay *L, double W, double H, double myR, double mcy)
{
    const int n = in->seats, me = in->me, others = n - 1;
    const double step = dmin(CN_LAY_BADGE + CN_LAY_MARGIN, (W - 2 * CN_LAY_MARGIN) / others), bandTop = H - CN_LAY_SHORT_BAND;
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

static int valid(const CnLayIn *in)
{
    if (in->seats < 2 || in->seats > CN_LAY_SEATS || in->me >= in->seats || in->turn >= in->seats) return 0;
    for (int s = 0; s < in->seats; s++) if (in->dice[s] > CN_LAY_DICE) return 0;
    for (int k = 0; k < in->dice[in->me]; k++) if (in->my_faces[k] < 1 || in->my_faces[k] > 6) return 0;
    if (!(in->w > 2 * CN_LAY_SIDE + 4 * CN_LAY_MARGIN) || !(in->h > 0)) return 0;
    return 1;
}

/* my throw as cn_lay_throws writes it, before its reach */
static void my_throw(const CnLayIn *in, const CnLay *L, int kind, CnThrow *t)
{
    const int me = in->me;
    cn_throw_default(t, kind, L->cup_x[me], L->cup_y[me], L->my_r, L->d, L->ring);
    t->dice = in->dice[me];
}

static int make(const CnLayIn *in, CnLay *L, int fit);

int cn_lay_make(const CnLayIn *in, CnLay *L) { return make(in, L, 1); }

static int make(const CnLayIn *in, CnLay *L, int fit)
{
    memset(L, 0, sizeof *L);
    if (!in || !valid(in)) return 0;
    const double W = in->w, H = in->h, inner = W - 2 * CN_LAY_SIDE;
    const int shelf = in->turn == in->me || in->rolling;
    const double shelf_h = in->rolling ? CN_LAY_ROLL_H : CN_LAY_PICKER_H;
    /* short or tall reads the drawer: the board it has with the picker up. A
     * short board IS that board on every screen, so nothing on it moves when
     * the turn comes round, and the room the picker takes on my turn is the
     * room my cup's throw comes down through on theirs. */
    const double topM = H > 400 ? 30 : 8, pickerBoard = H - topM - 12 - (CN_LAY_PICKER_H + 10);
    const int shrt = pickerBoard < CN_LAY_SHORT_H;
    const double boardH = shrt ? pickerBoard : H - topM - 12 - (shelf ? shelf_h + 10 : 0);
    if (!(boardH > CN_LAY_SHORT_BAND)) return 0;
    const double band = shrt ? CN_LAY_SHORT_BAND : CN_LAY_MY_BAND;
    const double mcx = inner / 2, mcy = boardH - band + (shrt ? 30 : 34);
    const double hudB = shrt ? 0 : CN_LAY_HUD_TOP + CN_LAY_PLATE_H + 6;

    L->w = (float)W; L->h = (float)H;
    L->board_x = CN_LAY_SIDE; L->board_y = (float)topM; L->board_w = (float)inner; L->board_h = (float)boardH;
    L->top_m = (float)topM; L->seats = in->seats; L->me = in->me; L->short_board = (uint8_t)shrt;
    L->my_band[0] = CN_LAY_MARGIN; L->my_band[1] = (float)(boardH - band); L->my_band[2] = (float)(inner - 2 * CN_LAY_MARGIN); L->my_band[3] = (float)band;
    L->pad_below = CN_LAY_PAD_BELOW;
    cn_cam_make(&L->cam, (float)inner, (float)boardH, (float)(CN_LAY_SIDE + mcx), (float)(topM + mcy), 1);

    /* my five sit in a ring that keeps turned dice from touching (centre
     * spacing 1.7 of a side; the diagonal is 1.41), under a cup whose mouth
     * just covers the ring. On a short board the dice are the study's 16, then
     * half a point smaller at a time until my cup, tipped as far as any peek
     * tips it (its farthest die at the ring's top, pushed out by the most the
     * jitter does), stays inside the drawer: tipped about the far edge of its
     * mouth, its crown swings up the screen, past the top under a 310 drawer. */
    double d = shrt ? 16 : 24, ring, myR;
    for (;; d -= .5) {
        ring = 1.7 * d / (2 * cn_m_sin(CN_PI / 5)); myR = ring + d * .8 + 5;
        if (!shrt || d - .5 < CN_LAY_MY_D_MIN) break;
        const float fy = (float)(mcy - ring * 1.05), fd = (float)d;
        const float a = cn_cam_peek_angle(&L->cam, (float)myR, (float)mcy, &fy, &fd, 1);
        const CnPeek p = cn_cam_peek_tilt((float)myR, a, a);
        CnObj o;
        cn_geom_cup_obj(&o, (float)myR, 0, 3, (float)mcx, (float)mcy, 0, 0);
        o.tilt_angle = p.angle; o.tilt_hinge_y = p.hinge_y; o.tilt_back = p.back; o.tilt_lift = p.lift;
        Reach r = reach_none();
        cup_reach(L, &o, &r);
        if (reach_inside(&r, W)) break;
    }
    L->d = (float)d; L->ring = (float)ring; L->my_r = (float)myR;

    /* the HUD: the plate at the top of the glass on a tall board, beside the row
     * on a short one (not drawn when the row leaves it under 100); the shelf at
     * the drawer's foot. Both flat. */
    if (shrt) {
        const double rowEnd = CN_LAY_MARGIN + (in->seats - 1) * (CN_LAY_BADGE + CN_LAY_MARGIN);
        const double w = dmin(CN_LAY_PLATE_W, inner - CN_LAY_MARGIN - rowEnd);
        if (w >= CN_LAY_PLATE_MIN) {
            L->has_plate = 1;
            L->plate[0] = (float)(CN_LAY_SIDE + inner - CN_LAY_MARGIN - w);
            L->plate[1] = (float)(topM + dmax(0, (boardH - CN_LAY_SHORT_BAND - CN_LAY_PLATE_H) / 2));
            L->plate[2] = (float)w; L->plate[3] = CN_LAY_PLATE_H;
        }
    } else {
        L->has_plate = 1;
        L->plate[0] = (float)(W / 2 - CN_LAY_PLATE_W / 2); L->plate[1] = CN_LAY_HUD_TOP;
        L->plate[2] = CN_LAY_PLATE_W; L->plate[3] = CN_LAY_PLATE_H;
    }
    if (shelf) {
        L->has_shelf = 1;
        L->shelf[0] = 0; L->shelf[1] = (float)(H - shelf_h); L->shelf[2] = (float)W; L->shelf[3] = (float)shelf_h;
    }

    if (shrt) row_seats(in, L, inner, boardH, myR, mcy);
    else ring_seats(in, L, inner, boardH, myR, mcx, mcy, hudB);
    const int me = in->me;
    L->cup_x[me] = (float)mcx; L->cup_y[me] = (float)mcy;
    if (shrt) { L->name_x[me] = (float)(mcx - (myR + 64)); L->name_y[me] = (float)(boardH - band / 2); L->name_how[me] = CN_NAME_LEFT; }
    else { L->name_x[me] = (float)mcx; L->name_y[me] = (float)(boardH - 4); L->name_how[me] = CN_NAME_FOOT; }
    L->sd = (float)(d * L->cup_r / myR);
    L->sring = (float)(1.7 * L->sd / (2 * cn_m_sin(CN_PI / 5)));

    /* the peek: the least tip that shows every one of my dice from the eye */
    float dy[CN_LAY_DICE], dd[CN_LAY_DICE];
    int nd = in->dice[me];
    for (int k = 0; k < nd; k++) { double x, y, yaw; die_spot(me, k, mcx, mcy, ring, &x, &y, &yaw); dy[k] = (float)y; dd[k] = (float)d; }
    L->peek_target = nd ? cn_cam_peek_angle(&L->cam, (float)myR, (float)mcy, dy, dd, nd) : 0;
    float pk = in->peek < 0 ? 0 : in->peek > 1 ? 1 : in->peek;
    L->peek = cn_cam_peek_tilt((float)myR, L->peek_target * pk, L->peek_target);

    /* a die's side on the glass at each seat (the turn's own scale there), and its brass ring at the reveal */
    for (int s = 0; s < in->seats; s++) {
        const double side = s == me ? d : L->sd, x = L->board_x + L->cup_x[s], y = L->board_y + L->cup_y[s];
        float ax, ay, bx, by;
        cn_cam_map(&L->cam, (float)(x - side / 2), (float)y, &ax, &ay);
        cn_cam_map(&L->cam, (float)(x + side / 2), (float)y, &bx, &by);
        L->die_g[s] = (float)hyp(bx - ax, by - ay);
        L->brass_r[s] = (float)(L->die_g[s] * CN_LAY_BRASS);
    }

    /* my throw's reach, fitted on this drawer's picker-up board (the tightest), so it is one throw on every screen */
    L->my_reach = 1;
    if (fit && nd) {
        CnLayIn p = *in;
        p.turn = p.me; p.rolling = 0;
        CnLay P;
        const int same = shelf && !in->rolling;
        const CnLayIn *fi = same ? in : &p;
        const CnLay *fl = same ? L : (make(&p, &P, 0) ? &P : L);
        if (fl == L) fi = in;
        CnThrow t;
        my_throw(fi, fl, CN_THROW_CUP, &t);
        L->my_reach = (float)fit_reach(fl, &t);
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
    const double W = L->board_w, H = L->board_h;
    const uint32_t seed = in->seed;
    int k = 0;
    for (int v = 0; v < n; v++) {
        const int i = (me + v) % n;
        /* on a short board the other seats' cups stay down: no room above the row for a held cup */
        if (v > 0 && (kind != CN_THROW_CUP || L->short_board || (in->out_mask >> i & 1) || !in->dice[i])) continue;
        if (v == 0 && !in->dice[i]) continue;
        if (k >= cap) return -1;
        CnLayThrow *t = &out[k++];
        memset(t, 0, sizeof *t);
        const double mcx = L->cup_x[i], mcy = L->cup_y[i];
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
    return k;
}

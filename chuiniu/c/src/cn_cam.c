/* Chui Niu - the camera. See cn_cam.h. The study's tiltOf, applyPeekB,
 * peekTilt, peekAngleFor and animatePeek's ease, in doubles. */
#include "cn_cam.h"
#include "cn_geom.h"
#include <string.h>

/* row-vector 4x4 product: out = a * b (a applied first) */
static void mul4(const double a[16], const double b[16], double out[16])
{
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        double s = 0;
        for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
        out[r * 4 + c] = s;
    }
}

void cn_cam_make(CnCam *c, float board_w, float board_h, float origin_x, float origin_y, float t)
{
    memset(c, 0, sizeof *c);
    const double W = board_w, H = board_h;
    const double hc = CN_CAM_HC + (CN_CAM_VIEW_HC - CN_CAM_HC) * t, down = CN_CAM_DOWN + (CN_CAM_VIEW_DOWN - CN_CAM_DOWN) * t;
    const double e0y = H / 2 + CN_CAM_DOWN * H, ey = H / 2 + down * H;
    /* the turn: the angle between straight down and the line from the eye to my
     * cup, less the seat's own. The study measures that line to origin_y less 8,
     * a SCREEN y, from an eye at a BOARD y: the board's top margin (30 on a tall
     * screen, 8 on a short one) is in it. That is the study's, kept. */
    const double py = origin_y - CN_CAM_ORIGIN_UP;
    const double phi0 = cn_m_atan2(e0y - py, CN_CAM_HC), phi = cn_m_atan2(ey - py, hc);
    const double D = cn_m_sqrt(hc * hc + (ey - py) * (ey - py)), theta = phi - phi0, zoom = 1 + (CN_CAM_ZOOM - 1) * t;
    c->board_w = board_w; c->board_h = board_h;
    c->eye_x = (float)(W / 2); c->eye_y = (float)ey; c->eye_z = (float)hc;
    c->origin_x = origin_x; c->origin_y = origin_y;
    c->theta = (float)theta; c->D = (float)D; c->zoom = (float)zoom; c->t = t;

    /* scale(zoom), then rotateX(theta), then the perspective (m34 = -1/D) */
    const double cs = cn_m_cos(theta), sn = cn_m_sin(theta);
    const double S[16] = { zoom, 0, 0, 0,  0, zoom, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    const double Rx[16] = { 1, 0, 0, 0,  0, cs, sn, 0,  0, -sn, cs, 0,  0, 0, 0, 1 };
    const double P[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, -1 / D,  0, 0, 0, 1 };
    const double To[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -(double)origin_x, -(double)origin_y, 0, 1 };
    const double Tb[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  origin_x, origin_y, 0, 1 };
    double SR[16], M[16], A[16], F[16];
    mul4(S, Rx, SR); mul4(SR, P, M);
    mul4(To, M, A); mul4(A, Tb, F);
    for (int i = 0; i < 16; i++) { c->ca[i] = (float)M[i]; c->ca_screen[i] = (float)F[i]; }
    /* the plane z = 0: x' = x m11 + y m21 + m41, y' = x m12 + y m22 + m42, w' = x m14 + y m24 + m44 */
    const double h[9] = { F[0], F[4], F[12],  F[1], F[5], F[13],  F[3], F[7], F[15] };
    for (int i = 0; i < 9; i++) c->h[i] = (float)h[i];
}

float cn_cam_to_screen(const CnCam *c, float dy, float *scale)
{
    const double th = c->theta, D = c->D, z = dy * cn_m_sin(th), sc = D / (D - z) * c->zoom;
    if (scale) *scale = (float)sc;
    return (float)(c->origin_y + dy * cn_m_cos(th) * sc);
}

float cn_cam_from_screen(const CnCam *c, float y)
{
    const double th = c->theta, D = c->D, sy = (y - (double)c->origin_y) / c->zoom;
    return (float)(sy * D / (D * cn_m_cos(th) + sy * cn_m_sin(th)));
}

void cn_cam_map(const CnCam *c, float x, float y, float *sx, float *sy)
{
    const float *h = c->h;
    const double X = (double)h[0] * x + (double)h[1] * y + h[2];
    const double Y = (double)h[3] * x + (double)h[4] * y + h[5];
    const double Wd = (double)h[6] * x + (double)h[7] * y + h[8];
    *sx = (float)(X / Wd); *sy = (float)(Y / Wd);
}

void cn_cam_project(const CnCam *c, float x, float y, float z, float *px, float *py)
{
    const double k = (double)c->eye_z / (c->eye_z - (double)z);
    *px = (float)(c->eye_x + (x - (double)c->eye_x) * k);
    *py = (float)(c->eye_y + (y - (double)c->eye_y) * k);
}

CnPeek cn_cam_peek_tilt(float R, float angle, float full)
{
    const double f = full != 0 ? full : angle != 0 ? angle : 1;
    CnPeek p;
    p.angle = angle; p.hinge_y = -R;
    p.back = (float)(R * CN_PEEK_BACK * angle / f);
    p.lift = (float)(R * CN_PEEK_LIFT * angle / f);
    return p;
}

float cn_cam_peek_angle(const CnCam *c, float R, float my_cy, const float *dice_y, const float *dice_d, int n)
{
    double far = 1e30;
    for (int i = 0; i < n; i++) { double e = dice_y[i] - dice_d[i] * .6; if (e < far) far = e; }
    const double ey = c->eye_y, hc = c->eye_z;
    for (int deg = 10; deg <= 90; deg++) {
        const double a = deg * CN_PI / 180, back = R * CN_PEEK_BACK, lift = R * CN_PEEK_LIFT;
        const double yr = my_cy - R - back + 2 * R * cn_m_cos(a), zr = 2 * R * cn_m_sin(a) + lift;
        const double yp = ey + (yr - ey) * hc / (hc - zr);
        if (yp <= far - 4) {
            const double t = a + CN_PEEK_EXTRA_DEG * CN_PI / 180;
            return (float)(t < CN_PI / 2 ? t : CN_PI / 2);
        }
    }
    return (float)(CN_PI / 2);
}

float cn_cam_peek_ease(float t)
{
    const double u = t < 0 ? 0 : t > 1 ? 1 : t, v = 1 - u;
    return (float)(1 - v * v * v * (1 - cn_m_sin(u * CN_PI) * .12));
}

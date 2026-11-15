/* ALT-CELLO geometry, see geom.h.  Simplifications (all deliberate):
 *  - smooth body outline (no corners, no purfling, no scroll volute detail);
 *  - plates have constant thickness, no graduation, no bass bar, no sound post;
 *  - the f-holes are straight capsule slots;
 *  - the pegbox is a solid block, the scroll a spiral ribbon (mesh) and a disc (inside test);
 *  - the neck/fingerboard stand on a heel block, the fingerboard overhangs the top;
 *  - the bow stick is a tube, not tested by geom_inside_pose.
 * C11 + libm, no randomness. */
#include "geom.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

/* ---- local constants (the public ones are in geom.h) ---- */
#define WL (GEOM_W_LOWER / 2)
#define WW (GEOM_W_WAIST / 2)
#define WU (GEOM_W_UPPER / 2)
#define R_STR 0.0008      /* m, nominal string radius used for clearances */
#define BR_BASE_H 0.084   /* m, string height over the plate at the bridge, arc base */
#define BR_ARC_A 0.006    /* m, bridge arc rise of the middle strings over the base */
#define BR_ARC_X0 0.020   /* m, bridge arc half width (zero crossing) */
#define NUT_ARC_A 0.0015  /* m, nut arc rise */
#define NUT_ARC_X0 0.012  /* m, nut arc half width */
#define FB_H_NUT 0.001    /* m, string clearance over the board at the nut */
#define FB_H_END 0.006    /* m, string clearance over the board at its end (5-6 mm) */
#define FB_THICK_NUT 0.010
#define FB_THICK_END 0.058 /* wedge: the board underside rises away from the string line so the end hangs about 3 cm over the top */
#define FB_HW_NUT 0.0125  /* m, fingerboard half width at the nut */
#define FB_HW_END 0.020   /* m, at the far end */
#define NUT_T 0.005       /* m, nut thickness along the string */
#define NECK_NW_NUT 0.0125 /* m, neck bar half width at the nut */
#define NECK_NW_ROOT 0.0225
#define NECK_ND_NUT 0.012 /* m, neck bar depth below the board at the nut */
#define NECK_ND_ROOT 0.040
#define HEEL_HW 0.0225
#define HEEL_ZBOT (-0.060)
#define PB_HW 0.014       /* m, pegbox half width */
#define PB_UBOT (-0.038)  /* m, pegbox bottom, local U */
#define SCROLL_CS (-(GEOM_PEGBOX_LEN + 0.028)) /* spiral centre, local S */
#define SCROLL_CU (-0.012)
#define SCROLL_R 0.028
#define SCROLL_HW 0.013
#define PEG_R 0.0065
#define PEG_HL 0.035
#define TP_Y0 0.06        /* tailpiece lower end */
#define TP_Y1 0.27        /* tailpiece upper end */
#define TP_HW0 0.025
#define TP_HW1 0.018
#define TP_LIFT 0.010     /* m, tailpiece bottom above the plate */
#define TP_T 0.010
#define TP_SP 0.0095      /* m, string spacing at the tailpiece */
#define ENDPIN_Z (-0.060)
#define ENDPIN_R 0.005
#define BOW_STICK_R0 0.0045
#define BOW_STICK_R1 0.0030
#define BOW_GRIP_EXT 0.09 /* stick extension beyond the frog, fraction of hair length */
#define FINGER_SEMI_A 0.008 /* fingertip semi axes (0.016 x 0.012 x 0.012 full) */
#define FINGER_SEMI_B 0.006
#define FINGER_SEMI_C 0.006
#define FINGER_SHAFT_R 0.0055
#define FINGER_SHAFT_L 0.07
#define STAND_R 0.008
#define STAND_BASE_R 0.14
#define STAND_BASE_T 0.008

static const double STR_R[4] = {0.0009, 0.0008, 0.0007, 0.0006};

/* ---- vectors ---- */
typedef struct { double x, y, z; } V;
static V vk(double x, double y, double z) { V v = {x, y, z}; return v; }
static V vadd(V a, V b) { return vk(a.x + b.x, a.y + b.y, a.z + b.z); }
static V vsub(V a, V b) { return vk(a.x - b.x, a.y - b.y, a.z - b.z); }
static V vmul(V a, double s) { return vk(a.x * s, a.y * s, a.z * s); }
static double vdot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V vcross(V a, V b) {
  return vk(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static double vlen(V a) { return sqrt(vdot(a, a)); }
static V vunit(V a) { double l = vlen(a); return l > 0 ? vmul(a, 1 / l) : a; }
static V vlerp(V a, V b, double t) { return vadd(vmul(a, 1 - t), vmul(b, t)); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double clamp01(double t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }
static double sq(double a) { return a * a; }

/* ---- outline and plates ---- */
static double cosint(double a, double b, double t) { /* zero slope at both ends */
  return a + (b - a) * (1 - cos(PI * t)) / 2;
}

double geom_outline_halfwidth(double y) {
  const double L = GEOM_BODY_LENGTH;
  if (y <= 0 || y >= L) return 0;
  if (y < GEOM_Y_LOWER) return WL * sqrt(1 - sq((GEOM_Y_LOWER - y) / GEOM_Y_LOWER));
  if (y < GEOM_Y_WAIST) return cosint(WL, WW, (y - GEOM_Y_LOWER) / (GEOM_Y_WAIST - GEOM_Y_LOWER));
  if (y < GEOM_Y_UPPER) return cosint(WW, WU, (y - GEOM_Y_WAIST) / (GEOM_Y_UPPER - GEOM_Y_WAIST));
  return WU * sqrt(1 - sq((y - GEOM_Y_UPPER) / (L - GEOM_Y_UPPER)));
}

static int inside_outline(double x, double y, double t) {
  if (y <= t || y >= GEOM_BODY_LENGTH - t) return 0;
  return fabs(x) < geom_outline_halfwidth(y) - t;
}

/* arching profile in [0,1]: product of cosines across and along, zero on the outline */
static double arch_f(double x, double y) {
  double w = geom_outline_halfwidth(y);
  if (w <= 0) return 0;
  double q = fabs(x) / w;
  if (q >= 1) return 0;
  double v = 2 * y / GEOM_BODY_LENGTH - 1;
  return cos(PI / 2 * q) * cos(PI / 2 * v);
}
double geom_top_z(double x, double y) { return GEOM_ARCH_TOP * arch_f(x, y); }
static double back_z(double x, double y) { return -GEOM_RIB_H - GEOM_ARCH_BACK * arch_f(x, y); }

/* f-hole slot (capsule), sgn = -1 bass side, +1 treble side */
static int in_slot_one(double x, double y, int sgn) {
  double dx = x - sgn * GEOM_FH_CX, dy = y - GEOM_FH_CY;
  double ax = -sgn * sin(GEOM_FH_ANG), ay = cos(GEOM_FH_ANG);
  double t = dx * ax + dy * ay;
  double p = fabs(-dx * ay + dy * ax);
  double h = GEOM_FH_LEN / 2 - GEOM_FH_W / 2;
  double a = fabs(t) - h;
  if (a < 0) a = 0;
  return a * a + p * p <= sq(GEOM_FH_W / 2);
}
static int in_slot(double x, double y) { return in_slot_one(x, y, -1) || in_slot_one(x, y, 1); }
int geom_fhole_xy(double x, double y) { return in_slot(x, y); }

static int body_part(double x, double y, double z, int slot) {
  if (y <= 0 || y >= GEOM_BODY_LENGTH) return 0;
  if (fabs(x) >= geom_outline_halfwidth(y)) return 0;
  double f = arch_f(x, y);
  double zt = GEOM_ARCH_TOP * f, zb = -GEOM_RIB_H - GEOM_ARCH_BACK * f;
  if (z > zt || z < zb) return 0;
  if (!inside_outline(x, y, GEOM_T_RIB)) return GEOM_RIBS;
  if (z >= zt - GEOM_T_TOP) return (slot && in_slot(x, y)) ? 0 : GEOM_BODY_TOP;
  if (z <= zb + GEOM_T_BACK) return GEOM_BODY_BACK;
  return 0;
}

int geom_cavity(double x, double y, double z) {
  if (!inside_outline(x, y, GEOM_T_RIB)) return 0;
  double f = arch_f(x, y);
  return z < GEOM_ARCH_TOP * f - GEOM_T_TOP && z > -GEOM_RIB_H - GEOM_ARCH_BACK * f + GEOM_T_BACK;
}

/* ---- bridge, strings, neck frame ---- */
static double plate_ref(void) { return geom_top_z(0, GEOM_BRIDGE_Y); }
static double str_xb(int i) { return (i - 1.5) * GEOM_SP_BRIDGE; }
static double str_xn(int i) { return (i - 1.5) * GEOM_SP_NUT; }
static double arc_a(double x, double A, double X0) { return A * (1 - sq(x / X0)); }

static V bridge_pt(int i) {
  double x = str_xb(i);
  return vk(x, GEOM_BRIDGE_Y, plate_ref() + BR_BASE_H + arc_a(x, BR_ARC_A, BR_ARC_X0));
}
static V dirD(void) { return vk(0, cos(GEOM_NECK_ANGLE), sin(GEOM_NECK_ANGLE)); } /* bridge to nut */
static V dirE(void) { return vk(0, -sin(GEOM_NECK_ANGLE), cos(GEOM_NECK_ANGLE)); }
static V nut_ref(void) {
  V b = vk(0, GEOM_BRIDGE_Y, plate_ref() + BR_BASE_H);
  return vadd(b, vmul(dirD(), GEOM_STRING_L));
}
static V nut_pt(int i) {
  V B = bridge_pt(i), R = nut_ref();
  double x = str_xn(i), z = R.z + arc_a(x, NUT_ARC_A, NUT_ARC_X0);
  double dx = x - B.x, dz = z - B.z;
  double dy = sqrt(sq(GEOM_STRING_L) - dx * dx - dz * dz); /* exact string length */
  return vk(x, B.y + dy, z);
}
static V tail_pt(int i) {
  double x = (i - 1.5) * TP_SP;
  return vk(x, TP_Y1 - 0.005, geom_top_z(0, TP_Y1) + TP_LIFT + TP_T + 0.0005);
}

/* local board frame: X across, S from the nut toward the bridge, U along E */
static V loc2w(double X, double S, double U) {
  V n = nut_ref();
  return vadd(vadd(vadd(n, vmul(dirD(), -S)), vmul(dirE(), U)), vk(X, 0, 0));
}
static void w2loc(V p, double *X, double *S, double *U) {
  V r = vsub(p, nut_ref());
  *X = p.x;
  *S = -vdot(r, dirD());
  *U = vdot(r, dirE());
}
static double env(double X, double S) {
  double t = S / GEOM_STRING_L;
  return arc_a(X, lerp(NUT_ARC_A, BR_ARC_A, t), lerp(NUT_ARC_X0, BR_ARC_X0, t));
}
static double envslope(double X, double S) {
  double t = S / GEOM_STRING_L;
  double A = lerp(NUT_ARC_A, BR_ARC_A, t), X0 = lerp(NUT_ARC_X0, BR_ARC_X0, t);
  return -2 * A * X / (X0 * X0);
}
static double fb_h(double S) { return lerp(FB_H_NUT, FB_H_END, S / GEOM_FB_LEN); }
static double fb_top(double X, double S) { return env(X, S) - fb_h(S); }
static double fb_flat(double S) { return fb_top(0, S) - lerp(FB_THICK_NUT, FB_THICK_END, S / GEOM_FB_LEN); }
static double fb_hw(double S) { return lerp(FB_HW_NUT, FB_HW_END, S / GEOM_FB_LEN); }
static double s_root(void) { return (nut_ref().y - GEOM_BODY_LENGTH) / cos(GEOM_NECK_ANGLE); }
static double neck_nw(double S) { return lerp(NECK_NW_NUT, NECK_NW_ROOT, S / s_root()); }
static double neck_nd(double S) { return lerp(NECK_ND_NUT, NECK_ND_ROOT, S / s_root()); }
static double heel_ztop(void) { return loc2w(0, s_root(), fb_flat(s_root()) - NECK_ND_ROOT).z + 0.004; }

double geom_neck_projection(void) { /* board plane extended to the bridge, above the plate at the bridge */
  return loc2w(0, GEOM_STRING_L, fb_top(0, GEOM_STRING_L)).z - plate_ref();
}
double geom_fb_end_gap(void) { /* underside of the board end over the top plate */
  V p = loc2w(0, GEOM_FB_LEN, fb_flat(GEOM_FB_LEN));
  return p.z - geom_top_z(0, p.y);
}

void geom_string_endpoints(int s, double nut[3], double bridge[3]) {
  V n = nut_pt(s), b = bridge_pt(s);
  nut[0] = n.x; nut[1] = n.y; nut[2] = n.z;
  bridge[0] = b.x; bridge[1] = b.y; bridge[2] = b.z;
}

/* bridge profile */
static double bridge_top(double x) { return plate_ref() + BR_BASE_H + arc_a(x, BR_ARC_A, BR_ARC_X0) + 0.0005; }
static double bridge_hw(double zfrac) { return lerp(GEOM_BRIDGE_W / 2, GEOM_BRIDGE_WTOP / 2, clamp01(zfrac)); }
static double bridge_zfrac(double z) { return (z - plate_ref() + 0.004) / (BR_BASE_H + 0.004 - 0.015); }

static double tp_hw(double y) { return lerp(TP_HW0, TP_HW1, (y - TP_Y0) / (TP_Y1 - TP_Y0)); }
static double tp_zb(double y) { return geom_top_z(0, y) + TP_LIFT; }

static double seg_dist(V p, V a, V b) {
  V ab = vsub(b, a), ap = vsub(p, a);
  double t = clamp01(vdot(ap, ab) / vdot(ab, ab));
  return vlen(vsub(p, vadd(a, vmul(ab, t))));
}

/* ---- pose frames ---- */
void geom_pose_default(GeomPose *p) {
  p->bow_string = 2;
  p->bow_beta = 0.1;
  p->bow_angle = 0.0;
  p->bow_tilt = 0.35;
  p->bow_u = 0.5;
  p->finger_string = 3;
  p->finger_dist = GEOM_STRING_L * (1 - pow(2.0, -7.0 / 12.0));
  p->finger2_string = -1;
  p->finger2_dist = 0.2;
  p->finger2_lift = 0.03;
  p->mic[0] = 0.0; p->mic[1] = 0.45; p->mic[2] = 1.0;
}

typedef struct { V P, Nc, hd, wv, m, dn; } BowFrame;
static BowFrame bow_frame(const GeomPose *p) {
  BowFrame f;
  int i = p->bow_string;
  V B = bridge_pt(i), N = nut_pt(i);
  V d = vunit(vsub(N, B)); /* toward the nut */
  f.dn = d;
  f.P = vadd(B, vmul(d, p->bow_beta * GEOM_STRING_L));
  double S = GEOM_STRING_L * (1 - p->bow_beta), X = f.P.x;
  double s = envslope(X, S);
  V u1 = vk(1, 0, 0), u2 = vunit(vsub(dirE(), vmul(d, vdot(dirE(), d))));
  f.Nc = vunit(vadd(vmul(u1, -s), u2));
  V T = vunit(vadd(u1, vmul(u2, s)));
  f.hd = vunit(vsub(vmul(vadd(vmul(T, cos(p->bow_angle)), vmul(d, sin(p->bow_angle))), -1), vk(0, 0, 0)));
  f.wv = vunit(vcross(f.Nc, f.hd));
  V w1 = vunit(vsub(d, vmul(f.hd, vdot(d, f.hd))));
  f.m = vunit(vadd(vmul(f.Nc, cos(p->bow_tilt)), vmul(w1, sin(p->bow_tilt))));
  return f;
}
void geom_bow_contact(const GeomPose *p, double out[3]) {
  BowFrame f = bow_frame(p);
  out[0] = f.P.x; out[1] = f.P.y; out[2] = f.P.z;
}
static V hair_pt(const BowFrame *f, const GeomPose *p, double u) {
  V c = vadd(f->P, vmul(f->Nc, R_STR + 0.00075));
  return vadd(c, vmul(f->hd, (u - p->bow_u) * GEOM_HAIR_L));
}
static double stick_h(double u) {
  if (u < 0) return 0.040 + 0.012 * (-u) / BOW_GRIP_EXT;
  return 0.040 * (1 - u) + 0.025 * u - 0.020 * 4 * u * (1 - u);
}

typedef struct { V press, open, ctr, up, ay; } FingerFrame;
static FingerFrame finger_frame(int s, double dist, double lift) {
  FingerFrame f;
  V B = bridge_pt(s), N = nut_pt(s);
  double t = dist / GEOM_STRING_L;
  f.open = vlerp(N, B, t);
  double X = f.open.x, S = dist;
  f.press = loc2w(X, S, fb_top(X, S) + R_STR);
  f.up = dirE();
  f.ay = dirD();
  f.ctr = vadd(f.press, vmul(f.up, R_STR + FINGER_SEMI_C + lift));
  if (lift > 0) f.ctr = vadd(f.open, vmul(f.up, R_STR + FINGER_SEMI_C + lift));
  return f;
}
void geom_finger_point(const GeomPose *p, double out[3]) {
  if (p->finger_string < 0) { out[0] = out[1] = out[2] = 0; return; }
  FingerFrame f = finger_frame(p->finger_string, p->finger_dist, 0);
  out[0] = f.press.x; out[1] = f.press.y; out[2] = f.press.z;
}

static V mic_aim(const GeomPose *p) {
  return vunit(vsub(vk(0, GEOM_BRIDGE_Y, 0.05), vk(p->mic[0], p->mic[1], p->mic[2])));
}

/* ---- inside tests ---- */
static int inside_static(double x, double y, double z) {
  int b = body_part(x, y, z, 1);
  if (b) return b;
  /* bridge */
  if (fabs(y - GEOM_BRIDGE_Y) <= GEOM_BRIDGE_T / 2 && fabs(x) < GEOM_BRIDGE_W / 2 &&
      z >= geom_top_z(x, GEOM_BRIDGE_Y) - 0.0005 && z <= bridge_top(x) &&
      fabs(x) <= bridge_hw(bridge_zfrac(z)))
    return GEOM_BRIDGE;
  /* tailpiece and endpin */
  if (y >= TP_Y0 && y <= TP_Y1 && fabs(x) <= tp_hw(y) && z >= tp_zb(y) && z <= tp_zb(y) + TP_T)
    return GEOM_TAILPIECE;
  if (y <= 0 && y >= -GEOM_ENDPIN_LEN && sq(x) + sq(z - ENDPIN_Z) <= sq(ENDPIN_R)) return GEOM_ENDPIN;
  /* heel block under the neck */
  if (fabs(x) <= HEEL_HW && y >= GEOM_BODY_LENGTH - 0.035 && y <= GEOM_BODY_LENGTH + 0.005 &&
      z >= HEEL_ZBOT && z <= heel_ztop())
    return GEOM_NECK;
  /* strings (cheap reject first) */
  if (fabs(x) < 0.03 && z > 0.02 && y > 0.2) {
    V p = vk(x, y, z);
    for (int i = 0; i < 4; i++)
      if (seg_dist(p, nut_pt(i), bridge_pt(i)) <= STR_R[i] ||
          seg_dist(p, bridge_pt(i), tail_pt(i)) <= STR_R[i])
        return GEOM_STRING_C + i;
  }
  /* neck and fingerboard in the local frame */
  if (y > 0.4) {
    double X, S, U;
    w2loc(vk(x, y, z), &X, &S, &U);
    if (S >= 0 && S <= GEOM_FB_LEN && fabs(X) <= fb_hw(S) && U <= fb_top(X, S) && U >= fb_flat(S))
      return GEOM_FINGERBOARD;
    if (S >= -NUT_T && S < 0 && fabs(X) <= FB_HW_NUT && U >= -0.012 && U <= env(X, 0))
      return GEOM_FINGERBOARD;
    if (S >= 0 && S <= s_root()) {
      double dU = fb_flat(S) - U, nw = neck_nw(S), nd = neck_nd(S);
      if (dU > 0 && dU < nd && sq(X / nw) + sq(dU / nd) < 1) return GEOM_NECK;
    }
    if (S < -NUT_T && S >= -GEOM_PEGBOX_LEN && fabs(X) <= PB_HW && U >= PB_UBOT && U <= fb_top(0, 0) + 0.0005)
      return GEOM_NECK;
    if (S < -GEOM_PEGBOX_LEN + 0.01 && sq(S - SCROLL_CS) + sq(U - SCROLL_CU) <= sq(SCROLL_R) && fabs(X) <= SCROLL_HW)
      return GEOM_SCROLL;
    for (int k = 0; k < 4; k++) {
      double ps = -0.03 - 0.035 * k, pu = -0.019 + ((k & 1) ? 0.008 : -0.008);
      if (fabs(X) <= PEG_HL && sq(S - ps) + sq(U - pu) <= sq(PEG_R)) return GEOM_SCROLL;
    }
  }
  return 0;
}
int geom_inside(double x, double y, double z) { return inside_static(x, y, z); }

int geom_inside_pose(const GeomPose *p, double x, double y, double z) {
  V pt = vk(x, y, z);
  /* mic capsule and stand */
  V mc = vk(p->mic[0], p->mic[1], p->mic[2]), aim = mic_aim(p);
  V r = vsub(pt, mc);
  double t = vdot(r, aim);
  if (fabs(t) <= GEOM_MIC_L / 2 && vlen(vsub(r, vmul(aim, t))) <= GEOM_MIC_R) return GEOM_MIC;
  if (y < mc.y - 0.03 && y > -GEOM_ENDPIN_LEN && sq(x - mc.x) + sq(z - mc.z) <= sq(STAND_R)) return GEOM_MIC;
  if (y >= -GEOM_ENDPIN_LEN && y <= -GEOM_ENDPIN_LEN + STAND_BASE_T && sq(x - mc.x) + sq(z - mc.z) <= sq(STAND_BASE_R))
    return GEOM_MIC;
  /* bow hair */
  BowFrame f = bow_frame(p);
  V c = vadd(f.P, vmul(f.Nc, R_STR + 0.00075));
  V hm = vadd(c, vmul(f.hd, (0.5 - p->bow_u) * GEOM_HAIR_L));
  V q = vsub(pt, hm);
  if (fabs(vdot(q, f.hd)) <= GEOM_HAIR_L / 2 && fabs(vdot(q, f.wv)) <= GEOM_HAIR_W / 2 &&
      fabs(vdot(q, f.Nc)) <= 0.00075)
    return GEOM_BOW_HAIR;
  /* fingers */
  for (int k = 0; k < 2; k++) {
    int s = k ? p->finger2_string : p->finger_string;
    if (s < 0) continue;
    FingerFrame ff = finger_frame(s, k ? p->finger2_dist : p->finger_dist, k ? p->finger2_lift : 0);
    V d = vsub(pt, ff.ctr);
    if (sq(d.x / FINGER_SEMI_A) + sq(vdot(d, ff.ay) / FINGER_SEMI_B) + sq(vdot(d, ff.up) / FINGER_SEMI_C) <= 1)
      return GEOM_FINGER;
    V e = vadd(ff.ctr, vmul(ff.up, FINGER_SHAFT_L));
    if (seg_dist(pt, ff.ctr, e) <= FINGER_SHAFT_R) return GEOM_FINGER;
  }
  return inside_static(x, y, z);
}

/* cell supersampling: n samples per axis so that the thinnest wall (2 mm ribs) is always hit */
static int cell_scan(double cx, double cy, double cz, double dx, int *cnt, int *nslot) {
  int n = (int)ceil(dx / 0.002);
  if (n < 2) n = 2;
  if (n > 13) n = 13;
  memset(cnt, 0, GEOM_NPARTS * sizeof(int));
  *nslot = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      for (int k = 0; k < n; k++) {
        double x = cx + dx * ((i + 0.5) / n - 0.5), y = cy + dx * ((j + 0.5) / n - 0.5),
               z = cz + dx * ((k + 0.5) / n - 0.5);
        int p = inside_static(x, y, z);
        if (p) cnt[p]++;
        else if (body_part(x, y, z, 0) == GEOM_BODY_TOP) (*nslot)++;
      }
  /* the f-hole area bias: a cell whose plate column is at least a quarter slot is open */
  int open = *nslot > 0 && *nslot * 4 >= cnt[GEOM_BODY_TOP] + *nslot;
  if (open) cnt[GEOM_BODY_TOP] = 0;
  return open;
}

int geom_cell(double cx, double cy, double cz, double dx) {
  int cnt[GEOM_NPARTS], nslot;
  cell_scan(cx, cy, cz, dx, cnt, &nslot);
  int best = 0, bc = 0;
  for (int p = 1; p < GEOM_NPARTS; p++)
    if (cnt[p] > bc) { bc = cnt[p]; best = p; }
  return best;
}

int geom_cell_fhole(double cx, double cy, double cz, double dx) {
  int cnt[GEOM_NPARTS], nslot;
  int open = cell_scan(cx, cy, cz, dx, cnt, &nslot);
  if (!open) return 0;
  for (int p = 1; p < GEOM_NPARTS; p++)
    if (cnt[p]) return 0; /* something else solid in the cell wins */
  return 1;
}

void geom_to_room(const double in[3], double out[3]) {
  double x = in[0], y = in[1], z = in[2];
  out[0] = x;
  out[1] = GEOM_ROOM_CELLO_Y - z;
  out[2] = y + GEOM_ENDPIN_LEN;
}

static const char *NAMES[GEOM_NPARTS] = {"AIR", "BODY_TOP", "BODY_BACK", "RIBS", "FHOLE", "BRIDGE", "NECK",
                                         "FINGERBOARD", "SCROLL", "STRING_C", "STRING_G", "STRING_D",
                                         "STRING_A", "BOW_STICK", "BOW_HAIR", "FINGER", "MIC", "TAILPIECE",
                                         "ENDPIN"};
const char *geom_part_name(int part) { return (part >= 0 && part < GEOM_NPARTS) ? NAMES[part] : "?"; }

/* ---- mesh building ---- */
static void m_tri(GeomMesh *m, V a, V b, V c, int part) {
  if (m->n >= m->cap) {
    m->cap = m->cap ? m->cap * 2 : 4096;
    m->v = (float *)realloc(m->v, (size_t)m->cap * 9 * sizeof(float));
    m->part = (uint8_t *)realloc(m->part, (size_t)m->cap);
  }
  float *o = m->v + (size_t)m->n * 9;
  o[0] = (float)a.x; o[1] = (float)a.y; o[2] = (float)a.z;
  o[3] = (float)b.x; o[4] = (float)b.y; o[5] = (float)b.z;
  o[6] = (float)c.x; o[7] = (float)c.y; o[8] = (float)c.z;
  m->part[m->n++] = (uint8_t)part;
}
static void m_quad(GeomMesh *m, V a, V b, V c, V d, int part) {
  m_tri(m, a, b, c, part);
  m_tri(m, a, c, d, part);
}
void geom_mesh_free(GeomMesh *m) {
  free(m->v);
  free(m->part);
  memset(m, 0, sizeof *m);
}

static void perp_basis(V t, V *u, V *w) {
  V ref = fabs(t.x) < 0.9 ? vk(1, 0, 0) : vk(0, 1, 0);
  *u = vunit(vcross(t, ref));
  *w = vcross(t, *u);
}

/* tube along a polyline, parallel transported frame */
static void m_tube(GeomMesh *m, const V *pts, int n, double r0, double r1, int nseg, int part, int caps) {
  V *ring = (V *)malloc((size_t)n * nseg * sizeof(V));
  V u = vk(0, 0, 0), w = vk(0, 0, 0);
  for (int i = 0; i < n; i++) {
    V t = vunit(vsub(pts[i < n - 1 ? i + 1 : i], pts[i > 0 ? i - 1 : i]));
    if (i == 0) perp_basis(t, &u, &w);
    else {
      u = vunit(vsub(u, vmul(t, vdot(u, t))));
      w = vcross(t, u);
    }
    double r = lerp(r0, r1, n > 1 ? (double)i / (n - 1) : 0);
    for (int k = 0; k < nseg; k++) {
      double a = 2 * PI * k / nseg;
      ring[i * nseg + k] = vadd(pts[i], vadd(vmul(u, r * cos(a)), vmul(w, r * sin(a))));
    }
  }
  for (int i = 0; i + 1 < n; i++)
    for (int k = 0; k < nseg; k++) {
      int k2 = (k + 1) % nseg;
      m_quad(m, ring[i * nseg + k], ring[i * nseg + k2], ring[(i + 1) * nseg + k2], ring[(i + 1) * nseg + k], part);
    }
  if (caps)
    for (int k = 0; k < nseg; k++) {
      int k2 = (k + 1) % nseg;
      m_tri(m, pts[0], ring[k2], ring[k], part);
      m_tri(m, pts[n - 1], ring[(n - 1) * nseg + k], ring[(n - 1) * nseg + k2], part);
    }
  free(ring);
}
static void m_cyl(GeomMesh *m, V a, V b, double r, int nseg, int part) {
  V p[2] = {a, b};
  m_tube(m, p, 2, r, r, nseg, part, 1);
}
static void m_box(GeomMesh *m, V c, V ax, V ay, V az, double hx, double hy, double hz, int part) {
  V p[8];
  for (int i = 0; i < 8; i++)
    p[i] = vadd(c, vadd(vadd(vmul(ax, (i & 1) ? hx : -hx), vmul(ay, (i & 2) ? hy : -hy)), vmul(az, (i & 4) ? hz : -hz)));
  m_quad(m, p[0], p[1], p[3], p[2], part);
  m_quad(m, p[4], p[6], p[7], p[5], part);
  m_quad(m, p[0], p[4], p[5], p[1], part);
  m_quad(m, p[2], p[3], p[7], p[6], part);
  m_quad(m, p[0], p[2], p[6], p[4], part);
  m_quad(m, p[1], p[5], p[7], p[3], part);
}
static void m_ellipsoid(GeomMesh *m, V c, V a, V b, V d, double ra, double rb, double rd, int part) {
  enum { NU = 16, NV = 10 };
  V g[NV + 1][NU];
  for (int j = 0; j <= NV; j++) {
    double th = PI * j / NV;
    for (int i = 0; i < NU; i++) {
      double ph = 2 * PI * i / NU;
      g[j][i] = vadd(c, vadd(vadd(vmul(a, ra * sin(th) * cos(ph)), vmul(b, rb * sin(th) * sin(ph))),
                             vmul(d, rd * cos(th))));
    }
  }
  for (int j = 0; j < NV; j++)
    for (int i = 0; i < NU; i++) m_quad(m, g[j][i], g[j][(i + 1) % NU], g[j + 1][(i + 1) % NU], g[j + 1][i], part);
}
/* loft: nst rings of nk points each (closed ring), caps as fans */
static void m_loft(GeomMesh *m, const V *pts, int nst, int nk, int part) {
  for (int s = 0; s + 1 < nst; s++)
    for (int k = 0; k < nk; k++) {
      int k2 = (k + 1) % nk;
      m_quad(m, pts[s * nk + k], pts[s * nk + k2], pts[(s + 1) * nk + k2], pts[(s + 1) * nk + k], part);
    }
  for (int e = 0; e < 2; e++) {
    const V *r = pts + (e ? (nst - 1) * nk : 0);
    V c = vk(0, 0, 0);
    for (int k = 0; k < nk; k++) c = vadd(c, r[k]);
    c = vmul(c, 1.0 / nk);
    for (int k = 0; k < nk; k++) m_tri(m, c, r[k], r[(k + 1) % nk], part);
  }
}

static void build_plates(GeomMesh *m) {
  enum { NY = 90, NU = 28 };
  static V top[NY + 1][NU + 1], back[NY + 1][NU + 1];
  for (int j = 0; j <= NY; j++) {
    double y = GEOM_BODY_LENGTH * (1 - cos(PI * j / NY)) / 2;
    double w = geom_outline_halfwidth(y);
    for (int i = 0; i <= NU; i++) {
      double x = w * (2.0 * i / NU - 1);
      top[j][i] = vk(x, y, geom_top_z(x, y));
      back[j][i] = vk(x, y, back_z(x, y));
    }
  }
  for (int j = 0; j < NY; j++)
    for (int i = 0; i < NU; i++) {
      m_quad(m, top[j][i], top[j][i + 1], top[j + 1][i + 1], top[j + 1][i], GEOM_BODY_TOP);
      m_quad(m, back[j][i], back[j + 1][i], back[j + 1][i + 1], back[j][i + 1], GEOM_BODY_BACK);
    }
  /* ribs: strip along the outline */
  enum { NR = 160 };
  V rt[NR + 1], rb[NR + 1];
  for (int k = 0; k <= NR; k++) {
    /* right side going up, then left side coming down; cosine spacing in y */
    double s = (double)k / NR; /* 0..1 around */
    double yy, sgn;
    if (s < 0.5) { yy = 2 * s; sgn = 1; } else { yy = 2 * (1 - s); sgn = -1; }
    double y = GEOM_BODY_LENGTH * (1 - cos(PI * yy)) / 2;
    double x = sgn * geom_outline_halfwidth(y);
    rt[k] = vk(x, y, 0);
    rb[k] = vk(x, y, -GEOM_RIB_H);
  }
  for (int k = 0; k < NR; k++) m_quad(m, rt[k], rb[k], rb[k + 1], rt[k + 1], GEOM_RIBS);
  /* f-hole openings: dark decals on the top plate */
  for (int sg = -1; sg <= 1; sg += 2) {
    enum { NP = 24 };
    V ring[NP];
    double ax = -sg * sin(GEOM_FH_ANG), ay = cos(GEOM_FH_ANG);
    double h = GEOM_FH_LEN / 2 - GEOM_FH_W / 2, r = GEOM_FH_W / 2;
    for (int k = 0; k < NP; k++) {
      double a = 2 * PI * k / NP;
      double side = (k < NP / 2) ? h : -h;
      double px = sg * GEOM_FH_CX + side * ax + r * cos(a) * ay + r * sin(a) * ax;
      double py = GEOM_FH_CY + side * ay - r * cos(a) * ax + r * sin(a) * ay;
      ring[k] = vk(px, py, geom_top_z(px, py) + 0.0006);
    }
    V c = vk(sg * GEOM_FH_CX, GEOM_FH_CY, geom_top_z(sg * GEOM_FH_CX, GEOM_FH_CY) + 0.0006);
    for (int k = 0; k < NP; k++) m_tri(m, c, ring[k], ring[(k + 1) % NP], GEOM_FHOLE);
  }
}

static void build_bridge(GeomMesh *m) {
  enum { NK = 24 };
  V fb[NK + 1], ft[NK + 1], bb[NK + 1], bt[NK + 1];
  double hy = GEOM_BRIDGE_T / 2;
  for (int k = 0; k <= NK; k++) {
    double s = 2.0 * k / NK - 1;
    double xb = s * GEOM_BRIDGE_W / 2, xt = s * GEOM_BRIDGE_WTOP / 2;
    double zb = geom_top_z(xb, GEOM_BRIDGE_Y) - 0.0005, zt = bridge_top(xt);
    fb[k] = vk(xb, GEOM_BRIDGE_Y + hy, zb);
    ft[k] = vk(xt, GEOM_BRIDGE_Y + hy, zt);
    bb[k] = vk(xb, GEOM_BRIDGE_Y - hy, zb);
    bt[k] = vk(xt, GEOM_BRIDGE_Y - hy, zt);
  }
  for (int k = 0; k < NK; k++) {
    m_quad(m, fb[k], fb[k + 1], ft[k + 1], ft[k], GEOM_BRIDGE);
    m_quad(m, bb[k], bt[k], bt[k + 1], bb[k + 1], GEOM_BRIDGE);
    m_quad(m, ft[k], ft[k + 1], bt[k + 1], bt[k], GEOM_BRIDGE);
    m_quad(m, fb[k], bb[k], bb[k + 1], fb[k + 1], GEOM_BRIDGE);
  }
  m_quad(m, fb[0], ft[0], bt[0], bb[0], GEOM_BRIDGE);
  m_quad(m, fb[NK], bb[NK], bt[NK], ft[NK], GEOM_BRIDGE);
}

static void build_neck(GeomMesh *m) {
  enum { NS = 14, NX = 10 };
  /* fingerboard: ring of NX+1 top points (arc) plus 2 flat bottom corners */
  static V fbr[NS + 1][NX + 3];
  for (int s = 0; s <= NS; s++) {
    double S = GEOM_FB_LEN * s / NS, hw = fb_hw(S);
    for (int k = 0; k <= NX; k++) {
      double X = hw * (2.0 * k / NX - 1);
      fbr[s][k] = loc2w(X, S, fb_top(X, S));
    }
    fbr[s][NX + 1] = loc2w(hw, S, fb_flat(S));
    fbr[s][NX + 2] = loc2w(-hw, S, fb_flat(S));
  }
  m_loft(m, &fbr[0][0], NS + 1, NX + 3, GEOM_FINGERBOARD);
  /* nut */
  {
    V c = loc2w(0, -NUT_T / 2, (-0.012 + env(0, 0)) / 2);
    V ax = vk(1, 0, 0), ay = dirD(), az = dirE();
    m_box(m, c, ax, ay, az, FB_HW_NUT, NUT_T / 2, (env(0, 0) + 0.012) / 2, GEOM_FINGERBOARD);
  }
  /* neck bar: closed half ellipse under the board, from the nut to the heel */
  enum { NE = 12, NSN = 8 };
  static V nr[NSN + 1][NE + 1];
  double sr = s_root();
  for (int s = 0; s <= NSN; s++) {
    double S = sr * s / NSN, nw = neck_nw(S), nd = neck_nd(S), ub = fb_flat(S);
    for (int k = 0; k <= NE; k++) {
      double a = PI * k / NE; /* 0..pi, below the board */
      nr[s][k] = loc2w(nw * cos(a), S, ub - nd * sin(a));
    }
  }
  m_loft(m, &nr[0][0], NSN + 1, NE + 1, GEOM_NECK);
  /* heel block */
  {
    double zt = heel_ztop();
    V c = vk(0, GEOM_BODY_LENGTH - 0.015, (HEEL_ZBOT + zt) / 2);
    m_box(m, c, vk(1, 0, 0), vk(0, 1, 0), vk(0, 0, 1), HEEL_HW, 0.020, (zt - HEEL_ZBOT) / 2, GEOM_NECK);
  }
  /* pegbox */
  {
    double s0 = -NUT_T, s1 = -GEOM_PEGBOX_LEN, ut = fb_top(0, 0) + 0.0005;
    V c = loc2w(0, (s0 + s1) / 2, (ut + PB_UBOT) / 2);
    m_box(m, c, vk(1, 0, 0), dirD(), dirE(), PB_HW, (s0 - s1) / 2, (ut - PB_UBOT) / 2,
          GEOM_NECK);
  }
  /* pegs */
  for (int k = 0; k < 4; k++) {
    double ps = -0.03 - 0.035 * k, pu = -0.019 + ((k & 1) ? 0.008 : -0.008);
    m_cyl(m, loc2w(-PEG_HL, ps, pu), loc2w(PEG_HL, ps, pu), PEG_R, 10, GEOM_SCROLL);
  }
  /* scroll spiral ribbon */
  {
    enum { NT = 60 };
    double tmax = 2 * PI * 1.75;
    V o[2][NT + 1], in[2][NT + 1];
    for (int k = 0; k <= NT; k++) {
      double th = tmax * k / NT;
      double r = SCROLL_R * (1 - 0.85 * th / tmax);
      double ri = r - 0.009 > 0.0015 ? r - 0.009 : 0.0015;
      for (int sd = 0; sd < 2; sd++) {
        double X = sd ? SCROLL_HW : -SCROLL_HW;
        o[sd][k] = loc2w(X, SCROLL_CS + r * cos(th), SCROLL_CU - r * sin(th));
        in[sd][k] = loc2w(X, SCROLL_CS + ri * cos(th), SCROLL_CU - ri * sin(th));
      }
    }
    for (int k = 0; k < NT; k++) {
      m_quad(m, o[0][k], o[0][k + 1], o[1][k + 1], o[1][k], GEOM_SCROLL);
      m_quad(m, in[0][k], in[1][k], in[1][k + 1], in[0][k + 1], GEOM_SCROLL);
      m_quad(m, o[0][k], in[0][k], in[0][k + 1], o[0][k + 1], GEOM_SCROLL);
      m_quad(m, o[1][k], o[1][k + 1], in[1][k + 1], in[1][k], GEOM_SCROLL);
    }
  }
}

static int dense(V *out, const V *in, int n, double maxseg) {
  int k = 0;
  for (int i = 0; i + 1 < n; i++) {
    int parts = (int)ceil(vlen(vsub(in[i + 1], in[i])) / maxseg);
    if (parts < 1) parts = 1;
    for (int j = 0; j < parts; j++) out[k++] = vlerp(in[i], in[i + 1], (double)j / parts);
  }
  out[k++] = in[n - 1];
  return k;
}

static void build_strings(GeomMesh *m, const GeomPose *p) {
  for (int i = 0; i < 4; i++) {
    V N = nut_pt(i), B = bridge_pt(i), T = tail_pt(i);
    V pts[8], d[128];
    int n = 0;
    pts[n++] = N;
    if (p->finger_string == i) {
      FingerFrame f = finger_frame(i, p->finger_dist, 0);
      pts[n++] = f.press;
    }
    pts[n++] = B;
    m_tube(m, d, dense(d, pts, n, 0.03), STR_R[i], STR_R[i], 6, GEOM_STRING_C + i, 1);
    V af[2] = {B, T};
    m_tube(m, d, dense(d, af, 2, 0.03), STR_R[i], STR_R[i], 6, GEOM_STRING_C + i, 1);
  }
}

static void build_tail_end(GeomMesh *m) {
  enum { NS = 8 };
  V r[NS + 1][4];
  for (int s = 0; s <= NS; s++) {
    double y = TP_Y0 + (TP_Y1 - TP_Y0) * s / NS, hw = tp_hw(y), zb = tp_zb(y);
    r[s][0] = vk(-hw, y, zb);
    r[s][1] = vk(hw, y, zb);
    r[s][2] = vk(hw, y, zb + TP_T);
    r[s][3] = vk(-hw, y, zb + TP_T);
  }
  m_loft(m, &r[0][0], NS + 1, 4, GEOM_TAILPIECE);
  /* tailgut: over the lower edge to the end button */
  V g[3] = {vk(0, TP_Y0, tp_zb(TP_Y0) + TP_T / 2), vk(0, -0.004, 0.004), vk(0, -0.006, ENDPIN_Z)};
  m_tube(m, g, 3, 0.002, 0.002, 6, GEOM_TAILPIECE, 1);
  /* endpin */
  m_cyl(m, vk(0, 0.0, ENDPIN_Z), vk(0, -GEOM_ENDPIN_LEN, ENDPIN_Z), ENDPIN_R, 10, GEOM_ENDPIN);
}

static void build_bow(GeomMesh *m, const GeomPose *p) {
  BowFrame f = bow_frame(p);
  V c = vadd(f.P, vmul(f.Nc, R_STR + 0.00075));
  V hm = vadd(c, vmul(f.hd, (0.5 - p->bow_u) * GEOM_HAIR_L));
  V ax = f.hd, ay = f.wv, az = vcross(ax, ay);
  {
    enum { NH = 32 };
    V r[NH + 1][4];
    for (int s = 0; s <= NH; s++) {
      V c0 = vadd(hm, vmul(ax, GEOM_HAIR_L * (s / (double)NH - 0.5)));
      r[s][0] = vadd(c0, vadd(vmul(ay, -GEOM_HAIR_W / 2), vmul(az, -0.00075)));
      r[s][1] = vadd(c0, vadd(vmul(ay, GEOM_HAIR_W / 2), vmul(az, -0.00075)));
      r[s][2] = vadd(c0, vadd(vmul(ay, GEOM_HAIR_W / 2), vmul(az, 0.00075)));
      r[s][3] = vadd(c0, vadd(vmul(ay, -GEOM_HAIR_W / 2), vmul(az, 0.00075)));
    }
    m_loft(m, &r[0][0], NH + 1, 4, GEOM_BOW_HAIR);
  }
  /* stick */
  enum { NS = 40 };
  V pts[NS + 1];
  for (int k = 0; k <= NS; k++) {
    double u = -BOW_GRIP_EXT + (1 + BOW_GRIP_EXT) * k / NS;
    pts[k] = vadd(hair_pt(&f, p, u), vmul(f.m, stick_h(u)));
  }
  m_tube(m, pts, NS + 1, BOW_STICK_R0, BOW_STICK_R1, 8, GEOM_BOW_STICK, 1);
  /* frog block and head, the connections stick to hair */
  {
    V fc = vadd(hair_pt(&f, p, 0.025), vmul(f.m, stick_h(0.025) / 2));
    m_box(m, fc, ax, ay, f.m, 0.025 * GEOM_HAIR_L, 0.0085, stick_h(0.025) / 2, GEOM_BOW_STICK);
    V hc = vadd(hair_pt(&f, p, 1.0), vmul(f.m, stick_h(1.0) / 2));
    m_box(m, hc, ax, ay, f.m, 0.012, 0.007, stick_h(1.0) / 2, GEOM_BOW_STICK);
  }
}

static void build_fingers(GeomMesh *m, const GeomPose *p) {
  for (int k = 0; k < 2; k++) {
    int s = k ? p->finger2_string : p->finger_string;
    if (s < 0) continue;
    FingerFrame f = finger_frame(s, k ? p->finger2_dist : p->finger_dist, k ? p->finger2_lift : 0);
    m_ellipsoid(m, f.ctr, vk(1, 0, 0), f.ay, f.up, FINGER_SEMI_A, FINGER_SEMI_B, FINGER_SEMI_C, GEOM_FINGER);
    m_cyl(m, f.ctr, vadd(f.ctr, vadd(vmul(f.up, FINGER_SHAFT_L), vmul(f.ay, -0.01))), FINGER_SHAFT_R, 10,
          GEOM_FINGER);
  }
}

static void build_mic(GeomMesh *m, const GeomPose *p) {
  V c = vk(p->mic[0], p->mic[1], p->mic[2]), aim = mic_aim(p);
  m_cyl(m, vsub(c, vmul(aim, GEOM_MIC_L / 2)), vadd(c, vmul(aim, GEOM_MIC_L / 2)), GEOM_MIC_R, 14, GEOM_MIC);
  V top = vk(c.x, c.y - 0.03, c.z - 0.0);
  V foot = vk(c.x, -GEOM_ENDPIN_LEN + STAND_BASE_T, c.z);
  m_cyl(m, top, foot, STAND_R, 8, GEOM_MIC);
  m_cyl(m, vk(c.x, -GEOM_ENDPIN_LEN, c.z), foot, STAND_BASE_R, 24, GEOM_MIC);
}

void geom_mesh_build(GeomMesh *m, const GeomPose *p) {
  memset(m, 0, sizeof *m);
  build_plates(m);
  build_bridge(m);
  build_neck(m);
  build_strings(m, p);
  build_tail_end(m);
  build_bow(m, p);
  build_fingers(m, p);
  build_mic(m, p);
}

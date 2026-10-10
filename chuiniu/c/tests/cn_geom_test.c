/* The bodies (src/cn_geom.c): the cup's and the die's meshes are the shapes
 * the study drew (counts, closed and outward, the fillet continuous, the inside
 * where the throw's physics puts it), the die is a standard one, a lying cup
 * rests on both rims, the emitted arrays are the renderer's format, and a
 * baked pose is read between its frames. */
#include "../src/cn_geom.h"
#include "cn_check.h"
#include <math.h>

static CnMesh M1, M2;

static int near(double a, double b, double eps) { return fabs(a - b) <= eps; }

/* the mesh's triangles as the renderer will fan them, by position */
static void tri_pos(const CnMesh *m, int p, int c, double out[3][3])
{
    const int c0 = m->first[p], at[3] = { m->at[c0], m->at[c0 + c], m->at[c0 + c + 1] };
    for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) out[k][a] = m->pos[at[k]][a];
}
/* the sum of the triangles' area vectors (0 for a closed surface) and the
 * signed volume they bound (positive when they all face out) */
static void closure(const CnMesh *m, double area[3], double *vol)
{
    area[0] = area[1] = area[2] = 0; *vol = 0;
    for (int p = 0; p < m->npoly; p++) for (int c = 1; c + 1 < m->count[p]; c++) {
        double P[3][3]; tri_pos(m, p, c, P);
        double u[3] = { P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2] }, v[3] = { P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2] };
        double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        for (int a = 0; a < 3; a++) area[a] += n[a] / 2;
        double x[3] = { P[1][1] * P[2][2] - P[1][2] * P[2][1], P[1][2] * P[2][0] - P[1][0] * P[2][2], P[1][0] * P[2][1] - P[1][1] * P[2][0] };
        *vol += (P[0][0] * x[0] + P[0][1] * x[1] + P[0][2] * x[2]) / 6;
    }
}
/* every smooth corner's normal on the outward side of its polygon */
static int normals_agree(const CnMesh *m)
{
    int bad = 0;
    for (int p = 0; p < m->npoly; p++) {
        if (!m->smooth[p]) continue;
        double P[3][3]; tri_pos(m, p, 1, P);
        double u[3] = { P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2] }, v[3] = { P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2] };
        double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        for (int c = 0; c < m->count[p]; c++) {
            const float *k = m->nrm[m->first[p] + c];
            if (k[0] * n[0] + k[1] * n[1] + k[2] * n[2] <= 0) bad++;
        }
    }
    return bad;
}

static void test_maths(void)
{
    TEST("the kernel's maths: sin, cos, atan2, the study's hash");
    for (double x = -20; x <= 20; x += .37) {
        CHECK(near(cn_m_sin(x), sin(x), 1e-12) && near(cn_m_cos(x), cos(x), 1e-12), "sin, cos at %g (cn_roll.c's series: 2.5e-13 at pi/4)", x);
        for (double y = -3; y <= 3; y += 1.1) CHECK(near(cn_m_atan2(y, x), atan2(y, x), 1e-14), "atan2(%g, %g)", y, x);
    }
    CHECK(near(cn_m_atan2(3, -4), 2.498091544796509, 1e-14), "atan2(3, -4) is JS's");
    CHECK(near(cn_m_atan2(1e3, 1), 1.569796327128230, 1e-14), "atan2(1000, 1) is JS's");
    CHECK(cn_m_atan2(0, 0) == 0 && near(cn_m_atan2(1, 0), CN_PI / 2, 1e-15), "atan2 on the axis");
    /* TEX.hash in Chromium (Math.imul): the jitter every placement draws from */
    CHECK(near(cn_geom_hash(0, 71, 977), 0.169133142801, 1e-12), "hash(0, 71, 977) %.12f", cn_geom_hash(0, 71, 977));
    CHECK(near(cn_geom_hash(4, 73, 982), 0.441867705900, 1e-12), "hash(4, 73, 982)");
    CHECK(near(cn_geom_hash(35, 77, 977), 0.579207109520, 1e-12), "hash(35, 77, 977)");
    CHECK(near(cn_geom_hash(-3, 5, 123456789), 0.324749136344, 1e-12), "hash with a negative lattice point");
    CHECK(near(cn_geom_hash(2, 81, (int32_t)4000000000u), 0.112608814612, 1e-12), "hash with a seed past 2^31 (JS's ToInt32)");
}

static void test_cup_mesh(void)
{
    TEST("the cup: counts, closed and outward, a fillet that meets the wall and the crown");
    const double R = 59, rc = R * CN_CUP_RC, h = R * CN_CUP_TALL, t = R * CN_CUP_WALL, dome = CN_DOME * 24;
    const int S = CN_CUP_SEGS;
    CHECK(cn_geom_cup_mesh(&M1, R, rc, h, S, 0, 0), "the outside builds");
    /* wall 3 rings, fillet 4 rings, the crown's ring; quads 2 + 3 rows and the crown's 36-gon */
    CHECK(M1.npos == S * 3 + S * 4 + S, "outside positions %d", M1.npos);
    CHECK(M1.npoly == S * 2 + S * 3 + 1, "outside polygons %d", M1.npoly);
    CHECK(M1.ncorner == (S * 5) * 4 + S && M1.ntri == S * 5 * 2 + S - 2, "outside corners %d triangles %d", M1.ncorner, M1.ntri);
    CHECK(cn_geom_cup_mesh(&M2, R, rc, h, S, t, dome), "the cup with an inside builds");
    CHECK(M2.npos == S * 8 + S * 3 + S + 1 + S * 2, "positions %d", M2.npos);
    CHECK(M2.npoly == S * 5 + 1 + S * 2 + S + S, "polygons %d", M2.npoly);
    CHECK(M2.ncorner == S * 5 * 4 + S + S * 2 * 4 + S * 3 + S * 4, "corners %d", M2.ncorner);
    CHECK(M2.ntri == S * 5 * 2 + (S - 2) + S * 2 * 2 + S + S * 2, "triangles %d", M2.ntri);
    CHECK(M2.ncorner <= CN_MESH_MAX_CORNER && M2.npos <= CN_MESH_MAX_POS, "the caps hold the largest cup");
    CHECK(!cn_geom_cup_mesh(&M1, R, rc, h, 200, t, dome), "a cup past the caps is refused, not overrun");

    /* closed, every face out: the area vectors cancel and the volume is the wall's */
    double area[3], vol;
    closure(&M2, area, &vol);
    CHECK(fabs(area[0]) + fabs(area[1]) + fabs(area[2]) < 1e-2, "the cup with an inside is closed (%g %g %g)", area[0], area[1], area[2]);
    const double facet = S / (2 * CN_PI) * sin(2 * CN_PI / S);   /* a 36-gon's area of its circle's */
    const double hf = h - t, outer = CN_PI * h / 3 * (R * R + R * rc + rc * rc), inner = CN_PI * hf / 3 * ((R - t) * (R - t) + (R - t) * (rc - t) + (rc - t) * (rc - t));
    const double domev = CN_PI * (rc - t) * (rc - t) * dome / 3;   /* the floor's cone, raised toward the mouth */
    const double want = facet * (outer - inner + domev);
    CHECK(vol > 0 && near(vol, want, want * .02), "the wall's volume %.0f, a frustum less a frustum %.0f", vol, want);
    CHECK(normals_agree(&M2) == 0, "every corner normal on its face's outer side (%d not)", normals_agree(&M2));

    /* the fillet: its first ring is the wall's last, its last ring the crown's, at z = h */
    const double f = R * CN_CUP_FILLET, rcf = rc + (R - rc) * f / h, ccx = rcf - f, cz = h - f;
    const int wall_top = 2, fil = S * 3;       /* the wall's ring 2 of segment 0; the fillet's first position */
    CHECK(near(M2.pos[wall_top][0], rcf, 1e-4) && near(M2.pos[wall_top][2], cz, 1e-4), "the wall ends at the fillet's foot");
    for (int i = 0; i < S; i++) {
        const float *w = M2.pos[i * 3 + 2], *a = M2.pos[fil + i * 4], *b = M2.pos[fil + i * 4 + 3];
        CHECK(near(w[0], a[0], 1e-4) && near(w[1], a[1], 1e-4) && near(w[2], a[2], 1e-4), "segment %d: the fillet starts where the wall ends", i);
        CHECK(near(hypot(b[0], b[1]), ccx, 1e-4) && near(b[2], h, 1e-4), "segment %d: the fillet ends on the crown's edge", i);
        const float *c = M2.pos[S * 7 + i];
        CHECK(near(c[0], b[0], 1e-4) && near(c[1], b[1], 1e-4) && near(c[2], b[2], 1e-4), "segment %d: the crown's ring is the fillet's last", i);
    }
    /* the fillet's normals run from level to straight up; the wall's lean out of level by the cone's slope */
    const int fq = S * 2;   /* the first fillet polygon */
    const float *n0 = M2.nrm[M2.first[fq]], *n3 = M2.nrm[M2.first[fq + 2] + 2], *nw = M2.nrm[M2.first[0] + 3];
    CHECK(near(n0[2], 0, 1e-6) && near(hypot(n0[0], n0[1]), 1, 1e-6), "the fillet starts level");
    CHECK(near(n3[2], 1, 1e-6), "the fillet ends straight up, the crown's normal (%g)", n3[2]);
    CHECK(near(asin(nw[2]), atan((R - rc) / h), 1e-5), "the wall's normal rises by the cone's slope (%g)", asin(nw[2]));

    /* the inside: the inner wall runs from R - t at the mouth to rc - t at h - t,
     * straight; cn_roll.c's wall runs the same radii over h, so the drawn wall
     * is inside the physics' by at most (R - rc) t / h (docs_pkgB.md) */
    const int iw = S * 8;   /* the inner wall's first position */
    double worst = 0;
    for (int i = 0; i < S; i++) for (int r = 0; r <= 2; r++) {
        const float *p = M2.pos[iw + i * 3 + r];
        const double z = p[2], rr = hypot(p[0], p[1]), want_r = (R - t) + ((rc - t) - (R - t)) * z / hf;
        CHECK(near(rr, want_r, 1e-3), "inner wall %d.%d: radius %g at z %g, want %g", i, r, rr, z, want_r);
        const double phys = (R - t) + ((rc - t) - (R - t)) * z / h;
        if (phys - rr > worst) worst = phys - rr;
    }
    CHECK(worst > 0 && worst <= (R - rc) * t / h + 1e-3, "the drawn inside is within %.3f of the physics' (%.3f)", (R - rc) * t / h, worst);
    /* the floor: its rim at h - t, its centre `dome` toward the mouth */
    const float *fc = M2.pos[S * 12];
    CHECK(near(fc[0], 0, 1e-6) && near(fc[2], hf - dome, 1e-4), "the floor's centre is domed toward the mouth (%g)", fc[2]);
}

static void test_die_mesh(void)
{
    TEST("the die: counts, closed and outward, the rounded edge");
    const double d = 24, r = d * CN_DIE_ROUND;
    CHECK(cn_geom_die_mesh(&M1, d), "the die builds");
    CHECK(M1.npos == 6 * 36 && M1.npoly == 6 * 25 && M1.ncorner == 600 && M1.ntri == 300, "%d positions, %d polygons, %d corners, %d triangles", M1.npos, M1.npoly, M1.ncorner, M1.ntri);
    double area[3], vol;
    closure(&M1, area, &vol);
    CHECK(fabs(area[0]) + fabs(area[1]) + fabs(area[2]) < 1e-3, "the die is closed");
    const double c = d - 2 * r, round_vol = c * c * c + 6 * c * c * r + 3 * CN_PI * r * r * c + 4.0 / 3 * CN_PI * r * r * r;
    CHECK(vol > 0 && vol < round_vol && vol > round_vol * .98, "its volume %.1f, a rounded cube's %.1f less the facets", vol, round_vol);
    CHECK(normals_agree(&M1) == 0, "every corner normal on its face's outer side");
    int out = 0, onsurf = 0;
    for (int i = 0; i < M1.npos; i++) {
        const float *p = M1.pos[i];
        double q[3];
        for (int a = 0; a < 3; a++) q[a] = fabs(p[a]) > d / 2 - r ? fabs(p[a]) - (d / 2 - r) : 0;
        if (near(sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), r, 1e-4)) onsurf++;
        if (fabs(p[0]) > d / 2 + 1e-4 || fabs(p[1]) > d / 2 + 1e-4 || fabs(p[2]) > d / 2 + 1e-4) out++;
    }
    CHECK(onsurf == M1.npos && out == 0, "every position on the rounded cube (%d of %d), none outside it", onsurf, M1.npos);
    for (int p = 0; p < M1.npoly; p++) CHECK(M1.face[p] == p / 25, "polygon %d is on face slot %d", p, p / 25);
}

static void test_die_values(void)
{
    TEST("the die's values: a standard right-handed die, any face up");
    for (int axis = 0; axis < 3; axis++) for (int sign = -1; sign <= 1; sign += 2) for (int v = 1; v <= 6; v++) {
        uint8_t cells[6];
        cn_die_cells(axis, sign, v, cells);
        CHECK(cn_die_value(cells, axis, sign) == v, "axis %d sign %d: %d up", axis, sign, v);
        int seen = 0, pos[3];
        for (int a = 0; a < 3; a++) {
            int p = cn_die_value(cells, a, 1), n = cn_die_value(cells, a, -1);
            CHECK(p + n == 7, "axis %d sign %d value %d: opposite faces on axis %d are %d and %d", axis, sign, v, a, p, n);
            seen |= 1 << p | 1 << n; pos[a] = p;
        }
        CHECK(seen == 0x7e, "every value once (%x)", seen);
        /* 1, 2, 3 counterclockwise round their corner: the signed axes carrying them make a right-handed frame */
        double e[3][3] = { { 0 } };
        for (int a = 0; a < 3; a++) { if (pos[a] <= 3) e[pos[a] - 1][a] = 1; else e[7 - pos[a] - 1][a] = -1; }
        double det = e[0][0] * (e[1][1] * e[2][2] - e[1][2] * e[2][1]) - e[0][1] * (e[1][0] * e[2][2] - e[1][2] * e[2][0]) + e[0][2] * (e[1][0] * e[2][1] - e[1][1] * e[2][0]);
        CHECK(det > 0, "axis %d sign %d value %d: right-handed", axis, sign, v);
    }
    /* cn_roll.c's up index to a slot and back */
    for (int up = 0; up < 6; up++) {
        int s = cn_die_slot_of_up(up);
        CHECK(s >= 0 && s < 6, "up %d has a slot", up);
        uint8_t cells[6]; cn_die_cells(up >> 1, up & 1 ? -1 : 1, 4, cells);
        CHECK(cells[s] == 4, "up %d: its slot carries the value set up", up);
    }
    CHECK(cn_die_slot_of_up(6) == -1 && cn_die_slot_of_up(-1) == -1, "no slot past the six");
}

static void test_bodies(void)
{
    TEST("a lying cup rests on both rims; a standing one on its mouth");
    CnObj o;
    const float R = 42.9f;
    cn_geom_cup_obj(&o, R, 3, 35, 273.7f, 384.4f, 1, 179);
    CHECK(o.out && o.has_rot && o.lift < R && o.lift > R * .99f, "out: lying, its mouth's centre a little under a radius up (%g)", o.lift);
    CHECK(near(o.R * CN_CUP_RC, o.R * .72, 1e-6) && o.tint[0] == 4 && near(o.kmul[0], .45, 1e-6), "dim");
    /* the mouth's and the crown's lowest points touch the table */
    const double rc = R * CN_CUP_RC, h = R * CN_CUP_TALL;
    double lo_mouth = 1e9, lo_crown = 1e9;
    for (int i = 0; i < 360; i++) {
        const double a = i * CN_PI / 180, pm[3] = { R * cos(a), R * sin(a), 0 }, pc[3] = { rc * cos(a), rc * sin(a), h };
        double zm = o.rot[2] * pm[0] + o.rot[5] * pm[1] + o.rot[8] * pm[2] + o.lift;
        double zc = o.rot[2] * pc[0] + o.rot[5] * pc[1] + o.rot[8] * pc[2] + o.lift;
        if (zm < lo_mouth) lo_mouth = zm;
        if (zc < lo_crown) lo_crown = zc;
    }
    CHECK(near(lo_mouth, 0, 1e-3) && near(lo_crown, 0, 1e-3), "both rims on the table (%g, %g)", lo_mouth, lo_crown);
    /* the mouth faces the ring's centre (toward_x left of it here) */
    CHECK(o.rot[6] > .9f, "the axis runs from the mouth to the crown away from toward_x: the mouth faces it (%g)", o.rot[6]);
    /* the body is centred on the seat: the axis's middle (the crown's centre is
     * the mouth's plus h along the axis) over (x, y) */
    const double mx = o.x + o.rot[6] * h / 2, my = o.y + o.rot[7] * h / 2;
    CHECK(near(mx, 273.7, 1e-3) && near(my, 384.4, 1e-3), "centred on its seat (%g, %g)", mx, my);
    float occ[5];
    cn_geom_occluder(&o, 10, occ);
    CHECK(near(occ[0], 273.7 + 10, 1e-3) && near(occ[1], 384.4, 1e-3) && occ[3] == 0 && near(occ[4], .45, 1e-6), "a lying cup's footprint is under its middle");

    CnObj s;
    cn_geom_cup_obj(&s, 58.9f, 5, 3, 179, 490, 0, 0);
    cn_geom_occluder(&s, 0, occ);
    CHECK(!s.out && !s.has_rot && s.lift == 0 && near(occ[2], 58.9, 1e-4) && near(occ[4], .55, 1e-6), "a standing cup's footprint is its mouth");
    CHECK(!s.crown_big, "a big cup's count is set small");
    cn_geom_cup_obj(&s, 36, 5, 3, 179, 490, 0, 0);
    CHECK(s.crown_big, "under 40 points of radius the count is set at 184");
    CnObj d;
    cn_geom_die_obj(&d, 24, 6, 61, 100, 100, .2f);
    cn_geom_occluder(&d, 0, occ);
    CHECK(d.lift == 12 && d.value == 6 && d.cells[0] == 6 && d.cells[1] == 1 && near(occ[2], 24 * .62, 1e-4) && occ[3] == 0, "a die rests on the table, its value up");
}

static float VB[(CN_MESH_MAX_CORNER + 8) * CN_GEOM_VF], FB[(CN_MESH_MAX_CORNER + 8) * CN_GEOM_FF];

static void test_emit(void)
{
    TEST("the emitted arrays are the renderer's: corners, fans, textures, the peek's hinge");
    const double R = 40;
    cn_geom_cup_mesh(&M1, R, R * CN_CUP_RC, R * CN_CUP_TALL, CN_CUP_SEGS, R * CN_CUP_WALL, 8);
    CnObj o;
    cn_geom_cup_obj(&o, (float)R, 5, 3, 100, 200, 0, 0);
    const int tex[CN_TEX_SLOTS] = { 11, 12, 13, 14, 15 };
    memset(VB, 0, sizeof VB); memset(FB, 0, sizeof FB);
    cn_geom_emit(&M1, &o, 5, tex, VB, 3, FB, 2);
    int bad = 0, texs = 0;
    for (int f = 2; f < 2 + M1.ntri; f++) {
        const float *F = FB + f * CN_GEOM_FF;
        for (int k = 0; k < 3; k++) if (F[k] < 3 || F[k] >= 3 + M1.ncorner) bad++;
        if (F[15] != CN_GEOM_F_BODY) bad++;
        if (F[9] >= 11 && F[9] <= 14) texs |= 1 << (int)(F[9] - 11);
        if (F[10] != 0 || F[11] != 4 || F[12] != 3 || F[13] != 1 || F[14] != 0) bad++;
    }
    CHECK(bad == 0, "every face indexes its own corners, is a body, untinted (%d bad)", bad);
    CHECK(texs == 0xf, "the side, the crown, the inside and the floor each wear their texture (%x)", texs);
    CHECK(FB[(2 + M1.ntri) * CN_GEOM_FF + 15] == 0 && FB[1 * CN_GEOM_FF + 15] == 0, "nothing written past the faces it owns");
    /* the first wall corner: (R, 0, 0) at (100 + 5, 200, 0), its normal out along +x */
    const float *v = VB + 3 * CN_GEOM_VF;
    CHECK(near(v[0], 100 + 5 + R, 1e-4) && near(v[1], 200, 1e-4) && near(v[2], 0, 1e-4), "the world is the board, shifted by pad_x");
    CHECK(v[3] > .99f && near(hypot(hypot(v[3], v[4]), v[5]), 1, 1e-5), "the normals are unit and turned with the body");
    /* the peek: the mouth's far point (0, -R, 0) is the hinge; with no back or lift it does not move */
    o.tilt_angle = 1.0f; o.tilt_hinge_y = (float)-R;
    cn_geom_emit(&M1, &o, 0, tex, VB, 0, FB, 0);
    int found = 0;
    for (int c = 0; c < M1.ncorner; c++) {
        const float *p = VB + c * CN_GEOM_VF;
        if (near(p[0], 100, 1e-3) && near(p[1], 200 - R, 1e-3) && near(p[2], 0, 1e-3)) found++;
        if (p[2] < -1e-3) found = -1000;
    }
    CHECK(found > 0, "the far edge of the mouth stays on the table as the cup tips, nothing goes under it (%d)", found);
    /* a die: its top face reads the cell of the value up */
    cn_geom_die_mesh(&M2, 24);
    CnObj d; cn_geom_die_obj(&d, 24, 5, 60, 0, 0, 0);
    cn_geom_emit(&M2, &d, 0, tex, VB, 0, FB, 0);
    int in_cell = 1;
    for (int f = 0; f < M2.ntri; f++) {
        const float *F = FB + f * CN_GEOM_FF;
        const int slot = M2.face[f / 2], cell = d.cells[slot] - 1;
        for (int k = 0; k < 3; k++) if (F[3 + 2 * k] < cell / 6.0 || F[3 + 2 * k] > (cell + 1) / 6.0) in_cell = 0;
        if (F[9] != 15) in_cell = 0;
    }
    CHECK(in_cell, "every face of the die reads only its value's cell of the atlas");
    CHECK(d.cells[cn_die_slot(2, 1)] == 5, "5 up");
}

static void test_pose(void)
{
    TEST("a baked throw between its frames: lerp, nlerp the short way, the clamp, the phase");
    static float fr[3 * CN_ROLL_FRAME_FLOATS];
    static uint8_t ph[3] = { CN_RP_SHAKE, CN_RP_FLIP, CN_RP_IDLE };
    memset(fr, 0, sizeof fr);
    /* the cup: frame 0 at (0,0,0) unturned, frame 1 at (6,0,12) a quarter turn about z written as -q, frame 2 at rest */
    const double s = sqrt(.5);
    float *a = fr, *b = fr + CN_ROLL_FRAME_FLOATS, *c = fr + 2 * CN_ROLL_FRAME_FLOATS;
    a[6] = 1;
    b[0] = 6; b[2] = 12; b[5] = (float)-s; b[6] = (float)-s;
    c[0] = 6; c[6] = 1;
    CnPose p;
    cn_geom_pose_at(fr, ph, 3, .5 / CN_ROLL_HZ, 0, &p);
    CHECK(near(p.p[0], 3, 1e-5) && near(p.p[2], 6, 1e-5), "half a frame in: half way");
    /* the short way round: half of a quarter turn, not a turn through the long side */
    CHECK(near(p.rot[0], cos(CN_PI / 4), 1e-5) && near(p.rot[1], sin(CN_PI / 4), 1e-5) && near(p.rot[8], 1, 1e-5), "an eighth of a turn (%g %g)", p.rot[0], p.rot[1]);
    CHECK(p.phase == CN_RP_SHAKE && p.frame == 0, "the phase of the frame at or before");
    cn_geom_pose_at(fr, ph, 3, 99, 0, &p);
    CHECK(p.frame == 2 && p.phase == CN_RP_IDLE && near(p.p[0], 6, 1e-6), "past the end: the last frame");
    cn_geom_pose_at(fr, ph, 3, -1, 0, &p);
    CHECK(p.frame == 0 && near(p.p[0], 0, 1e-6), "before the start: the first");
    CnObj cup; cn_geom_cup_obj(&cup, 40, 5, 3, 50, 60, 0, 0);
    cn_geom_pose_at(fr, ph, 3, 1.0 / CN_ROLL_HZ, 0, &p);
    cn_geom_place(&cup, &p, -1);
    CHECK(cup.has_rot && near(cup.x, 6, 1e-6) && near(cup.lift, 12, 1e-6), "a cup in the air takes the pose");
    cn_geom_pose_at(fr, ph, 3, 2.0 / CN_ROLL_HZ, 0, &p);
    cn_geom_place(&cup, &p, -1);
    CHECK(!cup.has_rot && cup.x == 50 && cup.y == 60 && cup.lift == 0, "an idle cup stands at home");
    CnObj die; cn_geom_die_obj(&die, 24, 2, 60, 0, 0, 0);
    cn_geom_place(&die, &p, 0);
    CHECK(die.value == die.cells[cn_die_slot_of_up(0)] && die.has_rot, "an idle die reads the face the bake says is up");
}

int main(void)
{
    test_maths();
    test_cup_mesh();
    test_die_mesh();
    test_die_values();
    test_bodies();
    test_emit();
    test_pose();
    return report("cn_geom_test");
}

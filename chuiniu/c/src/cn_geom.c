/* Chui Niu - the bodies on the table. See cn_geom.h.
 *
 * Every function here is the study's (chuiniu/docs/UI.html) arithmetic in the
 * study's order, in doubles, stored as floats at the end as the study's
 * Float32Array did; where it is not, the comment says why. */
#include "cn_geom.h"
#include <string.h>

/* ---- the kernel's own maths --------------------------------------------------------- */

/* sin and cos as cn_roll.c spells them (shared/c/wasm/libm.c's series on
 * [-pi/4, pi/4], then quadrant by quadrant), copied so this file stands alone. */
static double sin_k(double x)
{
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}
static double cos_k(double x)
{
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132)))));
}
static double quad(double x, int *q)
{
    double k = __builtin_floor(x / (CN_PI / 2) + .5);
    *q = (int)((long long)k & 3);
    return x - k * (CN_PI / 2);
}
double cn_m_sin(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}
double cn_m_cos(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return cos_k(r); case 1: return -sin_k(r); case 2: return -cos_k(r); default: return sin_k(r); }
}
/* IEEE sqrt, floor and ceil are single correctly rounded instructions on
 * every target here (as cn_roll.c says of sqrt). */
double cn_m_sqrt(double x) { return __builtin_sqrt(x); }
double cn_m_floor(double x) { return __builtin_floor(x); }
double cn_m_ceil(double x) { return __builtin_ceil(x); }
/* atan: two half-angle steps (atan x = 2 atan(x / (1 + sqrt(1 + x^2)))) bring
 * |x| <= 1 under tan(pi/16) = .199, where thirteen terms of the series are
 * exact to a double. */
static double atan_k(double x)
{
    double x2 = x * x, term = x, sum = 0;
    for (int k = 0; k < 13; k++) { sum += term / (2 * k + 1); term *= -x2; }
    return sum;
}
static double m_atan(double x)
{
    int neg = x < 0, inv;
    if (neg) x = -x;
    inv = x > 1;
    if (inv) x = 1 / x;
    x = x / (1 + cn_m_sqrt(1 + x * x));
    x = x / (1 + cn_m_sqrt(1 + x * x));
    double r = 4 * atan_k(x);
    if (inv) r = CN_PI / 2 - r;
    return neg ? -r : r;
}
double cn_m_atan2(double y, double x)
{
    if (x > 0) return m_atan(y / x);
    if (x < 0) return y < 0 ? m_atan(y / x) - CN_PI : m_atan(y / x) + CN_PI;
    return y > 0 ? CN_PI / 2 : y < 0 ? -CN_PI / 2 : 0;
}

double cn_geom_hash(int32_t ix, int32_t iy, int32_t seed)
{
    uint32_t h = ((uint32_t)ix * 0x8da6b343u) ^ ((uint32_t)iy * 0xd8163841u) ^ ((uint32_t)seed * 0xcb1ab31fu);
    h = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) * 0x297a2d39u;
    h ^= h >> 15;
    return h / 4294967296.0;
}

/* ---- vectors ------------------------------------------------------------------------- */
typedef struct { double x, y, z; } V;
static V v3(double x, double y, double z) { V v = { x, y, z }; return v; }
static V vsub(V a, V b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V cross(V a, V b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V vnorm(V v)
{
    double n = cn_m_sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(n > 0)) n = 1;
    return v3(v.x / n, v.y / n, v.z / n);
}

/* ---- the mesh builder ------------------------------------------------------------------ */
static int pos_add(CnMesh *m, V p)
{
    if (m->npos >= CN_MESH_MAX_POS) return -1;
    m->pos[m->npos][0] = (float)p.x; m->pos[m->npos][1] = (float)p.y; m->pos[m->npos][2] = (float)p.z;
    return m->npos++;
}
/* a polygon of k corners: positions idx, uv pairs, normals (or none: flat) */
static int poly_add(CnMesh *m, int k, const int *idx, const double (*uv)[2], const V *nrm, int tex, int face)
{
    if (m->npoly >= CN_MESH_MAX_POLY || m->ncorner + k > CN_MESH_MAX_CORNER || k < 3 || k > 255) return 0;
    int p = m->npoly++, c0 = m->ncorner;
    m->first[p] = (uint16_t)c0; m->count[p] = (uint8_t)k; m->tex[p] = (uint8_t)tex;
    m->smooth[p] = nrm ? 1 : 0; m->face[p] = (int8_t)face;
    for (int i = 0; i < k; i++) {
        m->at[c0 + i] = (uint16_t)idx[i];
        m->uv[c0 + i][0] = (float)uv[i][0]; m->uv[c0 + i][1] = (float)uv[i][1];
        V n = nrm ? nrm[i] : v3(0, 0, 0);
        m->nrm[c0 + i][0] = (float)n.x; m->nrm[c0 + i][1] = (float)n.y; m->nrm[c0 + i][2] = (float)n.z;
    }
    m->ncorner = (uint16_t)(c0 + k); m->ntri = (uint16_t)(m->ntri + k - 2);
    return 1;
}
/* reverse a polygon's corners in place (the study's idx/uv/nrm .reverse()) */
static void rev(int k, int *idx, double (*uv)[2], V *nrm)
{
    for (int a = 0, b = k - 1; a < b; a++, b--) {
        int ti = idx[a]; idx[a] = idx[b]; idx[b] = ti;
        double tu = uv[a][0], tv = uv[a][1]; uv[a][0] = uv[b][0]; uv[a][1] = uv[b][1]; uv[b][0] = tu; uv[b][1] = tv;
        if (nrm) { V tn = nrm[a]; nrm[a] = nrm[b]; nrm[b] = tn; }
    }
}

/* The cone's wall from radius rad0 at z0 to rad1 at z1, in rows, its normal the
 * outer cone's (nz) at every row; flip winds it inward, for the inside. */
static int wall(CnMesh *m, int segs, double rad0, double rad1, double z0, double z1, double nz, int tex, int flip)
{
    const int ROWS = CN_CUP_WALL_ROWS, VR = ROWS + 1, base = m->npos;
    for (int i = 0; i < segs; i++) {
        double a = (double)i / segs * CN_PI * 2;
        for (int r = 0; r <= ROWS; r++) {
            double t = (double)r / ROWS, rr = rad0 + (rad1 - rad0) * t;
            if (pos_add(m, v3(rr * cn_m_cos(a), rr * cn_m_sin(a), z0 + (z1 - z0) * t)) < 0) return 0;
        }
    }
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % segs;
        double u0 = (double)i / segs, u1 = (double)(i + 1) / segs, a0 = (double)i / segs * CN_PI * 2, a1 = (double)(i + 1) / segs * CN_PI * 2;
        V n0 = vnorm(v3(cn_m_cos(a0), cn_m_sin(a0), nz)), n1 = vnorm(v3(cn_m_cos(a1), cn_m_sin(a1), nz));
        for (int r = 0; r < ROWS; r++) {
            double v0 = 1 - (double)r / ROWS, v1 = 1 - (double)(r + 1) / ROWS;
            int idx[4] = { base + i * VR + r, base + j * VR + r, base + j * VR + r + 1, base + i * VR + r + 1 };
            double uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
            V nrm[4] = { n0, n1, n1, n0 };
            if (flip) { rev(4, idx, uv, nrm); for (int k = 0; k < 4; k++) nrm[k] = v3(-nrm[k].x, -nrm[k].y, -nrm[k].z); }
            if (!poly_add(m, 4, idx, (const double (*)[2])uv, nrm, tex, -1)) return 0;
        }
    }
    return 1;
}
/* A disc: one polygon, or a fan about a centre moved down by dome. */
static int disc(CnMesh *m, int segs, double rad, double z, int tex, int flip, double dome)
{
    int base = m->npos, idx[CN_CUP_SEGS * 2];
    double uv[CN_CUP_SEGS * 2][2];
    if (segs > CN_CUP_SEGS * 2) return 0;
    for (int i = 0; i < segs; i++) {
        double a = (double)i / segs * CN_PI * 2;
        if (pos_add(m, v3(rad * cn_m_cos(a), rad * cn_m_sin(a), z)) < 0) return 0;
        idx[i] = base + i; uv[i][0] = .5 + .5 * cn_m_cos(a); uv[i][1] = .5 + .5 * cn_m_sin(a);
    }
    if (!(dome != 0)) {
        if (flip) rev(segs, idx, uv, 0);
        return poly_add(m, segs, idx, (const double (*)[2])uv, 0, tex, -1);
    }
    int c = pos_add(m, v3(0, 0, z - dome));
    if (c < 0) return 0;
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % segs, fi[3] = { c, idx[i], idx[j] };
        double fu[3][2] = { { .5, .5 }, { uv[i][0], uv[i][1] }, { uv[j][0], uv[j][1] } };
        if (flip) rev(3, fi, fu, 0);
        if (!poly_add(m, 3, fi, (const double (*)[2])fu, 0, tex, -1)) return 0;
    }
    return 1;
}

int cn_geom_cup_mesh(CnMesh *m, double R, double rc, double h, int segs, double t, double dome)
{
    memset(m, 0, sizeof *m);
    if (segs < 3 || segs > CN_CUP_SEGS * 2 || !(R > 0) || !(rc > 0) || !(h > 0)) return 0;
    const double nz = (R - rc) / h;
    /* the edge between the wall and the crown is a fillet of radius f with its
     * own normals; the crown inside it is flat */
    const double f = R * CN_CUP_FILLET, rcf = rc + (R - rc) * f / h, ccx = rcf - f, cz = h - f, phEnd = CN_PI / 2;
    if (!wall(m, segs, R, rcf, 0, cz, nz, CN_TEX_CUP_SIDE, 0)) return 0;
    {
        const int FR = CN_CUP_FILLET_ROWS, V1 = FR + 1, base = m->npos;
        for (int i = 0; i < segs; i++) {
            double a = (double)i / segs * CN_PI * 2;
            for (int r = 0; r <= FR; r++) {
                double ph = phEnd * r / FR, rr = ccx + f * cn_m_cos(ph);
                if (pos_add(m, v3(rr * cn_m_cos(a), rr * cn_m_sin(a), cz + f * cn_m_sin(ph))) < 0) return 0;
            }
        }
        for (int i = 0; i < segs; i++) {
            int j = (i + 1) % segs;
            double u0 = (double)i / segs, u1 = (double)(i + 1) / segs, a0 = (double)i / segs * CN_PI * 2, a1 = (double)(i + 1) / segs * CN_PI * 2;
            for (int r = 0; r < FR; r++) {
                double p0 = phEnd * r / FR, p1 = phEnd * (r + 1) / FR, v0 = .05 - (double)r / FR * .05, v1 = .05 - (double)(r + 1) / FR * .05;
                int idx[4] = { base + i * V1 + r, base + j * V1 + r, base + j * V1 + r + 1, base + i * V1 + r + 1 };
                double uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
                V nrm[4] = {
                    v3(cn_m_cos(p0) * cn_m_cos(a0), cn_m_cos(p0) * cn_m_sin(a0), cn_m_sin(p0)),
                    v3(cn_m_cos(p0) * cn_m_cos(a1), cn_m_cos(p0) * cn_m_sin(a1), cn_m_sin(p0)),
                    v3(cn_m_cos(p1) * cn_m_cos(a1), cn_m_cos(p1) * cn_m_sin(a1), cn_m_sin(p1)),
                    v3(cn_m_cos(p1) * cn_m_cos(a0), cn_m_cos(p1) * cn_m_sin(a0), cn_m_sin(p1)),
                };
                if (!poly_add(m, 4, idx, (const double (*)[2])uv, nrm, CN_TEX_CUP_SIDE, -1)) return 0;
            }
        }
    }
    if (!disc(m, segs, ccx, h, CN_TEX_CUP_CROWN, 0, 0)) return 0;
    if (t > 0) {
        const double hf = h - t;
        if (!wall(m, segs, R - t, rc - t, 0, hf, nz, CN_TEX_CUP_INNER, 1)) return 0;
        if (!disc(m, segs, rc - t, hf, CN_TEX_CUP_FLOOR, 1, dome)) return 0;
        /* the rim: the wall's thickness at the mouth, facing out of it */
        const int base = m->npos;
        for (int i = 0; i < segs; i++) {
            double a = (double)i / segs * CN_PI * 2;
            if (pos_add(m, v3(R * cn_m_cos(a), R * cn_m_sin(a), 0)) < 0) return 0;
            if (pos_add(m, v3((R - t) * cn_m_cos(a), (R - t) * cn_m_sin(a), 0)) < 0) return 0;
        }
        for (int i = 0; i < segs; i++) {
            int j = (i + 1) % segs, idx[4] = { base + i * 2, base + i * 2 + 1, base + j * 2 + 1, base + j * 2 };
            double u0 = (double)i / segs, u1 = (double)(i + 1) / segs;
            double uv[4][2] = { { u0, .52 }, { u0, .48 }, { u1, .48 }, { u1, .52 } };
            if (!poly_add(m, 4, idx, (const double (*)[2])uv, 0, CN_TEX_CUP_INNER, -1)) return 0;
        }
    }
    return 1;
}

/* the study's AX: the face slots' axis and sign */
static const int AX[6][2] = { { 2, 1 }, { 2, -1 }, { 1, -1 }, { 0, 1 }, { 1, 1 }, { 0, -1 } };

int cn_geom_die_mesh(CnMesh *m, double d)
{
    memset(m, 0, sizeof *m);
    if (!(d > 0)) return 0;
    const double s = d / 2, r = d * CN_DIE_ROUND, k = r * (1 - 0.70710678118654752440);
    /* the grid lines along a face: the flat middle, then two rows that bend round
     * the radius to the 45-degree line */
    const double T[6] = { -s, -s + k, -(s - r), s - r, s - k, s };
    for (int fs = 0; fs < 6; fs++) {
        const int axis = AX[fs][0], sign = AX[fs][1], base = m->npos;
        V grid[36];
        for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) {
            double p[3] = { 0, 0, 0 }, q[3];
            p[axis] = s * sign; p[(axis + 1) % 3] = T[i]; p[(axis + 2) % 3] = T[j];
            /* a point on the sharp cube, rounded: clamp to the inner box, push out along the sphere */
            for (int c = 0; c < 3; c++) q[c] = p[c] < -(s - r) ? -(s - r) : p[c] > s - r ? s - r : p[c];
            V n = vnorm(v3(p[0] - q[0], p[1] - q[1], p[2] - q[2]));
            if (pos_add(m, v3(q[0] + r * n.x, q[1] + r * n.y, q[2] + r * n.z)) < 0) return 0;
            grid[i * 6 + j] = n;
        }
        for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) {
            int q[4] = { base + i * 6 + j, base + (i + 1) * 6 + j, base + (i + 1) * 6 + j + 1, base + i * 6 + j + 1 };
            int idx[4], g[4][2];
            if (sign > 0) { for (int c = 0; c < 4; c++) idx[c] = q[c]; }
            else { idx[0] = q[0]; idx[1] = q[3]; idx[2] = q[2]; idx[3] = q[1]; }
            /* the uv runs with the grid so the atlas cell covers the whole face, bevel and all */
            if (sign > 0) { int G[4][2] = { { i, j }, { i + 1, j }, { i + 1, j + 1 }, { i, j + 1 } }; memcpy(g, G, sizeof g); }
            else { int G[4][2] = { { i, j }, { i, j + 1 }, { i + 1, j + 1 }, { i + 1, j } }; memcpy(g, G, sizeof g); }
            double uv[4][2];
            V nrm[4];
            for (int c = 0; c < 4; c++) { uv[c][0] = (T[g[c][0]] + s) / d; uv[c][1] = (T[g[c][1]] + s) / d; nrm[c] = grid[idx[c] - base]; }
            if (!poly_add(m, 4, idx, (const double (*)[2])uv, nrm, CN_TEX_DIE_ATLAS, fs)) return 0;
        }
    }
    return 1;
}

/* ---- the die's values -------------------------------------------------------------------- */
int cn_die_slot(int axis, int sign)
{
    for (int i = 0; i < 6; i++) if (AX[i][0] == axis && AX[i][1] == (sign < 0 ? -1 : 1)) return i;
    return -1;
}
int cn_die_slot_of_up(int up) { return up < 0 || up > 5 ? -1 : cn_die_slot(up >> 1, up & 1 ? -1 : 1); }

void cn_die_cells(int axis, int sign, int value, uint8_t cells[6])
{
    if (value < 1 || value > 6) value = 1;
    if (axis < 0 || axis > 2) axis = 2;
    sign = sign < 0 ? -1 : 1;
    int lo[2], n = 0;   /* the two other pairs, by their lower value */
    for (int a = 1; a <= 3; a++) if (a != value && a != 7 - value) lo[n++] = a;
    const int u = (axis + 1) % 3, w = (axis + 2) % 3;
    /* the value on the positive side of each axis */
    int pos[3];
    pos[axis] = sign > 0 ? value : 7 - value;
    pos[u] = lo[0];
    pos[w] = lo[1];
    /* right-handed: the axes (signed) that carry 1, 2 and 3 make a positive
     * determinant; if not, turn the w pair over */
    for (int pass = 0; pass < 2; pass++) {
        double e[3][3] = { { 0 } };
        for (int ax = 0; ax < 3; ax++) {
            int vp = pos[ax], vn = 7 - vp;
            if (vp <= 3) e[vp - 1][ax] = 1; else e[vn - 1][ax] = -1;
        }
        double det = e[0][0] * (e[1][1] * e[2][2] - e[1][2] * e[2][1]) - e[0][1] * (e[1][0] * e[2][2] - e[1][2] * e[2][0])
                   + e[0][2] * (e[1][0] * e[2][1] - e[1][1] * e[2][0]);
        if (det > 0) break;
        pos[w] = 7 - pos[w];
    }
    for (int i = 0; i < 6; i++) cells[i] = (uint8_t)(AX[i][1] > 0 ? pos[AX[i][0]] : 7 - pos[AX[i][0]]);
}
int cn_die_value(const uint8_t cells[6], int axis, int sign)
{
    int s = cn_die_slot(axis, sign);
    return s < 0 ? 0 : cells[s];
}

/* ---- bodies ---------------------------------------------------------------------------------- */
static void obj_clear(CnObj *o)
{
    memset(o, 0, sizeof *o);
    o->tint[0] = 0; o->tint[1] = 4; o->tint[2] = 3;   /* the study's untinted bodies */
    o->kmul[0] = 1; o->kmul[1] = 0;
}

void cn_geom_cup_obj(CnObj *o, float R, int count, uint32_t seed, float x, float y, int out, float toward_x)
{
    obj_clear(o);
    const double rc = R * CN_CUP_RC, h = R * CN_CUP_TALL;
    o->kind = CN_OBJ_CUP; o->value = (uint8_t)count; o->tex_seed = seed;
    o->crown_big = R < 40;
    o->x = o->home_x = x; o->y = o->home_y = y;
    o->R = R; o->h = (float)h;
    o->shadow_r = R;
    if (!out) return;
    /* a seat with no dice: its cup lies on its side across the seat, dim. Both
     * rims touch the table (the cup is a cone, so its axis dips toward the
     * crown), the body is centred on the seat, and the mouth faces the ring's
     * centre, turned a little by seed. */
    o->out = 1;
    o->tint[0] = 4; o->tint[1] = 14; o->tint[2] = 15; o->kmul[0] = .45f; o->kmul[1] = .5f;
    const double hh = cn_m_sqrt(h * h - (R - rc) * (R - rc)), side = toward_x < x ? 1 : -1;
    const double yaw = side * CN_PI / 2 + (cn_geom_hash((int32_t)seed, 77, 977) - .5) * .5;
    const double cy = cn_m_cos(yaw), sy = cn_m_sin(yaw);
    /* THE LIE, EXACTLY. Both rims touch when the side's lowest line is level:
     * the axis dips by a with tan a = (R - rc) / h, and the mouth's centre is
     * R cos a up. The study dipped it by sin a = (R - rc) / h and lifted it R,
     * which left the mouth .38 points and the crown .28 off the table at
     * R = 43 (docs_pkgB.md); tests/cn_geom_test.c measures the contact. */
    const double sl = cn_m_sqrt(h * h + (R - rc) * (R - rc)), ca = h / sl, sa = (R - rc) / sl;
    const double c[3][3] = { { 1, 0, 0 }, { 0, -sa, ca }, { 0, -ca, -sa } };
    for (int k = 0; k < 3; k++) {
        o->rot[k * 3 + 0] = (float)(c[k][0] * cy - c[k][1] * sy);
        o->rot[k * 3 + 1] = (float)(c[k][0] * sy + c[k][1] * cy);
        o->rot[k * 3 + 2] = (float)c[k][2];
    }
    o->has_rot = 1; o->lift = (float)(R * ca);
    /* the axis's middle over the seat (the study's place for it) */
    const double ax0 = c[2][0] * cy - c[2][1] * sy, ax1 = c[2][0] * sy + c[2][1] * cy;
    o->x = (float)(x - ax0 * h / 2); o->y = (float)(y - ax1 * h / 2);
    o->shadow_r = (float)(hh * .55); o->shadow_dx = (float)(ax0 * h / 2); o->shadow_dy = (float)(ax1 * h / 2);
}

void cn_geom_die_obj(CnObj *o, float d, int value, uint32_t seed, float x, float y, float yaw)
{
    obj_clear(o);
    o->kind = CN_OBJ_DIE; o->value = (uint8_t)value; o->tex_seed = seed;
    o->x = o->home_x = x; o->y = o->home_y = y; o->yaw = yaw;
    o->lift = d / 2; o->d = d;
    o->shadow_r = d * .7f;
    cn_die_cells(2, 1, value, o->cells);
}

void cn_geom_occluder(const CnObj *o, float pad_x, float out[5])
{
    if (o->kind == CN_OBJ_DIE) {
        float h = o->lift - o->d / 2;
        out[0] = o->x + pad_x; out[1] = o->y; out[2] = o->d * .62f; out[3] = h > 0 ? h : 0; out[4] = .5f;
    } else if (o->out) {
        out[0] = o->x + o->shadow_dx + pad_x; out[1] = o->y + o->shadow_dy; out[2] = o->shadow_r > 0 ? o->shadow_r : o->R; out[3] = 0; out[4] = .45f;
    } else {
        out[0] = o->x + pad_x; out[1] = o->y; out[2] = o->R; out[3] = o->lift > 0 ? o->lift : 0; out[4] = .55f;
    }
}

/* the body's turn (rot, or yaw about z), then the peek's turn about x */
static V turn(const CnObj *o, V p)
{
    V q;
    if (o->has_rot) {
        const float *r = o->rot;
        q = v3(r[0] * p.x + r[3] * p.y + r[6] * p.z, r[1] * p.x + r[4] * p.y + r[7] * p.z, r[2] * p.x + r[5] * p.y + r[8] * p.z);
    } else {
        double c = cn_m_cos(o->yaw), s = cn_m_sin(o->yaw);
        q = v3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
    }
    return q;
}
static V rot_x(V p, double a)
{
    double c = cn_m_cos(a), s = cn_m_sin(a);
    return v3(p.x, p.y * c - p.z * s, p.y * s + p.z * c);
}

void cn_geom_emit(const CnMesh *m, const CnObj *o, float pad_x, const int tex_ids[CN_TEX_SLOTS],
                  float *verts, int vbase, float *faces, int fbase)
{
    const int tilted = o->tilt_angle != 0;
    int vi = vbase, fi = fbase;
    for (int p = 0; p < m->npoly; p++) {
        const int c0 = m->first[p], k = m->count[p], base = vi;
        V P[3];
        for (int c = 0; c < k; c++) {
            const float *s = m->pos[m->at[c0 + c]];
            V q = turn(o, v3(s[0], s[1], s[2]));
            if (tilted) {
                q.y -= o->tilt_hinge_y; q = rot_x(q, o->tilt_angle);
                q.y += o->tilt_hinge_y - o->tilt_back; q.z += o->tilt_lift;
            }
            q = v3(q.x + o->x + pad_x, q.y + o->y, q.z + o->lift);
            if (c < 3) P[c] = q;
            float *out = verts + (size_t)(vi + c) * CN_GEOM_VF;
            out[0] = (float)q.x; out[1] = (float)q.y; out[2] = (float)q.z;
        }
        V flat = vnorm(cross(vsub(P[1], P[0]), vsub(P[2], P[0])));
        for (int c = 0; c < k; c++) {
            V n = flat;
            if (m->smooth[p]) {
                const float *s = m->nrm[c0 + c];
                n = turn(o, v3(s[0], s[1], s[2]));
                if (tilted) n = rot_x(n, o->tilt_angle);
                n = vnorm(n);
            }
            float *out = verts + (size_t)(vi + c) * CN_GEOM_VF;
            out[3] = (float)n.x; out[4] = (float)n.y; out[5] = (float)n.z;
        }
        vi += k;
        /* a die's face reads its value's cell of the six-wide atlas */
        int cell = m->face[p] >= 0 ? o->cells[m->face[p]] - 1 : -1;
        float uv[256][2];
        for (int c = 0; c < k; c++) {
            float u = m->uv[c0 + c][0], v = m->uv[c0 + c][1];
            if (cell >= 0) u = (float)((cell + .5 / 128 + u * (1 - 1.0 / 128)) / 6);
            uv[c][0] = u; uv[c][1] = v;
        }
        const float tex = (float)tex_ids[m->tex[p]];
        for (int c = 1; c + 1 < k; c++) {
            float *F = faces + (size_t)fi * CN_GEOM_FF;
            F[0] = (float)base; F[1] = (float)(base + c); F[2] = (float)(base + c + 1);
            F[3] = uv[0][0]; F[4] = uv[0][1]; F[5] = uv[c][0]; F[6] = uv[c][1]; F[7] = uv[c + 1][0]; F[8] = uv[c + 1][1];
            F[9] = tex; F[10] = o->tint[0]; F[11] = o->tint[1]; F[12] = o->tint[2]; F[13] = o->kmul[0]; F[14] = o->kmul[1];
            F[15] = CN_GEOM_F_BODY;
            fi++;
        }
    }
}

/* ---- a baked throw at any instant ---------------------------------------------------------------- */
void cn_geom_pose_at(const float *frames, const uint8_t *phase, int n, double T, int k, CnPose *out)
{
    memset(out, 0, sizeof *out);
    if (!frames || n < 1 || k < 0 || k >= CN_ROLL_POSES) { out->rot[0] = out->rot[4] = out->rot[8] = 1; return; }
    double f = T * CN_ROLL_HZ;
    if (f > n - 1) f = n - 1;
    if (f < 0) f = 0;
    const int i = (int)cn_m_floor(f), j = i + 1 < n ? i + 1 : n - 1;
    const double a = f - i;
    const float *A = frames + (size_t)i * CN_ROLL_FRAME_FLOATS + k * CN_ROLL_POSE_FLOATS;
    const float *B = frames + (size_t)j * CN_ROLL_FRAME_FLOATS + k * CN_ROLL_POSE_FLOATS;
    double qx = A[3], qy = A[4], qz = A[5], qw = A[6], rx = B[3], ry = B[4], rz = B[5], rw = B[6];
    if (qx * rx + qy * ry + qz * rz + qw * rw < 0) { rx = -rx; ry = -ry; rz = -rz; rw = -rw; }
    qx += (rx - qx) * a; qy += (ry - qy) * a; qz += (rz - qz) * a; qw += (rw - qw) * a;
    double l = cn_m_sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!(l > 0)) l = 1;
    const double x = qx / l, y = qy / l, z = qz / l, w = qw / l;
    for (int c = 0; c < 3; c++) out->p[c] = (float)(A[c] + (B[c] - A[c]) * a);
    const double R[9] = {
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
        2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
        2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y),
    };
    for (int c = 0; c < 9; c++) out->rot[c] = (float)R[c];
    out->phase = phase ? phase[i] : 0;
    out->frame = (uint16_t)i;
}

void cn_geom_place(CnObj *o, const CnPose *p, int up)
{
    const int idle = p->phase == CN_RP_IDLE;
    if (o->kind == CN_OBJ_CUP && idle) {
        o->has_rot = 0; o->yaw = 0; o->x = o->home_x; o->y = o->home_y; o->lift = 0;
        return;
    }
    o->has_rot = 1; memcpy(o->rot, p->rot, sizeof o->rot);
    o->x = p->p[0]; o->y = p->p[1]; o->lift = p->p[2];
    if (o->kind == CN_OBJ_DIE && idle && up >= 0) o->value = (uint8_t)cn_die_value(o->cells, up >> 1, up & 1 ? -1 : 1);
}

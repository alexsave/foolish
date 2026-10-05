/* A six-seat table for the renderer, built the way docs/UI.html builds it: the
 * study's cup (a wall of 36 facets in two rows, a fillet of three rows into a flat
 * crown, and the inside: the inner wall, the domed floor and the rim) and its die
 * (a rounded cube, 25 quads a face), every seat's cup the same size, each seat's
 * five dice in front of its cup, the contact footprints, the study's light and
 * the leaning head's eye (VIEW_B). Textures are of the study's sizes (the cup's side
 * and inside 1024 by 512, the crown and floor 256 by 256, the die atlas 768 by 128,
 * each with a normal map) and filled with noise, which costs what a painting costs.
 * Shared by cn_scene_test (its golden) and cn_scene_bench (its speed).
 *
 * Every number here is made with its own float arithmetic (a sine series, no libm),
 * so the frame, and the golden hash over it, is the same on every compiler. */
#ifndef CN_SCENE_FRAME_H
#define CN_SCENE_FRAME_H

#include "../src/cn_scene.h"
#include <string.h>

#define CNF_SEATS 6
#define CNF_DICE 5
#define CNF_SEGS 36
#define CNF_PI 3.14159265358979f

/* sine and cosine: the angle brought into [-pi, pi], then the series to x^13 */
static inline float cnf_sin(float x)
{
    while (x > CNF_PI) x -= 2 * CNF_PI;
    while (x < -CNF_PI) x += 2 * CNF_PI;
    float x2 = x * x, t = x, s = x;
    for (int k = 1; k <= 6; k++) { t = -t * x2 / (float)((2 * k) * (2 * k + 1)); s += t; }
    return s;
}
static inline float cnf_cos(float x) { return cnf_sin(x + CNF_PI / 2); }

/* the builder's state: the arrays it writes and how far it has got */
typedef struct { float *V; float *F; int nv, nf, vcap, fcap; } CnfOut;
typedef struct { float m[3][3]; float px, py, pz; } CnfPose;   /* world = m * p + pos */

static inline void cnf_xf(const CnfPose *P, const float p[3], float o[3])
{
    for (int i = 0; i < 3; i++) o[i] = P->m[i][0] * p[0] + P->m[i][1] * p[1] + P->m[i][2] * p[2];
    o[0] += P->px; o[1] += P->py; o[2] += P->pz;
}
static inline void cnf_xn(const CnfPose *P, const float n[3], float o[3])
{
    for (int i = 0; i < 3; i++) o[i] = P->m[i][0] * n[0] + P->m[i][1] * n[1] + P->m[i][2] * n[2];
    float l = __builtin_sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]); if (l > 0) { o[0] /= l; o[1] /= l; o[2] /= l; }
}
/* a convex polygon of k corners (object space), its uv and normals (0: the face's own), as a fan */
static inline void cnf_poly(CnfOut *o, const CnfPose *P, int k, const float (*p)[3], const float (*uv)[2], const float (*nrm)[3], int tex)
{
    if (o->nv + k > o->vcap || o->nf + k - 2 > o->fcap) return;
    float w[64][3], fn[3];
    for (int i = 0; i < k; i++) cnf_xf(P, p[i], w[i]);
    { float a[3] = { w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2] }, b[3] = { w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2] };
      fn[0] = a[1] * b[2] - a[2] * b[1]; fn[1] = a[2] * b[0] - a[0] * b[2]; fn[2] = a[0] * b[1] - a[1] * b[0];
      float l = __builtin_sqrtf(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]); if (l > 0) { fn[0] /= l; fn[1] /= l; fn[2] /= l; } }
    int base = o->nv;
    for (int i = 0; i < k; i++) {
        float *v = &o->V[(size_t)o->nv * CN_SCENE_VF], n[3];
        if (nrm) cnf_xn(P, nrm[i], n); else { n[0] = fn[0]; n[1] = fn[1]; n[2] = fn[2]; }
        v[0] = w[i][0]; v[1] = w[i][1]; v[2] = w[i][2]; v[3] = n[0]; v[4] = n[1]; v[5] = n[2]; o->nv++;
    }
    for (int i = 1; i + 1 < k; i++) {
        float *f = &o->F[(size_t)o->nf * CN_SCENE_FF];
        f[0] = (float)base; f[1] = (float)(base + i); f[2] = (float)(base + i + 1);
        f[3] = uv[0][0]; f[4] = uv[0][1]; f[5] = uv[i][0]; f[6] = uv[i][1]; f[7] = uv[i + 1][0]; f[8] = uv[i + 1][1];
        f[9] = (float)tex; f[10] = 0; f[11] = 4; f[12] = 3; f[13] = 1; f[14] = 0;
        f[15] = CN_SCENE_F_CULL | CN_SCENE_F_CAST | CN_SCENE_F_RECEIVE;
        o->nf++;
    }
}

/* a wall: a ring from radius r0 at z0 to r1 at z1, two rows of facets; flip winds it inward */
static inline void cnf_wall(CnfOut *o, const CnfPose *P, float r0, float r1, float z0, float z1, float nz, int tex, int flip)
{
    for (int i = 0; i < CNF_SEGS; i++) {
        float a0 = i * 2 * CNF_PI / CNF_SEGS, a1 = (i + 1) * 2 * CNF_PI / CNF_SEGS, c0 = cnf_cos(a0), s0 = cnf_sin(a0), c1 = cnf_cos(a1), s1 = cnf_sin(a1);
        float u0 = (float)i / CNF_SEGS, u1 = (float)(i + 1) / CNF_SEGS;
        float l = __builtin_sqrtf(1 + nz * nz);
        for (int r = 0; r < 2; r++) {
            float t0 = r * .5f, t1 = (r + 1) * .5f, ra = r0 + (r1 - r0) * t0, rb = r0 + (r1 - r0) * t1, za = z0 + (z1 - z0) * t0, zb = z0 + (z1 - z0) * t1;
            float p[4][3] = { { ra * c0, ra * s0, za }, { ra * c1, ra * s1, za }, { rb * c1, rb * s1, zb }, { rb * c0, rb * s0, zb } };
            float uv[4][2] = { { u0, 1 - t0 }, { u1, 1 - t0 }, { u1, 1 - t1 }, { u0, 1 - t1 } };
            float n[4][3] = { { c0 / l, s0 / l, nz / l }, { c1 / l, s1 / l, nz / l }, { c1 / l, s1 / l, nz / l }, { c0 / l, s0 / l, nz / l } };
            if (flip) {
                float q[4][3], qu[4][2], qn[4][3];
                for (int k = 0; k < 4; k++) { memcpy(q[k], p[3 - k], sizeof q[k]); memcpy(qu[k], uv[3 - k], sizeof qu[k]); for (int c = 0; c < 3; c++) qn[k][c] = -n[3 - k][c]; }
                cnf_poly(o, P, 4, (const float (*)[3])q, (const float (*)[2])qu, (const float (*)[3])qn, tex);
            } else cnf_poly(o, P, 4, (const float (*)[3])p, (const float (*)[2])uv, (const float (*)[3])n, tex);
        }
    }
}
/* a disc: one polygon (a fan from its first corner), or a fan about a centre raised by dome */
static inline void cnf_disc(CnfOut *o, const CnfPose *P, float rad, float z, int tex, int flip, float dome)
{
    float p[CNF_SEGS][3], uv[CNF_SEGS][2];
    for (int i = 0; i < CNF_SEGS; i++) {
        float a = i * 2 * CNF_PI / CNF_SEGS, c = cnf_cos(a), s = cnf_sin(a);
        int k = flip ? CNF_SEGS - 1 - i : i;
        p[k][0] = rad * c; p[k][1] = rad * s; p[k][2] = z; uv[k][0] = .5f + .5f * c; uv[k][1] = .5f + .5f * s;
    }
    if (dome == 0) { cnf_poly(o, P, CNF_SEGS, (const float (*)[3])p, (const float (*)[2])uv, 0, tex); return; }
    for (int i = 0; i < CNF_SEGS; i++) {
        int j = (i + 1) % CNF_SEGS;
        float q[3][3] = { { 0, 0, z - dome }, { p[i][0], p[i][1], p[i][2] }, { p[j][0], p[j][1], p[j][2] } };
        float qu[3][2] = { { .5f, .5f }, { uv[i][0], uv[i][1] }, { uv[j][0], uv[j][1] } };
        cnf_poly(o, P, 3, (const float (*)[3])q, (const float (*)[2])qu, 0, tex);
    }
}
/* the study's cup: mouth radius R on the table, crown radius .72 R at 2.1 R, a fillet of .09 R into the
 * flat crown, and the inside (wall .06 R thick, the floor domed 0) */
static inline void cnf_cup(CnfOut *o, const CnfPose *P, float R, int side, int crown, int inner, int floor)
{
    float rc = R * .72f, h = R * 2.1f, nz = (R - rc) / h, f = R * .09f, rcf = rc + (R - rc) * f / h, ccx = rcf - f, cz = h - f;
    cnf_wall(o, P, R, rcf, 0, cz, nz, side, 0);
    for (int i = 0; i < CNF_SEGS; i++) {
        float a0 = i * 2 * CNF_PI / CNF_SEGS, a1 = (i + 1) * 2 * CNF_PI / CNF_SEGS, c0 = cnf_cos(a0), s0 = cnf_sin(a0), c1 = cnf_cos(a1), s1 = cnf_sin(a1);
        float u0 = (float)i / CNF_SEGS, u1 = (float)(i + 1) / CNF_SEGS;
        for (int r = 0; r < 3; r++) {
            float p0 = (CNF_PI / 2) * r / 3, p1 = (CNF_PI / 2) * (r + 1) / 3, cp0 = cnf_cos(p0), sp0 = cnf_sin(p0), cp1 = cnf_cos(p1), sp1 = cnf_sin(p1);
            float ra = ccx + f * cp0, rb = ccx + f * cp1, za = cz + f * sp0, zb = cz + f * sp1, v0 = .05f - r / 3.f * .05f, v1 = .05f - (r + 1) / 3.f * .05f;
            float p[4][3] = { { ra * c0, ra * s0, za }, { ra * c1, ra * s1, za }, { rb * c1, rb * s1, zb }, { rb * c0, rb * s0, zb } };
            float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
            float n[4][3] = { { cp0 * c0, cp0 * s0, sp0 }, { cp0 * c1, cp0 * s1, sp0 }, { cp1 * c1, cp1 * s1, sp1 }, { cp1 * c0, cp1 * s0, sp1 } };
            cnf_poly(o, P, 4, (const float (*)[3])p, (const float (*)[2])uv, (const float (*)[3])n, side);
        }
    }
    cnf_disc(o, P, ccx, h, crown, 0, 0);
    float t = R * .06f, hf = h - t;
    cnf_wall(o, P, R - t, rc - t, 0, hf, nz, inner, 1);
    cnf_disc(o, P, rc - t, hf, floor, 1, 0);
    for (int i = 0; i < CNF_SEGS; i++) {
        float a0 = i * 2 * CNF_PI / CNF_SEGS, a1 = (i + 1) * 2 * CNF_PI / CNF_SEGS, c0 = cnf_cos(a0), s0 = cnf_sin(a0), c1 = cnf_cos(a1), s1 = cnf_sin(a1);
        float u0 = (float)i / CNF_SEGS, u1 = (float)(i + 1) / CNF_SEGS;
        float p[4][3] = { { R * c0, R * s0, 0 }, { (R - t) * c0, (R - t) * s0, 0 }, { (R - t) * c1, (R - t) * s1, 0 }, { R * c1, R * s1, 0 } };
        float uv[4][2] = { { u0, .52f }, { u0, .48f }, { u1, .48f }, { u1, .52f } };
        cnf_poly(o, P, 4, (const float (*)[3])p, (const float (*)[2])uv, 0, inner);
    }
}
/* the study's die: side d, corners rounded at .18 d, face f of the atlas on each side */
static inline void cnf_die(CnfOut *o, const CnfPose *P, float d, int atlas)
{
    float s = d / 2, r = d * .18f, k = r * (1 - .70710678f);
    const float T[6] = { -s, -s + k, -(s - r), s - r, s - k, s };
    static const int AX[6][2] = { { 2, 1 }, { 2, -1 }, { 1, -1 }, { 0, 1 }, { 1, 1 }, { 0, -1 } };
    for (int fc = 0; fc < 6; fc++) {
        int axis = AX[fc][0], sign = AX[fc][1], u = (axis + 1) % 3, w = (axis + 2) % 3;
        float gp[6][6][3], gn[6][6][3];
        for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) {
            float p[3] = { 0, 0, 0 }; p[axis] = s * sign; p[u] = T[i]; p[w] = T[j];
            float q[3], dl[3], n = 0;
            for (int c = 0; c < 3; c++) { float lim = s - r; q[c] = p[c] < -lim ? -lim : p[c] > lim ? lim : p[c]; dl[c] = p[c] - q[c]; n += dl[c] * dl[c]; }
            n = __builtin_sqrtf(n); if (n == 0) n = 1;
            for (int c = 0; c < 3; c++) { gn[i][j][c] = dl[c] / n; gp[i][j][c] = q[c] + r * gn[i][j][c]; }
        }
        float cell = (float)fc;
        for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) {
            int qi[4][2] = { { i, j }, { i + 1, j }, { i + 1, j + 1 }, { i, j + 1 } };
            if (sign < 0) { int t0 = qi[1][0], t1 = qi[1][1]; qi[1][0] = qi[3][0]; qi[1][1] = qi[3][1]; qi[3][0] = t0; qi[3][1] = t1; }
            float p[4][3], uv[4][2], n[4][3];
            for (int c = 0; c < 4; c++) {
                memcpy(p[c], gp[qi[c][0]][qi[c][1]], sizeof p[c]); memcpy(n[c], gn[qi[c][0]][qi[c][1]], sizeof n[c]);
                float a = (T[qi[c][0]] + s) / d, b = (T[qi[c][1]] + s) / d;
                uv[c][0] = (cell + .5f / 128 + a * (1 - 1.f / 128)) / 6; uv[c][1] = b;
            }
            cnf_poly(o, P, 4, (const float (*)[3])p, (const float (*)[2])uv, (const float (*)[3])n, atlas);
        }
    }
}

/* xorshift32 noise for the textures, one draw a statement */
static uint32_t cnf_rs = 2463534242u;
static inline uint32_t cnf_rnd(void) { cnf_rs ^= cnf_rs << 13; cnf_rs ^= cnf_rs >> 17; cnf_rs ^= cnf_rs << 5; return cnf_rs; }
/* a texture of noise with the study's statistics (measured from its uploads): the cup's green-black
 * verdigris (red about 55) whose normal map is flat but for one texel in twenty at +-1 or 2, or the die's
 * ivory (red about 178) whose map is flat at seven texels in ten, +-1 to 3 elsewhere and +-12 at one in a
 * hundred (a pip's rim). A rougher map than that makes the light's branches random, which no real map does. */
static inline int8_t cnf_bump(uint32_t r, int die)
{
    uint32_t p = r & 1023, s = (r >> 10) & 1, m = (r >> 11) & 3;
    int v = die ? (p < 717 ? 0 : p < 1013 ? 1 + (int)(m % 3) : 12) : (p < 970 ? 0 : 1 + (int)(m & 1));
    return (int8_t)(s ? -v : v);
}
static inline int cnf_tex(int w, int h, int die)
{
    int id = cn_scene_tex_new(w, h, 1);
    if (id < 0) return -1;
    uint8_t *p = cn_scene_tex_rgba(id); int8_t *b = cn_scene_tex_bump(id);
    const int base = die ? 160 : 40;
    for (int i = 0; i < w * h; i++) {
        uint32_t r = cnf_rnd();
        p[i * 4] = (uint8_t)(base + (r & 31)); p[i * 4 + 1] = (uint8_t)(base + 20 + ((r >> 5) & 31)); p[i * 4 + 2] = (uint8_t)(base + 10 + ((r >> 10) & 31)); p[i * 4 + 3] = 255;
        uint32_t q = cnf_rnd();
        b[i * 2] = cnf_bump(q, die);
        b[i * 2 + 1] = cnf_bump(q >> 13, die);
    }
    return id;
}

/* THE TEXTURES of a table: per seat the cup's side, crown, inside and floor and one die atlas (the study
 * gives every die its own atlas; one a seat is the host's likely economy and costs the same to draw).
 * shared != 0 makes one of each for every seat. 0 on success, -1 when the arena has no room. */
typedef struct { int side[CNF_SEATS], crown[CNF_SEATS], inner[CNF_SEATS], floor_[CNF_SEATS], atlas[CNF_SEATS]; } CnfTex;
static inline int cnf_textures(CnfTex *t, int shared)
{
    cnf_rs = 2463534242u;
    for (int s = 0; s < CNF_SEATS; s++) {
        if (shared && s) { t->side[s] = t->side[0]; t->crown[s] = t->crown[0]; t->inner[s] = t->inner[0]; t->floor_[s] = t->floor_[0]; t->atlas[s] = t->atlas[0]; continue; }
        if ((t->side[s] = cnf_tex(1024, 512, 0)) < 0 || (t->crown[s] = cnf_tex(256, 256, 0)) < 0 || (t->inner[s] = cnf_tex(1024, 512, 0)) < 0 ||
            (t->floor_[s] = cnf_tex(256, 256, 0)) < 0 || (t->atlas[s] = cnf_tex(768, 128, 1)) < 0) return -1;
    }
    return 0;
}

/* THE FRAME on a board W by H points at dpr, the shadow map sr: my seat at the bottom, the five others
 * round an ellipse above, each seat's dice in a ring in front of its cup. shake (0..1) lifts and tilts
 * every cup as a throw would (0: standing on the table). cnf_build begins the frame and writes it (the
 * faces written, also cnf_nf, and the vertices cnf_nv), or -1 when it does not fit (cn_scene_begin refused
 * it); cnf_frame also renders it on this thread and returns the faces drawn. */
#define CNF_VCAP 40000
#define CNF_FCAP 20000
static int cnf_nv, cnf_nf;
static inline int cnf_build(const CnfTex *t, int W, int H, int pad, float dpr, int sr, float shake)
{
    const float L[3] = { -.45f, -.55f, 1 };
    const float eyeX = W / 2.f, eyeY = H / 2.f + .85f * H, hc = 560;
    if (!cn_scene_begin(W, H, pad, dpr, eyeX, eyeY, hc, L[0], L[1], L[2], sr, .55f, CNF_VCAP, CNF_FCAP)) return -1;
    CnfOut o = { cn_scene_verts(), cn_scene_faces(), 0, 0, CNF_VCAP, CNF_FCAP };
    const float R = 44, d_me = 24, d_far = 18;
    float seat[CNF_SEATS][2];
    seat[0][0] = W / 2.f; seat[0][1] = H * .78f;
    for (int j = 0; j < CNF_SEATS - 1; j++) {
        float a = CNF_PI * (.92f - .84f * j / (CNF_SEATS - 2));     /* the upper half of the ellipse, left to right */
        seat[j + 1][0] = W / 2.f + cnf_cos(a) * (W / 2.f - R - 14); seat[j + 1][1] = H * .52f - cnf_sin(a) * H * .27f;
    }
    for (int s = 0; s < CNF_SEATS; s++) {
        float cx = seat[s][0], cy = seat[s][1], dd = s ? d_far : d_me;
        /* the cup: a yaw, and in a throw a lift and a tilt about x */
        float yaw = .3f * s, tilt = shake * (.25f + .05f * s), lift = shake * (R * .8f);
        float cyw = cnf_cos(yaw), syw = cnf_sin(yaw), ct = cnf_cos(tilt), st = cnf_sin(tilt);
        CnfPose P = { { { cyw, -syw * ct, syw * st }, { syw, cyw * ct, -cyw * st }, { 0, st, ct } }, cx, cy, lift };
        cnf_cup(&o, &P, R, t->side[s], t->crown[s], t->inner[s], t->floor_[s]);
        cn_scene_occluder(cx, cy, R, lift, .55f);
        /* the dice: a ring in front of the cup, toward me */
        for (int k = 0; k < CNF_DICE; k++) {
            float a = 2 * CNF_PI * k / CNF_DICE + .4f * s, dx = cx + cnf_cos(a) * dd * 1.1f, dy = cy + R + dd * 1.6f + cnf_sin(a) * dd * 1.1f, dyaw = .7f * k + .2f * s;
            float c = cnf_cos(dyaw), sn = cnf_sin(dyaw);
            CnfPose D = { { { c, -sn, 0 }, { sn, c, 0 }, { 0, 0, 1 } }, dx, dy, dd / 2 };
            cnf_die(&o, &D, dd, t->atlas[s]);
            cn_scene_occluder(dx, dy, dd * .62f, 0, .5f);
        }
    }
    cnf_nv = o.nv; cnf_nf = o.nf;
    return o.nf;
}
static inline int cnf_frame(const CnfTex *t, int W, int H, int pad, float dpr, int sr, float shake)
{
    if (cnf_build(t, W, H, pad, dpr, sr, shake) < 0) return -1;
    return cn_scene_render(cnf_nv, cnf_nf);
}

/* FNV-1a over the framebuffer */
static inline uint64_t cnf_hash(void)
{
    const uint8_t *p = cn_scene_fb(); size_t n = (size_t)cn_scene_fb_w() * cn_scene_fb_h() * 4;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

#endif

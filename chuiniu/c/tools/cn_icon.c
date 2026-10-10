/* Chui Niu - the app icon, drawn by the table's own renderer.
 *
 *     make -C chuiniu/c icons          every PNG both asset catalogues ship, and a contact sheet
 *     ./build/cn_icon --pack build/cn_tex.pack --one 1024 1024 out.png
 *
 * A BUILD-TIME TOOL, host only, never in the app: the PNGs live in the asset
 * catalogues and Apple reads them out of the bundle. It exists so the icon is the
 * game's own dice (cn_geom.c's mesh, cn_tex.c's bone and pips from the same baked
 * pack, through cn_scene.c's rasterizer and shadow map) rather than a drawing that
 * drifts from the table the moment the table changes. The shape is uttt's
 * (uttt/c/tools/uttt_icon.c): one tool, every size drawn at ITS OWN size.
 *
 * THE PICTURE is the study's locked one (docs/UI.html, the Icon tab): two bone dice
 * in the deep, a 5 and a wild 1, with the caustic over them. No cup, because at 29pt
 * a cup is a thimble. The 1 is the icon's one drop of blood: its pip is the atlas's
 * own deep red (cn_tex.c, the 1 "in blood"). The deep is the study's .icon gradient
 * and its sea tile (TEX.sea, ported below), screen-blended and faded down the frame.
 * No pirate anything, no published product's dice or box (chuiniu/LEGAL.md).
 *
 * THE CAMERA. cn_scene.c draws what an eye over the table sees ON THE TABLE'S
 * PLANE (a shifted lens: every point lands where the ray from the eye through it
 * meets z = 0). The icon wants an eye that looks AT the dice, from above and in
 * front. Both are the same rays from the same eye, so the oblique camera's picture
 * is the table-plane picture re-sampled along those rays: a homography, exactly
 * what the game's leaning head does to its screen (cn_cam.h). So: put the eye where
 * the camera is, render the table-plane picture of the part of the table the frame
 * sees, and for every output pixel follow its ray down to z = 0 and read the picture
 * there. A body's pixel is where its ray says.
 *
 * SIZES. Each catalogue size is drawn at its own pixel size, supersampled
 * (SUB by SUB rays a pixel, bilinear reads of a table-plane picture of at least
 * TEXEL device pixels a pixel's footprint), never shrunk from the 1024.
 * PNGs are written with zlib (RGB, opaque: the App Store refuses an icon with
 * alpha), deterministic for a given pack and compiler. */
#include "../src/cn_scene.h"
#include "../src/cn_geom.h"
#include "../src/cn_cam.h"
#include "../src/cn_tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- the scene, in board points (z up, y toward the viewer) ------------------------- */
#define DIE_D      30.0      /* a die's side                                             */
#define NDICE      2
/* the study's icon(): the 5 low on the left turned -12 degrees, the 1 higher on the right turned 9 */
static const struct { int value; uint32_t seed; double x, y, yaw; } DICE[NDICE] = {
    { 5, 201, -19.0,  9.0, -0.21 },
    { 1, 202,  19.0, -9.0,  0.16 },
};
/* the camera: high and in front, so the up faces read; a little from the right */
#define CAM_EL     (1.12)    /* elevation, radians (about 64 degrees)                    */
#define CAM_AZ     (0.18)
#define CAM_DIST   340.0     /* points from the eye to the target                        */
#define MARGIN     0.13      /* of the frame clear round the bodies, at the tighter edges */
#define TARGET_Z   12.0      /* where the camera looks; the frame is then fitted to the bodies */
#define LIGHT_X    (-0.45)   /* toward the light, z = 1: the study's LIGHT, up and to the left */
#define LIGHT_Y    (-0.55)
#define SHADOW_DARK .62f
#define SOFT_PT    2.0f      /* the shadow's extra softness, a box radius in points (3 passes) */
/* the deep (UI.html .icon): a gradient down the frame, and the sea tile screen-blended
 * over it at CAUSTIC, faded out by CAUSTIC_FADE of the height; the tile spans
 * CAUSTIC_TILE of the frame's width (the study's 160px tile on a 60px icon); the dice
 * take it at CAUSTIC_BODY of that, the light on them from the same water */
#define CAUSTIC      .8
#define CAUSTIC_FADE .8
#define CAUSTIC_TILE (160.0 / 60.0)
#define CAUSTIC_BODY .35

#define SUB   4              /* rays a pixel, per axis                                   */
#define TEXEL 1.25           /* table-plane device pixels a ray step, at the nearest ray */

typedef struct { double x, y, z; } V3;
static V3 v3(double x, double y, double z) { V3 v = { x, y, z }; return v; }
static V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 vmul(V3 a, double k) { return v3(a.x * k, a.y * k, a.z * k); }
static V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V3 vnorm(V3 a) { double l = sqrt(a.x * a.x + a.y * a.y + a.z * a.z); return l > 0 ? vmul(a, 1 / l) : a; }

/* ---- textures through the renderer ---------------------------------------------------- */
static int sink_alloc(void *ctx, int w, int h, int has_bump, CnTexImage *img)
{
    (void)ctx;
    int id = cn_scene_tex_new(w, h, has_bump);
    if (id < 0) return -1;
    img->w = w; img->h = h; img->rgba = cn_scene_tex_rgba(id); img->bump = has_bump ? cn_scene_tex_bump(id) : 0;
    return id;
}

/* ---- the sea: the study's TEX.sea, one ridged fbm, warped, raised to a power ------------
 * (UI.html: SEA = { light [46 120 112], cells 5, warp .06, warpCells 3, power 4, seed 41 }),
 * its hash cn_tex.c's port of the study's, baked once to a wrapping tile */
#define SEA_N 512
static float SEA[SEA_N * SEA_N];
static double sea_hash(int ix, int iy, int seed) { return cn_tex_hash((uint32_t)ix, (uint32_t)iy, (uint32_t)seed) / 4294967296.0; }
static int wrapi(int i, int p) { return ((i % p) + p) % p; }
static double vnoise(double x, double y, int px, int py, int seed)
{
    const int x0 = (int)floor(x), y0 = (int)floor(y);
    const double fx = x - x0, fy = y - y0, sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double a = sea_hash(wrapi(x0, px), wrapi(y0, py), seed), b = sea_hash(wrapi(x0 + 1, px), wrapi(y0, py), seed);
    const double c = sea_hash(wrapi(x0, px), wrapi(y0 + 1, py), seed), d = sea_hash(wrapi(x0 + 1, px), wrapi(y0 + 1, py), seed);
    const double top = a + (b - a) * sx, bot = c + (d - c) * sx;
    return top + (bot - top) * sy;
}
static double fbm(double u, double v, int cx, int cy, int octaves, int seed)
{
    double sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves; o++) {
        sum += amp * vnoise(u * cx, v * cy, cx, cy, seed + o * 101);
        norm += amp; amp *= .5; cx *= 2; cy *= 2;
    }
    return sum / norm;
}
static void sea_bake(void)
{
    for (int j = 0; j < SEA_N; j++)
        for (int i = 0; i < SEA_N; i++) {
            const double u = (i + .5) / SEA_N, v = (j + .5) / SEA_N;
            const double wu = u + (fbm(u, v, 3, 3, 2, 41 + 7) - .5) * .06, wv = v + (fbm(u, v, 3, 3, 2, 41 + 11) - .5) * .06;
            const double ridge = 1 - fabs(2 * fbm(wu, wv, 5, 5, 3, 41) - 1);
            SEA[j * SEA_N + i] = (float)pow(ridge, 4);
        }
}
/* the caustic's strength at a frame point (fractions of the width and height), bilinear, wrapping */
static double sea_at(double fx, double fy, double aspect)
{
    const double u = fx / CAUSTIC_TILE * SEA_N, v = fy / aspect / CAUSTIC_TILE * SEA_N;
    const int x0 = (int)floor(u - .5), y0 = (int)floor(v - .5);
    const double ax = u - .5 - x0, ay = v - .5 - y0;
    const double a = SEA[wrapi(y0, SEA_N) * SEA_N + wrapi(x0, SEA_N)], b = SEA[wrapi(y0, SEA_N) * SEA_N + wrapi(x0 + 1, SEA_N)];
    const double c = SEA[wrapi(y0 + 1, SEA_N) * SEA_N + wrapi(x0, SEA_N)], d = SEA[wrapi(y0 + 1, SEA_N) * SEA_N + wrapi(x0 + 1, SEA_N)];
    return (a + (b - a) * ax) + ((c + (d - c) * ax) - (a + (b - a) * ax)) * ay;
}
/* the deep under the dice at a frame point: #134a4e, #0a2a2e at 55%, #04100f */
static void deep_rgb(double fy, double out[3])
{
    static const double s[3][3] = { { 0x13, 0x4a, 0x4e }, { 0x0a, 0x2a, 0x2e }, { 0x04, 0x10, 0x0f } };
    const int k = fy < .55 ? 0 : 1;
    const double t = k == 0 ? fy / .55 : (fy - .55) / .45;
    for (int c = 0; c < 3; c++) out[c] = (s[k][c] + (s[k + 1][c] - s[k][c]) * fmin(1, fmax(0, t))) / 255;
}
/* screen-blend the caustic (the study's light [46 120 112]) at strength k */
static void caustic(double rgb[3], double c, double k)
{
    static const double light[3] = { 46 / 255.0, 120 / 255.0, 112 / 255.0 };
    for (int i = 0; i < 3; i++) rgb[i] = 1 - (1 - rgb[i]) * (1 - light[i] * c * k);
}

/* ---- PNG through zlib --------------------------------------------------------------------- */
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *fp, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, fp);
    if (n) fwrite(d, 1, n, fp);
    uLong c = crc32(0, h + 4, 4); c = crc32(c, d, n);
    uint8_t t[4]; be32(t, (uint32_t)c); fwrite(t, 1, 4, fp);
}
static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    /* every row filtered Paeth, which suits a smooth picture */
    size_t row = (size_t)w * 3, raw_n = (row + 1) * h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *o = raw + (row + 1) * y; o[0] = 4;
        const uint8_t *cur = rgb + row * y, *up = y ? rgb + row * (y - 1) : 0;
        for (size_t i = 0; i < row; i++) {
            int a = i >= 3 ? cur[i - 3] : 0, b = up ? up[i] : 0, c = up && i >= 3 ? up[i - 3] : 0;
            int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            int pr = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            o[1 + i] = (uint8_t)(cur[i] - pr);
        }
    }
    uLongf zn = compressBound((uLong)raw_n);
    uint8_t *z = malloc(zn);
    if (!z || compress2(z, &zn, raw, (uLong)raw_n, 9) != Z_OK) { free(raw); free(z); return 0; }
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, fp);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(fp, "IHDR", ih, 13);
    chunk(fp, "IDAT", z, (uint32_t)zn);
    chunk(fp, "IEND", 0, 0);
    int ok = fclose(fp) == 0;
    free(raw); free(z);
    return ok;
}

/* ---- one icon ---------------------------------------------------------------------------- */
static uint8_t *ARENA;
static size_t ARENA_BYTES = (size_t)1536 << 20;
static CnTexPack PACK;
static CnMesh DIE_MESH;
static float VB[NDICE * CN_MESH_MAX_CORNER * CN_GEOM_VF], FB[NDICE * CN_MESH_MAX_CORNER * CN_GEOM_FF];

/* THE SOFT SHADOW. The renderer's shadow on the table softens over two map texels,
 * a lamp's edge; a hero shot wants a wider penumbra. The table's pixels (alpha below
 * 255: the shadow's darkness; a body's are opaque) are blurred among themselves only,
 * three box passes each way weighted by "is table", so no body's silhouette bleeds a
 * dark halo onto the table round it. Bodies are left as they are. */
static void box1(float *v, float *w, int n, int stride, int r, float *tv, float *tw)
{
    double sv = 0, sw = 0;
    for (int i = -r; i <= r; i++) { int k = i < 0 ? 0 : i >= n ? n - 1 : i; sv += v[(size_t)k * stride]; sw += w[(size_t)k * stride]; }
    for (int i = 0; i < n; i++) {
        tv[i] = (float)sv; tw[i] = (float)sw;
        int a = i - r < 0 ? 0 : i - r, b = i + r + 1 >= n ? n - 1 : i + r + 1;
        sv += v[(size_t)b * stride] - v[(size_t)a * stride]; sw += w[(size_t)b * stride] - w[(size_t)a * stride];
    }
    for (int i = 0; i < n; i++) { v[(size_t)i * stride] = tv[i]; w[(size_t)i * stride] = tw[i]; }
}
static void soften(uint8_t *fb, int fw, int fh, int r)
{
    if (r < 1) return;
    size_t n = (size_t)fw * fh;
    float *v = malloc(n * sizeof *v), *w = malloc(n * sizeof *w);
    int m = fw > fh ? fw : fh;
    float *tv = malloc((size_t)m * sizeof *tv), *tw = malloc((size_t)m * sizeof *tw);
    if (!v || !w || !tv || !tw) { free(v); free(w); free(tv); free(tw); return; }
    for (size_t i = 0; i < n; i++) { int t = fb[i * 4 + 3] < 255; w[i] = (float)t; v[i] = t ? fb[i * 4 + 3] : 0; }
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < fh; y++) box1(v + (size_t)y * fw, w + (size_t)y * fw, fw, 1, r, tv, tw);
        for (int x = 0; x < fw; x++) box1(v + x, w + x, fh, fw, r, tv, tw);
    }
    for (size_t i = 0; i < n; i++)
        if (fb[i * 4 + 3] < 255) { fb[i * 4] = 0; fb[i * 4 + 1] = 3; fb[i * 4 + 2] = 2; fb[i * 4 + 3] = (uint8_t)(w[i] > 0 ? v[i] / w[i] + .5f : 0); }
    free(v); free(w); free(tv); free(tw);
}

/* the camera's frame: the ray through (sx, sy) of the picture, each -1 to 1, y down */
typedef struct { V3 fwd, right, up; double uc, vc, hw, hh; } Lens;
static V3 lens_ray(const Lens *l, double sx, double sy)
{
    return vadd(l->fwd, vadd(vmul(l->right, l->uc + sx * l->hw), vmul(l->up, -(l->vc + sy * l->hh))));
}

static int draw(int W, int H, uint8_t *rgb)
{
    /* the camera, about the dice's middle at the origin */
    const double aspect = (double)W / H;
    const V3 target = v3(0, 0, TARGET_Z);
    const V3 eye = vadd(target, vmul(v3(sin(CAM_AZ) * cos(CAM_EL), cos(CAM_AZ) * cos(CAM_EL), sin(CAM_EL)), CAM_DIST));
    const V3 fwd = vnorm(vsub(target, eye)), right = vnorm(vcross(v3(0, 0, 1), fwd)), up = vcross(fwd, right);
    const V3 L = vnorm(v3(LIGHT_X, LIGHT_Y, 1));

    /* the dice, emitted once where they stand: their picture on the table sizes the board */
    CnObj die[NDICE];
    int tex[NDICE][CN_TEX_SLOTS];
    memset(tex, 0, sizeof tex);
    const int nv = NDICE * DIE_MESH.ncorner, nf = NDICE * DIE_MESH.ntri;
    for (int k = 0; k < NDICE; k++) {
        cn_geom_die_obj(&die[k], (float)DIE_D, DICE[k].value, DICE[k].seed, (float)DICE[k].x, (float)DICE[k].y, (float)DICE[k].yaw);
        cn_geom_emit(&DIE_MESH, &die[k], 0, tex[k], VB, k * DIE_MESH.ncorner, FB, k * DIE_MESH.ntri);
    }

    /* THE FRAME: the dice's extent as the camera sees it (u right, v down, as tangents),
     * fitted to the picture's shape with MARGIN of it clear at the tighter pair of edges,
     * so every aspect the catalogues ask for holds the same subject */
    double u0 = 1e30, u1 = -1e30, w0 = 1e30, w1 = -1e30;
    for (int i = 0; i < nv; i++) {
        const float *v = VB + (size_t)i * CN_GEOM_VF;
        const V3 d = vsub(v3(v[0], v[1], v[2]), eye);
        const double f = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
        const double u = (d.x * right.x + d.y * right.y + d.z * right.z) / f, w = -(d.x * up.x + d.y * up.y + d.z * up.z) / f;
        u0 = fmin(u0, u); u1 = fmax(u1, u); w0 = fmin(w0, w); w1 = fmax(w1, w);
    }
    Lens lens = { fwd, right, up, (u0 + u1) / 2, (w0 + w1) / 2, 0, 0 };
    lens.hh = fmax((w1 - w0) / 2, (u1 - u0) / 2 / aspect) / (1 - 2 * MARGIN);
    lens.hw = lens.hh * aspect;

    /* THE BOARD: every vertex where the eye sees it on the table and where the light
     * throws it (its shadow), with room round them, cut to the table the frame's four
     * corner rays bound. A ray that lands off the board meets bare table. */
    double bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30;
    for (int i = 0; i < nv; i++) {
        const float *v = VB + (size_t)i * CN_GEOM_VF;
        const double k = eye.z / (eye.z - v[2]);
        const double px[2] = { eye.x + (v[0] - eye.x) * k, v[0] - L.x / L.z * v[2] };
        const double py[2] = { eye.y + (v[1] - eye.y) * k, v[1] - L.y / L.z * v[2] };
        for (int j = 0; j < 2; j++) {
            bx0 = fmin(bx0, px[j]); bx1 = fmax(bx1, px[j]);
            by0 = fmin(by0, py[j]); by1 = fmax(by1, py[j]);
        }
    }
    bx0 -= 30; by0 -= 30; bx1 += 30; by1 += 30;
    double fx0 = 1e30, fy0 = 1e30, fx1 = -1e30, fy1 = -1e30;
    for (int k = 0; k < 4; k++) {
        const double sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1;
        const V3 d = lens_ray(&lens, sx, sy);
        if (d.z >= -1e-3) { fprintf(stderr, "cn_icon: a frame corner looks above the table\n"); return 0; }
        const double t = -eye.z / d.z;
        fx0 = fmin(fx0, eye.x + d.x * t); fx1 = fmax(fx1, eye.x + d.x * t);
        fy0 = fmin(fy0, eye.y + d.y * t); fy1 = fmax(fy1, eye.y + d.y * t);
    }
    bx0 = floor(fmax(bx0, fx0)); by0 = floor(fmax(by0, fy0)); bx1 = fmin(bx1, fx1); by1 = fmin(by1, fy1);
    const int bw = (int)ceil(bx1 - bx0), bh = (int)ceil(by1 - by0);
    /* the picture's density: TEXEL device pixels for the smallest step between two rays on
     * the table, which is at the frame's foot (the nearest table) */
    double foot = 1e30;
    for (int k = 0; k < 3; k++) {
        const double sx = k - 1.0;
        const V3 d0 = lens_ray(&lens, sx, 1), d1 = lens_ray(&lens, sx, 1 - 2.0 / (H * SUB));
        foot = fmin(foot, fabs(d1.y * (-eye.z / d1.z) - d0.y * (-eye.z / d0.z)));
    }
    float dpr = (float)(TEXEL / foot);
    if (bw * dpr > 8000) dpr = 8000.f / bw;
    if (bh * dpr > 8000) dpr = 8000.f / bh;

    cn_scene_init(ARENA, ARENA_BYTES);
    CnTexSink sink = { 0, sink_alloc };
    for (int k = 0; k < NDICE; k++) {
        for (int i = 0; i < CN_TEX_SLOTS; i++) tex[k][i] = -1;
        tex[k][CN_TEX_DIE_ATLAS] = cn_tex_upload(&PACK, &sink, &(CnTexSpec){ CN_TEX_DIE, DICE[k].seed, 0, 0, 0, 1 });
        if (tex[k][CN_TEX_DIE_ATLAS] < 0) { fprintf(stderr, "cn_icon: no room for a die's texture\n"); return 0; }
    }
    if (!cn_scene_begin(bw, bh, 0, dpr, (float)(eye.x - bx0), (float)(eye.y - by0), (float)eye.z,
                        (float)L.x, (float)L.y, (float)L.z, CN_SCENE_SHADOW_MAX, SHADOW_DARK, nv, nf)) {
        fprintf(stderr, "cn_icon: the frame does not fit (%dx%d points at %.2f)\n", bw, bh, dpr); return 0;
    }
    /* now onto the board, its corner at (bx0, by0), with the textures' ids */
    for (int k = 0; k < NDICE; k++) {
        float occ[5];
        die[k].x -= (float)bx0; die[k].y -= (float)by0;
        cn_geom_occluder(&die[k], 0, occ);
        cn_scene_occluder(occ[0], occ[1], occ[2], occ[3], occ[4]);
        cn_geom_emit(&DIE_MESH, &die[k], 0, tex[k], cn_scene_verts(), k * DIE_MESH.ncorner, cn_scene_faces(), k * DIE_MESH.ntri);
    }
    if (cn_scene_render(nv, nf) < 0) { fprintf(stderr, "cn_icon: render failed\n"); return 0; }
    uint8_t *fb = cn_scene_fb();
    const int fw = cn_scene_fb_w(), fh = cn_scene_fb_h();
    soften(fb, fw, fh, (int)(SOFT_PT * dpr + .5f));

    /* every pixel: SUB by SUB rays down to the table, the picture read there (bilinear)
     * over the deep, the caustic screen-blended over both */
    for (int py = 0; py < H; py++)
        for (int px = 0; px < W; px++) {
            double acc[3] = { 0, 0, 0 };
            for (int j = 0; j < SUB; j++)
                for (int i = 0; i < SUB; i++) {
                    const double ffx = (px + (i + .5) / SUB) / W, ffy = (py + (j + .5) / SUB) / H;
                    const V3 d = lens_ray(&lens, ffx * 2 - 1, ffy * 2 - 1);
                    const double t = -eye.z / d.z, x = eye.x + d.x * t, y = eye.y + d.y * t;
                    const double c = sea_at(ffx, ffy, aspect), mask = fmax(0, 1 - ffy / CAUSTIC_FADE);
                    double tb[3];
                    deep_rgb(ffy, tb);
                    caustic(tb, c, CAUSTIC * mask);
                    const double fx = (x - bx0) * dpr - .5, fy = (y - by0) * dpr - .5;
                    const int x0 = (int)floor(fx), y0 = (int)floor(fy);
                    const double ax = fx - x0, ay = fy - y0;
                    for (int q = 0; q < 4; q++) {
                        const int xx = x0 + (q & 1), yy = y0 + (q >> 1);
                        const double wq = (q & 1 ? ax : 1 - ax) * (q >> 1 ? ay : 1 - ay);
                        if (xx < 0 || yy < 0 || xx >= fw || yy >= fh) {   /* off the board: bare deep */
                            for (int k = 0; k < 3; k++) acc[k] += wq * tb[k];
                            continue;
                        }
                        const uint8_t *p = fb + ((size_t)yy * fw + xx) * 4;
                        const double a = p[3] / 255.0;
                        double s[3] = { p[0] / 255.0, p[1] / 255.0, p[2] / 255.0 };
                        if (p[3] == 255) caustic(s, c, CAUSTIC * CAUSTIC_BODY * mask);   /* a die, in the same water */
                        for (int k = 0; k < 3; k++) acc[k] += wq * (tb[k] * (1 - a) + s[k] * a);
                    }
                }
            for (int k = 0; k < 3; k++) {
                const double v = fmin(1, fmax(0, acc[k] / (SUB * SUB)));
                rgb[((size_t)py * W + px) * 3 + k] = (uint8_t)(v * 255 + .5);
            }
        }
    return 1;
}

static int draw_to(int W, int H, const char *path, uint8_t **keep)
{
    uint8_t *rgb = malloc((size_t)W * H * 3);
    if (!rgb || !draw(W, H, rgb) || !write_png(path, rgb, W, H)) { fprintf(stderr, "cn_icon: %s failed\n", path); free(rgb); return 0; }
    printf("  %s  %dx%d\n", path, W, H);
    if (keep) *keep = rgb; else free(rgb);
    return 1;
}

/* ---- the contact sheet: every size at its own pixel size, then the 29pt one at 1x, each
 *      on Messages' dark and light drawer --------------------------------------------- */
typedef struct { int w, h; uint8_t *rgb; } Img;
static void blit(uint8_t *sh, int sw, int shh, const Img *im, int x, int y, int scale_down, int round_r)
{
    int w = im->w / scale_down, h = im->h / scale_down;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            if (x + i >= sw || y + j >= shh) continue;
            /* the system's rounded mask, roughly, so the sheet shows what a person sees */
            if (round_r) {
                int cx = i < round_r ? round_r - i : i >= w - round_r ? i - (w - 1 - round_r) : 0;
                int cy = j < round_r ? round_r - j : j >= h - round_r ? j - (h - 1 - round_r) : 0;
                if (cx && cy && cx * cx + cy * cy > round_r * round_r) continue;
            }
            unsigned s[3] = { 0, 0, 0 };
            for (int b = 0; b < scale_down; b++)
                for (int a = 0; a < scale_down; a++)
                    for (int c = 0; c < 3; c++) s[c] += im->rgb[(((size_t)(j * scale_down + b)) * im->w + i * scale_down + a) * 3 + c];
            for (int c = 0; c < 3; c++) sh[(((size_t)(y + j)) * sw + x + i) * 3 + c] = (uint8_t)((s[c] + scale_down * scale_down / 2) / (scale_down * scale_down));
        }
}

int main(int argc, char **argv)
{
    const char *pack = 0, *ext = 0, *app = 0, *sheet = 0;
    int one_w = 0, one_h = 0; const char *one = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pack") && i + 1 < argc) pack = argv[++i];
        else if (!strcmp(argv[i], "--ext") && i + 1 < argc) ext = argv[++i];
        else if (!strcmp(argv[i], "--app") && i + 1 < argc) app = argv[++i];
        else if (!strcmp(argv[i], "--sheet") && i + 1 < argc) sheet = argv[++i];
        else if (!strcmp(argv[i], "--one") && i + 3 < argc) { one_w = atoi(argv[++i]); one_h = atoi(argv[++i]); one = argv[++i]; }
        else { fprintf(stderr, "usage: cn_icon --pack P (--one W H OUT.png | --ext DIR --app DIR [--sheet OUT.png])\n"); return 2; }
    }
    if (!pack || (!one && (!ext || !app))) { fprintf(stderr, "usage: cn_icon --pack P (--one W H OUT.png | --ext DIR --app DIR [--sheet OUT.png])\n"); return 2; }

    FILE *fp = fopen(pack, "rb");
    if (!fp) { fprintf(stderr, "cn_icon: cannot open %s (make tex)\n", pack); return 1; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)n);
    if (!bytes || fread(bytes, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return 1; }
    fclose(fp);
    if (cn_tex_pack_open(&PACK, bytes, (uint32_t)n) != CN_TEX_OK) { fprintf(stderr, "cn_icon: %s is not a texture pack\n", pack); return 1; }
    if (!cn_geom_die_mesh(&DIE_MESH, DIE_D)) { fprintf(stderr, "cn_icon: the die mesh does not fit\n"); return 1; }
    sea_bake();
    ARENA = aligned_alloc(16, ARENA_BYTES);
    if (!ARENA) return 1;

    if (one) return draw_to(one_w, one_h, one, 0) ? 0 : 1;

    /* the catalogues' files, as their Contents.json name them */
    static const struct { int w, h; const char *name; int app; } F[] = {
        {   58,   58, "sq-58.png", 0 },      {   87,   87, "sq-87.png", 0 },
        {  120,   90, "msg-120x90.png", 0 }, {  180,  135, "msg-180x135.png", 0 },
        {  134,  100, "msg-134x100.png", 0 },{  148,  110, "msg-148x110.png", 0 },
        {   54,   40, "msg-54x40.png", 0 },  {   81,   60, "msg-81x60.png", 0 },
        {   64,   48, "msg-64x48.png", 0 },  {   96,   72, "msg-96x72.png", 0 },
        { 1024,  768, "msg-1024x768.png", 0 },
        { 1024, 1024, "AppIcon-1024.png", 1 },
    };
    enum { NF = sizeof F / sizeof F[0] };
    Img im[NF];
    for (int i = 0; i < NF; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", F[i].app ? app : ext, F[i].name);
        im[i].w = F[i].w; im[i].h = F[i].h;
        if (!draw_to(F[i].w, F[i].h, path, &im[i].rgb)) return 1;
    }
    if (sheet) {
        /* row 1 and 2: every small size at its pixel size on the dark and the light drawer;
         * row 3: the 1024s at half size */
        const int SW = 1200, SH = 40 + 2 * 220 + 560;
        uint8_t *sh = malloc((size_t)SW * SH * 3);
        if (!sh) return 1;
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                uint8_t g = y < 20 + 220 ? 28 : y < 20 + 440 ? 242 : 46;
                for (int c = 0; c < 3; c++) sh[((size_t)y * SW + x) * 3 + c] = g;
            }
        for (int row = 0; row < 2; row++) {
            int x = 20, y = 30 + row * 220;
            for (int i = 0; i < NF; i++) {
                if (F[i].w > 200) continue;
                int r = F[i].w == F[i].h ? F[i].w / 5 : F[i].h / 5;
                blit(sh, SW, SH, &im[i], x, y, 1, r);
                x += F[i].w + 14;
            }
            /* the 29pt icon at 1x, and the 27x20pt at 1x: what a non-retina eye gets */
            blit(sh, SW, SH, &im[0], 20, y + 150, 2, 6);
            blit(sh, SW, SH, &im[6], 70, y + 150, 2, 4);
        }
        blit(sh, SW, SH, &im[NF - 2], 20, 40 + 440 + 10, 2, 0);
        blit(sh, SW, SH, &im[NF - 1], 560, 40 + 440 + 10, 2, 512 / 5);
        if (!write_png(sheet, sh, SW, SH)) return 1;
        printf("  %s  %dx%d (the contact sheet)\n", sheet, SW, SH);
    }
    return 0;
}

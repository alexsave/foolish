/* Chui Niu - the app icon, drawn by the table's own renderer.
 *
 *     make -C chuiniu/c icons          every PNG both asset catalogues ship, and a contact sheet
 *     ./build/cn_icon --pack build/cn_tex.pack --one 1024 1024 out.png
 *
 * A BUILD-TIME TOOL, host only, never in the app: the PNGs live in the asset
 * catalogues and Apple reads them out of the bundle. It exists so the icon is the
 * game's own cup and die (cn_geom.c's meshes, cn_tex.c's verdigris and bone, from
 * the same baked pack, through cn_scene.c's rasterizer and shadow map) rather than
 * a drawing that drifts from the table the moment the table changes. The shape is
 * uttt's (uttt/c/tools/uttt_icon.c): one tool, every size drawn at ITS OWN size.
 *
 * THE PICTURE. One verdigris cup, mouth down, tipped a little toward us about the
 * far edge of its mouth (the peek's own turn, cn_cam_peek_tilt), and one bone die
 * in front of it and to the left with a 5 up, on the drowned table (the deep's
 * teal, a pool of cold light behind the cup) under one light high on the left, the
 * shadows falling to the right and softened (soften() below). No pirate anything,
 * no published product's cup or box (chuiniu/LEGAL.md).
 *
 * THE CAMERA. cn_scene.c draws what an eye over the table sees ON THE TABLE'S
 * PLANE (a shifted lens: every point lands where the ray from the eye through it
 * meets z = 0). A hero shot wants an eye that looks AT the cup, low and in front.
 * Both are the same rays from the same eye, so the oblique camera's picture is the
 * table-plane picture re-sampled along those rays: a homography, exactly what the
 * game's leaning head does to its screen (cn_cam.h). So: put the eye where the
 * camera is, render the table-plane picture of the part of the table the camera's
 * frame sees, and for every output pixel follow its ray down to z = 0 and read the
 * picture there. Nothing is faked; a body's pixel is where its ray says.
 *
 * SIZES. Each catalogue size is drawn at its own pixel size, supersampled
 * (SUB by SUB rays a pixel, bilinear reads of a table-plane picture of at least
 * TEXEL device pixels a pixel's footprint), never shrunk from the 1024.
 * PNGs are written with zlib (RGB, opaque: the App Store refuses an icon with
 * alpha), deterministic for a given pack and compiler. */
#include "../src/cn_scene.h"
/* cn_geom.h's face slot and cn_tex.h's texture kind are both named CN_TEX_DIE, so
 * the two headers cannot share a file; the slot is renamed here, for this file
 * only, until the headers stop colliding (reported to their owners, docs/ICON.md). */
#define CN_TEX_DIE CN_TEX_SLOT_DIE
#include "../src/cn_geom.h"
#undef CN_TEX_DIE
#include "../src/cn_cam.h"
#include "../src/cn_tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- the scene, in board points (z up, y toward the viewer) ------------------------- */
#define CUP_R      40.0      /* the cup's mouth radius                                   */
#define DIE_D      31.0      /* the die's side: big beside the cup, so a face reads at 29pt */
#define DIE_VALUE  5
#define DIE_YAW    (-0.30)   /* radians, turned so two side faces show                   */
#define CUP_TIP    (0.20)    /* radians the cup is tipped toward us (about 11 degrees)   */
#define CUP_SEED   1u        /* the verdigris's offset (a seat's texture seed)          */
#define DIE_SEED   11u
#define DIE_X      (-38.0)   /* the die's centre from the cup's                          */
#define DIE_Y      54.0
/* the camera: above and in front, looking at a point between the cup and the die */
#define CAM_EL     (0.95)    /* elevation, radians (about 54 degrees)                    */
#define CAM_AZ     (0.25)    /* turned a little to the right of straight on              */
#define CAM_DIST   340.0     /* points from the eye to the target                        */
#define MARGIN     0.10      /* of the frame clear round the bodies, at the tighter edges */
/* where the camera looks; the frame is then fitted to the bodies, so this sets the angle only */
#define TARGET_X   (-8.0)
#define TARGET_Y   12.0
#define TARGET_Z   30.0
#define LIGHT_X    (-0.6)    /* toward the light, z = 1: high, on the left, a little behind */
#define LIGHT_Y    (-0.2)
/* the table: a pool of the cold light behind the cup and to its right, so the cup's
 * shaded side stands against the brightest of it */
#define POOL_X     (-50.0)   /* the pool's centre is at (-POOL_X, -POOL_Y) from the cup's */
#define POOL_Y     90.0
#define POOL_R     240.0
#define SHADOW_DARK .62f
#define VIGNETTE   .55
#define SOFT_PT    2.5f      /* the shadow's extra softness, a box radius in points (3 passes) */

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

/* ---- the table under everything: the deep's colours, a pool of the light behind the cup --- */
static void table_rgb(double x, double y, double out[3])
{
    /* x, y relative to the cup's centre, points */
    const double px = x + POOL_X, py = y + POOL_Y;
    double r = sqrt(px * px * .8 + py * py) / POOL_R;
    double t = r > 1 ? 1 : r;
    t = t * t * (3 - 2 * t);
    static const double lit[3] = { 0x25, 0x72, 0x72 }, deep[3] = { 0x03, 0x0f, 0x10 };
    for (int c = 0; c < 3; c++) out[c] = (lit[c] + (deep[c] - lit[c]) * t) / 255;
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
static CnMesh CUP_MESH, DIE_MESH;
static float VB[2 * CN_MESH_MAX_CORNER * CN_GEOM_VF], FB[2 * CN_MESH_MAX_CORNER * CN_GEOM_FF];

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
    /* the camera, about the cup's centre at the origin */
    const double aspect = (double)W / H;
    const double el = CAM_EL, az = CAM_AZ;
    const V3 target = v3(TARGET_X, TARGET_Y, TARGET_Z);
    const V3 eye = vadd(target, vmul(v3(sin(az) * cos(el), cos(az) * cos(el), sin(el)), CAM_DIST));
    const V3 fwd = vnorm(vsub(target, eye)), right = vnorm(vcross(v3(0, 0, 1), fwd)), up = vcross(fwd, right);
    const V3 L = vnorm(v3(LIGHT_X, LIGHT_Y, 1));

    /* the bodies, emitted once where they stand: their picture on the table sizes the board */
    CnObj cup, die;
    cn_geom_cup_obj(&cup, (float)CUP_R, -1, CUP_SEED, 0, 0, 0, 0);
    CnPeek pk = cn_cam_peek_tilt((float)CUP_R, (float)CUP_TIP, (float)CUP_TIP);
    cup.tilt_angle = pk.angle; cup.tilt_hinge_y = pk.hinge_y;
    cn_geom_die_obj(&die, (float)DIE_D, DIE_VALUE, DIE_SEED, (float)DIE_X, (float)DIE_Y, (float)DIE_YAW);
    const int nv = CUP_MESH.ncorner + DIE_MESH.ncorner, nf = CUP_MESH.ntri + DIE_MESH.ntri;
    int tex[CN_TEX_SLOTS] = { 0 };
    cn_geom_emit(&CUP_MESH, &cup, 0, tex, VB, 0, FB, 0);
    cn_geom_emit(&DIE_MESH, &die, 0, tex, VB, CUP_MESH.ncorner, FB, CUP_MESH.ntri);

    /* THE FRAME: the bodies' extent as the camera sees it (u right, v down, as tangents),
     * fitted to the picture's shape with MARGIN of its height clear at the tighter pair
     * of edges, so every aspect the catalogues ask for holds the same subject */
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
    bx0 = fmax(bx0, fx0); by0 = fmax(by0, fy0); bx1 = fmin(bx1, fx1); by1 = fmin(by1, fy1);
    bx0 = floor(bx0); by0 = floor(by0);
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
    tex[CN_TEX_CUP_SIDE] = cn_tex_upload(&PACK, &sink, &(CnTexSpec){ CN_TEX_SIDE, CUP_SEED, -1, 0, 0, 1 });
    tex[CN_TEX_CUP_CROWN] = cn_tex_upload(&PACK, &sink, &(CnTexSpec){ CN_TEX_CROWN_T, CUP_SEED, -1, 0, CN_TEX_NUMERAL_SMALL, 1 });
    tex[CN_TEX_CUP_INNER] = cn_tex_upload(&PACK, &sink, &(CnTexSpec){ CN_TEX_INNER, CUP_SEED, -1, 0, 0, 1 });
    tex[CN_TEX_CUP_FLOOR] = cn_tex_upload(&PACK, &sink, &(CnTexSpec){ CN_TEX_FLOOR, CUP_SEED, -1, 0, 0, 1 });
    tex[CN_TEX_SLOT_DIE] = cn_tex_upload(&PACK, &sink, &(CnTexSpec){ CN_TEX_DIE, DIE_SEED, 0, 0, 0, 1 });
    for (int i = 0; i < CN_TEX_SLOTS; i++)
        if (tex[i] < 0) { fprintf(stderr, "cn_icon: no room for a texture\n"); return 0; }
    if (!cn_scene_begin(bw, bh, 0, dpr, (float)(eye.x - bx0), (float)(eye.y - by0), (float)eye.z,
                        (float)L.x, (float)L.y, (float)L.z, CN_SCENE_SHADOW_MAX, SHADOW_DARK, nv, nf)) {
        fprintf(stderr, "cn_icon: the frame does not fit (%dx%d points at %.2f)\n", bw, bh, dpr); return 0;
    }
    /* now onto the board, its corner at (bx0, by0), with the textures' ids */
    CnObj *body[2] = { &cup, &die };
    for (int b = 0; b < 2; b++) {
        float occ[5];
        body[b]->x -= (float)bx0; body[b]->y -= (float)by0;
        cn_geom_occluder(body[b], 0, occ);
        cn_scene_occluder(occ[0], occ[1], occ[2], occ[3], occ[4]);
    }
    cn_geom_emit(&CUP_MESH, &cup, 0, tex, cn_scene_verts(), 0, cn_scene_faces(), 0);
    cn_geom_emit(&DIE_MESH, &die, 0, tex, cn_scene_verts(), CUP_MESH.ncorner, cn_scene_faces(), CUP_MESH.ntri);
    if (cn_scene_render(nv, nf) < 0) { fprintf(stderr, "cn_icon: render failed\n"); return 0; }
    uint8_t *fb = cn_scene_fb();
    const int fw = cn_scene_fb_w(), fh = cn_scene_fb_h();
    soften(fb, fw, fh, (int)(SOFT_PT * dpr + .5f));

    /* every pixel: SUB by SUB rays down to the table, the picture read there (bilinear)
     * over the table's own colour */
    for (int py = 0; py < H; py++)
        for (int px = 0; px < W; px++) {
            double acc[3] = { 0, 0, 0 };
            for (int j = 0; j < SUB; j++)
                for (int i = 0; i < SUB; i++) {
                    const double sx = (px + (i + .5) / SUB) / W * 2 - 1, sy = (py + (j + .5) / SUB) / H * 2 - 1;
                    const V3 d = lens_ray(&lens, sx, sy);
                    const double t = -eye.z / d.z, x = eye.x + d.x * t, y = eye.y + d.y * t;
                    double tb[3];
                    table_rgb(x, y, tb);
                    const double fx = (x - bx0) * dpr - .5, fy = (y - by0) * dpr - .5;
                    const int x0 = (int)floor(fx), y0 = (int)floor(fy);
                    const double ax = fx - x0, ay = fy - y0;
                    for (int q = 0; q < 4; q++) {
                        const int xx = x0 + (q & 1), yy = y0 + (q >> 1);
                        const double wq = (q & 1 ? ax : 1 - ax) * (q >> 1 ? ay : 1 - ay);
                        if (xx < 0 || yy < 0 || xx >= fw || yy >= fh) {   /* off the board: bare table */
                            for (int c = 0; c < 3; c++) acc[c] += wq * tb[c];
                            continue;
                        }
                        const uint8_t *p = fb + ((size_t)yy * fw + xx) * 4;
                        const double a = p[3] / 255.0;
                        for (int c = 0; c < 3; c++) acc[c] += wq * (tb[c] * (1 - a) + p[c] / 255.0 * a);
                    }
                }
            /* the deck's vignette (UI.html .stage::after), in the frame */
            const double vx = ((px + .5) / W - .5) * 2, vy = ((py + .5) / H - .45) * 2;
            const double vr = sqrt((vx * vx + vy * vy) * .7), vt = fmin(1, fmax(0, (vr - .55) / .75));
            for (int c = 0; c < 3; c++) {
                const double v = fmin(1, fmax(0, acc[c] / (SUB * SUB) * (1 - VIGNETTE * vt)));
                rgb[((size_t)py * W + px) * 3 + c] = (uint8_t)(v * 255 + .5);
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
    if (!cn_geom_cup_mesh(&CUP_MESH, CUP_R, CUP_R * CN_CUP_RC, CUP_R * CN_CUP_TALL, CN_CUP_SEGS, CUP_R * CN_CUP_WALL, DIE_D * CN_DOME)
        || !cn_geom_die_mesh(&DIE_MESH, DIE_D)) { fprintf(stderr, "cn_icon: a mesh does not fit\n"); return 1; }
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

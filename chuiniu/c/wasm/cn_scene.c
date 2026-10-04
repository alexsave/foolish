/* The scene renderer for the study, in C for speed. Browser-only (a Swift host
 * would hand the same meshes to Metal); freestanding, no libc beyond memset.
 *
 * WHAT IT IS. A software rasterizer with a shadow map, the way a small engine
 * does it. The page hands over, each frame, a vertex array (world position and
 * normal) and a face array (three vertex indices, texture coordinates, the
 * texture, the tint and the shade's multiplier, and flags), and reads back an
 * RGBA framebuffer.
 *
 *   pass 1  THE SHADOW MAP. The light is directional (one cold light over the
 *           table). Every casting triangle is rasterized orthographically into
 *           a depth map seen from the light: the nearest distance along the
 *           light at each map texel.
 *   pass 2  THE PICTURE. Every triangle is scanned pixel by pixel inside its
 *           edges; the texture coordinate, the normal and the world position are
 *           interpolated in 1/w so they are right under perspective; the texel
 *           is read with bilinear filtering; the normal is bent by the normal
 *           map along the triangle's tangent frame; the pixel's world position
 *           is carried into the light's frame and compared with the shadow map
 *           (nine taps round it, so the edge is soft) to decide how much of the
 *           light reaches it; the one light shades it; a depth buffer decides
 *           what is in front.
 *
 * The table itself is not a mesh (the page draws the planks): a RECEIVER face
 * writes only the shadow that falls on it, as dark pixels with alpha, so the
 * canvas composes over the planks. That is what a cup's shadow on the table is,
 * and a die's, and a cup's on a die. A die in a shaken cup shadows the cup's
 * floor; a die under a cup shadows nothing anyone sees; nothing leaks through
 * a wall, because a wall is in the map.
 *
 * THE SHADOW IS RESOLVED ONCE A PIXEL. Pass 2 keeps, for the fragment that
 * wins the depth test, its texel colour, its tint and shade, and its place in
 * the light's frame; pass 3 then walks the framebuffer once and takes four
 * weighted taps on the map for each pixel. Nine taps on every drawn fragment,
 * overdraw included, cost three times the whole picture.
 *
 * MEMORY is one static arena: textures grow down from its top (uploaded
 * whenever a page first needs them), the frame's buffers grow up from its
 * bottom (sized by scene_begin), and a frame that would meet the textures
 * fails. Fixed caps, no libc. */
#include <stdint.h>
#include <string.h>

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(#name)))
#else
#define EXPORT(name)   /* the host test includes this file */
#endif

/* ---- the arena ------------------------------------------------------------------- */
#define ARENA_BYTES (256u << 20)
static uint8_t arena[ARENA_BYTES] __attribute__((aligned(16)));
static uint32_t frame_top = 0, tex_bottom = ARENA_BYTES;
static void *take(uint32_t n) { n = (n + 15u) & ~15u; if (frame_top + n > tex_bottom) return 0; void *p = arena + frame_top; frame_top += n; return p; }
static void *take_tex(uint32_t n) { n = (n + 15u) & ~15u; if (tex_bottom < n || tex_bottom - n < frame_top) return 0; tex_bottom -= n; return arena + tex_bottom; }

/* ---- textures ---------------------------------------------------------------------- */
#define MAX_TEX 512
/* A texture and its smaller copies (each half the last, down to 8 texels a side, made the first time the
 * texture is drawn): a face is drawn from the copy nearest its size on the screen, so a cup's side at a tenth
 * of its texture's width reads a tenth of the texels, which is what makes it fast (the fetches then stay in
 * the cache) and what keeps it from sparkling. */
#define MAX_LV 8
typedef struct { uint8_t *rgba; int8_t *bump; int w, h; int nlv; uint8_t *lrgba[MAX_LV]; int8_t *lbump[MAX_LV]; int lw[MAX_LV], lh[MAX_LV]; } Tex;
static Tex texs[MAX_TEX]; static int ntex = 0;
static uint8_t *fb;                                      /* the frame's picture; 0 until a frame begins */

EXPORT(scene_reset) void scene_reset(void) { frame_top = 0; tex_bottom = ARENA_BYTES; ntex = 0; fb = 0; }
/* a texture: w by h RGBA, and a normal map of (dx, dy) as signed bytes at a 20th each, or none. -1 when full. */
EXPORT(scene_tex_new) int scene_tex_new(int w, int h, int has_bump)
{
    if (ntex >= MAX_TEX || w < 2 || h < 2) return -1;
    Tex *t = &texs[ntex];
    t->rgba = take_tex((uint32_t)w * h * 4); if (!t->rgba) return -1;
    t->bump = has_bump ? take_tex((uint32_t)w * h * 2) : 0; if (has_bump && !t->bump) return -1;
    t->w = w; t->h = h; t->nlv = 0;
    return ntex++;
}
/* the smaller copies, 2-by-2 means; a failure to find room leaves the chain as long as it got */
static void mips_of(Tex *t)
{
    t->lrgba[0] = t->rgba; t->lbump[0] = t->bump; t->lw[0] = t->w; t->lh[0] = t->h; t->nlv = 1;
    while (t->nlv < MAX_LV) {
        int l = t->nlv, pw = t->lw[l - 1], ph = t->lh[l - 1], w = pw / 2, h = ph / 2;
        if (w < 8 || h < 8) break;
        uint8_t *rg = take_tex((uint32_t)w * h * 4); if (!rg) break;
        int8_t *bm = 0; if (t->bump) { bm = take_tex((uint32_t)w * h * 2); if (!bm) break; }
        const uint8_t *pr = t->lrgba[l - 1]; const int8_t *pb = t->lbump[l - 1];
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int i00 = ((2 * y) * pw + 2 * x), i10 = i00 + 1, i01 = i00 + pw, i11 = i01 + 1, o = y * w + x;
            for (int c = 0; c < 4; c++) rg[o * 4 + c] = (uint8_t)((pr[i00 * 4 + c] + pr[i10 * 4 + c] + pr[i01 * 4 + c] + pr[i11 * 4 + c] + 2) >> 2);
            if (bm) for (int c = 0; c < 2; c++) bm[o * 2 + c] = (int8_t)((pb[i00 * 2 + c] + pb[i10 * 2 + c] + pb[i01 * 2 + c] + pb[i11 * 2 + c]) / 4);
        }
        t->lrgba[l] = rg; t->lbump[l] = bm; t->lw[l] = w; t->lh[l] = h; t->nlv++;
    }
}
EXPORT(scene_tex_rgba) uint8_t *scene_tex_rgba(int id) { return id >= 0 && id < ntex ? texs[id].rgba : 0; }
EXPORT(scene_tex_bump) int8_t *scene_tex_bump(int id) { return id >= 0 && id < ntex ? texs[id].bump : 0; }

/* ---- the frame ---------------------------------------------------------------------- */
#define F_CULL     1    /* a closed body: faces turned away are skipped       */
#define F_CAST     2    /* casts a shadow                                      */
#define F_RECEIVE  4    /* takes the shadow (every lit thing does)             */
#define F_RECEIVER 8    /* the table: writes only the shadow that falls on it  */

#define VF 6            /* floats a vertex: x y z nx ny nz                     */
#define FF 16           /* floats a face: i0 i1 i2 u0 v0 u1 v1 u2 v2 tex r g b km ka flags */

static float *verts, *faces; static int vcap, fcap;
static float *zb; static int FW, FH;
/* the deferred shadow: for the fragment that won each pixel, its texel colour (in fb), its tint (gt), its
 * shade (gk, 0..255), the pixel's place in the light's frame (gu, gv at a 64th of a map texel; gd), and
 * what it is (gf: 0 nothing, 1 a body, 2 the table) */
static uint8_t *gt, *gk, *gf; static uint16_t *gu, *gv; static float *gd;
static int32_t *order;                                   /* pass 2's faces, nearest first */
static float *smap; static int SR;                       /* the shadow map, SR by SR, depth along the light */
static float eyeX, eyeY, HC, DPR, PAD;
static float LX, LY, LZ, UX, UY, UZ, VX, VY, VZ;        /* the light, and the map's axes across it */
static float su0, sv0, sus, svs;                         /* the map's window: origin and scale      */
static float SH_DARK;                                    /* how much of the light a shadow takes    */
static uint32_t prof[4];                                 /* the last frame: fragments shaded, box pixels walked, map texels, map box pixels */
EXPORT(scene_prof) uint32_t scene_prof(int i) { return i >= 0 && i < 4 ? prof[i] : 0; }
static int skip;                                          /* for profiling only: passes to leave out (1 shadow map, 2 picture, 4 shading) */
EXPORT(scene_skip) void scene_skip(int mask) { skip = mask; }

static float fsqrt(float x) { return __builtin_sqrtf(x); }
static float fclamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a frame: the board W by H points (plus pad above it), DPR device pixels a point, the eye over
 * (eyeX, eyeY) at height hc, the light's direction (toward the light), the map's resolution, and how
 * dark a shadow is (0..1). Returns the vertex and face capacities' sum, 0 when memory runs out. */
EXPORT(scene_begin) int scene_begin(int W, int H, int pad, float dpr, float ex, float ey, float hc,
                                    float lx, float ly, float lz, int shadow_res, float dark, int vcapacity, int fcapacity)
{
    frame_top = 0;
    FW = (int)(W * dpr + .5f); FH = (int)((H + pad) * dpr + .5f);
    DPR = dpr; PAD = (float)pad; eyeX = ex; eyeY = ey; HC = hc; SR = shadow_res; SH_DARK = dark;
    uint32_t npx = (uint32_t)FW * FH;
    fb = take(npx * 4); zb = take(npx * 4); smap = take((uint32_t)SR * SR * 4);
    gt = take(npx * 3); gk = take(npx); gf = take(npx); gu = take(npx * 2); gv = take(npx * 2); gd = take(npx * 4);
    verts = take((uint32_t)vcapacity * VF * 4); faces = take((uint32_t)fcapacity * FF * 4); order = take((uint32_t)fcapacity * 4);
    if (!fb || !zb || !smap || !gt || !gk || !gf || !gu || !gv || !gd || !verts || !faces) { fb = 0; return 0; }
    vcap = vcapacity; fcap = fcapacity;
    /* the light's frame: L toward the light, U and V across it */
    float ln = fsqrt(lx * lx + ly * ly + lz * lz); LX = lx / ln; LY = ly / ln; LZ = lz / ln;
    /* U = L x Y, V = U x L */
    UX = LY * 0 - LZ * 1; UY = LZ * 0 - LX * 0; UZ = LX * 1 - LY * 0;
    { float n = fsqrt(UX * UX + UY * UY + UZ * UZ); UX /= n; UY /= n; UZ /= n; }
    VX = UY * LZ - UZ * LY; VY = UZ * LX - UX * LZ; VZ = UX * LY - UY * LX;
    /* the map's window: the board's box, from the table to 420 points up, seen from the light */
    float mnu = 1e30f, mxu = -1e30f, mnv = 1e30f, mxv = -1e30f;
    for (int i = 0; i < 8; i++) {
        float x = i & 1 ? (float)W : 0, y = i & 2 ? (float)H : -PAD, z = i & 4 ? 420.f : 0;
        float u = x * UX + y * UY + z * UZ, v = x * VX + y * VY + z * VZ;
        if (u < mnu) mnu = u;
        if (u > mxu) mxu = u;
        if (v < mnv) mnv = v;
        if (v > mxv) mxv = v;
    }
    su0 = mnu; sv0 = mnv; sus = (SR - 2) / (mxu - mnu); svs = (SR - 2) / (mxv - mnv);
    return vcapacity + fcapacity;
}
EXPORT(scene_verts) float *scene_verts(void) { return verts; }
EXPORT(scene_faces) float *scene_faces(void) { return faces; }
EXPORT(scene_fb) uint8_t *scene_fb(void) { return fb; }
EXPORT(scene_fb_w) int scene_fb_w(void) { return FW; }
EXPORT(scene_fb_h) int scene_fb_h(void) { return FH; }

/* ---- the scanline: a triangle covers, on the row through sy, the columns between the two edges that
 *      cross it. Walking only those (instead of the bounding box with a test at every pixel) is what
 *      makes a thin diagonal facet cheap: measured, the box walked seven pixels for every one drawn. */
static int span(float ax, float ay, float bx, float by, float cx, float cy, float sy, int lim, int *xl, int *xr)
{
    float lo = 1e30f, hi = -1e30f;
    const float px[3] = { ax, bx, cx }, py[3] = { ay, by, cy };
    for (int i = 0; i < 3; i++) {
        int j = i == 2 ? 0 : i + 1;
        float y0 = py[i], y1 = py[j];
        if ((sy < y0) == (sy < y1)) continue;             /* the row is not between this edge's ends */
        float x = px[i] + (sy - y0) * (px[j] - px[i]) / (y1 - y0);
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    if (lo > hi) return 0;
    int l = (int)__builtin_ceilf(lo - .5f), r = (int)__builtin_floorf(hi - .5f);
    if (l < 0) l = 0;
    if (r > lim - 1) r = lim - 1;
    if (l > r) return 0;
    *xl = l; *xr = r;
    return 1;
}

/* ---- pass 1: the shadow map ---------------------------------------------------------- */
typedef struct { float x, y, d; } SV;
static void shadow_tri(SV a, SV b, SV c)
{
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) { SV t = b; b = c; c = t; area = -area; }
    float inv = 1 / area;
    int x0 = (int)fclamp(__builtin_floorf(a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x)), 0, SR - 1);
    int x1 = (int)fclamp(__builtin_ceilf(a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x)), 0, SR - 1);
    int y0 = (int)fclamp(__builtin_floorf(a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y)), 0, SR - 1);
    int y1 = (int)fclamp(__builtin_ceilf(a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y)), 0, SR - 1);
    float e0x = (b.y - c.y) * inv, e1x = (c.y - a.y) * inv;
    (void)x0; (void)x1;
    for (int y = y0; y <= y1; y++) {
        float sy = y + .5f; int xl, xr;
        if (!span(a.x, a.y, b.x, b.y, c.x, c.y, sy, SR, &xl, &xr)) continue;
        float px = xl + .5f;
        float w0 = ((b.x - px) * (c.y - sy) - (b.y - sy) * (c.x - px)) * inv;
        float w1 = ((c.x - px) * (a.y - sy) - (c.y - sy) * (a.x - px)) * inv;
        for (int x = xl; x <= xr; x++, w0 += e0x, w1 += e1x) {
            float w2 = 1 - w0 - w1; prof[2]++; prof[3]++;
            float d = w0 * a.d + w1 * b.d + w2 * c.d;
            float *m = &smap[y * SR + x];
            if (d < *m) *m = d;
        }
    }
}

/* ---- pass 2: the picture --------------------------------------------------------------- */
typedef struct { float x, y, iw, uw, vw, nx, ny, nz, pxw, pyw, pzw; } PV;

/* a pixel's place in the light's frame, kept for pass 3. ndl: the surface's cosine to the light; a surface
 * the light grazes is pushed further toward the light (a slope-scaled bias, by the map's texel), or the
 * map's steps would stripe it with its own shadow */
static void keep_light(int idx, float x, float y, float z, float ndl)
{
    float u = (x * UX + y * UY + z * UZ - su0) * sus, v = (x * VX + y * VY + z * VZ - sv0) * svs;
    u = fclamp(u, 0, SR - 1.01f) * 64; v = fclamp(v, 0, SR - 1.01f) * 64;
    if (ndl < .15f) ndl = .15f;
    float g = 1 - ndl, texel = 1 / (sus < svs ? sus : svs), bias = 1.6f + 2.5f * texel * g * (2 + 4 * g);   /* g (2 + 4 g): near the tangent, without the divide */
    gu[idx] = (uint16_t)u; gv[idx] = (uint16_t)v; gd[idx] = -(x * LX + y * LY + z * LZ) - bias;
}
/* how much of the light a kept pixel gets: 1 lit, 0 in shadow; the four map texels round it, weighted */
static float lit_of(int idx)
{
    float u = gu[idx] * (1.f / 64), v = gv[idx] * (1.f / 64), d = gd[idx];
    int iu = (int)u, iv = (int)v; float fu = u - iu, fv = v - iv;
    int i1 = iu + 1 < SR ? iu + 1 : iu, j1 = iv + 1 < SR ? iv + 1 : iv;
    float l00 = d <= smap[iv * SR + iu], l10 = d <= smap[iv * SR + i1], l01 = d <= smap[j1 * SR + iu], l11 = d <= smap[j1 * SR + i1];
    return (l00 * (1 - fu) + l10 * fu) * (1 - fv) + (l01 * (1 - fu) + l11 * fu) * fv;
}

static void tri(const uint8_t *td, const int8_t *bn, int tw, int th, PV a, PV b, PV c, float Tx, float Ty, float Tz, float Bx, float By, float Bz,
                float tr, float tg, float tb, float km, float ka, int flags)
{
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) { PV t = b; b = c; c = t; area = -area; }
    float inv = 1 / area;
    int x0 = (int)fclamp(__builtin_floorf(a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x)), 0, FW - 1);
    int x1 = (int)fclamp(__builtin_ceilf(a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x)), 0, FW - 1);
    int y0 = (int)fclamp(__builtin_floorf(a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y)), 0, FH - 1);
    int y1 = (int)fclamp(__builtin_ceilf(a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y)), 0, FH - 1);
    float e0x = (b.y - c.y) * inv, e1x = (c.y - a.y) * inv;
    int receiver = flags & F_RECEIVER, receives = flags & F_RECEIVE;
    (void)x0; (void)x1;
    for (int y = y0; y <= y1; y++) {
        float sy = y + .5f; int xl, xr;
        if (!span(a.x, a.y, b.x, b.y, c.x, c.y, sy, FW, &xl, &xr)) continue;
        float px0 = xl + .5f;
        float w0 = ((b.x - px0) * (c.y - sy) - (b.y - sy) * (c.x - px0)) * inv;
        float w1 = ((c.x - px0) * (a.y - sy) - (c.y - sy) * (a.x - px0)) * inv;
        for (int x = xl; x <= xr; x++, w0 += e0x, w1 += e1x) {
            float w2 = 1 - w0 - w1; prof[1]++;
            float iw = w0 * a.iw + w1 * b.iw + w2 * c.iw, depth = 1 / iw;
            int idx = y * FW + x;
            if (depth >= zb[idx]) continue;
            zb[idx] = depth; prof[0]++;
            float wx = (w0 * a.pxw + w1 * b.pxw + w2 * c.pxw) * depth, wy = (w0 * a.pyw + w1 * b.pyw + w2 * c.pyw) * depth, wz = (w0 * a.pzw + w1 * b.pzw + w2 * c.pzw) * depth;
            uint8_t *o = &fb[idx * 4];
            if (receiver) { gf[idx] = 2; keep_light(idx, wx, wy, wz, 1); continue; }   /* the table: pass 3 writes its shadow */
            float u = (w0 * a.uw + w1 * b.uw + w2 * c.uw) * depth, v = (w0 * a.vw + w1 * b.vw + w2 * c.vw) * depth;
            float tx = u * tw - .5f, ty = v * th - .5f;
            if (tx < 0) tx = 0; else if (tx > tw - 1.001f) tx = tw - 1.001f;
            if (ty < 0) ty = 0; else if (ty > th - 1.001f) ty = th - 1.001f;
            int ix = (int)tx, iy = (int)ty; float fx = tx - ix, fy = ty - iy;
            int i00 = (iy * tw + ix) * 4, i10 = i00 + 4, i01 = i00 + tw * 4, i11 = i01 + 4;
            float g00 = (1 - fx) * (1 - fy), g10 = fx * (1 - fy), g01 = (1 - fx) * fy, g11 = fx * fy;
            float r = td[i00] * g00 + td[i10] * g10 + td[i01] * g01 + td[i11] * g11;
            float g = td[i00 + 1] * g00 + td[i10 + 1] * g10 + td[i01 + 1] * g01 + td[i11 + 1] * g11;
            float bl = td[i00 + 2] * g00 + td[i10 + 2] * g10 + td[i01 + 2] * g01 + td[i11 + 2] * g11;
            /* the normal: interpolated from the vertices, then bent by the normal map along the tangent frame */
            float nx = w0 * a.nx + w1 * b.nx + w2 * c.nx, ny = w0 * a.ny + w1 * b.ny + w2 * c.ny, nz = w0 * a.nz + w1 * b.nz + w2 * c.nz;
            if (bn) {
                int bi = (((int)(v * th) * tw) + (int)(u * tw)) * 2; if (bi < 0) bi = 0; if (bi > (tw * th - 1) * 2) bi = (tw * th - 1) * 2;
                float dx = bn[bi] * (1.f / 20), dy = bn[bi + 1] * (1.f / 20);
                nx += Tx * dx + Bx * dy; ny += Ty * dx + By * dy; nz += Tz * dx + Bz * dy;
            }
            /* the normal's length is near 1 (unit normals, a small bend): one Newton step from 1 for its inverse */
            float nn = nx * nx + ny * ny + nz * nz, nl = 1.5f - .5f * nn, lit = (nx * LX + ny * LY + nz * LZ) * nl;
            float k = .5f - .46f * lit; if (k < 0) k = 0; else if (k > .82f) k = .82f;
            k = k * km + ka; if (k > 1) k = 1;
            /* kept for pass 3: the texel, the tint, the shade, the place in the light */
            o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)bl; o[3] = 255;
            gt[idx * 3] = (uint8_t)tr; gt[idx * 3 + 1] = (uint8_t)tg; gt[idx * 3 + 2] = (uint8_t)tb; gk[idx] = (uint8_t)(k * 255 + .5f);
            gf[idx] = receives ? 1 : 3; if (receives) keep_light(idx, wx, wy, wz, lit);
        }
    }
}

/* render nverts and nfaces of the arrays: the shadow pass, then the picture. Returns the faces drawn. */
EXPORT(scene_render) int scene_render(int nverts, int nfaces)
{
    if (!fb || nverts > vcap || nfaces > fcap) return -1;
    memset(fb, 0, (uint32_t)FW * FH * 4); memset(gf, 0, (uint32_t)FW * FH); prof[0] = prof[1] = prof[2] = prof[3] = 0;
    for (int i = 0; i < FW * FH; i++) zb[i] = 1e30f;
    for (int i = 0; i < SR * SR; i++) smap[i] = 1e30f;
    /* pass 1 */
    for (int f = 0; f < nfaces && !(skip & 1); f++) {
        const float *F = &faces[f * FF]; int flags = (int)F[15];
        if (!(flags & F_CAST)) continue;
        SV s[3];
        for (int k = 0; k < 3; k++) {
            const float *V = &verts[(int)F[k] * VF];
            s[k].x = (V[0] * UX + V[1] * UY + V[2] * UZ - su0) * sus; s[k].y = (V[0] * VX + V[1] * VY + V[2] * VZ - sv0) * svs; s[k].d = -(V[0] * LX + V[1] * LY + V[2] * LZ);
        }
        shadow_tri(s[0], s[1], s[2]);
    }
    /* pass 2: the faces nearest the eye first (the eye is over the table, so a face's nearness is its height),
     * sorted into buckets by height, so a fragment a nearer face covers is mostly never shaded at all */
    int drawn = 0;
    enum { NB = 1024 };
    static int32_t head[NB]; static int32_t next[1 << 16];
    for (int i = 0; i < NB; i++) head[i] = -1;
    for (int f = 0; f < nfaces && f < (1 << 16); f++) {
        const float *F = &faces[f * FF];
        float z0 = verts[(int)F[0] * VF + 2], z1 = verts[(int)F[1] * VF + 2], z2 = verts[(int)F[2] * VF + 2];
        float z = z0 > z1 ? (z0 > z2 ? z0 : z2) : (z1 > z2 ? z1 : z2);
        int b = (int)((z + 100) * 2); if (b < 0) b = 0; if (b >= NB) b = NB - 1;
        next[f] = head[b]; head[b] = f;
    }
    int no = 0;
    for (int b = NB - 1; b >= 0; b--) for (int f = head[b]; f >= 0; f = next[f]) order[no++] = f;
    for (int oi = 0; oi < no && !(skip & 2); oi++) {
        int f = order[oi];
        const float *F = &faces[f * FF]; int flags = (int)F[15], ti = (int)F[9];
        const float *V0 = &verts[(int)F[0] * VF], *V1 = &verts[(int)F[1] * VF], *V2 = &verts[(int)F[2] * VF];
        if (flags & F_CULL) {
            /* turned away from the eye: the face's own normal against the line to the eye */
            float e1x = V1[0] - V0[0], e1y = V1[1] - V0[1], e1z = V1[2] - V0[2], e2x = V2[0] - V0[0], e2y = V2[1] - V0[1], e2z = V2[2] - V0[2];
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
            float cx = (V0[0] + V1[0] + V2[0]) / 3, cy = (V0[1] + V1[1] + V2[1]) / 3, cz = (V0[2] + V1[2] + V2[2]) / 3;
            if (nx * (eyeX - cx) + ny * (eyeY - cy) + nz * (HC - cz) <= 0) continue;
        }
        const Tex *tex = ti >= 0 && ti < ntex ? &texs[ti] : 0;
        if (!tex && !(flags & F_RECEIVER)) continue;
        PV p[3];
        for (int k = 0; k < 3; k++) {
            const float *V = k == 0 ? V0 : k == 1 ? V1 : V2;
            float w = HC - V[2], iw = 1 / w, kk = HC * iw;
            p[k].x = (eyeX + (V[0] - eyeX) * kk) * DPR; p[k].y = (eyeY + (V[1] - eyeY) * kk + PAD) * DPR;
            p[k].iw = iw; p[k].uw = F[3 + k * 2] * iw; p[k].vw = F[4 + k * 2] * iw;
            p[k].nx = V[3]; p[k].ny = V[4]; p[k].nz = V[5];
            p[k].pxw = V[0] * iw; p[k].pyw = V[1] * iw; p[k].pzw = V[2] * iw;
        }
        /* the tangent frame: where u and v run in the world, from the three corners */
        float Tx = 0, Ty = 0, Tz = 0, Bx = 0, By = 0, Bz = 0;
        if (tex && tex->bump) {
            float e1x = V1[0] - V0[0], e1y = V1[1] - V0[1], e1z = V1[2] - V0[2], e2x = V2[0] - V0[0], e2y = V2[1] - V0[1], e2z = V2[2] - V0[2];
            float du1 = F[5] - F[3], dv1 = F[6] - F[4], du2 = F[7] - F[3], dv2 = F[8] - F[4];
            float det = du1 * dv2 - du2 * dv1, rr = 1 / (det != 0 ? det : 1e-9f);
            Tx = (e1x * dv2 - e2x * dv1) * rr; Ty = (e1y * dv2 - e2y * dv1) * rr; Tz = (e1z * dv2 - e2z * dv1) * rr;
            Bx = (e2x * du1 - e1x * du2) * rr; By = (e2y * du1 - e1y * du2) * rr; Bz = (e2z * du1 - e1z * du2) * rr;
            float tn = fsqrt(Tx * Tx + Ty * Ty + Tz * Tz), bn = fsqrt(Bx * Bx + By * By + Bz * Bz);
            if (tn > 0) { Tx /= tn; Ty /= tn; Tz /= tn; }
            if (bn > 0) { Bx /= bn; By /= bn; Bz /= bn; }
        }
        /* the copy of the texture nearest the face's size on the screen: at most two texels a pixel, each way */
        int lv = 0; const uint8_t *td = 0; const int8_t *bn = 0; int tw = 1, th = 1;
        if (tex) {
            if (!tex->nlv) mips_of((Tex *)tex);
            float sa = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y); if (sa < 0) sa = -sa;
            float ta = ((F[5] - F[3]) * (F[8] - F[4]) - (F[7] - F[3]) * (F[6] - F[4])) * tex->w * tex->h; if (ta < 0) ta = -ta;
            while (lv + 1 < tex->nlv && ta > 4 * sa) { ta *= .25f; lv++; }
            td = tex->lrgba[lv]; bn = tex->lbump[lv]; tw = tex->lw[lv]; th = tex->lh[lv];
        }
        tri(td, bn, tw, th, p[0], p[1], p[2], Tx, Ty, Tz, Bx, By, Bz, F[10], F[11], F[12], F[13], F[14], flags);
        drawn++;
    }
    /* pass 3: the shadow, once a pixel. A pixel nothing was drawn on is the table (the lens is shifted, so
     * the table maps 1:1: its world point is the pixel's), and takes the shadow that falls there. */
    /* the open table first, in 4-by-4 blocks: one lookup each (its shadow is soft anyway) */
    if (skip & 4) return drawn;
    for (int by = 0; by < FH; by += 4) for (int bx = 0; bx < FW; bx += 4) {
        int i = by * FW + bx;
        if (gf[i] && gf[i + (bx + 3 < FW ? 3 : 0)] && gf[i + (by + 3 < FH ? 3 * FW : 0)]) continue;   /* a block some body covers is done below */
        keep_light(i, (bx + 1.5f) / DPR, (by + 1.5f) / DPR - PAD, 0, 1);
        float sh = 1 - lit_of(i); uint8_t al = (uint8_t)(sh * SH_DARK * 255 + .5f);
        for (int y = by; y < by + 4 && y < FH; y++) for (int x = bx; x < bx + 4 && x < FW; x++) {
            int j = y * FW + x; if (gf[j]) continue;
            uint8_t *o = &fb[j * 4]; o[0] = 0; o[1] = 3; o[2] = 2; o[3] = al; gf[j] = 4;
        }
    }
    for (int i = 0, n = FW * FH; i < n; i++) {
        int f = gf[i]; if (f == 4) continue;
        uint8_t *o = &fb[i * 4];
        if (!f) { float x = (i % FW) / DPR, y = (i / FW) / DPR - PAD; keep_light(i, x, y, 0, 1); f = 2; }
        if (f == 2) { float sh = 1 - lit_of(i); o[0] = 0; o[1] = 3; o[2] = 2; o[3] = (uint8_t)(sh * SH_DARK * 255 + .5f); continue; }
        float k = gk[i] * (1.f / 255);
        if (f == 1) { float sh = 1 - lit_of(i); k += (1 - k) * SH_DARK * sh; }
        const uint8_t *t = &gt[i * 3];
        o[0] = (uint8_t)(o[0] + (t[0] - o[0]) * k); o[1] = (uint8_t)(o[1] + (t[1] - o[1]) * k); o[2] = (uint8_t)(o[2] + (t[2] - o[2]) * k);
    }
    return drawn;
}

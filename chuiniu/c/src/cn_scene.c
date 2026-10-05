/* The scene renderer, in C for speed: the study runs it as wasm (wasm/cn_scene_web.c
 * exports it under the names the page calls) and the iOS host links it natively and
 * draws the framebuffer. Freestanding, no libc beyond memset. The API is cn_scene.h.
 *
 * WHAT IT IS. A software rasterizer with a shadow map, the way a small engine
 * does it. The host hands over, each frame, a vertex array (world position and
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
 *           is carried into the light's frame and kept; a depth buffer decides
 *           what is in front.
 *   pass 3  THE SHADE, once a pixel (below).
 *
 * The table itself is not a mesh (the host draws the planks): a RECEIVER face
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
 * MEMORY is the caller's arena (cn_scene_init): textures grow down from its top
 * (uploaded whenever a host first needs them), the frame's buffers grow up from
 * its bottom (sized by cn_scene_begin), and a frame that would meet the textures
 * fails. Fixed caps, no libc. */
#include "cn_scene.h"
#include <string.h>

/* ---- the arena ------------------------------------------------------------------- */
static uint8_t *arena;
static size_t arena_n, frame_top, tex_bottom;
static size_t rup16(size_t n) { return (n + 15u) & ~(size_t)15u; }
static void *take(size_t n) { n = rup16(n); if (!arena || n > tex_bottom - frame_top) return 0; void *p = arena + frame_top; frame_top += n; return p; }
static void *take_tex(size_t n) { n = rup16(n); if (!arena || n > tex_bottom - frame_top) return 0; tex_bottom -= n; return arena + tex_bottom; }

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

void cn_scene_reset(void) { frame_top = 0; tex_bottom = arena_n; ntex = 0; fb = 0; }
int cn_scene_init(void *mem, size_t bytes)
{
    /* the block's start rounded up to 16 and its length down: every buffer is 16-aligned */
    uintptr_t a = ((uintptr_t)mem + 15u) & ~(uintptr_t)15u;
    size_t lost = mem ? (size_t)(a - (uintptr_t)mem) : 0;
    if (!mem || bytes < lost + 4096) { arena = 0; arena_n = 0; cn_scene_reset(); return 0; }
    arena = (uint8_t *)a; arena_n = (bytes - lost) & ~(size_t)15u;
    cn_scene_reset();
    return 1;
}
/* a texture: w by h RGBA, and a normal map of (dx, dy) as signed bytes at a 20th each, or none. -1 when full. */
int cn_scene_tex_new(int w, int h, int has_bump)
{
    if (ntex >= MAX_TEX || w < 2 || h < 2 || w > 8192 || h > 8192) return -1;
    Tex *t = &texs[ntex];
    size_t top = tex_bottom;
    t->rgba = take_tex((size_t)w * h * 4); if (!t->rgba) return -1;
    t->bump = has_bump ? take_tex((size_t)w * h * 2) : 0; if (has_bump && !t->bump) { tex_bottom = top; return -1; }
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
        uint8_t *rg = take_tex((size_t)w * h * 4); if (!rg) break;
        int8_t *bm = 0; if (t->bump) { bm = take_tex((size_t)w * h * 2); if (!bm) break; }
        const uint8_t *pr = t->lrgba[l - 1]; const int8_t *pb = t->lbump[l - 1];
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int i00 = ((2 * y) * pw + 2 * x), i10 = i00 + 1, i01 = i00 + pw, i11 = i01 + 1, o = y * w + x;
            for (int c = 0; c < 4; c++) rg[o * 4 + c] = (uint8_t)((pr[i00 * 4 + c] + pr[i10 * 4 + c] + pr[i01 * 4 + c] + pr[i11 * 4 + c] + 2) >> 2);
            if (bm) for (int c = 0; c < 2; c++) bm[o * 2 + c] = (int8_t)((pb[i00 * 2 + c] + pb[i10 * 2 + c] + pb[i01 * 2 + c] + pb[i11 * 2 + c]) / 4);
        }
        t->lrgba[l] = rg; t->lbump[l] = bm; t->lw[l] = w; t->lh[l] = h; t->nlv++;
    }
}
uint8_t *cn_scene_tex_rgba(int id) { return id >= 0 && id < ntex ? texs[id].rgba : 0; }
int8_t *cn_scene_tex_bump(int id) { return id >= 0 && id < ntex ? texs[id].bump : 0; }

/* ---- the frame ---------------------------------------------------------------------- */
#define F_CULL     CN_SCENE_F_CULL
#define F_CAST     CN_SCENE_F_CAST
#define F_RECEIVE  CN_SCENE_F_RECEIVE
#define F_RECEIVER CN_SCENE_F_RECEIVER
#define VF CN_SCENE_VF
#define FF CN_SCENE_FF

static float *verts, *faces; static int vcap, fcap;
static float *zb; static int FW, FH;
/* the deferred shadow: for the fragment that won each pixel, its texel colour (in fb), its tint (gt), its
 * shade (gk, 0..255), the pixel's place in the light's frame (gu, gv at a 64th of a map texel; gd), and
 * what it is (gf: 0 nothing, 1 a body, 2 the table) */
static uint8_t *gt, *gk, *gf, *gm, *ga, *gc; static uint16_t *gu, *gv; static float *gd;   /* gk: the direct light's cosine; gm, ga: the tint's scale and floor; gc: the crease */
static int32_t *order, *chain;                           /* pass 2's faces, nearest first; the height buckets' chains */
static int prepared, prep_nfaces, prep_ndraw;               /* cn_scene_prepare has run on this frame; its counts */
static int32_t *rows, *srows;                            /* each drawn face's rows on the picture (by its place in order),
                                                            each casting face's on the map (by its index): first, last */
static float *smap; static int SR;                       /* the shadow map, SR by SR, depth along the light */
static float eyeX, eyeY, HC, DPR, PAD;
static float LX, LY, LZ, UX, UY, UZ, VX, VY, VZ;        /* the light, and the map's axes across it */
static float su0, sv0, sus, svs;                         /* the map's window: origin and scale      */
static float SH_DARK;                                    /* how much of the light a shadow takes    */
/* CONTACT. The map gives the light's shadow; it does not give the dark where a body meets the table, which
 * is what sets a thing down on it. Each body is given to the frame as a disc (its footprint) at a height:
 * the table under and just past the disc is darkened, most at the rim and fading out over .6 of the
 * radius, less the higher the body is held; and a body's own fragments within a few points of the table
 * are darkened too, the crease at a wall's foot. */
#define MAX_OCC 256
typedef struct { float x, y, r, s, out; } Occ;
static Occ occ[MAX_OCC]; static int nocc;
static float *ao; static int AW, AH;                      /* the contact dark, one a 4-by-4 block */
/* a footprint: its centre and radius (points), its height above the table, and its strength when down */
void cn_scene_occluder(float x, float y, float r, float lift, float strength)
{
    if (nocc >= MAX_OCC || !(r > 0)) return;
    float s = strength * (1 - lift / r); if (!(s > 0)) return;
    occ[nocc].x = x; occ[nocc].y = y; occ[nocc].r = r; occ[nocc].s = s; occ[nocc].out = r * .6f + lift * .5f; nocc++;
}
/* the last frame's counts, a row of them a band (each band's thread writes only its own): fragments shaded,
 * span pixels walked, map texels, map texels again (the old box count's place) */
static uint32_t prof[CN_SCENE_MAX_BANDS][4];
uint32_t cn_scene_prof(int i)
{
    uint32_t n = 0;
    if (i >= 0 && i < 4) for (int b = 0; b < CN_SCENE_MAX_BANDS; b++) n += prof[b][i];
    return n;
}
static int skip;                                          /* for profiling only: passes to leave out (1 shadow map, 2 picture, 4 shading) */
void cn_scene_skip(int mask) { skip = mask; }

static float fsqrt(float x) { return __builtin_sqrtf(x); }
static float fclamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* THE FRAME'S BUFFERS, in the order they are taken; cn_scene_begin and cn_scene_frame_bytes both read this
 * one list, so the bytes a host is told a frame needs are the bytes it takes */
#define MAX_FRAME_PX 8192
enum { B_FB, B_ZB, B_SMAP, B_AO, B_GT, B_GK, B_GF, B_GU, B_GV, B_GD, B_GM, B_GA, B_GC, B_VERTS, B_FACES, B_ORDER, B_CHAIN, B_ROWS, B_SROWS, NBUF };
/* 0 when the numbers are out of range */
static int frame_sizes(int W, int H, int pad, float dpr, int shadow_res, int vcapacity, int fcapacity, size_t sz[NBUF], int *fw, int *fh)
{
    /* each refusal has its own case (cn_scene_test holds one number to each):
     *   H < 1, pad < 0      a board of no height (the pad alone would still give it rows), a pad upward
     *   !(dpr > 0)          a scale of zero or less, which times a negative width would make a positive one
     *   !(... < MAX)        a framebuffer past 8,192 a side, or not a number (a NaN scale lands here too)
     *   w < 1, h < 1        a board and scale that round to no pixel, a board of no width among them
     *   shadow_res          a map too small to window, or past what 16 bits place (CN_SCENE_SHADOW_MAX) */
    if (H < 1 || pad < 0 || !(dpr > 0) || shadow_res < 4 || shadow_res > CN_SCENE_SHADOW_MAX || vcapacity < 0 || fcapacity < 0) return 0;
    float fwf = W * dpr + .5f, fhf = (H + pad) * dpr + .5f;
    if (!(fwf < MAX_FRAME_PX) || !(fhf < MAX_FRAME_PX)) return 0;
    int w = (int)fwf, h = (int)fhf;
    if (w < 1 || h < 1) return 0;
    size_t npx = (size_t)w * h, aw = (size_t)(w + 3) / 4, ah = (size_t)(h + 3) / 4;
    sz[B_FB] = npx * 4; sz[B_ZB] = npx * 4; sz[B_SMAP] = (size_t)shadow_res * shadow_res * 4; sz[B_AO] = aw * ah * 4;
    sz[B_GT] = npx * 3; sz[B_GK] = npx; sz[B_GF] = npx; sz[B_GU] = npx * 2; sz[B_GV] = npx * 2; sz[B_GD] = npx * 4;
    sz[B_GM] = npx; sz[B_GA] = npx; sz[B_GC] = npx;
    sz[B_VERTS] = (size_t)vcapacity * VF * 4; sz[B_FACES] = (size_t)fcapacity * FF * 4; sz[B_ORDER] = (size_t)fcapacity * 4; sz[B_CHAIN] = (size_t)fcapacity * 4;
    sz[B_ROWS] = (size_t)fcapacity * 8; sz[B_SROWS] = (size_t)fcapacity * 8;
    *fw = w; *fh = h;
    return 1;
}
size_t cn_scene_frame_bytes(int W, int H, int pad, float dpr, int shadow_res, int vcapacity, int fcapacity)
{
    size_t sz[NBUF], n = 0; int w, h;
    if (!frame_sizes(W, H, pad, dpr, shadow_res, vcapacity, fcapacity, sz, &w, &h)) return 0;
    for (int i = 0; i < NBUF; i++) n += rup16(sz[i]);
    return n;
}
size_t cn_scene_room(void) { return arena ? tex_bottom : 0; }

/* a frame: the board W by H points (plus pad above it), DPR device pixels a point, the eye over
 * (eyeX, eyeY) at height hc, the light's direction (toward the light), the map's resolution, and how
 * dark a shadow is (0..1). Returns the vertex and face capacities' sum, 0 when memory runs out. */
int cn_scene_begin(int W, int H, int pad, float dpr, float ex, float ey, float hc,
                   float lx, float ly, float lz, int shadow_res, float dark, int vcapacity, int fcapacity)
{
    frame_top = 0; fb = 0; nocc = 0; prepared = 0;
    size_t sz[NBUF]; int fw, fh;
    if (!frame_sizes(W, H, pad, dpr, shadow_res, vcapacity, fcapacity, sz, &fw, &fh)) return 0;
    void *buf[NBUF];
    for (int i = 0; i < NBUF; i++) if (!(buf[i] = take(sz[i]))) { frame_top = 0; return 0; }
    float ln = fsqrt(lx * lx + ly * ly + lz * lz);
    if (!(ln > 0)) { frame_top = 0; return 0; }
    FW = fw; FH = fh;
    DPR = dpr; PAD = (float)pad; eyeX = ex; eyeY = ey; HC = hc; SR = shadow_res; SH_DARK = dark;
    AW = (FW + 3) / 4; AH = (FH + 3) / 4;
    fb = buf[B_FB]; zb = buf[B_ZB]; smap = buf[B_SMAP]; ao = buf[B_AO];
    gt = buf[B_GT]; gk = buf[B_GK]; gf = buf[B_GF]; gu = buf[B_GU]; gv = buf[B_GV]; gd = buf[B_GD]; gm = buf[B_GM]; ga = buf[B_GA]; gc = buf[B_GC];
    verts = buf[B_VERTS]; faces = buf[B_FACES]; order = buf[B_ORDER]; chain = buf[B_CHAIN]; rows = buf[B_ROWS]; srows = buf[B_SROWS]; prepared = 0;
    vcap = vcapacity; fcap = fcapacity;
    /* the light's frame: L toward the light, U and V across it */
    LX = lx / ln; LY = ly / ln; LZ = lz / ln;
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
float *cn_scene_verts(void) { return fb ? verts : 0; }
float *cn_scene_faces(void) { return fb ? faces : 0; }
uint8_t *cn_scene_fb(void) { return fb; }
int cn_scene_fb_w(void) { return fb ? FW : 0; }
int cn_scene_fb_h(void) { return fb ? FH : 0; }

/* ---- the scanline: a triangle covers, on the row through sy, the columns between the two edges that
 *      cross it. Walking only those (instead of the bounding box with a test at every pixel) is what
 *      makes a thin diagonal facet cheap: measured, the box walked seven pixels for every one drawn. */
/* the three edges a to b, b to c, c to a: where each starts and how far it runs, worked out once a triangle */
typedef struct { float x0[3], y0[3], y1[3], dx[3], dy[3]; } Edges;
static inline __attribute__((always_inline)) Edges edges_of(float ax, float ay, float bx, float by, float cx, float cy)
{
    const float px[3] = { ax, bx, cx }, py[3] = { ay, by, cy };
    Edges E;
    for (int i = 0; i < 3; i++) {
        int j = i == 2 ? 0 : i + 1;
        E.x0[i] = px[i]; E.y0[i] = py[i]; E.y1[i] = py[j]; E.dx[i] = px[j] - px[i]; E.dy[i] = py[j] - py[i];
    }
    return E;
}
static inline __attribute__((always_inline)) int span(const Edges *E, float sy, int lim, int *xl, int *xr)
{
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 3; i++) {
        if ((sy < E->y0[i]) == (sy < E->y1[i])) continue;   /* the row is not between this edge's ends */
        float x = E->x0[i] + (sy - E->y0[i]) * E->dx[i] / E->dy[i];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    if (!(lo <= hi)) return 0;
    /* held to the screen before the conversion (a corner near the eye's plane lands far off it); the
     * clamps below give the same columns */
    lo = fclamp(lo, -1, (float)lim + 1); hi = fclamp(hi, -1, (float)lim + 1);
    int l = (int)__builtin_ceilf(lo - .5f), r = (int)__builtin_floorf(hi - .5f);
    if (l < 0) l = 0;
    if (r > lim - 1) r = lim - 1;
    if (l > r) return 0;
    *xl = l; *xr = r;
    return 1;
}

/* THE INNER LOOPS READ ONLY LOCALS. Every write a loop makes is a byte (uint8_t may alias anything), so a
 * loop that read a file-scope variable or a field of a struct in memory would have to load it again after
 * every pixel: the triangle's corners, the light's axes, the buffers' addresses, the counters. Measured, that
 * was a third of the picture. So each pass first copies what it reads into a Light (the map's frame) and a
 * Bufs (the frame's arrays) held by value, and its loops count into locals. */
typedef struct { float LX, LY, LZ, UX, UY, UZ, VX, VY, VZ, su0, sv0, sus, svs; int SR; } Light;
typedef struct { uint8_t *fb, *gt, *gk, *gf, *gm, *ga, *gc; uint16_t *gu, *gv; float *gd, *zb, *smap, *ao; int FW, FH, SR, AW, AH; } Bufs;
static Light light_now(void)
{
    Light L = { LX, LY, LZ, UX, UY, UZ, VX, VY, VZ, su0, sv0, sus, svs, SR };
    return L;
}
static Bufs bufs_now(void)
{
    Bufs B = { fb, gt, gk, gf, gm, ga, gc, gu, gv, gd, zb, smap, ao, FW, FH, SR, AW, AH };
    return B;
}

/* the rows a triangle's corners span, held to [0, lim) (both passes, and the bands' quick test) */
static void rows_of(float ay, float by, float cy, int lim, int *y0, int *y1)
{
    *y0 = (int)fclamp(__builtin_floorf(ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy)), 0, lim - 1);
    *y1 = (int)fclamp(__builtin_ceilf(ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy)), 0, lim - 1);
}

/* ---- pass 1: the shadow map ---------------------------------------------------------- */
typedef struct { float x, y, d; } SV;
/* a casting triangle into the map's rows [r0, r1) */
static void shadow_tri(SV a, SV b, SV c, float *smap_, int SR_, int r0, int r1, uint32_t *texels)
{
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (!(area != 0)) return;
    if (area < 0) { SV t = b; b = c; c = t; area = -area; }
    const float ax = a.x, ay = a.y, ad = a.d, bx = b.x, by = b.y, bd = b.d, cx = c.x, cy = c.y, cd = c.d;
    float inv = 1 / area;
    int y0, y1; rows_of(ay, by, cy, SR_, &y0, &y1);
    if (y0 < r0) y0 = r0;
    if (y1 > r1 - 1) y1 = r1 - 1;
    float e0x = (by - cy) * inv, e1x = (cy - ay) * inv;
    const Edges E = edges_of(ax, ay, bx, by, cx, cy);
    uint32_t n = 0;
    for (int y = y0; y <= y1; y++) {
        float sy = y + .5f; int xl, xr;
        if (!span(&E, sy, SR_, &xl, &xr)) continue;
        float px = xl + .5f;
        float w0 = ((bx - px) * (cy - sy) - (by - sy) * (cx - px)) * inv;
        float w1 = ((cx - px) * (ay - sy) - (cy - sy) * (ax - px)) * inv;
        float *row = &smap_[y * SR_];
        n += (uint32_t)(xr - xl + 1);
        for (int x = xl; x <= xr; x++, w0 += e0x, w1 += e1x) {
            float w2 = 1 - w0 - w1;
            float d = w0 * ad + w1 * bd + w2 * cd;
            if (d < row[x]) row[x] = d;
        }
    }
    *texels += n;
}

/* ---- pass 2: the picture --------------------------------------------------------------- */
typedef struct { float x, y, iw, uw, vw, nx, ny, nz, pxw, pyw, pzw; } PV;

/* FOUR PIXELS AT A TIME. The picture is arithmetic, the same for every pixel of a span, so it runs on four
 * neighbouring pixels at once in the compilers' vector types (NEON on a phone, SSE in the simulator, plain
 * scalar code in the study's wasm, which is built without SIMD). Every lane does the scalar operations in the
 * scalar order, IEEE single precision rounding each one alike, so the picture is the same bits as one pixel at
 * a time; a lane past the span's end or behind the depth buffer is computed and thrown away. */
typedef float f4 __attribute__((vector_size(16)));
typedef int32_t i4 __attribute__((vector_size(16)));
typedef uint32_t u4 __attribute__((vector_size(16)));
typedef uint8_t u8x4 __attribute__((vector_size(4)));
typedef uint16_t u16x4 __attribute__((vector_size(8)));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "cn_scene.c packs a pixel's four bytes into one word: little-endian only"
#endif
static inline __attribute__((always_inline)) f4 vf(float s) { f4 v = { s, s, s, s }; return v; }
static inline __attribute__((always_inline)) f4 vsel(i4 m, f4 a, f4 b) { return (f4)(((i4)a & m) | ((i4)b & ~m)); }
static inline __attribute__((always_inline)) i4 vsel_i(i4 m, i4 a, i4 b) { return (a & m) | (b & ~m); }
/* v < lo ? lo : v > hi ? hi : v, as fclamp */
static inline __attribute__((always_inline)) f4 vclamp(f4 v, float lo, float hi) { return vsel(v < lo, vf(lo), vsel(v > hi, vf(hi), v)); }
static inline __attribute__((always_inline)) i4 vint(f4 v) { return __builtin_convertvector(v, i4); }
static inline __attribute__((always_inline)) f4 vflt(i4 v) { return __builtin_convertvector(v, f4); }
static inline __attribute__((always_inline)) int vany(i4 m) { return (m[0] | m[1] | m[2] | m[3]) != 0; }

/* a pixel's place in the light's frame, kept for pass 3. (nx, ny, nz) the unit normal, ndl its cosine to
 * the light: the point is first moved out along its normal by a texel and a half times the sine (a normal
 * offset: the map's steps would otherwise stripe a surface the light grazes with its own shadow), then a
 * small constant bias toward the light. A depth bias that grew with the slope was tried first and pushed
 * the top of a cup's shaded side clean out of the cup's shadow: a lit band under the crown.
 * Four at a time: the map's u and v (at a 64th of a texel) and the depth along the light. */
static inline __attribute__((always_inline)) void light4(const Light *L, f4 x, f4 y, f4 z, f4 nx, f4 ny, f4 nz, f4 ndl, i4 *gu4, i4 *gv4, f4 *gd4)
{
    f4 sn = 1.f - ndl * ndl; sn = vsel(sn < 0, vf(0), sn);
    { f4 s = { fsqrt(sn[0]), fsqrt(sn[1]), fsqrt(sn[2]), fsqrt(sn[3]) }; sn = s; }
    f4 off = 1.5f * sn / (L->sus < L->svs ? L->sus : L->svs);
    x += nx * off; y += ny * off; z += nz * off;
    f4 u = (x * L->UX + y * L->UY + z * L->UZ - L->su0) * L->sus, v = (x * L->VX + y * L->VY + z * L->VZ - L->sv0) * L->svs;
    u = vclamp(u, 0, L->SR - 1.01f) * 64; v = vclamp(v, 0, L->SR - 1.01f) * 64;
    u = vsel(u == u, u, vf(0)); v = vsel(v == v, v, vf(0));   /* a host's NaN reads texel 0, never converts out of range */
    *gu4 = vint(u); *gv4 = vint(v); *gd4 = -(x * L->LX + y * L->LY + z * L->LZ) - 1.6f;
}
/* one pixel's, through the same four-wide arithmetic (pass 3's table) */
static inline __attribute__((always_inline)) void keep_light(const Light *L, const Bufs *B, int idx, float x, float y, float z, float nx, float ny, float nz, float ndl)
{
    i4 u, v; f4 d;
    light4(L, vf(x), vf(y), vf(z), vf(nx), vf(ny), vf(nz), vf(ndl), &u, &v, &d);
    B->gu[idx] = (uint16_t)u[0]; B->gv[idx] = (uint16_t)v[0]; B->gd[idx] = d[0];
}
/* the contact dark at a pixel, read between the blocks' centres */
static inline __attribute__((always_inline)) float ao_at(const Bufs *B, int x, int y)
{
    float fx = (x - 2) * .25f, fy = (y - 2) * .25f; if (fx < 0) fx = 0; if (fy < 0) fy = 0;
    int ix = (int)fx, iy = (int)fy; float ux = fx - ix, uy = fy - iy;
    if (ix >= B->AW - 1) { ix = B->AW - 1; ux = 0; }
    if (iy >= B->AH - 1) { iy = B->AH - 1; uy = 0; }
    const float *m = &B->ao[iy * B->AW + ix]; int dx = ux > 0 ? 1 : 0, dy = uy > 0 ? B->AW : 0;
    return (m[0] * (1 - ux) + m[dx] * ux) * (1 - uy) + (m[dy] * (1 - ux) + m[dy + dx] * ux) * uy;
}
/* how much of the light a kept pixel gets: 1 lit, 0 in shadow; the four map texels round it, weighted */
static inline __attribute__((always_inline)) float lit_of(const Bufs *B, int idx)
{
    /* the four texels round the point, two apart, so the edge softens over two texels (a lamp's penumbra, roughly) */
    const int S = B->SR; const float *sm = B->smap;
    float u = B->gu[idx] * (1.f / 128), v = B->gv[idx] * (1.f / 128), d = B->gd[idx];
    int iu = (int)u, iv = (int)v; float fu = u - iu, fv = v - iv; iu *= 2; iv *= 2;
    int i1 = iu + 2 < S ? iu + 2 : iu, j1 = iv + 2 < S ? iv + 2 : iv;
    float l00 = d <= sm[iv * S + iu], l10 = d <= sm[iv * S + i1], l01 = d <= sm[j1 * S + iu], l11 = d <= sm[j1 * S + i1];
    return (l00 * (1 - fu) + l10 * fu) * (1 - fv) + (l01 * (1 - fu) + l11 * fu) * fv;
}

static void tri(const Light *Lp, const Bufs *Bp, int r0, int r1, uint32_t *walked, uint32_t *shaded,
                const uint8_t *td, const int8_t *bn, int tw, int th, PV a, PV b, PV c, float Tx, float Ty, float Tz, float Bx, float By, float Bz,
                float tr, float tg, float tb, float km, float ka, int flags)
{
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (!(area != 0)) return;
    if (area < 0) { PV t = b; b = c; c = t; area = -area; }
    const Light L = *Lp; const Bufs B = *Bp;
    const int W = B.FW;
    /* the corners, as locals */
    const float ax = a.x, ay = a.y, bx_ = b.x, by_ = b.y, cx = c.x, cy = c.y;
    const float aiw = a.iw, biw = b.iw, ciw = c.iw, auw = a.uw, buw = b.uw, cuw = c.uw, avw = a.vw, bvw = b.vw, cvw = c.vw;
    const float anx = a.nx, bnx = b.nx, cnx = c.nx, any_ = a.ny, bny = b.ny, cny = c.ny, anz = a.nz, bnz = b.nz, cnz = c.nz;
    const float apx = a.pxw, bpx = b.pxw, cpx = c.pxw, apy = a.pyw, bpy = b.pyw, cpy = c.pyw, apz = a.pzw, bpz = b.pzw, cpz = c.pzw;
    /* the face's own bytes for pass 3, the same at every pixel */
    const uint8_t t0 = (uint8_t)tr, t1 = (uint8_t)tg, t2 = (uint8_t)tb;
    const uint8_t mk = (uint8_t)(fclamp(km, 0, 1) * 255 + .5f), mka = (uint8_t)(fclamp(ka, 0, 1) * 255 + .5f);
    const uint8_t tint12[12] = { t0, t1, t2, t0, t1, t2, t0, t1, t2, t0, t1, t2 };   /* four pixels' tints */
    float inv = 1 / area;
    int y0, y1; rows_of(ay, by_, cy, B.FH, &y0, &y1);
    if (y0 < r0) y0 = r0;
    if (y1 > r1 - 1) y1 = r1 - 1;
    float e0x = (by_ - cy) * inv, e1x = (cy - ay) * inv;
    const Edges E = edges_of(ax, ay, bx_, by_, cx, cy);
    const int receiver = flags & F_RECEIVER, receives = flags & F_RECEIVE;
    const uint8_t kind = receives ? 1 : 3;
    const float twf = (float)tw, thf = (float)th, txmax = tw - 1.001f, tymax = th - 1.001f;
    const int bmax = (tw * th - 1) * 2;
    const i4 lane = { 0, 1, 2, 3 };
    uint32_t nwalk = 0, nshade = 0;
    for (int y = y0; y <= y1; y++) {
        float sy = y + .5f; int xl, xr;
        if (!span(&E, sy, W, &xl, &xr)) continue;
        float px0 = xl + .5f;
        float w0 = ((bx_ - px0) * (cy - sy) - (by_ - sy) * (cx - px0)) * inv;
        float w1 = ((cx - px0) * (ay - sy) - (cy - sy) * (ax - px0)) * inv;
        nwalk += (uint32_t)(xr - xl + 1);
        for (int x = xl; x <= xr; x += 4) {
            /* the four pixels' weights, stepped one pixel at a time as the scalar loop steps them */
            f4 w0v, w1v;
            for (int k = 0; k < 4; k++) { w0v[k] = w0; w1v[k] = w1; w0 += e0x; w1 += e1x; }
            const int idx = y * W + x;
            f4 w2v = 1.f - w0v - w1v;
            f4 iw = w0v * aiw + w1v * biw + w2v * ciw, depth = 1.f / iw;
            /* the depth buffer four at a time while all four are on this row of the frame (a lane past the span is
             * written back as it was read, by the thread that owns the row); at the frame's right edge one at a
             * time, so no read or write reaches the next row, which another band's thread may be drawing */
            const int whole = x + 4 <= W;
            f4 zold = vf(0);
            if (whole) memcpy(&zold, &B.zb[idx], sizeof zold);
            else for (int k = 0; x + k < W; k++) zold[k] = B.zb[idx + k];
            const i4 m = ((lane + x) <= xr) & ~(depth >= zold);
            if (!vany(m)) continue;
            if (whole) { f4 znew = vsel(m, depth, zold); memcpy(&B.zb[idx], &znew, sizeof znew); }
            else for (int k = 0; k < 4; k++) if (m[k]) B.zb[idx + k] = depth[k];
            nshade -= (uint32_t)(m[0] + m[1] + m[2] + m[3]);
            f4 wx = (w0v * apx + w1v * bpx + w2v * cpx) * depth, wy = (w0v * apy + w1v * bpy + w2v * cpy) * depth, wz = (w0v * apz + w1v * bpz + w2v * cpz) * depth;
            if (receiver) {   /* the table: pass 3 writes its shadow */
                i4 gu4, gv4; f4 gd4;
                light4(&L, wx, wy, wz, vf(0), vf(0), vf(1), vf(L.LZ), &gu4, &gv4, &gd4);
                for (int k = 0; k < 4; k++) if (m[k]) { int j = idx + k; B.gf[j] = 2; B.gu[j] = (uint16_t)gu4[k]; B.gv[j] = (uint16_t)gv4[k]; B.gd[j] = gd4[k]; }
                continue;
            }
            f4 u = (w0v * auw + w1v * buw + w2v * cuw) * depth, v = (w0v * avw + w1v * bvw + w2v * cvw) * depth;
            f4 tx = u * twf - .5f, ty = v * thf - .5f;
            tx = vsel(tx == tx, tx, vf(0)); ty = vsel(ty == ty, ty, vf(0));   /* a lane thrown away may be NaN: texel 0 */
            tx = vclamp(tx, 0, txmax); ty = vclamp(ty, 0, tymax);
            i4 ix = vint(tx), iy = vint(ty); f4 fx = tx - vflt(ix), fy = ty - vflt(iy);
            i4 i00 = (iy * tw + ix) * 4;
            f4 g00 = (1.f - fx) * (1.f - fy), g10 = fx * (1.f - fy), g01 = (1.f - fx) * fy, g11 = fx * fy;
            /* the four texels round each pixel: two neighbours on its row and two on the next, one load a pair */
            i4 p00, p10, p01, p11;
            for (int k = 0; k < 4; k++) {
                uint32_t q[2];
                memcpy(q, td + i00[k], 8); p00[k] = (int32_t)q[0]; p10[k] = (int32_t)q[1];
                memcpy(q, td + i00[k] + tw * 4, 8); p01[k] = (int32_t)q[0]; p11[k] = (int32_t)q[1];
            }
            f4 r = vflt(p00 & 255) * g00 + vflt(p10 & 255) * g10 + vflt(p01 & 255) * g01 + vflt(p11 & 255) * g11;
            f4 g = vflt((p00 >> 8) & 255) * g00 + vflt((p10 >> 8) & 255) * g10 + vflt((p01 >> 8) & 255) * g01 + vflt((p11 >> 8) & 255) * g11;
            f4 bl = vflt((p00 >> 16) & 255) * g00 + vflt((p10 >> 16) & 255) * g10 + vflt((p01 >> 16) & 255) * g01 + vflt((p11 >> 16) & 255) * g11;
            /* the normal: interpolated from the vertices, then bent by the normal map along the tangent frame */
            f4 nx = w0v * anx + w1v * bnx + w2v * cnx, ny = w0v * any_ + w1v * bny + w2v * cny, nz = w0v * anz + w1v * bnz + w2v * cnz;
            if (bn) {
                /* the texel under the pixel; the products held to +-65536 first, so a thrown-away lane converts in range
                 * (a real one is between 0 and the texture's side) */
                f4 vt = v * thf, ut = u * twf;
                vt = vsel(vt == vt, vclamp(vt, -65536, 65536), vf(0)); ut = vsel(ut == ut, vclamp(ut, -65536, 65536), vf(0));
                i4 bi = (vint(vt) * tw + vint(ut)) * 2;
                bi = vsel_i(bi < 0, (i4){ 0, 0, 0, 0 }, bi); bi = vsel_i(bi > bmax, (i4){ bmax, bmax, bmax, bmax }, bi);
                /* a texel's two signed bytes in one load, then both split out of all four at once */
                i4 q;
                for (int k = 0; k < 4; k++) { uint16_t two; memcpy(&two, bn + bi[k], 2); q[k] = two; }
                f4 dx = vflt((i4)((u4)q << 24) >> 24) * (1.f / 20), dy = vflt((i4)((u4)q << 16) >> 24) * (1.f / 20);
                nx += Tx * dx + Bx * dy; ny += Ty * dx + By * dy; nz += Tz * dx + Bz * dy;
            }
            /* the normal's length is near 1 (unit normals, a small bend): one Newton step from 1 for its inverse */
            f4 nn = nx * nx + ny * ny + nz * nz, nl = 1.5f - .5f * nn, lit = (nx * L.LX + ny * L.LY + nz * L.LZ) * nl;
            lit = vsel(lit < 0, vf(0), vsel(lit > 1, vf(1), lit));
            f4 cr = vsel(wz < 10, 1.f - wz * .1f, vf(0)); cr = vsel(cr < 0, vf(0), cr);   /* the crease at the foot */
            /* kept for pass 3: the texel, the tint and its scale and floor, the direct light's cosine, the crease, the place in the light */
            const i4 opaque = { -16777216, -16777216, -16777216, -16777216 };   /* 0xff000000: alpha 255 */
            i4 rgba = (vint(r) & 255) | ((vint(g) & 255) << 8) | ((vint(bl) & 255) << 16) | opaque;
            i4 gk4 = vint(lit * 255 + .5f), gc4 = vint(cr * cr * 255 + .5f);
            i4 gu4 = { 0, 0, 0, 0 }, gv4 = { 0, 0, 0, 0 }; f4 gd4 = vf(0);
            const i4 keep = m & (lit > 0);
            if (receives && vany(keep)) light4(&L, wx, wy, wz, nx * nl, ny * nl, nz * nl, lit, &gu4, &gv4, &gd4);
            if (m[0] & m[1] & m[2] & m[3]) {
                /* all four drawn (most of a span): each buffer's four entries in one store */
                const u8x4 gk8 = __builtin_convertvector(gk4, u8x4), gc8 = __builtin_convertvector(gc4, u8x4);
                memcpy(&B.fb[idx * 4], &rgba, 16); memcpy(&B.gt[idx * 3], tint12, 12);
                memcpy(&B.gk[idx], &gk8, 4); memcpy(&B.gc[idx], &gc8, 4);
                memset(&B.gm[idx], mk, 4); memset(&B.ga[idx], mka, 4); memset(&B.gf[idx], kind, 4);
                if (receives && (keep[0] & keep[1] & keep[2] & keep[3])) {
                    const u16x4 gu16 = __builtin_convertvector(gu4, u16x4), gv16 = __builtin_convertvector(gv4, u16x4);
                    memcpy(&B.gu[idx], &gu16, 8); memcpy(&B.gv[idx], &gv16, 8); memcpy(&B.gd[idx], &gd4, 16);
                } else if (receives) for (int k = 0; k < 4; k++) if (keep[k]) { B.gu[idx + k] = (uint16_t)gu4[k]; B.gv[idx + k] = (uint16_t)gv4[k]; B.gd[idx + k] = gd4[k]; }
                continue;
            }
            for (int k = 0; k < 4; k++) {
                if (!m[k]) continue;
                int j = idx + k;
                int32_t px = rgba[k]; memcpy(&B.fb[j * 4], &px, 4);
                B.gt[j * 3] = t0; B.gt[j * 3 + 1] = t1; B.gt[j * 3 + 2] = t2; B.gk[j] = (uint8_t)gk4[k];
                B.gm[j] = mk; B.ga[j] = mka; B.gc[j] = (uint8_t)gc4[k];
                B.gf[j] = kind;
                if (receives && keep[k]) { B.gu[j] = (uint16_t)gu4[k]; B.gv[j] = (uint16_t)gv4[k]; B.gd[j] = gd4[k]; }
            }
        }
    }
    *walked += nwalk; *shaded += nshade;
}

/* ---- pass 3: the shadow, once a pixel. A pixel nothing was drawn on is the table (the lens is shifted, so
 *      the table maps 1:1: its world point is the pixel's), and takes the shadow that falls there. Rows
 *      [r0, r1), four-aligned (the open table is done in 4-by-4 blocks). */
static void shade(const Light *Lp, const Bufs *Bp, int r0, int r1)
{
    const Light L = *Lp; const Bufs B = *Bp;
    const int W = B.FW, H = B.FH, AW_ = B.AW, AH_ = B.AH;
    const float dpr = DPR, pad = PAD, dark = SH_DARK;
    const float *const aob = B.ao; uint8_t *const gfb = B.gf, *const fbb = B.fb;
    /* the open table first, in 4-by-4 blocks: one lookup each (its shadow is soft anyway) */
    for (int by = r0; by < r1; by += 4) for (int bx = 0; bx < W; bx += 4) {
        int i = by * W + bx;
        if (gfb[i] && gfb[i + (bx + 3 < W ? 3 : 0)] && gfb[i + (by + 3 < H ? 3 * W : 0)]) continue;   /* a block some body covers is done below */
        keep_light(&L, &B, i, (bx + 1.5f) / dpr, (by + 1.5f) / dpr - pad, 0, 0, 0, 1, L.LZ);
        float lit = 1 - (1 - lit_of(&B, i)) * dark;
        /* most blocks are under no footprint: one alpha for the block */
        int ab = (by / 4) * AW_ + bx / 4, near = aob[ab] > 0 || (bx / 4 + 1 < AW_ && aob[ab + 1] > 0) || (by / 4 + 1 < AH_ && (aob[ab + AW_] > 0 || (bx / 4 + 1 < AW_ && aob[ab + AW_ + 1] > 0))) || (bx >= 4 && aob[ab - 1] > 0) || (by >= 4 && aob[ab - AW_] > 0);
        uint8_t al = (uint8_t)((1 - lit) * 255 + .5f);
        const uint32_t open4 = 0x00020300u | (uint32_t)al << 24;   /* the bytes 0, 3, 2, al: the table's dark */
        for (int y = by; y < by + 4 && y < H; y++) {
            const int j0 = y * W + bx;
            if (!near && bx + 4 <= W) {
                /* a block row nothing is drawn on and no footprint reaches: its four pixels at once */
                uint32_t g4; memcpy(&g4, &gfb[j0], 4);
                if (!g4) { for (int k = 0; k < 4; k++) memcpy(&fbb[(j0 + k) * 4], &open4, 4); memset(&gfb[j0], 4, 4); continue; }
            }
            for (int x = bx; x < bx + 4 && x < W; x++) {
                int j = y * W + x; if (gfb[j]) continue;
                uint8_t *o = &fbb[j * 4]; o[0] = 0; o[1] = 3; o[2] = 2; o[3] = near ? (uint8_t)((1 - lit * (1 - ao_at(&B, x, y))) * 255 + .5f) : al; gfb[j] = 4;
            }
        }
    }
    /* then every pixel the blocks left: the table's at a body's edge, and the bodies' */
    for (int y = r0; y < r1; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, f = gfb[i]; if (f == 4) continue;
        uint8_t *o = &fbb[i * 4];
        if (!f) { float wx = x / dpr, wy = y / dpr - pad; keep_light(&L, &B, i, wx, wy, 0, 0, 0, 1, L.LZ); f = 2; }
        if (f == 2) { float lit = 1 - (1 - lit_of(&B, i)) * dark; o[0] = 0; o[1] = 3; o[2] = 2; o[3] = (uint8_t)((1 - lit * (1 - ao_at(&B, x, y))) * 255 + .5f); continue; }
        /* a body's light is the lamp's, by the cosine, less the shadow on it (a face turned from the lamp and a face
         * in its shadow look alike: neither sees it), on an ambient floor; the tint's scale and floor, then the crease */
        float dl = B.gk[i] * (1.f / 255);
        if (f == 1 && dl > 0) { float sh = 1 - lit_of(&B, i); dl *= 1 - .9f * sh; }
        float k = (.74f - .7f * dl) * (B.gm[i] * (1.f / 255)) + B.ga[i] * (1.f / 255), cr = B.gc[i] * (1.f / 255);
        k += (1 - k) * .4f * cr; if (k > 1) k = 1;
        const uint8_t *t = &B.gt[i * 3];
        o[0] = (uint8_t)(o[0] + (t[0] - o[0]) * k); o[1] = (uint8_t)(o[1] + (t[1] - o[1]) * k); o[2] = (uint8_t)(o[2] + (t[2] - o[2]) * k);
    }
}

/* the contact dark, a block at a time, over each footprint's reach (cn_scene_prepare: every band reads it) */
static void contact(void)
{
    const float dpr = DPR, pad = PAD;
    memset(ao, 0, (size_t)AW * AH * 4);
    for (int oi = 0; oi < nocc; oi++) {
        const Occ c = occ[oi]; float reach = c.r + c.out;
        /* held near the map before the conversion (a host's far footprint must not overflow it); -2 and one past
         * the end give the blocks the unheld numbers would */
        float fbx0 = fclamp((c.x - reach) * dpr / 4, -2, (float)AW + 1), fbx1 = fclamp((c.x + reach) * dpr / 4, -2, (float)AW + 1);
        float fby0 = fclamp((c.y - reach + pad) * dpr / 4, -2, (float)AH + 1), fby1 = fclamp((c.y + reach + pad) * dpr / 4, -2, (float)AH + 1);
        int bx0 = (int)fbx0, bx1 = (int)fbx1 + 1, by0 = (int)fby0, by1 = (int)fby1 + 1;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 >= AW) bx1 = AW - 1;
        if (by1 >= AH) by1 = AH - 1;
        for (int by = by0; by <= by1; by++) for (int bx = bx0; bx <= bx1; bx++) {
            float px = (bx * 4 + 2) / dpr - c.x, py = (by * 4 + 2) / dpr - pad - c.y, d = fsqrt(px * px + py * py) - c.r;
            float t = d <= 0 ? 1 : d >= c.out ? 0 : 1 - d / c.out, a = c.s * t * t;
            float *m = &ao[by * AW + bx]; *m = 1 - (1 - *m) * (1 - a);
        }
    }
}

/* a face whose three corners are vertices of this frame (a host's stray index skips the face, never reads
 * past the array) */
static int face_ok(const float *F, int nverts)
{
    return F[0] >= 0 && F[0] < nverts && F[1] >= 0 && F[1] < nverts && F[2] >= 0 && F[2] < nverts;
}
/* a face's corners in the light's frame (the shadow map) */
static void corners_light(const Light *L, const float *F, SV s[3])
{
    for (int k = 0; k < 3; k++) {
        const float *V = &verts[(int)F[k] * VF];
        s[k].x = (V[0] * L->UX + V[1] * L->UY + V[2] * L->UZ - L->su0) * L->sus; s[k].y = (V[0] * L->VX + V[1] * L->VY + V[2] * L->VZ - L->sv0) * L->svs; s[k].d = -(V[0] * L->LX + V[1] * L->LY + V[2] * L->LZ);
    }
}
/* a face's corners on the screen, with what pass 2 interpolates across them */
static void corners_eye(const float *F, PV p[3])
{
    for (int k = 0; k < 3; k++) {
        const float *V = &verts[(int)F[k] * VF];
        float w = HC - V[2], iw = 1 / w, kk = HC * iw;
        p[k].x = (eyeX + (V[0] - eyeX) * kk) * DPR; p[k].y = (eyeY + (V[1] - eyeY) * kk + PAD) * DPR;
        p[k].iw = iw; p[k].uw = F[3 + k * 2] * iw; p[k].vw = F[4 + k * 2] * iw;
        p[k].nx = V[3]; p[k].ny = V[4]; p[k].nz = V[5];
        p[k].pxw = V[0] * iw; p[k].pyw = V[1] * iw; p[k].pzw = V[2] * iw;
    }
}

/* ---- the frame, prepared once, then drawn in bands --------------------------------------------- */


int cn_scene_prepare(int nverts, int nfaces)
{
    prepared = 0;
    if (!fb || nverts < 0 || nfaces < 0 || nverts > vcap || nfaces > fcap) return -1;
    const Light L = light_now();
    memset(prof, 0, sizeof prof);
    /* the map's rows of every casting face, by its index (-1: it casts nothing) */
    for (int f = 0; f < nfaces; f++) {
        const float *F = &faces[f * FF]; int flags = (int)F[15];
        srows[f * 2] = -1; srows[f * 2 + 1] = -2;
        if (!(flags & F_CAST) || !face_ok(F, nverts)) continue;
        SV s[3]; corners_light(&L, F, s);
        rows_of(s[0].y, s[1].y, s[2].y, SR, &srows[f * 2], &srows[f * 2 + 1]);
    }
    /* pass 2's order: the faces nearest the eye first (the eye is over the table, so a face's nearness is its
     * height), sorted into buckets by height, so a fragment a nearer face covers is mostly never shaded at all;
     * then only the faces that will be drawn (turned toward the eye, with a texture or the table), each with
     * its rows on the picture and its texture's half-size copies made */
    enum { NB = 1024 };
    static int32_t head[NB];
    for (int i = 0; i < NB; i++) head[i] = -1;
    for (int f = 0; f < nfaces; f++) {
        const float *F = &faces[f * FF];
        if (!face_ok(F, nverts)) continue;
        float z0 = verts[(int)F[0] * VF + 2], z1 = verts[(int)F[1] * VF + 2], z2 = verts[(int)F[2] * VF + 2];
        float z = z0 > z1 ? (z0 > z2 ? z0 : z2) : (z1 > z2 ? z1 : z2);
        int b = (int)fclamp((z + 100) * 2, 0, NB - 1);
        chain[f] = head[b]; head[b] = f;
    }
    int no = 0;
    for (int b = NB - 1; b >= 0; b--) for (int f = head[b]; f >= 0; f = chain[f]) {
        const float *F = &faces[f * FF]; int flags = (int)F[15], ti = (int)F[9];
        const float *V0 = &verts[(int)F[0] * VF], *V1 = &verts[(int)F[1] * VF], *V2 = &verts[(int)F[2] * VF];
        if (flags & F_CULL) {
            /* turned away from the eye: the face's own normal against the line to the eye */
            float e1x = V1[0] - V0[0], e1y = V1[1] - V0[1], e1z = V1[2] - V0[2], e2x = V2[0] - V0[0], e2y = V2[1] - V0[1], e2z = V2[2] - V0[2];
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
            float cx = (V0[0] + V1[0] + V2[0]) / 3, cy = (V0[1] + V1[1] + V2[1]) / 3, cz = (V0[2] + V1[2] + V2[2]) / 3;
            if (nx * (eyeX - cx) + ny * (eyeY - cy) + nz * (HC - cz) <= 0) continue;
        }
        Tex *tex = ti >= 0 && ti < ntex ? &texs[ti] : 0;
        if (!tex && !(flags & F_RECEIVER)) continue;
        if (tex && !tex->nlv) mips_of(tex);
        PV p[3]; corners_eye(F, p);
        rows_of(p[0].y, p[1].y, p[2].y, FH, &rows[no * 2], &rows[no * 2 + 1]);
        order[no++] = f;
    }
    contact();
    prep_nfaces = nfaces; prep_ndraw = no; prepared = 1;
    return no;
}

/* the rows of pass's band: the map's for the shadow, the picture's for the picture, four-aligned for the shade */
static void band_rows(int pass, int band, int nbands, int *r0, int *r1)
{
    if (pass == CN_SCENE_PASS_SHADOW) { *r0 = (int)((int64_t)SR * band / nbands); *r1 = (int)((int64_t)SR * (band + 1) / nbands); return; }
    if (pass == CN_SCENE_PASS_PICTURE) { *r0 = (int)((int64_t)FH * band / nbands); *r1 = (int)((int64_t)FH * (band + 1) / nbands); return; }
    int b0 = (int)((int64_t)AH * band / nbands), b1 = (int)((int64_t)AH * (band + 1) / nbands);
    *r0 = b0 * 4; *r1 = b1 * 4 < FH ? b1 * 4 : FH;
}

void cn_scene_band(int pass, int band, int nbands)
{
    if (!prepared || !fb || nbands < 1 || nbands > CN_SCENE_MAX_BANDS || band < 0 || band >= nbands) return;
    const Light L = light_now(); const Bufs B = bufs_now();
    int r0, r1; band_rows(pass, band, nbands, &r0, &r1);
    if (r0 >= r1) return;
    uint32_t *P = prof[band];
    if (pass == CN_SCENE_PASS_SHADOW) {
        /* pass 1, the map's rows of this band: cleared, then every casting face that reaches them */
        for (int i = r0 * B.SR, n = r1 * B.SR; i < n; i++) B.smap[i] = 1e30f;
        if (skip & 1) return;
        uint32_t texels = 0;
        for (int f = 0; f < prep_nfaces; f++) {
            if (srows[f * 2 + 1] < r0 || srows[f * 2] >= r1) continue;
            SV s[3]; corners_light(&L, &faces[f * FF], s);
            shadow_tri(s[0], s[1], s[2], B.smap, B.SR, r0, r1, &texels);
        }
        P[2] += texels; P[3] += texels;
        return;
    }
    if (pass == CN_SCENE_PASS_PICTURE) {
        /* pass 2, the picture's rows of this band: cleared, then every drawn face that reaches them, nearest first */
        memset(B.fb + (size_t)r0 * B.FW * 4, 0, (size_t)(r1 - r0) * B.FW * 4); memset(B.gf + (size_t)r0 * B.FW, 0, (size_t)(r1 - r0) * B.FW);
        for (int i = r0 * B.FW, n = r1 * B.FW; i < n; i++) B.zb[i] = 1e30f;
        if (skip & 2) return;
        uint32_t walked = 0, shaded = 0;
        for (int oi = 0; oi < prep_ndraw; oi++) {
            if (rows[oi * 2 + 1] < r0 || rows[oi * 2] >= r1) continue;
            const int f = order[oi];
            const float *F = &faces[f * FF]; const int flags = (int)F[15], ti = (int)F[9];
            const float *V0 = &verts[(int)F[0] * VF], *V1 = &verts[(int)F[1] * VF], *V2 = &verts[(int)F[2] * VF];
            const Tex *tex = ti >= 0 && ti < ntex ? &texs[ti] : 0;
            PV p[3]; corners_eye(F, p);
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
                float sa = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y); if (sa < 0) sa = -sa;
                float ta = ((F[5] - F[3]) * (F[8] - F[4]) - (F[7] - F[3]) * (F[6] - F[4])) * tex->w * tex->h; if (ta < 0) ta = -ta;
                while (lv + 1 < tex->nlv && ta > 4 * sa) { ta *= .25f; lv++; }
                td = tex->lrgba[lv]; bn = tex->lbump[lv]; tw = tex->lw[lv]; th = tex->lh[lv];
            }
            tri(&L, &B, r0, r1, &walked, &shaded, td, bn, tw, th, p[0], p[1], p[2], Tx, Ty, Tz, Bx, By, Bz, F[10], F[11], F[12], F[13], F[14], flags);
        }
        P[0] += shaded; P[1] += walked;
        return;
    }
    if (pass == CN_SCENE_PASS_SHADE && !(skip & 4)) shade(&L, &B, r0, r1);
}

/* the whole frame on the calling thread: prepared, then each pass as one band. Returns the faces drawn. */
int cn_scene_render(int nverts, int nfaces)
{
    int drawn = cn_scene_prepare(nverts, nfaces);
    if (drawn < 0) return -1;
    for (int pass = 0; pass < CN_SCENE_PASSES; pass++) cn_scene_band(pass, 0, 1);
    return drawn;
}

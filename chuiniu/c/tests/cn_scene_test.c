/* The scene renderer (src/cn_scene.c) on the host: a lit quad over the table
 * draws its texture, the table under it takes its shadow where the light says,
 * and nowhere else; textures can be added between frames; the six-seat table
 * renders to a pinned picture (the study's wasm and the iOS library run this
 * same file, so the hash is what both draw); and a frame that does not fit the
 * caller's arena, or a host's stray index, fails cleanly instead of crashing. */
#include "../src/cn_scene.h"
#include "cn_scene_frame.h"
#include "cn_check.h"

/* the quad scene: the table as a receiver, a white square 40 high; extra appends faces after it */
static int quad_frame(int extra_faces, const float *extra)
{
    if (!cn_scene_begin(100, 100, 0, 1, 50, 50, 720, -.45f, -.55f, 1, 256, .55f, 64, 4 + extra_faces)) return -2;
    float *V = cn_scene_verts(), *F = cn_scene_faces();
    const float table[] = { 0, 0, 0, 0, 0, 1,  100, 0, 0, 0, 0, 1,  100, 100, 0, 0, 0, 1,  0, 100, 0, 0, 0, 1 };
    memcpy(V, table, sizeof table);
    const float quad[] = { 30, 30, 40, 0, 0, 1,  70, 30, 40, 0, 0, 1,  70, 70, 40, 0, 0, 1,  30, 70, 40, 0, 0, 1 };
    memcpy(V + 24, quad, sizeof quad);
    const float faces[] = {
        0, 1, 2, 0, 0, 1, 0, 1, 1, -1, 0, 0, 0, 1, 0, 12,   0, 2, 3, 0, 0, 1, 1, 0, 1, -1, 0, 0, 0, 1, 0, 12,
        4, 5, 6, 0, 0, 1, 0, 1, 1,  0, 0, 4, 3, 1, 0,  7,   4, 6, 7, 0, 0, 1, 1, 0, 1,  0, 0, 4, 3, 1, 0,  7,
    };
    memcpy(F, faces, sizeof faces);
    if (extra_faces) memcpy(F + 4 * CN_SCENE_FF, extra, (size_t)extra_faces * CN_SCENE_FF * sizeof(float));
    return cn_scene_render(8, 4 + extra_faces);
}

/* THE OPEN TABLE'S SHADOW EDGE. A square 40 up, turned by 30 degrees, centred on (45, 45), and no table face:
 * its shadow falls on the open table (pass 3's blocks), centred on (63, 67), its lower right side running
 * from (83.5, 61.5) to (68.5, 87.5). On each device row through that side, the column where the shadow's
 * alpha crosses half its depth (between pixels, linearly); a straight edge drawn smoothly puts those on a
 * straight line. The worst distance of any row's crossing from the rows' least-squares line, in device
 * pixels: the edge's steps. */
static float open_edge_steps(float dpr)
{
    if (!cn_scene_begin(120, 120, 0, dpr, 60, 60, 720, -.45f, -.55f, 1, 256, .55f, 4, 2)) return 1e9f;
    float *V = cn_scene_verts(), *F = cn_scene_faces();
    const float c = .8660254f, s = .5f, h = 15;
    const float k[4][2] = { { -h, -h }, { h, -h }, { h, h }, { -h, h } };
    for (int i = 0; i < 4; i++) {
        float v[6] = { 45 + c * k[i][0] - s * k[i][1], 45 + s * k[i][0] + c * k[i][1], 40, 0, 0, 1 };
        memcpy(V + i * 6, v, sizeof v);
    }
    const float faces[] = { 0, 1, 2, 0, 0, 1, 0, 1, 1, 0, 0, 4, 3, 1, 0, 7,   0, 2, 3, 0, 0, 1, 1, 0, 1, 0, 0, 4, 3, 1, 0, 7 };
    memcpy(F, faces, sizeof faces);
    if (cn_scene_render(4, 2) != 2) return 1e9f;
    const uint8_t *fb = cn_scene_fb(); const int W = cn_scene_fb_w();
    const float half = .55f * 255 / 2;
    float xs[256], ys[256]; int n = 0;
    for (int y = (int)(66 * dpr); y <= (int)(84 * dpr) && n < 256; y++) {
        int x = W - 2;
        while (x >= 0 && fb[(y * W + x) * 4 + 3] < half) x--;
        if (x < 0) return 1e9f;
        float a0 = fb[(y * W + x) * 4 + 3], a1 = fb[(y * W + x + 1) * 4 + 3];
        xs[n] = x + (a0 - half) / (a0 - a1); ys[n] = (float)y; n++;
    }
    double sy = 0, sx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; i++) { sy += ys[i]; sx += xs[i]; syy += (double)ys[i] * ys[i]; sxy += (double)ys[i] * xs[i]; }
    double b = (n * sxy - sy * sx) / (n * syy - sy * sy), a = (sx - b * sy) / n, worst = 0;
    for (int i = 0; i < n; i++) { double r = xs[i] - (a + b * ys[i]); if (r < 0) r = -r; if (r > worst) worst = r; }
    return (float)worst;
}

/* A SQUARE OVER THE BARE TABLE (no shadow: dark 0, so every table pixel is alpha 0 and a pixel's alpha is how much
 * of it the square covers): side 30 points, 40 up, turned by deg about (60, 60), its faces numbered id (0: none) and
 * cut into `fan` triangles about its middle (2: one diagonal). The faces drawn, or -1. */
static int square_frame(float dpr, float deg, int id, int fan, int skip_mask)
{
    if (!cn_scene_begin(120, 120, 0, dpr, 60, 60, 720, -.45f, -.55f, 1, 256, 0, 8, 8)) return -1;
    float *V = cn_scene_verts(), *F = cn_scene_faces();
    const float a = deg * 3.14159265f / 180, c = __builtin_cosf(a), s = __builtin_sinf(a), h = 15;
    const float k[5][2] = { { -h, -h }, { h, -h }, { h, h }, { -h, h }, { 0, 0 } };
    for (int i = 0; i < 5; i++) {
        float v[6] = { 60 + c * k[i][0] - s * k[i][1], 60 + s * k[i][0] + c * k[i][1], 40, 0, 0, 1 };
        memcpy(V + i * 6, v, sizeof v);
    }
    const float fl = (float)(CN_SCENE_F_CULL | CN_SCENE_F_CAST | CN_SCENE_F_RECEIVE | CN_SCENE_F_ID(id));
    int nf = 0;
    if (fan == 2) {
        const float f2[] = { 0, 1, 2, 0, 0, 1, 0, 1, 1, 0, 0, 4, 3, 1, 0, fl,   0, 2, 3, 0, 0, 1, 1, 0, 1, 0, 0, 4, 3, 1, 0, fl };
        memcpy(F, f2, sizeof f2); nf = 2;
    } else {
        for (int i = 0; i < 4; i++) {
            const float f1[] = { 4, (float)i, (float)((i + 1) % 4), .5f, .5f, 0, 0, 1, 1, 0, 0, 4, 3, 1, 0, fl };
            memcpy(F + i * CN_SCENE_FF, f1, sizeof f1); nf++;
        }
    }
    cn_scene_skip(skip_mask);
    const int d = cn_scene_render(5, nf);
    cn_scene_skip(0);
    return d;
}
/* The square's left side (turned 17 degrees it runs from (50.0, 41.3) to (41.3, 70.0) points, and is the square's
 * left edge on every row between), row by row: where its coverage crosses, from the alphas (a row's pixels from the
 * first one it touches, summed: the side's place to a fraction of a pixel). The worst distance of any row's crossing
 * from the rows' least-squares line, in device pixels: a staircase is half a pixel; four samples, a quarter at most. */
static float square_steps(float dpr)
{
    const uint8_t *fb = cn_scene_fb(); const int W = cn_scene_fb_w();
    float xs[512], ys[512]; int n = 0;
    for (int y = (int)(45 * dpr); y < (int)(66 * dpr) && n < 512; y++) {
        int x = 0;
        while (x < W && fb[(y * W + x) * 4 + 3] == 0) x++;
        if (x >= W - 4) continue;
        float cover = 0;
        for (int i = 0; i < 4; i++) cover += fb[(y * W + x + i) * 4 + 3] / 255.f;
        xs[n] = x + 4 - cover; ys[n] = (float)y; n++;
    }
    if (n < 8) return 1e9f;
    double sy = 0, sx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; i++) { sy += ys[i]; sx += xs[i]; syy += (double)ys[i] * ys[i]; sxy += (double)ys[i] * xs[i]; }
    double b = (n * sxy - sy * sx) / (n * syy - sy * sy), a = (sx - b * sy) / n, worst = 0;
    for (int i = 0; i < n; i++) { double r = xs[i] - (a + b * ys[i]); if (r < 0) r = -r; if (r > worst) worst = r; }
    return (float)worst;
}
/* the pixels where two pictures differ */
static int differ(const uint8_t *a, const uint8_t *b, int npx)
{
    int n = 0;
    for (int i = 0; i < npx; i++) n += memcmp(a + i * 4, b + i * 4, 4) != 0;
    return n;
}

int main(void)
{
    size_t ios = CN_SCENE_ARENA_IOS;
    uint8_t *mem = malloc(ios);
    CHECK(mem != 0, "the iOS arena");

    TEST("the scene: a quad, its shadow, the empty table");
    CHECK(cn_scene_init(mem, ios) == 1, "the arena is taken");
    int tx = cn_scene_tex_new(4, 4, 0);
    CHECK(tx == 0, "the first texture is 0");
    memset(cn_scene_tex_rgba(tx), 255, 4 * 4 * 4);
    int drawn = quad_frame(0, 0);
    CHECK(drawn == 4, "four faces drawn (%d)", drawn);
    const uint8_t *fb = cn_scene_fb(); int W = cn_scene_fb_w();
    CHECK(W == 100 && cn_scene_fb_h() == 100, "the framebuffer is the board");
    /* the quad's middle: its white, pulled a little toward the tint by the shade, opaque */
    const uint8_t *q = &fb[(50 * W + 50) * 4];
    CHECK(q[3] == 255 && q[0] > 150 && q[0] <= 255, "the quad is drawn at its middle (%d %d %d a%d)", q[0], q[1], q[2], q[3]);
    /* its shadow on the table: the light comes from (-.45, -.55, 1), so a point 40 up throws its shadow
     * +18, +22 along the table; (80, 85) is under the square's shadow, (15, 15) is not */
    const uint8_t *s1 = &fb[(85 * W + 80) * 4], *s0 = &fb[(15 * W + 15) * 4];
    CHECK(s1[3] > 100, "the table under the square's shadow is darkened (alpha %d)", s1[3]);
    CHECK(s0[3] == 0, "the open table shows nothing (alpha %d)", s0[3]);
    /* the square's own top is lit: no self-shadow */
    const uint8_t *q2 = &fb[(40 * W + 40) * 4];
    CHECK(q2[0] > 150, "the square's top is not in its own shadow (%d)", q2[0]);
    uint64_t quad_hash = cnf_hash();
    /* a frame can begin again and a texture can be added between frames */
    CHECK(cn_scene_begin(100, 100, 0, 1, 50, 50, 720, -.45f, -.55f, 1, 256, .55f, 64, 32) > 0, "a second frame begins");
    CHECK(cn_scene_tex_new(4, 4, 0) == 1, "a texture can still be added between frames");

    TEST("a host's stray index skips its face, never reads past the vertices");
    {
        cn_scene_init(mem, ios); tx = cn_scene_tex_new(4, 4, 0); memset(cn_scene_tex_rgba(tx), 255, 4 * 4 * 4);
        const float bad[] = {
            0, 1, 100000, 0, 0, 1, 0, 1, 1, 0, 0, 4, 3, 1, 0, 7,     /* a corner past the vertices */
            -1, 1, 2, 0, 0, 1, 0, 1, 1, 0, 0, 4, 3, 1, 0, 7,         /* a negative corner */
            0, __builtin_nanf(""), 2, 0, 0, 1, 0, 1, 1, 0, 0, 4, 3, 1, 0, 7,   /* not a number */
        };
        int d = quad_frame(3, bad);
        CHECK(d == 4, "the four good faces drawn, the three bad ones skipped (%d)", d);
        CHECK(cnf_hash() == quad_hash, "the picture is the quad's alone");
    }

    TEST("the open table's shadow edge is smooth at every scale");
    {
        cn_scene_init(mem, ios); tx = cn_scene_tex_new(4, 4, 0); memset(cn_scene_tex_rgba(tx), 255, 4 * 4 * 4);
        /* a map texel is about 1.1 points here. The map's own staircase, through the filter, leaves about a
         * third of a texel; a 4-by-4 block lit as one, or a filter that skips every other texel, leaves more
         * than a texel (the old renderer: 3.1 to 3.9 device pixels at 1.5x to 3x) */
        const float scales[4] = { 1, 2, 3, 1.5f };
        for (int i = 0; i < 4; i++) {
            float steps = open_edge_steps(scales[i]);
            CHECK(steps < .42f * scales[i], "%.1fx: the slanted edge's crossings stray %.2f device pixels from a line (at most %.2f)", scales[i], steps, .42f * scales[i]);
        }
    }

    TEST("the open table's blocks taken whole draw what every pixel lit alone draws");
    {
        cn_scene_init(mem, ios);
        CnfTex t;
        cnf_textures(&t, 1);
        const float scales[3] = { 1, 2, 3 };
        for (int i = 0; i < 3; i++) for (int roll = 0; roll < 2; roll++) {
            if (cnf_frame(&t, 390, 718, 40, scales[i], 1024, roll ? .6f : 0) < 0) continue;   /* 3x does not fit 48 MB beside these textures */
            const uint64_t h = cnf_hash();
            cn_scene_skip(8); cnf_frame(&t, 390, 718, 40, scales[i], 1024, roll ? .6f : 0); cn_scene_skip(0);
            CHECK(cnf_hash() == h, "%.0fx %s: the blocks settled whole are the pixels' own light (%016llx, pixel by pixel %016llx)",
                  scales[i], roll ? "in a throw" : "still", (unsigned long long)h, (unsigned long long)cnf_hash());
        }
    }

    TEST("past 65,536 faces every face is still ordered and drawn");
    {
        /* 70,000 faces: the quad's two last, everything before them a sliver far off the board (casting
         * nothing). The old fixed chain of 65,536 left every face past it out of the picture. */
        enum { N = 70000 };
        cn_scene_init(mem, ios); tx = cn_scene_tex_new(4, 4, 0); memset(cn_scene_tex_rgba(tx), 255, 4 * 4 * 4);
        CHECK(cn_scene_begin(100, 100, 0, 1, 50, 50, 720, -.45f, -.55f, 1, 256, .55f, 12, N) > 0, "a frame of 70,000 faces begins");
        float *V = cn_scene_verts(), *F = cn_scene_faces();
        const float v[] = { 30, 30, 40, 0, 0, 1,  70, 30, 40, 0, 0, 1,  70, 70, 40, 0, 0, 1,  30, 70, 40, 0, 0, 1,
                            -900, -900, 1, 0, 0, 1,  -899, -900, 1, 0, 0, 1,  -900, -899, 1, 0, 0, 1 };
        memcpy(V, v, sizeof v);
        for (int f = 0; f < N - 2; f++) { const float s[] = { 4, 5, 6, 0, 0, 1, 0, 0, 1, 0, 0, 4, 3, 1, 0, 4 }; memcpy(F + f * CN_SCENE_FF, s, sizeof s); }
        const float qf[] = { 0, 1, 2, 0, 0, 1, 0, 1, 1, 0, 0, 4, 3, 1, 0, 7,   0, 2, 3, 0, 0, 1, 1, 0, 1, 0, 0, 4, 3, 1, 0, 7 };
        memcpy(F + (N - 2) * CN_SCENE_FF, qf, sizeof qf);
        int d = cn_scene_render(7, N);
        const uint8_t *p = &cn_scene_fb()[(50 * 100 + 50) * 4];
        CHECK(d == N, "every face drawn (%d)", d);
        CHECK(p[3] == 255 && p[0] > 150, "the quad, faces 69,998 and 69,999, is in the picture (%d a%d)", p[0], p[3]);
    }

    TEST("the six-seat table: the picture is pinned");
    {
        cn_scene_init(mem, ios);
        CnfTex t;
        CHECK(cnf_textures(&t, 1) == 0, "one set of the study's textures fits the iOS arena");
        int d = cnf_frame(&t, 390, 718, 40, 1, 1024, 0);
        CHECK(cnf_nf == 12864 && d > 6000 && d < cnf_nf, "six cups and thirty dice: %d faces, %d drawn (the rest turned away)", cnf_nf, d);
        uint64_t h = cnf_hash();
        /* re-pinned for package V1: the picture is the same bytes but at the pixels on a surface's edge, which the
         * edges pass draws from four samples (the frame with the edges left out is the old golden, 78fcf817e8be90f8,
         * and 0xb65b95f13c6d841a below; checked when the pin moved) */
        CHECK(h == 0x658558ffc67caa06ull, "the still table at 1x: %016llx", (unsigned long long)h);
        d = cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        h = cnf_hash();
        CHECK(h == 0x2ffa90346b8d21d5ull, "a throw's frame at 1.5x: %016llx", (unsigned long long)h);
        /* the edges left out: the old renderer's picture, byte for byte (the strips change no pixel) */
        cn_scene_skip(16); cnf_frame(&t, 390, 718, 40, 1, 1024, 0); cn_scene_skip(0);
        CHECK(cnf_hash() == 0x78fcf817e8be90f8ull, "the still table at 1x without the edges: the old golden (%016llx)", (unsigned long long)cnf_hash());
        cn_scene_skip(16); cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f); cn_scene_skip(0);
        CHECK(cnf_hash() == 0xb65b95f13c6d841aull, "a throw's frame at 1.5x without the edges: the old golden (%016llx)", (unsigned long long)cnf_hash());
        cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        /* drawing it again draws it the same: nothing carries from one frame to the next */
        cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        CHECK(cnf_hash() == h, "the same frame twice, the same picture");
    }

    TEST("in bands, any number, in any order: the same picture and the same work");
    {
        cn_scene_init(mem, ios);
        CnfTex t;
        cnf_textures(&t, 1);
        cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        const uint64_t h = cnf_hash();
        const uint32_t shaded = cn_scene_prof(0), walked = cn_scene_prof(1), texels = cn_scene_prof(2), edged = cn_scene_prof(4);
        const int counts[5] = { 2, 3, 7, 13, CN_SCENE_MAX_BANDS };
        for (int c = 0; c < 5; c++) {
            const int nb = counts[c];
            cnf_build(&t, 390, 718, 40, 1.5f, 1024, .6f);
            int d = cn_scene_prepare(cnf_nv, cnf_nf);
            /* the bands of a pass last to first: a band may not lean on a band before it */
            for (int pass = 0; pass < CN_SCENE_PASSES; pass++) for (int b = nb - 1; b >= 0; b--) cn_scene_band(pass, b, nb);
            CHECK(d > 6000 && cnf_hash() == h, "%d bands draw the one-thread picture (%016llx)", nb, (unsigned long long)cnf_hash());
            /* every row is walked once: bands that overlapped would walk the shared rows twice */
            CHECK(cn_scene_prof(0) == shaded && cn_scene_prof(1) == walked && cn_scene_prof(2) == texels && cn_scene_prof(4) == edged && edged > 0,
                  "%d bands: %u fragments, %u walked, %u texels, %u edge pixels (one thread: %u, %u, %u, %u)", nb, cn_scene_prof(0), cn_scene_prof(1), cn_scene_prof(2),
                  cn_scene_prof(4), shaded, walked, texels, edged);
        }
        /* a band out of range, or before cn_scene_prepare, draws nothing */
        cnf_build(&t, 390, 718, 40, 1.5f, 1024, .6f);
        cn_scene_band(CN_SCENE_PASS_PICTURE, 0, 1);
        CHECK(cn_scene_prof(1) == walked, "a band before cn_scene_prepare draws nothing (the last frame's %u walked stand, not %u)", walked, cn_scene_prof(1));
        cn_scene_prepare(cnf_nv, cnf_nf);
        cn_scene_band(CN_SCENE_PASS_PICTURE, 0, CN_SCENE_MAX_BANDS + 1);
        cn_scene_band(CN_SCENE_PASS_PICTURE, 3, 3);
        cn_scene_band(CN_SCENE_PASS_PICTURE, -1, 3);
        CHECK(cn_scene_prof(1) == 0, "bands out of range draw nothing (%u walked)", cn_scene_prof(1));
    }

    TEST("the edges: a body's silhouette is smooth, and only its edge pixels change");
    {
        cn_scene_init(mem, ios); tx = cn_scene_tex_new(4, 4, 0); memset(cn_scene_tex_rgba(tx), 255, 4 * 4 * 4);
        static uint8_t off[400 * 400 * 4];
        const float scales[3] = { 1, 1.5f, 2 };
        for (int i = 0; i < 3; i++) {
            const float dpr = scales[i];
            CHECK(square_frame(dpr, 17, 1, 2, 16) == 2, "%.1fx: the square, the edges left out", dpr);
            const int npx = cn_scene_fb_w() * cn_scene_fb_h();
            const float stair = square_steps(dpr);
            memcpy(off, cn_scene_fb(), (size_t)npx * 4);
            CHECK(square_frame(dpr, 17, 1, 2, 0) == 2, "%.1fx: the square, its edges drawn", dpr);
            const float smooth = square_steps(dpr);
            /* the side, turned 17 degrees, steps a pixel every three or so rows: drawn from the centres alone it strays
             * up to half a pixel from its line; from four samples (coverage in quarters) under a quarter */
            CHECK(stair > .4f && smooth < .25f, "%.1fx: the slanted side strays %.2f pixels from a line (centres alone %.2f)", dpr, smooth, stair);
            const int changed = differ(off, cn_scene_fb(), npx);
            CHECK(changed > 0 && (uint32_t)changed <= cn_scene_prof(4), "%.1fx: %d pixels changed, each an edge pixel (%u)", dpr, changed, cn_scene_prof(4));
        }
    }

    TEST("the surfaces: one body's facets are never an edge; two bodies are");
    {
        cn_scene_init(mem, ios); tx = cn_scene_tex_new(4, 4, 0); memset(cn_scene_tex_rgba(tx), 255, 4 * 4 * 4);
        /* the square as two triangles and as four (a fan from its middle): the same silhouette, the same edge pixels,
         * however its inside is cut */
        square_frame(2, 17, 1, 2, 0); const uint32_t two = cn_scene_prof(4);
        square_frame(2, 17, 1, 4, 0); const uint32_t four = cn_scene_prof(4);
        CHECK(two > 0 && two == four, "two facets or four: %u and %u edge pixels", two, four);
        /* two squares of one texture, the second 10 up and over the first's corner: numbered apart, the line where the
         * upper one crosses the lower is an edge too; numbered alike, it is one surface and it is not */
        int ids[2][2] = { { 1, 2 }, { 1, 1 } };
        uint32_t edge[2];
        for (int k = 0; k < 2; k++) {
            CHECK(cn_scene_begin(120, 120, 0, 2, 60, 60, 720, -.45f, -.55f, 1, 256, 0, 8, 4) > 0, "two squares begin");
            float *V = cn_scene_verts(), *F = cn_scene_faces();
            const float v[] = { 30, 30, 30, 0, 0, 1,  70, 30, 30, 0, 0, 1,  70, 70, 30, 0, 0, 1,  30, 70, 30, 0, 0, 1,
                                50, 52, 40, 0, 0, 1,  90, 52, 40, 0, 0, 1,  90, 92, 40, 0, 0, 1,  50, 92, 40, 0, 0, 1 };
            memcpy(V, v, sizeof v);
            for (int q = 0; q < 2; q++) {
                const float fl = (float)(CN_SCENE_F_CULL | CN_SCENE_F_CAST | CN_SCENE_F_RECEIVE | CN_SCENE_F_ID(ids[k][q])), b = (float)(q * 4);
                const float f2[] = { b, b + 1, b + 2, 0, 0, 1, 0, 1, 1, 0, 0, 4, 3, 1, 0, fl,   b, b + 2, b + 3, 0, 0, 1, 1, 0, 1, 0, 0, 4, 3, 1, 0, fl };
                memcpy(F + q * 2 * CN_SCENE_FF, f2, sizeof f2);
            }
            cn_scene_render(8, 4);
            edge[k] = cn_scene_prof(4);
        }
        CHECK(edge[0] > edge[1] + 40, "where the upper square crosses the lower: %u edge pixels numbered apart, %u numbered alike", edge[0], edge[1]);
    }

    TEST("premultiplied: the straight picture times its alpha");
    {
        cn_scene_init(mem, ios);
        CnfTex t;
        cnf_textures(&t, 1);
        static uint8_t st[585 * 1137 * 4];
        cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        const int npx = cn_scene_fb_w() * cn_scene_fb_h();
        CHECK(npx <= 585 * 1137, "the frame fits the copy");
        memcpy(st, cn_scene_fb(), (size_t)npx * 4);
        cn_scene_premultiply(1);
        cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        cn_scene_premultiply(0);
        const uint8_t *pm = cn_scene_fb();
        /* every pixel's alpha is the straight one's, and each colour is the straight one's times it, rounded (an edge
         * pixel's mean is taken in each form, so it may round a step the other way) */
        int bad = 0, opaque = 0, same = 0, worst = 0;
        for (int i = 0; i < npx; i++) {
            const uint8_t *s = &st[i * 4], *p = &pm[i * 4];
            if (s[3] != p[3]) { bad++; continue; }
            for (int c = 0; c < 3; c++) {
                int want = (s[c] * s[3] + 127) / 255, d = p[c] - want; if (d < 0) d = -d;
                if (d > worst) worst = d;
                bad += d > 1;
            }
            if (s[3] == 255) { opaque++; same += !memcmp(s, p, 4); }
        }
        CHECK(bad == 0 && worst <= 1, "%d pixels' alpha or colour not the straight one's times its alpha (worst %d)", bad, worst);
        CHECK(opaque > 10000 && same == opaque, "an opaque pixel is the same bytes either way (%d of %d)", same, opaque);
        cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        CHECK(!memcmp(st, cn_scene_fb(), (size_t)npx * 4), "straight again: the straight picture");
    }

    TEST("the memory: a frame keeps its picture whole and a strip a band");
    {
        /* the stage's tallest drawer: 430 by 830 points and the 361 above it the far cups reach, beside the 12.2 MB
         * texture set (DECISIONS I20); and the bench's six seats on 390 by 718 */
        const size_t tex = (size_t)12800000;   /* 12.2 MB with the half-size copies */
        const struct { int W, H, pad; } d[2] = { { 430, 830, 361 }, { 390, 718, 40 } };
        const float scales[4] = { 1, 1.5f, 2, 3 };
        for (int i = 0; i < 2; i++) for (int k = 0; k < 4; k++) {
            const size_t b = cn_scene_frame_bytes(d[i].W, d[i].H, d[i].pad, scales[k], 1024, CNF_VCAP, CNF_FCAP);
            printf("  %d by %d, %d above, at %.1fx: the frame %.1f MB, with the textures %.1f of 48 MB\n", d[i].W, d[i].H, d[i].pad, scales[k],
                   b / 1048576.0, (b + tex) / 1048576.0);
        }
        const size_t tall2 = cn_scene_frame_bytes(430, 830, 361, 2, 1024, CNF_VCAP, CNF_FCAP);
        CHECK(tall2 + tex <= CN_SCENE_ARENA_IOS, "430 by 830 and 361 above at 2x fits 48 MB beside the textures (%.1f MB)", (tall2 + tex) / 1048576.0);
        /* a picture's bytes a pixel: the picture's 4, its surface's 1, its face's 2 and the contact dark's quarter; the
         * strips and the faces do not grow with its height: a million pixels more is 7.25 MB more and a row's 8 (the
         * renderer before package V1 kept 25 a pixel whole) */
        const size_t a = cn_scene_frame_bytes(1000, 1000, 0, 1, 1024, 64, 64), b = cn_scene_frame_bytes(1000, 2000, 0, 1, 1024, 64, 64);
        CHECK(b - a >= (size_t)7250000 && b - a < (size_t)7270000, "a million pixels more: %zu bytes more", b - a);
    }

    TEST("a frame that does not fit fails cleanly");
    {
        cn_scene_init(mem, ios);
        CnfTex t;
        cnf_textures(&t, 1);
        size_t need3 = cn_scene_frame_bytes(430, 830, 361, 3, 1024, CNF_VCAP, CNF_FCAP);
        CHECK(need3 > cn_scene_room(), "430 by 830 and 361 above at 3x is past 48 MB with the textures (%zu > %zu)", need3, cn_scene_room());
        CHECK(cnf_frame(&t, 430, 830, 361, 3, 1024, 0) == -1, "it does not begin");
        CHECK(cn_scene_fb() == 0 && cn_scene_verts() == 0 && cn_scene_faces() == 0 && cn_scene_fb_w() == 0, "nothing of a frame is handed out");
        CHECK(cn_scene_render(10, 10) == -1, "and nothing renders");
        CHECK(cnf_frame(&t, 390, 718, 40, 1, 1024, 0) > 0, "a 1x frame still draws after it");
        /* the bytes cn_scene_frame_bytes names are what a frame takes: an arena of exactly that begins,
         * 16 fewer does not */
        size_t need = cn_scene_frame_bytes(100, 100, 0, 1, 256, 64, 32);
        CHECK(need > 0 && need % 16 == 0, "a frame's bytes (%zu)", need);
        CHECK(cn_scene_init(mem, need) == 1 && cn_scene_begin(100, 100, 0, 1, 50, 50, 720, -.45f, -.55f, 1, 256, .55f, 64, 32) > 0, "exactly its bytes: it begins");
        CHECK(cn_scene_init(mem, need - 16) == 1 && cn_scene_begin(100, 100, 0, 1, 50, 50, 720, -.45f, -.55f, 1, 256, .55f, 64, 32) == 0, "16 fewer: it does not");
        CHECK(cn_scene_fb() == 0 && cn_scene_render(4, 4) == -1, "and is not drawable");
        /* a texture that does not fit takes nothing */
        size_t room = cn_scene_room();
        CHECK(cn_scene_tex_new(4096, 4096, 1) == -1 && cn_scene_room() == room, "a texture past the room is refused and takes nothing");
        CHECK(cn_scene_init(mem, ios) == 1, "the arena again");
        CHECK(cn_scene_tex_new(2048, 2048, 0) == 0 && cn_scene_tex_new(2048, 2048, 0) == 1, "two 16 MB textures");
        /* a third with a normal map: its 16 MB of colour fit, its 8 MB of bumps do not, and the colour is given back */
        room = cn_scene_room();
        CHECK(cn_scene_tex_new(2048, 2048, 1) == -1 && cn_scene_room() == room, "a third with bumps is refused whole (room %zu)", cn_scene_room());
        CHECK(cn_scene_tex_new(2048, 2048, 0) == 2, "a third without them fills the 48 MB exactly");
        CHECK(cn_scene_tex_new(2, 2, 0) == -1 && cn_scene_begin(1, 1, 0, 1, 0, 0, 720, 0, 0, 1, 4, .5f, 1, 1) == 0, "and nothing more fits");
        /* out of range numbers, in an empty arena (each refusal is the numbers', not the memory's) */
        CHECK(cn_scene_init(mem, ios) == 1 && cn_scene_begin(100, 100, 0, 1, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) > 0, "the control: these numbers begin");
        /* one number to each refusal in frame_sizes */
        CHECK(cn_scene_begin(100, 0, 10, 1, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a board of no height, though its pad has rows");
        CHECK(cn_scene_begin(100, 100, -1, 1, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a pad upward");
        /* (each of these frames would fit the arena: the refusal is the rule's, not the memory's) */
        CHECK(cn_scene_begin(-100, 1, 0, -.4f, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a negative scale: no rows, though times a negative width it gives columns");
        CHECK(cn_scene_begin(9000, 1, 0, 1, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a framebuffer past 8,192 a side");
        CHECK(cn_scene_begin(0, 100, 0, 1, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a board of no width");
        CHECK(cn_scene_begin(1, 100, 0, .1f, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a board and scale that round to no pixel");
        CHECK(cn_scene_begin(100, 100, 0, __builtin_nanf(""), 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a scale that is not a number");
        CHECK(cn_scene_begin(100, 100, 0, 1, 0, 0, 720, 0, 0, 0, 256, .5f, 1, 1) == 0, "no light");
        CHECK(cn_scene_begin(100, 100, 0, 1, 0, 0, 720, 0, 0, 1, 256, .5f, -1, 1) == 0, "a negative capacity");
        CHECK(cn_scene_begin(100, 100, 0, 1, 0, 0, 720, 0, 0, 1, CN_SCENE_SHADOW_MAX + 1, .5f, 1, 1) == 0, "a shadow map past 1,024: a place on it would not fit 16 bits");
        CHECK(cn_scene_begin(100, 100, 0, 1, 0, 0, 720, 0, 0, 1, CN_SCENE_SHADOW_MAX, .5f, 1, 1) > 0, "1,024 itself is drawable");
        CHECK(cn_scene_init(0, ios) == 0 && cn_scene_tex_new(4, 4, 0) == -1 && cn_scene_room() == 0, "no arena: no texture");
        CHECK(cn_scene_begin(100, 100, 0, 1, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "no arena: no frame");
    }
    free(mem);
    return report("cn_scene_test");
}

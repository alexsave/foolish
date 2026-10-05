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
        CHECK(h == 0x7c136d3580720f13ull, "the still table at 1x: %016llx", (unsigned long long)h);
        d = cnf_frame(&t, 390, 718, 40, 1.5f, 1024, .6f);
        h = cnf_hash();
        CHECK(h == 0xd79fc9ffeecb40ddull, "a throw's frame at 1.5x: %016llx", (unsigned long long)h);
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
        const uint32_t shaded = cn_scene_prof(0), walked = cn_scene_prof(1), texels = cn_scene_prof(2);
        const int counts[5] = { 2, 3, 7, 13, CN_SCENE_MAX_BANDS };
        for (int c = 0; c < 5; c++) {
            const int nb = counts[c];
            cnf_build(&t, 390, 718, 40, 1.5f, 1024, .6f);
            int d = cn_scene_prepare(cnf_nv, cnf_nf);
            /* the bands of a pass last to first: a band may not lean on a band before it */
            for (int pass = 0; pass < CN_SCENE_PASSES; pass++) for (int b = nb - 1; b >= 0; b--) cn_scene_band(pass, b, nb);
            CHECK(d > 6000 && cnf_hash() == h, "%d bands draw the one-thread picture (%016llx)", nb, (unsigned long long)cnf_hash());
            /* every row is walked once: bands that overlapped would walk the shared rows twice */
            CHECK(cn_scene_prof(0) == shaded && cn_scene_prof(1) == walked && cn_scene_prof(2) == texels,
                  "%d bands: %u fragments, %u walked, %u texels (one thread: %u, %u, %u)", nb, cn_scene_prof(0), cn_scene_prof(1), cn_scene_prof(2), shaded, walked, texels);
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

    TEST("a frame that does not fit fails cleanly");
    {
        cn_scene_init(mem, ios);
        CnfTex t;
        cnf_textures(&t, 1);
        size_t need3 = cn_scene_frame_bytes(390, 718, 40, 3, 1024, CNF_VCAP, CNF_FCAP);
        CHECK(need3 > cn_scene_room(), "3x with the textures is past 48 MB (%zu > %zu)", need3, cn_scene_room());
        CHECK(cnf_frame(&t, 390, 718, 40, 3, 1024, 0) == -1, "3x does not begin");
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
        CHECK(cn_scene_begin(0, 100, 0, 1, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a board of no width");
        CHECK(cn_scene_begin(100, 100, 0, __builtin_nanf(""), 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a scale that is not a number");
        CHECK(cn_scene_begin(100, 100, 0, 1000, 0, 0, 720, 0, 0, 1, 256, .5f, 1, 1) == 0, "a framebuffer past 8,192 a side");
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

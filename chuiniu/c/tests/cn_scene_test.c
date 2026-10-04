/* The scene renderer (wasm/cn_scene.c), on the host: a lit quad over the table
 * draws its texture, the table under it takes its shadow where the light says,
 * and nowhere else; textures can be added between frames. The renderer is browser-only in
 * use; this holds its arithmetic. */
#include "../wasm/cn_scene.c"
#include "cn_check.h"

int main(void)
{
    TEST("the scene: a quad, its shadow, the empty table");
    scene_reset();
    int tx = scene_tex_new(4, 4, 0);
    CHECK(tx == 0, "the first texture is 0");
    memset(scene_tex_rgba(tx), 255, 4 * 4 * 4);
    int ok = scene_begin(100, 100, 0, 1, 50, 50, 720, -.45f, -.55f, 1, 256, .55f, 64, 32);
    CHECK(ok > 0, "a frame begins");
    float *V = scene_verts(), *F = scene_faces();
    /* the table: a receiver over the whole board */
    const float table[] = { 0, 0, 0, 0, 0, 1,  100, 0, 0, 0, 0, 1,  100, 100, 0, 0, 0, 1,  0, 100, 0, 0, 0, 1 };
    memcpy(V, table, sizeof table);
    /* a square 40 high, 30..70 both ways, lit from above */
    const float quad[] = { 30, 30, 40, 0, 0, 1,  70, 30, 40, 0, 0, 1,  70, 70, 40, 0, 0, 1,  30, 70, 40, 0, 0, 1 };
    memcpy(V + 24, quad, sizeof quad);
    const float faces[] = {
        0, 1, 2, 0, 0, 1, 0, 1, 1, -1, 0, 0, 0, 1, 0, 12,   0, 2, 3, 0, 0, 1, 1, 0, 1, -1, 0, 0, 0, 1, 0, 12,
        4, 5, 6, 0, 0, 1, 0, 1, 1,  0, 0, 4, 3, 1, 0,  7,   4, 6, 7, 0, 0, 1, 1, 0, 1,  0, 0, 4, 3, 1, 0,  7,
    };
    memcpy(F, faces, sizeof faces);
    int drawn = scene_render(8, 4);
    CHECK(drawn == 4, "four faces drawn (%d)", drawn);
    const uint8_t *fb = scene_fb(); int W = scene_fb_w();
    CHECK(W == 100 && scene_fb_h() == 100, "the framebuffer is the board");
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
    /* a frame can begin again and a texture cannot be added mid-stream */
    CHECK(scene_begin(100, 100, 0, 1, 50, 50, 720, -.45f, -.55f, 1, 256, .55f, 64, 32) > 0, "a second frame begins");
    CHECK(scene_tex_new(4, 4, 0) == 1, "a texture can still be added between frames");
    return report("cn_scene_test");
}

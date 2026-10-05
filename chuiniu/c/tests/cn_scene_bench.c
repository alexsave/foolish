/* THE RENDERER'S SPEED: a six-seat frame (tests/cn_scene_frame.h: six full-size cups
 * with their fillets, crowns and insides, five dice a seat, the contact footprints,
 * textures of the study's sizes) on a 390 by 718 point board, natively, at 1x, 2x and
 * 3x device pixels a point, built at -O2 as the iOS library is. Each line is 100
 * frames: the mean and the 95th percentile of a whole frame (cn_scene_begin, the
 * vertices and faces written, cn_scene_render), still (the cups on the table) and
 * in a throw (every cup lifted and tilted). And the memory: the frame's bytes at each
 * scale against the iOS arena, with every seat's own textures and with one set shared.
 *
 *   ./build/cn_scene_bench                    the table above
 *   ./build/cn_scene_bench loop 2 20          2x frames for 20 seconds, for `sample`
 *   ./build/cn_scene_bench ppm 2 out.ppm      one 2x frame as a picture (on black)
 *
 * Not a test: it asserts nothing and is not in `make run` (make bench). */
#define _POSIX_C_SOURCE 199309L
#include "cn_scene_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BOARD_W 390
#define BOARD_H 718
#define BOARD_PAD 40
#define SHADOW_RES 1024

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void run(const CnfTex *t, float dpr, int frames, int roll, double *mean, double *p95)
{
    double ms[1000], sum = 0;
    if (frames > 1000) frames = 1000;
    for (int i = 0; i < frames; i++) {
        float shake = roll ? (float)((i * 7) % 10) / 10.f : 0;
        double t0 = now_ms();
        if (cnf_frame(t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, shake) < 0) { printf("dpr %.1f: the frame does not fit\n", dpr); exit(1); }
        ms[i] = now_ms() - t0; sum += ms[i];
    }
    qsort(ms, (size_t)frames, sizeof ms[0], cmp_d);
    *mean = sum / frames; *p95 = ms[(int)(frames * .95)];
}

int main(int argc, char **argv)
{
    size_t big = (size_t)256 << 20;
    void *mem = malloc(big);
    if (!mem || !cn_scene_init(mem, big)) { printf("no arena\n"); return 1; }
    CnfTex t;
    if (cnf_textures(&t, 0)) { printf("no room for the textures\n"); return 1; }
    size_t tex_own = big - cn_scene_room();
    if (argc > 3 && !strcmp(argv[1], "loop")) {
        float dpr = (float)atof(argv[2]); double secs = atof(argv[3]), t0 = now_ms(); int n = 0;
        while (now_ms() - t0 < secs * 1000) { cnf_frame(&t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, (float)(n % 10) / 10.f); n++; }
        printf("%d frames at %.1fx in %.1f s: %.2f ms a frame\n", n, dpr, secs, (now_ms() - t0) / n);
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "ppm")) {
        float dpr = (float)atof(argv[2]);
        cnf_frame(&t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, argc > 4 ? (float)atof(argv[4]) : 0);
        FILE *f = fopen(argv[3], "wb"); if (!f) return 1;
        int w = cn_scene_fb_w(), h = cn_scene_fb_h(); const uint8_t *p = cn_scene_fb();
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) {   /* over a mid-brown table, as the planks would show through */
            int a = p[i * 4 + 3];
            for (int c = 0; c < 3; c++) { int bg = c == 0 ? 120 : c == 1 ? 84 : 52; fputc((p[i * 4 + c] * a + bg * (255 - a)) / 255, f); }
        }
        fclose(f); printf("wrote %s (%dx%d)\n", argv[3], w, h);
        return 0;
    }
    /* the first frame makes the textures' half-size copies; the table is warm after it */
    cnf_frame(&t, BOARD_W, BOARD_H, BOARD_PAD, 1, SHADOW_RES, 0);
    size_t tex_all = big - cn_scene_room();
    printf("six seats, %d faces, %d vertices; board %dx%d + %d above, shadow map %d\n", cnf_nf, cnf_nv, BOARD_W, BOARD_H, BOARD_PAD, SHADOW_RES);
    printf("| scale | framebuffer | still mean | still p95 | throw mean | throw p95 | fragments shaded | frame bytes |\n|---|---|---|---|---|---|---|---|\n");
    const float scales[3] = { 1, 2, 3 };
    for (int k = 0; k < 3; k++) {
        double sm, sp, rm, rp;
        run(&t, scales[k], 100, 0, &sm, &sp);
        uint32_t frags = cn_scene_prof(0);
        run(&t, scales[k], 100, 1, &rm, &rp);
        printf("| %.0fx | %dx%d | %.2f ms | %.2f ms | %.2f ms | %.2f ms | %u | %.1f MB |\n", scales[k], cn_scene_fb_w(), cn_scene_fb_h(), sm, sp, rm, rp, frags,
               cn_scene_frame_bytes(BOARD_W, BOARD_H, BOARD_PAD, scales[k], SHADOW_RES, CNF_VCAP, CNF_FCAP) / 1048576.0);
    }
    /* THE MEMORY against the iOS arena: the textures (their copies included, once drawn) and a frame */
    cn_scene_init(mem, big);
    if (cnf_textures(&t, 1)) return 1;
    cnf_frame(&t, BOARD_W, BOARD_H, BOARD_PAD, 1, SHADOW_RES, 0);
    size_t tex_shared = big - cn_scene_room();
    printf("textures: every seat its own %.1f MB (%.1f MB uploaded), one set shared %.1f MB; the iOS arena %.0f MB\n",
           tex_all / 1048576.0, tex_own / 1048576.0, tex_shared / 1048576.0, CN_SCENE_ARENA_IOS / 1048576.0);
    for (float dpr = 1; dpr <= 3.01f; dpr += .25f) {
        size_t fbytes = cn_scene_frame_bytes(BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, CNF_VCAP, CNF_FCAP);
        printf("  %.2fx: frame %.1f MB; fits 48 MB with own textures %s, with shared %s\n", dpr, fbytes / 1048576.0,
               fbytes + tex_all <= CN_SCENE_ARENA_IOS ? "yes" : "no", fbytes + tex_shared <= CN_SCENE_ARENA_IOS ? "yes" : "no");
    }
    free(mem);
    return 0;
}

/* THE RENDERER'S SPEED: a six-seat frame (tests/cn_scene_frame.h: six full-size cups
 * with their fillets, crowns and insides, five dice a seat, the contact footprints,
 * textures of the study's sizes) on a 390 by 718 point board, natively, at 1x, 1.5x,
 * 2x and 3x device pixels a point, built at -O2 as the iOS library is. Each cell is
 * 100 frames: the mean and the 95th percentile of a whole frame (cn_scene_begin, the
 * vertices and faces written, the render), still (the cups on the table) and in a
 * throw (every cup lifted and tilted), on one thread (cn_scene_render) and in 2 and
 * 4 bands on as many threads (cn_scene_prepare, then each pass's bands through
 * GCD's dispatch_apply_f, which is what DispatchQueue.concurrentPerform is on iOS).
 * And the memory: the frame's bytes at each scale against the iOS arena, with every
 * seat's own textures and with one set shared.
 *
 *   ./build/cn_scene_bench                    the tables above
 *   ./build/cn_scene_bench loop 2 20 [bands] [skip]  2x frames for 20 seconds, for `sample` (skip: cn_scene_skip's mask)
 *   ./build/cn_scene_bench passes 2           each pass's share of a 2x frame, one thread
 *   ./build/cn_scene_bench cpu 2 200 [skip]   the thread's CPU time a 2x frame: least and median of 200
 *   ./build/cn_scene_bench split 2 200        the same, each part of the frame (prepare, each pass) apart
 *   ./build/cn_scene_bench ppm 2 out.ppm      one 2x frame as a picture (over brown)
 *
 * Not a test: it asserts nothing and is not in `make run` (make bench). */
#define _POSIX_C_SOURCE 199309L
#include "cn_scene_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#define BOARD_W 390
#define BOARD_H 718
#define BOARD_PAD 40
#define SHADOW_RES 1024

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double cpu_ms(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* the frame in nbands bands, each pass's bands at once; one band is cn_scene_render */
static int g_pass, g_bands;
#ifdef __APPLE__
static void band_fn(void *ctx, size_t i) { (void)ctx; cn_scene_band(g_pass, (int)i, g_bands); }
#endif
static void render_bands(int nbands)
{
    if (nbands <= 1) { cn_scene_render(cnf_nv, cnf_nf); return; }
    cn_scene_prepare(cnf_nv, cnf_nf);
    g_bands = nbands;
    for (g_pass = 0; g_pass < CN_SCENE_PASSES; g_pass++) {
#ifdef __APPLE__
        dispatch_apply_f((size_t)nbands, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), 0, band_fn);
#else
        for (int b = 0; b < nbands; b++) cn_scene_band(g_pass, b, nbands);   /* no GCD: the bands in turn */
#endif
    }
}

static void run(const CnfTex *t, float dpr, int frames, int roll, int nbands, double *mean, double *p95)
{
    double ms[1000], sum = 0;
    if (frames > 1000) frames = 1000;
    for (int i = 0; i < frames; i++) {
        float shake = roll ? (float)((i * 7) % 10) / 10.f : 0;
        double t0 = now_ms();
        if (cnf_build(t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, shake) < 0) { printf("dpr %.1f: the frame does not fit\n", dpr); exit(1); }
        render_bands(nbands);
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
    if (argc > 3 && !strcmp(argv[1], "cpu")) {
        /* the thread's CPU time a frame, one thread, still and thrown frames alternating: the least and the median
         * of n (the least is what a loaded machine leaves alone) */
        float dpr = (float)atof(argv[2]); int n = atoi(argv[3]); if (n > 1000) n = 1000;
        if (argc > 4) cn_scene_skip(atoi(argv[4]));
        static double ms[1000];
        for (int i = 0; i < n; i++) {
            double t0 = cpu_ms();
            cnf_build(&t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, (float)(i % 2) * .5f); render_bands(1);
            ms[i] = cpu_ms() - t0;
        }
        qsort(ms, (size_t)n, sizeof ms[0], cmp_d);
        printf("%.1fx, %d frames, thread CPU: least %.2f ms, median %.2f ms\n", dpr, n, ms[0], ms[n / 2]);
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "split")) {
        /* the thread's CPU time of each part of a frame, the frame drawn as cn_scene_render draws it (the shadow map
         * as one band, every other pass in CN_SCENE_MAX_BANDS bands in turn): the build, cn_scene_prepare, then each pass; the least of n each */
        float dpr = (float)atof(argv[2]); int n = atoi(argv[3]); if (n > 1000) n = 1000;
        enum { NP = 2 + CN_SCENE_PASSES };
        static double ms[NP][1000];
        for (int i = 0; i < n; i++) {
            double t0 = cpu_ms();
            cnf_build(&t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, (float)(i % 2) * .5f);
            double t1 = cpu_ms(); cn_scene_prepare(cnf_nv, cnf_nf); double t2 = cpu_ms();
            ms[0][i] = t1 - t0; ms[1][i] = t2 - t1;
            for (int p = 0; p < CN_SCENE_PASSES; p++) {
                double a = cpu_ms();
                if (p == CN_SCENE_PASS_SHADOW) cn_scene_band(p, 0, 1);
                else for (int b = 0; b < CN_SCENE_MAX_BANDS; b++) cn_scene_band(p, b, CN_SCENE_MAX_BANDS);
                ms[2 + p][i] = cpu_ms() - a;
            }
        }
        static const char *name[NP] = { "build", "prepare", "shadow map", "picture and shade", "edges", "commit" };
        printf("%.1fx, %d frames, thread CPU, least:", dpr, n);
        for (int p = 0; p < NP; p++) { qsort(ms[p], (size_t)n, sizeof ms[p][0], cmp_d); printf(" %s %.2f ms%s", name[p], ms[p][0], p + 1 < NP ? ";" : "\n"); }
        printf("  edge pixels %u, edge samples shaded where they lie %u\n", cn_scene_prof(4), cn_scene_prof(5));
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "loop")) {
        float dpr = (float)atof(argv[2]); double secs = atof(argv[3]), t0 = now_ms(); int n = 0, nb = argc > 4 ? atoi(argv[4]) : 1;
        if (argc > 5) cn_scene_skip(atoi(argv[5]));
        while (now_ms() - t0 < secs * 1000) { cnf_build(&t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, (float)(n % 10) / 10.f); render_bands(nb); n++; }
        printf("%d frames at %.1fx in %d bands in %.1f s: %.2f ms a frame\n", n, dpr, nb, secs, (now_ms() - t0) / n);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "passes")) {
        /* each pass's share, by leaving passes out (cn_scene_skip): 1 the shadow map, 2 the picture, 4 the shade,
         * 16 the edges (the picture's share is with the edges left out: without a picture there are no edges) */
        float dpr = (float)atof(argv[2]); double m[32], p;
        cnf_frame(&t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, 0);
        const int masks[6] = { 0, 1, 16, 18, 20, 23 };
        for (int k = 0; k < 6; k++) { cn_scene_skip(masks[k]); run(&t, dpr, 60, 0, 1, &m[masks[k]], &p); }
        cn_scene_skip(0); cnf_frame(&t, BOARD_W, BOARD_H, BOARD_PAD, dpr, SHADOW_RES, 0);
        printf("%.1fx: whole %.2f ms; the clears and the build %.2f; the shadow map %.2f; the picture %.2f; the shade %.2f; the edges %.2f\n", dpr,
               m[0], m[23], m[0] - m[1], m[16] - m[18], m[16] - m[20], m[0] - m[16]);
        printf("  fragments shaded %u, span pixels walked %u, map texels %u, edge pixels %u, edge samples shaded %u\n", cn_scene_prof(0), cn_scene_prof(1), cn_scene_prof(2),
               cn_scene_prof(4), cn_scene_prof(5));
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
    printf("| scale | framebuffer | bands | still mean | still p95 | throw mean | throw p95 | fragments shaded | frame bytes |\n|---|---|---|---|---|---|---|---|---|\n");
    const float scales[4] = { 1, 1.5f, 2, 3 };
    const int bands[3] = { 1, 2, 4 };
    for (int k = 0; k < 4; k++) for (int b = 0; b < 3; b++) {
        double sm, sp, rm, rp;
        run(&t, scales[k], 100, 0, bands[b], &sm, &sp);
        uint32_t frags = cn_scene_prof(0);
        run(&t, scales[k], 100, 1, bands[b], &rm, &rp);
        printf("| %.1fx | %dx%d | %d | %.2f ms | %.2f ms | %.2f ms | %.2f ms | %u | %.1f MB |\n", scales[k], cn_scene_fb_w(), cn_scene_fb_h(), bands[b], sm, sp, rm, rp, frags,
               cn_scene_frame_bytes(BOARD_W, BOARD_H, BOARD_PAD, scales[k], SHADOW_RES, CNF_VCAP, CNF_FCAP) / 1048576.0);
    }
    /* the bands draw the same bits as one thread */
    {
        cnf_build(&t, BOARD_W, BOARD_H, BOARD_PAD, 2, SHADOW_RES, .5f); render_bands(1); uint64_t h1 = cnf_hash();
        cnf_build(&t, BOARD_W, BOARD_H, BOARD_PAD, 2, SHADOW_RES, .5f); render_bands(4); uint64_t h4 = cnf_hash();
        cnf_build(&t, BOARD_W, BOARD_H, BOARD_PAD, 2, SHADOW_RES, .5f); render_bands(16); uint64_t h16 = cnf_hash();
        printf("2x throw frame: one thread %016llx, 4 bands %016llx, 16 bands %016llx: %s\n", (unsigned long long)h1, (unsigned long long)h4,
               (unsigned long long)h16, h1 == h4 && h1 == h16 ? "the same bits" : "DIFFERENT");
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

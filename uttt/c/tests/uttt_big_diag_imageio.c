/* The 243 diagnostics against JPEGs ImageIO writes NOW (macOS: ImageIO).
 *
 *     make -C uttt/c big-diag-imageio
 *
 * Not in `make run` (CI is Linux). Three things only a Mac can check:
 *   1. src/uttt_jpeg_imageio.inc is what ImageIO writes today, every step.
 *   2. The parser reads ImageIO's own files right on several pictures (a
 *      ramp, a flat white 729 x 732, noise, a painted 243 board) at qualities
 *      on and between the steps: size, components, 4:2:0 (4:4:4 at 1.00,
 *      the one ImageIO writes), baseline or progressive, grey, and the
 *      quality - exact with the step in its range on a step, within 0.02
 *      between steps.
 *   3. THE CHAIN a real send makes (shared/tools/layout_probe/README.md): a
 *      painted board at 3 px a cell, JPEG'd at 0.50 (the sender's extension)
 *      and again at 0.89 (the transport's transcoder), decoded: the second
 *      file reads as 0.89-0.91 exact, and the greys sampled from it agree with
 *      the kit's bd_sample. Prints the greys per class and the worst deviation
 *      from nominal, the number a 4-level picture would have to fit in.
 */
#include "../src/uttt_big_diag.h"
#include "../src/uttt_big_msg.h"
#include "../../../shared/c/test/check.h"
#include "../../../shared/swift/BubbleDataKit/Sources/CBubbleData/include/bubble_data.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

static CGColorSpaceRef g_srgb;

static uint32_t rs = 777;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static CGImageRef image_of(uint8_t *rgba, int w, int h)
{
    CGContextRef ctx = CGBitmapContextCreate(rgba, (size_t)w, (size_t)h, 8, (size_t)w * 4, g_srgb,
                                             (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return img;
}

static CGImageRef grey_of(int w, int h)
{
    uint8_t *px = malloc((size_t)w * h);
    for (int i = 0; i < w * h; i++) px[i] = (uint8_t)(i * 7);
    CGColorSpaceRef g = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w, g, (CGBitmapInfo)kCGImageAlphaNone);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(g);
    free(px);
    return img;
}

/* ImageIO's JPEG of `img` at `q`, optionally progressive. */
static CFDataRef jpeg_of(CGImageRef img, double q, int progressive)
{
    CFMutableDataRef d = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(d, CFSTR("public.jpeg"), 1, NULL);
    CFNumberRef qn = CFNumberCreate(NULL, kCFNumberDoubleType, &q);
    CFMutableDictionaryRef opts = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(opts, kCGImageDestinationLossyCompressionQuality, qn);
    if (progressive) {
        const void *k[] = { kCGImagePropertyJFIFIsProgressive };
        const void *v[] = { kCFBooleanTrue };
        CFDictionaryRef jfif = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(opts, kCGImagePropertyJFIFDictionary, jfif);
        CFRelease(jfif);
    }
    CGImageDestinationAddImage(dst, img, opts);
    CGImageDestinationFinalize(dst);
    CFRelease(opts);
    CFRelease(qn);
    CFRelease(dst);
    return d;
}

/* A JPEG decoded back to sRGB RGBA at its own size, as BubbleData.swift
 * draws a picture before it samples it. */
static uint8_t *decoded(CFDataRef jpg, int *w, int *h)
{
    CGImageSourceRef src = CGImageSourceCreateWithData(jpg, NULL);
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    *w = (int)CGImageGetWidth(img);
    *h = (int)CGImageGetHeight(img);
    uint8_t *px = calloc((size_t)*w * *h * 4, 1);
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)*w, (size_t)*h, 8, (size_t)*w * 4, g_srgb,
                                             (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, CGRectMake(0, 0, *w, *h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    CFRelease(src);
    return px;
}

static void test_tables_are_imageio(void)
{
    TEST("imageio: the committed tables are what ImageIO writes today");
    uint8_t px[32 * 32 * 4];
    for (int i = 0; i < 32 * 32; i++) { px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = (uint8_t)(i * 37); px[4 * i + 3] = 255; }
    CGImageRef img = image_of(px, 32, 32);
    for (int pct = 0; pct <= 100; pct++) {
        CFDataRef d = jpeg_of(img, pct / 100.0, 0);
        UbdJpeg j;
        CHECK(ubd_jpeg_parse(CFDataGetBytePtr(d), CFDataGetLength(d), &j) == UBD_JPEG_OK, "parse %d", pct);
        int same = 1;
        for (int k = 0; k < 64; k++) same &= j.luma[k] == ubd_imageio_luma(pct)[k];
        CHECK(same, "step %d differs: run make jpeg-imageio", pct);
        CFRelease(d);
    }
    CGImageRelease(img);
}

static void test_files(void)
{
    static uint8_t board[729 * 732 * 4], white[729 * 732 * 4], noise[300 * 200 * 4], ramp[64 * 64 * 4];
    static uint8_t sym[UTB_CELLS], cells[243 * 244];
    for (int i = 0; i < UTB_CELLS; i++) sym[i] = rnd() % 100 < 30 ? (uint8_t)(1 + (rnd() & 1)) : 0;
    bd_frame(sym, UTB_CELLS, BD_KIND_SYMBOLS, 0, 243, cells);
    bd_paint(cells, 243, 3, board);
    memset(white, 255, sizeof white);
    for (size_t i = 0; i < sizeof noise; i++) noise[i] = (uint8_t)rnd();
    for (int i = 0; i < 64 * 64; i++) { ramp[4 * i] = (uint8_t)i; ramp[4 * i + 1] = (uint8_t)(i >> 4); ramp[4 * i + 2] = 200; ramp[4 * i + 3] = 255; }
    struct { uint8_t *px; int w, h; } pics[] = { { board, 729, 732 }, { white, 729, 732 }, { noise, 300, 200 }, { ramp, 64, 64 } };
    static const double on_steps[] = { 0.00, 0.10, 0.25, 0.50, 0.75, 0.89, 0.90, 0.91, 0.95, 0.99, 1.00 };
    static const double between[] = { 0.555, 0.635, 0.855, 0.915 };
    for (size_t p = 0; p < sizeof pics / sizeof pics[0]; p++) {
        CGImageRef img = image_of(pics[p].px, pics[p].w, pics[p].h);
        for (size_t k = 0; k < sizeof on_steps / sizeof on_steps[0]; k++)
            for (int prog = 0; prog < 2; prog++) {
                double q = on_steps[k];
                CFDataRef d = jpeg_of(img, q, prog);
                UbdJpeg j;
                TEST("imageio: size, components, chroma and frame of ImageIO's own files");
                CHECK(ubd_jpeg_parse(CFDataGetBytePtr(d), CFDataGetLength(d), &j) == UBD_JPEG_OK, "parse");
                CHECK(j.width == pics[p].w && j.height == pics[p].h && j.components == 3, "pic %zu q %.2f", p, q);
                CHECK(!strcmp(ubd_jpeg_chroma(&j), q >= 1.0 ? "4:4:4" : "4:2:0"), "pic %zu q %.2f chroma %s", p, q, ubd_jpeg_chroma(&j));
                CHECK(j.sof == (prog ? UBD_SOF_PROGRESSIVE : UBD_SOF_BASELINE), "pic %zu q %.2f sof %x", p, q, j.sof);
                TEST("imageio: a step's quality is exact and in its range");
                int t = (int)(q * 1000 + 0.5);
                CHECK(j.q_exact && j.q_lo <= t && j.q_hi >= t, "pic %zu q %.2f: %d..%d", p, q, j.q_lo, j.q_hi);
                CFRelease(d);
            }
        for (size_t k = 0; k < sizeof between / sizeof between[0]; k++) {
            double q = between[k];
            CFDataRef d = jpeg_of(img, q, 0);
            UbdJpeg j;
            TEST("imageio: a quality between steps estimates within 0.02");
            CHECK(ubd_jpeg_parse(CFDataGetBytePtr(d), CFDataGetLength(d), &j) == UBD_JPEG_OK, "parse");
            int t = (int)(q * 1000 + 0.5);
            CHECK(!j.q_exact && j.q_est >= t - 20 && j.q_est <= t + 20, "pic %zu q %.3f: est %d", p, q, j.q_est);
            CFRelease(d);
        }
        CGImageRelease(img);
    }
    TEST("imageio: a grey picture is one component");
    CGImageRef g = grey_of(96, 99);
    CFDataRef d = jpeg_of(g, 0.5, 0);
    UbdJpeg j;
    CHECK(ubd_jpeg_parse(CFDataGetBytePtr(d), CFDataGetLength(d), &j) == UBD_JPEG_OK && j.components == 1 &&
          !strcmp(ubd_jpeg_chroma(&j), "grey") && j.q_exact && j.q_lo == 500, "grey");
    CFRelease(d);
    CGImageRelease(g);
}

/* The two JPEGs of a real send, on a mid-game board and on a sparse one. */
static void test_chain(void)
{
    static uint8_t paint[729 * 732 * 4], sym[UTB_CELLS], cells[243 * 244], grid[243 * 244];
    static const int density[] = { 3, 300 };
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < UTB_CELLS; i++) sym[i] = (int)(rnd() % 1000) < density[s] ? (uint8_t)(1 + (rnd() & 1)) : 0;
        bd_frame(sym, UTB_CELLS, BD_KIND_SYMBOLS, 0, 243, cells);
        bd_paint(cells, 243, 3, paint);
        CGImageRef img = image_of(paint, 729, 732);
        CFDataRef first = jpeg_of(img, 0.50, 0);
        int w, h;
        uint8_t *px1 = decoded(first, &w, &h);
        CGImageRef img1 = image_of(px1, w, h);
        CFDataRef second = jpeg_of(img1, 0.89, 0);
        uint8_t *px2 = decoded(second, &w, &h);
        UbdJpeg j1, j2;
        TEST("imageio: the chain's files read as 0.50 then 0.89-0.91, 4:2:0");
        CHECK(ubd_jpeg_parse(CFDataGetBytePtr(first), CFDataGetLength(first), &j1) == UBD_JPEG_OK &&
              j1.q_exact && j1.q_lo == 500 && !strcmp(ubd_jpeg_chroma(&j1), "4:2:0"), "first");
        CHECK(ubd_jpeg_parse(CFDataGetBytePtr(second), CFDataGetLength(second), &j2) == UBD_JPEG_OK &&
              j2.q_exact && j2.q_lo == 890 && j2.q_hi == 910 && j2.width == 729 && j2.height == 732, "second");
        TEST("imageio: the chain's greys are the kit's reading");
        BdReading kit;
        UbdLuma l;
        CHECK(bd_sample(px2, w, h, 243, grid, &kit) == BD_EOK && ubd_luma(px2, w, h, 243, &l) == 0, "sample");
        CHECK(l.risky == kit.risky && l.min_margin == kit.min_margin, "risky %d/%d margin %d/%d",
              l.risky, kit.risky, l.min_margin, kit.min_margin);
        uint8_t back[UTB_CELLS];
        CHECK(bd_unframe(grid, 243, back, UTB_CELLS, NULL, NULL) == UTB_CELLS && !memcmp(back, sym, UTB_CELLS),
              "the board reads back");
        printf("chain %s (%ld then %ld bytes): risky %d, min margin %d, worst deviation %d.%d\n",
               s ? "mid-game" : "sparse", (long)CFDataGetLength(first), (long)CFDataGetLength(second),
               l.risky, l.min_margin, l.worst_dev3 / 3, (l.worst_dev3 % 3) * 10 / 3);
        static const char *names[3] = { "empty", "X", "O" };
        for (int c = 0; c < 3; c++)
            printf("  %-5s n %6d mean %6.2f sd %5.2f min %3d max %3d\n", names[c], l.cls[c].n,
                   l.cls[c].mean, l.cls[c].sd, l.cls[c].min, l.cls[c].max);
        free(px1);
        free(px2);
        CGImageRelease(img1);
        CFRelease(first);
        CFRelease(second);
        CGImageRelease(img);
    }
}

int main(void)
{
    g_srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    test_tables_are_imageio();
    test_files();
    test_chain();
    return report("uttt_big_diag_imageio");
}

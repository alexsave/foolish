/* sweep.c - how hard a recompression the probe's pattern survives.
 *
 * Messages re-encodes a bubble's picture as a JPEG before the reading
 * extension sees it (README.md). On a simulator that is the only thing that
 * happens to it; a real transport may compress harder or scale it down. This
 * tool does both to the pattern on a Mac, with the same encoder (ImageIO), and
 * counts the cells that decode wrong - the margin a design would be buying.
 *
 *   sweep                      the two tables, for a 243 x 243 grid
 *   sweep --cells N            the same for an N x N grid
 *   sweep --match FILE.jpeg    which ImageIO quality wrote FILE (by its
 *                              quantisation table), and its chroma sampling
 *
 * macOS only: ImageIO and CoreGraphics are the codec. The pattern and the
 * judge are layout_probe.h, the same file the probe app compiles.
 */
#include "layout_probe.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CGColorSpaceRef g_space;

static CGImageRef image_of(uint8_t *rgba, int side)
{
    CGContextRef ctx = CGBitmapContextCreate(rgba, side, side, 8, side * 4, g_space, kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return img;
}

/* JPEG bytes of `img` at ImageIO quality q (0..1). */
static CFDataRef jpeg_of(CGImageRef img, double q)
{
    CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, NULL);
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberDoubleType, &q);
    const void *keys[] = { kCGImageDestinationLossyCompressionQuality }, *vals[] = { num };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CGImageDestinationAddImage(dst, img, opts);
    CGImageDestinationFinalize(dst);
    CFRelease(opts); CFRelease(num); CFRelease(dst);
    return data;
}

/* Draw `img` into a fresh w x h RGBA buffer (the caller frees it). */
static uint8_t *pixels_of(CGImageRef img, int w, int h)
{
    uint8_t *buf = calloc((size_t)w * h, 4);
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, g_space, kCGImageAlphaNoneSkipLast);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx);
    return buf;
}

/* Paint n cells at p px, optionally scale to `scaled` px a side, JPEG it at q,
 * decode it, and count. */
static LpVerdict round_trip(int n, int p, int grey, int scaled, double q, long *bytes)
{
    const int side = n * p;
    uint8_t *rgba = malloc((size_t)side * side * 4);
    lp_fill(rgba, n, p, grey);
    CGImageRef img = image_of(rgba, side);
    int out = side;
    uint8_t *shrunk = NULL;
    if (scaled > 0 && scaled < side) {
        shrunk = pixels_of(img, scaled, scaled);
        CGImageRelease(img);
        img = image_of(shrunk, scaled);
        out = scaled;
    }
    CFDataRef jpg = jpeg_of(img, q);
    if (bytes) *bytes = (long)CFDataGetLength(jpg);
    CGImageSourceRef src = CGImageSourceCreateWithData(jpg, NULL);
    CGImageRef back = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    uint8_t *got = pixels_of(back, out, out);
    LpVerdict v = lp_judge(got, out, out, n, grey);
    free(got); CGImageRelease(back); CFRelease(src); CFRelease(jpg); CGImageRelease(img);
    free(shrunk); free(rgba);
    return v;
}

/* The luminance quantisation table (id 0, 8-bit) of a JPEG, in file order,
 * and the luma component's sampling factors. Returns 1 when both were found. */
static int jpeg_tables(const uint8_t *b, long n, uint8_t dqt[64], int *hs, int *vs)
{
    int have_q = 0, have_s = 0;
    long i = 2;
    while (i + 4 <= n && b[i] == 0xFF) {
        const int marker = b[i + 1];
        const long len = (b[i + 2] << 8) | b[i + 3];
        if (marker == 0xDA) break;                           /* the scan: no more tables */
        if (marker == 0xDB) {
            long k = i + 4;
            while (k < i + 2 + len) {
                const int prec = b[k] >> 4, id = b[k] & 15;
                if (prec == 0 && id == 0) { memcpy(dqt, b + k + 1, 64); have_q = 1; }
                k += 1 + (prec ? 128 : 64);
            }
        }
        if (marker == 0xC0 || marker == 0xC2) {              /* frame header: the first component is luma */
            *hs = b[i + 11] >> 4; *vs = b[i + 11] & 15; have_s = 1;
        }
        i += 2 + len;
    }
    return have_q && have_s;
}

static int match(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "sweep: cannot read %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "sweep: short read\n"); return 1; }
    fclose(f);
    uint8_t want[64]; int hs = 0, vs = 0;
    if (!jpeg_tables(b, n, want, &hs, &vs)) { fprintf(stderr, "sweep: %s has no baseline tables\n", path); return 1; }
    printf("%s: %ld bytes, luma sampled %dx%d (%s)\n", path, n, hs, vs,
           hs == 2 && vs == 2 ? "4:2:0, chroma at half size both ways" : hs == 1 && vs == 1 ? "4:4:4" : "other");

    uint8_t rgba[64 * 64 * 4];
    lp_fill(rgba, 16, 4, LP_COLOUR);
    CGImageRef img = image_of(rgba, 64);
    int best = -1; long best_d = -1;
    for (int pct = 0; pct <= 100; pct++) {
        CFDataRef jpg = jpeg_of(img, pct / 100.0);
        uint8_t got[64]; int h2, v2;
        if (jpeg_tables(CFDataGetBytePtr(jpg), (long)CFDataGetLength(jpg), got, &h2, &v2)) {
            long d = 0;
            for (int k = 0; k < 64; k++) d += labs((long)got[k] - want[k]);
            if (best_d < 0 || d < best_d) { best_d = d; best = pct; }
        }
        CFRelease(jpg);
    }
    CGImageRelease(img);
    printf("closest ImageIO quality: %.2f (table distance %ld%s)\n", best / 100.0, best_d,
           best_d == 0 ? ", an exact match" : "");
    free(b);
    return 0;
}

int main(int argc, char **argv)
{
    g_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    int n = 243;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--match") && i + 1 < argc) return match(argv[i + 1]);
        else if (!strcmp(argv[i], "--cells") && i + 1 < argc) n = atoi(argv[++i]);
        else { fprintf(stderr, "usage: sweep [--cells N] | --match FILE.jpeg\n"); return 2; }
    }
    static const double qs[] = { 0.90, 0.80, 0.70, 0.60, 0.50, 0.40, 0.30, 0.20, 0.10 };
    const int nq = (int)(sizeof qs / sizeof *qs);

    printf("wrong cells of %d after one JPEG at ImageIO quality q, by palette and pixels per cell\n", n * n);
    printf("%-10s", "");
    for (int k = 0; k < nq; k++) printf("q%.2f   ", qs[k]);
    printf("\n");
    for (int grey = 1; grey >= 0; grey--)
        for (int p = 1; p <= 4; p++) {
            printf("%-6s %dpx ", grey ? "grey" : "colour", p);
            for (int k = 0; k < nq; k++) printf("%-8d", round_trip(n, p, grey, 0, qs[k], NULL).wrong);
            printf("\n");
        }

    static const int sides[] = { 1458, 1200, 972, 729, 600, 486, 400, 300, 243 };
    const int ns = (int)(sizeof sides / sizeof *sides);
    printf("\nwrong cells of %d after a scale to S px a side, then one JPEG at q0.75 (grey)\n", n * n);
    printf("%-10s", "");
    for (int k = 0; k < ns; k++) printf("S%-7d", sides[k]);
    printf("\n");
    for (int p = 2; p <= 6; p += (p == 4 ? 2 : 1)) {
        printf("%-6s %dpx ", "grey", p);
        for (int k = 0; k < ns; k++) {
            if (sides[k] >= n * p) printf("%-8s", "-");
            else printf("%-8d", round_trip(n, p, LP_GREY, sides[k], 0.75, NULL).wrong);
        }
        printf("\n");
    }
    return 0;
}

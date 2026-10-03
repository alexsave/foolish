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
 *   sweep --judge FILE [--cells N] [--colour]
 *                              decode ANY image file (jpeg, png, heic) and judge
 *                              it as an N x N probe pattern, as received: its
 *                              pixel size, bytes, wrong cells, worst channel error
 *   sweep --samples DIR        pictures of what comes back: for each case, a
 *                              corner of the decoded picture magnified, beside
 *                              the same corner as it DECODES, wrong cells green
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

static void png_to(const char *path, uint8_t *rgba, int w, int h)
{
    CGContextRef ctx = CGBitmapContextCreate(rgba, w, h, 8, w * 4, g_space, kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    CGImageDestinationFinalize(dst);
    CFRelease(dst); CFRelease(url); CFRelease(s); CGImageRelease(img); CGContextRelease(ctx);
}

/* One sample: the top-left SHOW x SHOW cells, each ZOOM px. Left, the picture
 * as it came back (nearest pixel, so what is drawn is what was decoded from).
 * Right, each cell as it READS: its palette colour, or green (in neither
 * palette) where the reading is not what was painted. q < 0 is the pattern as painted, no encoder. */
#define SHOW 32
#define ZOOM 10
#define GAP  12
static void sample(const char *dir, int idx, const char *label, int n, int p, int grey, int scaled, double q)
{
    const int side = n * p;
    uint8_t *rgba = malloc((size_t)side * side * 4);
    lp_fill(rgba, n, p, grey);
    uint8_t *got = rgba; int out = side;
    CGImageRef img = image_of(rgba, side);
    if (scaled > 0 && scaled < side) {
        uint8_t *shrunk = pixels_of(img, scaled, scaled);
        CGImageRelease(img);
        img = image_of(shrunk, scaled);
        got = shrunk; out = scaled;
    }
    if (q >= 0) {
        CFDataRef jpg = jpeg_of(img, q);
        CGImageSourceRef src = CGImageSourceCreateWithData(jpg, NULL);
        CGImageRef back = CGImageSourceCreateImageAtIndex(src, 0, NULL);
        uint8_t *dec = pixels_of(back, out, out);
        if (got != rgba) free(got);
        got = dec;
        CGImageRelease(back); CFRelease(src); CFRelease(jpg);
    }
    CGImageRelease(img);
    const LpVerdict v = lp_judge(got, out, out, n, grey);

    const int panel = SHOW * ZOOM, W = panel * 2 + GAP, H = panel;
    uint8_t *sheet = malloc((size_t)W * H * 4);
    memset(sheet, 40, (size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < panel; x++) {
            const uint8_t *s = got + (((int64_t)y * out / (n * ZOOM)) * out + (int64_t)x * out / (n * ZOOM)) * 4;
            memcpy(sheet + (y * W + x) * 4, s, 4);
            const int cx = x / ZOOM, cy = y / ZOOM;
            const int read = lp_read(got, out, out, n, grey, cx, cy, NULL);
            static const uint8_t bad[3] = { 40, 230, 70 };
            const uint8_t *c = read == lp_state((uint32_t)(cy * n + cx)) ? lp_rgb(read, grey) : bad;
            uint8_t *o = sheet + (y * W + panel + GAP + x) * 4;
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255;
        }
    char path[1024];
    snprintf(path, sizeof path, "%s/%02d_%s_wrong%d.png", dir, idx, label, v.wrong);
    png_to(path, sheet, W, H);
    printf("%s  (%d of %d cells wrong)\n", path, v.wrong, v.cells);
    free(sheet);
    if (got != rgba) free(got);
    free(rgba);
}

static int samples(const char *dir, int n)
{
    int i = 0;
    sample(dir, i++, "grey_1px_as_painted", n, 1, LP_GREY, 0, -1);
    sample(dir, i++, "grey_1px_q050_what_Messages_does", n, 1, LP_GREY, 0, 0.50);
    sample(dir, i++, "grey_1px_q030", n, 1, LP_GREY, 0, 0.30);
    sample(dir, i++, "grey_1px_q010", n, 1, LP_GREY, 0, 0.10);
    sample(dir, i++, "grey_3px_q050_what_Messages_does", n, 3, LP_GREY, 0, 0.50);
    sample(dir, i++, "grey_3px_q010", n, 3, LP_GREY, 0, 0.10);
    sample(dir, i++, "grey_3px_scaled_to_2px_q050", n, 3, LP_GREY, n * 2, 0.50);
    sample(dir, i++, "grey_3px_scaled_to_1px_q050", n, 3, LP_GREY, n, 0.50);
    sample(dir, i++, "colour_1px_as_painted", n, 1, LP_COLOUR, 0, -1);
    sample(dir, i++, "colour_1px_q050_what_Messages_does", n, 1, LP_COLOUR, 0, 0.50);
    sample(dir, i++, "colour_3px_q050_what_Messages_does", n, 3, LP_COLOUR, 0, 0.50);
    sample(dir, i++, "colour_3px_q010", n, 3, LP_COLOUR, 0, 0.10);
    return 0;
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

static int judge_file(const char *path, int n, int grey)
{
    CFStringRef cs = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cs, kCFURLPOSIXPathStyle, false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url); CFRelease(cs);
    if (!src) { fprintf(stderr, "sweep: cannot open %s\n", path); return 1; }
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    if (!img) { fprintf(stderr, "sweep: %s is not an image\n", path); return 1; }
    const int w = (int)CGImageGetWidth(img), h = (int)CGImageGetHeight(img);
    uint8_t *got = pixels_of(img, w, h);
    LpVerdict v = lp_judge(got, w, h, n, grey);
    FILE *f = fopen(path, "rb"); long bytes = 0;
    if (f) { fseek(f, 0, SEEK_END); bytes = ftell(f); fclose(f); }
    printf("%s\n  %d x %d px, %ld bytes, read as %d x %d cells (%s)\n  wrong %d of %d, exact %d, worst channel error %d\n",
           path, w, h, bytes, n, n, grey ? "grey" : "colour", v.wrong, v.cells, v.exact, v.max_err);
    free(got); CGImageRelease(img); CFRelease(src);
    return v.wrong ? 3 : 0;
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
    const char *dir = NULL, *judge = NULL;
    int grey = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--match") && i + 1 < argc) return match(argv[i + 1]);
        else if (!strcmp(argv[i], "--judge") && i + 1 < argc) judge = argv[++i];
        else if (!strcmp(argv[i], "--colour")) grey = 0;
        else if (!strcmp(argv[i], "--cells") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) dir = argv[++i];
        else { fprintf(stderr, "usage: sweep [--cells N] [--samples DIR] | --match FILE.jpeg | --judge FILE [--colour]\n"); return 2; }
    }
    if (judge) return judge_file(judge, n, grey);
    if (dir) return samples(dir, n);
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

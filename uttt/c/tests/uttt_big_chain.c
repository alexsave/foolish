/* THE BIG GAME THROUGH A REAL SEND'S PICTURE CHAIN, in C, on a Mac.
 *
 *     make -C uttt/c big-chain                    (the whole game)
 *     ./build/uttt_big_chain --plies N [--every K]
 *
 * One random depth-5 game, and after every ply the bubble a phone would
 * stage: the URL (utb_msg_text_encode) and the picture - the cells framed
 * and painted by the kit's own C (bubble_data.c, 243 cells at 1 px: 243 x
 * 244 RGBA) - then what a send does to that picture, as the layout probe
 * measured it (shared/tools/layout_probe/README.md, "A real send"): the
 * extension's JPEG at quality 0.50, decoded, the transport's JPEG at 0.89,
 * decoded, both with ImageIO and both 4:2:0 (the SOF0 sampling byte 0x22,
 * as BubbleDataKitTests.swift asserts). The reader's half is bd_sample,
 * bd_unframe and utb_msg_text_decode, and the game it reads must be the game
 * that was played: cells, nodes, turn, over, last, ply count, region.
 *
 * Prints the plies, the seconds, the worst (smallest) margin any cell read
 * at, and how many cell reads were risky (within BD_RISKY_MARGIN of a
 * threshold) over the whole game.
 *
 * macOS only: ImageIO is the codec. NOT in `run`/`asan`/`all`, because CI is
 * Linux. */
#include "../src/uttt_big_msg.h"
#include "../../../shared/swift/BubbleDataKit/Sources/CBubbleData/include/bubble_data.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Pixels a cell: 1 is the kit's board243 (the default, the geometry the
 * design names); --px 3 is robust243. */
#define PX_MAX 4
static int PX = 1, W = UTB_SIDE, H = UTB_SIDE + 1;

static CGColorSpaceRef g_space;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

/* The kit's own image: sRGB, RGBX, no interpolation (BubbleData.image). */
static CGImageRef image_of(uint8_t *rgba)
{
    CGContextRef ctx = CGBitmapContextCreate(rgba, W, H, 8, W * 4, g_space, kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return img;
}

/* JPEG bytes of `img` at ImageIO quality q (sweep.c's jpeg_of). */
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

/* The first component's sampling factors from the start-of-frame segment:
 * 0x22 is luma at twice the chroma both ways, 4:2:0. -1 if there is none. */
static int luma_sampling(CFDataRef jpeg, int *components)
{
    const uint8_t *b = CFDataGetBytePtr(jpeg);
    long n = CFDataGetLength(jpeg), i = 2;
    while (i + 4 < n && b[i] == 0xFF) {
        int marker = b[i + 1], len = (b[i + 2] << 8) | b[i + 3];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            if (i + 11 >= n) return -1;
            if (components) *components = b[i + 9];
            return b[i + 11];
        }
        i += 2 + len;
    }
    return -1;
}

/* Decode JPEG bytes into a W x H RGBA buffer, drawn 1:1. 0 on failure. */
static int decode_into(CFDataRef jpeg, uint8_t *rgba)
{
    CGImageSourceRef src = CGImageSourceCreateWithData(jpeg, NULL);
    if (!src) return 0;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!img) return 0;
    int ok = (int)CGImageGetWidth(img) == W && (int)CGImageGetHeight(img) == H;
    if (ok) {
        CGContextRef ctx = CGBitmapContextCreate(rgba, W, H, 8, W * 4, g_space, kCGImageAlphaNoneSkipLast);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
        CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), img);
        CGContextRelease(ctx);
    }
    CGImageRelease(img);
    return ok;
}

static UtbMsg M, BACK;
static int32_t LEGAL[UTB_CELLS];
static uint8_t FRAMED[UTB_SIDE * (UTB_SIDE + 1)], READ[UTB_SIDE * (UTB_SIDE + 1)], SYM[UTB_CELLS];
static uint8_t RGBA[UTB_SIDE * PX_MAX * (UTB_SIDE + 1) * PX_MAX * 4];

static int same_game(const UtbMsg *a, const UtbMsg *b)
{
    const UtbGame *g = &a->game, *h = &b->game;
    return a->seed == b->seed && a->sealed == b->sealed
        && !memcmp(a->o, b->o, UTM_TAG_LEN) && !memcmp(a->x, b->x, UTM_TAG_LEN)
        && g->turn == h->turn && g->over == h->over && g->last == h->last
        && g->n_plies == h->n_plies && utb_region(g) == utb_region(h)
        && !memcmp(g->cell, h->cell, UTB_CELLS)
        && !memcmp(g->node, h->node, (size_t)utb_nodes(UTB_DEPTH));
}

int main(int argc, char **argv)
{
    long max_plies = -1;
    int every = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--plies") && i + 1 < argc) max_plies = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--px") && i + 1 < argc) PX = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--plies N] [--every K] [--px 1..4]\n", argv[0]); return 2; }
    }
    if (PX < 1 || PX > PX_MAX) PX = 1;
    W = bd_width(UTB_SIDE, PX);
    H = bd_height(UTB_SIDE, PX);
    if (every < 1) every = 1;
    g_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    const int32_t seed = 1790000077;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN];
    utm_tag(seed, (const uint8_t *)"creator", 7, a);
    utm_tag(seed, (const uint8_t *)"joiner", 6, b);
    utb_msg_open(&M, seed, 17, a);

    long checked = 0, wrong = 0, refused = 0, misread = 0, risky = 0, not420 = 0, bytes1 = 0, bytes2 = 0;
    int min_margin = 1 << 30;
    double t0 = now(), last_print = t0;
    for (;;) {
        if (M.game.n_plies % every == 0 || M.game.over) {
            char text[UTB_MAX_TEXT];
            int ok = utb_msg_text_encode(&M, text, sizeof text) > 0
                  && bd_frame(M.game.cell, UTB_CELLS, BD_KIND_SYMBOLS, 0, UTB_SIDE, FRAMED) == BD_EOK
                  && bd_paint(FRAMED, UTB_SIDE, PX, RGBA) == BD_EOK;
            if (ok) {
                CGImageRef img = image_of(RGBA);
                CFDataRef first = jpeg_of(img, 0.50);
                CGImageRelease(img);
                int comps = 0;
                if (luma_sampling(first, &comps) != 0x22 || comps != 3) not420++;
                bytes1 += CFDataGetLength(first);
                memset(RGBA, 0, (size_t)W * (size_t)H * 4);
                ok = decode_into(first, RGBA);
                CFRelease(first);
                if (ok) {
                    img = image_of(RGBA);
                    CFDataRef second = jpeg_of(img, 0.89);
                    CGImageRelease(img);
                    if (luma_sampling(second, NULL) != 0x22) not420++;
                    bytes2 += CFDataGetLength(second);
                    memset(RGBA, 0, (size_t)W * (size_t)H * 4);
                    ok = decode_into(second, RGBA);
                    CFRelease(second);
                }
            }
            BdReading rd = { 0, 0, 0 };
            ok = ok && bd_sample(RGBA, W, H, UTB_SIDE, READ, &rd) == BD_EOK
                    && bd_unframe(READ, UTB_SIDE, SYM, UTB_CELLS, NULL, NULL) == UTB_CELLS;
            char text2[UTB_MAX_TEXT];
            /* REFUSED (the kit's CRC or the link's board check said no) is a
             * bubble that cannot be read; MISREAD (it read, as another game)
             * would be a lie on the screen, and must never happen */
            int read = ok && utb_msg_text_encode(&M, text2, sizeof text2) > 0
                          && utb_msg_text_decode(text2, SYM, &BACK) == UTM_EOK;
            if (read && !same_game(&M, &BACK)) misread++;
            if (!read) refused++;
            ok = read && same_game(&M, &BACK);
            if (!ok) {
                wrong++;
                if (wrong <= 5) {
                    int bad = 0, first = -1;
                    for (int i = 0; i < W / PX * (H / PX); i++)
                        if (READ[i] != FRAMED[i]) { bad++; if (first < 0) first = i; }
                    printf("  WRONG at ply %d: %d cells misread (first %d, row %d: wrote %d read %d);"
                           " risky %d, min margin %d\n", M.game.n_plies, bad, first, first / UTB_SIDE,
                           first >= 0 ? FRAMED[first] : -1, first >= 0 ? READ[first] : -1, rd.risky, rd.min_margin);
                }
            }
            risky += rd.risky;
            if (rd.cells && rd.min_margin < min_margin) min_margin = rd.min_margin;
            checked++;
        }
        if (M.game.over || (max_plies >= 0 && M.game.n_plies >= max_plies)) break;
        int n = utb_legal(&M.game, LEGAL, UTB_CELLS);
        const uint8_t *who = M.game.n_plies % 2 == 0 ? b : a;
        if (n <= 0 || !utb_msg_play(&M, who, LEGAL[rnd() % (unsigned)n])) {
            printf("  a legal move would not play at ply %d\n", M.game.n_plies);
            wrong++;
            break;
        }
        double t = now();
        if (t - last_print > 30) {
            printf("  ... ply %d, %.0f s, %.2f ms a checked ply\n", M.game.n_plies, t - t0,
                   1e3 * (t - t0) / (double)(checked ? checked : 1));
            fflush(stdout);
            last_print = t;
        }
    }
    double secs = now() - t0;
    printf("big-chain: %d x %d px (%d px a cell), %d plies (over %d), %ld positions through q0.50 -> q0.89"
           " (every %d), %.1f s, %.2f ms each\n", W, H, PX, M.game.n_plies, M.game.over, checked, every, secs,
           1e3 * secs / (double)(checked ? checked : 1));
    printf("  worst min_margin %d (of 64), risky cell reads %ld, JPEG bytes mean %.0f then %.0f\n",
           min_margin, risky, (double)bytes1 / (double)(checked ? checked : 1),
           (double)bytes2 / (double)(checked ? checked : 1));
    printf("  wrong %ld (refused %ld, misread %ld), not 4:2:0 %ld\n", wrong, refused, misread, not420);
    CGColorSpaceRelease(g_space);
    int fail = wrong != 0 || not420 != 0 || checked == 0;
    printf(fail ? "big-chain: FAILED\n" : "big-chain: ok\n");
    return fail;
}

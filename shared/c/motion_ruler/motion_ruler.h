/* motion_ruler.h - the debug ruler's palette and geometry, ONCE.
 *
 * The ruler (shared/swift/MotionRuler.swift) paints a red bar on a moving
 * container's top edge, a green bar on its bottom, a banded strip, a
 * millisecond clock and a 12pt fully saturated square on every element that
 * moves; the finder (shared/tools/motion) reads them back off filmed frames.
 * Both read THIS file, so the colour drawn and the colour looked for can never
 * drift apart: Swift through the CMotionRuler module (module.modulemap beside
 * this file), C by including it.
 *
 * WHY SATURATED PRIMARIES AND HALF-CHANNEL INKS, AS LITERAL sRGB. They survive
 * h264 4:2:0 chroma subsampling, nothing on a paper or felt surface is that
 * colour, and literal values do not shift between light and dark. They are
 * told apart by HUE (30 degrees apart), with a saturation and value floor that
 * keeps paper, ink and anti-aliased edges out.
 *
 * Product-free, header-only, no strings. */
#ifndef MOTION_RULER_H
#define MOTION_RULER_H

#include <stdint.h>

/* Every ink, by hue. RED and GREEN are the edge bars' and never a square. */
enum {
    MR_INK_RED = 0,
    MR_INK_ORANGE,
    MR_INK_YELLOW,
    MR_INK_LIME,
    MR_INK_GREEN,
    MR_INK_CYAN,
    MR_INK_BLUE,
    MR_INK_VIOLET,
    MR_INK_MAGENTA,
    MR_INK_PINK,
    MR_INK_COUNT
};

/* An ink's sRGB channel in HALVES (0, 1 or 2 -> 0, 0.5, 1): exact in every
 * representation, and the half channels are what make eight square inks. */
static inline int32_t mr_ink_half(int32_t ink, int32_t channel) {
    static const uint8_t rgb[MR_INK_COUNT][3] = {
        {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}, {0, 2, 0},
        {0, 2, 2}, {0, 0, 2}, {1, 0, 2}, {2, 0, 2}, {2, 0, 1},
    };
    return (ink >= 0 && ink < MR_INK_COUNT && channel >= 0 && channel < 3) ? rgb[ink][channel] : 0;
}

/* An ink's channel as 0..1, for a painter. */
static inline double mr_ink_unit(int32_t ink, int32_t channel) {
    return mr_ink_half(ink, channel) / 2.0;
}

/* An ink's hue, degrees: 30 apart from RED 0 to PINK 330, with no ink at
 * 150 or 210 (spring green and azure). */
static inline int32_t mr_ink_hue(int32_t ink) {
    static const int16_t hue[MR_INK_COUNT] = {0, 30, 60, 90, 120, 180, 240, 270, 300, 330};
    return (ink >= 0 && ink < MR_INK_COUNT) ? hue[ink] : -1;
}

/* The square inks, in the order a finder reports them. */
enum { MR_SQUARE_INKS = 8 };
static inline int32_t mr_square_ink(int32_t i) {
    static const uint8_t k[MR_SQUARE_INKS] = {
        MR_INK_MAGENTA, MR_INK_CYAN, MR_INK_YELLOW, MR_INK_ORANGE,
        MR_INK_BLUE, MR_INK_VIOLET, MR_INK_LIME, MR_INK_PINK,
    };
    return (i >= 0 && i < MR_SQUARE_INKS) ? k[i] : -1;
}

/* GEOMETRY, points. */
#define MR_SIDE_PT       12.0   /* a square                                  */
#define MR_EDGE_PT        4.0   /* the red and green bars' thickness          */
#define MR_BAND_PT       10.0   /* one band of the strip under the red bar    */
#define MR_STRIP_PT      18.0   /* the banded strip's width                   */
#define MR_CLOCK_GAP_PT   6.0   /* between the strip and the clock            */
#define MR_CLOCK_CELL_PT 12.0   /* one clock bit                              */
#define MR_CLOCK_BITS    14     /* milliseconds modulo 2^14                   */

/* The band strip's colour for band i, counted down from the red bar: red
 * first, yellow every tenth, cyan and magenta between. */
static inline int32_t mr_band_ink(int32_t i) {
    if (i == 0) return MR_INK_RED;
    if (i % 10 == 0) return MR_INK_YELLOW;
    return i % 2 == 0 ? MR_INK_CYAN : MR_INK_MAGENTA;
}

/* CLASSIFICATION: a pixel is an ink when its hue is within MR_HUE_TOL of the
 * ink's and its saturation and value clear the floors. */
#define MR_HUE_TOL        13.0  /* degrees                                    */
#define MR_SAT_MIN        0.85
#define MR_VAL_MIN        0.80
/* A bar is the rows of its ink covering more than this share of the width. */
#define MR_BAR_COVER      0.60
/* A square is an ink blob of 0.35 to 2.4 squares' area whose box is 0.5 to
 * 1.8 sides: a square on a rider scales with it, and a board that scales down
 * through an auto-collapse starts ~1.4x its compact size. */
#define MR_AREA_MIN       0.35
#define MR_AREA_MAX       2.40
#define MR_BOX_MIN        0.50
#define MR_BOX_MAX        1.80
/* The strip is left out of the square search, from the left edge. */
#define MR_SQUARE_SKIP_PT 24.0
/* A clock cell reads 1 when its darkest pixel is above this luminance, 0 when
 * its brightest is below MR_CLOCK_DARK, and not at all between. */
#define MR_CLOCK_LIGHT    200
#define MR_CLOCK_DARK     60

#endif

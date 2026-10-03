/* layout_probe.h - the test pattern a message bubble's picture carries, and
 * the verdict on what came back.
 *
 * THE QUESTION THIS MEASURES. A Messages app message carries a URL, and the URL
 * is capped at 5,000 characters. It also carries a LAYOUT: a picture and seven
 * strings. If the extension that reads a message is handed that layout back,
 * the picture is a second channel, and far wider than the URL. README.md is
 * the evidence; this file is the pattern both ends agree on.
 *
 * THE PATTERN is an n x n grid of cells, each in one of three states, drawn at
 * p pixels per cell. A cell's state is a hash of its index and nothing else,
 * so the sender and the judge need no shared file: whoever knows n knows the
 * whole picture. Three states because that is what a board square holds
 * (empty, one mark, the other).
 *
 * TWO PALETTES, and the difference is the finding. GREY separates the states
 * by luminance alone. COLOUR separates two of them mostly by hue - which is
 * what a 4:2:0 JPEG throws away first, so COLOUR is here to show the failure.
 *
 * Header-only, no allocation, no libc beyond the fixed-width integers; a
 * module.modulemap beside it (CLayoutProbe) hands it to Swift.
 */
#ifndef LAYOUT_PROBE_H
#define LAYOUT_PROBE_H

#include <stdint.h>

#define LP_STATES 3
#define LP_GREY   1
#define LP_COLOUR 0

/* The state of cell i (row * n + column): 0, 1 or 2. splitmix64's finalizer
 * over the index, so neighbours are unrelated and every state is a third of
 * the grid. */
static inline int lp_state(uint32_t i)
{
    uint64_t x = (uint64_t)i * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return (int)(x % LP_STATES);
}

/* What a state is painted as: r, g, b. */
static inline const uint8_t *lp_rgb(int state, int grey)
{
    static const uint8_t g[LP_STATES][3] = { {255, 255, 255}, {128, 128, 128}, {0, 0, 0} };
    static const uint8_t c[LP_STATES][3] = { {245, 240, 230}, {26, 63, 176}, {120, 20, 20} };
    return grey ? g[state] : c[state];
}

/* Paint the pattern into `rgba`, which holds (n*p) * (n*p) * 4 bytes, rows
 * top to bottom, alpha 255. */
static inline void lp_fill(uint8_t *rgba, int n, int p, int grey)
{
    const int side = n * p;
    for (int cy = 0; cy < n; cy++)
        for (int cx = 0; cx < n; cx++) {
            const uint8_t *c = lp_rgb(lp_state((uint32_t)(cy * n + cx)), grey);
            for (int dy = 0; dy < p; dy++)
                for (int dx = 0; dx < p; dx++) {
                    uint8_t *o = rgba + ((cy * p + dy) * side + cx * p + dx) * 4;
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255;
                }
        }
}

typedef struct {
    int cells;     /* n * n                                                  */
    int wrong;     /* cells whose nearest palette entry is not their state   */
    int exact;     /* cells whose sampled pixel is the painted value exactly */
    int max_err;   /* the worst channel error at a sampled pixel, 0..255     */
} LpVerdict;

/* The state cell (cx, cy) of an n x n pattern reads as, in a w x h picture.
 * ONE SAMPLE PER CELL, at the cell's centre wherever the picture's own size
 * puts it - so a picture that came back scaled is read at the size it came
 * back at, and the answer is what a reader with no resampling of its own would
 * decode. Nearest palette entry by summed channel distance. `err`, when not
 * NULL, takes the worst channel distance from what the cell was painted as. */
static inline int lp_read(const uint8_t *rgba, int w, int h, int n, int grey, int cx, int cy, int *err)
{
    int px = (int)(((int64_t)(2 * cx + 1) * w) / (2 * n));
    int py = (int)(((int64_t)(2 * cy + 1) * h) / (2 * n));
    if (px > w - 1) px = w - 1;
    if (py > h - 1) py = h - 1;
    const uint8_t *s = rgba + ((int64_t)py * w + px) * 4;
    int best = 0, best_d = 1 << 30;
    for (int k = 0; k < LP_STATES; k++) {
        const uint8_t *c = lp_rgb(k, grey);
        int d = 0;
        for (int ch = 0; ch < 3; ch++) d += s[ch] > c[ch] ? s[ch] - c[ch] : c[ch] - s[ch];
        if (d < best_d) { best_d = d; best = k; }
    }
    if (err) {
        const uint8_t *c = lp_rgb(lp_state((uint32_t)(cy * n + cx)), grey);
        int e = 0;
        for (int ch = 0; ch < 3; ch++) {
            int d = s[ch] > c[ch] ? s[ch] - c[ch] : c[ch] - s[ch];
            if (d > e) e = d;
        }
        *err = e;
    }
    return best;
}

/* Read the whole pattern back and count. */
static inline LpVerdict lp_judge(const uint8_t *rgba, int w, int h, int n, int grey)
{
    LpVerdict v = { n * n, 0, 0, 0 };
    for (int cy = 0; cy < n; cy++)
        for (int cx = 0; cx < n; cx++) {
            int e = 0;
            if (lp_read(rgba, w, h, n, grey, cx, cy, &e) != lp_state((uint32_t)(cy * n + cx))) v.wrong++;
            if (e == 0) v.exact++;
            if (e > v.max_err) v.max_err = e;
        }
    return v;
}

#endif

// playout_rom - the Game Boy side: PLAYOUT_RUNS playouts timed in scanlines
// (a scanline is 108.7 us), left in RAM for tools/gbrun: the total moves (to
// match the clang build), and the sum and the most scanlines of a playout.
#include <gb/gb.h>
#include <stdint.h>
// Each phase of a move, summed over every move of every playout (a second
// pass, whose clock marks slow it by about a third): [0] the two
// menu counts, [1] the draw, [2] the unrank, [3] the apply.
uint32_t po_phase[4];
static uint32_t po_t;
static uint8_t po_prof;          // the second pass only: marks cost time
static uint32_t clock_lines(void);
#define PO_MARK(i) do { if (!po_prof) break; { const uint32_t t_ = clock_lines(); if ((i) > 0) po_phase[(i) - 1] += t_ - po_t; po_t = t_; } } while (0)
#include "playout.h"
#include "playout.c"
#include "../src/gd_rng.h"

uint16_t po_moves_total;
uint32_t po_lines_sum;
uint16_t po_lines_max_hi;        // the slowest playout, in units of 256 scanlines (it can pass 65535)
uint32_t po_lines_max;
uint16_t po_moves_max;
uint8_t po_done;

static uint32_t clock_lines(void) {
    uint16_t t;
    uint8_t ly;
    __asm__("di");
    t = sys_time;
    ly = LY_REG;
    if (ly >= 144 && (IF_REG & VBL_IFLAG)) t++;
    __asm__("ei");
    return (uint32_t)t * 154u + (ly >= 144 ? ly - 144u : ly + 10u);
}

void main(void) {
    uint8_t r;
    gd_rng_seed(PLAYOUT_SEED);
    for (r = 0; r < PLAYOUT_RUNS; r++) {
        const uint32_t t0 = clock_lines();
        const uint16_t m = playout_one();
        const uint32_t d = clock_lines() - t0;
        po_moves_total += m;
        po_lines_sum += d;
        if (d > po_lines_max) po_lines_max = d;
        if (m > po_moves_max) po_moves_max = m;
    }
    // the same playouts again, timed phase by phase
    po_prof = 1;
    gd_rng_seed(PLAYOUT_SEED);
    for (r = 0; r < PLAYOUT_RUNS; r++) (void)playout_one();
    po_done = 1;
    for (;;) vsync();
}

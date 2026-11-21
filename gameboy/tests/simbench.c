// simbench - one rollout-engine playout timed on the Game Boy: SIMBENCH_RUNS
// fresh deals, each played out by gd_sim to the end. The clang build
// (SIMBENCH_HOST) prints the plies to match; the ROM build leaves in RAM the
// total plies, the scanlines (sum and slowest) of each playout from the board
// to the end, and, built with SIM_PROF, how a ply's time splits.
#include <stdint.h>
#ifdef SIMBENCH_HOST
#include <stdio.h>
#else
#include <gb/gb.h>
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
#ifdef SIM_PROF
// [0] policy, [1] apply, [2] the draws (out of whichever phase made them)
uint32_t sb_phase[3];
static uint32_t sb_mark, sb_draw_t0, sb_draw_acc;
#define SIM_PROF_DRAW_IN()  (sb_draw_t0 = clock_lines())
#define SIM_PROF_DRAW_OUT() (sb_draw_acc += clock_lines() - sb_draw_t0)
#define SIM_PROF_MARK(i) do { const uint32_t t_ = clock_lines(); \
    if ((i) > 0) { sb_phase[(i) - 1] += t_ - sb_mark - sb_draw_acc; sb_phase[2] += sb_draw_acc; } \
    sb_draw_acc = 0; sb_mark = clock_lines(); } while (0)
#endif
#endif
#include "../src/gd_sim.c"
#include "../src/gd_rng.h"

#define SIMBENCH_SEED 0x2468u
#define SIMBENCH_RUNS 64u
#define SIMBENCH_CAP  1000u

static gd_game g;
static gd_sim s;

// A fresh deal, as a rollout board.
static void deal(void) {
    uint8_t deck[GD_NCARDS], i, j, t;
    for (i = 0; i < GD_NCARDS; i++) deck[i] = i;
    for (i = GD_NCARDS - 1u; i > 0; i--) {
        j = (uint8_t)gd_rng_below((uint32_t)i + 1u);
        t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    gd_deal(&g, 0, deck, (uint8_t)gd_rng_below(2));
    gd_sim_from_game(&s, &g);
}

static uint16_t plies_of_playout(void) {
    uint16_t n = 0;
    while (!gd_sim_done(&s) && n < SIMBENCH_CAP && gd_sim_step(&s) >= 0) n++;
    return n;
}

#ifdef SIMBENCH_HOST
int main(void) {
    unsigned long total = 0, mx = 0;
    gd_rng_seed(SIMBENCH_SEED);
    for (unsigned r = 0; r < SIMBENCH_RUNS; r++) {
        deal();
        unsigned n = plies_of_playout();
        total += n;
        if (n > mx) mx = n;
    }
    printf("%02lx %02lx\n", total & 0xFF, total >> 8);
    fprintf(stderr, "clang: %u playouts, %lu plies (%.1f a playout, %lu at most)\n", SIMBENCH_RUNS, total, (double)total / SIMBENCH_RUNS, mx);
    return 0;
}
#else
uint16_t sb_plies;
uint32_t sb_lines_sum, sb_lines_max;
uint8_t sb_done;
void main(void) {
    uint8_t r;
    gd_rng_seed(SIMBENCH_SEED);
    for (r = 0; r < SIMBENCH_RUNS; r++) {
        uint32_t t0, d;
        deal();
        t0 = clock_lines();
        sb_plies += plies_of_playout();
        d = clock_lines() - t0;
        sb_lines_sum += d;
        if (d > sb_lines_max) sb_lines_max = d;
    }
    sb_done = 1;
    for (;;) vsync();
}
#endif

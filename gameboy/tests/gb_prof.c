// gb_prof - a bank-0 scanline clock for section marks in the profiling ROM
// (Makefile durak_prof.gb). gb_prof_mark(i) adds the time since the last
// mark to section i - 1 (mark 0 only starts the clock).
#include <gb/gb.h>
#include <stdint.h>

uint32_t gb_prof_sect[4];
static uint32_t last;

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

// lines from a to b, across sys_time's wrap at 65536 frames
#define SPAN(a, b) ((b) >= (a) ? (b) - (a) : (b) + 65536ul * 154u - (a))

void gb_prof_mark(uint8_t i) {
    const uint32_t t = clock_lines();
    if (i > 0 && i <= 4) gb_prof_sect[i - 1] += SPAN(last, t);
    last = clock_lines();
}

// selfplay_rom - the SDCC side of the cross-compiler check: a test ROM that
// runs selfplay and leaves the sum in RAM for tools/gbrun to read
// (selfplay_out: sum lo, sum hi, moves lo, moves hi; selfplay_done = 1).
#include <gb/gb.h>
#include "selfplay.h"
uint8_t selfplay_out[4];
uint8_t selfplay_done;
void main(void) {
    uint16_t moves, sum = selfplay(SELFPLAY_SEED, SELFPLAY_GAMES, &moves);
    selfplay_out[0] = (uint8_t)sum; selfplay_out[1] = (uint8_t)(sum >> 8);
    selfplay_out[2] = (uint8_t)moves; selfplay_out[3] = (uint8_t)(moves >> 8);
    selfplay_done = 1;
    for (;;) vsync();
}

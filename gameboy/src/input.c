// input - see input.h.
#include <gb/gb.h>
#include "input.h"

static uint8_t now, before;
static uint8_t lr_frames;      // frames LEFT or RIGHT has been held
static uint8_t a_frames, sel_frames;

void in_update(void) {
    before = now;
    now = joypad();
    if (now & (J_LEFT | J_RIGHT)) { if (lr_frames < 255) lr_frames++; } else lr_frames = 0;
    if (now & J_A) { if (a_frames < 255) a_frames++; } else a_frames = 0;
    if (now & J_SELECT) { if (sel_frames < 255) sel_frames++; } else sel_frames = 0;
}

uint8_t in_pressed(uint8_t mask) { return (now & mask) && !(before & mask); }
uint8_t in_down(uint8_t mask) { return (now & mask) != 0; }
uint8_t in_any_pressed(void) { return (now & (uint8_t)~before) != 0; }

int8_t in_lr_step(void) {
    const int8_t dir = (now & J_LEFT) ? -1 : (now & J_RIGHT) ? 1 : 0;
    if (!dir) return 0;
    if (lr_frames == 1) return dir;
    if (lr_frames > IN_REPEAT_DELAY && ((uint8_t)(lr_frames - IN_REPEAT_DELAY) % IN_REPEAT_RATE) == 0) return dir;
    return 0;
}

// a_frames counts the frame A went down as 1, so "held for N frames" is > N.
uint8_t in_a_held(void) { return a_frames > IN_A_HELD_FRAMES; }
uint8_t in_select_held(void) { return sel_frames > IN_SELECT_HELD_FRAMES; }

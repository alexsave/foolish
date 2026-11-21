// input - see input.h.
//
// The buttons are read in the timer interrupt, about 60 times a second,
// whatever the game loop is doing: a press is latched until the loop takes it,
// so a redraw that runs past a frame never drops a short tap. The timer, not
// VBlank, because a screen change turns the LCD off for a few frames, and with
// the LCD off there are no VBlank interrupts. The repeat timing counts ticks.
#include <gb/gb.h>
#include "input.h"

static volatile uint8_t isr_now, isr_pressed;   // down now; went down since last taken
static volatile int8_t isr_steps;               // LEFT/RIGHT presses and repeats not yet taken
static volatile uint8_t lr_frames, a_frames, sel_frames;

static volatile uint16_t first_press_time;     // sys_time at the first press ever seen
static volatile uint8_t first_press_seen;

static uint8_t now, pressed;
static int8_t steps;

static void in_tick(void) {
    const uint8_t j = joypad();
    isr_pressed |= (uint8_t)(j & (uint8_t)~isr_now);
    if (isr_pressed && !first_press_seen) { first_press_time = sys_time; first_press_seen = 1; }
    isr_now = j;
    if (j & (J_LEFT | J_RIGHT)) {
        const int8_t dir = (j & J_LEFT) ? -1 : 1;
        if (lr_frames < 255) lr_frames++;
        if (lr_frames == 1
            || (lr_frames > IN_REPEAT_DELAY && ((uint8_t)(lr_frames - IN_REPEAT_DELAY) % IN_REPEAT_RATE) == 0)) {
            if (isr_steps > -8 && isr_steps < 8) isr_steps += dir;
        }
    } else lr_frames = 0;
    if (j & J_A) { if (a_frames < 255) a_frames++; } else a_frames = 0;
    if (j & J_SELECT) { if (sel_frames < 255) sel_frames++; } else sel_frames = 0;
}

// 4096 Hz / (256 - 188) = 60.2 ticks a second, one a frame near enough.
#define IN_TMA 188u

void in_init(void) {
    CRITICAL {
        add_TIM(in_tick);
        TMA_REG = IN_TMA;
        TIMA_REG = IN_TMA;
        TAC_REG = 0x04u;           // timer on, 4096 Hz
        set_interrupts(IE_REG | TIM_IFLAG);
    }
}

void in_update(void) {
    CRITICAL {
        now = isr_now;
        pressed = isr_pressed;
        isr_pressed = 0;
        steps = isr_steps;
        isr_steps = 0;
    }
}

uint8_t in_pressed(uint8_t mask) { return (pressed & mask) != 0; }
uint8_t in_down(uint8_t mask) { return (now & mask) != 0; }
uint8_t in_any_pressed(void) { return pressed != 0; }

int8_t in_lr_steps(void) { return steps; }

void in_keep_except(uint8_t used) {
    CRITICAL {
        isr_pressed |= (uint8_t)(pressed & (uint8_t)~used);
        isr_steps = (int8_t)(isr_steps + steps);
        pressed = 0;
        steps = 0;
    }
}

uint16_t in_first_press_time(void) {
    uint16_t t;
    CRITICAL { t = first_press_time; }
    return t;
}

// a_frames counts the frame A went down as 1, so "held for N frames" is > N.
uint8_t in_a_held(void) { return (now & J_A) && a_frames > IN_A_HELD_FRAMES; }
uint8_t in_select_held(void) { return (now & J_SELECT) && sel_frames > IN_SELECT_HELD_FRAMES; }

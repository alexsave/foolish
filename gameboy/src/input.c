// input - see input.h.
//
// The buttons are read in the timer interrupt, about 60 times a second,
// whatever the game loop is doing: a press is latched until the loop takes it,
// so a redraw that runs past a frame never drops a short tap. The timer, not
// VBlank, because a screen change turns the LCD off for a few frames, and with
// the LCD off there are no VBlank interrupts. The repeat timing counts ticks.
#include <gb/gb.h>
#include "input.h"
#include "snd.h"

static volatile uint8_t isr_now, isr_pressed;   // down now; went down since last taken
static volatile int8_t isr_steps;               // LEFT/RIGHT presses and repeats not yet taken
static volatile uint8_t lr_frames, a_frames, sel_frames;

static volatile uint16_t first_press_time;     // sys_time at the first press ever seen
static volatile uint8_t first_press_seen;

static uint8_t now, pressed;
static int8_t steps;

// The profiling ROM (Makefile durak_sndprof.gb) times the interrupt: how late
// it ran after the timer's overflow, and how long the sound tick took, in
// steps of 256 clocks (61 us), read from TIMA (reloaded to IN_TMA at the
// overflow, then counting every 1024 clocks) and DIV's low two bits (every
// 256: both run off the one divider, so DIV's bits are 0 at each TIMA step).
#define IN_TMA 188u            // 4096 Hz / (256 - 188) = 60.2 ticks a second
#ifdef SND_PROF
// lateness with the LCD on and off (a screen load), from the first second on
// (the first tick after the timer starts is late by the boot)
uint8_t  snd_prof_late_max, snd_prof_late_off_max, snd_prof_tick_max;
uint16_t snd_prof_ticks, snd_prof_late_at;
static uint8_t prof_t0;
// TIMA and DIV are read apart, so DIV is read on both sides and the pair is
// read again if it moved between (a TIMA step in between read as 4 too few)
static uint8_t prof_now(void) {
    uint8_t d0, t, d1;
    do { d0 = DIV_REG; t = TIMA_REG; d1 = DIV_REG; } while (d0 != d1);
    return (uint8_t)(((uint8_t)(t - IN_TMA) << 2) | (d1 & 3u));
}
// and how late the VBlank interrupt ran: the line it began on, past 144
// (a VBlank held off past line 153 begins in the next frame's lines)
uint8_t snd_prof_vbl_max;
static void prof_vbl(void) {
    const uint8_t ly = LY_REG, late = ly >= 144u ? (uint8_t)(ly - 144u) : (uint8_t)(ly + 10u);
    if (late > snd_prof_vbl_max) snd_prof_vbl_max = late;
}
#define SND_PROF_ENTER() do { prof_t0 = prof_now(); } while (0)
#define SND_PROF_LEAVE() do { const uint8_t d = (uint8_t)(prof_now() - prof_t0); if (d > snd_prof_tick_max) snd_prof_tick_max = d; } while (0)
#else
#define SND_PROF_ENTER()
#define SND_PROF_LEAVE()
#endif

static void in_tick(void) {
#ifdef SND_PROF
    {
        const uint8_t late = prof_now();
        snd_prof_ticks++;
        if (snd_prof_ticks > 60u) {
            if (!(LCDC_REG & LCDCF_ON)) { if (late > snd_prof_late_off_max) snd_prof_late_off_max = late; }
            else if (late > snd_prof_late_max) { snd_prof_late_max = late; snd_prof_late_at = sys_time; }
        }
    }
#endif
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
    // The sound driver's 60 Hz (README "Sound"), with interrupts on again: a
    // tick that starts a cue runs for up to 4 ms of SDCC's code, and held in
    // here it held the VBlank interrupt off by up to 37 lines (the OAM copy
    // then lands in the picture). Only VBlank can come in meanwhile (the next
    // timer interrupt is a tick away, longer than any tick), and it touches
    // nothing of the sound's; the pad is read above, before it.
    // Nothing sounding (always so while a bot thinks): no call, no frame, no
    // nesting, so a think's stack and time are as they were.
    if (!snd_active) return;
    enable_interrupts();
    SND_PROF_ENTER();
    snd_tick();
    SND_PROF_LEAVE();
}


void in_init(void) {
    CRITICAL {
        add_TIM(in_tick);
#ifdef SND_PROF
        add_VBL(prof_vbl);
#endif
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

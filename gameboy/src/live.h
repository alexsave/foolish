// live - the screen while a bot thinks. Seoul and Moscow search for seconds
// in the game loop; meanwhile the input timer's interrupt (input.c in_tick)
// calls live_tick, which runs the game's handler (main.c live_think: the
// thinking dots, the cursor, the selection, the hand's order) a tick at a
// time, on a stack of its own in the cart's RAM, and puts back the ROM and
// RAM banks the search had mapped. README.md "The screen while a bot thinks".
#ifndef GD_LIVE_H
#define GD_LIVE_H

#include <stdint.h>

// The handler's stack: the top of cart RAM bank 3, above the solver's
// transposition table (gd_solve.c checks the table ends below the floor).
#define LIVE_RAM_BANK    3
#define LIVE_STACK_TOP   0xC000     // no suffix: live.c uses these in assembly
#define LIVE_STACK_FLOOR 0xB400

// From banked code: the handler, in the ROM bank mapped at this call (the
// caller's own), runs once every timer tick until live_stop.
void live_start(void (*fn)(void));
void live_stop(void);
// From the timer interrupt only (interrupts off), while live_on.
extern volatile uint8_t live_on;
void live_tick(void);

#endif

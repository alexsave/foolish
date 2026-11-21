// The stack guard: a canary painted over the gap between the end of the work
// RAM's data and the stack at boot, read back at the end of a game. Game Boy
// builds only (the host builds have no such gap).
#ifndef GD_STACK_H
#define GD_STACK_H
#include <stdint.h>

#define GD_STACK_CANARY 0xA5u
#define GD_STACK_TOP    0xDFF0u   // the scan stops here: main's own frames live above

extern uint16_t gd_stack_free;    // bytes of the gap still untouched, set by gd_stack_scan

void gd_stack_paint(void);        // boot, interrupts off: paint from the end of the data up to the stack pointer
void gd_stack_scan(void);         // set gd_stack_free: the canary bytes from the end of the data up that are still there

#endif

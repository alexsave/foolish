// The stack guard (gd_stack.h). The linker's s__HEAP_END is the first byte
// past the data; the stack grows down from 0xE000. Everything between is
// painted with GD_STACK_CANARY at boot, and the stack's deepest reach is the
// lowest byte no longer holding it, so the untouched run from the bottom up
// is the headroom. Bank 0: it runs before anything is banked in.
#include "gd_stack.h"

uint16_t gd_stack_free;

// bc = the end of the data (SDCC SM83 returns a 16-bit value in BC)
static uint16_t data_end(void) __naked {
    __asm
        ld bc, #s__HEAP_END
        ret
    __endasm;
}

// Paint [s__HEAP_END, sp - 4): up to this call's own return address.
void gd_stack_paint(void) __naked {
    __asm
        ldhl sp, #-4
        ld d, h
        ld e, l
        ld hl, #s__HEAP_END
        ld b, #0xA5
    00001$:
        ld (hl), b
        inc hl
        ld a, h
        cp a, d
        jr nz, 00001$
        ld a, l
        cp a, e
        jr nz, 00001$
        ret
    __endasm;
}

void gd_stack_scan(void) {
    const uint16_t lo = data_end();
    const uint8_t *p = (const uint8_t *)lo;
    while ((uint16_t)p < GD_STACK_TOP && *p == GD_STACK_CANARY) p++;
    gd_stack_free = (uint16_t)((uint16_t)p - lo);
}

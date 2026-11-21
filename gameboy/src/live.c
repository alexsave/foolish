// live - see live.h. In bank 0: it runs from an interrupt while any bank is
// mapped.
#include <gb/gb.h>
#include "live.h"
#include "gd_botlib.h"

static volatile uint8_t live_on;
static uint8_t live_busy;          // the handler is running (a nested tick returns)
static uint8_t live_bank;          // the handler's ROM bank
static uint8_t live_rom;           // the search's ROM bank, while the handler runs
static void (*live_fn)(void);
static uint16_t live_sp;           // the search's stack pointer, while the handler runs

void live_start(void (*fn)(void)) {
    CRITICAL {
        live_fn = fn;
        live_bank = _current_bank;
        live_on = 1;
    }
}

void live_stop(void) { live_on = 0; }

// The search was stopped at any instruction, in any ROM bank and any cart RAM
// bank, with its stack wherever it had grown to (work RAM leaves about 80
// bytes). So: map the handler's ROM bank and the stack's RAM bank, move the
// stack, let interrupts in again (the VBlank must still copy the sprites and
// count frames; a tick that comes meanwhile sees live_busy and returns), run
// the handler, and put everything back. Both banks are put back from what the
// search last asked for: GBDK's banked calls write _current_bank before the
// MBC register, and the solver writes gd_ram_bank before its register, so a
// tick between the two still restores the bank meant. On the search's stack
// this costs only in_tick's call here (2 bytes).
void live_tick(void) __naked {
    __asm
        ld      a, (_live_on)
        or      a
        ret     z
        ld      hl, #_live_busy
        ld      a, (hl)
        or      a
        ret     nz
        ld      (hl), #1
        ldh     a, (__current_bank)
        ld      (_live_rom), a
        ld      a, (_live_bank)
        ldh     (__current_bank), a
        ld      (0x2000), a
        ld      a, #LIVE_RAM_BANK
        ld      (0x4000), a
        ld      hl, #0
        add     hl, sp
        ld      a, l
        ld      (_live_sp), a
        ld      a, h
        ld      (_live_sp + 1), a
        ld      sp, #LIVE_STACK_TOP
        ei
        ld      hl, #_live_fn
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        rst     0x20
        di
        ld      hl, #_live_sp
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        ld      sp, hl
        ld      a, (_gd_ram_bank)
        ld      (0x4000), a
        ld      a, (_live_rom)
        ldh     (__current_bank), a
        ld      (0x2000), a
        xor     a
        ld      (_live_busy), a
        ret
    __endasm;
}

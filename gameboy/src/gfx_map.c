// gfx_map - the background map in RAM and its flush to VRAM, in bank 0 (no
// bank flag: bank 1, main.c and gfx.c, is full). See gfx.h.
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"

#define map gfx_map
#define shown_valid gfx_shown_valid
uint8_t gfx_map[SCR_H * SCR_W];
// What VRAM holds, so a flush writes only the rows that changed: writing all
// 360 tiles costs about 180 scanlines, more than a frame, and a redraw that
// long drops short button presses.
static uint8_t shown[SCR_H * SCR_W];
uint8_t gfx_shown_valid;
// Two maps, so a redraw never shows half old and half new (README "The screen
// in one piece"): the picture is the BG map LCDC bit 3 selects, and a flush
// writes the other one, which LCDC bit 6 (the window's map; the window is
// never shown) always names, so set_win_tiles writes the back map. The
// VBlank swaps the two bits (flip_req), never mid-picture. `stale`: the rows
// the back map lacks (those the last flush changed, now in front only), a bit
// a row.
static volatile uint8_t flip_req;
static uint8_t stale[3];
#define STALE(y) (stale[(y) >> 3] & (uint8_t)(1u << ((y) & 7u)))
// The VBlank: the back map goes up whole (bank 0: an interrupt runs while
// any bank is mapped).
static void gfx_vbl(void) {
    if (flip_req) { LCDC_REG ^= (uint8_t)(LCDCF_BG9C00 | LCDCF_WIN9C00); flip_req = 0; }
}

// One row, into both maps (the thinking dots, from the interrupt, while the
// search, not a flush, has the main loop: one row cannot tear).
void gfx_flush_row(uint8_t y) {
    const uint16_t o = (uint16_t)y * SCR_W;
    if (shown_valid && !memcmp(map + o, shown + o, SCR_W)) return;
    set_bkg_tiles(0, y, SCR_W, 1, map + o);
    set_win_tiles(0, y, SCR_W, 1, map + o);
    memcpy(shown + o, map + o, SCR_W);
}

void gfx_flush(void) {
    uint8_t y, any = 0, now[3] = { 0, 0, 0 };
    const uint8_t on = LCDC_REG & LCDCF_ON;
    // the back map may still be waiting to go up: never write it then (with
    // the LCD off there is no VBlank, and no flip is ever asked for)
    while (flip_req) ;
    for (y = 0; y < SCR_H; y++) {
        const uint16_t o = (uint16_t)y * SCR_W;
        const uint8_t changed = !shown_valid || memcmp(map + o, shown + o, SCR_W);
        if (!changed && !STALE(y)) continue;
        set_win_tiles(0, y, SCR_W, 1, map + o);
        // with the LCD off both maps are written: nothing is on show
        if (!on) set_bkg_tiles(0, y, SCR_W, 1, map + o);
        if (changed) { memcpy(shown + o, map + o, SCR_W); now[y >> 3] |= (uint8_t)(1u << (y & 7u)); any = 1; }
    }
    shown_valid = 1;
    if (on) { memcpy(stale, now, 3); if (any) flip_req = 1; }
    else memset(stale, 0, 3);
}


// Between redraws, a few of the stale rows at a time go into the back map
// (what is on show, `shown`), so the next flush seldom has to write a row
// twice: the main loop calls this once a pass. Never while a flip waits (the
// back map is then the one about to go up).
#define CATCH_UP_ROWS 2
void gfx_catch_up(void) {
    uint8_t y, k = CATCH_UP_ROWS;
    if (flip_req || !(stale[0] | stale[1] | stale[2])) return;
    for (y = 0; y < SCR_H && k; y++) {
        if (!STALE(y)) continue;
        set_win_tiles(0, y, SCR_W, 1, shown + (uint16_t)y * SCR_W);
        stale[y >> 3] &= (uint8_t)~(1u << (y & 7u));
        k--;
    }
}

// The picture on the 0x9800 map, the back map (the window's, never shown) on
// 0x9C00, and the flip in the VBlank.
void gfx_map_init(void) {
    LCDC_REG = (uint8_t)((LCDC_REG | LCDCF_WIN9C00) & (uint8_t)~(LCDCF_BG9C00 | LCDCF_WINON));
    CRITICAL { add_VBL(gfx_vbl); }
}

/* The art check ROM: loads the three tile banks the way the game will
 * (cards, ui, sprites into one 256-tile VRAM space at 0x8000), then shows
 * every table mockup composed from them.  A or right = next screen,
 * left = previous, B = the next full screen (dashboard, win, lose).  Proves the exported tiles, indices and palettes work on
 * the hardware before the game code uses them.  Build: make -C gameboy/art/gbdk/check */
#include <gb/gb.h>
#include <stdint.h>
#include "../art_tiles.h"
#include "../cards.h"
#include "../ui.h"
#include "../sprites.h"
#include "check_screens.h"
#include "../dashboard.h"
#include "../dashboard_new.h"
#include "../win.h"
#include "../lose.h"

/* The full screens ship as png2asset maps with their own tilesets. */
typedef struct { const uint8_t *tiles; uint8_t count; const uint8_t *map; } full_screen;
static const full_screen fulls[4] = {
    {dashboard_tiles, dashboard_TILE_COUNT, dashboard_map},
    {dashboard_new_tiles, dashboard_new_TILE_COUNT, dashboard_new_map},
    {win_tiles, win_TILE_COUNT, win_map},
    {lose_tiles, lose_TILE_COUNT, lose_map},
};

static void load_banks(void)
{
    set_bkg_data(ART_CARDS_BASE, ART_CARDS_COUNT, cards_tiles);
    set_bkg_data(ART_UI_BASE, ART_UI_COUNT, ui_tiles);
    set_sprite_data(ART_SPRITES_BASE, ART_SPRITES_COUNT, sprites_tiles);
}

static void show_full(uint8_t k)
{
    uint8_t i;
    DISPLAY_OFF;
    set_bkg_data(0, fulls[k].count, fulls[k].tiles);
    set_bkg_tiles(0, 0, 20, 18, fulls[k].map);
    for (i = 0; i < 40; i++)
        move_sprite(i, 0, 0);
    DISPLAY_ON;
}

static void show(uint8_t n)
{
    const check_screen *s = &check_screens[n];
    uint8_t i;
    set_bkg_tiles(0, 0, 20, 18, s->map);
    for (i = 0; i < 40; i++) {
        if (i < s->nspr) {
            set_sprite_tile(i, s->spr[i].tile);
            set_sprite_prop(i, s->spr[i].flags);
            move_sprite(i, s->spr[i].x, s->spr[i].y);
        } else {
            move_sprite(i, 0, 0);
        }
    }
}

void main(void)
{
    uint8_t n = 0, f = 0, keys, prev = 0, in_full = 0;
    DISPLAY_OFF;
    LCDC_REG |= LCDCF_BG8000;          /* background tiles at 0x8000, shared with sprites */
    BGP_REG = ART_BGP;
    OBP0_REG = ART_OBP0;
    load_banks();
    SPRITES_8x8;
    show(n);
    SHOW_BKG;
    SHOW_SPRITES;
    DISPLAY_ON;
    while (1) {
        vsync();
        keys = joypad();
        if ((keys & J_B) && !(prev & J_B)) {
            show_full(f);
            f = (uint8_t)((f + 1) % 4);
            in_full = 1;
        } else if (in_full && (keys & (J_A | J_RIGHT | J_LEFT)) && !(prev & (J_A | J_RIGHT | J_LEFT))) {
            DISPLAY_OFF;
            load_banks();
            show(n);
            DISPLAY_ON;
            in_full = 0;
        } else if ((keys & (J_A | J_RIGHT)) && !(prev & (J_A | J_RIGHT))) {
            n = (uint8_t)((n + 1) % CHECK_SCREENS);
            show(n);
        } else if ((keys & J_LEFT) && !(prev & J_LEFT)) {
            n = (uint8_t)((n + CHECK_SCREENS - 1) % CHECK_SCREENS);
            show(n);
        }
        prev = keys;
    }
}

// gfx - see gfx.h.
#include <gb/gb.h>
#include "gfx.h"
#include "gd_rules.h"
#include "tiles.h"

static uint8_t map[SCR_H * SCR_W];

void gfx_init(void) {
    DISPLAY_OFF;
    BGP_REG = 0xE4;     // 0 white, 1 light, 2 dark, 3 black
    OBP0_REG = 0xE4;
    set_bkg_data(0, BG_TILES, bg_tile_data);
    set_sprite_data(0, SPR_TILES, spr_tile_data);
    SPRITES_8x8;
    set_sprite_tile(SPR_CURSOR, S_UNDERLINE);
    set_sprite_tile(SPR_FRAME_TOP, S_FRAME_TOP);
    set_sprite_tile(SPR_FRAME_BOT, S_FRAME_BOT);
    gfx_sprite(SPR_CURSOR, 0, 0, 0);
    gfx_sprite(SPR_FRAME_TOP, 0, 0, 0);
    gfx_sprite(SPR_FRAME_BOT, 0, 0, 0);
    gfx_clear();
    gfx_flush();
    SHOW_BKG;
    SHOW_SPRITES;
    DISPLAY_ON;
}

void gfx_clear(void) {
    uint16_t i;
    for (i = 0; i < SCR_W * SCR_H; i++) map[i] = T_FELT;
}

void gfx_tile(uint8_t x, uint8_t y, uint8_t tile) {
    if (x < SCR_W && y < SCR_H) map[(uint16_t)y * SCR_W + x] = tile;
}

static uint8_t char_tile(char c) {
    uint8_t i;
    for (i = 0; font_chars[i]; i++) if (font_chars[i] == c) return (uint8_t)(T_FONT + i);
    return T_FELT;
}

void gfx_text(uint8_t x, uint8_t y, const char *s) {
    while (*s && x < SCR_W) gfx_tile(x++, y, char_tile(*s++));
}

void gfx_num(uint8_t x, uint8_t y, uint8_t n) {
    if (n >= 10) gfx_tile(x++, y, char_tile((char)('0' + n / 10)));
    gfx_tile(x, y, char_tile((char)('0' + n % 10)));
}

void gfx_card(uint8_t x, uint8_t y, uint8_t card, uint8_t selected) {
    gfx_tile(x, y, (uint8_t)((selected ? T_RANK_SEL : T_RANK) + gd_rank_of[card]));
    gfx_tile(x, (uint8_t)(y + 1u), (uint8_t)((selected ? T_SUIT_SEL : T_SUIT) + gd_suit_of[card]));
}

void gfx_suit(uint8_t x, uint8_t y, uint8_t suit) { gfx_tile(x, y, (uint8_t)(T_SUIT + suit)); }

void gfx_flush(void) { set_bkg_tiles(0, 0, SCR_W, SCR_H, map); }

void gfx_sprite(uint8_t id, uint8_t show, uint8_t x, uint8_t y) {
    if (!show) { move_sprite(id, 0, 0); return; }
    move_sprite(id, (uint8_t)(x * 8u + 8u), (uint8_t)(y * 8u + 16u));
}

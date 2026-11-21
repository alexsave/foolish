// gfx - the screen as a 20x18 tile map in RAM, flushed to VRAM in one call,
// plus the few sprites the game uses. Placeholder tiles from tools/gen_tiles.c.
#ifndef GD_GFX_H
#define GD_GFX_H

#include <stdint.h>

#define SCR_W 20
#define SCR_H 18

#define SPR_CURSOR    0
#define SPR_FRAME_TOP 1
#define SPR_FRAME_BOT 2

void gfx_init(void);
void gfx_clear(void);
void gfx_text(uint8_t x, uint8_t y, const char *s);
void gfx_num(uint8_t x, uint8_t y, uint8_t n);      // 1 or 2 digits, left aligned
void gfx_card(uint8_t x, uint8_t y, uint8_t card, uint8_t selected);   // two tiles tall
void gfx_suit(uint8_t x, uint8_t y, uint8_t suit);  // a card's lower half alone
void gfx_tile(uint8_t x, uint8_t y, uint8_t tile);
void gfx_flush(void);
// Sprites at tile cell (x, y); hidden when show is 0.
void gfx_sprite(uint8_t id, uint8_t show, uint8_t x, uint8_t y);

#endif

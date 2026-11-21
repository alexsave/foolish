// gfx - the screen as a 20x18 tile map in RAM, flushed to VRAM in one call,
// the sprites, and the full-screen pictures. All art is gameboy/art/gbdk
// (gameboy/art/README.md has the layout this follows).
#ifndef GD_GFX_H
#define GD_GFX_H

#include <stdint.h>

#define SCR_W 20
#define SCR_H 18

// Icons inside a text string (the rail font has no lower case).
#define IC_UP    "\x01"
#define IC_DOWN  "\x02"
#define IC_LEFT  "\x03"
#define IC_RIGHT "\x04"
#define IC_A     "\x05"
#define IC_B     "\x06"
#define IC_BACK  "\x07"     // the mini card back (the bot's count)
#define IC_DECK  "\x08"     // the mini deck
#define IC_SUIT0 9          // 9..12: the suit icons, as a char code

// The full-screen pictures.
#define FULL_DASHBOARD     0
#define FULL_DASHBOARD_NEW 1
#define FULL_LOSE          2
#define FULL_WIN           3    // + the bot's city, 0..3

void gfx_init(void);
void gfx_load_table(void);                       // the three banks (after a full screen)
void gfx_load_full(uint8_t which);               // a picture into the map, and the font it prints with
void gfx_clear_table(void);                      // wool, rails empty
void gfx_tile(uint8_t x, uint8_t y, uint8_t tile);
void gfx_text(uint8_t x, uint8_t y, const char *s);
uint8_t gfx_text_len(const char *s);
void gfx_rail(uint8_t row, const char *left, const char *right);
// A card face, a back or the pass slot (2x3 tiles) at tile (x, y), its corner
// tiles chosen by what is under each round corner (art README "round-corner rule").
void gfx_card(uint8_t x, uint8_t y, uint8_t card);
void gfx_back(uint8_t x, uint8_t y);
void gfx_pass_slot(uint8_t x, uint8_t y);
void gfx_flush(void);

// Sprites: a running allocator, reset every frame that redraws them.
void gfx_spr_begin(void);
void gfx_spr(int16_t px, int16_t py, uint8_t tile, uint8_t flags);   // screen pixels
void gfx_spr_end(void);                          // hides every sprite not placed
void gfx_spr_text(int16_t px, int16_t py, const char *s);           // a "chip" of font tiles

#endif

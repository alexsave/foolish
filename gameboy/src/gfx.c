// gfx - see gfx.h.
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "gd_rules.h"
#include "art_tiles.h"
#include "cards.h"
#include "ui.h"
#include "sprites.h"
#include "dashboard.h"
#include "dashboard_new.h"
#include "win.h"
#include "lose.h"

// The full-screen pictures live in ROM bank 2 (Makefile ART_BANK); the
// loader that switches to it runs from bank 0.
#define ART_BANK 2
// While a full screen is up its tileset takes the low tiles, so the rail font
// moves here (art README "Dashboard").
#define FULL_UI_BASE 160u
#define FULL_POINTER 230u

static uint8_t map[SCR_H * SCR_W];
static uint8_t font_base = ART_UI_BASE;

// ---- loading --------------------------------------------------------------------------
void gfx_load_table(void) {
    DISPLAY_OFF;
    set_bkg_data(ART_CARDS_BASE, ART_CARDS_COUNT, cards_tiles);
    set_bkg_data(ART_UI_BASE, ART_UI_COUNT, ui_tiles);
    set_sprite_data(ART_SPRITES_BASE, ART_SPRITES_COUNT, sprites_tiles);
    font_base = ART_UI_BASE;
    DISPLAY_ON;
}

void gfx_init(void) {
    DISPLAY_OFF;
    LCDC_REG |= LCDCF_BG8000;
    BGP_REG = ART_BGP;
    OBP0_REG = ART_OBP0;
    SPRITES_8x8;
    gfx_load_table();
    gfx_spr_begin();
    gfx_spr_end();
    SHOW_BKG;
    SHOW_SPRITES;
    DISPLAY_ON;
}

// From bank 0, with the display off: switch the picture's bank in, copy its
// tiles to VRAM and its map to RAM with plain loops, switch back. Interrupts
// are off and no library routine is called while bank 2 is mapped, because
// the library and the game code live in 0x4000-0x7FFF, which bank 2 replaces;
// only this function (in _HOME, which the Makefile places in bank 0) runs.
static void copy_banked(uint8_t *dst, const uint8_t *src, uint16_t n) NONBANKED {
    while (n--) *dst++ = *src++;
}

static void load_full_banked(uint8_t which) NONBANKED {
    const uint8_t save = _current_bank;
    const uint8_t *tiles, *m;
    uint16_t count;
    __asm__("di");
    SWITCH_ROM(ART_BANK);
    switch (which) {
    case FULL_DASHBOARD:     tiles = dashboard_tiles; count = dashboard_TILE_COUNT; m = dashboard_map; break;
    case FULL_DASHBOARD_NEW: tiles = dashboard_new_tiles; count = dashboard_new_TILE_COUNT; m = dashboard_new_map; break;
    case FULL_WIN:           tiles = win_tiles; count = win_TILE_COUNT; m = win_map; break;
    default:                 tiles = lose_tiles; count = lose_TILE_COUNT; m = lose_map; break;
    }
    copy_banked((uint8_t *)0x8000, tiles, (uint16_t)(count * 16u));
    copy_banked(map, m, sizeof map);
    SWITCH_ROM(save);
    __asm__("ei");
}

void gfx_load_full(uint8_t which) {
    DISPLAY_OFF;    // VRAM is free to write while the LCD is off
    load_full_banked(which);
    set_bkg_data(FULL_UI_BASE, ART_UI_COUNT, ui_tiles);
    set_sprite_data(FULL_POINTER, 1, sprites_tiles + 16u * (T_SP_POINTER - ART_SPRITES_BASE));
    font_base = FULL_UI_BASE;
    DISPLAY_ON;
}

// ---- the map -------------------------------------------------------------------------
void gfx_tile(uint8_t x, uint8_t y, uint8_t tile) {
    if (x < SCR_W && y < SCR_H) map[(uint16_t)y * SCR_W + x] = tile;
}

static uint8_t at(uint8_t x, uint8_t y) {
    return (x < SCR_W && y < SCR_H) ? map[(uint16_t)y * SCR_W + x] : T_WOOL;
}

void gfx_clear_table(void) {
    uint16_t i;
    for (i = 0; i < SCR_W * SCR_H; i++) map[i] = T_WOOL;
    gfx_rail(0, "", "");
    gfx_rail(SCR_H - 1, "", "");
}

static uint8_t char_tile(char c) {
    const char *f = ART_FONT_CHARS;
    uint8_t i;
    if (c == IC_UP[0]) return (uint8_t)(font_base + (T_IC_UP - ART_UI_BASE));
    if (c == IC_DOWN[0]) return (uint8_t)(font_base + (T_IC_DOWN - ART_UI_BASE));
    if (c == IC_LEFT[0]) return (uint8_t)(font_base + (T_IC_LEFT - ART_UI_BASE));
    if (c == IC_RIGHT[0]) return (uint8_t)(font_base + (T_IC_RIGHT - ART_UI_BASE));
    if (c == IC_A[0]) return (uint8_t)(font_base + (T_IC_BTN_A - ART_UI_BASE));
    if (c == IC_B[0]) return (uint8_t)(font_base + (T_IC_BTN_B - ART_UI_BASE));
    if (c == IC_BACK[0]) return (uint8_t)(font_base + (T_IC_MINIBACK - ART_UI_BASE));
    if (c == IC_DECK[0]) return (uint8_t)(font_base + (T_IC_MINIDECK - ART_UI_BASE));
    if (c >= IC_SUIT0 && c < IC_SUIT0 + 4) return (uint8_t)(font_base + (T_IC_S - ART_UI_BASE) + (c - IC_SUIT0));
    for (i = 0; f[i]; i++) if (f[i] == c) return (uint8_t)(font_base + ART_FONT_FIRST + i);
    return (uint8_t)(font_base + ART_FONT_FIRST);   // a space
}

uint8_t gfx_text_len(const char *s) { return (uint8_t)strlen(s); }

void gfx_text(uint8_t x, uint8_t y, const char *s) {
    while (*s && x < SCR_W) gfx_tile(x++, y, char_tile(*s++));
}

void gfx_rail(uint8_t row, const char *left, const char *right) {
    uint8_t x;
    const uint8_t sp = char_tile(' ');
    for (x = 0; x < SCR_W; x++) gfx_tile(x, row, sp);
    gfx_text(0, row, left);
    x = gfx_text_len(right);
    gfx_text((uint8_t)(SCR_W - (x < SCR_W ? x : SCR_W)), row, right);
}

// ---- cards and the round-corner rule ------------------------------------------------------
// The colour index of a tile's pixel (0, row): what a card's round corner, drawn
// over that tile, would let show through.
static uint8_t left_pixel(uint8_t tile, uint8_t row) {
    const uint8_t *d;
    if (tile < ART_CARDS_BASE + ART_CARDS_COUNT) d = cards_tiles + 16u * (tile - ART_CARDS_BASE);
    else if (tile >= ART_UI_BASE && tile < ART_UI_BASE + ART_UI_COUNT) d = ui_tiles + 16u * (tile - ART_UI_BASE);
    else return 0;
    return (uint8_t)(((d[2u * row] >> 7) & 1u) | (((d[2u * row + 1u] >> 7) & 1u) << 1));
}

// Variant offsets by the colour under the corner. Rank tiles: _u0, _u3, _u1
// (no _u2: no top-left corner ever sits on dark grey, so it reads as _u1).
static uint8_t rank_tile(uint8_t rank, uint8_t under) {
    if (under == 3) return (uint8_t)(T_RANK_6_U3 + rank);
    if (under == 0) return (uint8_t)(T_RANK_6_U0 + rank);
    return (uint8_t)(T_RANK_6_U1 + rank);
}

// PIPL_s comes as _u0, _u3, _u1, _u2 in that order for each suit.
static uint8_t pipl_tile(uint8_t suit, uint8_t under) {
    static const uint8_t off[4] = { 0, 2, 3, 1 };   // colour 0, 1, 2, 3 -> u0, u1, u2, u3
    return (uint8_t)(T_PIPL_S_U0 + 4u * suit + off[under]);
}

void gfx_card(uint8_t x, uint8_t y, uint8_t card) {
    const uint8_t s = gd_suit_of[card];
    const uint8_t tl = left_pixel(at(x, y), 0), bl = left_pixel(at(x, (uint8_t)(y + 2u)), 7);
    gfx_tile(x, y, rank_tile(gd_rank_of[card], tl));
    gfx_tile((uint8_t)(x + 1u), y, T_FACE_TR);
    gfx_tile(x, (uint8_t)(y + 1u), (uint8_t)(T_SUIT_S + s));
    gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 1u), (uint8_t)(T_PIPT_S + s));
    gfx_tile(x, (uint8_t)(y + 2u), pipl_tile(s, bl));
    gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 2u), (uint8_t)(T_PIPR_S + s));
}

void gfx_back(uint8_t x, uint8_t y) {
    const uint8_t tl = left_pixel(at(x, y), 0), bl = left_pixel(at(x, (uint8_t)(y + 2u)), 7);
    gfx_tile(x, y, tl ? T_BACK_TL_U3 : T_BACK_TL_U0);
    gfx_tile((uint8_t)(x + 1u), y, T_BACK_TR);
    gfx_tile(x, (uint8_t)(y + 1u), T_BACK_ML);
    gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 1u), T_BACK_MR);
    gfx_tile(x, (uint8_t)(y + 2u), bl ? T_BACK_BL_U3 : T_BACK_BL_U0);
    gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 2u), T_BACK_BR);
}

void gfx_pass_slot(uint8_t x, uint8_t y) {
    gfx_tile(x, y, T_PASS_TL); gfx_tile((uint8_t)(x + 1u), y, T_PASS_TR);
    gfx_tile(x, (uint8_t)(y + 1u), T_PASS_ML); gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 1u), T_PASS_MR);
    gfx_tile(x, (uint8_t)(y + 2u), T_PASS_BL); gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 2u), T_PASS_BR);
}

void gfx_flush(void) { set_bkg_tiles(0, 0, SCR_W, SCR_H, map); }

// ---- sprites ---------------------------------------------------------------------------
static uint8_t spr_n;

void gfx_spr_begin(void) { spr_n = 0; }

void gfx_spr(int16_t px, int16_t py, uint8_t tile, uint8_t flags) {
    if (spr_n >= 40) return;
    set_sprite_tile(spr_n, tile);
    set_sprite_prop(spr_n, flags);
    move_sprite(spr_n, (uint8_t)(px + 8), (uint8_t)(py + 16));
    spr_n++;
}

void gfx_spr_end(void) {
    uint8_t i;
    for (i = spr_n; i < 40; i++) move_sprite(i, 0, 0);
}

void gfx_spr_text(int16_t px, int16_t py, const char *s) {
    while (*s) { gfx_spr(px, py, char_tile(*s++), 0); px += 8; }
}

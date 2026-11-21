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
// What VRAM holds, so a flush writes only the rows that changed: writing all
// 360 tiles costs about 180 scanlines, more than a frame, and a redraw that
// long drops short button presses.
static uint8_t shown[SCR_H * SCR_W];
static uint8_t shown_valid;
static uint8_t font_base = ART_UI_BASE;

// ---- loading --------------------------------------------------------------------------
void gfx_load_table(void) {
    DISPLAY_OFF;
    set_bkg_data(ART_CARDS_BASE, ART_CARDS_COUNT, cards_tiles);
    set_bkg_data(ART_UI_BASE, ART_UI_COUNT, ui_tiles);
    set_sprite_data(ART_SPRITES_BASE, ART_SPRITES_COUNT, sprites_tiles);
    font_base = ART_UI_BASE;
    shown_valid = 0;
    DISPLAY_ON;
}

void gfx_init(void) {
    DISPLAY_OFF;
    LCDC_REG |= LCDCF_BG8000;
    BGP_REG = ART_BGP;
    OBP0_REG = ART_OBP0;
    SPRITES_8x8;
    build_char_table();
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
    shown_valid = 0;
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
    memset(map, T_WOOL, sizeof map);
    gfx_rail(0, "", "");
    gfx_rail(SCR_H - 1, "", "");
}

// Character code -> tile offset from the font base, built once (gfx_init):
// the rail font and the icons in ART_FONT_CHARS order.
static uint8_t char_off[128];

static void build_char_table(void) {
    const char *f = ART_FONT_CHARS;
    uint8_t i;
    for (i = 0; i < 128; i++) char_off[i] = ART_FONT_FIRST;      // unknown: a space
    for (i = 0; f[i]; i++) char_off[(uint8_t)f[i]] = (uint8_t)(ART_FONT_FIRST + i);
    char_off[(uint8_t)IC_UP[0]] = T_IC_UP - ART_UI_BASE;
    char_off[(uint8_t)IC_DOWN[0]] = T_IC_DOWN - ART_UI_BASE;
    char_off[(uint8_t)IC_LEFT[0]] = T_IC_LEFT - ART_UI_BASE;
    char_off[(uint8_t)IC_RIGHT[0]] = T_IC_RIGHT - ART_UI_BASE;
    char_off[(uint8_t)IC_A[0]] = T_IC_BTN_A - ART_UI_BASE;
    char_off[(uint8_t)IC_B[0]] = T_IC_BTN_B - ART_UI_BASE;
    char_off[(uint8_t)IC_BACK[0]] = T_IC_MINIBACK - ART_UI_BASE;
    char_off[(uint8_t)IC_DECK[0]] = T_IC_MINIDECK - ART_UI_BASE;
    for (i = 0; i < 4; i++) char_off[IC_SUIT0 + i] = (uint8_t)(T_IC_S - ART_UI_BASE + i);
}

static uint8_t char_tile(char c) { return (uint8_t)(font_base + char_off[(uint8_t)c & 127u]); }

uint8_t gfx_text_len(const char *s) { return (uint8_t)strlen(s); }

void gfx_text(uint8_t x, uint8_t y, const char *s) {
    uint8_t *p;
    if (y >= SCR_H) return;
    p = map + (uint16_t)y * SCR_W + x;
    while (*s && x++ < SCR_W) *p++ = char_tile(*s++);
}

void gfx_rail(uint8_t row, const char *left, const char *right) {
    uint8_t x;
    memset(map + (uint16_t)row * SCR_W, char_tile(' '), SCR_W);
    gfx_text(0, row, left);
    // Both, when they fit with a space between; else the left one alone (a
    // bot's move flashing by wins over the action word until it fades).
    x = gfx_text_len(right);
    if (gfx_text_len(left) + 1u + x <= SCR_W) gfx_text((uint8_t)(SCR_W - x), row, right);
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

// The six tiles of a 2x3 card at (x, y), in reading order; clipped at the edges.
static void put6(uint8_t x, uint8_t y, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f) {
    uint8_t *p;
    if (x + 1u >= SCR_W || y + 2u >= SCR_H) {
        gfx_tile(x, y, a); gfx_tile((uint8_t)(x + 1u), y, b);
        gfx_tile(x, (uint8_t)(y + 1u), c); gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 1u), d);
        gfx_tile(x, (uint8_t)(y + 2u), e); gfx_tile((uint8_t)(x + 1u), (uint8_t)(y + 2u), f);
        return;
    }
    p = map + (uint16_t)y * SCR_W + x;
    p[0] = a; p[1] = b; p[SCR_W] = c; p[SCR_W + 1] = d; p[2 * SCR_W] = e; p[2 * SCR_W + 1] = f;
}

void gfx_card(uint8_t x, uint8_t y, uint8_t card) {
    const uint8_t s = gd_suit_of[card];
    const uint8_t tl = left_pixel(at(x, y), 0), bl = left_pixel(at(x, (uint8_t)(y + 2u)), 7);
    put6(x, y, rank_tile(gd_rank_of[card], tl), T_FACE_TR, (uint8_t)(T_SUIT_S + s), (uint8_t)(T_PIPT_S + s),
         pipl_tile(s, bl), (uint8_t)(T_PIPR_S + s));
}

void gfx_back(uint8_t x, uint8_t y) {
    const uint8_t tl = left_pixel(at(x, y), 0), bl = left_pixel(at(x, (uint8_t)(y + 2u)), 7);
    put6(x, y, tl ? T_BACK_TL_U3 : T_BACK_TL_U0, T_BACK_TR, T_BACK_ML, T_BACK_MR,
         bl ? T_BACK_BL_U3 : T_BACK_BL_U0, T_BACK_BR);
}

void gfx_pass_slot(uint8_t x, uint8_t y) {
    put6(x, y, T_PASS_TL, T_PASS_TR, T_PASS_ML, T_PASS_MR, T_PASS_BL, T_PASS_BR);
}


void gfx_flush(void) {
    uint8_t y;
    for (y = 0; y < SCR_H; y++) {
        const uint16_t o = (uint16_t)y * SCR_W;
        if (shown_valid && !memcmp(map + o, shown + o, SCR_W)) continue;
        set_bkg_tiles(0, y, SCR_W, 1, map + o);
        memcpy(shown + o, map + o, SCR_W);
    }
    shown_valid = 1;
}

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

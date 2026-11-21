// gfx - see gfx.h.
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "gd_rules.h"
#include "art_tiles.h"
#include "cards.h"
#include "ui.h"
#include "sprites.h"
#include "pile.h"
#include "dashboard.h"
#include "dashboard_new.h"
#include "win_miami.h"
#include "win_new_york.h"
#include "win_seoul.h"
#include "win_moscow.h"
#include "lose.h"
#include "lacquer.h"
#include "paper.h"
#include "plane.h"
#include "fireworks.h"

// All the art lives in switchable ROM banks (the Makefile places each file;
// BANK() reads where it went). The loaders that switch run from bank 0.

static uint8_t map[SCR_H * SCR_W];
// What VRAM holds, so a flush writes only the rows that changed: writing all
// 360 tiles costs about 180 scanlines, more than a frame, and a redraw that
// long drops short button presses.
static uint8_t shown[SCR_H * SCR_W];
static uint8_t shown_valid;
// The tile of character code c is font_base + char_off(c) (see char_tile).
static uint8_t font_base = ART_UI_BASE;

#define VRAM_TILE(i) ((uint8_t *)(0x8000u + 16u * (uint16_t)(i)))
static void from_bank(uint8_t bank, uint8_t *dst, const uint8_t *src, uint16_t n) NONBANKED;
static void build_corner_table(void);

// ---- loading --------------------------------------------------------------------------
void gfx_load_table(void) {
    DISPLAY_OFF;    // VRAM is free to write while the LCD is off
    from_bank(BANK(cards), VRAM_TILE(ART_CARDS_BASE), cards_tiles, 16u * ART_CARDS_COUNT);
    from_bank(BANK(ui), VRAM_TILE(ART_UI_BASE), ui_tiles, 16u * ART_UI_COUNT);
    from_bank(BANK(sprites), VRAM_TILE(ART_SPRITES_BASE), sprites_tiles, 16u * ART_SPRITES_COUNT);
    from_bank(BANK(pile), VRAM_TILE(ART_PILE_BASE), pile_tiles, 16u * ART_PILE_COUNT);
    font_base = ART_UI_BASE;
    // Never show new tiles under the old screen's map, even for a frame: the
    // map goes out (plain wool) before the display comes back.
    memset(map, T_WOOL, sizeof map);
    shown_valid = 0;
    gfx_flush();
    DISPLAY_ON;
}

void gfx_init(void) {
    DISPLAY_OFF;
    LCDC_REG |= LCDCF_BG8000;
    BGP_REG = ART_BGP;
    OBP0_REG = ART_OBP0;
    OBP1_REG = ART_OBP1;            // flying club cards only (gfx_spr_card)
    SPRITES_8x8;
    build_corner_table();
    gfx_load_table();
    gfx_spr_begin();
    gfx_spr_end();
    SHOW_BKG;
    SHOW_SPRITES;
    DISPLAY_ON;
}

// From bank 0, with the display off when the copy goes to VRAM: switch the
// art's bank in, copy with a plain loop, switch back. Interrupts are off and no
// library routine is called while the art bank is mapped, because the library
// and the game code live in 0x4000-0x7FFF, which the art bank replaces; only
// these functions (in _HOME, which the Makefile places in bank 0) run.
static void copy_banked(uint8_t *dst, const uint8_t *src, uint16_t n) NONBANKED {
    while (n--) *dst++ = *src++;
}

// In pieces of COPY_CHUNK bytes, with interrupts on between them (and the
// game's bank mapped again), so the input timer keeps ticking through a load.
#define COPY_CHUNK 256u
static void from_bank(uint8_t bank, uint8_t *dst, const uint8_t *src, uint16_t n) NONBANKED {
    const uint8_t save = _current_bank;
    while (n) {
        const uint16_t k = n < COPY_CHUNK ? n : COPY_CHUNK;
        __asm__("di");
        SWITCH_ROM(bank);
        copy_banked(dst, src, k);
        SWITCH_ROM(save);
        __asm__("ei");
        dst += k; src += k; n -= k;
    }
}

// A full-screen picture: its tileset from tile 0, its map into RAM.
static void load_picture(uint8_t bank, const uint8_t *tiles, uint8_t count, const uint8_t *m) {
    from_bank(bank, VRAM_TILE(0), tiles, (uint16_t)(count * 16u));
    from_bank(bank, map, m, sizeof map);
}

void gfx_load_full(uint8_t which) {
    DISPLAY_OFF;    // VRAM is free to write while the LCD is off
    switch (which) {
    case FULL_DASHBOARD:
    case FULL_DASHBOARD_NEW:
        if (which == FULL_DASHBOARD) load_picture(BANK(dashboard), dashboard_tiles, dashboard_TILE_COUNT, dashboard_map);
        else load_picture(BANK(dashboard_new), dashboard_new_tiles, dashboard_new_TILE_COUNT, dashboard_new_map);
        // the paper font (the city on the boarding pass), the pointer, the plane
        from_bank(BANK(paper), VRAM_TILE(ART_PAPER_BASE), paper_tiles, 16u * ART_PAPER_COUNT);
        from_bank(BANK(sprites), VRAM_TILE(ART_DASH_POINTER_TILE), sprites_tiles + 16u * (T_SP_POINTER - ART_SPRITES_BASE), 16u);
        from_bank(BANK(plane), VRAM_TILE(ART_DASH_PLANE_TILE), plane_tiles, 16u);
        font_base = (uint8_t)(ART_PAPER_BASE - ART_FONT_FIRST);
        break;
    case FULL_LOSE:
        load_picture(BANK(lose), lose_tiles, lose_TILE_COUNT, lose_map);
        break;
    default: {
        // a win: the city's picture, the lacquer font (the bot's name), the fireworks
        const uint8_t city = (uint8_t)(which - FULL_WIN);
        if (city == 0) load_picture(BANK(win_miami), win_miami_tiles, win_miami_TILE_COUNT, win_miami_map);
        else if (city == 1) load_picture(BANK(win_new_york), win_new_york_tiles, win_new_york_TILE_COUNT, win_new_york_map);
        else if (city == 2) load_picture(BANK(win_seoul), win_seoul_tiles, win_seoul_TILE_COUNT, win_seoul_map);
        else load_picture(BANK(win_moscow), win_moscow_tiles, win_moscow_TILE_COUNT, win_moscow_map);
        from_bank(BANK(lacquer), VRAM_TILE(ART_LACQUER_BASE), lacquer_tiles, 16u * ART_LACQUER_COUNT);
        from_bank(BANK(fireworks), VRAM_TILE(ART_FW_BASE), fireworks_tiles, 16u * fireworks_TILE_COUNT);
        font_base = (uint8_t)(ART_LACQUER_BASE - ART_FONT_FIRST);
        break;
    }
    }
    // the picture's map goes out before the display comes back (see gfx_load_table)
    shown_valid = 0;
    gfx_flush();
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

// Character code -> tile offset from the font base: the rail font in
// ART_FONT_CHARS order, the icons (gfx.h IC_*, codes 1 to 12, in the order of
// icon_off), and anything else a space. Computed, not tabled: a 128-byte RAM
// table for this was the cheapest RAM in the program to give back (README
// "stack guard").
static const uint8_t icon_off[12] = {
    T_IC_UP - ART_UI_BASE, T_IC_DOWN - ART_UI_BASE, T_IC_LEFT - ART_UI_BASE, T_IC_RIGHT - ART_UI_BASE,
    T_IC_BTN_A - ART_UI_BASE, T_IC_BTN_B - ART_UI_BASE, T_IC_MINIBACK - ART_UI_BASE, T_IC_MINIDECK - ART_UI_BASE,
    T_IC_S - ART_UI_BASE, T_IC_H - ART_UI_BASE, T_IC_C - ART_UI_BASE, T_IC_D - ART_UI_BASE,
};

// (tests/fontorder.c, in make test, fails the build if ART_FONT_CHARS stops being that order)
static uint8_t char_off(uint8_t c) {
    const char *f = ART_FONT_CHARS;
    uint8_t i;
    c &= 127u;
    if (c >= 1u && c <= 12u) return icon_off[c - 1u];
    // the common characters by arithmetic (the order of ART_FONT_CHARS: a space, A..Z, 0..9); the search is for the rest
    if (c == ' ') return ART_FONT_FIRST;
    if (c >= 'A' && c <= 'Z') return (uint8_t)(ART_FONT_FIRST + 1u + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(ART_FONT_FIRST + 27u + (c - '0'));
    for (i = 0; c && f[i]; i++)
        if ((uint8_t)f[i] == c) return (uint8_t)(ART_FONT_FIRST + i);
    return ART_FONT_FIRST;                      // unknown: a space
}

static uint8_t char_tile(char c) { return (uint8_t)(font_base + char_off((uint8_t)c)); }

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
// The colour under a card's round corners, cached per table tile at start (the
// tile data itself lives in ROM bank 2): bits 0-1 the tile's top-left pixel,
// bits 2-3 its bottom-left pixel.
#define CORNER_TILES (ART_UI_BASE + ART_UI_COUNT)
static uint8_t corner[CORNER_TILES];

static uint8_t px_colour(const uint8_t *t, uint8_t row) {
    return (uint8_t)(((t[2u * row] >> 7) & 1u) | (((t[2u * row + 1u] >> 7) & 1u) << 1));
}

static void build_corner_table(void) {
    uint8_t t[16];
    uint8_t i;
    for (i = 0; i < CORNER_TILES; i++) {
        if (i < ART_CARDS_COUNT) from_bank(BANK(cards), t, cards_tiles + 16u * i, 16);
        else from_bank(BANK(ui), t, ui_tiles + 16u * (i - ART_UI_BASE), 16);
        corner[i] = (uint8_t)(px_colour(t, 0) | (px_colour(t, 7) << 2));
    }
}

static uint8_t left_pixel(uint8_t tile, uint8_t row) {
    if (tile >= CORNER_TILES) return 0;
    return row ? (uint8_t)(corner[tile] >> 2) : (uint8_t)(corner[tile] & 3u);
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
    // under a corner: the table (_u0), a black border (_u3, the deck over the
    // trump) or another back's dark grey ring (_u2, the bot's hand)
    put6(x, y, tl == 3 ? T_BACK_TL_U3 : tl == 2 ? T_BACK_TL_U2 : T_BACK_TL_U0, T_BACK_TR, T_BACK_ML, T_BACK_MR,
         bl == 3 ? T_BACK_BL_U3 : bl == 2 ? T_BACK_BL_U2 : T_BACK_BL_U0, T_BACK_BR);
}

void gfx_pass_slot(uint8_t x, uint8_t y) {
    put6(x, y, T_PASS_TL, T_PASS_TR, T_PASS_ML, T_PASS_MR, T_PASS_BL, T_PASS_BR);
}


void gfx_flush_row(uint8_t y) {
    const uint16_t o = (uint16_t)y * SCR_W;
    if (shown_valid && !memcmp(map + o, shown + o, SCR_W)) return;
    set_bkg_tiles(0, y, SCR_W, 1, map + o);
    memcpy(shown + o, map + o, SCR_W);
}

void gfx_flush(void) {
    uint8_t y;
    for (y = 0; y < SCR_H; y++) gfx_flush_row(y);
    shown_valid = 1;
}

// ---- sprites ---------------------------------------------------------------------------
// A frame's sprites are built in a staging copy and handed to the shadow OAM
// in one copy with interrupts off: GBDK's VBlank copies the shadow OAM to the
// screen's, and building in place let a VBlank land mid-build and show half
// the new sprites over half the old (the aim's arrow moving in TARGET mode).
static uint8_t spr_n;
static uint8_t stage[40][4];          // y, x, tile, flags: OAM's layout

void gfx_spr_begin(void) { spr_n = 0; }

void gfx_spr(int16_t px, int16_t py, uint8_t tile, uint8_t flags) {
    if (spr_n >= 40) return;
    stage[spr_n][0] = (uint8_t)(py + 16);
    stage[spr_n][1] = (uint8_t)(px + 8);
    stage[spr_n][2] = tile;
    stage[spr_n][3] = flags;
    spr_n++;
}

void gfx_spr_end(void) {
    uint8_t i;
    for (i = spr_n; i < 40; i++) { stage[i][0] = 0; stage[i][1] = 0; }
    CRITICAL { memcpy((void *)shadow_OAM, stage, sizeof stage); }
}

// A card as six sprites, its top left at screen pixel (px, py). A club's
// seams are light grey, which a sprite cannot draw as index 0: its three seam
// tiles have sprite copies that store the seams as index 2, shown light grey
// by OBP1 (art README "Flying club cards"); every other card stays on OBP0.
void gfx_spr_card(int16_t px, int16_t py, uint8_t card) {
    const uint8_t s = gd_suit_of[card], club = s == GD_CLUBS;
    const uint8_t pal = club ? S_PALETTE : 0;
    gfx_spr(px, py, (uint8_t)(T_RANK_6_U0 + gd_rank_of[card]), pal);
    gfx_spr(px + 8, py, T_FACE_TR, pal);
    gfx_spr(px, py + 8, club ? T_SP_CLUB_SUIT : (uint8_t)(T_SUIT_S + s), pal);
    gfx_spr(px + 8, py + 8, (uint8_t)(T_PIPT_S + s), pal);
    gfx_spr(px, py + 16, club ? T_SP_CLUB_PIPL : pipl_tile(s, 0), pal);
    gfx_spr(px + 8, py + 16, club ? T_SP_CLUB_PIPR : (uint8_t)(T_PIPR_S + s), pal);
}

void gfx_spr_text(int16_t px, int16_t py, const char *s) {
    while (*s) { gfx_spr(px, py, char_tile(*s++), 0); px += 8; }
}

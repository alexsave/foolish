/* The textures the cups and dice wear (package C), read from a pack baked at
 * build time. Freestanding: no libc beyond memcpy / memset, no libm, no heap.
 *
 * WHY A PACK. The study (docs/UI.html, TEX and TEX3) makes every texture with
 * browser canvas code: an fbm cloud per texel for the verdigris and the bone,
 * then per-seed copies at an offset with a gradient, a numeral or the pips
 * over them. The noise is the expensive part, and a procedural texture made
 * at launch once took the iMessage extension down on a real device, so the
 * noise is baked by tools/cn_texgen.c (`make tex`) into build/cn_tex.pack and
 * the runtime only reads it: two source tiles (the verdigris, 256 square, and
 * the bone, 128 square) and the count numerals 0-9 as coverage masks at the
 * two sizes the crown stamps (112 and 184 texels, Libre Caslon Text's lining figures).
 *
 * WHAT THE RUNTIME DOES. Every per-seat and per-die texture is DERIVED here
 * from the shared tiles when it is uploaded, never stored: a cup's side,
 * inside, floor and crown are the verdigris at the seed's own offset with the
 * study's gradient, numeral or "out" tint over it, and a die's atlas is the
 * bone at six offsets with the strike wear and the pips. That is integer
 * copying plus a few gradients, a pass over the texels, no noise. Each
 * texture can come with its relief (a normal map of int8 (dx, dy) at a 20th,
 * the study's reliefOf / dieHeight), and cn_tex_mip makes the half-size copy
 * the scene renderer draws a small face from, with the renderer's formula.
 *
 * DETERMINISM. Integer arithmetic plus IEEE double +, -, *, / and sqrt (each
 * correctly rounded everywhere; build with -ffp-contract=off), so a seed gives
 * the same bytes on every compiler and on wasm; tests/cn_tex_test.c pins it.
 *
 * PACK LAYOUT (little-endian, fixed):
 *   header, CN_TEX_HEAD_BYTES:
 *     u32 magic 'CNTX'   u16 version   u16 entries
 *     u32 total bytes    u32 check (FNV-1a 32 of every byte after the header)
 *     u32 0              u32 0
 *   then `entries` records of CN_TEX_ENTRY_BYTES:
 *     u8 kind  u8 bytes a texel (3 RGB, 1 coverage)  u8 digit  u8 0
 *     u16 px (a glyph's font size, 0 for a tile)  u16 w  u16 h
 *     i16 ox  i16 oy (a glyph: its top-left from the pen's anchor)  u16 0
 *     u32 offset of the texels from the start of the pack (4-aligned)
 *   then the texels, rows top to bottom, each record's w * h * bytes. */
#ifndef CN_TEX_H
#define CN_TEX_H

#include <stdint.h>

#define CN_TEX_MAGIC        0x58544E43u    /* "CNTX" read as a little-endian u32 */
#define CN_TEX_VERSION      1
#define CN_TEX_HEAD_BYTES   24
#define CN_TEX_ENTRY_BYTES  20

/* THE PLANKS' TILE (tools/cn_texgen.c bakes it, the host lays it, the stage places its phase): six planks of
 * CN_TEX_PLANK_W points in a running bond, CN_TEX_PLANK_TILE_H tall; the even planks end at the tile's top, the
 * odd ones half way down, so a plank's end (a dark bar, CN_TEX_PLANK_END_HALF points either side, its lit lip
 * with it) crosses the table every half tile, alternate planks (package S: one fell behind the picker) */
#define CN_TEX_PLANK_W        86
#define CN_TEX_PLANK_TILE_H   830
#define CN_TEX_PLANK_END_HALF 12     /* .014 of the tile, 11.6 points, rounded up */

enum {
    CN_TEXK_VERD = 1,       /* the verdigris tile, RGB                       */
    CN_TEXK_BONE = 2,       /* the bone (the study's 'tallow'), RGB          */
    CN_TEXK_GLYPH = 3,      /* a numeral's coverage, one byte a texel        */
};

/* the sizes, each the study's own */
enum {
    CN_TEX_VERD = 256,      /* the verdigris tile (TEX.bake 'verdigris')     */
    CN_TEX_BONE = 128,      /* the bone tile (TEX.bake 'die.tallow')         */
    CN_TEX_SIDE_W = 1024,   /* a cup's side and its inside (TEX3.cupSide)    */
    CN_TEX_SIDE_H = 512,
    CN_TEX_CROWN = 256,     /* a cup's crown and its floor, square           */
    CN_TEX_DIE_CELL = 128,  /* one face of a die's atlas                     */
    CN_TEX_DIE_W = 6 * 128, /* the atlas: faces 1 to 6 left to right         */
    CN_TEX_NUMERAL_SMALL = 112,  /* the count on a crown                      */
    CN_TEX_NUMERAL_LARGE = 184,  /* ...on a cup under 40 points of radius     */
    CN_TEX_GLYPH_SIZES = 2,
    CN_TEX_DIGITS = 10,
    CN_TEX_ENTRIES = 2 + CN_TEX_GLYPH_SIZES * CN_TEX_DIGITS,
};

/* the relief's strength a texture is given, times 20 (the study's reliefOf
 * strength 1.6 for the side, 1.2 for the crown, inside and floor, and
 * dieHeight's 5.5), so the int8 at a 20th is exact integer arithmetic */
enum { CN_TEX_RELIEF_SIDE = 32, CN_TEX_RELIEF_CUP = 24, CN_TEX_RELIEF_DIE = 110 };

typedef struct {
    const uint8_t *px;      /* w * h coverage bytes, 0 none to 255 full      */
    int w, h, ox, oy;       /* top-left at (anchor x + ox, anchor y + oy)    */
} CnTexGlyph;

/* An opened pack: pointers INTO the caller's bytes, which must outlive it. */
typedef struct {
    const uint8_t *verd;    /* CN_TEX_VERD squared, RGB                      */
    const uint8_t *bone;    /* CN_TEX_BONE squared, RGB                      */
    CnTexGlyph glyph[CN_TEX_GLYPH_SIZES][CN_TEX_DIGITS];  /* [0] 112, [1] 184 */
} CnTexPack;

enum {
    CN_TEX_OK = 0, CN_TEX_E_SHORT = -1, CN_TEX_E_MAGIC = -2, CN_TEX_E_VERSION = -3,
    CN_TEX_E_CHECK = -4, CN_TEX_E_ENTRY = -5, CN_TEX_E_MISSING = -6,
};
/* Checks the header, the check and every record's bounds; CN_TEX_OK or an error. */
int cn_tex_pack_open(CnTexPack *p, const uint8_t *bytes, uint32_t len);
uint32_t cn_tex_fnv1a(const uint8_t *b, uint32_t n);

/* the study's integer hash, (ix, iy, seed) -> [0, 1) as a 32-bit numerator */
uint32_t cn_tex_hash(uint32_t ix, uint32_t iy, uint32_t seed);

/* ---- the textures, each written into the caller's RGBA (opaque) --------------- */
/* a cup's side, CN_TEX_SIDE_W by CN_TEX_SIDE_H: the verdigris at the seed's offset, grime toward the mouth */
void cn_tex_cup_side(const CnTexPack *p, uint32_t seed, uint8_t *rgba);
/* the inner wall, same size: the verdigris darker the deeper */
void cn_tex_cup_inner(const CnTexPack *p, uint32_t seed, uint8_t *rgba);
/* the inner floor, CN_TEX_CROWN square: darker toward the wall */
void cn_tex_cup_floor(const CnTexPack *p, uint32_t seed, uint8_t *rgba);
/* the crown, CN_TEX_CROWN square: the verdigris at its own offset, then the
 * count (0-9; -1 for none) at numeral_px (CN_TEX_NUMERAL_SMALL or _LARGE), or
 * when `out` the dark tint instead of any numeral */
void cn_tex_cup_crown(const CnTexPack *p, uint32_t seed, int count, int out, int numeral_px, uint8_t *rgba);
/* a die's atlas, CN_TEX_DIE_W by CN_TEX_DIE_CELL */
void cn_tex_die_atlas(const CnTexPack *p, uint32_t seed, uint8_t *rgba);

/* the pieces they are made of, for a host that composes its own */
/* the numeral `digit` in its shadow and its pale face at the crown's anchor; 0 when the digit or size is unknown */
int cn_tex_stamp_numeral(const CnTexPack *p, uint8_t *rgba, int w, int h, int digit, int numeral_px);
/* source-over a flat colour at alpha a255 (0..255) on every texel */
void cn_tex_tint(uint8_t *rgba, int w, int h, int r, int g, int b, int a255);

/* ---- relief and the smaller copies ----------------------------------------------- */
/* the study's reliefOf: the texture's own red, half strength over mid-grey, as height; its slope as int8 (dx, dy) at a 20th */
void cn_tex_relief(const uint8_t *rgba, int w, int h, int strength20, int8_t *bump);
/* the study's dieHeight: drilled pips and a trace of the grain, from the atlas the same seed made */
void cn_tex_die_relief(const uint8_t *atlas, uint32_t seed, int8_t *bump);
/* the next smaller copy (w / 2 by h / 2) by 2-by-2 means, the scene renderer's own formula; bump may be 0 */
void cn_tex_mip(const uint8_t *rgba, const int8_t *bump, int w, int h, uint8_t *half_rgba, int8_t *half_bump);

/* ---- upload ------------------------------------------------------------------------ */
/* What the scene hands back for a new texture: room for w * h RGBA texels
 * and, when asked, w * h (dx, dy) int8 pairs. The same shape as the
 * renderer's scene_tex_new / scene_tex_rgba / scene_tex_bump, without naming it. */
typedef struct { int w, h; uint8_t *rgba; int8_t *bump; } CnTexImage;
typedef struct {
    void *ctx;
    /* make a texture; fill *img and return its id (>= 0), or -1 when there is no room */
    int (*alloc)(void *ctx, int w, int h, int has_bump, CnTexImage *img);
} CnTexSink;

typedef enum { CN_TEX_SIDE, CN_TEX_INNER, CN_TEX_FLOOR, CN_TEX_CROWN_T, CN_TEX_DIE } CnTexWhat;
typedef struct {
    CnTexWhat what;
    uint32_t seed;
    int count, out, numeral_px;   /* the crown's; ignored otherwise          */
    int bump;                     /* 1: make the relief too                   */
} CnTexSpec;
/* the texture's size for a spec */
void cn_tex_size(CnTexWhat what, int *w, int *h);
/* allocate through the sink, write the texture (and its relief), return the sink's id or -1 */
int cn_tex_upload(const CnTexPack *p, const CnTexSink *sink, const CnTexSpec *spec);

#endif

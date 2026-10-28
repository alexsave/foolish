/* tex_test.c - the texture generators alone (textures.c), for tools/texparity.mjs: each export
 * makes one texture with the given size and seed and returns where its bytes are. Its own memory
 * from memory.grow: nothing else in this module allocates. */
#include "../src/textures.h"

#define EXPORT(n) __attribute__((export_name(n)))
extern unsigned char __heap_base;
static uintptr_t top;
static void *grab(uint32_t n) {
  if (!top) top = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15;
  uintptr_t p = top, end = p + ((n + 15) & ~15u), have = (uintptr_t)__builtin_wasm_memory_size(0) << 16;
  if (end > have && __builtin_wasm_memory_grow(0, (end - have + 65535) >> 16) < 0) __builtin_trap();
  top = end;
  return (void *)p;
}
EXPORT("reset") void reset(void) { top = 0; }

/* The weave of a w x h picture, kept in the crop cw x ch at (cx, cy). */
EXPORT("wool") uint8_t *wool(int32_t w, int32_t h, int32_t cx, int32_t cy, int32_t cw, int32_t ch, double ox, double oy) {
  Wool t;
  uint8_t *px = grab((uint32_t)(cw * ch * 3));
  wool_begin(&t, px, w, h, cx, cy, cw, ch, ox, oy);
  while (!wool_run(&t, 1 << 20)) {}
  return px;
}

/* The wood: offsets drawn from the seed as WoodTexture's getRandomOffsets draws them, worked out
 * inside the nwin rectangles written to wood_rects(), or, with none, the whole picture. *col gets
 * the float colours (of the last rectangle; of the whole picture with none) for the finer check. */
static float *wood_col;
static TexRect rects[WOOD_MAX_WIN];
EXPORT("wood_rects") TexRect *wood_rects(void) { return rects; }
EXPORT("wood_col") float *get_wood_col(void) { return wood_col; }
EXPORT("wood") uint8_t *wood(int32_t w, int32_t h, uint32_t seed, int32_t nwin) {
  Wood t;
  double ox = tex_random(&seed) * 1000 - 500, oy = tex_random(&seed) * 1000 - 500;
  int32_t most = 0;
  if (!nwin) { rects[0] = (TexRect){0, 0, w, h}; nwin = 1; }
  for (int32_t k = 0; k < nwin; k++) if (rects[k].w * rects[k].h > most) most = rects[k].w * rects[k].h;
  wood_col = grab((uint32_t)(most * 12));
  uint8_t *px = grab((uint32_t)(w * h * 3));
  wood_begin(&t, wood_col, px, w, h, ox, oy, rects, nwin);
  while (!wood_run(&t, 64)) {}
  return px;
}

EXPORT("fern") uint8_t *fern(int32_t w, int32_t h, int32_t iterations, uint32_t seed, int32_t msaa) {
  Fern t;
  uint8_t *px = grab((uint32_t)(w * h * 4));
  fern_begin(&t, px, msaa ? grab((uint32_t)(w * h * 4)) : 0, w, h, iterations, seed);
  while (!fern_run(&t, 1 << 20)) {}
  return px;
}

EXPORT("random") double random_(uint32_t seed, int32_t n) { double v = 0; while (n-- > 0) v = tex_random(&seed); return v; }

/* For the parity test: the trig this module uses, against Chrome's own. */
double cr_cos(double), tex_sin(double), tex_cos(double), tex_tan(double);
EXPORT("cr_cos") double cr_cos_(double x) { return cr_cos(x); }
EXPORT("cos") double cos_(double x) { return tex_cos(x); }
EXPORT("sin") double sin_(double x) { return tex_sin(x); }
EXPORT("tan") double tan_(double x) { return tex_tan(x); }

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

EXPORT("wool") uint8_t *wool(int32_t w, int32_t h, double ox, double oy) {
  Wool t;
  uint8_t *px = grab((uint32_t)(w * h * 4));
  wool_begin(&t, px, w, h, ox, oy);
  while (!wool_run(&t, 1 << 20)) {}
  return px;
}

/* The wood: offsets drawn from the seed as WoodTexture's getRandomOffsets draws them. *col gets the
 * float colours (before the blit) for the finer comparison. */
static float *wood_col;
EXPORT("wood_col") float *get_wood_col(void) { return wood_col; }
EXPORT("wood") uint8_t *wood(int32_t w, int32_t h, uint32_t seed) {
  Wood t;
  double ox = tex_random(&seed) * 1000 - 500, oy = tex_random(&seed) * 1000 - 500;
  wood_col = grab((uint32_t)(w * h * 12));
  float *ifac = grab((uint32_t)(h * 4));
  uint8_t *px = grab((uint32_t)(w * h * 4));
  wood_begin(&t, wood_col, ifac, px, w, h, ox, oy);
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

/* For the parity test's diagnosis: the trig this module uses, to run the TS on it. */
double cr_sin(double), cr_cos(double), js_tan(double);
EXPORT("cos") double cos_(double x) { return cr_cos(x); }
EXPORT("sin") double sin_(double x) { return cr_sin(x); }
EXPORT("tan") double tan_(double x) { return js_tan(x); }

/* textures.h - the app's three textures as RGBA pixels in C memory, each a port of its TS
 * generator, run in steps so a caller can spread one across frames:
 * - the wool weave behind the table (src/utils/woolPixels.ts paintWool);
 * - the wood on the buttons (src/components/WoodTexture.tsx generateWoodPixelData, the path the
 *   app takes: colours kept as float32 and turned into bytes once, as its WebGL blit does);
 * - the fern on the card backs (src/utils/fernFractal.tsx generateFernPoints, the card size).
 * Each is written to match its TS, operation for operation, so the same seed and size give the
 * same pixels (tools/texparity.mjs); README.md, Textures, says where they still differ. */
#ifndef CDOM_POC_TEXTURES_H
#define CDOM_POC_TEXTURES_H
#include <stdint.h>

/* Math.random's stand-in: mulberry32, a double in [0, 1). The parity test gives the TS the same. */
double tex_random(uint32_t *state);

typedef struct {
  uint8_t *px; /* w * h * 4 */
  int32_t w, h;
  double offx, offy, r, u;
  int32_t i, max, switch_at, horizontal;
} Wool;
void wool_begin(Wool *t, uint8_t *px, int32_t w, int32_t h, double offx, double offy);
int wool_run(Wool *t, int32_t steps); /* 1 when the weave is done */

typedef struct {
  float *col; /* w * h * 3, the colours as the TS keeps them */
  float *ifac;
  uint8_t *px; /* w * h * 4, written when done */
  int32_t w, h, column;
  double offx, offy;
} Wood;
void wood_begin(Wood *t, float *col, float *ifac, uint8_t *px, int32_t w, int32_t h, double offx, double offy);
int wood_run(Wood *t, int32_t columns); /* 1 when px holds the texture */

typedef struct {
  uint8_t *px; /* w * h * 4, transparent where no point fell */
  uint8_t *smp; /* w * h * 4 samples (palette index + 1), or 0: plot each point on one pixel */
  int32_t w, h, i, iterations, color;
  uint32_t rng;
  double x, y, scale, cum[6], cos_rot, sin_rot;
} Fern;
/* With smp, the points are drawn as the app's WebGL draw does on this Mac (ANGLE on Metal): into
 * 4 samples per pixel at Metal's standard positions, then resolved, as a multisampled canvas is. */
void fern_begin(Fern *t, uint8_t *px, uint8_t *smp, int32_t w, int32_t h, int32_t iterations, uint32_t seed);
int fern_run(Fern *t, int32_t steps); /* 1 when px holds the fern */

#endif

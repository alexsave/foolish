/* sc_sincosf / sc_sincos: sin and cos of one angle in one call.
 *
 * On wasm that is sincosf / sincos from shared/c/wasm/libm.c (one range
 * reduction, bit-identical to the separate calls). Everywhere else it is the
 * platform's sinf and cosf, which clang already fuses into one call. */
#ifndef SHARED_SINCOS_H
#define SHARED_SINCOS_H

#include <math.h>

#ifdef __wasm__
#define sc_sincosf(x, s, c) sincosf((x), (s), (c))
#define sc_sincos(x, s, c)  sincos((x), (s), (c))
#else
static inline void sc_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
static inline void sc_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
#endif

#endif

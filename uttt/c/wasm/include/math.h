/* Freestanding math for the wasm32 web build (no libc; uttt_web.c).
 *
 * Only what the drawing kernel calls. The easy half are single wasm
 * instructions (sqrt, abs, floor, ceil, min, max) and go straight to clang's
 * builtins; the rest - the trig, exp, log and pow the pen and the motion
 * curves use - are implemented in uttt_web.c. They are close to a libm, not
 * bit-identical to Apple's, which a replay on a web page can afford: the
 * shapes are the same shapes, a hair apart. */
#ifndef UTTT_WASM_MATH_H
#define UTTT_WASM_MATH_H

#define M_PI 3.14159265358979323846

#define sqrtf(x)    __builtin_sqrtf(x)
#define sqrt(x)     __builtin_sqrt(x)
#define fabsf(x)    __builtin_fabsf(x)
#define fabs(x)     __builtin_fabs(x)
#define floorf(x)   __builtin_floorf(x)
#define ceilf(x)    __builtin_ceilf(x)

double sin(double x);
double cos(double x);
double exp(double x);
double log(double x);
double log2(double x);
float  sinf(float x);
float  cosf(float x);
float  acosf(float x);
float  powf(float x, float y);
float  logf(float x);
long   lroundf(float x);
float  fminf(float a, float b);
float  fmaxf(float a, float b);

#endif

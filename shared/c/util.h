/* util.h - the few one-line helpers every product wrote for itself: the length
 * of an array, a clamp, a min and a max in the three types used, and a
 * popcount. Header-only and freestanding (a builtin for the popcount, so a
 * -nostdlib wasm32 build links nothing for it).
 *
 * The clamps do NOT order their bounds: lo > hi returns hi for a value above
 * lo, as the hand-written ones did. A NaN passes through the float clamps
 * unchanged (both comparisons are false), which callers rely on to see it. */
#ifndef SHARED_UTIL_H
#define SHARED_UTIL_H

#include <stdint.h>

/* The element count of an array (not a pointer: the compiler sees a size of
 * 8 / sizeof *p and says nothing, so only use it on a real array). */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static inline int    util_clampi(int v, int lo, int hi)          { return v < lo ? lo : v > hi ? hi : v; }
static inline float  util_clampf(float v, float lo, float hi)    { return v < lo ? lo : v > hi ? hi : v; }
static inline double util_clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int    util_mini(int a, int b)       { return a < b ? a : b; }
static inline int    util_maxi(int a, int b)       { return a > b ? a : b; }
static inline double util_mind(double a, double b) { return a < b ? a : b; }
static inline double util_maxd(double a, double b) { return a > b ? a : b; }

static inline int util_popcount32(uint32_t x) { return __builtin_popcount(x); }
static inline int util_popcount64(uint64_t x) { return __builtin_popcountll(x); }

#endif

/* ease.h - CSS cubic-bezier(x1, y1, x2, y2) evaluated at x, header-only and
 * freestanding (no libm), so a kernel and a wasm build run the same arithmetic.
 *
 * The curve is x(u) = x, y = y(u): the parameter u is solved by Newton from
 * u = x (a fixed eight steps, stopping when |x(u) - x| < 1e-6), then by
 * bisection when Newton stalled on a flat derivative or ran out of steps, as a
 * browser does. x <= 0 gives 0 and x >= 1 gives 1 exactly.
 *
 * Only the solver is shared. Each product keeps its own table of named curves
 * and its own clock (the beat clocks are different on purpose). */
#ifndef SHARED_EASE_H
#define SHARED_EASE_H

static inline float ease_absf(float v) { return v < 0.f ? -v : v; }

static inline float ease_cubic_bezier(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0.f) return 0.f;
    if (x >= 1.f) return 1.f;
    const float cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
    const float cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    float u = x;
    int converged = 0;
    for (int i = 0; i < 8; i++) {
        const float fx = ((ax * u + bx) * u + cx) * u - x;
        const float d  = (3 * ax * u + 2 * bx) * u + cx;
        if (ease_absf(fx) < 1e-6f) { converged = 1; break; }
        if (ease_absf(d) < 1e-6f) break;
        u -= fx / d;
    }
    if (!converged) {
        float lo = 0.f, hi = 1.f;
        u = x;
        for (int i = 0; i < 40; i++) {
            const float fx = ((ax * u + bx) * u + cx) * u;
            if (ease_absf(fx - x) < 1e-6f) break;
            if (fx < x) lo = u; else hi = u;
            u = (lo + hi) * .5f;
        }
    }
    return ((ay * u + by) * u + cy) * u;
}

#endif

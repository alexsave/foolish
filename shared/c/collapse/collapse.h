/* collapse.h - the auto-collapse's push: how far a Messages extension's
 * compact sheet is pushed down while the host's drawer finishes falling.
 *
 * NOT A MEASURED TWEEN. The host moves the drawer on a critically damped
 * spring of its own; COLLAPSE_RESPONSE_MS is that spring's response, read off
 * the host's layer and matched by mean squared error against filmed collapses.
 * Once the collapse flips, the sheet is laid out at the compact height and a
 * keyframe animation on the compositor pushes it down by the drawer's
 * remaining travel, so its bottom edge never moves; this is that push,
 * evaluated over COLLAPSE_MS. How many keyframes sample it, which drop counts
 * as the flip, and what share of the push each element rides back are the
 * product's; this header is only the curve and its two numbers.
 *
 * Header-only and freestanding (exp from <math.h>, which a wasm32 build gets
 * from shared/c/wasm). A product includes it by a relative path; Swift reaches
 * the constants through this directory's module.modulemap (CCollapse). */
#ifndef SHARED_COLLAPSE_H
#define SHARED_COLLAPSE_H

#include <math.h>
#include <stdint.h>

#define COLLAPSE_RESPONSE_MS 338   /* the host's drawer spring, critically damped */
#define COLLAPSE_MS          600   /* the length of the push */

/* How far the compact sheet is pushed down `t_ms` into a slide of `travel`
 * points: the whole travel at or before 0, falling on the host's curve, and
 * exactly 0 from COLLAPSE_MS on. */
static inline float collapse_push(float travel, int32_t t_ms)
{
    if (t_ms <= 0) return travel;
    if (t_ms >= COLLAPSE_MS) return 0.f;
    double w = 2.0 * 3.14159265358979 / COLLAPSE_RESPONSE_MS, t = (double)t_ms;
    double left = (1.0 + w * t) * exp(-w * t);      /* 1 - the host's progress */
    /* THE LAST KEYFRAME IS EXACTLY ZERO, and the curve reaches it without a
     * step: what the spring still has left at the end (under 0.3%) is faded
     * out linearly over the slide, so removing the animation moves nothing. */
    double tail = (1.0 + w * COLLAPSE_MS) * exp(-w * COLLAPSE_MS);
    return (float)(travel * (left - tail * t / COLLAPSE_MS));
}

#endif

/* rating.c - see rating.h. */
#include "rating.h"

#include <math.h>

int cc_speed(int base_s, int inc_s) {
    int est = base_s + 40 * inc_s;
    return est < 180 ? CC_BULLET : est < 480 ? CC_BLITZ : est < 1500 ? CC_RAPID : CC_CLASSICAL;
}

g2_player cc_rating_new(void) { return (g2_player){ CC_RATING_START, CC_RD_START, CC_VOL_START }; }

static g2_player clamp(g2_player p) {
    if (p.deviation < CC_RD_MIN) p.deviation = CC_RD_MIN;
    if (p.deviation > CC_RD_MAX) p.deviation = CC_RD_MAX;
    if (p.volatility > CC_VOL_MAX) p.volatility = CC_VOL_MAX;
    if (p.rating < 100) p.rating = 100;
    return p;
}

void cc_rate(g2_player *white, g2_player *black, double white_score) {
    g2_result rw = { *black, white_score }, rb = { *white, 1.0 - white_score };
    g2_player w = g2_update(*white, &rw, 1, CC_TAU, 1e-6);
    g2_player b = g2_update(*black, &rb, 1, CC_TAU, 1e-6);
    *white = clamp(w);
    *black = clamp(b);
}

int cc_rating_int(const g2_player *p) { return (int)lround(p->rating); }

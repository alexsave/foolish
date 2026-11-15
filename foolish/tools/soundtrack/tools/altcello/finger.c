#include "finger.h"
#include <math.h>

void finger_init(Finger *f)
{
    f->width = 0.010;
    f->K = 2e7;
    f->R = 200.0;
}

void finger_weights(const Finger *f, const String *s, double xF, int *lo, int *hi, double *g)
{
    double hw = 0.5 * f->width;
    int l0 = (int)ceil((xF - hw) / s->h), l1 = (int)floor((xF + hw) / s->h);
    if (l0 < 1) l0 = 1;
    if (l1 > s->N - 1) l1 = s->N - 1;
    if (l1 - l0 + 1 > 64) l1 = l0 + 63;
    *lo = l0;
    *hi = l1;
    for (int l = l0; l <= l1; l++) {
        double d = (l * s->h - xF) / hw; /* -1..1 */
        g[l - l0] = (fabs(d) < 1.0) ? 0.5 * (1.0 + cos(M_PI * d)) : 0.0;
    }
}

void finger_apply(const Finger *f, String *s, double xF, double a)
{
    int lo, hi;
    double g[64];
    if (a <= 0.0) {
        string_set_finger(s, 1, 0, NULL, 0.0, f->K, f->R);
        return;
    }
    finger_weights(f, s, xF, &lo, &hi, g);
    string_set_finger(s, lo, hi, g, a > 1.0 ? 1.0 : a, f->K, f->R);
}

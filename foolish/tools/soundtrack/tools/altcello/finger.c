#include "finger.h"
#include <math.h>

void finger_init(Finger *f)
{
    f->width = 0.010;
    f->K = 2e7;
    f->R = 200.0;
}

void finger_weights(const Finger *f, double h, int N, double xF, int *lo, int *hi, double *g)
{
    double hw = 0.5 * f->width;
    int l0 = (int)ceil((xF - hw) / h), l1 = (int)floor((xF + hw) / h);
    if (l0 < 1) l0 = 1;
    if (l1 > N - 1) l1 = N - 1;
    if (l1 - l0 + 1 > 64) l1 = l0 + 63;
    *lo = l0;
    *hi = l1;
    for (int l = l0; l <= l1; l++) {
        double d = (l * h - xF) / hw; /* -1..1 */
        g[l - l0] = (fabs(d) < 1.0) ? 0.5 * (1.0 + cos(M_PI * d)) : 0.0;
    }
}

void finger_apply(const Finger *f, String *s, double xF, double a)
{
    int lo, hi;
    double g[64];
    if (a <= 0.0) {
        string_set_finger(s, 1, 0, NULL, 0.0, f->K, f->R);
        torsion_set_finger(s->tor, 1, 0, NULL, 0.0, f->K, f->R);
        return;
    }
    if (a > 1.0) a = 1.0;
    finger_weights(f, s->h, s->N, xF, &lo, &hi, g);
    string_set_finger(s, lo, hi, g, a, f->K, f->R);
    /* the torsion grid can be as coarse as the pad (h_t about 10 mm): widen the pad to two grid steps
       so it always covers points, and scale the weights by the width ratio so the lumped pad
       stiffness K * integral(g dx) is the same as on the transverse grid */
    Finger ft = *f;
    if (ft.width < 2.0 * s->tor->h) ft.width = 2.0 * s->tor->h;
    finger_weights(&ft, s->tor->h, s->tor->N, xF, &lo, &hi, g);
    for (int l = lo; l <= hi; l++) g[l - lo] *= f->width / ft.width;
    torsion_set_finger(s->tor, lo, hi, g, a, f->K, f->R);
}

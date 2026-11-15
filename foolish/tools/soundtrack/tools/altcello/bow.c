/* Bow friction.
 *
 * Per sample the bow force on the string is -F phi(v_rel) J_l (u polarisation),
 * v_rel = sum_l J_l h (u2_l - u0_l)/(2k) - vB, and u2_l = ufree_l - A_l J_l F phi
 * with A_l = k^2 / (rho D_l) (D_l = the implicit diagonal of altstring.c). Hence
 *   v_rel = q - G phi(v_rel),
 *   q = sum J h (ufree - u0)/(2k) - vB,   G = F sum_l h A_l J_l^2 / (2k).
 * Friedlander construction, for q > 0 (q < 0 by oddness: solve for |v|, flip):
 *   g(w) = w + G phi(w) - |q|, w > 0, g is convex (phi is convex, decreasing).
 *   - |q| > alpha G (alpha = phi(0+) = 1.2): g(0+) < 0 and g(|q|) > 0, so
 *     exactly one slipping root; Newton from the right of the root is monotone.
 *   - |q| <= alpha G: sticking is possible (v = 0, phi = q/G, force in the
 *     static cone). Slipping roots may also exist (g(0+) >= 0 but g dips
 *     below zero). Hysteresis, the point of the construction: if the
 *     previous step was sticking, stay stuck. If it was slipping, continue
 *     on the stable slipping branch (the larger root, g' > 0, the one reached
 *     by Newton from the right) when it exists, else stick.
 */
#include "bow.h"
#include <math.h>

double bow_phi(double v)
{
    double a = fabs(v);
    double p = 0.4 * exp(-a / 0.01) + 0.45 * exp(-a / 0.1) + 0.35;
    return v < 0.0 ? -p : p;
}

double bow_dphi(double v)
{
    double a = fabs(v);
    return -40.0 * exp(-a / 0.01) - 4.5 * exp(-a / 0.1);
}

void bow_weights(const String *s, double xB, double width, int *lo, int *hi, double *J)
{
    double hw = 0.5 * width;
    int l0 = (int)ceil((xB - hw) / s->h), l1 = (int)floor((xB + hw) / s->h);
    if (l0 < 1) l0 = 1;
    if (l1 > s->N - 1) l1 = s->N - 1;
    if (l1 - l0 + 1 > BOW_MAXPTS) l1 = l0 + BOW_MAXPTS - 1;
    double sum = 0.0;
    for (int l = l0; l <= l1; l++) {
        double d = (l * s->h - xB) / hw;
        double w = (fabs(d) < 1.0) ? 0.5 * (1.0 + cos(M_PI * d)) : 0.0;
        J[l - l0] = w;
        sum += w;
    }
    if (sum <= 0.0) { /* bow narrower than the grid: nearest point */
        l0 = l1 = (int)floor(xB / s->h + 0.5);
        J[0] = 1.0;
        sum = 1.0;
    }
    for (int l = l0; l <= l1; l++) J[l - l0] /= sum * s->h;
    *lo = l0;
    *hi = l1;
}

/* largest root of g(w) = w + G phi(w) - aq, Newton from the right; 0 if none */
static int largest_root(double aq, double G, double *w)
{
    double x = aq;
    for (int it = 0; it < 100; it++) {
        double p = bow_phi(x);
        double g = x + G * p - aq;
        double gp = 1.0 + G * bow_dphi(x);
        if (gp <= 1e-12) return 0;
        double xn = x - g / gp;
        if (xn <= 0.0) return 0;
        if (fabs(xn - x) < 1e-15 * (1.0 + x)) {
            x = xn;
            /* a Newton fixed point with g' > 0 is a root */
            *w = x;
            return 1;
        }
        x = xn;
    }
    return 0;
}

double bow_solve(double q, double G, double vprev, int stuck_prev, double *Phi, int *stuck)
{
    (void)vprev;
    if (G < 1e-300) {
        *Phi = 0.0;
        *stuck = 0;
        return q;
    }
    double aq = fabs(q), sg = q < 0.0 ? -1.0 : 1.0, w;
    if (aq <= BOW_PHI_STATIC * G) {
        if (stuck_prev || !largest_root(aq, G, &w)) {
            *Phi = q / G;
            *stuck = 1;
            return 0.0;
        }
    } else if (!largest_root(aq, G, &w)) {
        /* cannot happen (unique root); safe bisection on [0, aq] */
        double lo = 0.0, hi = aq;
        for (int i = 0; i < 200; i++) {
            double m = 0.5 * (lo + hi);
            if (m + G * bow_phi(m) - aq < 0.0) lo = m; else hi = m;
        }
        w = 0.5 * (lo + hi);
    }
    *stuck = 0;
    *Phi = sg * bow_phi(w);
    return sg * w;
}

void bow_noise_init(BowNoise *b, double level, double fs)
{
    b->level = level;
    b->lp_a = exp(-2.0 * M_PI * 3000.0 / fs);
    b->lp_y = 0.0;
    b->x_prev = b->x_next = 0.0;
    b->idx_next = 0;
    b->started = 0;
    b->nlog = 0;
}

static double draw(BowNoise *b, Rng *r, long idx, double now)
{
    double x = b->level * rng_gauss(r);
    if (b->nlog < 1000) {
        b->log_t[b->nlog] = (double)idx / BOW_NOISE_RATE;
        b->log_x[b->nlog] = x;
        b->log_now[b->nlog] = now;
        b->nlog++;
    }
    return x;
}

double bow_noise_next(BowNoise *b, Rng *r, long n, double fs)
{
    if (b->level <= 0.0) return 0.0;
    long ifs = (long)(fs + 0.5);
    if (!b->started) {
        b->x_prev = draw(b, r, 0, 0.0);
        b->x_next = draw(b, r, 1, 0.0);
        b->idx_next = 1;
        b->started = 1;
    }
    /* advance while sample n is at or beyond draw idx_next:
       n/fs >= idx_next/RATE  <=>  n*RATE >= idx_next*fs */
    while (n * (long)BOW_NOISE_RATE >= b->idx_next * ifs) {
        b->x_prev = b->x_next;
        b->idx_next++;
        b->x_next = draw(b, r, b->idx_next, (double)n / fs);
    }
    double frac = ((double)n * BOW_NOISE_RATE - (double)(b->idx_next - 1) * ifs) / (double)ifs;
    double x = b->x_prev + frac * (b->x_next - b->x_prev);
    b->lp_y = b->lp_a * b->lp_y + (1.0 - b->lp_a) * x;
    return b->lp_y;
}

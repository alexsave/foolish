/* Bow friction.
 *
 * Per sample the bow force on the string is -F phi(v_rel) J_l (u polarisation),
 * v_rel = v_contact - vB, where the contact velocity is the one of the string
 * SURFACE under the bow hair:
 *   v_contact = sum_l J_l h (u2_l - u0_l)/(2k) + r_s sum_m Jt_m ht (th2_m - th0_m)/(2k)
 * (the transverse velocity plus r_s times the torsional angular velocity, torsion.h).
 * The same force acts on u (force density -F phi J_l) and as the torque density
 * -r_s F phi Jt_m on theta. With the free updates ufree, thfree:
 *   u2_l  = ufree_l  - A_l  J_l  F phi,   A_l = k^2 / (rho D_l)  (D_l = the implicit diagonal of altstring.c)
 *   th2_m = thfree_m - r_s B_m Jt_m F phi, B_m = k^2 / (I_p Dt_m) (torsion.c)
 * so the contact velocity depends on phi linearly, with the SUM of the transverse and the torsional
 * one-step admittances, and the equation stays scalar:
 *   v_rel = q - G phi(v_rel),
 *   q = sum J h (ufree - u0)/(2k) + r_s sum Jt ht (thfree - th0)/(2k) - vB,
 *   G = F [ sum_l h A_l J_l^2 + r_s^2 sum_m ht B_m Jt_m^2 ] / (2k)  =  G_transverse + G_torsion.
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

double bow_phi(const Friction *fr, double v)
{
    double a = fabs(v);
    double p = fr->a1 * exp(-a / fr->v1) + fr->a2 * exp(-a / fr->v2) + fr->c;
    return v < 0.0 ? -p : p;
}

double bow_dphi(const Friction *fr, double v)
{
    double a = fabs(v);
    return -(fr->a1 / fr->v1) * exp(-a / fr->v1) - (fr->a2 / fr->v2) * exp(-a / fr->v2);
}

void bow_weights(const String *s, double xB, double width, int *lo, int *hi, double *J)
{
    bow_weights_grid(s->h, s->N, xB, width, lo, hi, J);
}

void bow_weights_grid(double h, int N, double xB, double width, int *lo, int *hi, double *J)
{
    double hw = 0.5 * width;
    int l0 = (int)ceil((xB - hw) / h), l1 = (int)floor((xB + hw) / h);
    if (l0 < 1) l0 = 1;
    if (l1 > N - 1) l1 = N - 1;
    if (l1 - l0 + 1 > BOW_MAXPTS) l1 = l0 + BOW_MAXPTS - 1;
    double sum = 0.0;
    for (int l = l0; l <= l1; l++) {
        double d = (l * h - xB) / hw;
        double w = (fabs(d) < 1.0) ? 0.5 * (1.0 + cos(M_PI * d)) : 0.0;
        J[l - l0] = w;
        sum += w;
    }
    if (sum <= 0.0) { /* bow narrower than the grid: nearest point */
        l0 = l1 = (int)floor(xB / h + 0.5);
        J[0] = 1.0;
        sum = 1.0;
    }
    for (int l = l0; l <= l1; l++) J[l - l0] /= sum * h;
    *lo = l0;
    *hi = l1;
}

/* largest root of g(w) = w + G phi(w) - aq, Newton from the right; 0 if none */
static int largest_root(const Friction *fr, double aq, double G, double *w)
{
    double x = aq;
    for (int it = 0; it < 100; it++) {
        double p = bow_phi(fr, x);
        double g = x + G * p - aq;
        double gp = 1.0 + G * bow_dphi(fr, x);
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

double bow_solve(const Friction *fr, double q, double G, double vprev, int stuck_prev, double *Phi, int *stuck)
{
    (void)vprev;
    if (G < 1e-300) {
        *Phi = 0.0;
        *stuck = 0;
        return q;
    }
    double aq = fabs(q), sg = q < 0.0 ? -1.0 : 1.0, w;
    if (aq <= fr->mu_static * G) {
        if (stuck_prev || !largest_root(fr, aq, G, &w)) {
            *Phi = q / G;
            *stuck = 1;
            return 0.0;
        }
    } else if (!largest_root(fr, aq, G, &w)) {
        /* cannot happen (unique root); safe bisection on [0, aq] */
        double lo = 0.0, hi = aq;
        for (int i = 0; i < 200; i++) {
            double m = 0.5 * (lo + hi);
            if (m + G * bow_phi(fr, m) - aq < 0.0) lo = m; else hi = m;
        }
        w = 0.5 * (lo + hi);
    }
    *stuck = 0;
    *Phi = sg * bow_phi(fr, w);
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

void hair_init(Hair *h, double kh, double dt)
{
    h->on = 1;
    h->m = HAIR_MASS;
    h->k = kh;
    h->r = 2.0 * HAIR_ZETA * sqrt(kh * HAIR_MASS);
    double d = h->r * dt / (2.0 * h->m);
    h->A = (2.0 - dt * dt * kh / h->m) / (1.0 + d);
    h->B = -(1.0 - d) / (1.0 + d);
    h->g = dt * dt / (h->m * (1.0 + d));
    hair_reset(h);
}

void hair_reset(Hair *h)
{
    h->d0 = h->d1 = h->d2 = h->dfree = h->dprev = 0.0;
}

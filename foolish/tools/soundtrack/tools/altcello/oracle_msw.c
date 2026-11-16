/* ORACLE: the McIntyre-Schumacher-Woodhouse / Smith digital waveguide bowed string (see oracle_msw.h).
 *
 * Wave picture. Velocity waves on an ideal string, wave impedance Z0 = sqrt(T rho) = T/c. At the bow junction let a1 be the wave
 * arriving from the nut side (travelling toward the bridge) and a2 the wave arriving from the bridge side (travelling toward the
 * nut); b1, b2 are the waves leaving toward the bridge and the nut. Continuity of velocity on each side: v = a1 + b2 = a2 + b1. Force balance
 * on the massless junction (slopes y_x = -v/c for a right going and +v/c for a left going wave, tension force T [y_x]):
 *     Z0 (a1 + a2 - b1 - b2) + f_s = 0   =>   v = a1 + a2 + f_s / (2 Z0), b1 = v - a2, b2 = v - a1,
 * with f_s the transverse force the bow applies to the string: f_s = -F_B phi(v - v_B) (friction opposes the relative motion),
 * the same normalisation as sim.c / bow.c. So with v_rel = v - v_B:   v_rel = q - G phi(v_rel),  q = a1 + a2 - v_B (+ fext/(2 Z0)),
 * G = F_B / (2 Z0). The solve below is written independently of bow.c's Newton iteration: it finds the minimum of
 * g(w) = w + G phi(w) - |q| by bisection on g' and then the largest root by bisection (Friedlander construction), and keeps the
 * previous branch (stick) while the static cone |q| <= mu_s G holds.
 *
 * Loop. Bridge side: b1 -> delay -> (arrives at the bridge: force 2 Z0 c) -> reflection -H(z) -> delay -> a2. Nut side likewise
 * with b2 -> a1. Delay budget per side: the side's round trip time is beta N0 (bridge side) or (1 - beta) N0 (nut side) samples,
 * N0 = fs / f0; of that the loss filter takes its phase delay at f0 and the junction ordering one sample, the rest is shared by
 * the two lines (Lagrange 3rd order, fraction in [1, 2) of the four taps), so the fundamental sits exactly at f0. */
#include "oracle_msw.h"
#include "altstring.h"
#include "analysis.h"
#include "bow.h"
#include "sim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double *buf;
    long mask, idx;
    long d0;
    double h[4];
} DL;

static void dl_init(DL *d, double delay)
{
    long sz = 1;
    while (sz < (long)delay + 16) sz <<= 1;
    d->buf = calloc((size_t)sz, sizeof(double));
    d->mask = sz - 1;
    d->idx = 0;
    d->d0 = (long)floor(delay) - 1;
    if (d->d0 < 0) abort(); /* delay below one sample: the bow is too close to an end for this f0 and fs */
    double u = delay - (double)d->d0; /* in [1, 2) */
    for (int j = 0; j < 4; j++) {
        double w = 1.0;
        for (int m = 0; m < 4; m++)
            if (m != j) w *= (u - m) / (double)(j - m);
        d->h[j] = w;
    }
}

static void dl_push(DL *d, double x)
{
    d->idx++;
    d->buf[d->idx & d->mask] = x;
}

/* value delayed by the fixed delay of this line, relative to the newest pushed sample */
static double dl_tap(const DL *d)
{
    double y = 0.0;
    for (int j = 0; j < 4; j++) y += d->h[j] * d->buf[(d->idx - d->d0 - j) & d->mask];
    return y;
}

struct Oracle {
    OracleSpec s;
    DL A1, A2, B1, B2;
    double ya, yb; /* loss filter states (bridge, nut) */
    double gm;     /* g (1 - p) */
    int stuck;
    Thermal T; /* thermal elasto-plastic contact (OracleSpec.thermal) */
};

double oracle_H_mag(double g, double p, double w)
{
    return g * (1.0 - p) / sqrt(1.0 - 2.0 * p * cos(w) + p * p);
}

double oracle_H_phase_delay(double p, double w)
{
    if (w < 1e-9) return p / (1.0 - p);
    return atan2(p * sin(w), 1.0 - p * cos(w)) / w;
}

double oracle_alpha_model(double g, double p, double f0, double fs, double w)
{
    (void)fs;
    return -2.0 * f0 * log(oracle_H_mag(g, p, w));
}

Oracle *oracle_new(const OracleSpec *s)
{
    Oracle *o = calloc(1, sizeof *o);
    o->s = *s;
    double N0 = s->fs / s->f0;
    double w0 = 2.0 * M_PI * s->f0 / s->fs;
    double tau = oracle_H_phase_delay(s->p, w0);
    double dA = (s->beta * N0 - tau - 1.0) / 2.0, dB = ((1.0 - s->beta) * N0 - tau - 1.0) / 2.0;
    dl_init(&o->A1, dA);
    dl_init(&o->A2, dA);
    dl_init(&o->B1, dB);
    dl_init(&o->B2, dB);
    o->gm = s->g * (1.0 - s->p);
    if (s->thermal) thermal_init(&o->T, &s->th, s->bow_width, 1.0 / s->fs);
    analysis_slip_thr_for(s->thermal);
    return o;
}

void oracle_free(Oracle *o)
{
    if (!o) return;
    free(o->A1.buf);
    free(o->A2.buf);
    free(o->B1.buf);
    free(o->B2.buf);
    free(o);
}

/* the largest root of g(w) = w + G phi(w) - a, w > 0, a > 0; returns 1 and *w if there is one */
static int slip_root(const Friction *fr, double a, double G, double *w)
{
    double lo, hi = a;
    double gp0 = 1.0 + G * bow_dphi(fr, 0.0);
    if (gp0 >= 0.0) {
        lo = 0.0; /* g increases from g(0+) = G mu_s - a: a root exists only if that is negative */
        if (G * bow_phi(fr, 0.0) - a >= 0.0) return 0;
    } else {
        /* minimum of g: g'(w) = 1 + G dphi(w) is increasing (phi convex); bisect its zero */
        double wl = 0.0, wh = fr->v1;
        while (1.0 + G * bow_dphi(fr, wh) < 0.0) wh *= 2.0;
        for (int i = 0; i < 100; i++) {
            double m = 0.5 * (wl + wh);
            if (1.0 + G * bow_dphi(fr, m) < 0.0) wl = m; else wh = m;
        }
        lo = 0.5 * (wl + wh);
        if (lo + G * bow_phi(fr, lo) - a >= 0.0) return 0; /* the dip does not reach zero: no slipping root */
        if (lo >= a) return 0;
    }
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (m + G * bow_phi(fr, m) - a < 0.0) lo = m; else hi = m;
    }
    *w = 0.5 * (lo + hi);
    return 1;
}

double oracle_junction(const Friction *fr, double q, double G, int *stuck, double *Phi)
{
    if (G < 1e-300) {
        *Phi = 0.0;
        *stuck = 0;
        return q;
    }
    double a = fabs(q), sg = q < 0.0 ? -1.0 : 1.0, w = 0.0;
    int have = 0;
    if (a <= fr->mu_static * G) {
        if (*stuck || !(have = slip_root(fr, a, G, &w))) {
            *Phi = q / G;
            *stuck = 1;
            return 0.0;
        }
    } else {
        have = slip_root(fr, a, G, &w);
        if (!have) w = a; /* cannot happen: g(0+) < 0 and g(a) > 0 */
    }
    *stuck = 0;
    *Phi = sg * bow_phi(fr, w);
    return sg * w;
}

void oracle_step(Oracle *o, double vB, double FB, double fext, double *vrel, double *bridge)
{
    const OracleSpec *s = &o->s;
    double a1 = dl_tap(&o->B2), a2 = dl_tap(&o->A2);
    double q = a1 + a2 + fext / (2.0 * s->Z0) - vB;
    double G = FB / (2.0 * s->Z0), Phi;
    double vr = s->thermal ? thermal_solve(&o->T, FB, q, G, 0.0, &Phi) : oracle_junction(&s->fr, q, G, &o->stuck, &Phi);
    double v = vr + vB;
    double b1 = v - a2, b2 = v - a1; /* the wave that leaves toward the bridge is the continuation of a1 plus the bow's part */
    dl_push(&o->A1, b1);
    dl_push(&o->B1, b2);
    double c = dl_tap(&o->A1), e = dl_tap(&o->B1); /* waves arriving at the bridge and at the nut */
    o->ya = o->gm * (-c) + s->p * o->ya;
    o->yb = o->gm * (-e) + s->p * o->yb;
    dl_push(&o->A2, o->ya);
    dl_push(&o->B2, o->yb);
    *vrel = vr;
    *bridge = -2.0 * s->Z0 * c;
}

double oracle_Z0(const StringSpec *sp) { return sqrt(sp->T * string_rho(sp)); }

double oracle_fit_loss(const double *alpha, int K, double f0, double fs, double *g, double *p)
{
    double bestp = 0.0, bestc = 0.0, beste = 1e300;
    for (int pass = 0; pass < 2; pass++) {
        double p0 = pass == 0 ? 0.0 : bestp - 0.005, p1 = pass == 0 ? 0.985 : bestp + 0.005, dp = pass == 0 ? 0.005 : 0.00005;
        for (double pp = p0; pp <= p1; pp += dp) {
            if (pp < 0.0) continue;
            double num = 0.0, den = 0.0, b[ORACLE_KFIT_MAX];
            for (int k = 1; k <= K; k++) {
                double w = 2.0 * M_PI * k * f0 / fs;
                b[k - 1] = -2.0 * f0 * log((1.0 - pp) / sqrt(1.0 - 2.0 * pp * cos(w) + pp * pp)); /* the shape without g */
                num += (alpha[k - 1] - b[k - 1]) / (alpha[k - 1] * alpha[k - 1]);
                den += 1.0 / (alpha[k - 1] * alpha[k - 1]);
            }
            double c = num / den;
            if (c < 0.0) c = 0.0; /* g <= 1 */
            double e = 0.0;
            for (int k = 1; k <= K; k++) {
                double r = (c + b[k - 1]) / alpha[k - 1] - 1.0;
                e += r * r;
            }
            if (e < beste) { beste = e; bestp = pp; bestc = c; }
        }
    }
    *p = bestp;
    *g = exp(-bestc / (2.0 * f0));
    return sqrt(beste / K);
}

double oracle_spec_from_string(const Instrument *I, const StringSpec *sp, double fs, double beta, double fit_fmax, OracleSpec *out,
                               double *alpha_out, int *K_out)
{
    Sim S;
    sim_init(&S, I, sp, fs);
    S.bow_on = 0;
    sim_start(&S);
    sim_pluck(&S, 0.13 * sp->L, 0.02 * sp->L, 1e-3);
    Trace *tr = sim_run(&S, 3.0, NULL, NULL);
    int K = (int)(fit_fmax / sp->f0);
    if (K > ORACLE_KFIT_MAX) K = ORACLE_KFIT_MAX;
    double f[ORACLE_KFIT_MAX], alpha[ORACLE_KFIT_MAX];
    partial_decays(tr->ch[0], tr->n, fs, sp->f0, K, 0.05, sp->f0 > 100.0 ? 0.04 : 0.08, 40.0, f, alpha);
    int Kg = 0;
    while (Kg < K && alpha[Kg] > 0.0) Kg++; /* contiguous run of measurable partials */
    trace_free(tr);
    sim_free(&S);
    out->fs = fs;
    out->f0 = sp->f0;
    out->Z0 = oracle_Z0(sp);
    out->beta = beta;
    out->fr = I->bow.friction;
    out->thermal = I->bow.model == BOW_THERMAL;
    out->th = I->bow.thermal;
    out->bow_width = I->bow.width;
    double err = oracle_fit_loss(alpha, Kg, sp->f0, fs, &out->g, &out->p);
    if (alpha_out) memcpy(alpha_out, alpha, sizeof(double) * (size_t)Kg);
    if (K_out) *K_out = Kg;
    return err;
}

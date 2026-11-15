#include "torsion.h"
#include <math.h>
#include <stdlib.h>

Torsion *torsion_new(const StringSpec *sp, double fs)
{
    Torsion *t = calloc(1, sizeof *t);
    t->fs = fs;
    t->k = 1.0 / fs;
    t->L = sp->L;
    t->ct = string_ct(sp);
    t->Ip = string_Ip(sp);
    t->rs = sp->r;
    t->kk = t->k * t->k / t->Ip;
    /* sigma_t = zeta_t w_t1, w_t1 = pi c_t / L */
    t->sigma = sp->zeta_t * M_PI * t->ct / sp->L;
    t->N = (int)floor(sp->L / (t->ct * t->k)); /* h = L/N >= c_t k: Courant number <= 1 */
    if (t->N < 4) t->N = 4;
    t->h = sp->L / t->N;
    size_t len = (size_t)t->N + 1;
    t->th0 = calloc(len, sizeof(double));
    t->th1 = calloc(len, sizeof(double));
    t->th2 = calloc(len, sizeof(double));
    t->invD = calloc(len, sizeof(double));
    t->C = calloc(len, sizeof(double));
    for (int l = 0; l <= t->N; l++) t->invD[l] = 1.0 / (1.0 + t->sigma * t->k);
    t->fing_lo = 1;
    t->fing_hi = 0;
    return t;
}

void torsion_free(Torsion *t)
{
    if (!t) return;
    free(t->th0);
    free(t->th1);
    free(t->th2);
    free(t->invD);
    free(t->C);
    free(t);
}

void torsion_set_finger(Torsion *t, int lo, int hi, const double *g, double a, double K, double R)
{
    double base = 1.0 / (1.0 + t->sigma * t->k);
    for (int l = t->fing_lo; l <= t->fing_hi; l++) {
        t->invD[l] = base;
        t->C[l] = 0.0;
    }
    t->fing_lo = 1;
    t->fing_hi = 0;
    if (lo > hi || a <= 0.0) return;
    if (lo < 1) lo = 1;
    if (hi > t->N - 1) hi = t->N - 1;
    double Kt = K * t->rs * t->rs, Rt = R * t->rs * t->rs;
    for (int l = lo; l <= hi; l++) {
        double gam = t->kk * a * g[l - lo];
        t->invD[l] = 1.0 / (1.0 + t->sigma * t->k + gam * (0.5 * Kt + Rt / (2.0 * t->k)));
        t->C[l] = gam * (0.5 * Kt - Rt / (2.0 * t->k));
    }
    t->fing_lo = lo;
    t->fing_hi = hi;
}

void torsion_step_free(Torsion *t)
{
    double ih2 = 1.0 / (t->h * t->h), k = t->k;
    double a1 = t->ct * t->ct * k * k;
    double a4 = 1.0 - t->sigma * k;
    const double *y1 = t->th1, *y0 = t->th0;
    double *y2 = t->th2;
    for (int l = 1; l < t->N; l++) {
        double d1 = (y1[l + 1] - 2.0 * y1[l] + y1[l - 1]) * ih2;
        y2[l] = (2.0 * y1[l] + a1 * d1 - a4 * y0[l] - t->C[l] * y0[l]) * t->invD[l];
    }
    y2[0] = 0.0;
    y2[t->N] = 0.0;
}

void torsion_commit(Torsion *t)
{
    double *p = t->th0;
    t->th0 = t->th1;
    t->th1 = t->th2;
    t->th2 = p;
}

double torsion_energy(const Torsion *t)
{
    double h = t->h, k = t->k, kin = 0.0, pot = 0.0;
    for (int l = 1; l < t->N; l++) {
        double v = (t->th1[l] - t->th0[l]) / k;
        kin += v * v;
    }
    for (int l = 0; l < t->N; l++) pot += (t->th1[l + 1] - t->th1[l]) * (t->th0[l + 1] - t->th0[l]) / (h * h);
    return h * (0.5 * kin + 0.5 * t->ct * t->ct * pot);
}

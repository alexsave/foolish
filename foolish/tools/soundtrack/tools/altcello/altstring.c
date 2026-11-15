#include "string.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PAD 2

double string_h_min(const StringSpec *sp, double fs)
{
    double k = 1.0 / fs;
    double rho = string_rho(sp);
    double c2 = sp->T / rho;
    double kap2 = sp->E * string_I(sp) / rho;
    double a = c2 * k * k + 4.0 * sp->sigma1 * k;
    return sqrt((a + sqrt(a * a + 16.0 * kap2 * k * k)) / 2.0);
}

String *string_new(const StringSpec *sp, double fs)
{
    String *s = calloc(1, sizeof *s);
    s->sp = *sp;
    s->fs = fs;
    s->k = 1.0 / fs;
    s->rho = string_rho(sp);
    s->c2 = sp->T / s->rho;
    s->EI = sp->E * string_I(sp);
    s->kap2 = s->EI / s->rho;
    s->s0 = sp->sigma0;
    s->s1 = sp->sigma1;
    double hmin = string_h_min(sp, fs);
    s->N = (int)floor(sp->L / hmin);
    s->h = sp->L / s->N;
    s->kk = s->k * s->k / s->rho;
    int len = s->N + 1 + 2 * PAD;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 3; i++) s->store[p][i] = calloc((size_t)len, sizeof(double));
    for (int p = 0; p < 2; p++) {
        s->y0[p] = s->store[p][0] + PAD;
        s->y1[p] = s->store[p][1] + PAD;
        s->y2[p] = s->store[p][2] + PAD;
    }
    s->invD = calloc((size_t)s->N + 1, sizeof(double));
    s->C = calloc((size_t)s->N + 1, sizeof(double));
    for (int l = 0; l <= s->N; l++) s->invD[l] = 1.0 / (1.0 + s->s0 * s->k);
    s->fing_lo = 1;
    s->fing_hi = 0;
    return s;
}

void string_free(String *s)
{
    if (!s) return;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 3; i++) free(s->store[p][i]);
    free(s->invD);
    free(s->C);
    free(s);
}

void string_set_finger(String *s, int lo, int hi, const double *g, double a, double K, double R)
{
    double base = 1.0 / (1.0 + s->s0 * s->k);
    for (int l = s->fing_lo; l <= s->fing_hi; l++) {
        s->invD[l] = base;
        s->C[l] = 0.0;
    }
    s->fing_lo = 1;
    s->fing_hi = 0;
    if (lo > hi || a <= 0.0) return;
    if (lo < 1) lo = 1;
    if (hi > s->N - 1) hi = s->N - 1;
    for (int l = lo; l <= hi; l++) {
        double gam = s->kk * a * g[l - lo];
        s->invD[l] = 1.0 / (1.0 + s->s0 * s->k + gam * (0.5 * K + R / (2.0 * s->k)));
        s->C[l] = gam * (0.5 * K - R / (2.0 * s->k));
    }
    s->fing_lo = lo;
    s->fing_hi = hi;
}

void string_ghosts(String *s, int pol, double *y)
{
    int N = s->N;
    (void)pol;
    y[0] = 0.0;
    y[N] = 0.0;
    y[-1] = -y[1];
    y[-2] = -y[2];
    y[N + 1] = -y[N - 1];
    y[N + 2] = -y[N - 2];
}

void string_step_free(String *s, int pol, const double *fext)
{
    const double *y1 = s->y1[pol], *y0 = s->y0[pol];
    double *y2 = s->y2[pol];
    double ih2 = 1.0 / (s->h * s->h), ih4 = ih2 * ih2;
    double k = s->k;
    double a1 = s->c2 * k * k + 2.0 * s->s1 * k;
    double a2 = s->kap2 * k * k;
    double a3 = 2.0 * s->s1 * k;
    double a4 = 1.0 - s->s0 * k;
    for (int l = 1; l < s->N; l++) {
        double d1 = (y1[l + 1] - 2.0 * y1[l] + y1[l - 1]) * ih2;
        double d0 = (y0[l + 1] - 2.0 * y0[l] + y0[l - 1]) * ih2;
        double d4 = (y1[l + 2] - 4.0 * y1[l + 1] + 6.0 * y1[l] - 4.0 * y1[l - 1] + y1[l - 2]) * ih4;
        double S = 2.0 * y1[l] + a1 * d1 - a2 * d4 - a4 * y0[l] - a3 * d0 - s->C[l] * y0[l];
        if (fext) S += s->kk * fext[l];
        y2[l] = S * s->invD[l];
    }
    y2[0] = 0.0;
    y2[s->N] = 0.0;
}

void string_commit(String *s)
{
    for (int p = 0; p < 2; p++) {
        string_ghosts(s, p, s->y2[p]);
        double *t = s->y0[p];
        s->y0[p] = s->y1[p];
        s->y1[p] = s->y2[p];
        s->y2[p] = t;
    }
}

double string_bridge_force(const String *s, const double *y)
{
    int N = s->N;
    double h = s->h;
    double yx = -y[N - 1] / h; /* centred with y_{N+1} = -y_{N-1} */
    double yxxx = (2.0 * y[N - 1] - y[N - 2]) / (h * h * h);
    return s->sp.T * yx - s->EI * yxxx;
}

double string_energy(const String *s, const double *cur, const double *prev)
{
    int N = s->N;
    double h = s->h, k = s->k;
    double kin = 0.0, pot = 0.0, stf = 0.0;
    for (int l = 1; l < N; l++) {
        double v = (cur[l] - prev[l]) / k;
        kin += v * v;
        double xx1 = (cur[l + 1] - 2.0 * cur[l] + cur[l - 1]) / (h * h);
        double xx0 = (prev[l + 1] - 2.0 * prev[l] + prev[l - 1]) / (h * h);
        stf += xx1 * xx0;
    }
    for (int l = 0; l < N; l++) pot += (cur[l + 1] - cur[l]) * (prev[l + 1] - prev[l]) / (h * h);
    return h * (0.5 * kin + 0.5 * s->c2 * pot + 0.5 * s->kap2 * stf);
}

#include "lossmeas.h"
#include "altstring.h"
#include <math.h>
#include <stdlib.h>

int string_partial_decay(const StringSpec *sp, double fs, double secs, int pmax, double *f, double *alpha)
{
    String *s = string_new(sp, fs);
    int N = s->N;
    if (pmax > N / 2) pmax = N / 2;
    double hw = 0.012 * s->sp.L, x0 = 0.173 * s->sp.L;
    for (int l = 1; l < N; l++) {
        double d = (l * s->h - x0) / hw;
        double v = fabs(d) < 1.0 ? 0.5 * (1.0 + cos(M_PI * d)) * 1e-3 : 0.0;
        s->y1[0][l] = s->y0[0][l] = v;
    }
    string_ghosts(s, 0, s->y1[0]);
    string_ghosts(s, 0, s->y0[0]);
    long nsteps = (long)(secs * fs), every = (long)(fs / 4000.0);
    long nrec = nsteps / every + 1;
    double *lnA = malloc(sizeof(double) * (size_t)pmax * (size_t)nrec);
    double *tt = malloc(sizeof(double) * (size_t)nrec);
    double B = string_B(sp);
    double *mode = malloc(sizeof(double) * (size_t)pmax * (size_t)N);
    for (int p = 1; p <= pmax; p++)
        for (int l = 1; l < N; l++) mode[(size_t)(p - 1) * (size_t)N + (size_t)l] = sin(M_PI * p * l / N);
    long r = 0;
    for (long n = 0; n < nsteps; n++) {
        string_step_free(s, 0, NULL);
        string_commit(s); /* y1 = n + 1, y0 = n */
        if (n % every) continue;
        tt[r] = (n + 0.5) / fs;
        for (int p = 1; p <= pmax; p++) {
            double P = 0.0, V = 0.0, w = 2.0 * M_PI * p * sp->f0 * sqrt(1.0 + B * p * p);
            for (int l = 1; l < N; l++) {
                double m = mode[(size_t)(p - 1) * (size_t)N + (size_t)l];
                P += m * 0.5 * (s->y1[0][l] + s->y0[0][l]);
                V += m * (s->y1[0][l] - s->y0[0][l]) * fs;
            }
            P *= 2.0 / N;
            V *= 2.0 / N;
            lnA[(size_t)(p - 1) * (size_t)nrec + (size_t)r] = log(sqrt(P * P + V * V / (w * w)) + 1e-300);
        }
        r++;
    }
    for (int p = 1; p <= pmax; p++) {
        const double *a = lnA + (size_t)(p - 1) * (size_t)nrec;
        double a0 = a[2];
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        long m = 0;
        for (long i = 0; i < r; i++) {
            if (tt[i] < 0.01) continue;
            if (a[i] < a0 - log(1e4)) break;
            sx += tt[i]; sy += a[i]; sxx += tt[i] * tt[i]; sxy += tt[i] * a[i];
            m++;
        }
        double den = (double)m * sxx - sx * sx;
        alpha[p - 1] = m > 3 && den > 0.0 ? -((double)m * sxy - sx * sy) / den : -1.0;
        f[p - 1] = p * sp->f0 * sqrt(1.0 + B * p * p);
    }
    free(mode);
    free(lnA);
    free(tt);
    string_free(s);
    return pmax;
}

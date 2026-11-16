#include "control.h"
#include <math.h>
#include <stdlib.h>

void env_const(Env *e, double v)
{
    e->n = 1;
    e->t[0] = 0.0;
    e->v[0] = v;
}

void env_add(Env *e, double t, double v)
{
    if (e->n < ENV_MAX) {
        e->t[e->n] = t;
        e->v[e->n] = v;
        e->n++;
    }
}

double env_eval(const Env *e, double t)
{
    if (e->n == 0) return 0.0;
    if (t <= e->t[0]) return e->v[0];
    for (int i = 1; i < e->n; i++)
        if (t < e->t[i]) {
            double u = (t - e->t[i - 1]) / (e->t[i] - e->t[i - 1]);
            return e->v[i - 1] + u * (e->v[i] - e->v[i - 1]);
        }
    return e->v[e->n - 1];
}

double trk_eval(const double *a, long n, double rate, double t)
{
    double x = t * rate;
    if (x <= 0.0) return a[0];
    long i = (long)x;
    if (i >= n - 1) return a[n - 1];
    double u = x - (double)i;
    return a[i] + u * (a[i + 1] - a[i]);
}

void trk_alloc(PlayTracks *p, double rate, long n)
{
    p->rate = rate;
    p->n = n;
    p->vB = calloc((size_t)n, sizeof(double));
    p->FB = calloc((size_t)n, sizeof(double));
    p->beta = calloc((size_t)n, sizeof(double));
    p->bowed = calloc((size_t)n, 1);
    for (int i = 0; i < TRK_NSTR; i++) {
        p->xF[i] = calloc((size_t)n, sizeof(double));
        p->aF[i] = calloc((size_t)n, sizeof(double));
    }
}

void trk_free(PlayTracks *p)
{
    free(p->vB); free(p->FB); free(p->beta); free(p->bowed);
    for (int i = 0; i < TRK_NSTR; i++) { free(p->xF[i]); free(p->aF[i]); }
    p->vB = p->FB = p->beta = NULL;
    p->bowed = NULL;
}

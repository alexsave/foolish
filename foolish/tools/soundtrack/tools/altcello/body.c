#include "body.h"
#include <math.h>
#include <stdlib.h>

Body *body_new(const BodyModes *bm, double fs, int lossless)
{
    Body *b = calloc(1, sizeof *b);
    int n = bm->n;
    b->n = n;
    b->fs = fs;
    b->k = 1.0 / fs;
    double k = b->k;
#define ALLOC(x) b->x = calloc((size_t)n, sizeof(double))
    ALLOC(A); ALLOC(B); ALLOC(Gu); ALLOC(Gw); ALLOC(q0); ALLOC(q1); ALLOC(q2);
    ALLOC(m); ALLOC(w2); ALLOC(cu); ALLOC(cw); ALLOC(r);
    for (int i = 0; i < n; i++) {
        double w = 2.0 * M_PI * bm->f[i];
        double s = sin(0.5 * w * k);
        double W2 = 4.0 * s * s;           /* k^2 w_eff^2 */
        double d = lossless ? 0.0 : w * k / (2.0 * bm->Q[i]);
        b->A[i] = (2.0 - W2) / (1.0 + d);
        b->B[i] = -(1.0 - d) / (1.0 + d);
        b->Gu[i] = k * k * bm->cu[i] / (bm->m[i] * (1.0 + d));
        b->Gw[i] = k * k * bm->cw[i] / (bm->m[i] * (1.0 + d));
        b->m[i] = bm->m[i];
        b->w2[i] = W2 / (k * k);
        b->cu[i] = bm->cu[i];
        b->cw[i] = bm->cw[i];
        b->r[i] = bm->r[i];
    }
    return b;
}

void body_free(Body *b)
{
    if (!b) return;
    free(b->A); free(b->B); free(b->Gu); free(b->Gw); free(b->q0); free(b->q1); free(b->q2);
    free(b->m); free(b->w2); free(b->cu); free(b->cw); free(b->r);
    free(b);
}

void body_step(Body *b, double Fu, double Fw)
{
    int n = b->n;
    double zu[4] = {0}, zw[4] = {0}, vu[4] = {0}, vw[4] = {0}, p[4] = {0};
    const double *A = b->A, *B = b->B, *Gu = b->Gu, *Gw = b->Gw, *cu = b->cu, *cw = b->cw, *r = b->r;
    const double *q0 = b->q0, *q1 = b->q1;
    double *q2 = b->q2;
    int i = 0;
    for (; i + 4 <= n; i += 4)
        for (int j = 0; j < 4; j++) {
            int l = i + j;
            double q = A[l] * q1[l] + B[l] * q0[l] + Gu[l] * Fu + Gw[l] * Fw;
            q2[l] = q;
            zu[j] += cu[l] * q;
            zw[j] += cw[l] * q;
            double dq = q - q0[l];
            vu[j] += cu[l] * dq;
            vw[j] += cw[l] * dq;
            p[j] += r[l] * (dq - 2.0 * (q1[l] - q0[l]));
        }
    for (; i < n; i++) {
        double q = A[i] * q1[i] + B[i] * q0[i] + Gu[i] * Fu + Gw[i] * Fw;
        q2[i] = q;
        zu[0] += cu[i] * q;
        zw[0] += cw[i] * q;
        double dq = q - q0[i];
        vu[0] += cu[i] * dq;
        vw[0] += cw[i] * dq;
        p[0] += r[i] * (dq - 2.0 * (q1[i] - q0[i]));
    }
    double k = b->k;
    b->zu = zu[0] + zu[1] + zu[2] + zu[3];
    b->zw = zw[0] + zw[1] + zw[2] + zw[3];
    b->vu = (vu[0] + vu[1] + vu[2] + vu[3]) / (2.0 * k);
    b->vw = (vw[0] + vw[1] + vw[2] + vw[3]) / (2.0 * k);
    b->p = (p[0] + p[1] + p[2] + p[3]) / (k * k);
    double *t = b->q0;
    b->q0 = b->q1;
    b->q1 = b->q2;
    b->q2 = t;
}

double body_energy(const Body *b)
{
    double E = 0.0, k = b->k;
    for (int i = 0; i < b->n; i++) {
        double v = (b->q1[i] - b->q0[i]) / k;
        E += 0.5 * b->m[i] * (v * v + b->w2[i] * b->q1[i] * b->q0[i]);
    }
    return E;
}

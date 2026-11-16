#include "body.h"
#include <math.h>
#include <stdlib.h>

Body *body_new(const Instrument *I, const BodyModes *bm, double fs, int lossless, const BodyOpts *opts)
{
    Body *b = calloc(1, sizeof *b);
    int n = bm->n;
    b->n = n;
    b->fs = fs;
    b->k = 1.0 / fs;
    double k = b->k;
#define ALLOC(x) b->x = calloc((size_t)n, sizeof(double))
    ALLOC(A); ALLOC(B); ALLOC(Gu); ALLOC(Gw); ALLOC(q0); ALLOC(q1); ALLOC(q2);
    ALLOC(m); ALLOC(w2); ALLOC(cu); ALLOC(cw); ALLOC(r); ALLOC(rp);
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
        b->rp[i] = i < bm->ncompact ? 0.0 : bm->r[i];
    }
    b->nc = bm->ncompact;
    if (bm->nfe > 0) {
        int nfe = bm->nfe;
        b->nfe = nfe;
        b->variant = I->body.fe_variant;
        b->Gus = calloc((size_t)(BODY_NS * nfe), sizeof(double)); b->Gws = calloc((size_t)(BODY_NS * nfe), sizeof(double));
        b->cus = calloc((size_t)(BODY_NS * nfe), sizeof(double)); b->cws = calloc((size_t)(BODY_NS * nfe), sizeof(double));
        b->wc = calloc((size_t)nfe, sizeof(double)); b->wp = calloc((size_t)nfe, sizeof(double)); b->wv = calloc((size_t)nfe, sizeof(double));
        b->tq = calloc((size_t)nfe, sizeof(double)); b->tdq = calloc((size_t)nfe, sizeof(double));
        for (int s = 0; s < BODY_NS; s++)
            for (int i = 0; i < nfe; i++) {
                double d = lossless ? 0.0 : 2.0 * M_PI * bm->f[i] * k / (2.0 * bm->Q[i]);
                b->cus[s * nfe + i] = bm->cus[s * nfe + i];
                b->cws[s * nfe + i] = bm->cws[s * nfe + i];
                b->Gus[s * nfe + i] = k * k * bm->cus[s * nfe + i] / (bm->m[i] * (1.0 + d));
                b->Gws[s * nfe + i] = k * k * bm->cws[s * nfe + i] / (bm->m[i] * (1.0 + d));
            }
        /* radiation weights (fe_body.h): FE-A the monopole of r_net; FE-B compact modes (air_share > 0.5 or f < 250 Hz) with r_net / |r_net(A0)|, the others
           with the sign of r_net times the one crossover scale */
        double rA0 = 0.0;
        for (int i = 0; i < nfe; i++) if (bm->air[i] > 0.5) { rA0 = fabs(bm->rnet[i]); break; }
        for (int i = 0; i < nfe; i++) {
            if (b->variant == 1) { b->wv[i] = bm->rnet[i]; continue; }
            if (bm->air[i] > 0.5 || bm->f[i] < 250.0) b->wc[i] = bm->rnet[i] / rA0;
            else b->wp[i] = (bm->rnet[i] > 0.0 ? 1.0 : bm->rnet[i] < 0.0 ? -1.0 : 0.0) * I->body.fe_gain_b;
        }
        b->gain_a = I->body.fe_gain_a * 1.2041 / (4.0 * M_PI * 1.0); /* rho / (4 pi d), d = 1 m */
    }
    BodyOpts def = body_opts_default(I);
    if (radiator_init(&b->rad, &I->radiation, fs, (opts ? opts : &def)->fc, I->radiation.g_plate)) {
        free(b);
        return NULL;
    }
    return b;
}

void body_free(Body *b)
{
    if (!b) return;
    free(b->A); free(b->B); free(b->Gu); free(b->Gw); free(b->q0); free(b->q1); free(b->q2);
    free(b->m); free(b->w2); free(b->cu); free(b->cw); free(b->r); free(b->rp);
    free(b->Gus); free(b->Gws); free(b->cus); free(b->cws); free(b->wc); free(b->wp); free(b->wv); free(b->tq); free(b->tdq);
    free(b);
}

void body_step(Body *b, double Fu, double Fw)
{
    if (b->nfe > 0) abort(); /* the FE body has one coupling per string: body_step_str */
    int n = b->n;
    double zu[4] = {0}, zw[4] = {0}, vu[4] = {0}, vw[4] = {0}, p[4] = {0};
    const double *A = b->A, *B = b->B, *Gu = b->Gu, *Gw = b->Gw, *cu = b->cu, *cw = b->cw, *rp = b->rp;
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
            p[j] += rp[l] * dq;
        }
    for (; i < n; i++) {
        double q = A[i] * q1[i] + B[i] * q0[i] + Gu[i] * Fu + Gw[i] * Fw;
        q2[i] = q;
        zu[0] += cu[i] * q;
        zw[0] += cw[i] * q;
        double dq = q - q0[i];
        vu[0] += cu[i] * dq;
        vw[0] += cw[i] * dq;
        p[0] += rp[i] * dq;
    }
    double k = b->k;
    b->zu = zu[0] + zu[1] + zu[2] + zu[3];
    b->zw = zw[0] + zw[1] + zw[2] + zw[3];
    b->vu = (vu[0] + vu[1] + vu[2] + vu[3]) / (2.0 * k);
    b->vw = (vw[0] + vw[1] + vw[2] + vw[3]) / (2.0 * k);
    /* modal velocities at n: (q2 - q0) / 2k. The compact modes sum with their monopole strengths, the plate modes with their signs */
    double pc = 0.0;
    for (int i = 0; i < b->nc; i++) pc += b->r[i] * (q2[i] - q0[i]);
    double pp = p[0] + p[1] + p[2] + p[3];
    b->p_compact = radiator_step_compact(&b->rad, pc / (2.0 * k));
    b->p_plate = radiator_step_plate(&b->rad, pp / (2.0 * k));
    b->p = b->p_compact + b->rad.g_plate * b->p_plate;
    double *t = b->q0;
    b->q0 = b->q1;
    b->q1 = b->q2;
    b->q2 = t;
}

/* dot product of two arrays of n doubles */
static double dotp(const double *a, const double *x, int n)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i = 0;
    for (; i + 4 <= n; i += 4) { s0 += a[i] * x[i]; s1 += a[i + 1] * x[i + 1]; s2 += a[i + 2] * x[i + 2]; s3 += a[i + 3] * x[i + 3]; }
    for (; i < n; i++) s0 += a[i] * x[i];
    return (s0 + s1) + (s2 + s3);
}

void body_step_str(Body *b, const double *Fu, const double *Fw)
{
    int n = b->n, nfe = b->nfe;
    double k = b->k;
    const double *A = b->A, *B = b->B, *q0 = b->q0, *q1 = b->q1;
    double *q2 = b->q2, *tq = b->tq, *tdq = b->tdq;
    /* the FE modes: each string drives them through its own participation */
    for (int i = 0; i < nfe; i++) tq[i] = A[i] * q1[i] + B[i] * q0[i];
    for (int s = 0; s < BODY_NS; s++) {
        const double *gu = b->Gus + s * nfe, *gw = b->Gws + s * nfe;
        double fu = Fu[s], fw = Fw[s];
        if (fu == 0.0 && fw == 0.0) continue;
        for (int i = 0; i < nfe; i++) tq[i] += gu[i] * fu + gw[i] * fw;
    }
    double pv = 0.0, pc = 0.0, pp = 0.0;
    for (int i = 0; i < nfe; i++) {
        q2[i] = tq[i];
        tdq[i] = tq[i] - q0[i];
        pv += b->wv[i] * (tq[i] - 2.0 * q1[i] + q0[i]);
        pc += b->wc[i] * tdq[i];
        pp += b->wp[i] * tdq[i];
    }
    /* the statistical modes above the crossover: one coupling for every string (the summed force) */
    double Fut = 0.0, Fwt = 0.0, zu = 0.0, zw = 0.0, vu = 0.0, vw = 0.0, ps = 0.0;
    for (int s = 0; s < BODY_NS; s++) { Fut += Fu[s]; Fwt += Fw[s]; }
    for (int i = nfe; i < n; i++) {
        double q = A[i] * q1[i] + B[i] * q0[i] + b->Gu[i] * Fut + b->Gw[i] * Fwt;
        q2[i] = q;
        zu += b->cu[i] * q;
        zw += b->cw[i] * q;
        double dq = q - q0[i];
        vu += b->cu[i] * dq;
        vw += b->cw[i] * dq;
        ps += b->rp[i] * dq;
    }
    for (int s = 0; s < BODY_NS; s++) {
        const double *cu = b->cus + s * nfe, *cw = b->cws + s * nfe;
        b->zus[s] = dotp(cu, tq, nfe) + zu;
        b->zws[s] = dotp(cw, tq, nfe) + zw;
        b->vus[s] = (dotp(cu, tdq, nfe) + vu) / (2.0 * k);
        b->vws[s] = (dotp(cw, tdq, nfe) + vw) / (2.0 * k);
    }
    b->zu = b->zus[0]; /* string 0 (the C string), for the diagnostics that read one bridge point */
    b->zw = b->zws[0];
    b->vu = b->vus[0];
    b->vw = b->vws[0];
    /* FE-A: the monopole; FE-B: wc, wp are zero in FE-A and wv unused in FE-B (body_new) */
    b->p_fe = b->variant == 1 ? b->gain_a * pv / (k * k) : 0.0;
    b->p_compact = radiator_step_compact(&b->rad, pc / (2.0 * k));
    b->p_plate = radiator_step_plate(&b->rad, (pp + ps) / (2.0 * k));
    b->p = b->p_fe + b->p_compact + b->rad.g_plate * b->p_plate;
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

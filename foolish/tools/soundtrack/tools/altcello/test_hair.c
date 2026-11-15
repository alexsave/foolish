/* T18 compliant bow-hair contact (bow.h Hair, sim.c):
 *  a) the hair oscillator alone: constant force settles to F/k, an impulse rings at sqrt(k/m) sqrt(1 - zeta^2) (within 1 percent)
 *     and decays with the damping ratio 0.3 (within 5 percent);
 *  b) the friction solve with the hair in series stays exact: after every step the contact slip velocity, computed from the updated
 *     string, torsion and hair states, equals the solved v_rel (1e-9 m/s);
 *  c) the hair is switched off by default and changes the instrument when on (ripple hump of D3, body on). */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void oscillator(void)
{
    double fs = 96000.0, dt = 1.0 / fs, kh = 1e4;
    Hair h;
    hair_init(&h, kh, dt);
    for (long n = 0; n < 96000; n++) {
        h.d2 = h.A * h.d1 + h.B * h.d0 + h.g * 1.0;
        h.d0 = h.d1;
        h.d1 = h.d2;
    }
    printf("T18a static deflection %.6e m (F/k = %.6e)\n", h.d1, 1.0 / kh);
    CHECK(fabs(h.d1 * kh - 1.0) < 1e-4, "deflection %g, want %g", h.d1, 1.0 / kh);
    hair_reset(&h);
    h.d1 = 1e-3;
    long n = 96000;
    double *x = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) {
        h.d2 = h.A * h.d1 + h.B * h.d0;
        x[i] = h.d1;
        h.d0 = h.d1;
        h.d1 = h.d2;
    }
    /* frequency from upward zero crossings, decay from the first and last positive peaks */
    long z0 = -1, z1 = -1;
    int nz = 0;
    for (long i = 1; i < 2000; i++)
        if (x[i - 1] < 0.0 && x[i] >= 0.0) {
            double fr = -x[i - 1] / (x[i] - x[i - 1]);
            if (z0 < 0) z0 = i, z1 = i, nz = 0;
            else z1 = i, nz++;
            (void)fr;
        }
    double w0 = sqrt(kh / h.m), wd = w0 * sqrt(1.0 - HAIR_ZETA * HAIR_ZETA);
    double f_meas = nz > 0 ? nz * fs / (double)(z1 - z0) : 0.0;
    printf("T18a ring frequency %.1f Hz (expected %.1f Hz)\n", f_meas, wd / (2.0 * M_PI));
    CHECK(fabs(f_meas / (wd / (2.0 * M_PI)) - 1.0) < 0.01, "ring %.1f Hz, expected %.1f Hz", f_meas, wd / (2.0 * M_PI));
    double pk0 = 0, pk1 = 0, t0 = 0, t1 = 0;
    for (long i = 1; i < 2000; i++)
        if (x[i] > x[i - 1] && x[i] >= x[i + 1] && x[i] > 0) {
            if (pk0 == 0) { pk0 = x[i]; t0 = i / fs; }
            else { pk1 = x[i]; t1 = i / fs; }
        }
    double rate = -log(pk1 / pk0) / (t1 - t0), want = HAIR_ZETA * w0;
    printf("T18a decay rate %.1f 1/s (zeta w0 = %.1f)\n", rate, want);
    CHECK(fabs(rate / want - 1.0) < 0.05, "decay %.1f 1/s, want %.1f", rate, want);
    free(x);
}

typedef struct { double beta, vB, worst; long checked; } Chk;
static void chk_cb(const Sim *S, long i, void *u)
{
    Chk *c = u;
    if (i < 12000) return;
    int lo, hi, tlo, thi;
    double J[BOW_MAXPTS], Jt[BOW_MAXPTS];
    sim_bow_weights(S, c->beta, &lo, &hi, J, &tlo, &thi, Jt);
    const String *s = S->s;
    const Torsion *t = s->tor;
    double vu = 0.0, vt = 0.0;
    for (int l = lo; l <= hi; l++) vu += J[l - lo] * s->h * (s->y1[0][l] - s->y2[0][l]);
    for (int l = tlo; l <= thi; l++) vt += Jt[l - tlo] * t->h * (t->th1[l] - t->th2[l]);
    double vc = (vu + t->rs * vt) / (2.0 * S->k);
    /* after the commit: hair d0 = d(n), d1 = d(n+1); the hair velocity at n needs d(n-1), kept in dprev */
    double vh = (S->hair.d1 - S->hair.dprev) / (2.0 * S->k);
    double err = fabs(vc - c->vB - vh - S->out[3]);
    if (err > c->worst) c->worst = err;
    c->checked++;
}

static void consistency(void)
{
    const StringSpec *sp = instrument_string(instrument_cello(), "D3");
    Sim S;
    sim_init(&S, instrument_cello(), sp, 96000.0);
    hair_init(&S.hair, 1e4, S.k);
    sim_set_bow(&S, 0.1, 0.1, 0.6, 0.03);
    sim_start(&S);
    Chk c = {0.1, 0.1, 0.0, 0};
    Trace *tr = sim_run(&S, 0.6, chk_cb, &c);
    printf("T18b %ld steps checked, worst |contact velocity - (vB + hair velocity + v_rel)| = %.3e m/s\n", c.checked, c.worst);
    CHECK(c.checked > 40000, "only %ld steps checked", c.checked);
    CHECK(c.worst < 1e-9, "contact velocity differs by %g m/s", c.worst);
    trace_free(tr);
    sim_free(&S);
}

static void matters(void)
{
    const StringSpec *sp = instrument_string(instrument_cello(), "D3");
    double h[2];
    for (int on = 0; on < 2; on++) {
        SimOpts o = {1, 0, 0, NULL};
        Sim S;
        sim_init_instrument(&S, instrument_cello(), 96000.0, &o, "D3");
        CHECK(S.hair.on == 0, "the hair must be off by default");
        if (on) hair_init(&S.hair, 1e4, S.k);
        sim_set_bow(&S, 0.1, 0.1, 0.526, 0.03);
        sim_start(&S);
        Trace *tr = sim_run(&S, 1.5, NULL, NULL);
        h[on] = hump_db(tr->ch[0], tr->n, tr->fs, sp->f0, 2500.0, 3500.0);
        printf("T18c hair %s: hump 2.5-3.5 kHz %.1f dB re h1\n", on ? "k_h 1e4" : "off    ", h[on]);
        trace_free(tr);
        sim_free(&S);
    }
    CHECK(fabs(h[1] - h[0]) > 0.5, "the hair changes the ripple hump by only %.2f dB", h[1] - h[0]);
}

int main(void)
{
    oscillator();
    consistency();
    matters();
    TEST_DONE("test_hair");
}

/* T14 torsion of the string.
 *  a) free torsional partials of D3 (lossless) against the discrete eigenfrequencies of the torsion scheme
 *     and the continuum p c_t / (2 L);
 *  b) torsional damping: energy decays as exp(-2 sigma_t t), sigma_t = zeta_t pi c_t / L;
 *  c) the surface impedance: a point torque on the infinite-string stage gives a surface velocity
 *     F/(2 Z) with Z = 3.0 Z0 (the brief's torsional impedance at the surface);
 *  d) the bow solve: after every step the contact velocity recomputed from the updated states
 *     (transverse velocity plus r_s times the torsional angular velocity at the hair) equals vB + v_rel,
 *     i.e. the torque and the combined admittance in the scalar solve are consistent with the scheme;
 *  e) torsion is on in the instrument: it changes the Helmholtz bridge force (reported). */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { double *x; int idx; } Rec;
static void rec_cb(const Sim *S, long i, void *u)
{
    Rec *r = u;
    r->x[i] = S->s->tor->th1[r->idx];
}

static void partials(void)
{
    StringSpec sp = *instrument_string(instrument_cello(), "D3");
    sp.zeta_t = 0.0;
    double fs = 96000.0;
    Sim S;
    sim_init(&S, instrument_cello(), &sp, fs);
    S.bow_on = 0;
    sim_start(&S);
    sim_pluck_torsion(&S, 0, 0.137 * sp.L, 0.03 * sp.L, 0.1);
    Torsion *t = S.s->tor;
    long n = (long)(1.0 * fs);
    Rec r = {calloc((size_t)n, sizeof(double)), (int)(0.29 * t->N)};
    Trace *tr = sim_run(&S, 1.0, rec_cb, &r);
    Spectrum *sa = spec_make(r.x, n, fs, 8);
    double cr = t->ct * t->k / t->h;
    printf("T14a D3 torsion: c_t %.2f m/s (5 c), N_t %d, h_t %.3f mm, Courant number %.4f, sigma_t(default) %.1f /s\n", t->ct, t->N, t->h * 1e3, cr, 0.01 * M_PI * t->ct / sp.L);
    printf("  p  f_discrete  f_measured  meas-disc[c]  f_continuum  disc-cont[c]\n");
    for (int p = 1; p <= 12; p++) {
        double beta = p * M_PI / sp.L;
        double fd = 2.0 / t->k * asin(cr * sin(0.5 * beta * t->h)) / (2.0 * M_PI);
        double fc = p * t->ct / (2.0 * sp.L);
        double fm = spec_peak(sa, fd, 0.02, NULL);
        printf(" %2d  %10.3f  %10.3f  %+9.3f  %10.3f  %+9.3f\n", p, fd, fm, cents(fm, fd), fc, cents(fd, fc));
        CHECK(fabs(cents(fm, fd)) < 0.5, "torsional partial %d: %.3f Hz vs discrete %.3f Hz = %.3f cents", p, fm, fd, cents(fm, fd));
        CHECK(fabs(cents(fd, fc)) < 25.0, "torsional partial %d: discrete %.3f Hz vs continuum %.3f Hz = %.2f cents", p, fd, fc, cents(fd, fc));
    }
    spec_free(sa);
    free(r.x);
    trace_free(tr);
    sim_free(&S);
}

typedef struct { double *E; } Log;
static void elog(const Sim *S, long i, void *u) { ((Log *)u)->E[i] = torsion_energy(S->s->tor); }

static void damping(void)
{
    const StringSpec *sp = instrument_string(instrument_cello(), "D3");
    double fs = 96000.0;
    Sim S;
    sim_init(&S, instrument_cello(), sp, fs);
    S.bow_on = 0;
    sim_start(&S);
    sim_pluck_torsion(&S, 0, 0.137 * sp->L, 0.03 * sp->L, 0.1);
    long n = (long)(0.1 * fs);
    Log lg = {calloc((size_t)n, sizeof(double))};
    Trace *tr = sim_run(&S, 0.1, elog, &lg);
    double sig = sp->zeta_t * M_PI * S.s->tor->ct / sp->L;
    long a = (long)(0.02 * fs), b = (long)(0.06 * fs);
    /* the energy of the centred scheme oscillates a little between kinetic and potential: average over 100 samples */
    double Ea = 0, Eb = 0;
    for (int i = 0; i < 100; i++) { Ea += lg.E[a + i]; Eb += lg.E[b + i]; }
    double rate = -log(Eb / Ea) / (double)(b - a) * fs; /* energy decay rate, 1/s */
    printf("T14b torsional damping: energy decay rate %.2f /s, expected 2 sigma_t = %.2f /s (zeta_t %.3f)\n", rate, 2.0 * sig, sp->zeta_t);
    CHECK(fabs(rate / (2.0 * sig) - 1.0) < 0.03, "decay rate %.2f vs 2 sigma_t %.2f", rate, 2.0 * sig);
    free(lg.E);
    trace_free(tr);
    sim_free(&S);
}

static void impedance(void)
{
    StringSpec sp = *instrument_string(instrument_cello(), "D3");
    sp.zeta_t = 0.0;
    double fs = 96000.0;
    Torsion *t = torsion_new(&sp, fs);
    double Z0 = sqrt(sp.T * string_rho(&sp));
    double F0 = 1.0; /* N at the surface */
    int m = t->N / 2;
    double T0 = F0 * t->rs;
    double v = 0.0;
    int steps = t->N / 2 - 4; /* no reflection from the ends yet */
    for (int i = 0; i < steps; i++) {
        torsion_step_free(t);
        t->th2[m] += t->kk * t->invD[m] * T0 / t->h;
        torsion_commit(t);
        v = t->rs * (t->th1[m] - t->th2[m]) / (2.0 * t->k); /* centred, as the bow solve measures it (th2 = n-1 after the commit) */
    }
    double Zmeas = F0 / v;
    printf("T14c torsional surface impedance: F %.1f N gives surface velocity %.4f m/s -> F/v = %.4f kg/s = %.3f x (2 * 3.0 Z0 = %.4f); Z0 %.4f\n", F0, v, Zmeas, Zmeas / (6.0 * Z0), 6.0 * Z0, Z0);
    CHECK(fabs(Zmeas / (6.0 * Z0) - 1.0) < 0.05, "F/v = %.4f vs 6 Z0 = %.4f", Zmeas, 6.0 * Z0);
    torsion_free(t);
}

typedef struct { double beta, worst, vB; long checked; double *fb; } Chk;
static void chk_cb(const Sim *S, long i, void *u)
{
    Chk *c = u;
    if (c->fb) c->fb[i] = S->out[0];
    if (i < 12000) return;
    int lo, hi, tlo, thi;
    double J[BOW_MAXPTS], Jt[BOW_MAXPTS];
    sim_bow_weights(S, c->beta, &lo, &hi, J, &tlo, &thi, Jt);
    const String *s = S->s;
    const Torsion *t = s->tor;
    /* after the commit: y1 = n+1, y0 = n, y2 = n-1 (the buffers were rotated) */
    double vu = 0.0, vt = 0.0;
    for (int l = lo; l <= hi; l++) vu += J[l - lo] * s->h * (s->y1[0][l] - s->y2[0][l]);
    for (int l = tlo; l <= thi; l++) vt += Jt[l - tlo] * t->h * (t->th1[l] - t->th2[l]);
    double vc = (vu + t->rs * vt) / (2.0 * S->k);
    double err = fabs(vc - c->vB - S->out[3]);
    if (err > c->worst) c->worst = err;
    c->checked++;
}

static void bow_solve_consistency(void)
{
    const StringSpec *sp = instrument_string(instrument_cello(), "D3");
    Sim S;
    sim_init(&S, instrument_cello(), sp, 96000.0);
    sim_set_bow(&S, 0.1, 0.1, 0.6, 0.03);
    sim_start(&S);
    Chk c = {0.1, 0.0, 0.1, 0, NULL};
    Trace *tr = sim_run(&S, 0.6, chk_cb, &c);
    printf("T14d bow solve: %ld steps checked, worst |contact velocity - (vB + v_rel)| = %.3e m/s\n", c.checked, c.worst);
    CHECK(c.checked > 40000, "only %ld steps checked", c.checked);
    CHECK(c.worst < 1e-9, "contact velocity from the updated states differs from vB + v_rel by %g m/s", c.worst);
    trace_free(tr);
    sim_free(&S);
}

static void torsion_matters(void)
{
    const StringSpec *sp = instrument_string(instrument_cello(), "D3");
    double d[2];
    for (int on = 0; on < 2; on++) {
        Sim S;
        sim_init(&S, instrument_cello(), sp, 96000.0);
        S.torsion_on = on;
        sim_set_bow(&S, 0.1, 0.1, 0.6, 0.03);
        sim_start(&S);
        Trace *tr = sim_run(&S, 1.5, NULL, NULL);
        double h = hump_db(tr->ch[0], tr->n, tr->fs, sp->f0, 2500.0, 3500.0);
        double e = 0;
        for (long i = 0; i < tr->n; i++) e += tr->ch[0][i] * tr->ch[0][i];
        d[on] = h;
        printf("T14e torsion %s: hump 2.5-3.5 kHz %.1f dB re h1, bridge force rms %.4f N\n", on ? "on " : "off", h, sqrt(e / (double)tr->n));
        trace_free(tr);
        sim_free(&S);
    }
    CHECK(fabs(d[1] - d[0]) > 1.0, "torsion changes the ripple hump by only %.2f dB", d[1] - d[0]);
}

int main(void)
{
    partials();
    damping();
    impedance();
    bow_solve_consistency();
    torsion_matters();
    TEST_DONE("test_torsion");
}

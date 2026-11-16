/* T1 free string partials against the discrete scheme eigenfrequencies (and
 * the continuous stiff-string formula, reported), T2 numerical energy. */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdlib.h>

/* eigenfrequency of simply supported mode p of the lossless scheme */
static double discrete_freq(const String *s, int p)
{
    double beta = p * M_PI / s->sp.L;
    double sn = sin(0.5 * beta * s->h);
    double sq = sn * sn;
    double arg = s->c2 * s->k * s->k * sq / (s->h * s->h) + 4.0 * s->kap2 * s->k * s->k * sq * sq / pow(s->h, 4);
    return 2.0 / s->k * asin(sqrt(arg)) / (2.0 * M_PI);
}

static double *pluck_trace(const StringSpec *sp, double fs, double xfrac, double secs, long *n)
{
    Sim S;
    sim_init(&S, instrument_cello(), sp, fs);
    S.bow_on = 0;
    sim_start(&S);
    sim_pluck(&S, xfrac * sp->L, 0.02 * sp->L, 1e-3);
    Trace *tr = sim_run(&S, secs, NULL, NULL);
    double *x = tr->ch[0];
    tr->ch[0] = NULL;
    *n = tr->n;
    trace_free(tr);
    sim_free(&S);
    return x;
}

typedef struct { double *E; long n; Sim *S; } EnergyLog;
static void energy_cb(const Sim *S, long i, void *user)
{
    EnergyLog *e = user;
    e->E[i] = string_energy(S->s, S->s->y1[0], S->s->y0[0]);
}

static void energy_test(const StringSpec *sp0, int lossless)
{
    StringSpec sp = *sp0;
    if (lossless) {
        sp.sigma0 = sp.sigma1 = 0.0;
        for (int r = 0; r < STR_NREL; r++) sp.rel_s1[r] = 0.0;
    }
    Sim S;
    sim_init(&S, instrument_cello(), &sp, 96000.0);
    S.bow_on = 0;
    sim_start(&S);
    sim_pluck(&S, 0.13 * sp.L, 0.02 * sp.L, 1e-3);
    long n = 96000;
    EnergyLog e = {calloc((size_t)n, sizeof(double)), n, &S};
    Trace *tr = sim_run(&S, 1.0, energy_cb, &e);
    double mx = 0, mn = 1e300, worst_up = 0;
    for (long i = 0; i < n; i++) {
        if (e.E[i] > mx) mx = e.E[i];
        if (e.E[i] < mn) mn = e.E[i];
        if (i > 0) {
            double up = (e.E[i] - e.E[i - 1]) / e.E[0];
            if (up > worst_up) worst_up = up;
        }
    }
    if (lossless) {
        double rel = (mx - mn) / e.E[0];
        printf("T2 lossless: E0 %.6e relative spread %.3e (limit 1e-10)\n", e.E[0], rel);
        CHECK(rel < 1e-10, "energy not conserved: relative spread %g", rel);
    } else {
        printf("T2 lossy: E0 %.6e -> E_end %.6e, largest relative step increase %.3e\n", e.E[0], e.E[n - 1], worst_up);
        CHECK(worst_up < 1e-12, "energy increased by relative %g in one step", worst_up);
        CHECK(e.E[n - 1] < 0.9 * e.E[0], "energy did not decay: %g -> %g", e.E[0], e.E[n - 1]);
    }
    free(e.E);
    trace_free(tr);
    sim_free(&S);
}

int main(void)
{
    const StringSpec *sp = instrument_string(instrument_cello(), "D3");
    const char *names[4] = {"A3", "D3", "G2", "C2"};
    for (int i = 0; i < 4; i++) {
        const StringSpec *q = instrument_string(instrument_cello(), names[i]);
        String *s = string_new(q, 96000.0);
        printf("string %s: rho %.5e kg/m, B = %.4e, N = %d, h = %.4f mm\n", q->name, string_rho(q), string_B(q), s->N, s->h * 1e3);
        string_free(s);
    }

    /* T1 */
    double fs = 96000.0;
    long n1, n2;
    /* the partials are compared with the eigenfrequencies of the LOSSLESS scheme: the relaxation loss (altstring.h) adds a small dispersion (a rise of under 0.3 percent
       above its corners, test_string_loss), so this check runs with the relaxation terms off; the sigma0 / sigma1 loss does not move the partials */
    StringSpec spn = *sp;
    for (int r = 0; r < STR_NREL; r++) spn.rel_s1[r] = 0.0;
    double *x1 = pluck_trace(&spn, fs, 0.1, 2.0, &n1);
    double *x2 = pluck_trace(&spn, fs, 0.137, 2.0, &n2);
    Spectrum *sa = spec_make(x1, n1, fs, 8), *sb = spec_make(x2, n2, fs, 8);
    String *s = string_new(sp, fs);
    double B = string_B(sp);
    printf("T1 partials of D3 plucked at 0.1 L (0.137 L for n = 10, 20), fs %.0f\n", fs);
    printf(" n  f_discrete   f_measured   meas-disc[c]  f_continuous  meas-cont[c]  disc-cont[c]\n");
    for (int p = 1; p <= 20; p++) {
        double fd = discrete_freq(s, p);
        double fc = p * sp->f0 * sqrt(1.0 + B * p * p);
        double fm = spec_peak((p % 10 == 0) ? sb : sa, fd, 0.01, NULL);
        double cd = cents(fm, fd), cc = cents(fm, fc), dc = cents(fd, fc);
        printf("%2d  %10.3f  %10.3f  %+10.3f    %10.3f  %+10.3f  %+10.3f\n", p, fd, fm, cd, fc, cc, dc);
        CHECK(fabs(cd) < 1.0, "partial %d: measured %.3f Hz vs discrete eigenfrequency %.3f Hz = %.3f cents", p, fm, fd, cd);
        if (p <= 10) CHECK(fabs(cc) < 3.0, "partial %d: %.3f cents from the continuous formula", p, cc);
    }
    string_free(s);
    spec_free(sa);
    spec_free(sb);
    free(x1);
    free(x2);

    /* T2: lossless, then lossy with each string loss model (sigma1 k^2 and the viscoelastic relaxation of altstring.h, whose stored energy is in string_energy) */
    energy_test(sp, 1);
    energy_test(sp, 0);
    Instrument R = *instrument_cello();
    instrument_cello_string_loss(&R, 1);
    printf("T2 lossy with the relaxation loss:\n");
    energy_test(instrument_string(&R, "D3"), 0);
    TEST_DONE("test_string_free");
}

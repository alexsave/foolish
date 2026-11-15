/* T10 bridge loss: a plucked open string with the body on decays faster than
 * with a rigid bridge (T60 of partials 1..10, both printed). Also printed: the
 * G2 string, whose 2nd partial (196 Hz) sits near B1- (178 Hz) and B1+ (205 Hz). */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SECS 5.0
#define T60_T0 0.05 /* fit window of the dB envelope, s: the coupled string loses most of its energy to the body in the first 0.3 s, then beats slowly */
#define T60_T1 1.0

static Trace *pluck(const char *name, int body, double fs)
{
    SimOpts o = {body, 0, 0};
    Sim S;
    sim_init_instrument(&S, fs, &o, name);
    S.bow_on = 0;
    sim_start(&S);
    sim_pluck(&S, 0.137 * S.s->sp.L, 0.02 * S.s->sp.L, 1e-3);
    Trace *tr = sim_run(&S, SECS, NULL, NULL);
    sim_free(&S);
    return tr;
}

/* T60 of the partial near f from the demodulated envelope: slope of the dB
   envelope between T60_T0 and T60_T1 (the moving average spans 2/f0 so the other
   partials of the harmonic series average to zero) */
static double t60(const double *x, long n, double fs, double f, double f0)
{
    long L = (long)(2.0 * fs / f0 + 0.5);
    double *cr = malloc(sizeof(double) * (size_t)n), *ci = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) {
        double ph = 2.0 * M_PI * f * i / fs;
        cr[i] = x[i] * cos(ph);
        ci[i] = -x[i] * sin(ph);
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    double ar = 0, ai = 0;
    for (long i = 0; i < L; i++) { ar += cr[i]; ai += ci[i]; }
    for (long i = 0; i + L < n; i++) {
        double t = (i + 0.5 * L) / fs;
        if (t >= T60_T0 && t <= T60_T1 && i % 480 == 0) {
            double dB = 20.0 * log10(hypot(ar, ai) / L + 1e-300);
            sx += t; sy += dB; sxx += t * t; sxy += t * dB;
            cnt++;
        }
        ar += cr[i + L] - cr[i];
        ai += ci[i + L] - ci[i];
    }
    free(cr);
    free(ci);
    double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx); /* dB per s */
    return -60.0 / slope;
}

static void table(const char *name, int assert_all)
{
    double fs = 96000.0;
    Trace *rig = pluck(name, 0, fs), *bod = pluck(name, 1, fs);
    double f0 = string_spec(name)->f0;
    long i0 = (long)(0.3 * fs);
    Spectrum *sr = spec_make(rig->ch[0] + i0, rig->n - i0, fs, 4), *sb = spec_make(bod->ch[0] + i0, bod->n - i0, fs, 4);
    printf("T10 %s plucked, partials 1..10: T60 rigid bridge vs body on\n", name);
    printf(" n  f_rigid[Hz]  T60_rigid[s]  f_body[Hz]  T60_body[s]  ratio\n");
    for (int p = 1; p <= 10; p++) {
        double fr = spec_peak(sr, p * f0, 0.02, NULL), fb = spec_peak(sb, p * f0, 0.02, NULL);
        double tr_ = t60(rig->ch[0], rig->n, fs, fr, f0), tb = t60(bod->ch[0], bod->n, fs, fb, f0);
        printf("%2d  %10.3f  %11.3f  %10.3f  %10.3f  %6.3f\n", p, fr, tr_, fb, tb, tb / tr_);
        if (assert_all) CHECK(tb < tr_, "partial %d: body-on T60 %.3f s is not shorter than rigid %.3f s", p, tb, tr_);
    }
    spec_free(sr);
    spec_free(sb);
    trace_free(rig);
    trace_free(bod);
}

int main(void)
{
    table("D3", 1);
    table("G2", 0);
    TEST_DONE("test_bridge_loss");
}

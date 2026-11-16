/* T25 the MSW oracle (oracle_msw.h): a plucked string and a bowed string.
 *  a) pluck: partials of the oracle at k f0 within 1 cent (k = 1..20), their decay rates against the target (the decay rates the
 *     finite difference D3 string with the C1 loss gives for a pluck, measured the same way) within 10 percent,
 *  b) bowed at a mid force (beta 0.1, v_B 0.1 m/s, F 0.67 N): one slip per period for 20 periods, slip fraction within 0.02 of
 *     beta, pitch within 2 cents of f0. */
#include "analysis.h"
#include "bow.h"
#include "oracle_msw.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define KP 20
#define KDEC 20 /* the decay comparison covers partials 1..20 (to 2.9 kHz; the loss fit covers 27 partials, to 4 kHz) */

int main(void)
{
    /* the oracle's friction CURVE junction (the thermal junction is tested in test_thermal): the curve instrument, whatever the default */
    Instrument curve = *instrument_cello();
    curve.bow.model = BOW_CURVE;
    const Instrument *I = &curve;
    const StringSpec *sp = instrument_string(I, "D3");
    double fs = 96000.0;
    OracleSpec os;
    double alpha_t[ORACLE_KFIT_MAX];
    int Kt = 0;
    /* a pluck needs a bow position that is no simple fraction (a pluck at beta = 0.1 lacks every 10th partial) */
    double err = oracle_spec_from_string(I, sp, fs, 0.2371, 4000.0, &os, alpha_t, &Kt);
    printf("T25 D3 oracle: Z0 %.4f g %.5f p %.4f, loss fit rms relative error %.3f over %d partials\n", os.Z0, os.g, os.p, err, Kt);
    CHECK(Kt >= KDEC, "only %d measurable partials in the target pluck", Kt);

    /* a) pluck: a force pulse at the bow point, no bowing */
    {
        Oracle *o = oracle_new(&os);
        long n = (long)(3.0 * fs);
        double *x = calloc((size_t)n, sizeof(double)), vr;
        for (long i = 0; i < n; i++) {
            double f = i < 12 ? 0.5 * (1.0 - cos(2.0 * M_PI * (i + 0.5) / 12.0)) : 0.0;
            oracle_step(o, 0.0, 0.0, f, &vr, &x[i]);
        }
        double fk[KP], ak[KP];
        partial_decays(x, n, fs, sp->f0, KP, 0.05, 0.04, 40.0, fk, ak);
        double wc = 0.0, wd = 0.0;
        for (int k = 1; k <= KP; k++) {
            double c = cents(fk[k - 1], k * sp->f0);
            if (fabs(c) > wc) wc = fabs(c);
            double model = oracle_alpha_model(os.g, os.p, sp->f0, fs, 2.0 * M_PI * k * sp->f0 / fs);
            double rel = k <= KDEC && k <= Kt ? ak[k - 1] / alpha_t[k - 1] - 1.0 : 0.0;
            if (fabs(rel) > wd) wd = fabs(rel);
            printf("  partial %2d: %9.3f Hz (%+.3f c re k f0)  decay %7.3f /s  target %7.3f  (filter model %7.3f)\n", k, fk[k - 1], c, ak[k - 1],
                   k <= Kt ? alpha_t[k - 1] : 0.0, model);
            CHECK(fabs(c) <= 1.0, "partial %d is %.3f cents from k f0", k, c);
            if (k <= KDEC && k <= Kt) CHECK(fabs(rel) <= 0.10, "partial %d decay %.3f /s vs target %.3f /s: %.1f percent", k, ak[k - 1], alpha_t[k - 1], 100.0 * rel);
        }
        printf("T25a worst partial %.3f cents, worst decay error %.1f percent (partials 1..%d)\n", wc, 100.0 * wd, KDEC);
        free(x);
        oracle_free(o);
    }

    /* b) Helmholtz motion at a mid force */
    {
        OracleSpec ob = os;
        ob.beta = 0.1;
        Oracle *o = oracle_new(&ob);
        long n = (long)(4.0 * fs);
        double *br = calloc((size_t)n, sizeof(double)), *vr = calloc((size_t)n, sizeof(double));
        for (long i = 0; i < n; i++) {
            double t = (double)i / fs, r = t < 0.03 ? t / 0.03 : 1.0;
            oracle_step(o, 0.1 * r, 0.67082 * r, 0.0, &vr[i], &br[i]);
        }
        Helm h = analyse_helmholtz(br, vr, n, fs, sp->f0, 2.5);
        printf("T25b one slip per period %d (counts %d..%d), slip fraction %.4f (beta 0.1), pitch %+.3f cents\n", h.ok_one_per_period, h.counts_min, h.counts_max,
               h.slip_frac, h.cents_flat);
        CHECK(h.ok_one_per_period, "not one slip per period: counts %d..%d", h.counts_min, h.counts_max);
        CHECK(fabs(h.slip_frac - 0.1) <= 0.02, "slip fraction %.4f vs beta 0.1", h.slip_frac);
        CHECK(fabs(h.cents_flat) <= 2.0, "pitch %.3f cents from f0", h.cents_flat);
        free(br);
        free(vr);
        oracle_free(o);
    }

    /* c) the bow junction: solution of v = q - G phi(v), the hysteresis inside the static cone, the force normalisation */
    {
        const Friction *fr = &I->bow.friction;
        double G = 1.0, q = 0.95 * fr->mu_static * G; /* inside the cone, with a slipping root below it (g dips under zero) */
        int st1 = 1, st0 = 0;
        double ph1, ph0, v1 = oracle_junction(fr, q, G, &st1, &ph1), v0 = oracle_junction(fr, q, G, &st0, &ph0);
        printf("T25c in the cone: previous stick -> v %.4f stuck %d; previous slip -> v %.4f stuck %d, residual %.2e\n", v1, st1, v0, st0, fabs(v0 - (q - G * ph0)));
        CHECK(st1 == 1 && v1 == 0.0, "hysteresis: a stuck junction inside the cone must stay stuck");
        CHECK(st0 == 0 && v0 > 0.0 && fabs(v0 - (q - G * ph0)) < 1e-12, "a slipping junction inside the cone must stay on the slipping root: v %.4f", v0);
        double worst = 0.0;
        for (double qq = -3.0; qq <= 3.0; qq += 0.0137) {
            int s = 0;
            double ph, v = oracle_junction(fr, qq, 1.0, &s, &ph);
            if (fabs(qq) > fr->mu_static) { /* outside the cone: always slipping, v = q - G phi(v) exactly */
                CHECK(!s && fabs(v - (qq - ph)) < 1e-12, "outside the cone q %.4f: stuck %d residual %.2e", qq, s, fabs(v - (qq - ph)));
                worst = fmax(worst, fabs(v - (qq - ph)));
            }
        }
        /* force normalisation: from rest, bow speed 0.1 m/s, F 0.05 N (outside the static cone): the first step must satisfy
           v_rel = -v_B - (F / (2 Z0)) phi(v_rel), with the string's wave impedance Z0 */
        double F = 0.05, vr, br;
        Oracle *a = oracle_new(&os);
        oracle_step(a, 0.1, F, 0.0, &vr, &br);
        double res = fabs(vr - (-0.1 - F / (2.0 * os.Z0) * bow_phi(fr, vr)));
        printf("T25c first step from rest, F %.2f N, v_B 0.1: v_rel %.5f, residual of v = q - F/(2 Z0) phi(v): %.2e\n", F, vr, res);
        CHECK(res < 1e-12 && vr < -0.05, "force normalisation: residual %.2e, v_rel %.5f", res, vr);
        oracle_free(a);
    }
    TEST_DONE("test_oracle_msw");
}

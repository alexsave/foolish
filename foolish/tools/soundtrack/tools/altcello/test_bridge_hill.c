/* T17 bridge hill (Woodhouse eq (6), body_params.c) and the statistical loss factor:
 *  a) the hill factor is 1 at low frequency, peaks near the designed 1.5 kHz, and falls above;
 *  b) bridge inertia x1.5 lowers the hill frequency by about 1/sqrt(1.5) = 0.816 (within 10 percent), both for the factor and for
 *     the statistical modal masses of the built body are the same random draws divided by the hill factor of the chosen inertia;
 *  c) the statistical modes have Q = 1 / (eta0 (f / 500 Hz)^0.3) with eta0 = 0.012, the table modes keep their Q. */
#include "body_modes.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* the built body: m_k |G(f_k, I)| is the same random draw whatever the inertia, so the knob reaches the modal masses through the hill factor.
   (A band-averaged |Y_uu| of the body cannot show the 1/sqrt(1.5) shift: the random modal masses, 6 dB rms, move its peak by a band.) */
static double wiring(double I1, double I2)
{
    BodyOpts a = body_opts_default(instrument_cello()), b = a;
    a.bridge_inertia = I1;
    b.bridge_inertia = I2;
    BodyModes *ma = body_modes_build_opts(instrument_cello(), &a), *mb = body_modes_build_opts(instrument_cello(), &b);
    double worst = 0.0;
    for (int i = ma->nlead; i < ma->n; i++) {
        double e = fabs(ma->m[i] * body_bridge_gain(instrument_cello(), ma->f[i], I1) / (mb->m[i] * body_bridge_gain(instrument_cello(), mb->f[i], I2)) - 1.0);
        if (e > worst) worst = e;
    }
    /* and the modes below the statistical range (the table modes) do not see the bridge */
    for (int i = 0; i < ma->nlead; i++)
        if (ma->m[i] != mb->m[i]) worst = 1.0;
    body_modes_free(ma);
    body_modes_free(mb);
    return worst;
}

int main(void)
{
    /* the instrument's R_inf is 3 x Woodhouse's 30 (diagnosis C1, instrument_cello.c), which damps the hill away; the designed hill
       (a, b below) is the one of Woodhouse's R_inf, i.e. the default divided by 3 */
    Instrument iw = *instrument_cello();
    iw.body.bridge_R /= 3.0;
    BodyOpts def = body_opts_default(instrument_cello());
    double I0 = def.bridge_inertia;
    double g0 = body_bridge_gain(&iw, 20.0, I0), f0 = body_bridge_hill_freq(&iw, I0), f1 = body_bridge_hill_freq(&iw, 1.5 * I0);
    double gp = body_bridge_gain(&iw, f0, I0);
    printf("T17a hill factor (R_inf = %.0f): %.4f at 20 Hz, peak %.2f (%.1f dB) at %.0f Hz, %.2f at 8 kHz\n", iw.body.bridge_R, g0, gp, 20.0 * log10(gp), f0, body_bridge_gain(&iw, 8000.0, I0));
    CHECK(fabs(g0 - 1.0) < 0.01, "hill factor %.4f at 20 Hz, want 1", g0);
    CHECK(f0 > 1200.0 && f0 < 1800.0, "hill at %.0f Hz, designed about 1500 Hz", f0);
    CHECK(gp > 1.3, "hill peak only %.2f", gp);
    CHECK(body_bridge_gain(&iw, 8000.0, I0) < 0.5 * gp, "hill factor does not fall above the hill");
    double ratio = f1 / f0;
    printf("T17b hill frequency with 1.5x bridge inertia: %.0f Hz, ratio %.3f (1/sqrt(1.5) = %.3f)\n", f1, ratio, 1.0 / sqrt(1.5));
    CHECK(fabs(ratio / (1.0 / sqrt(1.5)) - 1.0) < 0.10, "ratio %.3f is not within 10 percent of %.3f", ratio, 1.0 / sqrt(1.5));
    /* the default instrument (C1): no hill is left, the factor stays at or under 1.02 and falls above 1.5 kHz */
    double pk = 0.0;
    for (double f = 20.0; f < 12000.0; f *= 1.01) pk = fmax(pk, body_bridge_gain(instrument_cello(), f, I0));
    printf("T17d default instrument (R_inf = %.0f): hill factor peak %.3f, %.2f at 8 kHz\n", instrument_cello()->body.bridge_R, pk, body_bridge_gain(instrument_cello(), 8000.0, I0));
    CHECK(pk < 1.02, "default hill factor peaks at %.3f: the C1 bridge damping should leave no hill", pk);
    CHECK(body_bridge_gain(instrument_cello(), 8000.0, I0) < 0.3, "default hill factor does not fall above 1.5 kHz");
    double w = wiring(I0, 1.5 * I0);
    printf("T17b modal masses: m_k |G(f_k)| identical for I and 1.5 I, worst relative difference %.2e\n", w);
    CHECK(w < 1e-12, "modal masses do not follow the bridge inertia: %g", w);

    BodyModes *bm = body_modes_build(instrument_cello());
    double worst = 0.0;
    for (int i = bm->nlead; i < bm->n; i++) {
        double want = 1.0 / (0.012 * pow(bm->f[i] / 500.0, instrument_cello()->body.eta_exp));
        double e = fabs(bm->Q[i] / want - 1.0);
        if (e > worst) worst = e;
    }
    printf("T17c statistical Q = 1/eta(f): %d modes, worst relative error %.2e; Q at %.0f Hz %.1f, at %.0f Hz %.1f\n", bm->n - bm->nlead, worst, bm->f[bm->nlead], bm->Q[bm->nlead], bm->f[bm->n - 1], bm->Q[bm->n - 1]);
    CHECK(worst < 1e-9, "statistical Q differs from 1/eta(f) by %g", worst);
    CHECK(bm->Q[0] == 25.0 && bm->Q[4] == 40.0, "table Q changed: A0 %.1f, m260 %.1f", bm->Q[0], bm->Q[4]);
    body_modes_free(bm);
    TEST_DONE("test_bridge_hill");
}

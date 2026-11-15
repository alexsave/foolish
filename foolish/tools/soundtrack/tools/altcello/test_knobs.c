/* T24 diagnosis knobs (stage 5), all off by default:
 *  a) slow random walks of F_B and v_B: relative sd as asked (within 15 percent over 600 s), lag-one correlation exp(-2 pi bw / rate)
 *     (within 0.03), a second call gives the identical table (one seed);
 *  b) vibrato finger shift: 0 cents leaves the finger, +1200 cents halves the vibrating length, -1200 doubles it, and the
 *     pitch ratio L-xF is exactly 2^(-c/1200);
 *  c) body knobs: stat_mass_scale scales exactly the statistical modal masses, lead_mass_scale exactly the table modes' masses, and the
 *     bridge resistance R_inf lowers the hill: peak of body_bridge_gain_R at 3 R is under half of the peak at R, and at R it is the
 *     default hill. */
#include "body_modes.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdlib.h>

int main(void)
{
    const Instrument *I = instrument_cello();
    Sim S, S2;
    sim_init(&S, I, &I->str[1], 48000.0);
    sim_init(&S2, I, &I->str[1], 96000.0);
    sim_set_walk(&S, 0.05, 2.0, 600.0);
    sim_set_walk(&S2, 0.05, 2.0, 600.0);
    long n = S.walk_n;
    double s2 = 0, c1 = 0;
    for (long i = 0; i < n; i++) s2 += S.walk_F[i] * S.walk_F[i];
    for (long i = 1; i < n; i++) c1 += S.walk_F[i] * S.walk_F[i - 1];
    double sd = sqrt(s2 / n), rho = c1 / s2, want = exp(-2.0 * M_PI * 2.0 / SIM_WALK_RATE);
    printf("T24a walk sd %.4f (want 0.05), lag-1 correlation %.4f (want %.4f)\n", sd, rho, want);
    CHECK(fabs(sd / 0.05 - 1.0) < 0.15, "walk sd %.4f", sd);
    CHECK(fabs(rho - want) < 0.03, "walk correlation %.4f want %.4f", rho, want);
    double worst = 0;
    for (long i = 0; i < n; i++) worst = fmax(worst, fabs(S.walk_F[i] - S2.walk_F[i]) + fabs(S.walk_v[i] - S2.walk_v[i]));
    CHECK(worst == 0.0, "the walk table depends on the sample rate (%g)", worst);
    sim_free(&S);
    sim_free(&S2);

    double L = 0.69, xF = 0.2;
    CHECK(sim_vib_finger(L, xF, 0.0) == xF, "0 cents moves the finger");
    CHECK(fabs((L - sim_vib_finger(L, xF, 1200.0)) / (L - xF) - 0.5) < 1e-12, "+1200 cents does not halve the length");
    CHECK(fabs((L - sim_vib_finger(L, xF, -1200.0)) / (L - xF) - 2.0) < 1e-12, "-1200 cents does not double the length");
    CHECK(fabs((L - sim_vib_finger(L, xF, 13.5)) / (L - xF) - pow(2.0, -13.5 / 1200.0)) < 1e-12, "13.5 cents ratio");
    printf("T24b vibrato finger shift checked\n");

    BodyOpts d = body_opts_default(I), o = d;
    o.stat_mass_scale = 0.5;
    o.lead_mass_scale = 2.0;
    BodyModes *a = body_modes_build_opts(I, &d), *b = body_modes_build_opts(I, &o);
    double w = 0;
    for (int i = 0; i < a->n; i++) {
        double r = b->m[i] / a->m[i], e = i < a->nlead ? 2.0 : 0.5;
        w = fmax(w, fabs(r / e - 1.0));
    }
    CHECK(w < 1e-12, "mass scales are not exact: %g", w);
    body_modes_free(a);
    body_modes_free(b);
    /* the instrument's R_inf is 3 x Woodhouse's 30 (diagnosis C1, instrument_cello.c): compare the Woodhouse value (default / 3) with 3 x it */
    double R0 = I->body.bridge_R / 3.0, pk0 = 0, pk3 = 0;
    for (double f = 300; f < 8000; f *= 1.01) {
        pk0 = fmax(pk0, body_bridge_gain_R(I, f, I->body.bridge_inertia, R0));
        pk3 = fmax(pk3, body_bridge_gain_R(I, f, I->body.bridge_inertia, 3.0 * R0));
    }
    printf("T24c hill peak %.2f at R, %.2f at 3 R\n", pk0, pk3);
    CHECK(pk3 < 0.5 * pk0, "bridge resistance does not lower the hill (%.2f vs %.2f)", pk3, pk0);
    CHECK(body_bridge_gain_R(I, 1500.0, I->body.bridge_inertia, I->body.bridge_R) == body_bridge_gain(I, 1500.0, I->body.bridge_inertia), "default R differs");
    TEST_DONE("test_knobs");
}

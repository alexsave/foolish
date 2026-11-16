/* T29 the measurement tools of the lowstrings study.
 * (a) shadow bodies: the per-string radiated pressures (sim.h sim_enable_shadow) add up to the radiated pressure of the instrument
 *     (the body is linear), D3 stopped on G2 bowed for 0.4 s with all strings on the bridge; and the open D string's share is not zero.
 * (b) knobs (knobs.h): s0 changes sigma0 of every string but keeps each string's quality factor at 2 kHz; q2k.CG sets Q of C and G only
 *     and leaves D and A alone; zeta and zt reach only the named strings; fifths tunes the open strings; a bad knob is refused; the
 *     shipped instrument is untouched by all of it (the copy is private). */
#include "knobs.h"
#include "legato.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static double q2k(const StringSpec *s)
{
    double c = 2.0 * s->L * s->f0, w = 2.0 * M_PI * 2000.0;
    return w / (2.0 * (s->sigma0 + s->sigma1 * (w / c) * (w / c)));
}

int main(void)
{
    const Instrument *I = instrument_cello();
    /* (b) */
    Instrument K = *I;
    double q0[4];
    for (int i = 0; i < 4; i++) q0[i] = q2k(&I->str[i]);
    CHECK(fabs(q0[0] - 300.0) < 1e-6 && fabs(q0[2] - 500.0) < 1e-6, "baseline Q2k %.3f %.3f", q0[0], q0[2]);
    CHECK(knobs_apply(&K, "s0=2.4") == 0, "s0 refused");
    for (int i = 0; i < 4; i++) {
        CHECK(fabs(K.str[i].sigma0 - 2.4) < 1e-12, "sigma0 %g", K.str[i].sigma0);
        CHECK(fabs(q2k(&K.str[i]) - q0[i]) < 1e-6 * q0[i], "string %d Q2k moved to %.4f from %.4f", i, q2k(&K.str[i]), q0[i]);
    }
    K = *I;
    CHECK(knobs_apply(&K, "q2k.CG=120,zeta.CG=0.12,zt.C=8") == 0, "refused");
    CHECK(fabs(q2k(&K.str[0]) - 120.0) < 1e-6 && fabs(q2k(&K.str[1]) - 120.0) < 1e-6, "C G Q2k %.3f %.3f", q2k(&K.str[0]), q2k(&K.str[1]));
    CHECK(fabs(q2k(&K.str[2]) - 500.0) < 1e-6 && fabs(q2k(&K.str[3]) - 500.0) < 1e-6, "D A Q2k moved");
    CHECK(K.str[0].zeta_t == 0.12 && K.str[1].zeta_t == 0.12 && K.str[2].zeta_t == I->str[2].zeta_t, "zeta reached the wrong strings");
    CHECK(K.str[0].zt_ratio == 8.0 && K.str[1].zt_ratio == I->str[1].zt_ratio, "zt reached the wrong strings");
    K = *I;
    CHECK(knobs_apply(&K, "lead_mass=2,stat_mass=3,fifths=1") == 0, "refused");
    CHECK(fabs(K.body.lead[0].m - 2.0 * I->body.lead[0].m) < 1e-12 && fabs(K.body.stat_m500 - 3.0 * I->body.stat_m500) < 1e-12, "masses");
    CHECK(fabs(K.str[3].f0 - 220.0) < 1e-9 && fabs(K.str[2].f0 - 146.6667) < 1e-9 && fabs(K.str[1].f0 - 97.7778) < 1e-9, "fifths %g %g", K.str[2].f0, K.str[1].f0);
    K = *I;
    CHECK(knobs_apply(&K, "bogus=1") != 0, "unknown knob accepted");
    CHECK(knobs_apply(&K, "q2k.X=100") != 0, "unknown string accepted");
    CHECK(I->str[0].sigma0 == 1.2 && instrument_cello()->str[3].f0 == 220.0, "the shipped instrument changed");

    /* (a) */
    Sim S;
    SimOpts so = {1, 1, 0, NULL};
    sim_init_instrument(&S, I, 96000.0, &so, "G2");
    CHECK(sim_enable_shadow(&S, NULL) == 0, "shadow failed");
    env_const(&S.beta, 0.1 / pow(2.0, 7.0 / 12.0));
    S.FB.n = 0; S.vB.n = 0;
    env_add(&S.FB, 0.0, 0.0); env_add(&S.FB, 0.005, 1.3);
    env_add(&S.vB, 0.0, 0.0); env_add(&S.vB, 0.05, 0.1);
    env_const(&S.xF, finger_for(I, 1, 7));
    env_const(&S.aF, 1.0);
    sim_start(&S);
    double mx = 0.0, md = 0.0, eo = 0.0, eb = 0.0;
    long n = (long)(0.4 * 96000.0);
    for (long i = 0; i < n; i++) {
        sim_step(&S);
        double p = S.out[7], sum = 0.0;
        for (int j = 0; j < S.nstr; j++) sum += S.out_pstr[j];
        if (fabs(p) > mx) mx = fabs(p);
        if (fabs(p - sum) > md) md = fabs(p - sum);
        if (i > n / 2) { eb += S.out_pstr[1] * S.out_pstr[1]; eo += S.out_pstr[2] * S.out_pstr[2]; }
    }
    CHECK(mx > 0.0, "silent");
    CHECK(md < 1e-9 * mx, "string pressures do not add up: %.3g of %.3g", md, mx);
    CHECK(eo > 1e-6 * eb && eb > 0.0, "open D string share %.3g of the bowed %.3g", eo, eb);
    sim_free(&S);
    TEST_DONE("test_shadow");
}

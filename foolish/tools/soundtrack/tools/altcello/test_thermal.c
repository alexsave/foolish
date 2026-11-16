/* T30 thermal elasto-plastic bow friction (friction_thermal.h, van Walstijn et al. 2026):
 *  a) the steady sliding map: at a constant relative speed the model settles to the contact temperature of eq (28) / Appendix B (found
 *     independently by bisection on (B.3)) and to the friction coefficient muss(tau(v)), both within 2 percent, for three bow forces
 *     and speeds 0.02-1.5 m/s,
 *  b) passivity: the dissipated friction power is never negative, and the work of the bow friction F phi v_rel summed over time is never
 *     negative and equals the dissipated heat plus the bristle energy (to 2 percent),
 *  c) the contact temperature relaxes after the bow lifts with the time constant a_tau / c_tau of the model (within 20 percent), in the
 *     module with the contact on at v = 0 and in a bowed note whose force drops to zero,
 *  d) Helmholtz motion with THERMAL on D3 (string alone, beta 0.1, v_B 0.1 m/s, F_B 0.67 N): one slip per period for 20 periods, slip
 *     fraction within 0.03 of beta, pitch within 5 cents; the peak contact temperature is printed and must lie in 10-80 K above ambient;
 *     the same on the MSW oracle with the thermal junction (oracle_msw.h),
 *  e) the substeps: 4 substeps per sample agree with 32 on pitch (0.2 cent) and slip fraction (0.005),
 *  f) determinism: two runs with the bow noise on are identical bit for bit, state included. */
#include "analysis.h"
#include "friction_thermal.h"
#include "oracle_msw.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 96000.0

static Instrument thermal_instrument(void)
{
    Instrument ic = *instrument_cello();
    ic.bow.model = BOW_THERMAL;
    ic.bow.thermal.ncont = 1; /* T30 tests the lumped contact (friction_thermal.h); the distributed contact of the default instrument is T33 */
    return ic;
}

typedef struct {
    double tau_peak;     /* K, over the last 10 periods */
    double qmin;         /* W, the smallest dissipated power of any step */
    double wmin, wend;   /* J: the smallest and the final cumulative work of the friction force */
    double heat, hz;     /* J: dissipated heat and the final bristle energy */
    Helm h;
} Note;

typedef struct {
    double dt, w, heat, wmin, qmin, tau_peak;
    long n, n10;
} Acc;

static void acc_cb(const Sim *S, long i, void *user)
{
    Acc *a = user;
    double p = S->out[4] * S->out[3]; /* friction force on the string times v_rel: the power the bow friction puts into the contact */
    a->w += p * a->dt;
    a->heat += S->th.qdiss * a->dt;
    if (a->w < a->wmin) a->wmin = a->w;
    if (S->th.qdiss < a->qmin) a->qmin = S->th.qdiss;
    if (i >= a->n10 && S->th.tau > a->tau_peak) a->tau_peak = S->th.tau;
}

static Note run_note(const Instrument *ic, const StringSpec *sp, double beta, double vB, double F, double secs, int noise, int nsub, double settle)
{
    Instrument c = *ic;
    if (nsub > 0) c.bow.thermal.nsub = nsub;
    Sim S;
    sim_init(&S, &c, sp, FS);
    S.bow_noise_level = noise ? c.bow.noise_level : 0.0;
    sim_set_bow(&S, beta, vB, F, 0.03);
    sim_start(&S);
    double P = FS / sp->f0;
    Acc a = {1.0 / FS, 0.0, 0.0, 0.0, 1e300, 0.0, 0, (long)(secs * FS - 10.0 * P)};
    Trace *tr = sim_run(&S, secs, acc_cb, &a);
    Note r;
    r.tau_peak = a.tau_peak;
    r.qmin = a.qmin;
    r.wmin = a.wmin;
    r.wend = a.w;
    r.heat = a.heat;
    r.hz = S.th.Hz;
    r.h = analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, sp->f0, settle);
    trace_free(tr);
    sim_free(&S);
    return r;
}

int main(void)
{
    Instrument ic = thermal_instrument();
    const ThermalSpec *p = &ic.bow.thermal;
    const StringSpec *sp = instrument_string(&ic, "D3");

    /* a) the steady sliding map */
    {
        double worst_tau = 0.0, worst_mu = 0.0;
        const double Fs[3] = {0.4, 1.0, 2.5}, vs[6] = {0.02, 0.05, 0.1, 0.3, 0.7, 1.5};
        for (int fi = 0; fi < 3; fi++)
            for (int vi = 0; vi < 6; vi++) {
                double F = Fs[fi], v = vs[vi], Fb = F / ic.bow.width;
                Thermal T;
                thermal_init(&T, p, ic.bow.width, 1.0 / FS);
                double phi = 0.0;
                for (int i = 0; i < (int)(0.4 * FS); i++) phi = thermal_advance(&T, F, v);
                double tb = thermal_tau_of_v(p, Fb, v), mss = thermal_mu_ss(p, tb);
                double et = fabs(T.tau / tb - 1.0), em = fabs(phi / mss - 1.0);
                if (et > worst_tau) worst_tau = et;
                if (em > worst_mu) worst_mu = em;
                CHECK(et <= 0.02, "F %.1f v %.2f: tau %.4f K vs Appendix B %.4f K (%.2f percent)", F, v, T.tau, tb, 100.0 * et);
                CHECK(em <= 0.02, "F %.1f v %.2f: mu %.5f vs muss(tau(v)) %.5f (%.2f percent)", F, v, phi, mss, 100.0 * em);
            }
        printf("T30a steady sliding, 3 forces x 6 speeds: worst tau error %.3f percent, worst mu error %.3f percent (limit 2)\n", 100.0 * worst_tau, 100.0 * worst_mu);
    }

    /* b) passivity */
    {
        Note r = run_note(&ic, sp, 0.1, 0.1, 0.67, 1.5, 0, 0, 1.0);
        double miss = fabs(r.wend - (r.heat + r.hz)) / r.wend;
        printf("T30b D3 1.5 s: smallest dissipated power %.3e W, smallest cumulative friction work %.3e J, final %.4e J = heat %.4e + bristle %.3e (mismatch %.2f percent)\n", r.qmin,
               r.wmin, r.wend, r.heat, r.hz, 100.0 * miss);
        CHECK(r.qmin >= 0.0, "negative dissipated power %.3e W", r.qmin);
        CHECK(r.wmin >= -1e-9 * r.wend, "the friction put energy into the contact: cumulative work %.3e J", r.wmin);
        CHECK(r.wend > 0.0 && miss <= 0.02, "work %.4e J vs heat + bristle %.4e J", r.wend, r.heat + r.hz);
    }

    /* c) relaxation of the temperature after the bow lifts */
    {
        double tc = p->a_tau / p->c_tau;
        for (int on = 0; on < 2; on++) {
            Thermal T;
            thermal_init(&T, p, ic.bow.width, 1.0 / FS);
            for (int i = 0; i < (int)(0.05 * FS); i++) thermal_advance(&T, 1.0, 0.5); /* heat at 0.5 m/s */
            double Fl = on ? 1.0 : 0.0; /* on: contact kept (v = 0 at F 1 N); off: force zero */
            double t1 = 0.0, t2 = 0.0, t0 = T.tau;
            for (int i = 0; i < (int)(0.03 * FS); i++) {
                thermal_advance(&T, Fl, 0.0);
                if (i == (int)(0.005 * FS) - 1) t1 = T.tau;
                if (i == (int)(0.025 * FS) - 1) t2 = T.tau;
            }
            double meas = 0.020 / log(t1 / t2);
            printf("T30c %s: tau %.2f K at the lift, %.3f at +5 ms, %.3f at +25 ms: time constant %.3f ms, a_tau / c_tau = %.3f ms\n", on ? "contact at rest" : "force zero", t0, t1, t2,
                   1e3 * meas, 1e3 * tc);
            CHECK(t0 > 1.0 && fabs(meas / tc - 1.0) <= 0.2, "relaxation time constant %.3f ms vs %.3f ms", 1e3 * meas, 1e3 * tc);
        }
        /* in a bowed note: the force drops to zero at 1.0 s */
        Sim S;
        sim_init(&S, &ic, sp, FS);
        S.bow_noise_level = 0.0;
        env_const(&S.beta, 0.1);
        S.FB.n = S.vB.n = 0;
        env_add(&S.FB, 0.0, 0.0); env_add(&S.FB, 0.03, 0.67); env_add(&S.FB, 1.0, 0.67); env_add(&S.FB, 1.001, 0.0);
        env_add(&S.vB, 0.0, 0.0); env_add(&S.vB, 0.03, 0.1);
        sim_start(&S);
        double t1 = 0.0, t2 = 0.0, tl = 0.0;
        for (long i = 0; i < (long)(1.04 * FS); i++) {
            sim_step(&S);
            if (i >= (long)((1.0 - 0.007) * FS) && i < (long)(1.0 * FS) && S.th.tau > tl) tl = S.th.tau; /* the peak of the last period before the lift */
            if (i == (long)(1.006 * FS)) t1 = S.th.tau;
            if (i == (long)(1.026 * FS)) t2 = S.th.tau;
        }
        double meas = 0.020 / log(t1 / t2);
        printf("T30c bowed D3, force to zero at 1.0 s: peak tau %.2f K in the last period before the lift, time constant %.3f ms (a_tau / c_tau %.3f ms)\n", tl, 1e3 * meas, 1e3 * tc);
        CHECK(tl > 1.0 && fabs(meas / tc - 1.0) <= 0.2, "bowed note: relaxation time constant %.3f ms vs %.3f ms", 1e3 * meas, 1e3 * tc);
        sim_free(&S);
    }

    /* d) Helmholtz motion on D3, string alone */
    {
        Note r = run_note(&ic, sp, 0.1, 0.1, 0.67, 4.0, 1, 0, 3.0);
        printf("T30d D3 thermal beta 0.1 vB 0.1 F 0.67: one slip per period %d (counts %d..%d), slip fraction %.4f, pitch %+.3f cents, peak contact temperature %.1f K above ambient = %.1f C\n",
               r.h.ok_one_per_period, r.h.counts_min, r.h.counts_max, r.h.slip_frac, r.h.cents_flat, r.tau_peak, 20.0 + r.tau_peak);
        CHECK(r.h.ok_one_per_period, "not one slip per period: counts %d..%d", r.h.counts_min, r.h.counts_max);
        CHECK(fabs(r.h.slip_frac - 0.1) <= 0.03, "slip fraction %.4f vs beta 0.1", r.h.slip_frac);
        CHECK(fabs(r.h.cents_flat) <= 5.0, "pitch %.3f cents from f0", r.h.cents_flat);
        CHECK(r.tau_peak >= 10.0 && r.tau_peak <= 80.0, "peak contact temperature %.1f K", r.tau_peak);

        OracleSpec os;
        oracle_spec_from_string(&ic, sp, FS, 0.1, 4000.0, &os, NULL, NULL);
        CHECK(os.thermal, "the oracle did not take the thermal junction from the instrument");
        Helm hh[2];
        for (int th = 1; th >= 0; th--) { /* th 1: the thermal junction, 0: the same oracle with the friction curve */
            OracleSpec o2 = os;
            o2.thermal = th;
            Oracle *o = oracle_new(&o2);
            long n = (long)(4.0 * FS);
            double *br = calloc((size_t)n, sizeof(double));
            double *vr = calloc((size_t)n, sizeof(double));
            for (long i = 0; i < n; i++) {
                double t = (double)i / FS, ra = t < 0.03 ? t / 0.03 : 1.0;
                oracle_step(o, 0.1 * ra, 0.67 * ra, 0.0, &vr[i], &br[i]);
            }
            hh[th] = analyse_helmholtz(br, vr, n, FS, sp->f0, 2.5);
            free(br);
            free(vr);
            oracle_free(o);
        }
        Helm h = hh[1];
        printf("T30d MSW oracle with the thermal junction: one slip per period %d (counts %d..%d), slip fraction %.4f, pitch %+.3f cents (the curve junction: %.4f, %+.3f cents)\n",
               h.ok_one_per_period, h.counts_min, h.counts_max, h.slip_frac, h.cents_flat, hh[0].slip_frac, hh[0].cents_flat);
        CHECK(h.ok_one_per_period, "oracle: not one slip per period: counts %d..%d", h.counts_min, h.counts_max);
        CHECK(fabs(h.slip_frac - 0.1) <= 0.03, "oracle slip fraction %.4f vs beta 0.1", h.slip_frac);
        CHECK(fabs(h.cents_flat) <= 5.0, "oracle pitch %.3f cents from f0", h.cents_flat);
        CHECK(fabs(h.slip_frac - hh[0].slip_frac) > 0.002 || fabs(h.cents_flat - hh[0].cents_flat) > 0.2, "the thermal oracle junction gives the friction curve's result");
    }

    /* e) substeps */
    {
        Note a = run_note(&ic, sp, 0.1, 0.1, 0.67, 4.0, 0, 4, 3.0), b = run_note(&ic, sp, 0.1, 0.1, 0.67, 4.0, 0, 32, 3.0);
        printf("T30e substeps 4 vs 32: pitch %+.3f vs %+.3f cents, slip fraction %.4f vs %.4f\n", a.h.cents_flat, b.h.cents_flat, a.h.slip_frac, b.h.slip_frac);
        CHECK(fabs(a.h.cents_flat - b.h.cents_flat) <= 0.2, "pitch differs by %.3f cents", a.h.cents_flat - b.h.cents_flat);
        CHECK(fabs(a.h.slip_frac - b.h.slip_frac) <= 0.005, "slip fraction differs by %.4f", a.h.slip_frac - b.h.slip_frac);
    }

    /* f) determinism, bow noise on */
    {
        double sig[2][3];
        for (int run = 0; run < 2; run++) {
            Sim S;
            sim_init(&S, &ic, sp, FS);
            sim_set_bow(&S, 0.1, 0.1, 0.67, 0.03);
            sim_start(&S);
            double a = 0.0, b = 0.0;
            for (long i = 0; i < (long)(0.5 * FS); i++) {
                sim_step(&S);
                a += S.out[0] * (double)(i % 7 + 1);
                b += S.out[3] * (double)(i % 5 + 1);
            }
            sig[run][0] = a; sig[run][1] = b; sig[run][2] = S.th.tau + 1e3 * S.th.z;
            sim_free(&S);
        }
        printf("T30f determinism: %.15g %.15g\n", sig[0][0], sig[1][0]);
        CHECK(!memcmp(sig[0], sig[1], sizeof sig[0]), "two runs differ");
    }
    TEST_DONE("test_thermal");
}

/* T33 roughness iteration 1: the three contact options of the low strings.
 *  a) per string bow noise scale (BowSpec.noise_scale): a bowed C string with scale 0 is bit for bit the run with the noise level 0, scale 0.5 differs from both; a
 *     bowed A string does not feel the C string's scale,
 *  b) the noise high-pass (bow_noise_highpass, BowSpec.noise_hp): white noise through a 3 kHz corner loses at least 38 dB under 300 Hz and under 2.5 dB above 15 kHz (the digital one-pole gain near Nyquist is 2a/(1+a), -1.9 dB for two); corner 0 is
 *     bit for bit the unfiltered run,
 *  c) the distributed thermal contact (ThermalSpec.ncont, sim.c multi_contact): the shipped instrument has ncont 4 (explicit 4 is bit for bit the default, 1 is the lumped
 *     contact and differs); with 4 points (uniform and tilted pressure) the
 *     friction power given to the contact is never negative, and on a lossless string with its torsion the energy balance closes: work of the bow (force times the bow
 *     speed) = energy gained by the string + heat + bristle energy, within 1 percent of the work, for the lumped contact and for 4 points; Helmholtz motion on D3 with 4 points
 *     (one dominant slip per period, slip fraction within 0.03 of beta, pitch within 5 cents). */
#include "analysis.h"
#include "fft.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 96000.0

static double run_sig(const Instrument *ic, const char *str, double F, int noise_on, double secs)
{
    SimOpts so = {0, 1, 0, NULL};
    Sim S;
    sim_init_instrument(&S, ic, FS, &so, str);
    S.bow_noise_level = noise_on ? ic->bow.noise_level : 0.0;
    sim_set_bow(&S, 0.1, 0.1, F, 0.03);
    sim_start(&S);
    double a = 0.0;
    for (long i = 0; i < (long)(secs * FS); i++) {
        sim_step(&S);
        a += S.out[3] * (double)(i % 7 + 1) + 1e-3 * S.out[0] * (double)(i % 5 + 1);
    }
    sim_free(&S);
    return a;
}

typedef struct {
    double work, heat, dE, hz, pmin, qmin;
} Bal;

/* energy balance of a bowed lossless string (torsion included, no body): bow work F Phi vB integrated over [t0, t1] against the string's energy change plus the heat and the
   change of the bristle energy */
static Bal balance(const Instrument *ic, double secs, double t0, double t1)
{
    SimOpts so = {0, 0, 1, NULL};
    Sim S;
    sim_init_instrument(&S, ic, FS, &so, "C2");
    S.bow_noise_level = 0.0;
    sim_set_bow(&S, 0.1, 0.1, 1.3, 0.03);
    sim_start(&S);
    Bal b = {0, 0, 0, 0, 1e300, 1e300};
    double E0 = 0.0, hz0 = 0.0, cum = 0.0;
    int multi = ic->bow.thermal.ncont > 1;
    for (long i = 0; i < (long)(secs * FS); i++) {
        double t = (double)i / FS;
        if (i == (long)(t0 * FS)) { E0 = sim_energy(&S); hz0 = S.th.Hz; }
        sim_step(&S);
        if (t >= t0 && t < t1) {
            b.work -= S.out[4] * env_eval(&S.vB, t) / FS; /* the friction force on the string is -fric: the bow gives -fric vB */
            b.heat += S.th.qdiss / FS;
            if (S.th.qdiss < b.qmin) b.qmin = S.th.qdiss;
        }
        cum += (multi ? S.pwr : S.out[4] * S.out[3]) / FS; /* work of the friction force in the contact, from the start */
        if (cum < b.pmin) b.pmin = cum;
        if (i == (long)(t1 * FS)) { b.dE = sim_energy(&S) - E0; b.hz = S.th.Hz - hz0; }
    }
    sim_free(&S);
    return b;
}

int main(void)
{
    Instrument base = *instrument_cello();

    /* a) per string noise scale */
    {
        Instrument ic = base;
        double s_def = run_sig(&base, "C2", 1.3, 1, 0.5), s_off = run_sig(&base, "C2", 1.3, 0, 0.5);
        ic.bow.noise_scale[0] = 0.0;
        double s0 = run_sig(&ic, "C2", 1.3, 1, 0.5);
        ic.bow.noise_scale[0] = 0.5;
        double s5 = run_sig(&ic, "C2", 1.3, 1, 0.5);
        double a_def = run_sig(&base, "A3", 0.5, 1, 0.5), a_c0 = 0.0;
        ic.bow.noise_scale[0] = 0.0;
        a_c0 = run_sig(&ic, "A3", 0.5, 1, 0.5);
        printf("T33a noise scale: C2 default %.9g, scale 0 %.9g, noise off %.9g, scale 0.5 %.9g; A3 with the C scale 0 equals default: %d\n", s_def, s0, s_off, s5, a_def == a_c0);
        CHECK(s0 == s_off, "scale 0 is not the noise-off run: %.12g vs %.12g", s0, s_off);
        CHECK(s5 != s_def && s5 != s_off, "scale 0.5 equals the default or the noise-off run");
        CHECK(s_def != s_off, "the noise has no effect on the C string");
        CHECK(a_def == a_c0, "the C string's noise scale changed a bowed A string");
    }

    /* b) the noise high-pass */
    {
        const int N = 65536;
        double *re = calloc((size_t)N, sizeof(double)), *im = calloc((size_t)N, sizeof(double)), *rf = calloc((size_t)N, sizeof(double)), *imf = calloc((size_t)N, sizeof(double));
        Rng r;
        rng_seed(&r, 7);
        double st[2] = {0, 0};
        for (int i = 0; i < N; i++) {
            double x = rng_gauss(&r);
            re[i] = x;
            rf[i] = bow_noise_highpass(st, 3000.0, FS, x);
        }
        fft(re, im, N);
        fft(rf, imf, N);
        double lo[2] = {0, 0}, hi[2] = {0, 0};
        for (int k = 1; k < N / 2; k++) {
            double f = k * FS / N;
            double p0 = re[k] * re[k] + im[k] * im[k], p1 = rf[k] * rf[k] + imf[k] * imf[k];
            if (f >= 50.0 && f <= 300.0) { lo[0] += p0; lo[1] += p1; }
            if (f >= 15000.0 && f <= 40000.0) { hi[0] += p0; hi[1] += p1; }
        }
        double dlo = 10.0 * log10(lo[1] / lo[0]), dhi = 10.0 * log10(hi[1] / hi[0]);
        printf("T33b noise high-pass 3 kHz: 50-300 Hz %.1f dB, 15-40 kHz %.2f dB\n", dlo, dhi);
        CHECK(dlo <= -38.0, "50-300 Hz only %.1f dB down", dlo);
        CHECK(fabs(dhi) <= 2.5, "15-40 kHz changed by %.2f dB", dhi);
        free(re); free(im); free(rf); free(imf);
        Instrument ic = base;
        double s_def = run_sig(&base, "C2", 1.3, 1, 0.3);
        ic.bow.noise_hp[0] = 0.0;
        double s_hp0 = run_sig(&ic, "C2", 1.3, 1, 0.3);
        ic.bow.noise_hp[0] = 3000.0;
        double s_hp = run_sig(&ic, "C2", 1.3, 1, 0.3);
        CHECK(s_def == s_hp0, "corner 0 is not the unfiltered run");
        CHECK(s_hp != s_def, "corner 3000 Hz equals the unfiltered run");
    }

    /* c) the distributed thermal contact */
    {
        Instrument ic = base;
        double s_def = run_sig(&base, "C2", 1.3, 1, 0.3);
        ic.bow.thermal.ncont = 1;
        double s_1 = run_sig(&ic, "C2", 1.3, 1, 0.3);
        ic.bow.thermal.ncont = 4;
        double s_4 = run_sig(&ic, "C2", 1.3, 1, 0.3);
        CHECK(base.bow.thermal.ncont == 4, "the shipped instrument has ncont %d, not 4", base.bow.thermal.ncont);
        CHECK(s_def == s_4, "explicit ncont 4 is not the shipped default");
        CHECK(s_4 != s_1, "ncont 4 equals the lumped contact");
        for (int variant = 0; variant < 3; variant++) {
            Instrument b2 = base;
            b2.bow.thermal.ncont = variant == 0 ? 1 : 4;
            b2.bow.thermal.tilt = variant == 2 ? 0.5 : 0.0;
            Bal b = balance(&b2, 0.5, 0.15, 0.45);
            double miss = fabs(b.work - (b.dE + b.heat + b.hz)) / b.work;
            printf("T33c %s: bow work %.4e J = string %.4e + heat %.4e + bristle %.3e (mismatch %.3f percent), smallest cumulative friction work %.3e J, smallest heat rate %.3e W\n",
                   variant == 0 ? "lumped contact" : variant == 1 ? "4 points" : "4 points, tilt 0.5", b.work, b.dE, b.heat, b.hz, 100.0 * miss, b.pmin, b.qmin);
            CHECK(b.pmin >= -1e-9 * b.work, "the friction put energy into the contact: cumulative work %.3e J", b.pmin);
            CHECK(b.qmin >= 0.0, "negative heat rate %.3e W", b.qmin);
            CHECK(b.work > 0.0 && miss <= 0.01, "energy balance off by %.3f percent", 100.0 * miss);
        }
        /* Helmholtz motion on D3 with 4 points */
        double sf1 = 0.0;
        for (int nc = 1; nc <= 4; nc += 3) { /* the lumped contact first, then 4 points */
        Instrument b4 = base;
        b4.bow.thermal.ncont = nc;
        const StringSpec *sp = instrument_string(&b4, "D3");
        Sim S;
        sim_init(&S, &b4, sp, FS);
        S.bow_noise_level = b4.bow.noise_level;
        sim_set_bow(&S, 0.1, 0.1, 0.67, 0.03);
        sim_start(&S);
        Trace *tr = sim_run(&S, 4.0, NULL, NULL);
        Helm h = analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, sp->f0, 3.0);
        if (nc == 1) { sf1 = h.slip_frac; trace_free(tr); sim_free(&S); continue; }
        SimpleHelm sh = analyse_simple(tr->ch[3], tr->n, tr->fs, h.f_est, 3.0, 0.2);
        printf("T33c D3 with 4 points: one dominant slip per period %d (bad periods %d of %d; the strict count shows %d..%d slips: the points release one after the other), slip fraction %.4f, pitch %+.3f cents\n",
               sh.ok, sh.bad_periods, sh.nper, h.counts_min, h.counts_max, h.slip_frac, h.cents_flat);
        CHECK(sh.ok, "not one dominant slip per period: %d bad periods of %d", sh.bad_periods, sh.nper);
        CHECK(fabs(h.slip_frac - 0.1) <= 0.04 && fabs(h.slip_frac - sf1) <= 0.03, "slip fraction %.4f vs beta 0.1 (limit 0.04) and vs the lumped contact's %.4f (limit 0.03)", h.slip_frac, sf1);
        CHECK(fabs(h.cents_flat) <= 5.0, "pitch %.3f cents from f0", h.cents_flat);
        trace_free(tr);
        sim_free(&S);
        }
    }
    /* d) the shipped instrument on C2: some force gives one dominant slip per period at beta 0.1 (the low string with the distributed contact) */
    {
        Instrument ic = *instrument_cello();
        const StringSpec *c2 = instrument_string(&ic, "C2");
        int nok = 0;
        for (int i = 0; i <= 8; i++) {
            double Ff = 0.4 * pow(7.5, i / 8.0);
            Sim S;
            sim_init(&S, &ic, c2, FS);
            S.bow_noise_level = ic.bow.noise_level;
            sim_set_bow(&S, 0.1, 0.1, Ff, 0.03);
            sim_start(&S);
            Trace *tr = sim_run(&S, 4.2, NULL, NULL);
            Helm h = analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, c2->f0, 3.0);
            SimpleHelm sh = analyse_simple(tr->ch[3], tr->n, tr->fs, h.f_est, 3.0, 0.2);
            if (sh.ok && fabs(h.slip_frac - 0.1) < 0.04 && fabs(h.cents_flat) < 5.0) nok++;
            trace_free(tr);
            sim_free(&S);
        }
        printf("T33d C2 beta 0.1, shipped contact: %d of 9 forces 0.4-3 N give one dominant slip per period\n", nok);
        CHECK(nok > 0, "no force in 0.4..3 N gives Helmholtz motion on C2");
    }
    /* e) the contact velocity of every point (transverse plus r_s times the torsional angular velocity, each averaged over the point's own share), recomputed from the updated
          states, equals vB + the point's v_rel (the multi-point version of T14d), and the torsion matters: on and off give different bridge forces */
    {
        Instrument ic = *instrument_cello();
        const StringSpec *sp = instrument_string(&ic, "D3");
        double sig[2] = {0, 0};
        for (int on = 0; on < 2; on++) {
            Sim S;
            sim_init(&S, &ic, sp, FS);
            S.check_contact = 1;
            S.torsion_on = on;
            sim_set_bow(&S, 0.1, 0.1, 0.6, 0.03);
            sim_start(&S);
            for (long i = 0; i < (long)(0.6 * FS); i++) {
                sim_step(&S);
                sig[on] += S.out[0] * (double)(i % 7 + 1);
            }
            printf("T33e D3 4 points, torsion %s: worst |contact velocity - (vB + v_rel)| over 57600 steps and 4 points %.3e m/s\n", on ? "on" : "off", S.check_err);
            CHECK(S.check_err < 1e-9, "contact velocity from the updated states differs from vB + v_rel by %g m/s (torsion %d)", S.check_err, on);
            sim_free(&S);
        }
        CHECK(sig[0] != sig[1], "torsion on and off give the same bridge force");
    }
    TEST_DONE("test_contact");
}

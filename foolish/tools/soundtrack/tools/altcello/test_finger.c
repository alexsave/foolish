/* T5 finger: A3 stopped a fifth up (E4 = 329.63 Hz), bowed; then a one
 * semitone glide of the finger over 0.5 s whose pitch track must be monotonic.
 * Settling: the string-only model needs seconds to reach steady stick-slip
 * (see test_helmholtz.c), so the hold is 3 s before the glide. */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define HOLD 3.0
#define GLIDE 0.5

static Trace *run(const StringSpec *sp, double F, int glide, double xa, double xb)
{
    Sim S;
    sim_init(&S, instrument_cello(), sp, 96000.0);
    sim_set_bow(&S, 0.1, 0.1, F, 0.03);
    env_const(&S.aF, 1.0);
    env_const(&S.xF, xa);
    if (glide) {
        S.xF.n = 0;
        env_add(&S.xF, 0.0, xa);
        env_add(&S.xF, HOLD, xa);
        env_add(&S.xF, HOLD + GLIDE, xb);
    }
    sim_start(&S);
    Trace *tr = sim_run(&S, glide ? HOLD + GLIDE + 0.4 : HOLD + 0.6, NULL, NULL);
    sim_free(&S);
    return tr;
}

/* pitch by autocorrelation on a 60 ms window centred at t, sub-sample peak */
static double pitch_at(const Trace *tr, double t, double fguess)
{
    double fs = tr->fs;
    long len = (long)(0.060 * fs);
    long i0 = (long)(t * fs) - len / 2;
    long P = (long)(fs / fguess);
    long lo = (long)(0.85 * P), hi = (long)(1.15 * P);
    double best = -2;
    long bl = lo;
    double *c = calloc((size_t)(hi + 2), sizeof(double));
    double mean = 0;
    for (long i = i0; i < i0 + len + hi; i++) mean += tr->ch[0][i];
    mean /= (double)(len + hi);
    for (long lag = lo - 1; lag <= hi + 1; lag++) {
        double sxy = 0, sxx = 0, syy = 0;
        for (long i = i0; i < i0 + len; i++) {
            double a = tr->ch[0][i] - mean, b = tr->ch[0][i + lag] - mean;
            sxy += a * b;
            sxx += a * a;
            syy += b * b;
        }
        c[lag] = sxy / sqrt(sxx * syy + 1e-300);
        if (lag >= lo && lag <= hi && c[lag] > best) {
            best = c[lag];
            bl = lag;
        }
    }
    double a = c[bl - 1], b = c[bl], d = c[bl + 1];
    double den = a - 2 * b + d;
    double off = fabs(den) > 1e-300 ? 0.5 * (a - d) / den : 0;
    free(c);
    return fs / ((double)bl + off);
}

int main(void)
{
    const StringSpec *sp = instrument_string(instrument_cello(), "A3");
    double L = sp->L;
    double xa = L * (1.0 - pow(2.0, -7.0 / 12.0));
    double xb = L * (1.0 - pow(2.0, -8.0 / 12.0));
    double target = 220.0 * pow(2.0, 7.0 / 12.0);
    printf("T5 A3 finger at x_F = %.5f m (fifth up), target %.3f Hz (E4 = 329.63)\n", xa, target);

    /* choose a bow force that gives a clean periodic note */
    double Fs[8] = {1.0, 0.7, 0.5, 0.35, 0.25, 0.18, 0.12, 0.08};
    double F = 0, fbest = 0, okF[8], okf[8];
    int nok = 0;
    Trace *tr = NULL;
    for (int i = 0; i < 8; i++) {
        tr = run(sp, Fs[i], 0, xa, xa);
        long i0 = (long)(HOLD * tr->fs);
        Spectrum *spc = spec_make(tr->ch[0] + i0, tr->n - i0, tr->fs, 8);
        double f = spec_peak(spc, target, 0.05, NULL);
        spec_free(spc);
        long P = (long)(tr->fs / f), best;
        RunStats st;
        classify_run(tr->ch[0], tr->ch[3], tr->fs, f, HOLD, HOLD + 0.5, &st);
        double corr = autocorr_max(tr->ch[0], i0, i0 + 5 * P, (long)(0.95 * P), (long)(1.05 * P), &best);
        printf("  F %.2f: f %.3f Hz (%+.2f cents vs E4), corr %.3f, class %s (%.2f slips/period)\n", Fs[i], f, cents(f, target), corr, cls_names[st.cls], st.onsets_per_period);
        if (st.cls == CLS_HELMHOLTZ && corr > 0.95) {
            okF[nok] = Fs[i];
            okf[nok++] = f;
        }
        trace_free(tr);
    }
    /* the middle of the forces that give a clean Helmholtz note (the pitch
       depends on the force: flatter when pressing harder, spread printed below) */
    if (nok > 0) {
        F = okF[nok / 2];
        fbest = okf[nok / 2];
        printf("T5 %d Helmholtz forces, pitch spread %+.2f .. %+.2f cents; using the middle one\n", nok, cents(okf[0], target), cents(okf[nok - 1], target));
    }
    CHECK(F > 0, "no bow force in the list gives a periodic Helmholtz note on the stopped A3");
    if (F > 0) {
        double c = cents(fbest, target);
        printf("T5 stopped pitch: F %.2f N, f %.3f Hz = %+.2f cents from 329.63 Hz (E4 target %.3f)\n", F, fbest, c, target);
        CHECK(fabs(c) < 5.0, "stopped fundamental %.3f Hz is %.2f cents from %.3f Hz", fbest, c, target);

        tr = run(sp, F, 1, xa, xb);
        printf("T5 glide pitch track (hold %.1f s, then %.1f s one semitone up):\n", HOLD, GLIDE);
        double prev = 0, first = 0, lastp = 0;
        int n = 0;
        double worst = 1e9;
        for (double t = HOLD - 0.1; t <= HOLD + GLIDE + 0.25; t += 0.020) {
            double f = pitch_at(tr, t, fbest * pow(2.0, (t < HOLD ? 0 : (t > HOLD + GLIDE ? 1 : (t - HOLD) / GLIDE)) / 12.0));
            double cc = cents(f, target);
            printf("  t %.3f  f %8.3f Hz  %+8.2f cents\n", t, f, cc);
            if (n > 0 && cc - prev < worst) worst = cc - prev;
            if (n == 0) first = cc;
            prev = cc;
            lastp = cc;
            n++;
        }
        printf("T5 glide: total rise %.1f cents (expect about 100), worst step %.2f cents (limit -2)\n", lastp - first, worst);
        CHECK(worst > -2.0, "pitch track steps down by %.2f cents", worst);
        CHECK(lastp - first > 80.0 && lastp - first < 120.0, "total rise %.1f cents", lastp - first);
        trace_free(tr);
    }
    TEST_DONE("test_finger");
}

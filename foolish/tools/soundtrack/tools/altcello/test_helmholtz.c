/* T3 Helmholtz motion: D3, beta 0.1, vB 0.1 m/s, F_B 1.0 N (swept 0.2..3 N if
 * that force does not give it). The string-only model has very little loss
 * (sigma0 = 1.2 /s), so the stick-slip state needs seconds to settle: the
 * analysis starts at SETTLE = 3.0 s, not 0.5 s (the 0.5 s result is printed
 * for the record). */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int ok_one_per_period;
    int counts_min, counts_max;
    double slip_frac, f_est, cents_flat, corr;
} Helm;

static Helm analyse(const Trace *tr, double f0, double settle)
{
    Helm h = {0};
    double fs = tr->fs;
    long i0 = (long)(settle * fs);
    Spectrum *sp = spec_make(tr->ch[0] + i0, tr->n - i0, fs, 8);
    h.f_est = spec_peak(sp, f0, 0.05, NULL);
    spec_free(sp);
    h.cents_flat = cents(h.f_est, f0);
    double P = fs / h.f_est;
    /* first full slip interval after 0.5 s, then a window of 20 periods that
       starts in the middle of the stick phase */
    long i = i0;
    while (i < tr->n - 1 && !(fabs(tr->ch[3][i]) <= 1e-4 && fabs(tr->ch[3][i + 1]) > 1e-4)) i++;
    long w0 = i + (long)(0.5 * P);
    h.counts_min = 1 << 20;
    h.counts_max = 0;
    for (int p = 0; p < 20; p++) {
        long a = w0 + (long)(p * P), b = w0 + (long)((p + 1) * P);
        if (b > tr->n) { h.counts_min = -1; break; }
        double fr;
        int c = slip_onsets(tr->ch[3], a, b, 1e-4, fs, &fr);
        if (c < h.counts_min) h.counts_min = c;
        if (c > h.counts_max) h.counts_max = c;
    }
    h.ok_one_per_period = (h.counts_min == 1 && h.counts_max == 1);
    long w1 = w0 + (long)(20 * P);
    if (w1 <= tr->n) slip_onsets(tr->ch[3], w0, w1, 1e-4, fs, &h.slip_frac);
    long best;
    h.corr = autocorr_max(tr->ch[0], w0, w0 + (long)(10 * P), (long)(0.95 * P), (long)(1.05 * P), &best);
    return h;
}

static Trace *run(const StringSpec *sp, double beta, double vB, double F, double secs)
{
    Sim S;
    sim_init(&S, sp, 96000.0);
    sim_set_bow(&S, beta, vB, F, 0.03);
    sim_start(&S);
    Trace *tr = sim_run(&S, secs, NULL, NULL);
    sim_free(&S);
    return tr;
}

#define SETTLE 3.0
#define RUNLEN 4.2

static int passes(const Helm *h, double beta)
{
    return h->ok_one_per_period && fabs(h->slip_frac - beta) < 0.03 && fabs(h->cents_flat) < 5.0;
}

int main(void)
{
    const StringSpec *sp = string_spec("D3");
    double beta = 0.1;
    double F = 1.0;
    Trace *tr = run(sp, beta, 0.1, F, RUNLEN);
    Helm h05 = analyse(tr, sp->f0, 0.5);
    printf("T3 (record) D3 beta 0.1 vB 0.1 F 1.00, window from 0.5 s: slips per period %d..%d, slip fraction %.4f, %.2f cents, corr %.3f\n",
           h05.counts_min, h05.counts_max, h05.slip_frac, h05.cents_flat, h05.corr);
    Helm h = analyse(tr, sp->f0, SETTLE);
    printf("T3 D3 beta 0.1 vB 0.1 F %.2f, window from %.1f s: slips per period min %d max %d, slip fraction %.4f, f %.3f Hz (%.2f cents vs open %.2f Hz), corr %.4f\n",
           F, SETTLE, h.counts_min, h.counts_max, h.slip_frac, h.f_est, h.cents_flat, sp->f0, h.corr);
    if (!passes(&h, beta)) {
        printf("F = 1.0 N does not satisfy T3; sweeping F in 0.2..3 N\n");
        double okF[16];
        Helm okH[16];
        int nok = 0;
        for (int i = 0; i <= 14; i++) {
            double Ff = 0.2 * pow(15.0, i / 14.0);
            trace_free(tr);
            tr = run(sp, beta, 0.1, Ff, RUNLEN);
            Helm g = analyse(tr, sp->f0, SETTLE);
            printf("  F %.3f: slips per period %d..%d, slip fraction %.4f, %.2f cents, corr %.3f%s\n", Ff, g.counts_min, g.counts_max, g.slip_frac, g.cents_flat, g.corr, passes(&g, beta) ? "  OK" : "");
            if (passes(&g, beta)) {
                okF[nok] = Ff;
                okH[nok++] = g;
            }
        }
        CHECK(nok > 0, "no force in 0.2..3 N gives Helmholtz motion");
        if (nok > 0) {
            int best = 0;
            for (int i = 1; i < nok; i++)
                if (okH[i].corr > okH[best].corr) best = i;
            F = okF[best];
            h = okH[best];
            printf("T3 forces that work: %d of 15; using the one with the best period correlation, F = %.3f N\n", nok, F);
        }
    }
    CHECK(h.ok_one_per_period, "slip intervals per period min %d max %d (want exactly 1 in all 20)", h.counts_min, h.counts_max);
    CHECK(fabs(h.slip_frac - beta) < 0.03, "mean slip fraction %.4f vs beta %.2f", h.slip_frac, beta);
    CHECK(fabs(h.cents_flat) < 5.0, "fundamental %.3f Hz is %.2f cents from %.2f Hz", h.f_est, h.cents_flat, sp->f0);
    CHECK(h.corr > 0.9, "period correlation %.3f", h.corr);
    printf("T3 result: F %.3f N, pitch %.2f cents from open, slip fraction %.4f\n", F, h.cents_flat, h.slip_frac);
    FILE *ff = fopen("out/t3_force.txt", "w");
    if (ff) {
        fprintf(ff, "%.6f\n", F);
        fclose(ff);
    }
    trace_free(tr);
    TEST_DONE("test_helmholtz");
}

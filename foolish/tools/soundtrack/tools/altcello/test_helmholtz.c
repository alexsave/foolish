/* T3 Helmholtz motion: D3, beta 0.1, vB 0.1 m/s, F_B 1.0 N (swept 0.2..3 N if
 * that force does not give it). Stage 2 iteration 2: the same sweep for the C2
 * string (beta 0.1 and 0.15), and the test asserts a Helmholtz point at beta 0.1
 * for both D3 and C2 (C2 did not reach Helmholtz motion at all with the stiff solid core). The string-only model has very little loss
 * (sigma0 = 1.2 /s), so the stick-slip state needs seconds to settle: the
 * analysis starts at SETTLE = 3.0 s, not 0.5 s (the 0.5 s result is printed
 * for the record). */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static Helm analyse(const Trace *tr, double f0, double settle)
{
    return analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, f0, settle);
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

    /* C2: sweep 0.2..3 N at beta 0.1 and 0.15 (iteration 1: never Helmholtz, B 4e-4) */
    const StringSpec *c2 = string_spec("C2");
    double betas[2] = {0.1, 0.15};
    for (int b = 0; b < 2; b++) {
        int nok2 = 0;
        for (int i = 0; i <= 14; i++) {
            double Ff = 0.2 * pow(15.0, i / 14.0);
            Trace *t2 = run(c2, betas[b], 0.1, Ff, RUNLEN);
            Helm g = analyse(t2, c2->f0, SETTLE);
            RunStats st;
            classify_run(t2->ch[0], t2->ch[3], t2->fs, c2->f0, RUNLEN - 0.5, RUNLEN, &st);
            printf("  C2 beta %.2f F %.3f: slips per period %d..%d (%.2f onsets/period, %s), slip fraction %.4f, %.2f cents, corr %.3f%s\n", betas[b], Ff, g.counts_min, g.counts_max, st.onsets_per_period, cls_names[st.cls], g.slip_frac, g.cents_flat, g.corr, passes(&g, betas[b]) ? "  OK" : "");
            if (passes(&g, betas[b])) nok2++;
            trace_free(t2);
        }
        printf("T3 C2 beta %.2f: %d of 15 forces give Helmholtz motion\n", betas[b], nok2);
        if (b == 0) CHECK(nok2 > 0, "C2 beta 0.1: no force in 0.2..3 N gives Helmholtz motion");
    }
    TEST_DONE("test_helmholtz");
}

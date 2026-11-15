/* T12 Helmholtz with the body on (D3 bowed, the other strings absent: the sweep
 * would be too slow with all four; one run with all four is reported). Reports
 * the T3 conditions, the force band at beta 0.05 0.1 0.2, settling time, the
 * hump near 3 kHz and the pitch, rigid against body. Asserts only that a
 * Helmholtz point exists at beta 0.1. Writes out/sweep_body.txt and
 * out/t12_force.txt for the chart. */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define NF 16
#define RUNLEN 6.0

static Trace *run(int body, int all, double beta, double F, double secs)
{
    SimOpts o = {body, all, 0};
    Sim S;
    sim_init_instrument(&S, 96000.0, &o, "D3");
    sim_set_bow(&S, beta, 0.1, F, 0.03);
    sim_start(&S);
    Trace *tr = sim_run(&S, secs, NULL, NULL);
    sim_free(&S);
    return tr;
}

static int passes(const Helm *h, double beta)
{
    return h->ok_one_per_period && fabs(h->slip_frac - beta) < 0.03 && fabs(h->cents_flat) < 5.0;
}

static void report(const char *tag, const Trace *tr, double f0)
{
    Helm h = analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, f0, 3.0);
    double P = tr->fs / h.f_est;
    double st = settle_time(tr->ch[3], tr->n, tr->fs, P);
    double hump = hump_db(tr->ch[0], tr->n, tr->fs, f0, 2500.0, 3500.0);
    printf("T12 %-22s slips/period %d..%d, slip fraction %.4f, pitch %+.2f cents, settling %.3f s, hump 2.5-3.5 kHz %.1f dB re h1, corr %.3f\n", tag, h.counts_min, h.counts_max, h.slip_frac, h.cents_flat, st, hump, h.corr);
}

int main(void)
{
    const StringSpec *sp = string_spec("D3");
    double betas[3] = {0.05, 0.1, 0.2};
    /* T3 conditions: choose the F in 0.2..3 N that passes with the best correlation, body on */
    double bestF = 0, bestc = -1;
    for (int i = 0; i <= 14; i++) {
        double F = 0.2 * pow(15.0, i / 14.0);
        Trace *tr = run(1, 0, 0.1, F, 4.2);
        Helm g = analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, sp->f0, 3.0);
        printf("  body on F %.3f: slips per period %d..%d, slip fraction %.4f, %.2f cents, corr %.3f%s\n", F, g.counts_min, g.counts_max, g.slip_frac, g.cents_flat, g.corr, passes(&g, 0.1) ? "  OK" : "");
        if (passes(&g, 0.1) && g.corr > bestc) { bestc = g.corr; bestF = F; }
        trace_free(tr);
    }
    printf("T12 T3 conditions with the body: best force %.3f N (rigid: 0.638 N)\n", bestF);
    double Ff = bestF > 0 ? bestF : 0.638;
    Trace *rig = run(0, 0, 0.1, 0.638, 4.2), *bod = run(1, 0, 0.1, Ff, 4.2), *all = run(1, 1, 0.1, Ff, 4.2);
    report("rigid F 0.638", rig, sp->f0);
    report("body on", bod, sp->f0);
    report("body on, all 4 strings", all, sp->f0);
    trace_free(rig);
    trace_free(bod);
    trace_free(all);
    FILE *ff = fopen("out/t12_force.txt", "w");
    if (ff) { fprintf(ff, "%.6f\n", Ff); fclose(ff); }

    /* T4 conditions with the body */
    FILE *out = fopen("out/sweep_body.txt", "w");
    CHECK(out != NULL, "cannot open out/sweep_body.txt (run via make test)");
    double Fmin[3] = {0}, Fmax[3] = {0};
    int nh1 = 0;
    for (int b = 0; b < 3; b++) {
        RunClass cls[NF];
        double F[NF];
        for (int i = 0; i < NF; i++) {
            F[i] = 0.05 * pow(60.0, (double)i / (NF - 1));
            Trace *tr = run(1, 0, betas[b], F[i], RUNLEN);
            RunStats st;
            classify_run(tr->ch[0], tr->ch[3], tr->fs, sp->f0, RUNLEN - 0.5, RUNLEN, &st);
            cls[i] = st.cls;
            if (out) fprintf(out, "%.3f %.5f %s %.3f %.4f %.4f\n", betas[b], F[i], cls_names[st.cls], st.onsets_per_period, st.corr, st.slip_frac);
            trace_free(tr);
        }
        int first = -1, last = -1, nh = 0;
        for (int i = 0; i < NF; i++)
            if (cls[i] == CLS_HELMHOLTZ) { if (first < 0) first = i; last = i; nh++; }
        printf("T12 body on, beta %.2f:", betas[b]);
        for (int i = 0; i < NF; i++) printf(" %.3f:%.3s", F[i], cls_names[cls[i]]);
        printf("\n");
        if (nh) {
            Fmin[b] = F[first];
            Fmax[b] = F[last];
            printf("T12 body on, beta %.2f: Helmholtz points %d, F_min %.3f N, F_max %.3f N (%s)\n", betas[b], nh, Fmin[b], Fmax[b], last - first + 1 == nh ? "contiguous" : "not contiguous");
        } else printf("T12 body on, beta %.2f: no Helmholtz point on the grid\n", betas[b]);
        if (b == 1) nh1 = nh;
    }
    if (out) fclose(out);
    CHECK(nh1 > 0, "no Helmholtz point at beta 0.1 with the body on");
    TEST_DONE("test_body_helmholtz");
}

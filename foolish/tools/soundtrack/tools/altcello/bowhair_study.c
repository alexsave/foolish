/* Bow-hair compliance study (stage 2 iteration 3): D3, body on (bowed string only), torsion on, beta 0.1, vB 0.1 m/s.
 * For each k_h in {off, 2e3, 5e3, 1e4, 3e4, 1e5} N/m (hair mass 0.5 g, contact damping ratio 0.3) and each bow force of the
 * 6-point grid 0.3 ... 1.5 N (geometric) plus the iteration 2 reference force 0.526 N: Helmholtz class (last 1 s), the ripple hump
 * (2.5-3.5 kHz re h1, last 1 s), settling time, slip fraction, pitch.
 *   usage: bowhair_study OUTFILE [seconds]
 * OUTFILE columns: k_h F class onsets_per_period corr slip_frac cents settle_s hump_dB */
#include "analysis.h"
#include "sim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: bowhair_study OUTFILE [seconds]\n"); return 2; }
    double secs = argc > 2 ? atof(argv[2]) : 4.5;
    const double kh[] = {0.0, 2e3, 5e3, 1e4, 3e4, 1e5};
    double F[7];
    for (int i = 0; i < 6; i++) F[i] = 0.3 * pow(5.0, i / 5.0);
    F[6] = 0.526;
    const StringSpec *sp = string_spec("D3");
    FILE *out = fopen(argv[1], "w");
    if (!out) { perror(argv[1]); return 1; }
    fprintf(out, "# k_h F class onsets_per_period corr slip_frac cents settle_s hump_dB\n");
    for (int a = 0; a < 6; a++)
        for (int b = 0; b < 7; b++) {
            SimOpts o = {1, 0, 0, NULL};
            Sim S;
            sim_init_instrument(&S, 96000.0, &o, "D3");
            if (kh[a] > 0.0) hair_init(&S.hair, kh[a], S.k);
            sim_set_bow(&S, 0.1, 0.1, F[b], 0.03);
            sim_start(&S);
            Trace *tr = sim_run(&S, secs, NULL, NULL);
            RunStats st;
            classify_run(tr->ch[0], tr->ch[3], tr->fs, sp->f0, secs - 1.0, secs, &st);
            Helm h = analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, sp->f0, secs - 1.5);
            double settle = settle_time(tr->ch[3], tr->n, tr->fs, tr->fs / h.f_est);
            double hump = hump_db(tr->ch[0], tr->n, tr->fs, sp->f0, 2500.0, 3500.0);
            fprintf(out, "%.0f %.4f %s %.3f %.4f %.4f %.2f %.3f %.2f\n", kh[a], F[b], cls_names[st.cls], st.onsets_per_period, st.corr, st.slip_frac, h.cents_flat, settle, hump);
            fflush(out);
            printf("k_h %6.0f F %.3f: %-9s slip %.3f %+.2f c settle %.3f s hump %.1f dB\n", kh[a], F[b], cls_names[st.cls], st.slip_frac, h.cents_flat, settle, hump);
            trace_free(tr);
            sim_free(&S);
        }
    fclose(out);
    return 0;
}

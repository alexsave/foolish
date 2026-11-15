/* T11 sympathetic string: bow D3 only (Helmholtz settings), body on, all four
 * strings on the bridge. After 2 s the open G2 string's velocity spectrum has a
 * peak at 293.7 Hz (D3's 2nd harmonic = G2's 3rd partial), at least 20 dB above
 * its level at 270 Hz. */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double *v; int g2, pt; long n0; } Rec;
static void cb(const Sim *S, long i, void *u)
{
    Rec *r = u;
    if (i >= r->n0) {
        const String *g = S->str[r->g2];
        r->v[i - r->n0] = (g->y1[0][r->pt] - g->y0[0][r->pt]) / S->k;
    }
}

int main(void)
{
    double fs = 96000.0, T0 = 2.0, T1 = 5.0;
    SimOpts o = {1, 1, 0, 0};
    Sim S;
    sim_init_instrument(&S, fs, &o, "D3");
    sim_set_bow(&S, 0.1, 0.1, 0.4, 0.03);
    sim_start(&S);
    int g2 = -1;
    for (int i = 0; i < S.nstr; i++)
        if (!strcmp(S.str[i]->sp.name, "G2")) g2 = i;
    CHECK(g2 >= 0, "no G2 string");
    Rec r = {calloc((size_t)((T1 - T0) * fs), sizeof(double)), g2, (int)(0.3 * S.str[g2]->N), (long)(T0 * fs)};
    Trace *tr = sim_run(&S, T1, cb, &r);
    long n = (long)((T1 - T0) * fs);
    Spectrum *sp = spec_make(r.v, n, fs, 4);
    double m;
    double fpk = spec_peak(sp, 293.7, 0.012, &m);
    double m270;
    spec_peak(sp, 270.0, 0.004, &m270);
    double db = 20.0 * log10((m + 1e-300) / (m270 + 1e-300));
    printf("T11 G2 velocity spectrum (2..5 s): peak %.3f Hz, %.1f dB re the level at 270 Hz (need >= 20)\n", fpk, db);
    /* context for the report: G2 partials and the 20 dB lines */
    for (int p = 1; p <= 5; p++) {
        double mm;
        double f = spec_peak(sp, 98.0 * p, 0.012, &mm);
        printf("   G2 partial region %d: %.2f Hz, %.1f dB re 270 Hz level\n", p, f, 20.0 * log10((mm + 1e-300) / (m270 + 1e-300)));
    }
    CHECK(db >= 20.0, "peak at %.2f Hz is only %.1f dB above the 270 Hz level", fpk, db);
    CHECK(fabs(fpk - 293.7) < 3.0, "peak at %.2f Hz is not near 293.7 Hz", fpk);
    spec_free(sp);
    trace_free(tr);
    free(r.v);
    sim_free(&S);
    TEST_DONE("test_sympathetic");
}

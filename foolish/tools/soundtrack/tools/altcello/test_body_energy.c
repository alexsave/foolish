/* T8 energy of the coupled instrument: four strings on the bridge plus the
 * modal body. Lossless (strings sigma = 0, body Q = infinity): the total energy
 * is constant to 1e-9 relative over 1 s after a pluck. With losses it never
 * increases. */
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { double *E; } Log;
static void cb(const Sim *S, long i, void *u) { ((Log *)u)->E[i] = sim_energy(S); }

static void run(int lossless)
{
    SimOpts o = {1, 1, lossless};
    Sim S;
    sim_init_instrument(&S, 96000.0, &o, "D3");
    S.bow_on = 0;
    sim_start(&S);
    /* pluck every string in both polarisations at different places */
    for (int i = 0; i < S.nstr; i++) {
        sim_pluck_string(&S, i, 0, (0.13 + 0.07 * i) * 0.69, 0.02 * 0.69, 1e-3);
        sim_pluck_string(&S, i, 1, (0.21 + 0.05 * i) * 0.69, 0.02 * 0.69, 5e-4);
    }
    long n = 96000;
    Log lg = {calloc((size_t)n, sizeof(double))};
    Trace *tr = sim_run(&S, 1.0, cb, &lg);
    double mx = 0, mn = 1e300, worst_up = 0;
    for (long i = 0; i < n; i++) {
        if (lg.E[i] > mx) mx = lg.E[i];
        if (lg.E[i] < mn) mn = lg.E[i];
        if (i > 0) {
            double up = (lg.E[i] - lg.E[i - 1]) / lg.E[0];
            if (up > worst_up) worst_up = up;
        }
    }
    /* how much energy actually sits in the body at the end (the test is vacuous if the coupling is nil) */
    double Ebody = body_energy(S.body);
    if (lossless) {
        double rel = (mx - mn) / lg.E[0];
        printf("T8 lossless: E0 %.6e J, relative spread %.3e (limit 1e-9), body energy at 1 s %.3e J (%.2f%% of total)\n", lg.E[0], rel, Ebody, 100.0 * Ebody / lg.E[n - 1]);
        CHECK(rel < 1e-9, "coupled energy not conserved: relative spread %g", rel);
        CHECK(Ebody > 1e-3 * lg.E[n - 1], "body holds %g of the energy: coupling is vacuous", Ebody / lg.E[n - 1]);
    } else {
        printf("T8 lossy: E0 %.6e -> %.6e J, largest relative step increase %.3e\n", lg.E[0], lg.E[n - 1], worst_up);
        CHECK(worst_up < 1e-12, "energy increased by relative %g in one step", worst_up);
        CHECK(lg.E[n - 1] < 0.9 * lg.E[0], "energy did not decay");
    }
    free(lg.E);
    trace_free(tr);
    sim_free(&S);
}

int main(void)
{
    run(1);
    run(0);
    TEST_DONE("test_body_energy");
}

/* fe_tool: diagnostics of the FE body option (fe_body.h).
 *   fe_tool cal              the crossover match scales: gain_a (FE-A) and gain_b (FE-B) such that the radiated pressure of the body in 2.0-2.5 kHz equals the shipped
 *                            body's under a white bridge force on every string; prints the values to put into instrument_cello.c
 *   fe_tool admit [FE]       |Y_uu| (m/s per N, dB) of the bridge notch of every string vs frequency, from the modal sum of the body, FE = 0 shipped, 1/2 the FE modes
 *   fe_tool rtf [FE]         the cost of body_step (real time factor of the body alone at 96 kHz, 4 white strings)
 * ALTCELLO_KNOBS applies. */
#include "body.h"
#include "fe_body.h"
#include "knobs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Instrument with(const Instrument *I, int variant, double ga, double gb)
{
    Instrument c = *I;
    c.body.fe_variant = variant;
    c.body.fe_gain_a = ga;
    c.body.fe_gain_b = gb;
    return c;
}

static void cal(const Instrument *I)
{
    const long n = 1L << 19;
    double ref = fe_white_psd(I, 2000.0, 2500.0, n);
    printf("shipped body: PSD 2.0-2.5 kHz %.6e\n", ref);
    for (int v = 1; v <= 2; v++) {
        double g = 1.0;
        for (int it = 0; it < 6; it++) {
            Instrument c = with(I, v, v == 1 ? g : 1.0, v == 2 ? g : 1.0);
            double P = fe_white_psd(&c, 2000.0, 2500.0, n);
            printf("  FE-%c gain %.6f: PSD %.6e (%+.2f dB re shipped)\n", 'A' + v - 1, g, P, 10.0 * log10(P / ref));
            g *= sqrt(ref / P);
        }
        printf("FE-%c: gain %.6f\n", 'A' + v - 1, g);
    }
}

static void admit(const Instrument *I)
{
    BodyModes *bm = body_modes_build(I);
    int nfe = bm->nfe;
    printf("# f_Hz then dB of |Y_uu| (m/s per N) of string C G D A\n");
    for (double f = 40.0; f < 3000.0; f *= 1.002) {
        double w = 2.0 * M_PI * f;
        printf("%.3f", f);
        for (int s = 0; s < BODY_NS; s++) {
            double yr = 0.0, yi = 0.0;
            for (int k = 0; k < bm->n; k++) {
                double wk = 2.0 * M_PI * bm->f[k], c = k < nfe ? bm->cus[s * nfe + k] : bm->cu[k];
                /* i w c^2 / (m (wk^2 - w^2 + i w wk / Q)) */
                double dr = wk * wk - w * w, di = w * wk / bm->Q[k], d2 = dr * dr + di * di, a = c * c / bm->m[k];
                /* i w (dr - i di) / d2 = (w di + i w dr) / d2 */
                yr += a * w * di / d2;
                yi += a * w * dr / d2;
            }
            printf(" %.3f", 20.0 * log10(hypot(yr, yi) + 1e-300));
        }
        printf("\n");
    }
    body_modes_free(bm);
}

static void rtf(const Instrument *I)
{
    const double fs = 96000.0;
    BodyModes *bm = body_modes_build(I);
    Body *b = body_new(I, bm, fs, 0, NULL);
    long n = 96000 * 4;
    clock_t t0 = clock();
    double acc = 0.0;
    for (long i = 0; i < n; i++) {
        double Fu[BODY_NS] = {1e-3 * sin(0.01 * i), 0, 0, 0}, Fw[BODY_NS] = {0};
        if (b->nfe) body_step_str(b, Fu, Fw);
        else body_step(b, Fu[0], 0.0);
        acc += b->p;
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("body only: %d modes (%d FE): %.2f s cpu for %.1f s audio, real time factor %.1f (sink %g)\n", bm->n, bm->nfe, s, n / fs, n / fs / s, acc);
    body_free(b);
    body_modes_free(bm);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: fe_tool cal | admit [FE] | rtf [FE]\n"); return 2; }
    knobs_install();
    const Instrument *I = instrument_cello();
    if (!strcmp(argv[1], "cal")) { cal(I); return 0; }
    Instrument c = with(I, argc > 2 ? atoi(argv[2]) : I->body.fe_variant, I->body.fe_gain_a, I->body.fe_gain_b);
    if (!strcmp(argv[1], "admit")) admit(&c);
    else if (!strcmp(argv[1], "rtf")) rtf(&c);
    else return 2;
    return 0;
}

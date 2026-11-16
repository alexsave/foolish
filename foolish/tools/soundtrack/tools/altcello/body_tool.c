/* body_tool: the body's response to a white bridge force (the transfer a bowed string sees), per third octave: the bridge velocity per unit force (admittance, dB), the
 * radiated pressure (dB, arbitrary reference) and its two paths (the compact modes, the plate modes with G_PLATE). ALTCELLO_KNOBS applies (stat_f1, eta_exp, mass_exp,
 * bridge_r, ...).      body_tool   ->   rows: fc_Hz Y_dB p_dB p_compact_dB p_plate_dB                                                                    */
#include "body.h"
#include "fft.h"
#include "knobs.h"
#include "rng.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void band_db(const double *x, int N, double fs, double fc, double *out)
{
    (void)x; (void)N; (void)fs; (void)fc; (void)out;
}

int main(void)
{
    knobs_install();
    const Instrument *I = instrument_cello();
    BodyModes *bm = body_modes_build(I);
    double fs = 96000.0;
    Body *b = body_new(I, bm, fs, 0, NULL);
    int N = 1 << 19;
    double *sig[4];
    for (int i = 0; i < 4; i++) sig[i] = calloc((size_t)N, sizeof(double));
    double *im = calloc((size_t)N, sizeof(double));
    Rng r;
    rng_seed(&r, ALTCELLO_SEED);
    for (int n = 0; n < N; n++) {
        body_step(b, rng_gauss(&r), 0.0);
        sig[0][n] = b->vu;
        sig[1][n] = b->p;
        sig[2][n] = b->p_compact;
        sig[3][n] = b->rad.g_plate * b->p_plate;
    }
    double *ps[4];
    for (int i = 0; i < 4; i++) {
        ps[i] = malloc(sizeof(double) * (size_t)(N / 2));
        for (int n = 0; n < N; n++) { sig[i][n] *= 0.5 * (1.0 - cos(2.0 * M_PI * (n + 0.5) / N)); im[n] = 0.0; }
        fft(sig[i], im, N);
        for (int k = 0; k < N / 2; k++) ps[i][k] = sig[i][k] * sig[i][k] + im[k] * im[k];
    }
    double df = fs / N;
    (void)band_db;
    for (int kk = 20; kk <= 42; kk++) {
        double fc = pow(10.0, kk / 10.0), lo = fc / pow(2.0, 1.0 / 6.0), hi = fc * pow(2.0, 1.0 / 6.0);
        double a[4] = {0, 0, 0, 0};
        int m = 0;
        for (int k = (int)(lo / df); k < (int)(hi / df) && k < N / 2; k++) {
            for (int i = 0; i < 4; i++) a[i] += ps[i][k];
            m++;
        }
        if (!m) continue;
        printf("%8.1f", fc);
        for (int i = 0; i < 4; i++) printf(" %8.2f", 10.0 * log10(a[i] / m + 1e-300));
        printf("\n");
    }
    return 0;
}

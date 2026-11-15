/* T9 bridge admittance of the body alone: a unit impulse force in u (and in w),
 * FFT of the bridge velocity. Passivity: Re Y_uu >= 0 and Re Y_ww >= 0 up to
 * 20 kHz. The peaks of |Y| for A0, CBR, B1-, B1+ land within 1 percent of the
 * table, read in the direction where the mode couples most strongly (c_u vs
 * c_w): in the weaker direction the skirts of the neighbouring modes pull the
 * peak (Y_uu of A0, c_u = 0.3, peaks 1.27 percent low; printed too). */
#include "body.h"
#include "fft.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void admittance(const BodyModes *bm, double fs, int pol, double *re, double *im, int N)
{
    double k = 1.0 / fs;
    Body *b = body_new(instrument_cello(), bm, fs, 0, NULL);
    for (int n = 0; n < N; n++) {
        body_step(b, pol == 0 && n == 0 ? 1.0 / k : 0.0, pol == 1 && n == 0 ? 1.0 / k : 0.0);
        re[n] = (pol == 0 ? b->vu : b->vw) * k;
        im[n] = 0;
    }
    fft(re, im, N);
    body_free(b);
}

int main(void)
{
    double fs = 96000.0;
    BodyModes *bm = body_modes_build(instrument_cello());
    printf("body: %d modes (%d table, %d statistical), f up to %.0f Hz\n", bm->n, bm->nlead, bm->n - bm->nlead, bm->f[bm->n - 1]);
    int N = 1 << 18; /* 2.7 s */
    double *re[2], *im[2];
    for (int p = 0; p < 2; p++) {
        re[p] = calloc((size_t)N, sizeof(double));
        im[p] = calloc((size_t)N, sizeof(double));
        admittance(bm, fs, p, re[p], im[p], N);
    }
    double df = fs / N;
    for (int p = 0; p < 2; p++) {
        double maxY = 0, worst = 1e300, wf = 0;
        for (int i = 1; i < N / 2; i++) {
            double a = hypot(re[p][i], im[p][i]);
            if (a > maxY) maxY = a;
        }
        for (int i = 1; i <= (int)(20000.0 / df); i++)
            if (re[p][i] < worst) { worst = re[p][i]; wf = i * df; }
        printf("T9 min Re Y_%s up to 20 kHz: %.3e s/kg at %.1f Hz (max |Y| %.3e)\n", p ? "ww" : "uu", worst, wf, maxY);
        CHECK(worst >= -1e-9 * maxY, "Re Y_%s = %g < 0 at %.1f Hz (passivity)", p ? "ww" : "uu", worst, wf);
    }
    for (int m = 0; m < 4; m++) {
        double f = bm->f[m];
        double fpk[2];
        for (int p = 0; p < 2; p++) {
            int lo = (int)(f * 0.95 / df), hi = (int)(f * 1.05 / df), best = lo;
            for (int i = lo; i <= hi; i++)
                if (hypot(re[p][i], im[p][i]) > hypot(re[p][best], im[p][best])) best = i;
            fpk[p] = best * df;
        }
        int strong = bm->cw[m] > bm->cu[m] ? 1 : 0;
        double err = 100.0 * (fpk[strong] - f) / f;
        printf("T9 %-4s table %.1f Hz: Y_uu peak %.2f Hz (%+.3f %%), Y_ww peak %.2f Hz (%+.3f %%); asserted in %s: %+.3f %%\n", body_mode_name(instrument_cello(), m), f,
               fpk[0], 100.0 * (fpk[0] - f) / f, fpk[1], 100.0 * (fpk[1] - f) / f, strong ? "w" : "u", err);
        CHECK(fabs(err) < 1.0, "%s peak at %.2f Hz is %.3f%% from %.1f Hz", body_mode_name(instrument_cello(), m), fpk[strong], err, f);
    }
    for (int p = 0; p < 2; p++) {
        free(re[p]);
        free(im[p]);
    }
    body_modes_free(bm);
    TEST_DONE("test_admittance");
}

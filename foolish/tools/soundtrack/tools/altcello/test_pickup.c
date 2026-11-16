/* T35 contact pickup (pickup.h): the piezo output is the derivative of the velocity through a one-pole high-pass at fc = 1/(2 pi R C).
 * Synthetic sine velocity at 96 kHz; the steady-state amplitude of the output is compared with the analytic 2 pi f * (f/fc) / sqrt(1 + (f/fc)^2)
 * and the phase with the analytic lead (cos(wt) for the derivative, plus atan(fc/f) from the high-pass). Constants: 1 nF into 10 MOhm 15.9 Hz, into 1 MOhm 159 Hz. */
#include "pickup.h"
#include "test_common.h"
#include <math.h>
#include <stdlib.h>

static void measure(double f, double fc, double *amp, double *ph)
{
    const double fs = 96000.0;
    long n = 96000;
    double *v = malloc(sizeof(double) * (size_t)n), *y = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) v[i] = sin(2.0 * M_PI * f * (double)i / fs);
    pickup_accel_hp(v, n, fs, fc, y);
    double s = 0.0, c = 0.0;
    long a = n / 2, m = 0; /* second half: the high-pass transient has gone */
    for (long i = a; i < n; i++) { double w = 2.0 * M_PI * f * (double)i / fs; s += y[i] * sin(w); c += y[i] * cos(w); m++; }
    *amp = 2.0 * sqrt(s * s + c * c) / (double)m;
    *ph = atan2(s, c); /* y = A cos(wt + lead) has s = -A sin(lead), c = A cos(lead) */
    free(v);
    free(y);
}

int main(void)
{
    CHECK(fabs(pickup_corner_hz(1e-9, 1e7) - 15.9155) < 0.01, "10 MOhm corner %.3f", pickup_corner_hz(1e-9, 1e7));
    CHECK(fabs(pickup_corner_hz(1e-9, 1e6) - 159.155) < 0.1, "1 MOhm corner %.3f", pickup_corner_hz(1e-9, 1e6));
    const double fcs[2] = {15.9155, 159.155};
    const double fr[5] = {40.0, 159.155, 500.0, 1000.0, 2000.0};
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 5; j++) {
            double f = fr[j], fc = fcs[k], A, ph;
            measure(f, fc, &A, &ph);
            double ref = 2.0 * M_PI * f * (f / fc) / sqrt(1.0 + (f / fc) * (f / fc));
            double lead = atan2(fc, f); /* the high-pass phase lead on top of the derivative's cos(wt) */
            double expect_ph = -lead; /* y = ref cos(wt + lead): the atan2(sin, cos) projections give -lead */
            CHECK(fabs(A / ref - 1.0) < 0.012, "fc %.1f f %.1f amplitude ratio %.4f", fc, f, A / ref);
            CHECK(fabs(ph - expect_ph) < 0.03, "fc %.1f f %.1f phase %.4f want %.4f", fc, f, ph, expect_ph);
        }
    /* below the corner the output falls 12 dB per octave of velocity frequency (the high-pass 6 dB on top of the rising derivative: gain ~ f^2) */
    double a1, a2, p;
    measure(2.0, 159.155, &a1, &p);
    measure(4.0, 159.155, &a2, &p);
    CHECK(fabs(a2 / a1 - 4.0) < 0.1, "f^2 below the corner: ratio %.3f", a2 / a1);
    TEST_DONE("test_pickup");
}

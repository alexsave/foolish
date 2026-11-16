#include "pickup.h"
#include <math.h>

double pickup_corner_hz(double c_farad, double r_ohm) { return 1.0 / (2.0 * M_PI * r_ohm * c_farad); }

void pickup_accel_hp(const double *v, long n, double fs, double fc, double *out)
{
    double rc = 1.0 / (2.0 * M_PI * fc), a = rc / (rc + 1.0 / fs);
    double y = 0.0, xp = 0.0;
    for (long i = 0; i < n; i++) {
        double x = (i + 1 < n ? v[i + 1] : v[i]) - (i > 0 ? v[i - 1] : v[i]); /* central difference times fs / 2 */
        x *= 0.5 * fs;
        y = a * (y + x - xp);
        xp = x;
        out[i] = y;
    }
}

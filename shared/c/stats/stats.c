/* stats.c - see stats.h. */
#include "stats.h"
#include <math.h>

void stat_add(StatSums *s, double x)
{
    s->n += 1;
    s->sum += x;
    s->sum2 += x * x;
}

double stat_mean(const StatSums *s)
{
    return s->n > 0 ? s->sum / s->n : 0;
}

double stat_variance(const StatSums *s)
{
    if (s->n < 2) return 0;
    double v = (s->sum2 - s->sum * s->sum / s->n) / (s->n - 1);
    return v > 0 ? v : 0;
}

double stat_stderr(const StatSums *s)
{
    return s->n > 0 ? sqrt(stat_variance(s) / s->n) : 0;
}

void stat_wilson(double k, double n, double z, double *lo, double *hi)
{
    if (!(n > 0)) {
        *lo = 0;
        *hi = 1;
        return;
    }
    double p = k / n, d = 1 + z * z / n;
    double c = (p + z * z / (2 * n)) / d, h = z * sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d;
    *lo = c - h;
    *hi = c + h;
}

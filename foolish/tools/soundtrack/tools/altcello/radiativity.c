#include "radiativity.h"
#include <math.h>

double radiativity_eval_db(double f)
{
    if (f <= radiativity_f[0]) return radiativity_db[0];
    if (f >= radiativity_f[RAD_NPTS - 1]) return radiativity_db[RAD_NPTS - 1];
    int j = 0;
    while (f > radiativity_f[j + 1]) j++;
    double x = (log2(f) - log2(radiativity_f[j])) / (log2(radiativity_f[j + 1]) - log2(radiativity_f[j]));
    return radiativity_db[j] + x * (radiativity_db[j + 1] - radiativity_db[j]);
}

double radiativity_gain(double f) { return pow(10.0, radiativity_eval_db(f) / 20.0); }

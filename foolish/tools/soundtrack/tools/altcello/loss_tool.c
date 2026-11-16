/* loss_tool [STRING] [SECS]: the partial T60 of one or all strings of the cello (a pluck, rigid bridge, lossmeas.h) against the model curve string_alpha:
 *   string p f_Hz alpha_measured alpha_model T60_s Q_measured
 * The default instrument is used (ALTCELLO_KNOBS applies, e.g. loss=0 for the earlier sigma1 k^2 loss). */
#include "knobs.h"
#include "lossmeas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    knobs_install();
    const Instrument *I = instrument_cello();
    double secs = argc > 2 ? atof(argv[2]) : 3.0;
    for (int i = 0; i < I->nstr; i++) {
        if (argc > 1 && strcmp(argv[1], "all") && strcmp(argv[1], I->str[i].name)) continue;
        double f[400], a[400];
        int n = string_partial_decay(&I->str[i], 96000.0, secs, 200, f, a);
        for (int p = 1; p <= n; p++) {
            if (f[p - 1] > 16000.0) break;
            printf("%s %3d %9.2f %9.3f %9.3f %9.4f %8.1f\n", I->str[i].name, p, f[p - 1], a[p - 1], string_alpha(&I->str[i], f[p - 1]), a[p - 1] > 0 ? log(1000.0) / a[p - 1] : -1.0,
                   a[p - 1] > 0 ? M_PI * f[p - 1] / a[p - 1] : -1.0);
        }
    }
    return 0;
}

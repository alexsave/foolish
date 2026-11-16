/* T31 no sound before the bow moves (richness F0): the expert's force lands softly, so the stopped notes G2 on C, D3 on G and A3 on D have less than -30 dB
 * (re their sustain) in the 45 ms before the note's start, where a 5 ms force step rang the string at -23 to -27 dB (the "blob before the note" of the dispatcher's spectrogram). */
#include "render.h"
#include "test_common.h"
#include <math.h>
#include <stdlib.h>

static double rms_db(const float *x, double a, double b)
{
    long i0 = (long)(a * 48000.0), i1 = (long)(b * 48000.0);
    double s = 0.0;
    for (long i = i0; i < i1; i++) s += (double)x[i] * x[i];
    return 10.0 * log10(s / (double)(i1 - i0) + 1e-300);
}

int main(void)
{
    int midi[3] = {43, 50, 57}, str[3] = {0, 1, 2};
    for (int k = 0; k < 3; k++) {
        Note n = {0.5, 1.5, midi[k], 0.85, str[k], 1, -1.0, -1.0, 0};
        RenderOpts o;
        render_opts_default(&o);
        o.rt60 = -1.0;
        float *pcm;
        long n48;
        Played pl;
        double rtf;
        CHECK(render_notes(&n, 1, &o, &pcm, &n48, &pl, &rtf) == 0, "render failed");
        double pre = rms_db(pcm, 0.45, 0.495), sus = rms_db(pcm, 1.0, 1.4);
        printf("T31 midi %d: level 45 ms before the start %.1f dB re the sustain\n", midi[k], pre - sus);
        CHECK(pre - sus < -30.0, "midi %d: %.1f dB re sustain before the note starts", midi[k], pre - sus);
        free(pcm);
        played_free(&pl);
    }
    TEST_DONE("test_preonset");
}

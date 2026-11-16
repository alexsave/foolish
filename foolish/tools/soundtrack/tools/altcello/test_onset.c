/* T28 the audio time-to-periodic measure (analysis.h: audio_onset, audio_periodic_time, pitch_track) on synthetic sounds.
 *  a: a periodic harmonic tone whose level swells over 400 ms (raised cosine) is periodic at once: under 0.1 s
 *  b: the same tone after 300 ms of noise (a rough start): over 0.3 s, and the noise alone never gives a periodic run
 *  b2: a burst of only 4 periods in noise gives no run of 10 periods
 *  d: a slurred finger change D3 to E3 on the G string with one continuous bow (legato.h): the pitch is within 10 cents of the new pitch for 5
 *     periods within SLUR_BOUND_S of the start of the finger event, for two bow-noise seeds, and the sound was periodic before;
 *  c: a tone with a pitch step: the pitch track finds the new pitch, and the period correlation at the new pitch is periodic again
 *     within a few periods of the step. */
#include "analysis.h"
#include "legato.h"
#include "rng.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FS 48000.0
/* measured by charts/finger_study.py (D3 to E3 on G2, 3 seeds, all strings): 0.116 - 0.170 s from the start of the 10 ms finger event; the bound is
   the largest measured value rounded up */
#define SLUR_BOUND_S 0.20

static double tone(double f0, double t)
{
    double s = 0.0;
    for (int k = 1; k <= 10; k++) s += sin(2.0 * M_PI * k * f0 * t + 0.3 * k) / k;
    return s;
}

int main(void)
{
    long n = (long)(1.5 * FS);
    double *x = calloc((size_t)n, sizeof(double));
    double f0 = 146.83;
    /* a: swell */
    for (long i = 0; i < n; i++) {
        double t = (double)i / FS, e = t < 0.4 ? 0.5 * (1.0 - cos(M_PI * t / 0.4)) : 1.0;
        x[i] = 0.01 + e * tone(f0, t) * 1.0 * (t > 0.0);
        x[i] -= 0.01;
    }
    double on = audio_onset(x, n, FS);
    double tp = audio_periodic_time(x, n, FS, f0, on, 0.95, 10);
    printf("swell tone: onset %.3f s, time to periodic %.4f s\n", on, tp - on);
    CHECK(on >= 0.0 && tp >= 0.0 && tp - on < 0.1, "tone with a 400 ms swell: time to periodic %.4f s, want under 0.1", tp - on);
    /* b: 300 ms of noise first */
    Rng r;
    rng_seed(&r, 12345);
    for (long i = 0; i < n; i++) {
        double t = (double)i / FS;
        x[i] = t < 0.3 ? 0.6 * rng_gauss(&r) : tone(f0, t);
    }
    on = audio_onset(x, n, FS);
    tp = audio_periodic_time(x, n, FS, f0, on, 0.95, 10);
    printf("noise then tone: onset %.3f s, time to periodic %.4f s\n", on, tp - on);
    CHECK(on >= 0.0 && on < 0.02 && tp >= 0.0 && tp - on > 0.3, "300 ms of noise then a tone: time to periodic %.4f s, want over 0.3", tp - on);
    for (long i = 0; i < n; i++) x[i] = 0.6 * rng_gauss(&r);
    tp = audio_periodic_time(x, n, FS, f0, 0.0, 0.95, 10);
    CHECK(tp < 0.0, "white noise alone is periodic from %.3f s", tp);
    /* a periodic burst of only 4 periods in noise is not 10 periods of periodicity */
    for (long i = 0; i < n; i++) {
        double t = (double)i / FS;
        x[i] = t >= 0.5 && t < 0.5 + 4.0 / f0 ? tone(f0, t) : 0.6 * rng_gauss(&r);
    }
    tp = audio_periodic_time(x, n, FS, f0, 0.0, 0.95, 10);
    CHECK(tp < 0.0, "a burst of 4 periods is periodic for 10 from %.3f s", tp);
    /* c: pitch step at 0.5 s from 146.83 to 164.81 Hz (two semitones), phase continuous */
    double fa = 146.83, fb = 164.81, ph = 0.0;
    for (long i = 0; i < n; i++) {
        double t = (double)i / FS, f = t < 0.5 ? fa : fb;
        ph += 2.0 * M_PI * f / FS;
        double s = 0.0;
        for (int k = 1; k <= 10; k++) s += sin(k * ph) / k;
        x[i] = s;
    }
    double tc[2000], fq[2000];
    long m = pitch_track(x, n, FS, 0.8 * fa, 1.2 * fb, 2.0 / fb, 0.4, 0.8, 0.0005, tc, fq, 2000);
    double t_in = -1.0;
    long run = 0;
    for (long k = 0; k < m; k++) {
        if (tc[k] < 0.5) continue;
        run = fabs(cents(fq[k], fb)) < 10.0 ? run + 1 : 0;
        if (run >= 40) { t_in = tc[k - 39] - 0.5; break; }
    }
    printf("pitch step: 10 cent hold for 20 ms reached %.4f s after the step\n", t_in);
    CHECK(t_in >= 0.0 && t_in < 0.02, "pitch step found %.4f s after the step", t_in);
    double tr = audio_periodic_time(x, n, FS, fb, 0.5, 0.95, 10) - 0.5;
    printf("pitch step: periodic at the new pitch %.4f s after the step\n", tr);
    CHECK(tr >= 0.0 && tr < 0.03, "periodic at the new pitch only %.4f s after the step", tr);
    free(x);
    for (int seed = 0; seed < 2; seed++) {
        LegatoArgs a;
        CHECK(legato_default(&a, "slur", "G2") == 0, "legato_default");
        a.seed = (uint64_t)seed;
        LegatoOut o;
        CHECK(legato_run(&a, &o) == 0, "legato_run");
        printf("slur D3 to E3 on G2, seed %d: pre_ok %d, pitch within 10 c after %.4f s (final %.2f Hz, %+.1f c re nominal), periodic %.4f s\n", seed, o.pre_ok, o.t_pitch, o.f_fin,
               o.cents_nominal, o.t_reper);
        CHECK(o.pre_ok, "seed %d: no Helmholtz motion before the finger change", seed);
        CHECK(fabs(o.cents_nominal) < 15.0, "seed %d: the new pitch is %+.1f cents from E3", seed, o.cents_nominal);
        CHECK(o.t_pitch >= 0.0 && o.t_pitch <= SLUR_BOUND_S, "seed %d: the new pitch is reached after %.4f s, bound %.2f", seed, o.t_pitch, SLUR_BOUND_S);
        legato_out_free(&o);
    }
    TEST_DONE("test_onset");
}

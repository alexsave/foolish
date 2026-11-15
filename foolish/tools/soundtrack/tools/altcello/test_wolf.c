/* T13 wolf probe (report only, nothing asserted): bow the stopped F3 (3 semitones
 * up on D3, 174.6 Hz, close to B1- at 178 Hz) with the body on, at a few
 * forces. Prints the slip-interval pattern (alternating long/short spacing
 * means period doubling) and the beating of the bridge force envelope. */
#include "analysis.h"
#include "fft.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const StringSpec *sp = string_spec("D3");
    double xF = sp->L * (1.0 - pow(2.0, -3.0 / 12.0));
    double fF3 = sp->f0 * pow(2.0, 3.0 / 12.0);
    double Fs[5] = {0.1, 0.2, 0.35, 0.5, 0.8};
    printf("T13 wolf probe: D3 stopped at x_F %.4f m (F3 %.2f Hz), body on, beta 0.1, v_B 0.1 m/s\n", xF, fF3);
    for (int c = 0; c < 2; c++)
        for (int fi = 0; fi < 5; fi++) {
            SimOpts o = {c == 0 ? 1 : 0, 0, 0};
            Sim S;
            sim_init_instrument(&S, 96000.0, &o, "D3");
            sim_set_bow(&S, 0.1, 0.1, Fs[fi], 0.03);
            env_const(&S.xF, xF);
            env_const(&S.aF, 1.0);
            sim_start(&S);
            Trace *tr = sim_run(&S, 7.0, NULL, NULL);
            double fs = tr->fs;
            long i0 = (long)(3.0 * fs);
            Spectrum *sp_ = spec_make(tr->ch[0] + i0, tr->n - i0, fs, 4);
            double f = spec_peak(sp_, fF3, 0.06, NULL);
            spec_free(sp_);
            double P = fs / f;
            /* slip onsets after 3 s and their spacing */
            int cnt = 0;
            long *on = malloc(sizeof(long) * 20000);
            long minlen = (long)ceil(SLIP_MIN_DUR_S * fs), i = i0;
            while (i < tr->n) {
                if (fabs(tr->ch[3][i]) > 1e-4) {
                    long j = i;
                    while (j < tr->n && fabs(tr->ch[3][j]) > 1e-4) j++;
                    if (j - i >= minlen && cnt < 20000) on[cnt++] = i;
                    i = j;
                } else i++;
            }
            double sum = 0, sum2 = 0, altsum = 0;
            for (int k = 0; k + 1 < cnt; k++) {
                double d = (on[k + 1] - on[k]) / P;
                sum += d;
                sum2 += d * d;
                if (k + 2 < cnt) altsum += ((on[k + 2] - on[k + 1]) - (on[k + 1] - on[k])) / P * ((k & 1) ? -1 : 1);
            }
            int m = cnt - 1;
            double mean = sum / m, sd = sqrt(sum2 / m - mean * mean);
            double alt = altsum / (m - 1);
            /* envelope of the bridge force: rms per 3 periods, spectrum 0.5..20 Hz */
            long W = (long)(3.0 * P + 0.5), nw = (tr->n - i0) / W;
            double *e = calloc((size_t)nw, sizeof(double)), em = 0;
            for (long w = 0; w < nw; w++) {
                double s = 0;
                for (long q = 0; q < W; q++) { double v = tr->ch[0][i0 + w * W + q]; s += v * v; }
                e[w] = sqrt(s / W);
                em += e[w];
            }
            em /= nw;
            double emax = 0, emin = 1e30;
            for (long w = 0; w < nw; w++) { if (e[w] > emax) emax = e[w]; if (e[w] < emin) emin = e[w]; }
            int NN = 256;
            double re[256] = {0}, im[256] = {0};
            for (long w = 0; w < nw && w < NN; w++) re[w] = (e[w] - em) * (0.5 - 0.5 * cos(2 * M_PI * (w + 0.5) / (nw < NN ? nw : NN)));
            fft(re, im, NN);
            int kb = 1;
            double bm = 0;
            for (int k = 1; k < NN / 2; k++) {
                double a = hypot(re[k], im[k]);
                if (a > bm) { bm = a; kb = k; }
            }
            printf("T13 %s F %.2f N: pitch %.2f Hz (%+.1f cents vs F3), slips %d, spacing/P mean %.3f sd %.3f, alternation %+.3f, envelope min/max %.2f, strongest envelope modulation %.2f Hz (rel depth %.2f)\n",
                   c == 0 ? "body" : "rigid", Fs[fi], f, cents(f, fF3), cnt, mean, sd, alt, emin / emax, kb / (NN * (double)W / fs), (emax - emin) / (emax + emin));
            free(on);
            free(e);
            trace_free(tr);
            sim_free(&S);
        }
    TEST_DONE("test_wolf");
}

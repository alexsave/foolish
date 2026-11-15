#include "analysis.h"
#include "fft.h"
#include <math.h>
#include <stdlib.h>

const char *const cls_names[4] = {"helmholtz", "multiple", "raucous", "none"};

double cents(double f, double ref) { return 1200.0 * log2(f / ref); }

Spectrum *spec_make(const double *x, long n, double fs, int padfactor)
{
    int N = 1;
    while (N < n * padfactor) N <<= 1;
    double *re = calloc((size_t)N, sizeof(double)), *im = calloc((size_t)N, sizeof(double));
    for (long i = 0; i < n; i++) re[i] = x[i] * 0.5 * (1.0 - cos(2.0 * M_PI * (i + 0.5) / n));
    fft(re, im, N);
    Spectrum *s = calloc(1, sizeof *s);
    s->n = N;
    s->df = fs / N;
    s->mag = malloc(sizeof(double) * (size_t)(N / 2));
    for (int i = 0; i < N / 2; i++) s->mag[i] = sqrt(re[i] * re[i] + im[i] * im[i]);
    free(re);
    free(im);
    return s;
}

void spec_free(Spectrum *s)
{
    if (!s) return;
    free(s->mag);
    free(s);
}

double spec_peak(const Spectrum *s, double f, double frac, double *mag)
{
    int lo = (int)floor(f * (1.0 - frac) / s->df), hi = (int)ceil(f * (1.0 + frac) / s->df);
    if (lo < 1) lo = 1;
    if (hi > s->n / 2 - 2) hi = s->n / 2 - 2;
    int best = lo;
    for (int i = lo; i <= hi; i++)
        if (s->mag[i] > s->mag[best]) best = i;
    double a = log(s->mag[best - 1] + 1e-300), b = log(s->mag[best] + 1e-300), c = log(s->mag[best + 1] + 1e-300);
    double den = a - 2.0 * b + c;
    double d = fabs(den) > 1e-300 ? 0.5 * (a - c) / den : 0.0;
    if (mag) *mag = s->mag[best];
    return (best + d) * s->df;
}

int slip_onsets(const double *v, long i0, long i1, double thr, double fs, double *frac)
{
    /* an interval is a maximal run of |v| > thr; runs shorter than
       SLIP_MIN_DUR_S are the stick-phase ripple micro-slips and are not
       counted (neither as an interval nor in the fraction) */
    long minlen = (long)ceil(SLIP_MIN_DUR_S * fs);
    int cnt = 0;
    long above = 0, i = i0;
    while (i < i1) {
        if (fabs(v[i]) > thr) {
            long j = i;
            while (j < i1 && fabs(v[j]) > thr) j++;
            if (j - i >= minlen && (i == i0 ? 0 : 1)) {
                cnt++;
                above += j - i;
            } else if (j - i >= minlen) {
                above += j - i; /* run cut by the window start: fraction only */
            }
            i = j;
        } else i++;
    }
    if (frac) *frac = (double)above / (double)(i1 - i0);
    return cnt;
}

double autocorr_max(const double *x, long i0, long i1, long lo, long hi, long *best)
{
    double mean = 0;
    for (long i = i0; i < i1 + hi; i++) mean += x[i];
    mean /= (double)(i1 - i0 + hi);
    double bestc = -2;
    for (long lag = lo; lag <= hi; lag++) {
        double sxy = 0, sxx = 0, syy = 0;
        for (long i = i0; i < i1; i++) {
            double a = x[i] - mean, b = x[i + lag] - mean;
            sxy += a * b;
            sxx += a * a;
            syy += b * b;
        }
        double c = sxy / sqrt(sxx * syy + 1e-300);
        if (c > bestc) {
            bestc = c;
            if (best) *best = lag;
        }
    }
    return bestc;
}

void classify_run(const double *bridge, const double *vrel, double fs, double f0, double t0, double t1, RunStats *out)
{
    long i0 = (long)(t0 * fs), i1 = (long)(t1 * fs);
    double frac;
    int on = slip_onsets(vrel, i0, i1, 1e-4, fs, &frac);
    double periods = (t1 - t0) * f0;
    long P = (long)(fs / f0 + 0.5);
    long len = i1 - i0 - (long)(1.1 * P);
    out->onsets_per_period = on / periods;
    out->slip_frac = frac;
    out->corr = autocorr_max(bridge, i0, i0 + len, (long)(0.95 * P), (long)(1.05 * P), NULL);
    if (out->onsets_per_period < 0.5) out->cls = CLS_NONE;
    else if (out->corr < 0.9) out->cls = CLS_RAUCOUS;
    else if (out->onsets_per_period <= 1.05) out->cls = CLS_HELMHOLTZ;
    else out->cls = CLS_MULTIPLE;
}

Helm analyse_helmholtz(const double *bridge, const double *vrel, long n, double fs, double f0, double settle)
{
    Helm h = {0};
    long i0 = (long)(settle * fs);
    Spectrum *sp = spec_make(bridge + i0, n - i0, fs, 8);
    h.f_est = spec_peak(sp, f0, 0.05, NULL);
    spec_free(sp);
    h.cents_flat = cents(h.f_est, f0);
    double P = fs / h.f_est;
    long i = i0;
    while (i < n - 1 && !(fabs(vrel[i]) <= 1e-4 && fabs(vrel[i + 1]) > 1e-4)) i++;
    long w0 = i + (long)(0.5 * P);
    h.counts_min = 1 << 20;
    h.counts_max = 0;
    for (int p = 0; p < 20; p++) {
        long a = w0 + (long)(p * P), b = w0 + (long)((p + 1) * P);
        if (b > n) { h.counts_min = -1; break; }
        double fr;
        int c = slip_onsets(vrel, a, b, 1e-4, fs, &fr);
        if (c < h.counts_min) h.counts_min = c;
        if (c > h.counts_max) h.counts_max = c;
    }
    h.ok_one_per_period = (h.counts_min == 1 && h.counts_max == 1);
    long w1 = w0 + (long)(20 * P);
    if (w1 <= n) slip_onsets(vrel, w0, w1, 1e-4, fs, &h.slip_frac);
    long best;
    if (w0 + (long)(11.1 * P) < n) h.corr = autocorr_max(bridge, w0, w0 + (long)(10 * P), (long)(0.95 * P), (long)(1.05 * P), &best);
    return h;
}

double settle_time(const double *vrel, long n, double fs, double P)
{
    long minlen = (long)ceil(SLIP_MIN_DUR_S * fs);
    long *on = malloc(sizeof(long) * (size_t)(n / 8 + 16));
    int cnt = 0;
    long i = 0;
    while (i < n) {
        if (fabs(vrel[i]) > 1e-4) {
            long j = i;
            while (j < n && fabs(vrel[j]) > 1e-4) j++;
            if (j - i >= minlen) on[cnt++] = i;
            i = j;
        } else i++;
    }
    double res = -1.0;
    for (int a = 0; a + 10 < cnt; a++) {
        int ok = 1;
        for (int b = a; b < a + 10; b++) {
            double d = (double)(on[b + 1] - on[b]);
            if (d < 0.85 * P || d > 1.15 * P) { ok = 0; break; }
        }
        if (ok) { res = (double)on[a] / fs; break; }
    }
    free(on);
    return res;
}

double hump_db(const double *x, long n, double fs, double f0, double flo, double fhi)
{
    long len = (long)fs;
    if (len > n) len = n;
    Spectrum *sp = spec_make(x + n - len, len, fs, 4);
    double m1;
    spec_peak(sp, f0, 0.05, &m1);
    int lo = (int)(flo / sp->df), hi = (int)(fhi / sp->df);
    double mx = 0;
    for (int i = lo; i <= hi; i++)
        if (sp->mag[i] > mx) mx = sp->mag[i];
    spec_free(sp);
    return 20.0 * log10(mx / m1);
}

SimpleHelm analyse_simple(const double *vrel, long n, double fs, double f_est, double settle, double frac_min)
{
    SimpleHelm r = {0, 0, 20, 0.0};
    long i0 = (long)(settle * fs);
    double P = fs / f_est;
    long i = i0;
    while (i < n - 1 && !(fabs(vrel[i]) <= 1e-4 && fabs(vrel[i + 1]) > 1e-4)) i++;
    long w0 = i + (long)(0.5 * P);
    double fr_sum = 0.0;
    for (int p = 0; p < 20; p++) {
        long a = w0 + (long)(p * P), b = w0 + (long)((p + 1) * P);
        if (b > n) { r.nper = p; r.bad_periods += 20 - p; break; }
        double area[64], dur[64];
        int cnt = 0;
        long j = a;
        while (j < b) {
            if (fabs(vrel[j]) > 1e-4) {
                long e = j;
                double ar = 0.0;
                while (e < b && fabs(vrel[e]) > 1e-4) { ar += fabs(vrel[e]) / fs; e++; }
                if (cnt < 64) { area[cnt] = ar; dur[cnt] = (double)(e - j); cnt++; }
                j = e;
            } else j++;
        }
        double mx = 0.0, md = 0.0;
        for (int c = 0; c < cnt; c++)
            if (area[c] > mx) { mx = area[c]; md = dur[c]; }
        int sig = 0;
        for (int c = 0; c < cnt; c++)
            if (mx > 0.0 && area[c] >= frac_min * mx) sig++;
        if (sig != 1) r.bad_periods++;
        fr_sum += md / P;
    }
    r.ok = (r.bad_periods == 0 && r.nper == 20);
    r.main_frac = fr_sum / 20.0;
    return r;
}

void partial_decays(const double *x, long n, double fs, double f0, int K, double t_start, double win_s, double range_db, double *f_out, double *alpha)
{
    long n2 = n < (long)(2.0 * fs) ? n : (long)(2.0 * fs);
    Spectrum *sp = spec_make(x, n2, fs, 16);
    long W = (long)(win_s * fs);
    long hop = W / 2;
    for (int k = 1; k <= K; k++) {
        double fk = spec_peak(sp, k * f0, 0.02, NULL);
        f_out[k - 1] = fk;
        alpha[k - 1] = 0.0;
        double w = 2.0 * M_PI * fk / fs, cr = cos(w), ci = sin(w);
        double lt[2048], la[2048];
        int m = 0;
        double first = 0.0;
        for (long s0 = (long)(t_start * fs); s0 + W <= n && m < 2048; s0 += hop) {
            double re = 0.0, im = 0.0, pr = 1.0, pi_ = 0.0;
            /* phasor rotated from the window start */
            double ph0 = w * (double)s0;
            pr = cos(ph0);
            pi_ = -sin(ph0);
            for (long t = 0; t < W; t++) {
                double wt = 0.5 * (1.0 - cos(2.0 * M_PI * ((double)t + 0.5) / (double)W));
                re += x[s0 + t] * wt * pr;
                im += x[s0 + t] * wt * pi_;
                double nr = pr * cr + pi_ * ci, ni = pi_ * cr - pr * ci; /* times e^{-iw} */
                pr = nr;
                pi_ = ni;
            }
            double amp = sqrt(re * re + im * im);
            if (m == 0) first = amp;
            if (amp < first * pow(10.0, -range_db / 20.0) || amp <= 0.0) break;
            lt[m] = ((double)s0 + 0.5 * (double)W) / fs;
            la[m] = log(amp);
            m++;
        }
        if (m < 4) continue;
        double st = 0, sa = 0, stt = 0, sta = 0;
        for (int q = 0; q < m; q++) { st += lt[q]; sa += la[q]; stt += lt[q] * lt[q]; sta += lt[q] * la[q]; }
        double den = m * stt - st * st;
        if (den != 0.0) alpha[k - 1] = -(m * sta - st * sa) / den;
    }
    spec_free(sp);
}

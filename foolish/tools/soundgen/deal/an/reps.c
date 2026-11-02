// reps: split a recording of one action repeated several times into its reps,
// write each rep as a WAV, and describe each rep's timeline and spectrum.
// usage: reps in.wav outdir [min_gap_s] [prominence_db]
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR 480                  // 10 ms frames at 48 kHz
#define NFFT 4096

static int16_t *pcm; static long n; static int rate;
static double *x, *lo, *hi;     // high-passed signal, 150-1500 Hz band, 2-12 kHz band

static int load(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) return -1; fclose(f);
    for (long i = 12; i + 8 < sz;) {
        uint32_t len = b[i+4] | b[i+5]<<8 | b[i+6]<<16 | (uint32_t)b[i+7]<<24;
        if (!memcmp(b+i, "fmt ", 4)) rate = b[i+12] | b[i+13]<<8 | b[i+14]<<16;
        if (!memcmp(b+i, "data", 4)) { n = len/2; pcm = (int16_t *)(b+i+8); return 0; }
        i += 8 + len + (len & 1);
    }
    return -1;
}
typedef struct { double b0,b1,b2,a1,a2,x1,x2,y1,y2; } Bq;
static Bq bq(int hp, double fc) { double w=2*M_PI*fc/rate, c=cos(w), al=sin(w)/(2*0.7071), a0=1+al; Bq q={0};
    if (hp) { q.b0=(1+c)/2/a0; q.b1=-(1+c)/a0; q.b2=q.b0; } else { q.b0=(1-c)/2/a0; q.b1=(1-c)/a0; q.b2=q.b0; }
    q.a1=-2*c/a0; q.a2=(1-al)/a0; return q; }
static double run(Bq *q, double v) { double y=q->b0*v+q->b1*q->x1+q->b2*q->x2-q->a1*q->y1-q->a2*q->y2; q->x2=q->x1; q->x1=v; q->y2=q->y1; q->y1=y; return y; }
static double db(double v) { return 20*log10(v > 1e-12 ? v : 1e-12); }

static void fft(double *re, double *im, int m) {
    for (int i = 1, j = 0; i < m; i++) { int bit = m >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (int len = 2; len <= m; len <<= 1) { double a = -2*M_PI/len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < m; i += len) { double cr = 1, ci = 0;
            for (int k = 0; k < len/2; k++) { int p = i+k, q = p+len/2;
                double vr = re[q]*cr - im[q]*ci, vi = re[q]*ci + im[q]*cr;
                re[q] = re[p]-vr; im[q] = im[p]-vi; re[p] += vr; im[p] += vi;
                double t = cr*wr - ci*wi; ci = cr*wi + ci*wr; cr = t; } } }
}

static void wav(const char *path, long a, long b) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    uint32_t nb = (uint32_t)(b - a) * 2, r = rate, br = rate * 2, sz = 36 + nb; uint16_t one = 1, two = 2, sixteen = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fl = 16; fwrite(&fl, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f);
    fwrite(&r, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&two, 2, 1, f); fwrite(&sixteen, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&nb, 4, 1, f); fwrite(pcm + a, 2, b - a, f); fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3 || load(argv[1])) { fprintf(stderr, "usage: reps in.wav outdir [gap_s] [prom_db]\n"); return 1; }
    double gap_s = argc > 3 ? atof(argv[3]) : 0.5, prom = argc > 4 ? atof(argv[4]) : 12;
    Bq h1 = bq(1,150), h2 = bq(1,150), l1 = bq(0,1500), l2 = bq(0,1500), g1 = bq(1,2000), g2 = bq(1,2000), k1 = bq(0,12000), k2 = bq(0,12000);
    x = malloc(n * sizeof *x); lo = malloc(n * sizeof *lo); hi = malloc(n * sizeof *hi);
    for (long i = 0; i < n; i++) { x[i] = run(&h2, run(&h1, pcm[i] / 32768.0)); lo[i] = run(&l2, run(&l1, x[i])); hi[i] = run(&k2, run(&k1, run(&g2, run(&g1, x[i])))); }
    long nf = n / FR;
    double *e = calloc(nf, sizeof *e), *el = calloc(nf, sizeof *el), *eh = calloc(nf, sizeof *eh), *sm = calloc(nf, sizeof *sm);
    for (long f = 0; f < nf; f++) { double s = 0, sl = 0, sh = 0;
        for (int k = 0; k < FR; k++) { long i = f*FR + k; s += x[i]*x[i]; sl += lo[i]*lo[i]; sh += hi[i]*hi[i]; }
        e[f] = s / FR; el[f] = sl / FR; eh[f] = sh / FR; }
    // smoothed envelope in dB (150 ms box on energy)
    for (long f = 0; f < nf; f++) { double s = 0; int c = 0; for (long k = f - 7; k <= f + 7; k++) if (k >= 0 && k < nf) { s += e[k]; c++; } sm[f] = db(sqrt(s / c)); }
    double *srt = malloc(nf * sizeof *srt); memcpy(srt, sm, nf * sizeof *srt);
    for (long i = 1; i < nf; i++) { double v = srt[i]; long j = i; while (j && srt[j-1] > v) { srt[j] = srt[j-1]; j--; } srt[j] = v; }
    double floor_db = srt[nf / 5], top = srt[nf - 1];
    // peaks: local max over +-gap, at least `prom` dB above the floor and within 30 dB of the loudest
    long g = (long)(gap_s * 100), pk[256]; int np = 0;
    for (long f = 0; f < nf && np < 256; f++) {
        if (sm[f] < floor_db + prom || sm[f] < top - 30) continue;
        int best = 1; for (long k = f - g; k <= f + g; k++) if (k >= 0 && k < nf && k != f && (sm[k] > sm[f] || (sm[k] == sm[f] && k < f))) { best = 0; break; }
        if (best) pk[np++] = f;
    }
    printf("%s: %.1f s, floor %.1f dB, loudest %.1f dB, %d reps\n", argv[1], (double)n / rate, floor_db, top, np);
    double sum[10] = {0}, sq[10] = {0};
    for (int r = 0; r < np; r++) {
        // bounds: the quietest point between neighbouring peaks, then trimmed to where the rep is 25 dB under its peak
        long lb = r ? pk[r-1] : 0, rb = r + 1 < np ? pk[r+1] : nf - 1;
        long a = pk[r], b = pk[r];
        for (long k = pk[r]; k > lb; k--) if (sm[k] < sm[a]) a = k;
        for (long k = pk[r]; k < rb; k++) if (sm[k] < sm[b]) b = k;
        double pkdb = -999; long pf = a; for (long k = a; k <= b; k++) { double v = db(sqrt(e[k])); if (v > pkdb) { pkdb = v; pf = k; } }
        long s = pf, t = pf;
        while (s > a && db(sqrt(e[s-1])) > pkdb - 25) s--;
        while (t < b && db(sqrt(e[t+1])) > pkdb - 25) t++;
        // include up to 60 ms more either side for the WAV
        long wa = s > 6 ? s - 6 : 0, wb = t + 6 < nf ? t + 6 : nf - 1;
        char path[1024]; snprintf(path, sizeof path, "%s/rep%02d.wav", argv[2], r + 1); wav(path, wa * FR, (wb + 1) * FR);
        // timeline: per 10 ms, L if the low band dominates (>6 dB over high), H if high dominates, B both, . quiet (< peak-25)
        double lpk = 0, hpk = 0; for (long k = s; k <= t; k++) { if (el[k] > lpk) lpk = el[k]; if (eh[k] > hpk) hpk = eh[k]; }
        char tl[400]; int m = 0;
        for (long k = s; k <= t && m < 399; k++) {
            double v = db(sqrt(e[k])), dl = 10*log10(el[k] + 1e-24), dh = 10*log10(eh[k] + 1e-24);
            tl[m++] = v < pkdb - 25 ? '.' : dl > dh + 6 ? 'L' : dh > dl + 6 ? 'H' : 'B';
        }
        tl[m] = 0;
        // where the energy is: time of the low-band peak and the high-band peak from the start
        long lp = s, hp = s; for (long k = s; k <= t; k++) { if (el[k] > el[lp]) lp = k; if (eh[k] > eh[hp]) hp = k; }
        // spectrum of the whole rep
        static double re[NFFT], im[NFFT], pw[NFFT/2]; memset(pw, 0, sizeof pw); int wins = 0;
        for (long w0 = s * FR; w0 < (t + 1) * FR; w0 += NFFT / 2) {
            for (int k = 0; k < NFFT; k++) { long i = w0 + k; re[k] = (i < (t + 1) * FR ? x[i] : 0) * (0.5 - 0.5*cos(2*M_PI*k/(NFFT-1))); im[k] = 0; }
            fft(re, im, NFFT); for (int k = 1; k < NFFT/2; k++) pw[k] += re[k]*re[k] + im[k]*im[k]; wins++;
        }
        double hz = (double)rate / NFFT, tot = 0, cs = 0, band[5] = {0};
        for (int k = 4; k < NFFT/2; k++) { double fr = k*hz; tot += pw[k]; cs += fr*pw[k];
            band[fr < 500 ? 0 : fr < 1500 ? 1 : fr < 4000 ? 2 : fr < 8000 ? 3 : 4] += pw[k]; }
        double run2 = 0, roll = 0; for (int k = 4; k < NFFT/2; k++) { run2 += pw[k]; if (run2 >= 0.85*tot) { roll = k*hz; break; } }
        // the strongest spectral peak between 150 Hz and 5 kHz (a body resonance, if any)
        int bk = 0; for (int k = 4; k < NFFT/2; k++) { double fr = k*hz; if (fr < 150 || fr > 5000) continue; if (!bk || pw[k] > pw[bk]) bk = k; }
        double len = (t - s + 1) * 10.0, cent = cs / tot;
        printf(" rep%02d t=%5.2fs len %4.0fms peak %5.1fdB lowpk@%4ldms highpk@%4ldms hi-lo %+5.1fdB cent %5.0fHz roll85 %5.0fHz  <500 %2.0f%% .5-1.5k %2.0f%% 1.5-4k %2.0f%% 4-8k %2.0f%% >8k %2.0f%%  peakbin %4.0fHz\n",
               r + 1, s / 100.0, len, pkdb, (lp - s) * 10, (hp - s) * 10, 10*log10(hpk/lpk), cent, roll,
               100*band[0]/tot, 100*band[1]/tot, 100*band[2]/tot, 100*band[3]/tot, 100*band[4]/tot, bk*hz);
        printf("        %s\n", tl);
        double v[10] = { len, pkdb, (double)(lp - s) * 10, (double)(hp - s) * 10, 10*log10(hpk/lpk), cent, roll, 100*band[0]/tot, 100*(band[1])/tot, 100*(band[2]+band[3]+band[4])/tot };
        for (int i = 0; i < 10; i++) { sum[i] += v[i]; sq[i] += v[i]*v[i]; }
    }
    if (np) { const char *nm[10] = { "len ms", "peak dB", "low peak at ms", "high peak at ms", "hi-lo dB", "centroid Hz", "rolloff Hz", "%<500", "%500-1.5k", "%>1.5k" };
        printf(" summary (mean, sd):"); for (int i = 0; i < 10; i++) { double mu = sum[i]/np; printf(" %s %.0f (%.0f);", nm[i], mu, sqrt(fmax(0, sq[i]/np - mu*mu))); } printf("\n"); }
    return 0;
}

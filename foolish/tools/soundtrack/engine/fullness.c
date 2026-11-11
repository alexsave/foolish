// fullness: how "full" or "thin" a lead sounds, as numbers. usage: fullness a.wav [t0 t1] [b.wav t0 t1 ...]
// For each file/window, over active frames (above -50 dB of the loudest):
//   centroid Hz; body = share of energy 150-1000 Hz; low-mid = 200-500 Hz share; flatness of the 100 Hz-8 kHz spectrum
//   (0 = pure tones, 1 = noise; a single sine-like lead is near 0); partials = average count of spectral peaks within
//   40 dB of the frame's maximum below 8 kHz (richness); width = side/mid energy (dB); octave-band levels relative to total.
#include "mix.h"
static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (int len = 2; len <= n; len <<= 1) { double a = -TAU / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) { double cr = 1, ci = 0;
            for (int j = 0; j < len / 2; j++) { int u = i + j, v = i + j + len / 2; double xr = re[v] * cr - im[v] * ci, xi = re[v] * ci + im[v] * cr;
                re[v] = re[u] - xr; im[v] = im[u] - xi; re[u] += xr; im[u] += xi; double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t; } } }
}
int main(int argc, char **argv) {
    printf("%-44s %8s %6s %7s %8s %8s %7s   octave bands 63..8k (dB rel. total)\n", "file [window]", "centroid", "body", "lowmid", "flatness", "partials", "width");
    for (int a = 1; a < argc;) {
        const char *path = argv[a++]; double t0 = 0, t1 = 1e9;
        if (a + 1 < argc && argv[a][0] != '/' && strchr(argv[a], '.') == 0 ? 0 : 0) {}
        if (a + 1 < argc + 1 && a < argc && (argv[a][0] >= '0' && argv[a][0] <= '9')) { t0 = atof(argv[a++]); t1 = atof(argv[a++]); }
        float *L, *R; long n = wav_read(path, &L, &R); long i0 = (long)(t0 * SR), i1 = (long)fmin(n, t1 * SR);
        const int W = 4096, H = 2048; double re[W], im[W], rs[W], is[W], win[W]; for (int i = 0; i < W; i++) win[i] = 0.5 - 0.5 * cos(TAU * i / W);
        int nfr = 0; double emax = 0;
        for (long s = i0; s + W < i1; s += H) { double e = 0; for (int i = 0; i < W; i++) e += L[s + i] * L[s + i] + R[s + i] * R[s + i]; if (e > emax) emax = e; }
        double cen = 0, body = 0, lm = 0, flat = 0, parts = 0, side = 0, mid = 0, oct[8] = {0}, tot = 0; int cnt = 0;
        for (long s = i0; s + W < i1; s += H) {
            double e = 0; for (int i = 0; i < W; i++) e += L[s + i] * L[s + i] + R[s + i] * R[s + i]; if (e < emax * 1e-5) continue;
            for (int i = 0; i < W; i++) { re[i] = (L[s + i] + R[s + i]) / 2 * win[i]; im[i] = 0; rs[i] = (L[s + i] - R[s + i]) / 2 * win[i]; is[i] = 0; }
            fft(re, im, W); fft(rs, is, W);
            double P[W / 2 + 1], pt = 0, pc = 0, pb = 0, pl = 0, lg = 0, ar = 0; int nb = 0, np = 0; double pm = 0;
            for (int k = 1; k <= W / 2; k++) { double f = k * (double)SR / W; P[k] = re[k] * re[k] + im[k] * im[k]; mid += P[k]; side += rs[k] * rs[k] + is[k] * is[k];
                pt += P[k]; pc += P[k] * f; if (f >= 150 && f < 1000) pb += P[k]; if (f >= 200 && f < 500) pl += P[k];
                if (f >= 100 && f < 8000) { lg += log(P[k] + 1e-20); ar += P[k]; nb++; if (P[k] > pm) pm = P[k]; }
                for (int o = 0; o < 8; o++) { double fc = 62.5 * pow(2, o); if (f >= fc / 1.4142 && f < fc * 1.4142) oct[o] += P[k]; } }
            for (int k = 2; k < W / 2; k++) { double f = k * (double)SR / W; if (f > 8000) break; if (P[k] > P[k - 1] && P[k] >= P[k + 1] && P[k] > pm * 1e-4) np++; }
            cen += pc / (pt + 1e-30); body += pb / (pt + 1e-30); lm += pl / (pt + 1e-30); flat += exp(lg / nb) / (ar / nb + 1e-30); parts += np; tot += pt; cnt++;
        }
        if (!cnt) { printf("%s: silent\n", path); continue; }
        char nm[64]; snprintf(nm, sizeof nm, "%.30s [%.0f-%.0f]", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, t0, fmin(t1, (double)n / SR));
        printf("%-44s %8.0f %6.2f %7.2f %8.3f %8.1f %6.1fdB  ", nm, cen / cnt, body / cnt, lm / cnt, flat / cnt, parts / cnt, 10 * log10(side / (mid + 1e-30) + 1e-30));
        for (int o = 0; o < 8; o++) printf(" %5.1f", 10 * log10(oct[o] / (tot + 1e-30) + 1e-30)); printf("\n");
    }
    return 0;
}

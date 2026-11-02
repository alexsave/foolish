// anal: measures a rendered theme the way a listener would check it.
// usage: anal in.wav outprefix [--bpm B] [--bar N beats] [--grid G per beat] [--off s] [--sec "t0,t1,..."] [--loop]
// writes outprefix.txt (the report), outprefix.spec (log-frequency spectrogram, float32),
// outprefix.loud (momentary/short-term loudness), outprefix.chroma, outprefix.ons (onsets).
#include "mix.h"

static int dcmp_(const double *a, const double *b) { return *a < *b ? -1 : *a > *b; }
static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (int len = 2; len <= n; len <<= 1) { double a = -TAU / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) { double cr = 1, ci = 0;
            for (int j = 0; j < len / 2; j++) { int u = i + j, v = i + j + len / 2; double xr = re[v] * cr - im[v] * ci, xi = re[v] * ci + im[v] * cr;
                re[v] = re[u] - xr; im[v] = im[u] - xi; re[u] += xr; im[u] += xi; double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t; } } }
}
static const char *NN[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const double KMAJ[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
static const double KMIN[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
static double corr(const double *a, const double *b, int n) { double ma = 0, mb = 0; for (int i = 0; i < n; i++) { ma += a[i]; mb += b[i]; } ma /= n; mb /= n;
    double s = 0, sa = 0, sb = 0; for (int i = 0; i < n; i++) { s += (a[i] - ma) * (b[i] - mb); sa += (a[i] - ma) * (a[i] - ma); sb += (b[i] - mb) * (b[i] - mb); } return s / sqrt(sa * sb + 1e-30); }
static void key_of(const double *ch, char *out) {
    double best = -2; int bk = 0, bm = 0;
    for (int k = 0; k < 12; k++) for (int m = 0; m < 2; m++) { double prof[12]; for (int i = 0; i < 12; i++) prof[i] = (m ? KMIN : KMAJ)[(i - k + 12) % 12];
        double c = corr(ch, prof, 12); if (c > best) { best = c; bk = k; bm = m; } }
    sprintf(out, "%s %s (r=%.2f)", NN[bk], bm ? "minor" : "major", best);
}
// chord templates
typedef struct { const char *name; int iv[5]; int n; } Tpl;
static const Tpl TPL[] = {{"", {0, 4, 7}, 3}, {"m", {0, 3, 7}, 3}, {"7", {0, 4, 7, 10}, 4}, {"m7", {0, 3, 7, 10}, 4}, {"maj7", {0, 4, 7, 11}, 4},
                          {"dim", {0, 3, 6}, 3}, {"dim7", {0, 3, 6, 9}, 4}, {"sus4", {0, 5, 7}, 3}, {"5", {0, 7}, 2}, {"add9", {0, 2, 4, 7}, 4}, {"m9", {0, 2, 3, 7, 10}, 5}};
static void chord_of(const double *ch, int bass, char *out) {
    double best = -2; int bi = 0, br = 0;
    for (int r = 0; r < 12; r++) for (unsigned t = 0; t < sizeof TPL / sizeof *TPL; t++) {
        double v[12] = {0}; for (int i = 0; i < TPL[t].n; i++) v[(r + TPL[t].iv[i]) % 12] = 1;
        double c = corr(ch, v, 12) - (TPL[t].n > 3 ? 0.03 * (TPL[t].n - 3) : 0) + (r == bass ? 0.04 : 0);
        if (c > best) { best = c; bi = t; br = r; } }
    if (bass >= 0 && bass != br) sprintf(out, "%s%s/%s", NN[br], TPL[bi].name, NN[bass]); else sprintf(out, "%s%s", NN[br], TPL[bi].name);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: anal in.wav outprefix [--bpm B --bar N --grid G --off s --sec list --loop]\n"); return 2; }
    double bpm = 0, off = 0; int bar = 4, grid = 4, loopm = 0; char secs[4096] = "";
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--bpm")) bpm = atof(argv[++i]); else if (!strcmp(argv[i], "--bar")) bar = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--grid")) grid = atoi(argv[++i]); else if (!strcmp(argv[i], "--off")) off = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sec")) snprintf(secs, sizeof secs, "%s", argv[++i]); else if (!strcmp(argv[i], "--loop")) loopm = 1;
    }
    float *L, *R; long n = wav_read(argv[1], &L, &R); double dur = (double)n / SR;
    char p[1024]; snprintf(p, sizeof p, "%s.txt", argv[2]); FILE *rep = fopen(p, "w");
    #define OUT(...) do { fprintf(rep, __VA_ARGS__); fprintf(stdout, __VA_ARGS__); } while (0)
    OUT("file %s\nduration %.3f s (%ld frames)\n", argv[1], dur, n);
    // ---- loudness ----
    long ns = 0; double *st = malloc(sizeof(double) * (n / (SR / 10) + 2)), *mo = malloc(sizeof(double) * (n / (SR / 10) + 2));
    double I = lufs(L, R, n, st, mo, &ns);
    double tp = a2db(true_peak(L, R, n)), sp = 0; for (long i = 0; i < n; i++) { sp = fmax(sp, fabs(L[i])); sp = fmax(sp, fabs(R[i])); }
    // LRA: 10th..95th percentile of gated short-term
    double *sv = malloc(sizeof(double) * ns); int nv = 0; for (long i = 29; i < ns; i++) if (st[i] > -70 && st[i] > I - 20) sv[nv++] = st[i];
    qsort(sv, nv, sizeof(double), (int (*)(const void *, const void *))dcmp_);
    double lra = nv > 2 ? sv[(int)(nv * 0.95)] - sv[(int)(nv * 0.10)] : 0;
    OUT("integrated %.2f LUFS, LRA %.1f LU, true peak %.2f dBTP, sample peak %.2f dBFS, PLR %.1f dB\n", I, lra, tp, a2db(sp), tp - I);
    snprintf(p, sizeof p, "%s.loud", argv[2]); FILE *f = fopen(p, "w"); for (long i = 0; i < ns; i++) fprintf(f, "%.2f %.2f %.2f\n", (i + 1) * 0.1, mo[i], st[i]); fclose(f);
    // ---- spectrogram, band balance, chroma ----
    const int W = 8192, H = 2048; int nfr = (int)((n - W) / H) + 1; if (nfr < 1) nfr = 1;
    const int NB = 240; double flo = 25, fhi = 20000;
    float *spec = calloc((size_t)nfr * NB, sizeof(float)); double *chroma = calloc((size_t)nfr * 12, sizeof(double)); int *bassn = calloc(nfr, sizeof(int));
    double *re = malloc(W * sizeof(double)), *im = malloc(W * sizeof(double)), *win = malloc(W * sizeof(double));
    for (int i = 0; i < W; i++) win[i] = 0.5 - 0.5 * cos(TAU * i / W);
    static const double bands[8] = {20, 60, 250, 500, 2000, 5000, 10000, 20000};
    static const char *bname[7] = {"sub 20-60", "bass 60-250", "lowmid 250-500", "mid 500-2k", "presence 2k-5k", "brill 5k-10k", "air 10k-20k"};
    double *bandE = calloc((size_t)nfr * 7, sizeof(double)), *sideLo = calloc(nfr, sizeof(double)), *midLo = calloc(nfr, sizeof(double));
    double *pw = malloc((W / 2 + 1) * sizeof(double));
    for (int fr = 0; fr < nfr; fr++) {
        long s0 = (long)fr * H; double ps[W / 2 + 1]; memset(ps, 0, sizeof ps);
        for (int c = 0; c < 3; c++) { // L, R, side
            for (int i = 0; i < W; i++) { long j = s0 + i; double v = j < n ? (c == 0 ? L[j] : c == 1 ? R[j] : (L[j] - R[j]) / 2) : 0; re[i] = v * win[i]; im[i] = 0; }
            fft(re, im, W);
            for (int k = 0; k <= W / 2; k++) { double e = (re[k] * re[k] + im[k] * im[k]) / (W * W / 4.0); if (c < 2) ps[k] += e / 2; else if (k * (double)SR / W < 120) sideLo[fr] += e; }
        }
        for (int k = 1; k <= W / 2; k++) { double fq = k * (double)SR / W; pw[k] = ps[k];
            for (int b = 0; b < 7; b++) if (fq >= bands[b] && fq < bands[b + 1]) bandE[fr * 7 + b] += ps[k];
            if (fq < 120) midLo[fr] += ps[k]; }
        for (int b = 0; b < NB; b++) { double a = flo * pow(fhi / flo, (double)b / NB), z = flo * pow(fhi / flo, (double)(b + 1) / NB); int ka = (int)(a * W / SR), kz = (int)(z * W / SR); if (kz < ka) kz = ka; double m = 0; int c = 0;
            for (int k = ka; k <= kz && k <= W / 2; k++) { m += pw[k]; c++; } spec[(size_t)fr * NB + b] = (float)(10 * log10(m / (c ? c : 1) + 1e-14)); }
        // chroma from 65 Hz to 4 kHz, peak-weighted (log-compressed magnitude); bass pitch class from 40-200 Hz
        double bc[12] = {0};
        for (int k = 2; k < W / 2; k++) { double fq = k * (double)SR / W; if (fq < 40 || fq > 4200) continue;
            if (!(pw[k] > pw[k - 1] && pw[k] >= pw[k + 1])) continue;
            // parabolic peak refinement
            double a = 10 * log10(pw[k - 1] + 1e-20), b = 10 * log10(pw[k] + 1e-20), cc = 10 * log10(pw[k + 1] + 1e-20), d = 0.5 * (a - cc) / (a - 2 * b + cc + 1e-12);
            double fp = (k + d) * (double)SR / W, mm = 69 + 12 * log2(fp / 440); int pc = ((int)lround(mm) % 12 + 12) % 12;
            double wgt = sqrt(pw[k]);
            if (fq >= 65) chroma[fr * 12 + pc] += wgt;
            if (fq < 200) bc[pc] += wgt * (fq < 130 ? 2 : 1); }
        int bb = 0; for (int i = 1; i < 12; i++) if (bc[i] > bc[bb]) bb = i; bassn[fr] = bc[bb] > 0 ? bb : -1;
    }
    snprintf(p, sizeof p, "%s.spec", argv[2]); f = fopen(p, "wb"); int hdr[4] = {nfr, NB, H, W}; fwrite(hdr, sizeof hdr, 1, f); fwrite(spec, sizeof(float), (size_t)nfr * NB, f); fclose(f);
    snprintf(p, sizeof p, "%s.chroma", argv[2]); f = fopen(p, "w");
    for (int fr = 0; fr < nfr; fr++) { double m = 1e-12; for (int i = 0; i < 12; i++) m = fmax(m, chroma[fr * 12 + i]); fprintf(f, "%.3f", (fr * H + W / 2) / (double)SR); for (int i = 0; i < 12; i++) fprintf(f, " %.3f", chroma[fr * 12 + i] / m); fprintf(f, "\n"); }
    fclose(f);
    // sections
    double sb[64]; int nsec = 0; sb[nsec++] = 0; { char *s = secs; while (*s) { double v = strtod(s, &s); if (v > 0 && v < dur) sb[nsec++] = v; if (*s == ',') s++; else break; } } sb[nsec] = dur;
    double tot[12] = {0};
    for (int fr = 0; fr < nfr; fr++) for (int i = 0; i < 12; i++) tot[i] += chroma[fr * 12 + i];
    char key[64]; key_of(tot, key); OUT("key estimate (whole): %s\nchroma:", key); { double m = 0; for (int i = 0; i < 12; i++) m = fmax(m, tot[i]); for (int i = 0; i < 12; i++) OUT(" %s=%.2f", NN[i], tot[i] / m); } OUT("\n");
    OUT("\nspectral balance per section (band level dB relative to the section total; tilt = dB/oct fit 100 Hz-10 kHz)\n%-16s", "section");
    for (int b = 0; b < 7; b++) OUT(" %14s", bname[b]); OUT("   tilt  lowSide/Mid\n");
    for (int s = 0; s < nsec; s++) {
        double e[7] = {0}, tl = 0, sl = 0, ml = 0; int c = 0;
        for (int fr = 0; fr < nfr; fr++) { double t = (fr * H + W / 2) / (double)SR; if (t < sb[s] || t >= sb[s + 1]) continue; for (int b = 0; b < 7; b++) e[b] += bandE[fr * 7 + b]; sl += sideLo[fr]; ml += midLo[fr]; c++; }
        for (int b = 0; b < 7; b++) tl += e[b];
        // tilt: least squares of per-octave power density vs octave (from the log spectrogram)
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
        for (int b = 0; b < NB; b++) { double fq = flo * pow(fhi / flo, (b + 0.5) / NB); if (fq < 100 || fq > 10000) continue; double y = 0; int cc2 = 0;
            for (int fr = 0; fr < nfr; fr++) { double t = (fr * H + W / 2) / (double)SR; if (t < sb[s] || t >= sb[s + 1]) continue; y += pow(10, spec[(size_t)fr * NB + b] / 10); cc2++; }
            if (!cc2) continue; y = 10 * log10(y / cc2 * fq + 1e-30); double x = log2(fq); sx += x; sy += y; sxx += x * x; sxy += x * y; m++; }
        double slope = m > 2 ? (m * sxy - sx * sy) / (m * sxx - sx * sx) : 0;
        char nm[32]; snprintf(nm, sizeof nm, "%.1f-%.1fs", sb[s], sb[s + 1]); OUT("%-16s", nm);
        for (int b = 0; b < 7; b++) OUT(" %14.1f", 10 * log10(e[b] / (tl + 1e-30) + 1e-30)); OUT("  %5.2f  %6.1f dB\n", slope, 10 * log10(sl / (ml + 1e-30) + 1e-30));
        if (c == 0) continue;
    }
    // ---- bar-wise chords ----
    if (bpm > 0) {
        double spb = 60 / bpm, barlen = spb * bar; int nbar = (int)((dur - off) / barlen + 0.5);
        OUT("\nchords per half bar (template match on chroma, bass from 40-200 Hz):\n");
        for (int b = 0; b < nbar && b < 200; b++) {
            OUT("bar %3d:", b + 1);
            for (int h = 0; h < 2; h++) {
                double t0 = off + b * barlen + h * barlen / 2, t1 = t0 + barlen / 2, ch[12] = {0}; int bc[12] = {0};
                for (int fr = 0; fr < nfr; fr++) { double t = (fr * H + W / 2) / (double)SR; if (t < t0 || t >= t1) continue; for (int i = 0; i < 12; i++) ch[i] += chroma[fr * 12 + i]; if (bassn[fr] >= 0) bc[bassn[fr]]++; }
                int bb = 0; for (int i = 1; i < 12; i++) if (bc[i] > bc[bb]) bb = i;
                char c[32]; chord_of(ch, bc[bb] ? bb : -1, c); OUT(" %-9s", c);
            }
            OUT("%s", (b % 4 == 3) ? "\n" : " |");
        }
        OUT("\n");
    }
    // ---- onsets per band (spectral flux, hop 128) ----
    {
        const int OW = 1024, OH = 128; int nof = (int)((n - OW) / OH) + 1;
        static const double ob[4] = {30, 160, 2000, 16000}; static const char *obn[3] = {"low 30-160", "mid 160-2k", "high 2k-16k"};
        double *flux = calloc((size_t)nof * 3, sizeof(double)), *prev = calloc(OW / 2 + 1, sizeof(double)), *ow = malloc(OW * sizeof(double));
        double ore[OW], oim[OW]; for (int i = 0; i < OW; i++) ow[i] = 0.5 - 0.5 * cos(TAU * i / OW);
        for (int fr = 0; fr < nof; fr++) {
            long s0 = (long)fr * OH; for (int i = 0; i < OW; i++) { long j = s0 + i; ore[i] = j < n ? (L[j] + R[j]) * 0.5 * ow[i] : 0; oim[i] = 0; }
            fft(ore, oim, OW);
            for (int k = 1; k <= OW / 2; k++) { double fq = k * (double)SR / OW, m = log(1 + 1000 * sqrt(ore[k] * ore[k] + oim[k] * oim[k])); double d = m - prev[k]; prev[k] = m; if (d < 0) d = 0;
                for (int b = 0; b < 3; b++) if (fq >= ob[b] && fq < ob[b + 1]) flux[fr * 3 + b] += d; }
        }
        snprintf(p, sizeof p, "%s.ons", argv[2]); f = fopen(p, "w");
        OUT("\nonsets (spectral flux peaks) and timing against the grid (%d per beat, beat 1 at %.3f s, %.2f BPM):\n", grid, off, bpm);
        int slots = bar * grid;
        for (int b = 0; b < 3; b++) {
            // adaptive threshold: local median + k * mean abs dev over +-0.5 s
            int wn = (int)(0.5 * SR / OH); double *ot = malloc(sizeof(double) * nof), *os = malloc(sizeof(double) * nof); int no = 0;
            double gmax = 0; for (int fr = 0; fr < nof; fr++) gmax = fmax(gmax, flux[fr * 3 + b]);
            for (int fr = 2; fr < nof - 2; fr++) {
                double v = flux[fr * 3 + b]; if (!(v > flux[(fr - 1) * 3 + b] && v >= flux[(fr + 1) * 3 + b])) continue;
                double mean = 0; int c = 0; for (int j = fr - wn; j <= fr + wn; j++) if (j >= 0 && j < nof) { mean += flux[j * 3 + b]; c++; } mean /= c;
                if (v < mean * 1.8 || v < gmax * 0.06) continue;
                double t = (fr * OH + OW / 2) / (double)SR; if (no && t - ot[no - 1] < 0.045) { if (v > os[no - 1]) { ot[no - 1] = t; os[no - 1] = v; } continue; }
                ot[no] = t; os[no] = v; no++;
            }
            double *dev = malloc(sizeof(double) * (no + 1)); int nd = 0; double hist[256] = {0}; double hmax = 0;
            for (int i = 0; i < no; i++) {
                fprintf(f, "%d %.4f %.3f\n", b, ot[i], os[i]);
                if (bpm <= 0) continue;
                double spb = 60 / bpm, pos = (ot[i] - off) / spb * grid; double near = floor(pos + 0.5); dev[nd++] = (pos - near) / grid * spb * 1000;
                long sl = ((long)near % slots + slots) % slots; if (sl < 256) { hist[sl] += os[i]; if (hist[sl] > hmax) hmax = hist[sl]; }
            }
            if (bpm > 0 && nd) {
                double *ad = malloc(sizeof(double) * nd); for (int i = 0; i < nd; i++) ad[i] = dev[i];
                qsort(ad, nd, sizeof(double), (int (*)(const void *, const void *))dcmp_);
                double med = ad[nd / 2]; for (int i = 0; i < nd; i++) ad[i] = fabs(dev[i] - med); qsort(ad, nd, sizeof(double), (int (*)(const void *, const void *))dcmp_);
                int w10 = 0; for (int i = 0; i < nd; i++) if (fabs(dev[i] - med) < 10) w10++;
                OUT("  %-12s %4d onsets, median offset %+5.1f ms, spread (MAD) %4.1f ms, %3.0f%% within 10 ms of the median offset\n", obn[b], no, med, ad[nd / 2], 100.0 * w10 / nd);
                if (grid % 2 == 0) { // swing: where the odd subdivisions land against the even ones
                    double ev[4096], od[4096]; int ne = 0, no2 = 0;
                    for (int i = 0; i < no && i < 4096; i++) { double spb = 60 / bpm, pos = (ot[i] - off) / spb * grid; double nr = floor(pos + 0.5), d = (pos - nr) / grid * spb * 1000; if (((long)nr & 1)) od[no2++] = d; else ev[ne++] = d; }
                    qsort(ev, ne, sizeof(double), (int (*)(const void *, const void *))dcmp_); qsort(od, no2, sizeof(double), (int (*)(const void *, const void *))dcmp_);
                    if (ne > 4 && no2 > 4) OUT("  %-12s swing: odd subdivisions land %+5.1f ms against the even ones (%d odd, %d even onsets)\n", "", od[no2 / 2] - ev[ne / 2], no2, ne);
                }
                OUT("  %-12s map ", "");
                for (int sl = 0; sl < slots; sl++) { if (sl && sl % grid == 0) OUT("|"); double v = hist[sl] / (hmax + 1e-12); OUT("%c", v < 0.05 ? '.' : (char)('0' + (int)fmin(9, v * 10))); }
                OUT("\n"); free(ad);
            } else OUT("  %-12s %4d onsets\n", obn[b], no);
            free(dev); free(ot); free(os);
        }
        fclose(f);
    }
    // ---- seam (for loops): the wrap from the last sample to the first ----
    if (loopm) {
        // click metric: energy above 3 kHz in 1 ms windows straddling the wrap vs the distribution of all such windows
        long w = SR / 1000; Bq h1 = bq_hp(3000, 0.7), h2 = bq_hp(3000, 0.7);
        long nw = n / w; double *e = calloc(nw + 1, sizeof(double));
        // run the filter over the circular signal so the wrap is a real neighbour: start 0.1 s before the end
        long pre = SR / 10; double ew = 0;
        for (long i = -pre; i < n; i++) { long j = (i + n) % n; double v = bq(&h1, (L[j] + R[j]) / 2); v = bq(&h2, v); if (i >= 0) e[i / w] += v * v; else if (i >= -w) ew += v * v; }
        // window across the wrap: last w/2 + first w/2
        Bq g1 = bq_hp(3000, 0.7), g2 = bq_hp(3000, 0.7); double ws = 0;
        for (long i = n - SR / 10; i < n + w / 2; i++) { long j = i % n; double v = bq(&g2, bq(&g1, (L[j] + R[j]) / 2)); if (i >= n - w / 2) ws += v * v; }
        double *es = malloc(sizeof(double) * nw); memcpy(es, e, sizeof(double) * nw); qsort(es, nw, sizeof(double), (int (*)(const void *, const void *))dcmp_);
        int rank = 0; for (long i = 0; i < nw; i++) if (es[i] < ws) rank++;
        double step = fabs(L[0] - L[n - 1]) + fabs(R[0] - R[n - 1]), ms = 0; for (long i = 1; i < n; i++) ms += fabs(L[i] - L[i - 1]) + fabs(R[i] - R[i - 1]); ms /= n - 1;
        double ss[2000]; int nss = 0; for (long i = 1; i < n && nss < 2000; i += n / 2000) ss[nss++] = fabs(L[i] - L[i - 1]) + fabs(R[i] - R[i - 1]);
        OUT("\nseam: wrap step %.5f (mean step %.5f, x%.2f); HF energy in the 1 ms across the wrap is at the %.1f percentile of all 1 ms windows\n", step, ms, step / (ms + 1e-12), 100.0 * rank / nw);
        if (bpm > 0) { // the same 1 ms HF window at every bar start, for a loop that starts on a downbeat hit
            double barlen = 60 / bpm * bar; double vals[512]; int nvv = 0;
            for (int b2 = 1; b2 < 512; b2++) { long c = lround(off * SR + b2 * barlen * SR); if (c + w / 2 >= n) break; Bq q1 = bq_hp(3000, 0.7), q2 = bq_hp(3000, 0.7); double e2 = 0;
                for (long i = c - SR / 10; i < c + w / 2; i++) { double v = bq(&q2, bq(&q1, (L[i] + R[i]) / 2)); if (i >= c - w / 2) e2 += v * v; } vals[nvv++] = e2; }
            qsort(vals, nvv, sizeof(double), (int (*)(const void *, const void *))dcmp_);
            if (nvv) OUT("seam vs downbeats: wrap window HF energy %.1f dB against the median of the same window at the other %d bar starts\n", 10 * log10(ws / (vals[nvv / 2] + 1e-30) + 1e-30), nvv);
        }
        double l0 = mo[3], l1 = mo[ns - 1]; OUT("seam loudness: momentary %.1f LUFS at the end vs %.1f LUFS 0.4 s into the start\n", l1, l0);
    }
    fclose(rep);
    return 0;
}

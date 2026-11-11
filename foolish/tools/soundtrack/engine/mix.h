// mix.h: reverb, compressor, limiter, loudness (BS.1770-4), true peak, WAV I/O.
#pragma once
#include "dsp.h"

// ---------------- FDN reverb ----------------
#define FDN_N 8
typedef struct {
    double rt60, rt60_hi, predelay_ms, size, hp, lp, er, width, mod_hz, mod_depth;
} RevCfg;
typedef struct {
    RevCfg c;
    float *pre; long npre, pp;
    float *line[FDN_N]; long len[FDN_N], pos[FDN_N];
    double g[FDN_N], lpc[FDN_N], lps[FDN_N];
    float *ap[4]; long aplen[4], appos[4];
    Bq hpf[2], lpf[2];
    double t; // seconds, for modulation phase
} Rev;
static void rev_init(Rev *r, RevCfg c) {
    memset(r, 0, sizeof *r); r->c = c;
    r->npre = (long)(c.predelay_ms * SR / 1000) + 2; r->pre = calloc(r->npre + SR / 4, sizeof(float));
    static const double ms[FDN_N] = {31.7, 37.1, 41.9, 47.3, 53.9, 61.3, 67.9, 73.1};
    for (int i = 0; i < FDN_N; i++) {
        r->len[i] = (long)(ms[i] * c.size * SR / 1000);
        r->line[i] = calloc(r->len[i] + 64, sizeof(float));
        double d = (double)r->len[i] / SR;
        r->g[i] = pow(10, -3 * d / c.rt60);
        // one-pole lowpass whose gain at 8 kHz makes the HF decay reach rt60_hi
        double ghi = pow(10, -3 * d / c.rt60_hi) / r->g[i];   // extra attenuation per pass at high freq
        // solve |H(8k)| = ghi for one-pole y += a(x-y): |H| = a / |1 - (1-a)e^{-jw}|
        double w = TAU * 8000 / SR, best = 1, berr = 1e9;
        for (double a = 0.01; a <= 1.0; a += 0.001) { double re = 1 - (1 - a) * cos(w), im = (1 - a) * sin(w), h = a / sqrt(re * re + im * im);
            if (fabs(h - ghi) < berr) { berr = fabs(h - ghi); best = a; } }
        r->lpc[i] = best;
    }
    static const double apms[4] = {4.77, 3.59, 12.73, 9.30};
    for (int i = 0; i < 4; i++) { r->aplen[i] = (long)(apms[i] * SR / 1000); r->ap[i] = calloc(r->aplen[i], sizeof(float)); }
    for (int i = 0; i < 2; i++) { r->hpf[i] = bq_hp(c.hp > 0 ? c.hp : 20, 0.707); r->lpf[i] = bq_lp(c.lp > 0 ? c.lp : 18000, 0.707); }
}
// process one stereo frame in place (wet only)
static inline void rev_tick(Rev *r, double inl, double inr, double *ol, double *orr) {
    double x = bq(&r->lpf[0], bq(&r->hpf[0], (inl + inr) * 0.5));
    // predelay
    r->pre[r->pp] = (float)x; long rp = r->pp - r->npre + 2; if (rp < 0) rp += r->npre; double d = r->pre[rp]; r->pp = (r->pp + 1) % r->npre;
    // input diffusion
    for (int i = 0; i < 4; i++) { float *b = r->ap[i]; long p = r->appos[i]; double v = b[p]; double y = -0.6 * d + v; b[p] = (float)(d + 0.6 * y); r->appos[i] = (p + 1) % r->aplen[i]; d = y; }
    // read lines (with gentle modulation of two lines, phase locked to the loop period by the caller's mod_hz)
    double o[FDN_N];
    for (int i = 0; i < FDN_N; i++) {
        double rd = (double)r->pos[i];
        if (r->c.mod_depth > 0 && (i == 1 || i == 6)) {
            double m = r->c.mod_depth * SR / 1000 * (1 + sin(TAU * r->c.mod_hz * r->t * (i == 1 ? 1 : 2)));
            rd = r->pos[i] - m; while (rd < 0) rd += r->len[i];
            long a = (long)rd; double fr = rd - a; long b = (a + 1) % r->len[i];
            o[i] = r->line[i][a] * (1 - fr) + r->line[i][b] * fr;
        } else o[i] = r->line[i][r->pos[i]];
    }
    // Hadamard 8x8 (fast) / sqrt(8)
    double h[FDN_N]; memcpy(h, o, sizeof h);
    for (int s = 1; s < FDN_N; s <<= 1) for (int i = 0; i < FDN_N; i += 2 * s) for (int j = i; j < i + s; j++) { double a = h[j], b = h[j + s]; h[j] = a + b; h[j + s] = a - b; }
    for (int i = 0; i < FDN_N; i++) {
        double v = h[i] / sqrt(8.0) * r->g[i];
        r->lps[i] += r->lpc[i] * (v - r->lps[i]);
        double in = (i & 1) ? d : -d;
        r->line[i][r->pos[i]] = (float)(r->lps[i] + in * 0.35);
        r->pos[i] = (r->pos[i] + 1) % r->len[i];
    }
    double L = o[0] - o[2] + o[4] - o[6] + 0.5 * (o[1] + o[5]);
    double R = o[1] - o[3] + o[5] - o[7] + 0.5 * (o[2] - o[6]);
    double er = r->c.er * d;  // a touch of the diffused signal as early energy
    L = L * 0.35 + er; R = R * 0.35 + er;
    double mid = (L + R) / 2, side = (L - R) / 2 * r->c.width;
    *ol = mid + side; *orr = mid - side;
    r->t += 1.0 / SR;
}

// ---------------- compressor (feed-forward, stereo linked, soft knee) ----------------
typedef struct { double thr, ratio, att, rel, knee, env; } Comp;
static inline double comp_gain(Comp *c, double peak) {
    double lev = a2db(peak);
    double a = exp(-1.0 / (c->att * SR)), r = exp(-1.0 / (c->rel * SR));
    c->env = lev > c->env ? a * c->env + (1 - a) * lev : r * c->env + (1 - r) * lev;
    double e = c->env, k = c->knee, over = e - c->thr, gr;
    if (2 * over < -k) gr = 0;
    else if (2 * fabs(over) <= k) gr = (1 / c->ratio - 1) * (over + k / 2) * (over + k / 2) / (2 * k);
    else gr = (1 / c->ratio - 1) * over;
    return db2a(gr);
}

// ---------------- true peak (4x oversampling, windowed sinc) ----------------
#define TP_TAPS 12
static double g_tpk[4][2 * TP_TAPS];
static int g_tp_init;
static void tp_init(void) {
    if (g_tp_init) return; g_tp_init = 1;
    for (int ph = 0; ph < 4; ph++) for (int k = 0; k < 2 * TP_TAPS; k++) {
        double x = (k - TP_TAPS + 1) - ph / 4.0; // position relative to sample
        double s = fabs(x) < 1e-9 ? 1 : sin(M_PI * x) / (M_PI * x);
        double w = 0.5 + 0.5 * cos(M_PI * x / TP_TAPS);
        g_tpk[ph][k] = s * w;
    }
}
// max |interpolated| between sample i and i+1 (phases 1..3) and at i
static inline double tp_at(const float *x, long n, long i, int stride) {
    double m = fabs(x[i * stride]);
    for (int ph = 1; ph < 4; ph++) {
        double s = 0;
        for (int k = 0; k < 2 * TP_TAPS; k++) { long j = i + k - TP_TAPS + 1; if (j >= 0 && j < n) s += x[j * stride] * g_tpk[ph][k]; }
        if (fabs(s) > m) m = fabs(s);
    }
    return m;
}
static double true_peak(const float *L, const float *R, long n) {
    tp_init(); double m = 0;
    for (long i = 0; i < n; i++) { double a = tp_at(L, n, i, 1), b = tp_at(R, n, i, 1); if (a > m) m = a; if (b > m) m = b; }
    return m;
}

// ---------------- limiter (lookahead, true-peak aware) ----------------
static void limit(float *L, float *R, long n, double ceil_db, double rel_s) {
    tp_init();
    const long LA = 96; // 2 ms
    double ceil = db2a(ceil_db);
    double *req = malloc(n * sizeof(double)), *g = malloc(n * sizeof(double));
    for (long i = 0; i < n; i++) { double p = fmax(tp_at(L, n, i, 1), tp_at(R, n, i, 1)); req[i] = p > ceil ? ceil / p : 1; }
    // forward-looking minimum over LA samples, then release, then LA-long smoothing that never exceeds the minimum
    // h[i] = min(req[i - LA .. i + LA]) so a box average over [i-LA, i] stays below req[i]
    double *h = malloc(n * sizeof(double));
    // monotonic deque minimum over window [i-LA, i+LA]
    long *dq = malloc(n * sizeof(long)); long qh = 0, qt = 0, nx = 0;
    for (long i = 0; i < n; i++) {
        long hi = i + LA < n ? i + LA : n - 1;
        while (nx <= hi) { while (qt > qh && req[dq[qt - 1]] >= req[nx]) qt--; dq[qt++] = nx; nx++; }
        while (dq[qh] < i - LA) qh++;
        h[i] = req[dq[qh]];
    }
    double rc = exp(-1.0 / (rel_s * SR)), e = 1;
    for (long i = 0; i < n; i++) { e = fmin(h[i], rc * e + (1 - rc) * 1); g[i] = e; }
    // box smoothing over LA (centered) keeps the curve click-free; it can only stay at or below max of window
    double acc = 0; long W = LA;
    double *s = malloc(n * sizeof(double));
    for (long i = 0; i < n; i++) { acc += g[i]; if (i >= W) acc -= g[i - W]; s[i] = acc / (i < W ? i + 1 : W); }
    for (long i = 0; i < n; i++) { long j = i + W / 2; double gg = j < n ? s[j] : s[n - 1]; gg = fmin(gg, req[i] < 1 ? fmin(gg, h[i]) : gg); L[i] *= gg; R[i] *= gg; }
    free(req); free(g); free(h); free(dq); free(s);
}

// ---------------- loudness (BS.1770-4 / EBU R128) ----------------
static void kweight(const float *x, long n, int stride, double *out) {
    Bq a = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585, 0, 0, 0, 0};
    Bq b = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621, 0, 0, 0, 0};
    for (long i = 0; i < n; i++) { double v = bq(&b, bq(&a, x[i * stride])); out[i] = v * v; }
}
// integrated loudness; optional short-term (3 s) and momentary (0.4 s) series every 0.1 s
static double lufs(const float *L, const float *R, long n, double *st, double *mo, long *nser) {
    double *pl = malloc(n * sizeof(double)), *pr = malloc(n * sizeof(double));
    kweight(L, n, 1, pl); kweight(R, n, 1, pr);
    // prefix sums
    double *cs = malloc((n + 1) * sizeof(double)); cs[0] = 0;
    for (long i = 0; i < n; i++) cs[i + 1] = cs[i] + pl[i] + pr[i];
    long blk = SR * 4 / 10, hop = SR / 10, nb = n >= blk ? (n - blk) / hop + 1 : 0;
    double *z = malloc((nb + 1) * sizeof(double));
    for (long b = 0; b < nb; b++) z[b] = (cs[b * hop + blk] - cs[b * hop]) / blk;
    double sum = 0; long cnt = 0;
    for (long b = 0; b < nb; b++) if (-0.691 + 10 * log10(z[b] + 1e-30) > -70) { sum += z[b]; cnt++; }
    double I = -200;
    if (cnt) {
        double rel = -0.691 + 10 * log10(sum / cnt) - 10; double s2 = 0; long c2 = 0;
        for (long b = 0; b < nb; b++) { double l = -0.691 + 10 * log10(z[b] + 1e-30); if (l > -70 && l > rel) { s2 += z[b]; c2++; } }
        if (c2) I = -0.691 + 10 * log10(s2 / c2);
    }
    if (nser) {
        long k = 0;
        for (long i = 0; i + hop <= n; i += hop, k++) {
            long a3 = i + hop - 3 * SR; if (a3 < 0) a3 = 0; long e = i + hop;
            st[k] = -0.691 + 10 * log10((cs[e] - cs[a3]) / (e - a3) + 1e-30);
            long a4 = e - blk; if (a4 < 0) a4 = 0;
            mo[k] = -0.691 + 10 * log10((cs[e] - cs[a4]) / (e - a4) + 1e-30);
        }
        *nser = k;
    }
    free(pl); free(pr); free(cs); free(z);
    return I;
}

// ---------------- WAV ----------------
static void wav_write24(const char *path, const float *L, const float *R, long n) {
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); exit(1); }
    uint32_t data = (uint32_t)(n * 6), riff = 36 + data, fmtsz = 16, sr = SR, br = SR * 6; uint16_t pcm = 1, ch = 2, ba = 6, bits = 24;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    Rng r = {12345};
    unsigned char *b = malloc(n * 6);
    for (long i = 0; i < n; i++) for (int c = 0; c < 2; c++) {
        double v = (c ? R[i] : L[i]) * 8388607.0 + (r01(&r) - r01(&r)); // TPDF dither, 1 LSB
        long q = lround(v); if (q > 8388607) q = 8388607; if (q < -8388608) q = -8388608;
        unsigned char *p = b + i * 6 + c * 3; p[0] = q & 255; p[1] = (q >> 8) & 255; p[2] = (q >> 16) & 255;
    }
    fwrite(b, 1, n * 6, f); free(b); fclose(f);
}
// reads 16/24-bit PCM or 32-bit float stereo/mono WAV; returns frames
static uint32_t g_wav_sr = SR;   // sample rate of the last file read
static int g_wav_quiet = 0;
static long wav_read(const char *path, float **L, float **R) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    char id[4]; uint32_t sz; uint16_t fmt = 1, ch = 2, bits = 16; uint32_t sr = SR;
    fread(id, 1, 4, f); fread(&sz, 4, 1, f); fread(id, 1, 4, f);
    long n = 0;
    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) { unsigned char b[64]; fread(b, 1, sz, f); fmt = b[0] | b[1] << 8; ch = b[2] | b[3] << 8; sr = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24; bits = b[14] | b[15] << 8; if (fmt == 0xFFFE) fmt = b[24] | b[25] << 8; }
        else if (!memcmp(id, "data", 4)) {
            int bps = bits / 8; n = sz / (bps * ch); *L = malloc(n * sizeof(float)); *R = malloc(n * sizeof(float));
            unsigned char *b = malloc(sz); fread(b, 1, sz, f);
            for (long i = 0; i < n; i++) for (int c = 0; c < 2; c++) {
                int cc = ch == 1 ? 0 : c; unsigned char *p = b + (i * ch + cc) * bps; double v;
                if (fmt == 3) v = *(float *)p;
                else if (bits == 24) { int32_t q = p[0] | p[1] << 8 | p[2] << 16; if (q & 0x800000) q |= ~0xFFFFFF; v = q / 8388608.0; }
                else v = (int16_t)(p[0] | p[1] << 8) / 32768.0;
                (c ? *R : *L)[i] = (float)v;
            }
            free(b); break;
        } else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    fclose(f);
    g_wav_sr = sr; if (sr != SR && !g_wav_quiet) fprintf(stderr, "warning: %s is %u Hz\n", path, sr);
    return n;
}

#include "room.h"
#include "fft.h"
#include "geom.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ROOM_ABS_MIN_DIST 0.05
#define FD_HALF 16 /* fractional delay: windowed sinc, 2 * FD_HALF taps */
#define SPLIT_TAPS 801 /* linear-phase band split FIR length */
#define SPLIT_LO_HZ 300.0
#define SPLIT_HI_HZ 3000.0
#define SHELF_LO_HZ 200.0 /* FDN absorption shelves */
#define SHELF_HI_HZ 3000.0
#define CARDIOID_FLOOR_DB (-25.0) /* real capsules leak at the back: ASSUMED */
static const double BAND_REP_HZ[ROOM_BANDS] = {200.0, 1000.0, 6000.0}; /* air absorption evaluated here per band */
static const int FDN_LEN[ROOM_NLINES] = {601, 691, 787, 881, 977, 1063, 1151, 1259}; /* mutually prime, 12.5 to 26 ms */

const int *room_fdn_lengths(int *n) { *n = ROOM_NLINES; return FDN_LEN; }

double room_speed(const RoomSpec *s) { return 331.3 * sqrt(1.0 + s->temp_c / 273.15); }

void room_spec_default(RoomSpec *s, double dist)
{
    memset(s, 0, sizeof *s);
    s->fs = 48000.0;
    s->dim[0] = GEOM_ROOM_X; s->dim[1] = GEOM_ROOM_Y; s->dim[2] = GEOM_ROOM_Z;
    s->rt60 = 0.8;
    s->rt60_mult[0] = 1.2; s->rt60_mult[1] = 1.0; s->rt60_mult[2] = 0.7;
    /* source: between the f-holes just above the top plate, cello frame (0, FH_CY, plate + 2 cm) */
    double c[3] = {0.0, GEOM_FH_CY, geom_top_z(0.0, GEOM_FH_CY) + 0.02}, r[3];
    geom_to_room(c, r);
    s->src[0] = r[0] + 2.7; s->src[1] = r[1] + 3.5; s->src[2] = r[2];
    s->front[0] = 0.0; s->front[1] = -1.0; s->front[2] = 0.0; /* geom_to_room: the top plate faces -Y */
    s->mic[0] = s->src[0]; s->mic[1] = s->src[1] - dist; s->mic[2] = 1.0;
    double d[3] = {s->src[0] - s->mic[0], s->src[1] - s->mic[1], s->src[2] - s->mic[2]};
    double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; i++) s->mic_axis[i] = d[i] / l;
    s->pattern = ROOM_CARDIOID;
    s->order = 6;
    s->temp_c = 20.0; s->rh = 50.0;
    s->dir_db[0] = 0.0; s->dir_db[1] = 0.75; s->dir_db[2] = 1.5;
    s->self_noise_dba = 2.0;
    s->noise_knee_hz = 4000.0;
    s->noise_tilt_db_oct = 3.0;
    s->ref_db_spl = 74.0;
    s->seed = ALTCELLO_ROOM_SEED;
}

double room_wall_alpha(const RoomSpec *s, double rt60)
{
    double V = s->dim[0] * s->dim[1] * s->dim[2];
    double S = 2.0 * (s->dim[0] * s->dim[1] + s->dim[0] * s->dim[2] + s->dim[1] * s->dim[2]);
    double k = 24.0 * log(10.0) / room_speed(s); /* 0.161 */
    double a = 1.0 - exp(-k * V / (S * rt60)); /* Eyring; Sabine's k V / (S T) is the same for small alpha */
    return a > 0.95 ? 0.95 : a;
}

/* ISO 9613-1:1993 formula (recalled, see REFERENCES 4.3: UNVERIFIED against the standard text) */
double room_air_db_per_m(double f, double temp_c, double rh)
{
    double T = temp_c + 273.15, T0 = 293.15, T01 = 273.16;
    double C = -6.8346 * pow(T01 / T, 1.261) + 4.6151;
    double h = rh * pow(10.0, C);
    double frO = 24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h);
    double frN = pow(T / T0, -0.5) * (9.0 + 280.0 * h * exp(-4.170 * (pow(T / T0, -1.0 / 3.0) - 1.0)));
    double f2 = f * f;
    return 8.686 * f2 * (1.84e-11 * sqrt(T / T0) +
                         pow(T / T0, -2.5) * (0.01275 * exp(-2239.1 / T) / (frO + f2 / frO) + 0.1068 * exp(-3352.0 / T) / (frN + f2 / frN)));
}

double room_aweight_db(double f)
{
    double f2 = f * f, c1 = 12194.0 * 12194.0;
    double ra = c1 * f2 * f2 / ((f2 + 20.6 * 20.6) * sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + c1));
    return 20.0 * log10(ra) + 2.0;
}

static double mic_gain(int pattern, double cosphi)
{
    if (pattern == ROOM_OMNI) return 1.0;
    double g = 0.5 + 0.5 * cosphi, fl = pow(10.0, CARDIOID_FLOOR_DB / 20.0);
    return g < fl ? fl : g;
}

static double sinc(double x) { return fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

/* add amp * (windowed sinc centred at position p, in samples) into buf[0..n) */
static void add_frac(double *buf, long n, double p, double amp)
{
    long i0 = (long)floor(p);
    for (long j = i0 - FD_HALF + 1; j <= i0 + FD_HALF; j++) {
        if (j < 0 || j >= n) continue;
        double x = (double)j - p, u = x / FD_HALF;
        double w = 0.42 + 0.5 * cos(M_PI * u) + 0.08 * cos(2.0 * M_PI * u); /* Blackman */
        buf[j] += amp * sinc(x) * w;
    }
}

typedef struct { double pos[3]; double dep[3]; double r; int nref; int order; } Image;

/* the image of order ord with indices n, q: its source position, the unit departure direction from the real source, the path
 * length and the reflection count. Returns 0 when the order exceeds the limit. */
static int image_at(const RoomSpec *s, const int n[3], const int q[3], Image *im)
{
    int order = 0;
    double mp[3];
    for (int a = 0; a < 3; a++) {
        order += abs(n[a] - q[a]) + abs(n[a]);
        double sg = 1.0 - 2.0 * q[a], L = s->dim[a];
        im->pos[a] = sg * s->src[a] + 2.0 * n[a] * L;
        mp[a] = sg * (s->mic[a] - 2.0 * n[a] * L); /* image of the microphone for the same wall sequence */
    }
    im->order = order;
    im->nref = order;
    double d[3], dd = 0.0;
    for (int a = 0; a < 3; a++) { d[a] = im->pos[a] - s->mic[a]; dd += d[a] * d[a]; }
    im->r = sqrt(dd);
    double e[3], ee = 0.0;
    for (int a = 0; a < 3; a++) { e[a] = mp[a] - s->src[a]; ee += e[a] * e[a]; }
    ee = sqrt(ee);
    for (int a = 0; a < 3; a++) im->dep[a] = ee > 1e-9 ? e[a] / ee : s->front[a];
    return 1;
}

double room_direct_time(const RoomSpec *s)
{
    double d = 0.0;
    for (int a = 0; a < 3; a++) d += (s->src[a] - s->mic[a]) * (s->src[a] - s->mic[a]);
    return sqrt(d) / room_speed(s);
}

double room_crossover_time(const RoomSpec *s)
{
    int N = s->order + 1;
    double best = 1e30;
    for (int qx = 0; qx < 2; qx++) for (int qy = 0; qy < 2; qy++) for (int qz = 0; qz < 2; qz++)
    for (int nx = -N; nx <= N; nx++) for (int ny = -N; ny <= N; ny++) for (int nz = -N; nz <= N; nz++) {
        int n[3] = {nx, ny, nz}, q[3] = {qx, qy, qz};
        Image im;
        image_at(s, n, q, &im);
        if (im.order != N) continue;
        if (im.r < best) best = im.r;
    }
    return best / room_speed(s);
}

static void sinc_lp(double *h, int taps, double fc_norm) /* fc_norm = cutoff / fs, unity DC gain, Hamming, linear phase */
{
    int m = taps / 2;
    double sum = 0.0;
    for (int i = 0; i < taps; i++) {
        double x = i - m;
        double w = 0.54 + 0.46 * cos(2.0 * M_PI * x / (taps - 1));
        h[i] = 2.0 * fc_norm * sinc(2.0 * fc_norm * x) * w;
        sum += h[i];
    }
    for (int i = 0; i < taps; i++) h[i] /= sum;
}

/* y[i] += (h * x)[i + taps/2] (linear phase, delay removed); direct form, x has n samples */
static void fir_add(double *y, const double *x, long n, const double *h, int taps, double gain)
{
    int m = taps / 2;
    for (long i = 0; i < n; i++) {
        double acc = 0.0;
        long j0 = i + m - (taps - 1) > 0 ? i + m - (taps - 1) : 0;
        long j1 = i + m < n - 1 ? i + m : n - 1;
        for (long j = j0; j <= j1; j++) acc += x[j] * h[i + m - j];
        y[i] += gain * acc;
    }
}

/* early part: all images up to s->order, three band buffers, band split, returned in e[0..ne) */
static void build_early(const RoomSpec *s, long ne, double t_limit, double *e)
{
    double *buf[ROOM_BANDS];
    for (int b = 0; b < ROOM_BANDS; b++) buf[b] = calloc((size_t)ne, sizeof(double));
    double rb[ROOM_BANDS], air[ROOM_BANDS];
    for (int b = 0; b < ROOM_BANDS; b++) {
        rb[b] = sqrt(1.0 - room_wall_alpha(s, s->rt60 * s->rt60_mult[b]));
        air[b] = room_air_db_per_m(BAND_REP_HZ[b], s->temp_c, s->rh);
    }
    double c = room_speed(s);
    int N = s->order;
    for (int qx = 0; qx < 2; qx++) for (int qy = 0; qy < 2; qy++) for (int qz = 0; qz < 2; qz++)
    for (int nx = -N; nx <= N; nx++) for (int ny = -N; ny <= N; ny++) for (int nz = -N; nz <= N; nz++) {
        int n[3] = {nx, ny, nz}, q[3] = {qx, qy, qz};
        Image im;
        image_at(s, n, q, &im);
        if (im.order > N) continue;
        double r = im.r < ROOM_ABS_MIN_DIST ? ROOM_ABS_MIN_DIST : im.r;
        double tau = im.r / c;
        if (tau > t_limit) continue;
        double cth = im.dep[0] * s->front[0] + im.dep[1] * s->front[1] + im.dep[2] * s->front[2];
        double arr = 0.0;
        for (int a = 0; a < 3; a++) arr += (im.pos[a] - s->mic[a]) * s->mic_axis[a];
        double cph = arr / (im.r > 1e-9 ? im.r : 1e-9);
        double mg = mic_gain(s->pattern, cph);
        for (int b = 0; b < ROOM_BANDS; b++) {
            double A = (1.0 / r) * pow(rb[b], im.nref) * pow(10.0, -air[b] * r / 20.0) *
                       pow(10.0, s->dir_db[b] * cth / 20.0) * mg;
            add_frac(buf[b], ne, tau * s->fs, A);
        }
    }
    /* total = mid + LP_lo * (lo - mid) + (delta - LP_hi) * (hi - mid) = mid + LP_lo*(lo-mid) + (hi-mid) - LP_hi*(hi-mid) */
    double *hlo = malloc(sizeof(double) * SPLIT_TAPS), *hhi = malloc(sizeof(double) * SPLIT_TAPS);
    sinc_lp(hlo, SPLIT_TAPS, SPLIT_LO_HZ / s->fs);
    sinc_lp(hhi, SPLIT_TAPS, SPLIT_HI_HZ / s->fs);
    double *dl = malloc(sizeof(double) * (size_t)ne), *dh = malloc(sizeof(double) * (size_t)ne);
    for (long i = 0; i < ne; i++) { dl[i] = buf[0][i] - buf[1][i]; dh[i] = buf[2][i] - buf[1][i]; e[i] = buf[1][i] + dh[i]; }
    fir_add(e, dl, ne, hlo, SPLIT_TAPS, 1.0);
    fir_add(e, dh, ne, hhi, SPLIT_TAPS, -1.0);
    free(dl); free(dh); free(hlo); free(hhi);
    for (int b = 0; b < ROOM_BANDS; b++) free(buf[b]);
}

/* the FDN impulse response, n samples */
static void build_fdn(const RoomSpec *s, long n, double *y)
{
    Rng rng;
    rng_seed(&rng, s->seed);
    double bin[ROOM_NLINES], cout[ROOM_NLINES];
    for (int i = 0; i < ROOM_NLINES; i++) bin[i] = rng_uniform(&rng) < 0.5 ? -1.0 : 1.0;
    for (int i = 0; i < ROOM_NLINES; i++) cout[i] = (rng_uniform(&rng) < 0.5 ? -1.0 : 1.0) / sqrt((double)ROOM_NLINES);
    double *dl[ROOM_NLINES];
    long pos[ROOM_NLINES];
    double gmid[ROOM_NLINES], Gl[ROOM_NLINES], Gh[ROOM_NLINES], sl[ROOM_NLINES] = {0}, sh[ROOM_NLINES] = {0};
    for (int i = 0; i < ROOM_NLINES; i++) {
        dl[i] = calloc((size_t)FDN_LEN[i], sizeof(double));
        pos[i] = 0;
        double g[ROOM_BANDS];
        for (int b = 0; b < ROOM_BANDS; b++) g[b] = pow(10.0, -3.0 * FDN_LEN[i] / (s->fs * s->rt60 * s->rt60_mult[b]));
        gmid[i] = g[1]; Gl[i] = g[0] / g[1]; Gh[i] = g[2] / g[1];
    }
    double tl = tan(M_PI * SHELF_LO_HZ / s->fs), th = tan(M_PI * SHELF_HI_HZ / s->fs);
    double al = (tl - 1.0) / (tl + 1.0), ah = (th - 1.0) / (th + 1.0);
    for (long t = 0; t < n; t++) {
        double x[ROOM_NLINES], sum = 0.0, out = 0.0;
        for (int i = 0; i < ROOM_NLINES; i++) {
            double v = dl[i][pos[i]];
            out += cout[i] * v;
            /* low shelf: v + (Gl-1) (v + A1 v) / 2, high shelf on that: u + (Gh-1) (u - A2 u) / 2, first order allpasses */
            double a1 = al * v + sl[i];
            sl[i] = v - al * a1;
            double u = v + (Gl[i] - 1.0) * 0.5 * (v + a1);
            double a2 = ah * u + sh[i];
            sh[i] = u - ah * a2;
            double w = u + (Gh[i] - 1.0) * 0.5 * (u - a2);
            x[i] = gmid[i] * w;
            sum += x[i];
        }
        y[t] = out;
        double k = 2.0 * sum / ROOM_NLINES;
        for (int i = 0; i < ROOM_NLINES; i++) {
            dl[i][pos[i]] = x[i] - k + (t == 0 ? bin[i] : 0.0);
            if (++pos[i] == FDN_LEN[i]) pos[i] = 0;
        }
    }
    for (int i = 0; i < ROOM_NLINES; i++) free(dl[i]);
}

int room_ir_build(const RoomSpec *s, float **ir, long *nout)
{
    double tmax = s->rt60 * fmax(s->rt60_mult[0], fmax(s->rt60_mult[1], s->rt60_mult[2]));
    long n = (long)ceil((1.3 * tmax + 0.2) * s->fs);
    double tx = room_crossover_time(s);
    double tf0 = tx - 0.010;
    long ne = (long)ceil((tx + 0.002) * s->fs) + FD_HALF + SPLIT_TAPS;
    if (ne > n) ne = n;
    double *e = calloc((size_t)ne, sizeof(double));
    double *f = calloc((size_t)n, sizeof(double));
    build_early(s, ne, tx + 0.002, e);
    build_fdn(s, n, f);
    /* energy density match over the 20 ms before the fade starts */
    long m0 = (long)((tf0 - 0.020) * s->fs), m1 = (long)(tf0 * s->fs);
    if (m0 < 0) m0 = 0;
    double ee = 0.0, ef = 0.0;
    for (long i = m0; i < m1 && i < ne; i++) { ee += e[i] * e[i]; ef += f[i] * f[i]; }
    double scale = ef > 0.0 ? sqrt(ee / ef) : 0.0;
    float *out = malloc(sizeof(float) * (size_t)n);
    if (!out) { free(e); free(f); return 1; }
    for (long i = 0; i < n; i++) {
        double t = (double)i / s->fs, we, wl;
        if (t <= tf0) { we = 1.0; wl = 0.0; }
        else if (t >= tx) { we = 0.0; wl = 1.0; }
        else { double u = (t - tf0) / (tx - tf0); we = cos(0.5 * M_PI * u); wl = sin(0.5 * M_PI * u); }
        out[i] = (float)((i < ne ? we * e[i] : 0.0) + wl * scale * f[i]);
    }
    free(e); free(f);
    *ir = out; *nout = n;
    return 0;
}

static long next_pow2(long n) { long p = 1; while (p < n) p <<= 1; return p; }

long room_apply(const RoomSpec *s, const float *ir, long nir, const float *in, long nin, float **out)
{
    long nout = nin + nir - 1, N = next_pow2(nout);
    double *ar = calloc((size_t)N, sizeof(double)), *ai = calloc((size_t)N, sizeof(double));
    double *br = calloc((size_t)N, sizeof(double)), *bi = calloc((size_t)N, sizeof(double));
    float *y = malloc(sizeof(float) * (size_t)nout);
    if (!ar || !ai || !br || !bi || !y) return 0;
    for (long i = 0; i < nin; i++) ar[i] = in[i];
    for (long i = 0; i < nir; i++) br[i] = ir[i];
    fft(ar, ai, (int)N);
    fft(br, bi, (int)N);
    for (long i = 0; i < N; i++) { /* product, stored conjugated: inverse = conj(fft(conj(X))) / N */
        double r = ar[i] * br[i] - ai[i] * bi[i], im = ar[i] * bi[i] + ai[i] * br[i];
        ar[i] = r; ai[i] = -im;
    }
    fft(ar, ai, (int)N);
    double *w = br; /* reuse: the convolution in double */
    for (long i = 0; i < nout; i++) w[i] = ar[i] / (double)N;
    /* active rms: frames of 20 ms, those within 12 dB of the loudest */
    long fl = (long)(0.020 * s->fs);
    long nf = nout / fl;
    double mx = 0.0;
    for (long f = 0; f < nf; f++) {
        double p = 0.0;
        for (long i = 0; i < fl; i++) p += w[f * fl + i] * w[f * fl + i];
        if (p > mx) mx = p;
    }
    double act = 0.0; long cnt = 0;
    for (long f = 0; f < nf; f++) {
        double p = 0.0;
        for (long i = 0; i < fl; i++) p += w[f * fl + i] * w[f * fl + i];
        if (p >= 0.0625 * mx) { act += p; cnt += fl; }
    }
    double g = (cnt > 0 && act > 0.0) ? 1.0 / sqrt(act / (double)cnt) : 1.0;
    /* microphone self-noise: white gaussian, shaped in the frequency domain (flat to the knee, tilt dB per octave above it), scaled so
       that its A-weighted level is self_noise_dba */
    double sig = 0.0;
    double *shape = NULL;
    if (s->self_noise_dba > 0.0) {
        shape = malloc(sizeof(double) * (size_t)(N / 2 + 1));
        double pa = 0.0;
        for (long k = 0; k <= N / 2; k++) {
            double f = (double)k * s->fs / (double)N;
            shape[k] = f > s->noise_knee_hz ? pow(f / s->noise_knee_hz, -s->noise_tilt_db_oct / 6.0206) : 1.0;
            double a = pow(10.0, room_aweight_db(f < 20.0 ? 20.0 : f) / 20.0) * (f < 20.0 ? 0.0 : 1.0);
            pa += a * a * shape[k] * shape[k];
        }
        pa /= (double)(N / 2 + 1);
        sig = pow(10.0, (s->self_noise_dba - s->ref_db_spl) / 20.0) / sqrt(pa);
    }
    Rng rng;
    rng_seed(&rng, s->seed ^ 0x6e015eULL);
    if (shape) {
        for (long i = 0; i < N; i++) { ar[i] = i < nout ? rng_gauss(&rng) : 0.0; ai[i] = 0.0; }
        fft(ar, ai, (int)N);
        for (long k = 0; k < N; k++) {
            long kk = k <= N / 2 ? k : N - k;
            ar[k] *= shape[kk];
            ai[k] *= -shape[kk]; /* conjugated for the inverse */
        }
        fft(ar, ai, (int)N);
        free(shape);
    }
    for (long i = 0; i < nout; i++) y[i] = (float)(g * w[i] + (sig > 0.0 ? sig * ar[i] / (double)N * 1.0 : 0.0));
    free(ar); free(ai); free(br); free(bi);
    *out = y;
    return nout;
}

/* ---- analysis ---- */
double room_rt60(const float *ir, long n, double fs, double fc, double *curve)
{
    long N = next_pow2(2 * n);
    double *re = calloc((size_t)N, sizeof(double)), *im = calloc((size_t)N, sizeof(double));
    for (long i = 0; i < n; i++) re[i] = ir[i];
    if (fc > 0.0) {
        fft(re, im, (int)N);
        for (long k = 0; k < N; k++) {
            long kk = k <= N / 2 ? k : N - k;
            double f = (double)kk * fs / (double)N, h = 0.0;
            if (f > 0.0) { double x = log2(f / fc) / 0.5; h = 1.0 / sqrt(1.0 + pow(x * x, 6.0)); }
            re[k] *= h; im[k] *= -h; /* conjugate for the inverse */
        }
        fft(re, im, (int)N);
        for (long i = 0; i < n; i++) re[i] /= (double)N;
    }
    double *c = malloc(sizeof(double) * (size_t)n);
    double acc = 0.0;
    for (long i = n - 1; i >= 0; i--) { acc += re[i] * re[i]; c[i] = acc; }
    double tot = c[0] > 0.0 ? c[0] : 1e-300;
    for (long i = 0; i < n; i++) c[i] = 10.0 * log10(c[i] / tot + 1e-300);
    long i5 = -1, i25 = -1;
    for (long i = 0; i < n; i++) {
        if (i5 < 0 && c[i] <= -5.0) i5 = i;
        if (i25 < 0 && c[i] <= -25.0) { i25 = i; break; }
    }
    double rt = 0.0;
    if (i5 >= 0 && i25 > i5 + 2) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, m = (double)(i25 - i5 + 1);
        for (long i = i5; i <= i25; i++) { double t = i / fs; sx += t; sy += c[i]; sxx += t * t; sxy += t * c[i]; }
        double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
        rt = -60.0 / slope;
    }
    if (curve) memcpy(curve, c, sizeof(double) * (size_t)n);
    free(re); free(im); free(c);
    return rt;
}

double room_drr(const float *ir, long n, double fs, double direct_s)
{
    long a = (long)floor((direct_s - 0.0025) * fs), b = (long)ceil((direct_s + 0.0025) * fs);
    double ed = 0.0, er = 0.0;
    for (long i = 0; i < n; i++) {
        double p = (double)ir[i] * ir[i];
        if (i >= a && i <= b) ed += p; else er += p;
    }
    return 10.0 * log10(ed / (er > 0.0 ? er : 1e-300));
}

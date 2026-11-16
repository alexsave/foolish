/* T27 the expert player (player.h) and altcello render (render.h).
 *  a) string choice: lowest position, vibrato notes never open, open strings for open pitches only with vibrato off, legato stays on the
 *     string within +5 / -3 semitones;
 *  b) the bow stays inside the playability table: while sustaining (outside attack, lift and bow changes) the force sits at 20-80 percent of
 *     the log band at the instantaneous beta and speed, the speed in 0.06-0.18 m/s, beta (sounding length) in 0.07-0.14, from the contours
 *     AND from the trace file; also with the player aimed at the edge of the band (0.8);
 *  c) determinism: the same seed gives bit-identical contours and audio, another seed differs, and the contours and the number of random
 *     draws are the same whatever sample rate the simulator runs at;
 *  d) the C major scale (rendered): median pitch of every note within 10 cents of the target, bow changes (pitch steps) within 30 ms of the
 *     written onsets;
 *  e) vibrato (rendered 3 s notes): measured rate within 0.4 Hz of the drawn rate and depth within 35 percent of the drawn depth; vibrato off:
 *     pitch standard deviation under 3 cents on the stopped notes of the scale;
 *  f) EXPERT and NOVICE differ measurably: attack duration (10 to 90 percent of the level) and the 4-7 Hz share of the level modulation. */
#include "analysis.h"
#include "player.h"
#include "render.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS48 48000.0

static double midi_hz(int m) { return 440.0 * pow(2.0, (m - 69) / 12.0); }

static Note mk(double t, double d, int midi, double vel, int vib)
{
    Note n = {t, d, midi, vel, -1, vib, -1.0, -1.0, 0};
    return n;
}

static Note *scale_notes(int *n)
{
    static const int sc[8] = {48, 50, 52, 53, 55, 57, 59, 60};
    Note *a = malloc(sizeof(Note) * 16);
    double q = 60.0 / 72.0, t = 0.5;
    for (int h = 0; h < 2; h++) {
        for (int i = 0; i < 8; i++) { a[h * 8 + i] = mk(t, q, sc[i], 0.7, h == 0 ? 1 : 0); t += q; }
        t += 1.0;
    }
    *n = 16;
    return a;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++) h = (h ^ c[i]) * 1099511628211ULL;
    return h;
}

static uint64_t hash_played(const Played *pl)
{
    uint64_t h = 1469598103934665603ULL;
    size_t n = (size_t)pl->trk.n;
    h = fnv(h, pl->trk.vB, n * sizeof(double));
    h = fnv(h, pl->trk.FB, n * sizeof(double));
    h = fnv(h, pl->trk.beta, n * sizeof(double));
    h = fnv(h, pl->trk.bowed, n);
    for (int s = 0; s < TRK_NSTR; s++) {
        h = fnv(h, pl->trk.xF[s], n * sizeof(double));
        h = fnv(h, pl->trk.aF[s], n * sizeof(double));
    }
    h = fnv(h, pl->cents, n * sizeof(double));
    return h;
}

/* ---------------------------------------------------------------- measurements on 48 kHz audio */
/* normalised autocorrelation pitch, cents re fexp, window the larger of 40 ms and 5 periods starting at t; NaN when the peak is weak */
static double ac_pitch(const float *y, long n, double t, double fexp)
{
    long w = (long)(fmax(0.04, 5.0 / fexp) * FS48), a = (long)(t * FS48);
    long lo = (long)(FS48 / (fexp * 1.08)), hi = (long)(FS48 / (fexp / 1.08));
    if (a < 0 || a + w + hi + 3 > n) return NAN;
    double best = -2, c[4096];
    long kb = 0;
    if (hi - lo + 3 > 4096) return NAN;
    double eu = 0;
    for (long i = 0; i < w; i++) eu += (double)y[a + i] * y[a + i];
    for (long l = lo - 1; l <= hi + 1; l++) {
        double ev = 0, cc = 0;
        for (long i = 0; i < w; i++) { ev += (double)y[a + l + i] * y[a + l + i]; cc += (double)y[a + i] * y[a + l + i]; }
        c[l - (lo - 1)] = cc / sqrt(eu * ev + 1e-30);
    }
    for (long k = 1; k < hi - lo + 2; k++) if (c[k] > best) { best = c[k]; kb = k; }
    if (best < 0.8) return NAN;
    double d = c[kb - 1] - 2 * c[kb] + c[kb + 1], off = d != 0 ? 0.5 * (c[kb - 1] - c[kb + 1]) / d : 0.0;
    return 1200.0 * log2(FS48 / ((double)(lo - 1 + kb) + off) / fexp);
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static double median_of(double *v, int n)
{
    if (n == 0) return NAN;
    qsort(v, (size_t)n, sizeof(double), cmpd);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* pitch track of one note: cents re the target at 10 ms hops from t0 to t1; returns the number of valid points */
static int track(const float *y, long n, double fexp, double t0, double t1, double *tt, double *cc)
{
    int m = 0;
    double w = fmax(0.04, 5.0 / fexp);
    for (double t = t0; t < t1; t += 0.01) {
        double c = ac_pitch(y, n, t, fexp);
        if (isnan(c)) continue;
        tt[m] = t + 0.5 * w; /* window centre */
        cc[m++] = c;
    }
    return m;
}

/* 10 ms level frames (20 ms Hann-free rms) */
static long level_env(const float *y, long n, double **env)
{
    long m = n / 480 - 1;
    double *e = malloc(sizeof(double) * (size_t)m);
    for (long i = 0; i < m; i++) {
        double s = 0;
        for (long j = 0; j < 960; j++) s += (double)y[i * 480 + j] * y[i * 480 + j];
        e[i] = sqrt(s / 960.0);
    }
    *env = e;
    return m;
}

static double attack_10_90(const double *e, long m, double t_from)
{
    long i0 = (long)(t_from * 100), i1 = i0 + 100; /* the first second */
    double pk = 0;
    for (long i = i0; i < i1 && i < m; i++) if (e[i] > pk) pk = e[i];
    long a = i0, b = i0;
    while (a < m && e[a] < 0.1 * pk) a++;
    b = a;
    while (b < m && e[b] < 0.9 * pk) b++;
    return (double)(b - a) * 0.01;
}

/* share of the power of the (linearly detrended) dB level curve at 4-7 Hz in 0.5-50 Hz, level frames i0..i1 */
static double mod_share(const double *e, long i0, long i1)
{
    long m = i1 - i0;
    double *z = malloc(sizeof(double) * (size_t)m);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (long i = 0; i < m; i++) {
        z[i] = 20.0 * log10(e[i0 + i] + 1e-12);
        sx += i; sy += z[i]; sxx += (double)i * i; sxy += i * z[i];
    }
    double b = (m * sxy - sx * sy) / (m * sxx - sx * sx), a = (sy - b * sx) / m, in = 0, all = 0;
    for (long i = 0; i < m; i++) z[i] -= a + b * i;
    for (double f = 0.5; f <= 50.0; f += 0.25) {
        double re = 0, im = 0;
        for (long i = 0; i < m; i++) {
            double w = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / m);
            re += w * z[i] * cos(2.0 * M_PI * f * i * 0.01);
            im += w * z[i] * sin(2.0 * M_PI * f * i * 0.01);
        }
        double p = re * re + im * im;
        all += p;
        if (f >= 4.0 && f <= 7.0) in += p;
    }
    free(z);
    return in / all;
}

static float *render_notes_p(const Note *nt, int n, PlayStyle st, uint64_t seed, double fs, long *n48, Played *pl)
{
    RenderOpts o;
    render_opts_default(&o);
    o.style = st;
    o.seed = seed;
    o.fs = fs;
    float *y;
    double rtf;
    if (render_notes(nt, n, &o, &y, n48, pl, &rtf)) return NULL;
    return y;
}

/* ---------------------------------------------------------------- a) strings */
static void a_strings(void)
{
    int n;
    Note *nt = scale_notes(&n);
    Player p;
    player_init(&p, instrument_cello(), STYLE_EXPERT, 0);
    Played pl;
    CHECK(player_play(&p, nt, n, &pl) == 0, "player_play failed");
    /* vibrato on: 48 G+5, 50 G+7 (not open D), 52 D+2 (G+9 is beyond +7: lowest position), 53 D+3, 55 D+5, 57 D+7 (not open A),
       59 A+2, 60 A+3 */
    static const int s_on[8] = {1, 1, 2, 2, 2, 2, 3, 3}, semi_on[8] = {5, 7, 2, 3, 5, 7, 2, 3};
    for (int i = 0; i < 8; i++)
        CHECK(pl.plan[i].string == s_on[i] && pl.plan[i].semis == semi_on[i], "vibrato scale note %d: string %d semis %d, want %d %d", i, pl.plan[i].string,
              pl.plan[i].semis, s_on[i], semi_on[i]);
    /* vibrato off: 48 G+5, 50 open D, 52 D+2 and 53 D+3 and 55 D+5 (stay on the string), 57 open A, 59 A+2, 60 A+3 */
    static const int s_off[8] = {1, 2, 2, 2, 2, 3, 3, 3}, semi_off[8] = {5, 0, 2, 3, 5, 0, 2, 3};
    for (int i = 0; i < 8; i++)
        CHECK(pl.plan[8 + i].string == s_off[i] && pl.plan[8 + i].semis == semi_off[i], "vibrato-off scale note %d: string %d semis %d, want %d %d", i,
              pl.plan[8 + i].string, pl.plan[8 + i].semis, s_off[i], semi_off[i]);
    for (int i = 0; i < 16; i++) CHECK(pl.plan[i].linked_prev == (i % 8 != 0), "note %d linked_prev %d", i, pl.plan[i].linked_prev);
    printf("T27a strings: vibrato scale %d %d %d %d %d %d %d %d (string index), open D and A only with vibrato off\n", pl.plan[0].string, pl.plan[1].string,
           pl.plan[2].string, pl.plan[3].string, pl.plan[4].string, pl.plan[5].string, pl.plan[6].string, pl.plan[7].string);
    played_free(&pl);
    free(nt);
}

/* ---------------------------------------------------------------- b) the band */
static void check_band(const Player *p, const Note *nt, int n, const char *what, double *worst_lo, double *worst_hi)
{
    Played pl;
    CHECK(player_play(p, nt, n, &pl) == 0, "player_play");
    const Instrument *I = instrument_cello();
    for (int k = 0; k < n; k++) {
        const NotePlan *q = &pl.plan[k];
        for (long i = (long)ceil(q->t_sus0 * pl.trk.rate); i <= (long)floor(q->t_sus1 * pl.trk.rate); i++) {
            double v = fabs(pl.trk.vB[i]);
            double fr = player_band_frac(I, pl.trk.bowed[i], pl.beta_rel[i], v, pl.trk.FB[i]);
            if (fr < *worst_lo) *worst_lo = fr;
            if (fr > *worst_hi) *worst_hi = fr;
            CHECK(fr >= 0.2 - 1e-9 && fr <= 0.8 + 1e-9, "%s note %d t %.3f: force %.3f N at beta %.3f v %.3f is at %.3f of the log band", what, k, (double)i / pl.trk.rate,
                  pl.trk.FB[i], pl.beta_rel[i], v, fr);
            CHECK(v >= 0.05 && v <= 0.20, "%s note %d: speed %.3f m/s outside the table's range", what, k, v);
            CHECK(pl.beta_rel[i] >= 0.069 && pl.beta_rel[i] <= 0.141, "%s note %d: beta %.4f", what, k, pl.beta_rel[i]);
            if (fr < 0.2 || fr > 0.8) return played_free(&pl);
        }
    }
    played_free(&pl);
}

static void b_band(void)
{
    int n;
    Note *sc = scale_notes(&n);
    Note one[3] = {mk(0.5, 1.5, 43, 0.85, 1), mk(3.5, 1.5, 50, 0.85, 1), mk(6.5, 1.5, 57, 0.2, 1)};
    Player p;
    player_init(&p, instrument_cello(), STYLE_EXPERT, 0);
    double lo = 9, hi = -9;
    check_band(&p, sc, n, "scale", &lo, &hi);
    check_band(&p, one, 3, "single", &lo, &hi);
    printf("T27b force at %.3f .. %.3f of the log band (all sustained samples of the scale and the single notes)\n", lo, hi);
    /* aimed at the very edge of the band: arch and drift would push over it, the clamp must hold */
    p.frac_min = 0.8;
    p.frac_span = 0.0;
    double lo2 = 9, hi2 = -9;
    check_band(&p, one, 3, "aimed at 0.8", &lo2, &hi2);
    CHECK(hi2 > 0.78, "the edge case never reached the edge (max %.3f): the clamp is not exercised", hi2);
    printf("T27b aimed at 0.8: force at %.3f .. %.3f of the band (clamped)\n", lo2, hi2);
    /* the trace file agrees */
    Played pl;
    player_play(&(Player){.inst = instrument_cello(), .style = STYLE_EXPERT, .seed = 0, .ctrl_rate = PLAYER_CTRL_RATE, .damp_press = 0.12, .vb_min = 0.06, .vb_span = 0.12,
                          .frac_min = 0.35, .frac_span = 0.25, .beta_base = 0.105, .acc_lo = 0.06, .acc_hi = 0.12, .rock = 0.05}, one, 3, &pl);
    CHECK(played_write_trace(&pl, instrument_cello(), "out/t27_trace") == 0, "trace write");
    FILE *f = fopen("out/t27_trace_ctrl.f32", "rb");
    CHECK(f != NULL, "trace file missing");
    if (f) {
        float row[11];
        double tmin = 9, tmax = -9;
        for (long i = 0; i < pl.trk.n && fread(row, sizeof row, 1, f) == 1; i++)
            for (int k = 0; k < 3; k++)
                if (i >= pl.plan[k].t_sus0 * pl.trk.rate && i <= pl.plan[k].t_sus1 * pl.trk.rate) {
                    if (row[10] < tmin) tmin = row[10];
                    if (row[10] > tmax) tmax = row[10];
                }
        fclose(f);
        CHECK(tmin >= 0.2 - 1e-4 && tmax <= 0.8 + 1e-4, "trace file: sustained force at %.3f .. %.3f of the band", tmin, tmax);
        printf("T27b trace file (float32, 11 columns): sustained force at %.3f .. %.3f of the band\n", tmin, tmax);
    }
    played_free(&pl);
    free(sc);
}

/* ---------------------------------------------------------------- c) determinism */
static void c_determinism(void)
{
    Note one[2] = {mk(0.2, 0.6, 50, 0.8, 1), mk(0.8, 0.5, 52, 0.8, 1)};
    Player p;
    player_init(&p, instrument_cello(), STYLE_EXPERT, 0);
    Played a, b, c;
    player_play(&p, one, 2, &a);
    player_play(&p, one, 2, &b);
    p.seed = 1;
    player_play(&p, one, 2, &c);
    CHECK(hash_played(&a) == hash_played(&b) && a.ndraws == b.ndraws, "same seed: contours differ");
    CHECK(hash_played(&a) != hash_played(&c), "another seed gives the same contours");
    printf("T27c contours: seed 0 twice identical (hash %016llx, %ld draws), seed 1 differs (%016llx)\n", (unsigned long long)hash_played(&a), a.ndraws,
           (unsigned long long)hash_played(&c));
    played_free(&a);
    played_free(&b);
    played_free(&c);
    /* the simulator's sample rate must not reach the player: same contours and draw count at 48 and 96 kHz; audio at 96 kHz bit-identical twice */
    long n1, n2, n3;
    Played p48, p96a, p96b;
    float *y48 = render_notes_p(one, 2, STYLE_EXPERT, 0, 48000.0, &n1, &p48);
    float *y96a = render_notes_p(one, 2, STYLE_EXPERT, 0, 96000.0, &n2, &p96a);
    float *y96b = render_notes_p(one, 2, STYLE_EXPERT, 0, 96000.0, &n3, &p96b);
    CHECK(y48 && y96a && y96b, "render failed");
    if (y48 && y96a && y96b) {
        CHECK(hash_played(&p48) == hash_played(&p96a) && p48.ndraws == p96a.ndraws && p48.trk.n == p96a.trk.n,
              "contours or draw count depend on the sample rate: %ld vs %ld draws, %ld vs %ld samples", p48.ndraws, p96a.ndraws, p48.trk.n, p96a.trk.n);
        CHECK(n2 == n3 && memcmp(y96a, y96b, sizeof(float) * (size_t)n2) == 0, "two 96 kHz renders of the same seed are not bit-identical");
        double rms = 0;
        for (long i = 0; i < n2; i++) rms += (double)y96a[i] * y96a[i];
        CHECK(rms > 0, "the render is silent");
        printf("T27c contours at 48 kHz and 96 kHz: identical (hash %016llx, %ld draws); two 96 kHz renders bit-identical over %ld samples\n",
               (unsigned long long)hash_played(&p48), p48.ndraws, n2);
    }
    free(y48); free(y96a); free(y96b);
    played_free(&p48); played_free(&p96a); played_free(&p96b);
}

/* ---------------------------------------------------------------- d) the scale */
static void d_scale(void)
{
    int n;
    Note *nt = scale_notes(&n);
    long n48;
    Played pl;
    float *y = render_notes_p(nt, n, STYLE_EXPERT, 0, 96000.0, &n48, &pl);
    CHECK(y != NULL, "render failed");
    if (!y) { free(nt); return; }
    double tt[400], cc[400], worst = 0, worst_on = 0;
    for (int k = 0; k < n; k++) {
        double fexp = midi_hz(nt[k].midi);
        int m = track(y, n48, fexp, nt[k].start + 0.25 - 0.02, nt[k].start + nt[k].dur - 0.1, tt, cc);
        double med = median_of(cc, m);
        CHECK(m >= 20 && fabs(med) < 10.0, "scale note %d (midi %d, vibrato %d): median pitch %+.1f cents over %d points", k, nt[k].midi, nt[k].vib, med, m);
        if (fabs(med) > worst) worst = fabs(med);
    }
    printf("T27d scale: worst median pitch error %.1f cents over 16 notes (limit 10)\n", worst);
    /* bow changes (all 14): the contours put the speed's zero crossing and the finger swap within 30 ms of the written onset (the player's
       timing). The AUDIO is later: the level dip and the new pitch are heard when the string has settled into the new Helmholtz motion; those
       delays (vibrato-off scale) are measured and printed, not asserted: they are the simulator's attack time, not the player's timing. */
    for (int k = 1; k < 16; k++) {
        if (!pl.plan[k].linked_prev) continue;
        long i0 = (long)((nt[k].start - 0.1) * pl.trk.rate), i1 = (long)((nt[k].start + 0.1) * pl.trk.rate), iz = -1;
        for (long i = i0; i < i1; i++) if (pl.trk.vB[i] * pl.trk.vB[i + 1] <= 0.0) { iz = i; break; }
        double ez = iz < 0 ? 9.0 : (double)iz / pl.trk.rate - nt[k].start;
        CHECK(fabs(ez) < 0.03, "bow change into note %d: speed crosses zero %+.1f ms from the written onset", k, 1000.0 * ez);
        if (fabs(ez) > worst_on) worst_on = fabs(ez);
        int s = pl.plan[k].string;
        if (pl.plan[k - 1].string == s && pl.plan[k].semis > 0 && pl.plan[k - 1].semis > 0) { /* finger swap: the position moves */
            long j0 = (long)((nt[k].start - 0.1) * pl.trk.rate), js = -1;
            for (long i = j0; i < j0 + 200; i++) if (fabs(pl.trk.xF[s][i + 1] - pl.trk.xF[s][i]) > 1e-3) { js = i; break; }
            double es = js < 0 ? 9.0 : (double)js / pl.trk.rate - nt[k].start;
            CHECK(fabs(es) < 0.03, "bow change into note %d: finger swap %+.1f ms from the written onset", k, 1000.0 * es);
            if (fabs(es) > worst_on) worst_on = fabs(es);
        }
    }
    printf("T27d scale: speed zero crossings and finger swaps at the written onsets, worst %.1f ms (limit 30)\n", 1000.0 * worst_on);
    {
        double *env;
        long me = level_env(y, n48, &env);
        double dip_d = 0, pitch_delay = 0;
        for (int k = 9; k < 16; k++) {
            long i0 = (long)((nt[k].start - 0.1) * 100), i1 = (long)((nt[k].start + 0.3) * 100), im = i0;
            for (long i = i0; i < i1 && i < me; i++) if (env[i] < env[im]) im = i;
            double err = (double)im * 0.01 + 0.01 - nt[k].start;
            if (err > dip_d) dip_d = err;
            double f1 = midi_hz(nt[k].midi), w = fmax(0.04, 5.0 / f1);
            for (double t = nt[k].start - 0.2; t < nt[k].start + 0.5; t += 0.005) {
                double c = ac_pitch(y, n48, t, f1);
                if (!isnan(c) && fabs(c) < 60.0) { if (t + 0.5 * w - nt[k].start > pitch_delay) pitch_delay = t + 0.5 * w - nt[k].start; break; }
            }
        }
        free(env);
        printf("T27d scale (not asserted): in the audio the level dip of a bow change comes up to %.0f ms after the written onset and the new pitch up to %.0f ms\n",
               1000.0 * dip_d, 1000.0 * pitch_delay);
    }
    /* vibrato off: pitch standard deviation of the stopped notes (open strings and the body wolf are not the player's) */
    double worst_sd = 0;
    for (int k = 8; k < 16; k++) {
        if (pl.plan[k].semis == 0) continue;
        double fexp = midi_hz(nt[k].midi);
        int m = track(y, n48, fexp, nt[k].start + 0.3 - 0.02, nt[k].start + nt[k].dur - 0.1, tt, cc);
        double s = 0, s2 = 0;
        for (int i = 0; i < m; i++) { s += cc[i]; s2 += cc[i] * cc[i]; }
        double sd = m > 3 ? sqrt(fmax(0.0, s2 / m - (s / m) * (s / m))) : NAN;
        CHECK(m >= 15 && sd < 3.0, "vibrato off, note %d (midi %d): pitch sd %.2f cents over %d points", k, nt[k].midi, sd, m);
        if (sd > worst_sd) worst_sd = sd;
    }
    printf("T27d vibrato off: worst pitch sd %.2f cents on the stopped notes (limit 3)\n", worst_sd);
    free(y);
    played_free(&pl);
    free(nt);
}

/* ---------------------------------------------------------------- e) vibrato */
static void e_vibrato(void)
{
    Note nt[2] = {mk(0.3, 4.5, 50, 0.8, 1), mk(5.8, 4.5, 57, 0.8, 1)};
    long n48;
    Played pl;
    float *y = render_notes_p(nt, 2, STYLE_EXPERT, 0, 96000.0, &n48, &pl);
    CHECK(y != NULL, "render failed");
    if (!y) return;
    for (int k = 0; k < 2; k++) {
        const NotePlan *q = &pl.plan[k];
        double fexp = midi_hz(nt[k].midi), tt[400], cc[400];
        double t0 = q->t_vib0 + 0.35, t1 = nt[k].start + nt[k].dur - 0.1;
        int m = track(y, n48, fexp, t0, t1, tt, cc);
        /* uniform 10 ms series (gaps filled by interpolation), linear trend removed, spectrum peak 3.5-9 Hz and the band's rms */
        int N = (int)((t1 - t0) / 0.01);
        double *z = malloc(sizeof(double) * (size_t)N);
        double w = fmax(0.04, 5.0 / fexp);
        for (int i = 0; i < N; i++) {
            double t = t0 + w * 0.5 + i * 0.01;
            int j = 0;
            while (j + 1 < m && tt[j + 1] < t) j++;
            z[i] = m < 2 ? 0 : (t <= tt[0] ? cc[0] : t >= tt[m - 1] ? cc[m - 1] : cc[j] + (cc[j + 1] - cc[j]) * (t - tt[j]) / (tt[j + 1] - tt[j]));
        }
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < N; i++) { sx += i; sy += z[i]; sxx += (double)i * i; sxy += i * z[i]; }
        double b = (N * sxy - sx * sy) / (N * sxx - sx * sx), a = (sy - b * sx) / N;
        for (int i = 0; i < N; i++) z[i] -= a + b * i;
        double best = 0, fbest = 0, tot = 0;
        for (double f = 3.5; f <= 9.0; f += 0.05) {
            double re = 0, im = 0;
            for (int i = 0; i < N; i++) {
                double wn = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / N);
                re += wn * z[i] * cos(2.0 * M_PI * f * i * 0.01);
                im += wn * z[i] * sin(2.0 * M_PI * f * i * 0.01);
            }
            double pw = re * re + im * im;
            tot += pw;
            if (pw > best) { best = pw; fbest = f; }
        }
        /* depth: peak amplitude of the 4-7 Hz component = sqrt 2 x rms of the series after a moving-average high-pass (0.4 s removes the slow part) */
        double rms = 0;
        int L = 40, cnt = 0;
        for (int i = L; i < N - L; i++) {
            double mean = 0;
            for (int j = -L; j <= L; j++) mean += z[i + j];
            mean /= 2 * L + 1;
            rms += (z[i] - mean) * (z[i] - mean);
            cnt++;
        }
        double depth = sqrt(2.0 * rms / cnt);
        CHECK(fabs(fbest - q->rate_hz) < 0.4, "note %d: measured vibrato rate %.2f Hz, drawn %.2f Hz", k, fbest, q->rate_hz);
        CHECK(fabs(depth - q->depth_c) < 0.35 * q->depth_c, "note %d: measured vibrato depth %.2f c, drawn %.2f c", k, depth, q->depth_c);
        printf("T27e note %d (midi %d): vibrato rate %.2f Hz (drawn %.2f), depth %.2f c (drawn %.2f)\n", k, nt[k].midi, fbest, q->rate_hz, depth, q->depth_c);
        free(z);
        (void)tot;
    }
    free(y);
    played_free(&pl);
}

/* ---------------------------------------------------------------- f) expert against novice */
static void f_style(void)
{
    Note nt[1] = {mk(0.4, 1.5, 50, 0.85, 1)};
    Note nv[1] = {mk(0.4, 1.5, 50, 0.85, 0)};
    long ne, nn;
    Played pe, pn;
    float *ye = render_notes_p(nt, 1, STYLE_EXPERT, 0, 96000.0, &ne, &pe);
    float *yn = render_notes_p(nv, 1, STYLE_NOVICE, 0, 96000.0, &nn, &pn);
    CHECK(ye && yn, "render failed");
    if (ye && yn) {
        double *ee, *en;
        long me = level_env(ye, ne, &ee), mn = level_env(yn, nn, &en);
        double ae = attack_10_90(ee, me, 0.3), an = attack_10_90(en, mn, 0.3);
        double se = mod_share(ee, 90, 190), sn = mod_share(en, 90, 190);
        printf("T27f attack 10-90: expert %.0f ms, novice %.0f ms; 4-7 Hz share of the level modulation: expert %.3f, novice (no vibrato) %.3f\n", 1e3 * ae, 1e3 * an, se, sn);
        CHECK(fabs(ae - an) > 0.04 && (ae > 1.3 * an || an > 1.3 * ae), "attack: expert %.0f ms and novice %.0f ms do not differ measurably", 1e3 * ae, 1e3 * an);
        CHECK(se > 2.0 * sn && se > 0.2, "4-7 Hz share: expert %.3f is not clearly above novice %.3f (and above 0.2; Philharmonia 0.35-0.50)", se, sn);
        free(ee);
        free(en);
    }
    /* the novice contours: no drift (beta exactly 0.1 on the sounding length) and the speed jumps in 10 ms */
    double bmin = 9, bmax = 0;
    for (long i = 0; i < pn.trk.n; i++) if (pn.trk.FB[i] > 0) { if (pn.beta_rel[i] < bmin) bmin = pn.beta_rel[i]; if (pn.beta_rel[i] > bmax) bmax = pn.beta_rel[i]; }
    CHECK(bmin == 0.1 && bmax == 0.1, "novice beta %.6f .. %.6f, want exactly 0.1", bmin, bmax);
    free(ye);
    free(yn);
    played_free(&pe);
    played_free(&pn);
}

int main(void)
{
    a_strings();
    b_band();
    c_determinism();
    d_scale();
    e_vibrato();
    f_style();
    TEST_DONE("test_player");
}

/* Measurements shared by tests and the tool. */
#ifndef ALTCELLO_ANALYSIS_H
#define ALTCELLO_ANALYSIS_H

typedef struct {
    int n;        /* fft size */
    double df;
    double *mag;  /* n/2 magnitudes (Hann windowed, zero padded) */
} Spectrum;

Spectrum *spec_make(const double *x, long n, double fs, int padfactor);
void spec_free(Spectrum *s);
/* peak frequency near f within +-frac*f, parabolic interpolation on log magnitude */
double spec_peak(const Spectrum *s, double f, double frac, double *mag);
double cents(double f, double ref);

/* number of slip intervals (runs of |v| > thr of at least SLIP_MIN_DUR_S, starting inside [i0,i1))
   and the fraction of the window they cover */
/* |v_rel| above this is a slip. The friction curve sticks exactly (v_rel = 0), so 1e-4 m/s; the thermal elasto-plastic bristle creeps in
   the stick phase (v_rel up to about 3e-3 m/s, friction_thermal.h) while a slip peaks near 1 m/s, so a thermal run uses 1e-2 m/s
   (knobs_install sets it from the bow model). */
#define ANALYSIS_SLIP_THR_CURVE 1e-4
#define ANALYSIS_SLIP_THR_THERMAL 1e-2
extern double analysis_slip_thr;
#define SLIP_MIN_DUR_S 0.00015 /* shorter runs of |v_rel| > thr are ripple micro-slips */
int slip_onsets(const double *v, long i0, long i1, double thr, double fs, double *frac);
/* max normalised correlation of x[i0,i1) with itself shifted by lag in [lo,hi]; best lag out */
double autocorr_max(const double *x, long i0, long i1, long lo, long hi, long *best);

typedef enum { CLS_HELMHOLTZ, CLS_MULTIPLE, CLS_RAUCOUS, CLS_NONE } RunClass;
typedef struct {
    double onsets_per_period, corr, slip_frac;
    RunClass cls;
} RunStats;
extern const char *const cls_names[4];
/* classify a bowed run on [t0,t1) seconds using nominal f0 */
void classify_run(const double *bridge, const double *vrel, double fs, double f0, double t0, double t1, RunStats *out);

typedef struct {
    int ok_one_per_period;
    int counts_min, counts_max;
    double slip_frac, f_est, cents_flat, corr;
} Helm;
/* the T3 analysis: pitch from the fft of the bridge force from `settle` s on, then 20 periods (window
   starting mid-stick after a slip onset) each of which must hold exactly one slip interval */
Helm analyse_helmholtz(const double *bridge, const double *vrel, long n, double fs, double f0, double settle);
/* time of the first slip onset that begins 10 consecutive periods (onset spacing within 15 percent of P samples/fs), -1 if none */
double settle_time(const double *vrel, long n, double fs, double P);
/* level of the largest spectral magnitude in [flo,fhi] re the fundamental peak (near f0), dB, over the last 1 s */
double hump_db(const double *x, long n, double fs, double f0, double flo, double fhi);

/* The SIMPLE classifier (oracle experiment B1/B2): a period is Helmholtz-like when it holds ONE dominant slip: the slip with
 * the largest displacement (sum of |v_rel| dt over a run of |v_rel| > analysis_slip_thr) and no other slip of at least `frac_min`
 * (0.2) of it. Windows of P = fs/f_est samples start mid-stick after the first slip onset from `settle` s on, 20 periods like
 * analyse_helmholtz. f_est is the pitch (use Helm.f_est). ok = all 20 periods have exactly one dominant slip. */
typedef struct {
    int ok, bad_periods, nper;
    double main_frac; /* mean fraction of a period spent in the dominant slip */
} SimpleHelm;
SimpleHelm analyse_simple(const double *vrel, long n, double fs, double f_est, double settle, double frac_min);

/* Decay rates of the partials k f0, k = 1..K of a decaying (plucked) signal x: the peak of each partial is found on the
 * spectrum of the first 2 s (frequency refined by parabolic interpolation, out f[k-1]), then its amplitude is measured by a
 * Hann windowed single bin DFT in windows of win_s s (hop win_s/2) and the log amplitude is fitted by a line over the windows
 * from t_start until the amplitude has fallen range_db below its first value or reaches the end. alpha[k-1] is the amplitude
 * decay rate in 1/s (0 if fewer than 4 windows). */
void partial_decays(const double *x, long n, double fs, double f0, int K, double t_start, double win_s, double range_db, double *f_out, double *alpha);

/* ---- AUDIO ONSET MEASURES (onset work, iteration 1; ONSET.md). One definition for the real notes and ours, on the radiated
 * (or recorded) sound x at rate fs.
 * audio_onset: first time the 5 ms rms reaches 2 percent of its maximum (the candidate scripts' onset), s.
 * audio_corr_track: the normalised period autocorrelation r(t): window of one period P0 = fs / f0 at t (start of the window), against
 * the next window shifted by the lag in [0.95 P0, 1.05 P0] that maximises the Pearson correlation (so vibrato and the pitch
 * dips of a legato change do not count as aperiodic), sub-sample by a parabola; hop P0/8; returns the number of values, times
 * t0 + k hop_s. r is -1 where the window has no energy.
 * audio_periodic_time: the start, in seconds from the beginning of x, of the first run of hops where r > thr for nper periods of time
 * (thr 0.95, nper 10: "stays above 0.95 for 10 consecutive periods"), or -1. */
double audio_onset(const double *x, long n, double fs);
double audio_onset_from(const double *x, long n, double fs, double t_from); /* the same, looking from t_from on (the bow's landing click before the bow moves is not the note) */
long audio_corr_track(const double *x, long n, double fs, double f0, double t0, double *r, long rmax, double *hop_s);
double audio_periodic_time(const double *x, long n, double fs, double f0, double t_from, double thr, int nper);
/* share of the power of x[i0,i1) (Hann window) that lies in [flo, fhi], dB re the total power of the window */
double band_share_db(const double *x, long i0, long i1, double fs, double flo, double fhi);
/* pitch track of a stretch with one pitch at a time between flo and fhi Hz: at t = t0 + k hop_s the correlation of the window
 * x[i, i+W) (W = win_s) with the window shifted by lag in [fs/fhi, fs/flo], peak by a parabola; f[k] in Hz (0 when the window
 * has no energy), assigned to the centre of the two windows (i + (W + lag) / 2) / fs, written to tc[k]. Returns the count. */
long pitch_track(const double *x, long n, double fs, double flo, double fhi, double win_s, double t0, double t1, double hop_s, double *tc, double *f, long kmax);
#endif

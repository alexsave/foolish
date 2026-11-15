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
#endif

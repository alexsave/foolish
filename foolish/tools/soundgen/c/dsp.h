// dsp.h - the numpy the card models were prototyped with, in C.
//
// The models (tools/soundgen/{deal,flip,throwin,cover,pass,discard}/an/*.py)
// are written against a handful of numpy idioms: noise coloured by a measured
// 1/12-octave template through an FFT, brick-wall band passes, moving RMS
// windows, piecewise-linear envelopes in dB, two-pole resonators. This is that
// handful and nothing more, so a model ports line by line (models.c) and reads
// like its Python.
//
// ONE RATE: 48 kHz, the rate the models and their measured tables are at.
// soundgen resamples a finished take to the rate it ships at (resample_3_2).
//
// NOT BIT-IDENTICAL WITH NUMPY, and not meant to be: the random draws come
// from shared/c/rng.h in a different order, and an FFT here is a power of two
// with zero padding where numpy's is the signal's own length. The bar is that
// a C take MEASURES like the approved Python take (throwin/an/measure.py).
#ifndef SOUNDGEN_DSP_H
#define SOUNDGEN_DSP_H

#include <stdint.h>

#define SR 48000

typedef struct { double *x; int n; } Sig;

// ---- random draws (shared/c/rng.h underneath) ------------------------------
typedef struct { uint64_t s; int has; double spare; } Rng;
void   rng_init(Rng *r, uint64_t seed);
double uni(Rng *r, double a, double b);        // [a, b)
double normal(Rng *r);                         // standard normal
int    irange(Rng *r, int lo, int hi);         // [lo, hi), numpy's integers
int    poisson(Rng *r, double lam);

// ---- signals -----------------------------------------------------------------
Sig    sig_new(int n);                          // zeroed
void   sig_free(Sig *s);
Sig    noise(Rng *r, int n);
double sig_rms(const double *x, int n);
double sig_peak(const double *x, int n);
int    sig_argmax_abs(const Sig *s);
void   sig_scale(Sig *s, double k);
void   sig_add(Sig *dst, const Sig *src, double k);

// ---- spectra -----------------------------------------------------------------
// numpy's interp: piecewise linear, clamped to the end values (or left/right).
double interp(double x, const double *xp, const double *fp, int n, double left, double right);
// The template's gain at f Hz, interpolated on log frequency (the models' eq).
double tmpl_db(double f, const double *cs, const double *tmpl, int n);
// y shaped by a template in dB over the bands `cs` (the models' eq()): every
// FFT bin's gain is the template at its frequency, -60 dB below `lo` Hz and
// above `hi` Hz (hi <= 0 for no top cut). In place. Zero-padded, so a click
// rings symmetrically about where it is; colored_noise is the circular case.
void   eq(Sig *y, const double *cs, const double *tmpl, int n, double lo, double hi);
// Noise of n samples coloured by a template: numpy's eq(rng.standard_normal(n), tmpl).
Sig    colored_noise(Rng *r, int n, const double *cs, const double *tmpl, int nt, double lo, double hi);
// Brick-wall band pass (numpy's bp), in place; lo 0 is a low pass.
void   bp(Sig *y, double lo, double hi);
Sig    bp_copy(const Sig *y, double lo, double hi);

// ---- envelopes and windows ---------------------------------------------------
// np.convolve(x * x, ones(w) / w, "same"), then sqrt: the moving RMS.
void   moving_rms(const double *x, int n, int w, double *out);
// The largest moving RMS over [from, to).
double max_moving_rms(const Sig *y, int w, int from, int to);

// A burst: noise from `at_s` under min(1, t / rise) * exp(-t / (width / 2.3)),
// `mult` widths long, at `db`, added into y. The click, strike, contact and snag
// of every model, which differ only in their rise and length.
void   burst_add(Rng *r, Sig *y, double at_s, double db, double width, double rise, double mult);

// Two-pole resonator (1 - r) / (1 - 2 r cos w z^-1 + r^2 z^-2), r = exp(-pi bw / SR);
// `hz` per sample when freqs is non-NULL. Returns a new signal.
Sig    reson(const Sig *x, double hz, const double *freqs, double bw);

// ---- rates ---------------------------------------------------------------------
// 48 kHz to 32 kHz through a windowed-sinc low pass at 15.6 kHz. Every model
// but the deal's thumb slide cuts at 15 kHz, so this keeps all of them.
Sig    resample_3_2(const Sig *x);

#endif

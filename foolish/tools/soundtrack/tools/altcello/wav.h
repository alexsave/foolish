#ifndef ALTCELLO_WAV_H
#define ALTCELLO_WAV_H
/* Plain float32 mono WAV writer, no libraries. Returns 0 on success. */
int wav_write_f32(const char *path, const float *x, long n, int rate);
/* Low-pass (windowed sinc, 511 taps, cutoff 0.45 * out rate) and decimate by
 * factor; allocates *out (caller frees), returns the number of samples. */
long wav_decimate(const double *x, long n, int factor, float **out);
#endif

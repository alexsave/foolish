/* ALT-CELLO stage 3a: shoebox room impulse response from the cello to a microphone, and the microphone.
 *
 * Early part: image sources up to order 6 (Allen and Berkley 1979), three frequency bands (low < 300 Hz, mid, high > 3 kHz) of
 * wall absorption set so the Eyring RT60 of each band equals its target (Sabine's alpha would make the image set decay faster than
 * the FDN: 0.54 s against 0.6 s at alpha 0.2), 1/r spreading, ISO 9613-1 air absorption per band,
 * windowed-sinc fractional delay, source directivity (ASSUMED, idea from Meyer) and microphone pattern per arrival direction.
 * Late part: an 8 line feedback delay network (Householder matrix, prime lengths, per-line absorption shelves) crossfaded in
 * where the order-6 image set stops being complete, scaled to the same energy density at the crossover.
 * Room frame: X, Y, Z in metres, Z up, floor Z = 0, same axes as geom_to_room (geom.h); the cello stands on the floor, its top
 * plate faces -Y. The IR is for a unit pressure source at 1 m: the direct sound has amplitude 1/r.
 * Level convention of room_apply: 0 dB (rms 1.0) is a nominal 74 dB SPL at the microphone for a forte cello note (ASSUMED). */
#ifndef ALTCELLO_ROOM_H
#define ALTCELLO_ROOM_H
#include "rng.h"
#include <stdint.h>

/* one sub-seed for the microphone noise and the FDN input/output signs: change ALTCELLO_SEED in rng.h to vary both */
#define ALTCELLO_ROOM_SEED (ALTCELLO_SEED ^ 0x40011ULL)

#define ROOM_NLINES 8
#define ROOM_BANDS 3 /* 0 low, 1 mid, 2 high */

enum { ROOM_OMNI = 0, ROOM_CARDIOID = 1 };

typedef struct {
    double fs;            /* 48000 */
    double dim[3];        /* room X Y Z, m */
    double rt60;          /* target RT60 of the mid band, s */
    double rt60_mult[ROOM_BANDS]; /* band target = rt60 * mult; default low 1.2, mid 1.0, high 0.7 (ASSUMED room colour) */
    double src[3];        /* source point (f-hole / bridge region), m */
    double front[3];      /* unit vector the cello faces (top plate normal) */
    double mic[3];        /* microphone capsule, m */
    double mic_axis[3];   /* unit vector the microphone points along */
    int pattern;          /* ROOM_OMNI or ROOM_CARDIOID */
    int order;            /* image source order, 6 */
    double temp_c, rh;    /* air for the absorption: 20 C, 50 percent */
    double dir_db[ROOM_BANDS]; /* source directivity: half the front-back dB per band (gain dB = dir_db * cos(angle from front)) */
    double self_noise_dba;     /* microphone self-noise, dB(A) SPL equivalent: 2 (candidate 2: 10 dB under the 12 of a U 87 Ai, to match the
                                  Philharmonia recordings' floor, ONSET.md), 0 or less: off */
    double noise_knee_hz, noise_tilt_db_oct; /* the noise is white up to the knee (4000 Hz) and falls by tilt dB per octave above it
                                  (3, ASSUMED: closer to the measured floor of the Philharmonia notes than white noise) */
    double ref_db_spl;         /* level of rms 1.0: 74 */
    uint64_t seed;        /* sub-seed of the noise and the FDN signs */
} RoomSpec;

/* the 6 x 5 x 3.2 m room of geom.h, cello from geom_to_room placed at X 2.7 m, Y about 4.0 m facing the open room, the mic
 * `dist` m in front of it at 1.0 m height pointing at the source. */
void room_spec_default(RoomSpec *s, double dist);

/* the IR (float, mono, fs); *ir is malloc'ed, caller frees. 0 on success. */
int room_ir_build(const RoomSpec *s, float **ir, long *n);

/* the wet signal: in convolved with ir (FFT), scaled so the sounding part of the result has rms 1.0 (= ref_db_spl), plus the
 * microphone self-noise over the full length. *out is malloc'ed with nin + nir - 1 samples (the return value), 0 on failure. */
long room_apply(const RoomSpec *s, const float *ir, long nir, const float *in, long nin, float **out);

/* diagnostics */
const int *room_fdn_lengths(int *n); /* delay line lengths of the FDN, samples */
double room_speed(const RoomSpec *s); /* speed of sound, m/s, from temp_c */
double room_wall_alpha(const RoomSpec *s, double rt60); /* uniform wall absorption coefficient whose Eyring RT60 is rt60 (the image set then decays at the same rate as the FDN) */
double room_air_db_per_m(double f, double temp_c, double rh); /* ISO 9613-1 pure tone, 101.325 kPa */
double room_aweight_db(double f);                          /* IEC 61672 A weighting, 0 dB at 1 kHz */
/* RT60 (T20 x 3) of the octave band centred at fc, Schroeder backward integration; if curve != NULL it receives the
 * n-sample decay curve in dB (0 at the start). Returns 0 if the decay does not span -5 to -25 dB. */
double room_rt60(const float *ir, long n, double fs, double fc, double *curve);
/* direct-to-reverberant ratio, dB: the +-2.5 ms window around the direct arrival against the rest */
double room_drr(const float *ir, long n, double fs, double direct_s);
/* image arrival: time of the direct sound, s */
double room_direct_time(const RoomSpec *s);
/* time at which the order-6 image set stops being complete (the earliest order-7 arrival), s */
double room_crossover_time(const RoomSpec *s);

#endif

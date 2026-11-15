/* Radiativity R(f): the weight of each body mode in the radiated pressure
 * p(t) = sum_k r_k q_k'', r_k = 10^(R(f_k)/20).
 * This is a stated CALIBRATION, not first-principles physics: a smooth curve,
 * piecewise linear in dB against log2 frequency through RAD_NPTS control points,
 * fitted so bowed notes across the range match the TARGET harmonic envelope
 * (charts/calibrate_radiation.py writes radiativity_table.c). R is held constant
 * below the first and above the last control point. Only the shape matters; it is
 * anchored at 0 dB at 200 Hz. */
#ifndef ALTCELLO_RADIATIVITY_H
#define ALTCELLO_RADIATIVITY_H

#define RAD_NPTS 12
extern const double radiativity_f[RAD_NPTS];  /* control point frequencies, Hz (generated table) */
extern const double radiativity_db[RAD_NPTS]; /* R at the control points, dB (generated table) */

double radiativity_eval_db(double f);   /* R(f), dB */
double radiativity_gain(double f);      /* 10^(R(f)/20) */
#endif

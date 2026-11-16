/* Decay of the partials of one string: a pluck with the bridge rigid, followed mode by mode.
 * Every simply supported mode sin(p pi x / L) is an exact eigenvector of the discrete scheme (altstring.h), so the modes do not mix and the amplitude of mode p,
 * sqrt(P^2 + (V / w_p)^2) with P and V the projections of the displacement and the velocity, is a clean exponential e^{-alpha t} (a straight line in dB). */
#ifndef ALTCELLO_LOSSMEAS_H
#define ALTCELLO_LOSSMEAS_H
#include "instrument.h"
/* pmax partials of the string (fs, secs of free decay); f[p-1] = frequency Hz of mode p, alpha[p-1] = fitted amplitude decay rate 1/s over the span from 10 ms until the mode is
   80 dB down or the run ends. Returns the number of modes measured (pmax limited by the grid). */
int string_partial_decay(const StringSpec *sp, double fs, double secs, int pmax, double *f, double *alpha);
#endif

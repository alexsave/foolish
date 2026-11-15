#ifndef ALTCELLO_BODY_MODES_H
#define ALTCELLO_BODY_MODES_H
#include "instrument.h"

/* The physical knobs of the body (stage 2 iteration 3), each with a stated plausible range (REFERENCES section 7).
 * body_opts_default(I) is the shipped instrument (the values live in the Instrument, instrument_cello.c). */
typedef struct {
    double eta0;           /* loss factor of the statistical modes at 500 Hz, 0.012 (range 0.008 - 0.02) */
    double eta_exp;        /* eta(f) = eta0 (f / 500 Hz)^eta_exp, 0.3 (range 0 - 0.6) */
    double bridge_inertia; /* bridge rocking inertia m a^2, kg m^2, 2.1e-6; the stiffness K stays at its default value */
    double fc;             /* plate coincidence frequency, Hz, 3000 (2000 - 5000; a value of radiate_plate_fits) */
    double bridge_R;       /* the bridge resistance R_inf, rad/(m s N), 30 (Woodhouse): larger = lower and wider bridge hill */
    double stat_mass_scale; /* factor on the statistical modal masses, 1 (instrument value times it) */
    double lead_mass_scale; /* factor on the effective masses of the named table modes, 1 (a diagnosis knob: heavier = weaker low resonances) */
} BodyOpts;
BodyOpts body_opts_default(const Instrument *I);

typedef struct {
    int n;
    int nlead;                 /* the first nlead modes are the named table modes */
    int ncompact;              /* the first ncompact modes (A0 CBR B1- B1+) radiate as compact monopoles */
    double *f, *Q, *m, *cu, *cw, *r; /* Hz, -, kg (effective mass at the bridge), -, -, and r: for the compact modes the
                                        monopole strength r_k (1.0 or 0.7), for all others the fixed random sign s_k = +-1 of the
                                        plate path (radiate_body.h) */
} BodyModes;

BodyModes *body_modes_build(const Instrument *I);                         /* default knobs */
BodyModes *body_modes_build_opts(const Instrument *I, const BodyOpts *o);       /* deterministic: statistical modes use ALTCELLO_BODY_SEED, signs ALTCELLO_RADIATE_SEED */
void body_modes_free(BodyModes *b);
const char *body_mode_name(const Instrument *I, int i); /* "A0" "CBR" "B1-" "B1+" ... */

/* Woodhouse bridge model (REFERENCES 7.2, eq (6)): the bridge-hill factor |Y_b| / |a^2 R_inf| of the body's mean
 * admittance, Y_b = a^2 (K R + i w) / (K - m a^2 w^2 + i w K m a^2 R), at the bridge inertia I = m a^2 and the
 * default stiffness K. 1 at low frequency; the statistical modal masses are divided by it. */
double body_bridge_gain(const Instrument *I, double f, double inertia);
double body_bridge_gain_R(const Instrument *I, double f, double inertia, double R); /* the same at an explicit R_inf */
/* the frequency of the maximum of the hill factor (Hz, found on a fine log grid) */
double body_bridge_hill_freq(const Instrument *I, double inertia);
double body_eta(const BodyOpts *o, double f);   /* loss factor of the statistical modes */

#endif

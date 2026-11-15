/* Physical radiation of the body (stage 2 iteration 3): the pressure channel is built from the modal VELOCITIES
 * q_k' of the body, not from a free calibration curve.
 *
 * Why. A mode far below the analysis frequency is mass controlled: its acceleration is flat, c_k F / m_k.
 * The old model p = sum_k r_k q_k'' therefore radiated the whole bridge-force spectrum, flat, up to 16 kHz, as if
 * the body were a compact monopole at every frequency (iteration 2: LTAS 18 dB too bright at 3-8 kHz even with the
 * free curve at -60 dB). Physically the body is compact only for ka < 1 (a_eff = 0.25 m, c = 343 m/s: f < 218 Hz);
 * above that, the plate modes radiate with an efficiency sigma(f) << 1 below the coincidence frequency, and the
 * mixed signs of their volume velocities cancel in the far field.
 *
 * Two paths, one total:
 *   p = p_compact + G_PLATE p_plate
 *
 * (a) Compact modes (A0, CBR, B1-, B1+; the first RADIATE_NCOMPACT modes of the table). Each is a monopole of
 *     strength r_k (A0 1.0, the others 0.7, the stage 2 values) driven by the modal velocity. A monopole of
 *     volume velocity Q radiates p = i omega rho Q / (4 pi r) (acceleration like, +6 dB/oct) while ka < 1 and
 *     with the radiation impedance of a pulsating sphere of radius a it becomes velocity like above ka = 1:
 *     the first-order form of the sphere's radiation load, H_c(s) = s tau / (1 + s tau), tau = a_eff / c_air,
 *     applied to sum_k r_k q_k'. Bilinear transform, c = 2 fs tau.
 *     (Pulsating sphere: Z_rad ~ rho c (i ka) / (1 + i ka); Fahy and Gardonio, Sound and Structural Vibration,
 *     2nd ed. 2007; the review of Woodhouse 2014 p.35 says the lowest violin modes below about 800 Hz are
 *     monopole sources.) a_eff = 0.25 m is an equivalent radius of the cello body: ASSUMED.
 *
 * (b) Plate modes (the 260-440 Hz table modes and all statistical modes). Their velocities are summed with fixed
 *     random signs s_k = +-1 (one instrument: sub-seed ALTCELLO_RADIATE_SEED) and weight 1, which stands for the
 *     mixed-sign net volume velocity of a mode field, and the sum passes through the radiation efficiency filter
 *     |H_p(f)| = sqrt(sigma(f)), sigma the baffled-plate efficiency (Maidanik, JASA 34, 809-826, 1962;
 *     Leppington, Broadbent and Heron, Proc. R. Soc. A 382, 245-271, 1982; the form below is the textbook one
 *     of Fahy and Gardonio, recalled, UNVERIFIED in REFERENCES.md section 7.4):
 *       alpha = sqrt(f / f_c), lambda_c = c / f_c
 *       f < f_c : sigma = (P lambda_c / (4 pi^2 A)) [ (1 - alpha^2) ln((1 + alpha)/(1 - alpha)) + 2 alpha ] / (1 - alpha^2)^(3/2)
 *       f > f_c : sigma = 1 / sqrt(1 - f_c / f)
 *     both clamped to 1 (the edge formula and the above-coincidence form both diverge at f_c).
 *     f_c = 3000 Hz (spruce top, h 4 mm, along grain; REFERENCES 7.3), plate area A = 0.17 m^2, perimeter
 *     P = 2.0 m (ASSUMED, REFERENCES 7.3). The magnitude is realised by a minimum-phase IIR: six real zeros and
 *     six real poles fitted (charts/fit_plate_filter.py, minimax in dB, max error below 1 dB on a log grid of
 *     50 Hz - 20 kHz) and kept as the generated table radiate_plate_table.c; the analog sections are turned into
 *     digital ones with the bilinear transform at the simulation rate. Supported f_c values: radiate_plate_fits.
 *
 * (c) The one relative level G_PLATE: with a white bridge force, the two paths have equal average power in the
 *     octave 212-424 Hz (centred on 300 Hz, the lowest octave where the plate modes exist). Measured once by
 *     test_radiate (T16) and set here; the test asserts equality within 1 dB so it cannot drift.
 *
 * The overall level of p is arbitrary (the wav writer normalises it); only the relative level of the two paths and
 * the shapes are physics. */
#ifndef ALTCELLO_RADIATE_BODY_H
#define ALTCELLO_RADIATE_BODY_H

#include "instrument.h"

/* The instrument-specific values (number of compact modes, a_eff, plate area and perimeter, f_c, G_PLATE) are in the
 * Instrument's RadiationSpec / BodySpec (instrument_cello.c); only the speed of sound is a constant here. */
#define RADIATE_C_AIR 343.0     /* m/s */
#define RADIATE_NSEC 6
#define RADIATE_NFC 6

typedef struct {
    double fc, K;
    double z[RADIATE_NSEC], p[RADIATE_NSEC]; /* rad/s */
} PlateFit;
extern const PlateFit radiate_plate_fits[RADIATE_NFC]; /* generated, charts/fit_plate_filter.py */

double radiate_plate_sigma(const RadiationSpec *rs, double f, double fc); /* the formula above, clamped to 1 */
double radiate_compact_mag(const RadiationSpec *rs, double f);            /* |H_c| analog, f in Hz */

typedef struct {
    double fs;
    double c_b0, c_b1, c_a1, c_z;                      /* compact first-order section and its state */
    int ns;
    double K;
    double p_b0[RADIATE_NSEC], p_b1[RADIATE_NSEC], p_a1[RADIATE_NSEC], p_z[RADIATE_NSEC];
    double g_plate;
} Radiator;

/* returns 0, or -1 if fc is not one of the fitted critical frequencies (the Radiator is then unusable) */
int radiator_init(Radiator *R, const RadiationSpec *rs, double fs, double fc, double g_plate);
/* vc = sum_k r_k q_k' of the compact modes, vp = sum_k s_k q_k' of the plate modes; returns the pressure */
double radiator_step(Radiator *R, double vc, double vp);
double radiator_step_compact(Radiator *R, double vc);
double radiator_step_plate(Radiator *R, double vp); /* without g_plate */
/* magnitude of the realised DIGITAL filter at f Hz: path 0 compact, 1 plate (without g_plate) */
double radiator_mag(const Radiator *R, int path, double f);
#endif

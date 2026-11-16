/* One-factor experiment knobs of the instrument (diagnostic tools only, not part of the shipped instrument).
 * knobs_install() reads the environment variable ALTCELLO_KNOBS, a comma separated list of name=value, builds a modified copy of the
 * cello and installs it with instrument_cello_override(), so every tool that links the library (altcello render, sweep_tool,
 * onset_tool, ring_tool, legato) sees the same changed instrument. Unset or empty: the shipped values, nothing installed.
 *   loss=0|1       the string loss: 0 = sigma1 k^2 with Q 300 / 500 at 2 kHz only (candidate 4), 1 = the viscoelastic relaxation loss with a flat Q (altstring.h, instrument_cello.c)
 *   s0=X            frequency independent loss sigma0 of every string, 1/s (default 1.2); sigma1 is recomputed so every string keeps
 *                   its quality factor at 2 kHz
 *   q2k.NAMES=Q     quality factor at 2 kHz of the strings whose first letter is in NAMES (C G D A, e.g. q2k.CG=200)
 *   zeta.NAMES=X    torsional damping ratio of those strings
 *   zt.NAMES=X      torsional surface impedance over Z0 of those strings
 *   fifths=1        tune the open strings in pure fifths from A3 = 220 Hz (the stopped equal-tempered unisons are then 2-4 cents off)
 *   bow=1           thermal elasto-plastic bow friction (friction_thermal.h; 0 = the friction curve, the default)
 *   th_xi=X th_b=X th_s0=X th_ns=N  thermal constants: transition steepness xi, b_tau, bristle stiffness sigma0, substeps per sample
 *   bn_fc=X bn_rate=N bn_level=X   bow noise: low-pass corner (Hz), draws per second of logical time, level (density at 4000 draws per second)
 *   stat_f1=X eta_exp=X mass_exp=X   body: top of the statistical modes (Hz), exponent of their loss factor eta0 (f/500)^e, exponent of their mass law m500 (f/500)^e
 *   bridge_r=X      factor on the bridge resistance R_inf (BRIDGE_R_INF)
 *   bw=X            bow hair contact width, m (0.010)
 *   lead_mass=X     factor on the effective masses of the named (table) body modes
 *   stat_mass=X     factor on the statistical body modal masses
 *   fe=0|1|2        the body: 0 statistical (default), 1 FE-A, 2 FE-B (fe_body.h); fe_path=FILE the BODYMODES file (else data/fin5_cpl.bodymodes.txt of this directory);
 *                   fe_fx=HZ the crossover to the statistical modes (2500); fe_ga=X fe_gb=X the crossover match scale of FE-A / FE-B
 */
#ifndef ALTCELLO_KNOBS_H
#define ALTCELLO_KNOBS_H
#include "instrument.h"
/* parse spec into a copy of the shipped cello; 0 on success, nonzero (message on stderr) for a bad knob */
int knobs_apply(Instrument *I, const char *spec);
/* read ALTCELLO_KNOBS and install the result; exits with a message on a bad spec */
void knobs_install(void);
#endif

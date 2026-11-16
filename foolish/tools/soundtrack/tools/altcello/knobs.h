/* One-factor experiment knobs of the instrument (diagnostic tools only, not part of the shipped instrument).
 * knobs_install() reads the environment variable ALTCELLO_KNOBS, a comma separated list of name=value, builds a modified copy of the
 * cello and installs it with instrument_cello_override(), so every tool that links the library (altcello render, sweep_tool,
 * onset_tool, ring_tool, legato) sees the same changed instrument. Unset or empty: the shipped values, nothing installed.
 *   s0=X            frequency independent loss sigma0 of every string, 1/s (default 1.2); sigma1 is recomputed so every string keeps
 *                   its quality factor at 2 kHz
 *   q2k.NAMES=Q     quality factor at 2 kHz of the strings whose first letter is in NAMES (C G D A, e.g. q2k.CG=200)
 *   zeta.NAMES=X    torsional damping ratio of those strings
 *   zt.NAMES=X      torsional surface impedance over Z0 of those strings
 *   fifths=1        tune the open strings in pure fifths from A3 = 220 Hz (the stopped equal-tempered unisons are then 2-4 cents off)
 *   lead_mass=X     factor on the effective masses of the named (table) body modes
 *   stat_mass=X     factor on the statistical body modal masses
 */
#ifndef ALTCELLO_KNOBS_H
#define ALTCELLO_KNOBS_H
#include "instrument.h"
/* parse spec into a copy of the shipped cello; 0 on success, nonzero (message on stderr) for a bad knob */
int knobs_apply(Instrument *I, const char *spec);
/* read ALTCELLO_KNOBS and install the result; exits with a message on a bad spec */
void knobs_install(void);
#endif

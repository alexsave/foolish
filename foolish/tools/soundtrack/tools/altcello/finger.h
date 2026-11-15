/* Left-hand finger: a pad of raised-cosine weights g_l in [0,1] pressing on
 * the grid points under it, force per metre of string
 *   f_l = -a g_l (K y_l + R y_t,l)      (both polarisations)
 * treated implicitly (centred) in altstring.c, so any K is stable. */
#ifndef ALTCELLO_FINGER_H
#define ALTCELLO_FINGER_H
#include "altstring.h"
#include "instrument.h"

/* pad width m, K N/m^2, R N s/m^2: the instrument's FingerSpec (instrument.h) */
typedef FingerSpec Finger;

void finger_init(Finger *f, const FingerSpec *spec);
/* weights for a pad centred at xF (m from the nut) on a grid of spacing h, points 0..N: fills g[0..hi-lo], sets
 * lo, hi (lo > hi when nothing is covered). g must hold 64 entries. */
void finger_weights(const Finger *f, double h, int N, double xF, int *lo, int *hi, double *g);
/* the pad acts on the transverse motion and on the string's torsion (torsion.h), each on its own grid */
void finger_apply(const Finger *f, String *s, double xF, double a);
#endif

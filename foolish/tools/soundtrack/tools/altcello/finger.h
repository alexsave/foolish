/* Left-hand finger: a pad of raised-cosine weights g_l in [0,1] pressing on
 * the grid points under it, force per metre of string
 *   f_l = -a g_l (K y_l + R y_t,l)      (both polarisations)
 * treated implicitly (centred) in altstring.c, so any K is stable. */
#ifndef ALTCELLO_FINGER_H
#define ALTCELLO_FINGER_H
#include "altstring.h"

typedef struct {
    double width; /* pad width, m */
    double K;     /* N/m^2 */
    double R;     /* N s/m^2 */
} Finger;

void finger_init(Finger *f);
/* weights for a pad centred at xF (m from the nut): fills g[0..hi-lo], sets
 * lo, hi (lo > hi when nothing is covered). g must hold 64 entries. */
void finger_weights(const Finger *f, const String *s, double xF, int *lo, int *hi, double *g);
void finger_apply(const Finger *f, String *s, double xF, double a);
#endif

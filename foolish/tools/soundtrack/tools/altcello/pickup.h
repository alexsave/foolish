/* Contact pickups (clip-on, piezo, magnetic): OUTPUT options only, nothing in the physics reads them.
 * A contact pickup senses the surface motion of one point of the body, not the radiated air pressure: no room, no distance.
 *   P2 velocity pickup (electrodynamic): the output is the point velocity.
 *   P1 piezo through a high-impedance preamp: the output is the point acceleration (the time derivative of the velocity) through a one-pole
 *      high-pass at the electrical corner fc = 1 / (2 pi R C) (C the piezo capacitance, R the preamp input resistance). Nothing else.
 * pickup_accel_hp: derivative (central difference) then the one-pole high-pass y[n] = a (y[n-1] + x[n] - x[n-1]), a = RC / (RC + 1/fs). */
#ifndef ALTCELLO_PICKUP_H
#define ALTCELLO_PICKUP_H
double pickup_corner_hz(double c_farad, double r_ohm); /* 1 / (2 pi R C) */
void pickup_accel_hp(const double *v, long n, double fs, double fc, double *out);
#endif

/* Thermal elasto-plastic bow friction (van Walstijn et al. 2026): see friction_thermal.h for the model, the equation numbers and the
   time stepping. */
#include "friction_thermal.h"
#include <math.h>

double thermal_y(const ThermalSpec *p, double tau)
{
    double r = pow(tau / p->tau_g, p->xi);
    return (1.0 + p->ya * r) / (1.0 + r);
}

double thermal_mu_ss(const ThermalSpec *p, double tau)
{
    double r = pow(tau / p->tau_g, p->xi);
    return (p->mu_s + p->mu_d * p->ya * r) / (1.0 + r);
}

double thermal_mu_str_tau(const ThermalSpec *p, double tau)
{
    double r = pow(tau / p->tau_g, p->xi);
    return (p->mu_s + p->mu_d * p->ya * r) / (1.0 + p->ya * r);
}

double thermal_v_of_tau(const ThermalSpec *p, double Fb, double tau)
{
    double m = thermal_mu_ss(p, tau);
    double s = p->b_tau * tau + sqrt(p->b_tau * p->b_tau * tau * tau + 4.0 * p->c_tau * tau * m * Fb);
    double u = s / (2.0 * m);
    return u * u / Fb;
}

double thermal_mu_ss_of_v(const ThermalSpec *p, double Fb, double v)
{
    return thermal_mu_ss(p, thermal_tau_of_v(p, Fb, v));
}

double thermal_tau_of_v(const ThermalSpec *p, double Fb, double v)
{
    /* f(tau) = muss(tau) v - [b sqrt(v/Fb) + c] tau (B.3) decreases from mus v at 0: bisection, then a few Newton-free halvings */
    double lam = p->b_tau * sqrt(v / Fb) + p->c_tau, lo = 0.0, hi = 1.0;
    while (thermal_mu_ss(p, hi) * v - lam * hi > 0.0 && hi < 1e12) hi *= 2.0;
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (thermal_mu_ss(p, m) * v - lam * m > 0.0) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

static void tab_build(Thermal *T, double Fb)
{
    const double t0 = 1e-4, t1 = 2000.0;
    double g = pow(t1 / t0, 1.0 / (TH_NTAB - 1));
    double tau = t0;
    for (int k = 0; k < TH_NTAB; k++, tau *= g) {
        T->tv[k] = thermal_v_of_tau(&T->p, Fb, tau);
        T->tm[k] = thermal_mu_str_tau(&T->p, tau);
    }
    T->fb_tab = Fb;
}

/* the Stribeck coefficient of the model at relative speed av >= 0: mustr(tau_ss(av)), read from the table */
static double mu_str_v(const Thermal *T, double av)
{
    if (av <= T->tv[0]) return T->p.mu_s;
    if (av >= T->tv[TH_NTAB - 1]) return T->tm[TH_NTAB - 1];
    int lo = 0, hi = TH_NTAB - 1;
    while (hi - lo > 1) {
        int m = (lo + hi) >> 1;
        if (T->tv[m] <= av) lo = m; else hi = m;
    }
    double u = (av - T->tv[lo]) / (T->tv[hi] - T->tv[lo]);
    return T->tm[lo] + u * (T->tm[hi] - T->tm[lo]);
}

void thermal_init(Thermal *T, const ThermalSpec *p, double width, double dt)
{
    T->p = *p;
    T->width = width;
    T->dt = dt;
    T->fb_tab = 0.0;
    thermal_reset(T);
}

void thermal_reset(Thermal *T)
{
    T->z = T->tau = T->vprev = T->qdiss = T->Hz = 0.0;
}

/* NSUB linearly implicit substeps at the relative velocity v (held over the step): (z, tau) in, out; *qint = integral of the
   dissipation per unit Fb over the step (m) */
static void run_step(const Thermal *T, double Fb, double v, double *z, double *tau, double *qint)
{
    const ThermalSpec *p = &T->p;
    int ns = p->nsub < 1 ? 1 : p->nsub;
    double h = T->dt / ns, av = fabs(v), s = v < 0.0 ? -1.0 : 1.0;
    double zs = s * *z, ta = *tau, qi = 0.0;
    if (av > 0.0) {
        double mstr = mu_str_v(T, av), lam = p->b_tau * sqrt(av / Fb) + p->c_tau, ha = h / p->a_tau;
        for (int j = 0; j < ns; j++) {
            double y = thermal_y(p, ta), zss = y * mstr / p->sigma0, zba = p->chi * y * p->mu_d / p->sigma0;
            double zp = zs + h * av, alpha = 0.0;
            if (zs > 0.0) {
                if (zp >= zss) alpha = 1.0;
                else if (zp > zba) alpha = 0.5 * (1.0 + sin(M_PI * ((zp - 0.5 * (zss + zba)) / (zss - zba))));
            }
            zs = zp / (1.0 + h * alpha * av / zss);
            double qh = p->sigma0 * alpha * av * zs * zs / zss; /* Qf / Fb, m/s (21) */
            ta = (ta + ha * qh) / (1.0 + ha * lam);
            qi += h * qh;
        }
    } else {
        /* v = 0: dz/dt = 0, no heat; the temperature relaxes with c_tau */
        ta *= exp(-T->dt * p->c_tau / p->a_tau);
    }
    *z = s * zs;
    *tau = ta;
    *qint = qi;
}

static double noise_gain(const ThermalSpec *p, double noise, double v)
{
    double r = (fabs(v) - p->v_slip) / p->v_slip;
    r = r < 0.0 ? 0.0 : r > 1.0 ? 1.0 : r;
    double m = 1.0 + noise * r;
    return m < 0.0 ? 0.0 : m;
}

static int contact(Thermal *T, double F_B, double *Fb)
{
    if (F_B < TH_F_CONTACT) {
        T->z = 0.0;
        T->tau *= exp(-T->dt * T->p.c_tau / T->p.a_tau);
        T->qdiss = T->Hz = 0.0;
        return 0;
    }
    *Fb = F_B / T->width;
    if (*Fb < TH_FB_MIN) *Fb = TH_FB_MIN;
    if (T->fb_tab <= 0.0 || fabs(*Fb - T->fb_tab) > 0.01 * T->fb_tab) tab_build(T, *Fb);
    return 1;
}

static void commit(Thermal *T, double F_B, double z, double tau, double qint)
{
    T->z = z;
    T->tau = tau;
    T->qdiss = F_B * qint / T->dt;
    T->Hz = 0.5 * F_B * T->p.sigma0 * z * z;
}

double thermal_advance(Thermal *T, double F_B, double v)
{
    double Fb, z = T->z, tau = T->tau, qi;
    if (!contact(T, F_B, &Fb)) return 0.0;
    run_step(T, Fb, v, &z, &tau, &qi);
    commit(T, F_B, z, tau, qi);
    T->vprev = v;
    return T->p.sigma0 * z;
}

typedef struct {
    Thermal *T;
    double Fb, G, q, noise, z, tau, qint, phi;
} Eval;

static double geval(Eval *E, double v)
{
    double z = E->T->z, tau = E->T->tau, qi;
    run_step(E->T, E->Fb, v, &z, &tau, &qi);
    E->z = z;
    E->tau = tau;
    E->qint = qi;
    E->phi = noise_gain(&E->T->p, E->noise, v) * E->T->p.sigma0 * z;
    return v + E->G * E->phi - E->q;
}

double thermal_solve(Thermal *T, double F_B, double q, double G, double noise, double *Phi)
{
    double Fb = 0.0;
    if (!contact(T, F_B, &Fb) || G < 1e-300) {
        /* no contact (or no admittance): the surface moves at q, the state still follows it */
        if (G < 1e-300 && F_B >= TH_F_CONTACT) { double z = T->z, tau = T->tau, qi; run_step(T, Fb, q, &z, &tau, &qi); commit(T, F_B, z, tau, qi); }
        *Phi = 0.0;
        T->vprev = q;
        return q;
    }
    Eval E = {T, Fb, G, q, noise, 0, 0, 0, 0};
    double P = 1.3 * T->p.mu_s * (1.0 + fabs(noise)) + 1e-3;
    double lo = q - G * P, hi = q + G * P;
    double x0 = T->vprev < lo ? lo : T->vprev > hi ? hi : T->vprev;
    double g0 = geval(&E, x0), tol = 1e-11 * (fabs(q) + G * P) + 1e-13;
    double a, b, ga, gb;
    if (fabs(g0) <= tol) { a = b = x0; ga = gb = g0; }
    else if (g0 > 0.0) { a = lo; b = x0; ga = geval(&E, lo); gb = g0; }
    else { a = x0; b = hi; ga = g0; gb = geval(&E, hi); }
    double v = x0;
    if (a != b) {
        /* Illinois regula falsi on [a, b], ga <= 0 <= gb */
        int side = 0;
        for (int it = 0; it < 80; it++) {
            double den = gb - ga;
            v = den > 0.0 ? (a * gb - b * ga) / den : 0.5 * (a + b);
            if (!(v > a && v < b)) v = 0.5 * (a + b);
            double gv = geval(&E, v);
            if (fabs(gv) <= tol || (b - a) <= tol) break;
            if (gv < 0.0) {
                a = v; ga = gv;
                if (side == -1) gb *= 0.5;
                side = -1;
            } else {
                b = v; gb = gv;
                if (side == 1) ga *= 0.5;
                side = 1;
            }
        }
    }
    geval(&E, v); /* the state and force at the accepted v */
    commit(T, F_B, E.z, E.tau, E.qint);
    T->vprev = v;
    *Phi = E.phi;
    return v;
}

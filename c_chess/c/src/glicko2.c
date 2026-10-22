/* glicko2.c - see glicko2.h. The steps are numbered as in the paper. */
#include "glicko2.h"

#include <math.h>

#define SCALE 173.7178
#define PI 3.14159265358979323846

static double g(double phi) { return 1.0 / sqrt(1.0 + 3.0 * phi * phi / (PI * PI)); }
static double E(double mu, double mu_j, double phi_j) { return 1.0 / (1.0 + exp(-g(phi_j) * (mu - mu_j))); }

g2_player g2_update(g2_player p, const g2_result *r, int n, double tau, double eps) {
    /* step 2: to the Glicko-2 scale */
    double mu = (p.rating - 1500.0) / SCALE, phi = p.deviation / SCALE, sigma = p.volatility;
    if (n == 0) {   /* no games: only the deviation grows */
        double phi_star = sqrt(phi * phi + sigma * sigma);
        return (g2_player){ p.rating, phi_star * SCALE, sigma };
    }
    /* steps 3 and 4: the estimated variance v and the improvement delta */
    double v_inv = 0, sum = 0;
    for (int j = 0; j < n; j++) {
        double mu_j = (r[j].opp.rating - 1500.0) / SCALE, phi_j = r[j].opp.deviation / SCALE;
        double gj = g(phi_j), ej = E(mu, mu_j, phi_j);
        v_inv += gj * gj * ej * (1.0 - ej);
        sum += gj * (r[j].score - ej);
    }
    double v = 1.0 / v_inv, delta = v * sum;
    /* step 5: the new volatility, by the Illinois algorithm on f(x) */
    double a = log(sigma * sigma), d2 = delta * delta, p2 = phi * phi;
#define F(x) (exp(x) * (d2 - p2 - v - exp(x)) / (2.0 * (p2 + v + exp(x)) * (p2 + v + exp(x))) - ((x) - a) / (tau * tau))
    double A = a, B;
    if (d2 > p2 + v) B = log(d2 - p2 - v);
    else {
        int k = 1;
        while (F(a - k * tau) < 0) k++;
        B = a - k * tau;
    }
    double fA = F(A), fB = F(B);
    while (fabs(B - A) > eps) {
        double C = A + (A - B) * fA / (fB - fA), fC = F(C);
        if (fC * fB <= 0) { A = B; fA = fB; }
        else fA /= 2.0;
        B = C; fB = fC;
    }
#undef F
    double sigma_new = exp(A / 2.0);
    /* steps 6 and 7: the new deviation and rating */
    double phi_star = sqrt(p2 + sigma_new * sigma_new);
    double phi_new = 1.0 / sqrt(1.0 / (phi_star * phi_star) + 1.0 / v);
    double mu_new = mu + phi_new * phi_new * sum;
    /* step 8: back to the Glicko-1 scale */
    return (g2_player){ SCALE * mu_new + 1500.0, SCALE * phi_new, sigma_new };
}

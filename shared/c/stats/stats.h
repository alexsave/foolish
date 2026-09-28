/* stats.h - the few lines of statistics at the bottom of a bot arena.
 *
 * An arena plays many games and reports two kinds of number: a mean with its
 * standard error (a score, a finish position, a paired difference), and a
 * proportion with its confidence interval (a win rate). Both are here once,
 * so every arena states the same quantity the same way.
 *
 * Pure arithmetic on doubles: no game type, no allocation, no I/O. Arenas
 * accumulate rather than store (a parallel reduction wants plain sums), so the
 * mean family works on the three running sums, not on an array. */
#ifndef SHARED_STATS_H
#define SHARED_STATS_H

/* The 97.5th percentile of the standard normal: a two-sided 95% interval. */
#define STAT_Z95 1.959964

/* n observations, their sum and their sum of squares. Zero it to start. */
typedef struct {
    double n, sum, sum2;
} StatSums;

void stat_add(StatSums *s, double x);

/* sum / n, and 0 for no observations. */
double stat_mean(const StatSums *s);

/* The sample variance with the Bessel correction,
 * (sum2 - sum * sum / n) / (n - 1): 0 below two observations, and never
 * negative (rounding can take a constant sample a hair below zero). */
double stat_variance(const StatSums *s);

/* The standard error of the mean, sqrt(variance / n); 0 for no observations. */
double stat_stderr(const StatSums *s);

/* The Wilson score interval for k successes in n trials at z standard
 * normal deviates (STAT_Z95 for 95%). It stays inside [0, 1] without a
 * clamp and is sound at k = 0 and k = n, where the normal approximation
 * collapses to a point. k may be fractional (a draw scored as half a win).
 * No trials at all is no information: [0, 1]. */
void stat_wilson(double k, double n, double z, double *lo, double *hi);

#endif

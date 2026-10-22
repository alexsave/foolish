/* glicko2.h - the Glicko-2 rating system (Glickman, "Example of the Glicko-2 system", 2013),
 * written from the paper: one rating period's update of one player against a list of opponents.
 * lichess's own parameters and rounding are layered on top in rating.c. */
#ifndef LICHESS_ROUND_GLICKO2_H
#define LICHESS_ROUND_GLICKO2_H

typedef struct { double rating, deviation, volatility; } g2_player;   /* the Glicko-1 scale (1500, 350, 0.06) */
typedef struct { g2_player opp; double score; } g2_result;            /* score 1, 0.5, 0 */

/* The player's new rating after the period's results (n may be 0: then only the deviation
 * grows, by the volatility). tau constrains the volatility's change; eps is the convergence
 * tolerance of the volatility iteration (the paper's 0.000001). */
g2_player g2_update(g2_player p, const g2_result *r, int n, double tau, double eps);

#endif

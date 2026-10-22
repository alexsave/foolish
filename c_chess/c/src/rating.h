/* rating.h - c_chess's ratings: Glicko-2 (glicko2.h) with this site's parameters. Every rated game
 * is one rating period for each player, against the other's rating before the game. */
#ifndef C_CHESS_RATING_H
#define C_CHESS_RATING_H

#include "glicko2.h"

#define CC_RATING_START 1500.0
#define CC_RD_START 350.0
#define CC_VOL_START 0.06
#define CC_RD_MIN 45.0
#define CC_RD_MAX 350.0
#define CC_VOL_MAX 0.1
#define CC_TAU 0.5
#define CC_PROVISIONAL_RD 110.0   /* a rating with a deviation at or above this shows a "?" */

g2_player cc_rating_new(void);
/* Both players' ratings after a game white scored `white_score` in (1, 0.5, 0). */
void cc_rate(g2_player *white, g2_player *black, double white_score);
/* The integer a rating displays as, and the change between two (as the site shows it). */
int cc_rating_int(const g2_player *p);

#endif

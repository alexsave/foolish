/* speed.h - a game's speed, as lichess sorts time controls: by the game's estimated length, the
 * base plus 40 increments (a 40-move game), under 3 minutes bullet, under 8 blitz, under 25 rapid.
 * A player holds a rating per speed (rating.h); the wire carries them in this order. */
#ifndef C_CHESS_SPEED_H
#define C_CHESS_SPEED_H

enum { CC_BULLET, CC_BLITZ, CC_RAPID, CC_CLASSICAL, CC_SPEEDS };
int cc_speed(int base_s, int inc_s);

#endif

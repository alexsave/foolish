// gd_play - what a gesture means on a board: foolish legal.c's play_* rules
// (play_pills, play_coverable_battles, play_best_cover_target, play_resolve,
// play_can_say_good) for this engine, so the d-pad offers exactly the buttons
// the website's keyboard mode offers (README.md "Controls").
//
// Portable C, like gd_rules. A selection is a list of card ids (a set).
#ifndef GD_PLAY_H
#define GD_PLAY_H

#include "gd_rules.h"

// foolish legal.h PLAY_PILL_*
#define GD_PILL_ATTACK 0x01u
#define GD_PILL_COVER  0x02u
#define GD_PILL_PASS   0x04u
#define GD_PILL_PICKUP 0x08u
#define GD_PILL_GOOD   0x10u

// May this seat end the bout (Done): a table with every attack covered, and
// good on its menu.
uint8_t gd_can_say_good(const gd_game *g, uint8_t seat);

// The battles a cover with exactly these cards could cover, as a bit per battle.
uint32_t gd_coverable(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n);

// The battle the Cover button aims at: the highest attack the selection can
// beat (every trump above every non-trump), ties to the leftmost; -1 for none.
int8_t gd_best_cover(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n);

// The cover with exactly these cards that covers battle `target`: the first
// such move in foolish's menu order (which pairs by hand order; this engine's
// hand order is card id order). 1 and `out` filled, or 0.
uint8_t gd_resolve_cover(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n,
                         uint8_t target, gd_move *out);

// The buttons the board shows for this selection (foolish play_pills, host 0).
uint8_t gd_pills(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n);

#endif

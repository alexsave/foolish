// gd_sound - which cue a moment of the game strikes, and when (README
// "Sound"; audio/DESIGN.md "The cues"). The driver (audio/snd.c) only plays
// what it is told; this is the telling. In ROM bank 9 beside the driver's
// tables, whose pre-rolls (snd_pre) it reads.
#ifndef GD_SOUND_H
#define GD_SOUND_H

#include <stdint.h>
#include "gd_rules.h"

// A move was applied: `b` the board before it, `g` after. `land` is the
// frames until its cards land (the flight's length), 0 if none flies (the
// bout ended under them). Called from main.c play(), after the flight is set.
void gd_snd_move(uint8_t seat, const gd_game *b, const gd_game *g, const gd_move *m, uint8_t land) GD_BANKED;

// For the deal's animation, one call a card: the card leaves the deck for
// `seat` now and lands `land` frames later (the art's deal: 8). The bot's seat
// lands at half the volume, across the table.
void gd_snd_deal(uint8_t seat, uint8_t land) GD_BANKED;
// The trump turned up from the deck now, landing `land` frames later.
void gd_snd_trump(uint8_t land) GD_BANKED;

#endif

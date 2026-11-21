// input - the buttons, with every timing that decides how they feel in one
// place. Alex tunes these by hand; nothing else in the ROM has a frame count
// for input.
#ifndef GD_INPUT_H
#define GD_INPUT_H

#include <stdint.h>

// A held LEFT or RIGHT moves once on the press, waits IN_REPEAT_DELAY frames,
// then moves every IN_REPEAT_RATE frames while still held (60 frames = 1 s).
#define IN_REPEAT_DELAY 20
#define IN_REPEAT_RATE   6
// A counts as HELD (so LEFT/RIGHT sweep-select) once it has been down this many
// frames. 0: from the frame it goes down, like Shift on the website.
#define IN_A_HELD_FRAMES 0
// SELECT counts as held (so LEFT/RIGHT rearrange the hand) after this many frames.
#define IN_SELECT_HELD_FRAMES 0

void    in_init(void);                 // installs the VBlank reader
void    in_update(void);               // once per pass of the game loop: takes what was latched
uint8_t in_pressed(uint8_t mask);      // went down since the last update
uint8_t in_down(uint8_t mask);         // is down
int8_t  in_lr_steps(void);             // LEFT (-) / RIGHT (+) presses and repeats since the last update
uint8_t in_a_held(void);
uint8_t in_select_held(void);
uint8_t in_any_pressed(void);

#endif

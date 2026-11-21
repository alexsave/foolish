// apu - a DMG APU model for the Mac, enough to preview the Game Boy's cues by
// ear before the build phase renders them in mGBA. Not shipped.
//
// What it models (Pan Docs, "Audio" and "Audio Registers",
// https://gbdev.io/pandocs/Audio.html, https://gbdev.io/pandocs/Audio_Registers.html):
// the four channels at their own clocks on a 2,097,152 Hz step (half the
// 4.19 MHz dot clock, fine enough for every channel's timer), the 512 Hz frame
// sequencer (length 256 Hz, sweep 128 Hz, envelope 64 Hz), the 4-bit DACs
// (on iff NRx2 & 0xF8, NR30 bit 7), NR51 panning and NR50 master volume, the
// DMG wave channel's first-sample-after-trigger quirk, and the output
// capacitor's high-pass (SameBoy's and Pan Docs' charge factor, 0.999958 a
// 4.19 MHz cycle). The output is mono (L+R)/2 at 32768 Hz.
//
// What it does not: zombie-mode envelope writes, the CH1 sweep's negate
// quirk, wave RAM corruption on a DMG retrigger, and the speaker's own
// response. mGBA is the judge in the build phase.
#ifndef APU_H
#define APU_H
#include <stdint.h>

#define APU_STEP_HZ 2097152u
#define APU_OUT_HZ  32768u

// Register write by its address' low byte: 0x10 (NR10) to 0x26 (NR52), and
// 0x30-0x3F wave RAM.
void apu_reset(void);
void apu_write(uint8_t reg, uint8_t val);
uint8_t apu_read(uint8_t reg);
// Run for `steps` 2 MHz steps, appending output samples to buf (float, about
// -1..1). Returns how many samples were appended; buf must hold steps/64 + 1.
uint32_t apu_run(uint32_t steps, float *buf);

#endif

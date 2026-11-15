# Score text: the VIB line

The score text format is described in the header comment of `scoretxt.h` (TEMPO, SECTION, parts, DYN, EVENTS).
No other composer-facing reference existed, so this file holds the vibrato switch and is the place to add more.

## The VIB line

`VIB PART: off`, `VIB PART: on` and `VIB PART: depth 0.5 rate 5.2` switch the sampler vibrato of one part in one section.
The words are `on`, `off`, `depth <scale>` and `rate <hz>`, in any order.
A later word overrides an earlier one, and a later VIB line for the same part and section overrides an earlier one field by field.
`on` means a depth scale of 1 and leaves a rate override alone.
`off` switches the vibrato off.
`depth 0` also switches it off.
The line applies to every note of that part in that section, wherever the line stands in the section, before or after the part's own line.
It covers notes written as bars and notes written as EVENTS alike.
A VIB line on a part that is not a sampler track is ignored with a note on stderr, because other synths use the same note slots for their own things (a glide, a pan).
A line is a VIB line only if everything after its colon is these words, so a part that happens to be named `VIB X` still reads as a part.

## What it sets

Each note carries two numbers the sampler reads.
`p[2]` is the depth scale: 0 means the track's own depth, a negative value means off for this note, and a positive value `s` means the track's depth times `s`.
`p[3]` is a rate override in Hz, and 0 means the track's rate.
A note with neither stays at 0 and 0, and plays as the track is set up.

## The track parameters

`sp[13]` is the vibrato depth in cents (the peak, not peak to peak), and 0 means no vibrato.
`sp[14]` is the rate in Hz.
`sp[17]` is the delay before the vibrato starts, in seconds, and 0 means the built-in 0.18 s.
The ramp to full depth stays 0.32 s after the delay.
The sampler gives every note a rate spread of plus or minus 8 percent and a random phase from the note's own seed.

## Defaults

The default is the track's own setting, so every existing song plays as before.
A track with `sp[13]` = 0 has no vibrato, and a VIB `on` does not give it any.
The Lyria flat instruments get their measured values in the score's C set-up: the cello 5.35 Hz and 10.1 cents, the haegeum 5.5 Hz and 11.9 cents, the trumpet 4.7 Hz and 5.5 cents.
Whether an instrument plays with vibrato in a section is the composer's decision, for example off for section playing.

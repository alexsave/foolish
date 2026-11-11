# 1 Miami - 2 a.m. (round 4, rebuilt)

Five parts, 200 bars at 122 BPM, 6:33.
The suite is its own seamless loop: the last bar of "Loop" is E-flat (VII), and its Rhodes sigh is the sigh of Tide bar 2 arriving one bar early. The wrap was verified at -143.7 dB.
Loudness -16 LUFS integrated, LRA 6.4 LU, true peak -2.9 dBTP.
The blind reviewer's key finder reads F minor (0.73). My own chroma estimate says A-flat major (the relative major), because the cycle Fm-Db-Ab-Eb leans on A-flat.
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `.txt`, `out/sections.txt`.
- `score.txt` (round 4, by Fable), `score.c`, `fable_suite_v4.md` (the composer's notes).
- Round 3 is kept in `r3/`, round 2 in `r2/`.

## Direction (approved by Alex)
Alex's notes on round 3:
- "Hard to listen to. Too happy, like straight sugar. The first part was the worst."
- "The main voice is super annoying and repetitive."

The direction he approved: Miami at 2 a.m., not a kids' beach.
- A minor-key melodic / deep house suite in F minor, with Miami's Cuban roots underneath.
- No supersaws, no major-key festival hook, no vocal chops.
- One 4-note motif that develops instead of repeating.

## Plan

| Part | Bars | Face | Arc |
|---|---|---|---|
| I Ocean Drive, 2 a.m. | 1-32 | filtered deep house intro | a dark Fm9 pad under a 400 Hz low-pass; the Rhodes sighs the motif's two intervals; shaker; at bar 9 sub, filtered kick and the piano, with the motif hidden as the top voice of its voicings -> Lift: the kick opens, the Rhodes pumps off-beat, congas start the tumbao, the piano states the motif in augmented octaves and the marimba answers with its retrograde |
| II Little Havana | 33-80 | minor piano montuno over the four-on-the-floor | the piano guajeo (the motif syncopated) over tumbao bass, clap, 16th shaker and conga marcha; bongo martillo from 9 -> Descarga: strings enter at eight beats a note, then in thirds, then a bar a note; marimba counter-line; bass walk-up into Causeway |
| III Causeway | 81-120 | driving build | the pluck takes the piano's churn as 16th arpeggios; the strings climb the motif by thirds, then re-sing it reharmonised (Db Bbm Eb C7) -> Build: whole-note motif, then a climb over a chord stretch, snare 8ths then 16ths, a riser, two empty beats: a false drop |
| IV Storm over Biscayne | 121-144 | beatless dark string break | the motif inverted in string octaves over Fm Bbm Gb C7 (the Neapolitan against the dominant), then three voices; piano answers; marimba rain; a filtered kick returns at 21 under a riser |
| V Before Sunrise | 145-200 | the drop, then the intro in reverse | the fully developed motif as an 8-bar string melody in the Cuban anticipation rhythm, then the piano guajeo, then the melody in thirds -> Loop: layers leave at 9, 13 and 16, the filters close, the Rhodes sighs return |

**The motif**, F Ab G C (up a minor third, down a step, up a fourth), develops through about fifteen transformations. Fable lists every one with bar numbers in `fable_suite_v4.md`. The main forms:
- split into two intervals (Rhodes sighs);
- hidden in the piano voicings, then augmented in piano octaves;
- in quarters and transposed up a third, with the marimba's retrograde answering;
- syncopated as the guajeo;
- at eight beats a note, then in thirds, in the strings;
- climbing by thirds and reharmonised;
- inverted in the storm;
- the full 8-bar developed melody at the drop, then in thirds.
- "Repeat a measure, change only its last notes" appears at about twenty phrase ends.

**Key journey**: F minor throughout, moved by harmony rather than modulation.
- Fm Db Ab Eb (i VI III VII) in Part I.
- Fm Bbm Db C7 (the Cuban cadence with E natural) in Part II.
- Reharmonisations over Db Bbm Eb C7 in Part III.
- Fm Bbm Gb C7, then a four-bar C7 pedal, in the storm.
- Back to Fm Db Ab Eb for the drop, ending on E-flat into bar 1.

**Hand-offs** (all at a steady 122 BPM; nothing is spliced):
| Bars | Device |
|---|---|
| 8 -> 9 | piano, sub and filtered kick enter together at the top of the chord cycle |
| 16 -> 17 | the kick's low-pass is fully open; the Rhodes turns from sighs to the off-beat pump; congas start |
| 32 -> 33 | the Rhodes pump and the congas run across the bar line; clap, tumbao bass and guajeo enter on a crash |
| 80 -> 81 | bass walk-up G Bb C E under a kick fill; the piano's churn becomes the pluck's 16ths (the same notes) |
| 120 -> 121 | two empty beats, then the crash over strings, pad and sub |
| 144 -> 145 | the strings climb Db-E / E-G into the drop's F |
| 176 -> 177 | the melody's falling tails into the reverse intro |
| 200 -> 1 | E-flat to F minor; the Rhodes sigh arrives one bar early |

No melody is quoted. The Cuban material (tumbao, martillo, guajeo, anticipated bass) is idiom, all original.

## Sound
Real recordings where they exist:
- Salamander piano;
- VSCO strings: split by register into violins, violas and cellos, with the bow swell skipped, 12 cents of added vibrato at 5.6 Hz, and solo violins above D6;
- VSCO marimba;
- VCSL congas (four strokes by pitch), bongos and shaker;
- Big Rusty Drums kick, snare and hats.

Synthesis:
- a sub kick under the sampled kick;
- the Rhodes (with the VCSL FM e-piano layered under it);
- a dark detuned-saw pluck with a dotted-eighth ping-pong;
- a chorused pad, a sine sub, and a driven saw bass, plus a wide copy of the bass.

Production:
- Everything melodic pumps against the kick (sub and bass at 6 dB, the rest at 3 dB).
- One dark hall (2.6 s) plus the pluck's delay.
- A master tape stage (anti-aliased), and a gentle 18 kHz roll-off.

Performance: kick on the grid (1 ms). Hands humanised (congas and bongos 7 ms and 12% velocity, the rest 4 ms and 8%). Hats and shaker accent the beat, with off-8ths at 80% and 16ths at 62%.

## Measurements and gates
Against the references Alex supplied (Disclosure "You & Me" Flume remix, Kygo "Firestone"; measured only, never used as audio):

| | Flume remix | Miami round 4 | round 4 first render |
|---|---|---|---|
| sub <60 Hz share | -8.0 dB | -6.3 | -2.9 |
| mid 0.8-3k | -9.2 | -9.4 | -18.8 |
| presence 3-8k | -14.2 | -16.3 | -28.1 |
| air 8-16k | -16.2 | -21.8 | -35.4 |
| side/mid low | -10.3 dB | -20.8 | -33.4 |
| side/mid mid | -5.4 | -2.9 | -5.5 |
| centroid | 808 Hz | 556 | 108 |
| LRA | 11.0 LU | 6.5 | 2.8 |

Blind reviewer's gates (its own scripts, rerun on this render):
- **High-frequency fizz**: the 16-20 kHz band is -39.5 dB re total and above 20 kHz is -60 dB. Round 3 measured -25 and -26 dB (target -35 to -50).
- **Width**: overall S/M -5.7 dB (target about -6). Sub-band correlation 0.98 (mono below 60 Hz).
- **Repetition**: the median similarity of returning material is 0.90 (target 0.9 or higher). Pulse clarity 0.71.

## Iteration log
- **First render**: the sub and kick sub dominated (centroid 108 Hz, the 25-62 Hz bands 10-16 dB over the references), and everything above 1 kHz was 10-19 dB short. Fixes: sub -11 dB, kick sub -6, piano +5, pluck +5, hats +4, shaker +4, a +5 dB master shelf from 3 kHz, -4 dB at 50 Hz.
- **Low-end width**: the master no longer monos the lows, and the wide bass layer came up 7 dB. Side/mid in the lows went from -33 to -21 dB; the references are at -10 to -14.
- **LRA was 2.8**: DYN depth went to 1.0 and the glue compression was lightened. LRA is now 6.4 (the references are 11-13; 5-7 is the blind reviewer's target for electronic).
- **The seam failed at -12.5 dB**, and the cause was in the engine:
  - A held filter drifted toward the track's next sweep, across the loop.
  - At the wrap, a ramp's end point and the next pass's step point had the same time, and their sort order differed between passes.
  - Fixed in `engine/render.h` and `engine/scoretxt.h`: sweeps step to their start, holds never drift, and ties sort ramp-before-step. The seam is now -143.7 dB.
- **Score format**: Fable wrote whole-bar chords without a length ("F3+Ab3+C4"). The parser now reads a lone token without a length as a whole bar.
- **Filters**: Fable's "FX: FILTER <part> a -> b bars N-M" lines are read by `score.c` and become low-pass automation.

## Weaknesses
- The low end is still about 10 dB narrower than the references.
- The LRA is half the references'.
- Fable ran out of credits after writing the score (its own checker passed), so there was no revision round on the writing.

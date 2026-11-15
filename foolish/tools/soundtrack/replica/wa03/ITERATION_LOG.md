# wa03 iteration log

Each entry: what the figure shows (viewed, not only scored), the fix it asks for, and whether the numbers agreed.

## Iteration 01 (overall 46.76): basic-pitch notes over 0.35, all piano, no pedal, no cello
Seen in the figure: the replica has 75 detected onsets against 54 in the original, so there are many extra (ghost) notes in the transcription, most visibly the dense grey ticks between the real onsets.
The original's long flat lines (the held bass notes at 5 to 12 s and 12 to 18 s, the chord tones at 19 to 22 s) are broken or missing in the replica CQT, and the level curve falls 8 dB under the original from 8 to 18 s and dips at 18 s: the held notes die too early (no pedal, no sustained cello).
Chroma replica has extra bright pitch classes at 0 to 5 s and 20 to 25 s that the original does not.
Fixes planned: fit templates to the CQT so ghosts are dropped and held lines become either piano with pedal or cello (wa03_nmf.py), then set note ends and pedal from the measured lines.

(Chart titles and labels were reworked after iteration 1 on Alex's request; iteration 1 was regenerated.)

## Iteration 02 (41.68): template fit (wa03_nmf.py), 485 piano + 31 cello notes, no pedal
Seen: held lines are back (level curve now follows the original much more closely, loudness corr 0.28 to 0.70) but the replica has 79 detected onsets against 54, the 23 ms grid of the fit puts notes off by up to 12 ms, and the replica spectrogram is clearly brighter (strong harmonic bands above 6 kHz).
Fixes asked for: exact onsets, fewer ghosts, a darker top, pedal.
Numbers: overall dropped (onset F fell from 0.505 to 0.415, mostly timing) while loudness, CQT and microtiming improved.

## Iterations 03 and 04: keep thresholds 0.06 and 0.12 of the largest fitted gain
Seen: fewer notes (250 and 121) barely changed the picture; at 0.12 whole notes vanish (CQT 0.64).
Kept 0.06.  Fewer notes did not fix the extra detected onsets, so the extra onsets are not ghost notes.

## Iteration 05 and 06: onsets moved to the strongest rise of the note's partials (wa03_post.py), then pedal
Seen: timing spread fell; 06 added a pedal that lifts at chroma-change peaks: chroma rose to 0.796 and CQT to 0.688, overall fell 48.76 to 47.08 only because the onset detector count moved.
Visible: the pedal lifted at 19.02 and 22.96 but the harmony changes at 18.6 and 22.4 (the replica showed dark gaps at 18 and 25 to 26 s).

## Iteration 07: treble
Seen: the replica spectrogram has a bright band 3 to 17 s the original lacks; 5 to 10 kHz energy was 15 dB over the original.
Cause (measured by rendering each part alone): the piano was nearly right (-44 dB against the original's -49 in the 5 to 10 kHz band) and the Lyria cello set carried most of the excess.
Fix: lowpass on the cello track.  overall 48.84 (lts 4.1 to 2.65 dB).  Piano shelf cuts had no effect on the numbers at all (a first attempt put the EQ on the wrong track and I checked by rendering with a 30 dB cut that changed nothing, which is how the cause was found).

## Iteration 08: pedal lifts from the low register
Seen: lifts at 19.02 and 22.96 against visible changes at 18.56 and 22.34.  Fix: lift times read off the low-register rise curve (5.07 8.13 11.83 18.56 22.34 26.18 28.35).  49.21, the dark gaps are gone.

## Iterations 09 and 10: closed-loop velocity calibration (wa03_vel.py), 3 and 6 passes
Seen: the level curve and chroma differ in many single notes (diff map: bright red and blue horizontal lines).
Result: 49.21 to 55.39: loudness corr 0.74 to 0.92, chroma 0.80 to 0.87.  Six passes did not help further (53.87, noise).
Note: these two iterations were rendered without the treble EQ (a build-script env variable I had only exported in an earlier shell); fixed in the next.

## Iteration 11: the EQ defaults put into wa03_build.sh. 56.22.
Seen: lts diff still +10 dB at 3 to 6 kHz.

## Iteration 12: master EQ fitted in closed loop to the clip's third-octave spectrum (wa03_eqloop.py)
lts_mad_db 4.0 to 1.2 dB, overall 58.62.  The replica spectrogram and the original now look the same in colour balance.

## Iteration 13: residual pass (wa03_resid.py): 8 notes the render lacked, velocities recalibrated. 58.77.
Seen in the difference map: red lines at MIDI 49 (15.6 to 19 s) and 53 and 62 at the start; the rule added only 8 notes because it asks for a fundamental, a supported 2nd partial and no lower note carrying it.
Not fixed: the map still shows blue and red scatter on the upper partials (the Lyria piano has a different harmonic balance from the Salamander).

## Iteration 14: per-note rise-time calibration (wa03_timing.py), 3 passes. 58.92.
The spread of per-note rise-time differences fell from 8.7 ms to 2.9 ms, but the tool's onset F did not move (0.54 to 0.54): the remaining onset mismatch is detection, not alignment.
The replica still shows detected onsets the original does not (27 of 115): the original's piano attacks are softer, so the detector skips many of its notes; an attack fade on the piano (8 ms, 25 ms) did not change the score.
Limitation: attack softness cannot be matched with the Salamander samples and the engine's linear attack fade.

## Iterations 15 to 18: instrument variants, each with its own velocity calibration (and its own EQ fit for VSCO)
15 Lyria cello with engine vibrato 61.10 (best); 18 the same notes without vibrato 60.32; 16 VSCO cello 58.17; 17 VSCO plus engine vibrato 58.06.
Differences between variants are within the render-to-render wobble of the onset terms (about 2 points), the clear difference is the level curve (0.95 against 0.88 for VSCO).

## Seen and not fixable with the current tools
- Brightness correlation (centroid curve per 100 ms) stays 0.14 to 0.28; the original's centroid falls to 450 to 550 Hz at 19 to 22 s while the replica stays near 700 Hz.
  The same notes sound softer in the Lyria piano; no fix found (velocity, hall, attack fade did not move it).
- Tempo: the tool's tempo estimate of the original is 178 BPM (the clip is rubato, 54 BPM in the prompt), the replica's estimate flips between 120 and 180; tempo_pct_diff swings from -33 to +0.7 between renders of nearly identical notes, so it is not a usable measure here.
- Piano and cello cannot be separated in the original (Demucs: one stem), so the cello and piano parts are scored only together.

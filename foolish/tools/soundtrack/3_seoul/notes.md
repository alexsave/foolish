# 3 Seoul - K-pop suite over Jersey club, with a 90s NY house part (round 3)

Five parts, 204 bars, 6:24.
The piece is its own seamless loop. It resolves on F# minor, then a B7 pickup bar leads back into bar 1 (E minor). The wrap was verified at -148 dB.
Loudness -16 LUFS integrated, LRA 5.4 LU, true peak -1.3 dBTP; the key estimate is E minor (r 0.78).
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `.txt`, `out/sections.txt`.
- `score.txt` (by Fable), `score.c`, `fable_suite.md` (the composer's plan).
- Round 2 is kept in `r2/`.

## Plan

| Part | Bars | Tempo | Key | Face | Arc |
|---|---|---|---|---|---|
| 1 Gangnam Girl-Crush | 1-48 | 156 | E minor | Jersey-club girl-crush K-pop | arp intro under an opening filter -> verse (Jersey kick, counter-line) -> pre-chorus climb, stop on beat 4 -> 16-bar chorus (motif A in four-note stacks) -> release to piano |
| 2 Han River 3AM | 49-68 | 78 (half-time) | E minor | midnight R&B ballad | piano arpeggios, ghost snares, bell fragments -> strings carry motif A augmented -> pivot stabbing in house rhythm, ramps to 124, ends on E7 |
| 3 Itaewon After Hours | 69-124 | 124 | A minor | the New York house elements | M1 organ bass riff, four-on-the-floor, choked off-beat open hats, swung 16ths, M1 piano stabs on Am9-Dm9-Gmaj9-Fmaj9/D13 (New York's progression moved to the iv key) with gospel pickups, bed squeak kept as the Seoul fingerprint -> peak: motif A as house chord stabs -> break ramps to 160 |
| 4 Dance Break | 125-164 | 160 | E minor | hard stop-hit dance break | 808 build, full stop -> 16-bar stop-hit drop (stacked hits in silence answered by Jersey kick bars) -> variation -> lift through D to C#7 |
| 5 Seoul Skyline | 165-204 | 160 | F# minor | the lifted final chorus | lifted chorus at full stack -> the opening arp returns a tone up -> last chorus (thickest) -> D - C#7 - F#m cadence, held resolution, then the B7 pickup into bar 1 |

**Motifs**
- **Motif A**, the chorus hook (E G B A G / E D E): Part 1 chorus; augmented in Part 2; as house stabs in Part 3; its head climbs to C#7 in Part 4; the full stack in F# minor in Part 5; its head forms the final cadence.
- **Motif B**, the gang rhythm (hit, rest, hit-hit): brass and chant in every chorus; the M1 organ riff's opening; the Part 4 unison hits.
- **Motif C**, the 16th arpeggio: Part 1 intro; Part 2 piano; Part 5 "Opening Returns" (the circle closes).

**Key journey**: E minor -> (E7) A minor -> E major turned to E minor -> D -> C#7 -> F# minor (the lift) -> B7 (IV of F# minor = V of E minor) -> loops to E minor.

**Hand-offs**
| Bars | Device |
|---|---|
| 45-48 | drums thin out, piano takes motif C; metric modulation: the 156 quarter becomes the 78 eighth |
| 65-68 | ramp 78->124 with the piano already stabbing in the house rhythm; E7; the organ enters on the and of 4 |
| 76 | the quote as the pickup into the stabs |
| 109-116 | ramp 124->160; four-on-the-floor becomes the Jersey kick at bar 113; E major becomes E minor at bar 116 |
| 124, 140, 164 | full stops and rolls; IMPACT on Part 5 bar 1 |
| 203-204 | B7 with the back half of the Jersey kick as a pickup into bar 1 |

**Quote (for the record)**: bar 76 (Itaewon Groove bar 8), M1 piano: the opening turn of Beethoven's "Für Elise" (E D# E D# E B D C | A), used as the gospel pickup into the house stabs.

## Alex's notes and what changed
- **"Main voice STILL thin"**: the lead is now a stack. A chord-voiced supersaw (11 voices, 30 cents) carries the full chord. The top note is doubled an octave up (7 voices, 36 cents) and by a detuned square for bite; the bottom note is doubled an octave down. They run through a group bus with drive and parallel compression.
  Around it: VSCO 2 string sections (violins, violas, cellos, chords split low to high) and VSCO staccato brass (trumpets, horns, trombones) on the gang rhythm.

  | Lead | Spectral peaks | Low-mid 200-500 | Octave band 250 Hz |
  |---|---|---|---|
  | Round 2 lead | 28 | 0.09 | -18.5 dB |
  | Round 3 stack | 78-86 | 0.20 | -12 to -14 dB |

  The stereo width is similar (about -7 dB side/mid).
- **"Increase the BPM"**: 140 -> 156, and 160 for the dance break and finale.
- **"It cuts off abruptly - I want to hear it resolve"**: Part 5 ends with a D - C#7 - F#m cadence, a held F# minor resolution with a piano afterglow, then a B7 pickup bar into the loop.
- **"Fold in the New York house elements"**: Part 3 (Itaewon After Hours) is New York round 2's house sound with minor tweaks, moved to A minor at 124:
  - the M1 Organ 2 bass recipe (organ body + square a fifth up, 24 dB low-pass at 2.5 kHz, pitch blip, 10 ms behind the beat);
  - the bright M1 piano;
  - the closed hat choking the off-beat open hat;
  - 58% swing on hats, shaker and organ in this part only;
  - New York's minor 9th / 13th voicings.
- **Still clearly minor**: E minor is home, with the harmonic-minor B7 and C#7 dominants.

## Iteration log
1. Seam residual -22 dB, traced to the PAD. Automation points interpolate between the end of one pass and the start of the next, and the end of pass 2 had no next pass. Fix in the engine: automation points from the first half second of the next pass are now copied in, as notes already were. The PAD's opening filter also gets an explicit hold point. Seam now -148 dB.
2. The lead stack measured 3x the round-2 partials, but the 250 Hz body was still lighter than the strings'. Low lead layer +3 dB; counter-line lead +5 dB (it measured -34 LUFS, close to inaudible).

## Weaknesses
- No sung vocals: the "chant" is still a small formant-voice gang shout, kept short and low.
- I cannot listen, and the tempo ramps (78->124, 124->160) were judged only from the score.

## Round 4 (Alex: best so far; "some parts sound a bit too soundtrack/classical")
The score is unchanged; the orchestra steps back behind K-pop production layers.
- **Strings**: every sampled string chord and line is now doubled by a synth string ensemble (a 7-voice detuned saw pad with chorus).
  - The synth layer leads and the VSCO sections sit 6 dB lower underneath, high-passed at 200 Hz.
  - Both are pumped by the kick (3 dB sidechain), the way K-pop strings breathe with the beat.
- **Brass stabs**: these are now synth brass (5 saws with a fast filter envelope and a 1.4 kHz bite). The sampled staccato brass sits 6 dB lower underneath, for the attack.
- The sampled strings also get the round 4 engine fix: the recorded bow swell is skipped, so short notes no longer sound reversed.
- **Result** (pre-master): violins at -32 LUFS and trumpets at -29, against the synth strings at -23 and the synth brass at -25.
- **Unchanged**: fullness (centroid 1498 vs 1476 Hz, 98 vs 99 spectral peaks), the seam at -149 dB, loudness at -16 LUFS. LRA is 7.7 LU.

## Round 4b (Alex: "little 'wiip' sounds that just sound annoying and add nothing"; the blind review and LE SSERAFIM "Eve" as reference)
- **The "wiip"**: it was the SQUEAK part, a 50 ms synth chirp sweeping 1.9 -> 2.8 kHz.
  - Fable wrote it as the "Jersey club bed squeak", the Seoul fingerprint, in the dance sections and Itaewon.
  - It is also the blind reviewer's 2-2.8 kHz "zap".
  - The part is muted, and nothing replaces it; the drums and chant carry those bars.
- **Foundation**, against LE SSERAFIM (measured only; round 4 had been 6-12 dB short of it under 160 Hz and 7 dB heavy at 0.8-3 kHz):
  - 808 +8 dB and sine sub +9 dB;
  - the kick reshaped around 32-120 Hz (high-pass moved from 50 to 32 Hz, +4 dB at 60 Hz, +2 dB at 120 Hz) and raised 3 dB;
  - a +4 dB low shelf at 90 Hz on the master;
  - the lead group cut -5 dB at 1.6 kHz and the master -3 dB at 1.8 kHz;
  - a +7 dB high shelf from 4.5 kHz.
- **Density**: master compression at -18 dB, ratio 3.5, plus an anti-aliased tape stage (0.6) and an 18 kHz roll-off. The DYN depth is 0.3, so the sections no longer swing in level. LRA is 1.9 LU (the reference is 2.6).

| vs "Eve" | ref | Seoul 4b | Seoul 4 |
|---|---|---|---|
| sub <60 share | -4.6 dB | -5.0 | (6-12 dB short) |
| bass 60-250 | -3.4 | -2.5 | |
| mid 0.8-3k | -13.2 | -14.4 | (7 dB over) |
| presence 3-8k | -14.5 | -17.5 | |
| air 8-16k | -16.6 | -21.2 | |
| centroid | 579 Hz | (close) | 1476 |

- **Aliasing** (reviewer's levels script): 16-20 kHz is -33.8 dB re total and >20 kHz is -53.9 (round 3: -17.7 and -17.7). The fixes were anti-aliased saturation in the lead stack and on the group buses, plus the top roll-off.
- **Not done**: the reviewer also asked for an 8-bar lead theme rewrite and a check of its 20% semitone-clash rate (its key finder rates the clarity low). Both need a composition pass, which I have not run.
- Seam -148.8 dB, -16 LUFS.

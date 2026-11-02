# 7 St. Petersburg - a ballet suite in Prokofiev's manner (round 3)

Six parts, 140 bars, 6:43.
The suite is its own seamless loop: it ends on the F dominant with full brass, and the next bar is the Bb gate chord of bar 1. The wrap was verified at -145.5 dB.
Loudness -16 LUFS integrated, LRA 6.9 LU, true peak -1.3 dBTP; the key estimate is B-flat minor (r 0.76).
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `.txt`, `out/sections.txt`.
- `score.txt` (by Fable), `score.c` (the sampled orchestra), `fable_suite.md` (the composer's plan).
- Round 2 is kept in `r2/` (`r2/full.wav`, `r2/score.c`).

## Plan

| Part | Bars | Tempo | Key | Face | Arc |
|---|---|---|---|---|---|
| I The Gates | 1-32 | 92 | B-flat minor | the heavy ballet march | 8 bars of minor-major-7th gate chords on whole-note lows, trumpets announce motif A -> 16 bars of the knights' stride in unison and octaves over dotted 3+1 lows, second statement adds a trumpet countertheme and snare -> 8-bar sideslip up to B minor, snap back, tutti onto the F pedal |
| II Balcony | 33-52 | 66 | D-flat major | the love scene | motif B in violins and oboe over viola arpeggios -> climactic octave restatement with flute and horns, cello and clarinet countermelody -> Night Wind winds down; a repeated measure changes one note (Gb-F becomes Gb-E) to open the pivot |
| III Street Fight | 53-84 | 144 | G minor, then E minor | the fight, spiccato and brass | motif A at double speed in spiccato violins and violas, horn stabs, held dark lows -> Chase: the stride in trombones, tuba, basses, cellos, horns under tremolo strings, trumpets and oboe pile on -> unison E |
| IV Lanterns | 85-104 | 80 | A major, then F-sharp minor | the toy-box scene, flute and celesta | flute sings motif B in the major over celesta tapping motif A's rhythm -> the flute dances the stride in F-sharp minor, doubled by violins, clarinet and celesta -> Lanterns Go Out repeats the measure with its last note changed (F# becomes F) and sinks to the F#/F clash |
| V Procession | 105-116 | 56 | B-flat minor | the funeral march | the stride as a dirge in the mid-low unison (cellos, violas, horns, sax) over the Gb-F bass sigh and muffled snare -> violins and oboe grieve motif B in the minor over a tutti F pedal |
| VI The Gates Again | 117-140 | 92 (8-beat ramp) | B-flat minor | the opening returns, tutti | the quoted Promenade bar in tutti octaves frames the gate chords -> the stride at full tutti, every lead doubled -> sideslip and snap -> F dominant, the upbeat to bar 1 |

**Motifs**
- **Motif A**, the stride (F F Bb Db / C Bb Gb F, dotted 1.5+0.5+1+1): I.2 and I.3; III.1 at double speed in G minor; III.2 in trombones in E minor; IV.1 bars 9-12 and IV.2 on the flute, tender; V as a dirge; VI.2 in full tutti.
- **Motif B**, the love theme (F Db / C Bb Ab, the same leap-and-fall stretched to a sixth): II in D-flat major; IV.1 in A major on the flute; V bars 9-12 in B-flat minor on the violins. Bar 7 of B carries A's dotted rhythm, so the two are heard as one family.

**Key journey**: B-flat minor -> D-flat major (relative major; F becomes its third) -> G minor -> E minor -> A major (E as dominant) -> F-sharp minor -> B-flat minor (F# respelled Gb, the Gb-F sigh) -> loops on the F dominant.

**Hand-offs**
| Bars | Device |
|---|---|
| 32 -> 33 | common tone: horns and cellos hold F through the bar line; Part II opens on that F as the third of D-flat major |
| 52 -> 53 | enharmonic pivot plus semitone bass: Db respelled C# in the bassoon, bass Db slides to D, the timpani roll bleeds across |
| 84 -> 85 | pedal: the unison E of the Chase is held by the cellos into Part IV as the dominant of A major |
| 104 -> 105 | enharmonic pivot: F# respelled Gb; the Gb-to-F sigh in the bass is the first thing Part V plays |
| 116 -> 117 | dominant pedal plus tempo ramp: tuba, basses and timpani hold F while the tempo ramps 56 -> 92 over the first 8 beats of Part VI |
| 140 -> 1 | the loop: full brass on F major, then the Bb gate chord of bar 1 |

**Quote (for the record)**: Mussorgsky, Pictures at an Exhibition, Promenade, bar 1, transposed to B-flat (Bb Ab Db Eb Ab F), at Part VI bar 1 (bar 117) in every part in octaves. The original is in 5/4; its fifth beat is folded into the last two 16ths so the bar sums to 4. Re-performed with our samples; no original audio is used.

## Alex's notes and what changed
- **"Thin"**: the GM orchestra is gone. Everything is a sampled section:
  - strings: VSCO 2 CE sustain, spiccato and tremolo sections, with violins, cellos and basses doubled by a second player (9 cents, +14 ms);
  - brass: horns, trumpets and trombones each doubled into a section, plus tuba;
  - winds: flute, oboe, clarinet, bassoon, and the VCSL tenor sax;
  - colour: harp, celesta (glockenspiel layered with vibraphone), the Salamander piano (pedal aware), VCSL timpani, field snare, bass drum and cymbals.
- **"An evil circus, not Dance of the Knights"**: the brief to Fable banned oom-pah, piccolo leads and bouncy bass. The lows hold long notes on beat 1, the theme sits in the middle-low register in unison and octaves (strings, horns, sax), and the darkness comes from minor-major-7th chords and sideslips rather than from chromatic clowning.
- **"The flute part is better but thin"**: the flute now leads only in Part IV, and from bar 9 it is doubled by violins, clarinet and celesta.
- **Length and form**: 1:39 -> 6:43, six parts, with the opening returning as Part VI.

## Iteration log
- First render: the score's DYN marks were silently ignored (LRA 2.5 LU, the balcony as loud as the gates). I implemented DYN in `engine/scoretxt.h`: velocities are scaled around mf, with a depth control `g_dyn_depth`.
- At full depth the LRA was 12.6 LU and the Night Wind and funeral march sank to -28 to -31 LUFS short-term, too quiet for a game bed. Depth 0.55 gave 5.2 LU (the ballet's contrast was flattened); 0.75 gives 6.9 LU: tutti at about -14, balcony and lanterns at -18 to -20, the funeral march at about -20.
- Balance by stem window: violins +6, trombones +7, sax +6, basses +5, celesta +8 (the glockenspiel alone vanished under the strings).
- Added a +3 dB master high shelf at 6 kHz to the shared orchestra: the sampled sections were dull next to the round 2 GM render.

## Measurements
Fullness (`engine/fullness`), whole suite against the round 2 render:

| | centroid | body 150-1k | low-mid 200-500 | partials | width |
|---|---|---|---|---|---|
| round 2 (GM) | 884 Hz | 0.57 | 0.20 | 73 | -13.5 dB |
| round 3 (samples) | 595 Hz | 0.65 | 0.29 | 77 | -4.9 dB |

The body and low-mid shares went up, and the stereo image is much wider (real seating and doubled players). The lower centroid is the intended dark ballet weight, and the octave bands stay within 5 dB of each other from 125 Hz to 1 kHz.
No commercial reference recording could be downloaded, so the comparison is against round 2 and against the VSCO ensemble recordings themselves.

## Weaknesses
- The VSCO sections are small chamber-sized ensembles; the tutti is full but not a 60-player orchestra.
- No legato transitions between sampled notes: fast string runs in Part III are spiccato and sound right, but slow legato lines (Part II) are note-to-note.
- The celesta is a layer of glockenspiel and vibraphone, not a real celesta.

## Round 4 (Alex: "This is art. Some higher-pitched strings or trumpets sound a bit synthy. Fix only those.")
Only the sound of the high strings and trumpets changed; the score is untouched.
- **High violins**: the VSCO violin section samples stop at D6, and the eleven notes above it were those samples stretched up to 3 semitones. They now play on three VSCO solo violins with vibrato, recorded up to C7. Each player has its own timing, detune and pan (`o_hi` in `engine/orch.h`).
- **High trumpets**: the trumpet samples stop at C6, and D6, E-flat 6 and F6 were stretched. The trumpets now take those notes an octave down, and the flute plays the written pitch on top.
- **Two engine fixes for every sampled string, found while checking this**:
  - The section sustain samples carry a recorded bow swell of 0.5-1 s, so short notes never reached full level and then stopped (a reversed-sounding note). The sampler now measures each sample's swell and starts a short note just before its attack point; long notes keep part of the swell.
  - The contrabass manifests were an octave high, so the basses doubled the cellos. The basses now sound where they are written, and the 63 Hz band rose by 1.7 dB.
- **The second players** now add their own light vibrato (5 cents at 5.2 Hz, starting after 0.18 s) on top of the recorded vibrato.
- **Reference**: I compared against a real orchestra, the Musopen recording of the Figaro overture (CC0, measurement only; see LICENSES.md).

| | centroid | body | low-mid | partials | width | 63 Hz band |
|---|---|---|---|---|---|---|
| Figaro overture (real orchestra) | 734 | 0.60 | 0.30 | 81 | -2.8 dB | -15.7 |
| St. Petersburg round 4 | 614 | 0.67 | 0.30 | 81 | -4.9 dB | -15.0 |

The seam is -145.0 dB, loudness -16 LUFS, LRA 6.9 LU.

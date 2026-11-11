# 2 New York - old-school NY hip hop suite (round 3)

The genre changed per Alex: out with the house (its elements moved to Seoul), in with old-school and 90s New York hip hop.
Five parts, 156 bars, 6:33.
The piece is its own seamless loop: the last bar leads into bar 1, and the wrap was verified at -194 dB.
Loudness -16 LUFS integrated, LRA 2.4 LU, true peak -1.3 dBTP; the key estimate is F minor (r 0.84).
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `out/an_loop.txt` (measurements), `out/sections.txt`.
- `score.txt` (by Fable), `score.c` (instruments, mix).
- `fable_suite.md` (the composer's plan).
- The round 2 house version is kept in `r2/`.

## Plan

| Part | Bars | Tempo | Key | Face | Arc |
|---|---|---|---|---|---|
| 1 Flash | 1-28 | 108 | F minor | Grandmaster Flash breakbeat party, electro-funk synth, cowbell | needle drop (scratch + break) -> Party (synth hook, horn stabs; piano octaves at 17-20) -> Break (bass + scratch) -> Hand-off (ramps to 92) |
| 2 Dusty | 29-60 | 92 | A-flat | Premier / Pete Rock jazz-loop boom bap | Loop (Rhodes, the quoted kick) -> Head (piano + vibes + flute + horn answers + guitar, peak 45-52) -> Release (drums out) -> Walkdown (ramps to 88) |
| 3 Queensbridge | 61-92 | 88 | C minor | dark minor piano loop (Mobb Deep / Illmatic) | Loop (piano + sub) -> Bridge (strings, muted horns, scratch; piano octaves at 81-84) -> Vanish (empties, pivots, ramps to 96) |
| 4 Brooklyn | 93-124 | 96 | D-flat | smooth soul-sample Biggie part | Glide (Rhodes groove) -> Hook (guitar + vibes + horn dyads + strings + soul "ahh" + flute) -> Turn (release; the bar-1 scratch over C7#9) |
| 5 Cypher | 125-156 | 90 -> 108 | F minor | scratch showcase, then the circle closes | Scratch (the scratch leads) -> Pass the mic -> Reprise (the opening Party returns, ramps to 108, peak at bar 149) -> Needle (the opening scratch hook leads back into bar 1) |

**Motifs**
- **Motif A**, the hook (F Ab C Bb Ab / F Eb F):
  - Party synth, bar 5;
  - Dusty piano / vibes / flute, bars 33-52, over A-flat;
  - Queensbridge horns, in C minor;
  - Brooklyn guitar / vibes / horns, bars 97-116, over D-flat;
  - scratched in Cypher;
  - the original form in the Reprise.
- **Motif S**, the scratch rhythm of bars 1-4, recurs at every hand-off and closes the loop.
- **Motif B**, the Queensbridge piano figure (C Eb D C G Ab G), is foreshadowed in bar 60 and returns in F minor in Pass the mic.

**Key journey**: F minor -> A-flat -> walkdown to G7 -> C minor -> Fm7 -> Ab7 -> D-flat -> C7#9 -> F minor.

**Hand-offs**
| Bars | Device |
|---|---|
| 25-28 | ramp 108->92; the bass is filtered down to 250 Hz and walks F Eb Db Eb G -> Ab |
| 57-60 | ramp 92->88; the bass walks down Ab G F Eb D Db G; motif B's pickup |
| 85-92 | ramp 88->96; drums out, then the half-time kick returns; Cm -> Fm7 -> Ab7 |
| 117-124 | ramp 96->90; drums out; the bar-1 scratch returns over C7#9 |
| 141-152 | ramp 90->108 into the reprise |
| 153-156 | the opening's scratch + kick + clap lead back into bar 1 |

**Quote (for the record)**: bar 29, kick, is the kick pattern of "Impeach the President" (The Honey Drippers), re-played on our own kit.

## Sound: what the research changed
- **Drums**: Big Rusty Drums (CC0), real recordings, close and overhead mics layered for kick and snare.
  - SP-1200 character: an anti-alias low-pass, then sample-and-hold at 26.04 kHz with 12-bit quantisation (rossum-electro; the research).
  - MPC swing: kick, snare and rim on 8ths at 57%; hats, shaker and cowbell at 62% on 8ths and 16ths (Attack, Linn).
  - The snare is 8 ms late, the bass 5 ms early.
  - Drum bus: 4:1 compression, 12 ms attack, drive, 60% parallel.
- **The "sample"**, re-performed on real instruments and made dusty (low-pass 5-7.5 kHz plus crush):
  - Salamander piano (CC-BY);
  - VSCO trumpets / trombones / horn section and string sections, chords split low to high across the players (CC0);
  - VCSL vibraphone and TX81Z FM e-piano layered on an FM Rhodes (CC0);
  - D. Smolken double bass pizzicato with a sine sub under it (CC0);
  - a Karplus-Strong guitar, a brassy supersaw for the electro-funk part, scratch synthesis (baby / chirp / transformer playback-rate curves), and vinyl crackle.

## Fullness of the main voice
Measured with `engine/fullness` on the solo melodic stem.

| Main voice | Centroid | Body 150-1k | Low-mid 200-500 | Spectral peaks | Stereo width |
|---|---|---|---|---|---|
| Round 2 house piano lead + stabs | 1396 Hz | 0.49 | 0.16 | 79 | -23 dB (mono) |
| Round 3 Dusty head | 704 Hz | 0.84 | 0.48 | 49 | -6.7 dB |
| Round 3 Queensbridge bridge | 911 Hz | 0.56 | 0.23 | 75 | -2.4 dB |
| Round 3 Brooklyn hook | 612 Hz | 0.72 | 0.36 | 72 | -9.6 dB |
| Round 3 Flash party | 1162 Hz | 0.63 | 0.28 | 56 | -5.9 dB |

The old lead was mono and had little low-mid body. The new main voice carries 2-3x the low-mid share and real stereo width.
No commercial hip-hop stems could be downloaded (licence rule), so the comparison is against round 2 and the research guidance.

## Iteration log
1. Seam residual -20 dB. The per-track debug (now a permanent tool, `SEAMDBG=1`) showed identical notes but a different waveform across the whole window. Cause: the SP-1200 sample-and-hold phase ran on absolute time, so with a 26.04 kHz grid it did not repeat with the loop. The hold grid now follows the loop position; the seam is -194 dB.
2. Snare -30 LUFS and hats -47 LUFS, against the Rhodes at -12: boom bap wants the kick and snare above the sample. Snare +10 dB, hats +4 to +11 dB, shaker +16 dB, Rhodes -4 dB.
3. The mix was very dark (brilliance -25 dB, air -29 dB). Part of that is intended dust, but the hats must cut, so hats +4 dB and the master high-shelf cut was removed.
4. The Queensbridge sub doubled the explicit SUB part, so the SUB was cut 3 dB.

## Weaknesses
- No rapper: the instrumental carries itself on the sample chops and scratches.
- The scratch source is synthetic (a formant "ahh" read through playback-rate curves), not a real record.
- The guitar is Karplus-Strong synthesis.
- I cannot listen; all of this was judged from measurements.

## Round 4 (Alex: "what starts at 1:19 is interesting; the dark piano part is good; everything before 1:19 sucks"; "the scratches are super off: scratching should mess with the music itself"; "more hip hop, slightly less jazz"; "more 808s")
Fable rewrote the score (`fable_suite_v2.md`, `score.txt`); the round 3 score is in `r3a/`. It now runs 6:44, with sections at 96 -> 92 -> 88 -> 96 -> 90 -> 96 BPM, all ramped.

**Part 1, rewritten** (bars 1-28, about 1:10):
- Needle drop: the dusty F-minor sample loop opens through a 300 Hz -> 6 kHz low-pass while the DJ cuts it, and a backspin ends the bar.
- Drop: a boom-bap kit and a long 808 under the unchanged loop; horn stabs, a guitar counter-line and 16th hats from bar 9; one-bar kit rests where the DJ cuts.
- Cut: a Grandmaster Flash quick-mix, with kit and 808 out, babies, chirps, a transformer and stutters on the loop itself.
- Hand-off: the filters close, and the turntable is powered off (a brake) on G7, into the quoted kick.
- The party funk, synth, cowbell and clap break are gone.

**More hip hop, less jazz**:
- An 808 part runs under every part where the beat plays.
- The Rhodes is cut to plain 7ths, the flute to one-bar tags, the horns to stabs.
- Queensbridge's sine sub is now the 808.

**Scratching works the playing record** (a new turntable stage in `engine/render.h`):
- SCRATCH baby and chirp strokes rock the sample group (keys, strings, vibes, guitar, vox) back and forth over the sound at that moment, so the loop's own harmonics bend. The drums keep time.
- Transformer, STUTTER, BRAKE and SPINBACK act on the drums too, so the whole beat stutters, slows to a stop or spins back.
- The old synthesized scratch voice is gone.
- The zoom of 0:02-0:10 shows the loop's partials bending in arches under the baby scratches and a rising backspin at bar 4.

**Production, against Nas, "N.Y. State of Mind"** (Alex's reference, measured only):
- Near mono:
  - the drums, bass and sample are narrowed;
  - the master side is at 0.3;
  - S/M is now -28.8 dB, against Nas's -25 to -32 (round 3 was 15-17 dB wider).
- Brighter:
  - the SP-1200 crunch now sits only on the kick, snare, rim and clap, so the hats keep their air;
  - the sample low-passes were raised to 8.5-11 kHz;
  - +7 dB shelf from 2.5 kHz, -4 dB at 400 Hz, +3 dB at 110 Hz;
  - presence is 2 dB under Nas, air 0.6 dB (round 3 was 7 and 6 dB under).
- Grit:
  - a tape stage at 0.8;
  - the vinyl louder, but low-passed at 9 kHz;
  - a 16.5 kHz roll-off.
- Sub: the sub share is -7.1 dB against Nas's -6.8. The 62 Hz band is still 10 dB over his, because our keys put the 808 root there.
- Performance:
  - hats and shaker accent the beat (off-8ths 78%, 16ths 60%), and off-beat snares are ghosts at 55%;
  - the kit is humanised 4 ms / 8% velocity, on top of the MPC swing.

**Engine bugs found here**:
- The Rhodes tremolo ran on absolute time, so the two passes disagreed at the wrap (seam -24.5 dB). Its rate is now rounded to whole cycles per loop.
- The SP-1200 sample-and-hold had no reconstruction filter. Its images were the blind reviewer's "inharmonic aliasing lines at 12-23 kHz" and its "noise bed above 16 kHz". A 2-pole low-pass now follows the hold.

**Gates** (blind reviewer's scripts on this render):
- 16-20 kHz is -41.4 dB re total, and >20 kHz is -62.9 dB.
- Return similarity is 0.96 and pulse clarity 0.63.
- The key finder gives A-flat major and F minor almost equal (0.70 / 0.69).
- LRA is 2.6 LU, the same as the Nas reference; the reviewer's generic 5-7 LU target was not applied.

Seam -150.6 dB, -16 LUFS.

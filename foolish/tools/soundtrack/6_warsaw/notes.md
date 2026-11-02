# 6 Warsaw - a Chopin suite for piano, with cellos in the songs (round 3)

Five parts, 116 bars, 5:54.
The suite is its own seamless loop. The Return ends on a D#7 double-octave run at 132 that lands on Part I bar 1 at the same tempo. The wrap was verified at -146.7 dB.
Loudness -16 LUFS integrated, LRA 10.4 LU, true peak -1.3 dBTP; the key estimate is A-flat minor, which is G-sharp minor spelled the other way (r 0.80).
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `.txt`, `out/sections.txt`.
- `score.txt` (by Fable), `score.c` (the piano, the pedal and the strings), `fable_suite.md` (the composer's plan).
- Round 2 is kept in `r2/` (`r2/loop.wav`, `r2/score.c`, `r2/notes.md`).

## Plan

| Part | Bars | Tempo | Key | Face | Arc |
|---|---|---|---|---|---|
| I Impromptu "Storm" | 1-20 | 132, ritardando to 54 | G-sharp minor | the Fantaisie-Impromptu storm (the round 2 melody, kept) | the theme in a single line, f -> Neapolitan and dim7 at bars 6-7 -> restated in double octaves, ff -> fury, fff, a fall the length of the keyboard -> hand-off: the lament bass sinks G#-F#-E and motif B is announced pp |
| II Tragic Nocturne | 21-48 | 54, then accelerando to 100 | E major, then E minor | the nocturne | the song, p dolce -> the song in octaves with the cellos under it, mf to f -> the darkening in E minor (Neapolitan F, dim7), a fff climax on motif A, subito pp -> the Prelude quote -> repeated chords sink E-D-C-B -> cadenza up B major |
| III Heroic | 49-72 | 100 | B major, then G-sharp minor | the polonaise | 16 bars of polonaise-rhythm octaves, ff to fff -> rising in G-sharp minor through the Neapolitan to a dim7 break and a double-octave fall -> one bar of bare G#, sffz dying to pp |
| IV Funeral | 73-92 | 50, then accelerando to 144 | C-sharp minor | the funeral march | block-chord march, pp rising to f with cellos and basses holding the bass -> the lament: motif B in minor octaves, sung by the cellos, over the chromatic bass C#-B-A#-A-G# -> a dim7 tremolo accelerando, pp to fff |
| V Coda and Return | 93-116 | 144, ritardando to 60, then 132 | G-sharp minor | the Ballade coda and the storm again | fury in double octaves over motif A in the bass -> Prelude-style repeated chords over a sinking bass -> collapse: the funeral chords in the home key, the lament bass alone -> the storm returns, p growing to ff, and loops |

**Motifs**
- **Motif A**, the storm turn (a note, the half step below, the note, the step below):
  - Part I's right hand throughout, and its hand-off in quarter-note octaves;
  - the Nocturne climax in eighth-note octaves, fff;
  - the polonaise in major, and the Rising section on the dominant;
  - the funeral march bar 3 in quarter notes, as a dirge;
  - the coda bass, augmented and then in eighth octaves, and the Return.
- **Motif B**, the song (a rising fourth to the tonic, the lower neighbour, a step up; this is the round 2 middle melody):
  - announced pp at the Part I hand-off, then the Nocturne song and the song in octaves in E major;
  - the Darkening in E minor, in sixths;
  - the polonaise opening in B major, and the Rising section in G-sharp minor;
  - the funeral lament in C-sharp minor.
- **The lament bass** (1-b7-b6-5) is the hand-off figure: Part I (G#-F#-E-B), the Nocturne (E-D-C-B), the funeral lament (C#-B-A#-A-G#) and the coda collapse (G#-F#-E-D#).

**Key journey**: G-sharp minor -> E major (VI heard as I) -> E minor -> B major (V of E, relative major of G-sharp minor) -> G-sharp minor -> C-sharp minor (iv) -> G-sharp minor. Every part has its Neapolitan: A, F, A, D and A.

**Hand-offs**
| Bars | Device |
|---|---|
| 16 -> 17-20 | a fall the length of the keyboard onto a D# pedal; the lament bass sinks G#-F#-E, E is reheard as tonic and B7 is its dominant; motif B is announced pp; ritardando 132 -> 54 over the 4 bars |
| 44 -> 45-48 | the Prelude quote, then repeated chords sink E-D-C-B; B7 drops its seventh and stands as B major; the cadenza run accelerates 54 -> 100 |
| 64 -> 65 | repeat-and-change: bar 64 repeats bar 56 and changes only its last chord (B major becomes D#7, the door to G-sharp minor) |
| 71 -> 72 -> 73 | double-octave fall against a rising bass onto a bare G# octave, the only sound for one bar; ritardando 100 -> 50; G# is the dominant of C-sharp minor |
| 88 -> 89-92 | the dim7 of G-sharp minor rises in tremolo, then D#7; accelerando 50 -> 144 into the coda |
| 104 -> 105-108 | repeat-and-change: bar 104 repeats the coda's bar 2 with its last note changed to the Neapolitan A; the funeral chords return in the home key; the lament bass alone; ritardando 144 -> 60 |
| 116 -> 1 | the loop: a D#7 double-octave run at 132 lands on Part I bar 1 |

**Quote (for the record)**: Chopin, Prelude Op. 28 No. 4 in E minor, bar 1: the held B over repeated E minor chords, at Nocturne Darkening bar 8 (bar 44). Re-performed with our samples; no original audio is used.

## Alex's notes and what changed
- **"Thin, the worst problem"**: round 2 was my additive synthesized piano.
  - Round 3 plays the Salamander Grand (a Yamaha C5, 16 velocity layers recorded at their real levels), with the sustain pedal.
  - Fable wrote a PEDAL part, which `score.c` turns into cc 64 legato pedalling: down 0.1 beat after each change, up 0.03 beat after the next.
  - Most of the score is double octaves or full chords in both hands.
- **"More tragic"**: added a tragic nocturne that darkens from E major to E minor, a funeral march with a sung lament, and a coda that collapses into the funeral chords.
  - A cello section (doubled) sings the top line an octave down in the song in octaves and in the lament.
  - Cellos and basses hold the funeral march's bass.
  - The strings appear only in those three places; everywhere else it is the piano alone.
- **"Keep the melody"**: Part I is the round 2 storm theme note for note. The round 2 middle melody is motif B.
- **Length and form**: 1:48 -> 5:54, five parts.

## Iteration log
- The score's DYN lines name bars ("f bars 1-4, cresc bars 5-8, ..."). The parser only read the first and last mark, so I rewrote it in `engine/scoretxt.h` as anchors with ramps.
  - It handles bar ranges, "bar 8 subito p", "cresc bars 5-7 to f", "from bar 9", "over bars 5-8", and "LEAD mp" (a part's own level, which it skips).
  - I checked it against every DYN line in all the scores. The St. Petersburg lines are plain marks, so they parse exactly as before.
- Fable's hand-off ramped 132 -> 84 and then jumped to the nocturne's 54. It now ramps straight to 54: one ritardando, no step.
- First render: LRA 14.1 LU, with the nocturne at -26 LUFS short-term.
  - The sampler applied velocity^1.15 on top of layers that already carry their recorded level, so the dynamics were counted twice.
  - The exponent is now 0.35, and the DYN depth is 0.6. Result: 10.4 LU, with the nocturne at -22 and the storm and polonaise at -13.
- Stereo width measured -1.2 dB (side almost as loud as mid).
  - The Salamander pair is spaced omnis, so single samples have side above mid: A4 +2.1 dB.
  - Pan by pitch was removed and both hands narrowed to width 0.55. Width is now -6 dB, which is mono-safe.
- Strings by stem window in the lament: the cellos started 6 dB under the right hand and the basses 14 dB under the left; raised by +3 and +6 dB.
- The cello bow noise drew bright streaks to 16 kHz over the piano (zoom at 90-110 s). The cellos now have a 6.5 kHz low-pass.

## Measurements
Fullness (`engine/fullness`), whole suite against round 2:

| | centroid | body 150-1k | low-mid 200-500 | partials | width |
|---|---|---|---|---|---|
| round 2 (synth piano) | 1098 Hz | 0.49 | 0.14 | 51 | -13.2 dB |
| round 3 (Salamander + strings) | 573 Hz | 0.58 | 0.23 | 58 | -6.0 dB |

The low-mid share went from 0.14 to 0.23, and the body share from 0.49 to 0.58.
The octave bands from 125 Hz to 1 kHz sit within 4 dB of each other; round 2 had a hole at 125-250 Hz.
No commercial reference recording could be downloaded. The Salamander samples themselves are the reference for the piano's timbre.

## Weaknesses
- The top end is dark: the 8 kHz band is -34 dB relative to the total. The soft layers of the Salamander are mellow, and the nocturne and funeral march live there.
- The cellos have no true legato transitions; the lines are overlapped sustain samples.
- The performance is metrical apart from the composed ramps: there is no rubato within phrases.

## Round 4 (Alex: "the strings that first come in sound a bit synthy or glossy"; "the sad part around 2:23 is super good, make it longer, you're onto something")
- **The sad part, made longer**: at 2:23 the round 3 render was at the Prelude quote (Darkening bar 8) and its 3-bar sinking hand-off. Fable replaced the hand-off with "II Nocturne: Elegy", 17 bars at 50 BPM (about 80 s, so the sad stretch went from about 18 s to about 85 s).
  - Bars 1-6 keep the quote's texture: the held B and its C-B sigh over the repeated chords. One inner voice sinks a half step every half bar, through B7, D minor, the Neapolitan F and F minor.
  - At bar 7 the bass octaves enter and carry the lament bass down, chromatically filled.
  - Bars 11-12 hold one quiet climax (mf) on a G5; it dissolves onto B7.
  - The last bar is the old hand-off's last bar, so the Cadenza follows unchanged.
  - The cello section sings the Elegy's top line an octave down, as in the octave song and the lament.
  - Every other section is byte-identical to round 3 (Fable's splice was verified).
- **The first strings**: these are the cellos of the octave song (75 s). They had the reversed-bow problem every sampled string had (see St. Petersburg round 4): the recorded swell is now skipped by note length. The second cello player also adds its own light vibrato.
- **Measurements**: 7:02 long, seam -146.0 dB, -16 LUFS, LRA 10.3 LU. The Elegy sits at -19 to -22 LUFS short-term.

## Round 4b (blind review items: full bandwidth, phrase-shaped rubato, melody lead, prepared changes, vibrato, round-robin)
- **Rubato**:
  - a 12% ritardando over the last two beats of every section and 5% at every 4-bar phrase end, with the tempo returning at the next downbeat (`sec_rit` in `engine/scoretxt.h`);
  - the score's own tempo changes keep their spots.
- **The hands**:
  - the melody hand is 22 ms ahead of the accompaniment (was 14), and the accompaniment 5 dB under it (was 1);
  - left-hand chords are rolled upward at 7 ms a note;
  - timing spread is 9 ms in the right hand and 6 ms in the left (was 5 and 3), and velocity spread 6%.
- **Prepared tempo change**: the one hard step in the suite (60 -> 132 into the storm's return) now accelerates over the first bar ("TEMPO 132 RAMP 4").
- **Cellos**: 12 cents of added vibrato at 5.6 Hz on top of the recorded vibrato. Every sampled note also varies by +-2 cents and +-0.6 dB, so repeated piano notes are never identical.
- **Bandwidth**: the "brickwall at 16.3 kHz" belonged to round 2's synthesized piano (its partials stopped at 15 kHz). The Salamander render has no wall, only the piano's natural roll-off.
  - The reviewer's levels script puts 10 kHz at -43 dB re 1 kHz and the HF extent at 15.4 kHz.
  - Beethoven's Moonlight third movement (Alex's reference recording) is darker still: -65 dB at 10 kHz.
- **Mix texture**: mono below 120 Hz, a light tape stage, room tone far down, an 18.5 kHz roll-off.
- **Sub**: the sub share is -29 dB. That is the piano's own (the Moonlight reference measures -35), so I left it rather than meet the reviewer's -12 to -20 for orchestral tracks.
- Seam -145.5 dB, -16 LUFS, LRA 11.5 LU.

# 4 Madrid - a salsa suite (rounds 3 and 4)

Six parts, 232 bars, 6:11.
The suite is its own seamless loop. Part VI ends on D7, and its last beat is the upbeat of Part I bar 1. The wrap was verified at -150.1 dB.
Loudness -16 LUFS integrated, LRA 5.4 LU, true peak -1.3 dBTP. The key estimate is G major (r 0.82), pulled by the G major mambo; the home key is G minor.
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `.txt`, `out/sections.txt`.
- `score.txt` (Fable's round 4 revision), `score.c`.
- `fable_suite.md` and `fable_suite_v2.md` (the composer's two versions with full notes), `r3a_score.txt` (the round 3 score).
- Round 2 is kept in `r2/`.

## Plan

| Part | Bars | Tempo | Key | Face | Arc |
|---|---|---|---|---|---|
| I Puerta del Sol | 1-24 | 84, ramps to 120 | G minor | bolero, then son | tres arpeggios and bolero bass, the solo trumpet states motif A with the flute echoing its head -> restated an octave up with vibes and flute over a two-voice string counter-line, bar 14 reharmonised -> Pivot: the tres turns the cycle into a son guajeo, clave and guiro enter, bass walk-up, cierre |
| II Calle de Alcala | 25-80 | 184 | G minor | salsa dura | five-part horn intro on motif A -> 16-bar verse for solo trumpet; a mona builds layer by layer (trombones and sax, then trumpets, then flute) -> 32-bar montuno: coro (five horns, close voicing) against pregon (solo trumpet) eight times, one layer added per round; the last coro bends its last note to B natural (G7) |
| III La Latina | 81-112 | 126 | C minor | charanga cha-cha-cha | motif A re-cut for flute alone, then flute, violins and vibes in unison, bar 13 reharmonised (F minor) -> Rompe: conga solo over bongo, shell, guiro and clave, band cierre -> tutti return with a five-horn chorale whose inner voices step every two beats -> D7 |
| IV Gran Via | 113-152 | 192 | G major | mambo and mona | the quote in octaves -> the mambo riff (motif C) in trumpet octaves, then with flute, vibes and strings -> clave punches under the solo -> the mona: trombones alone, trumpets in thirds, strings, then sax and the screaming solo trumpet -> five-stroke cierre on D7 |
| V Lavapies | 153-216 | 198 | G minor | descarga and final montuno | piano solo in octaves over the tres guajeo, one layer per chorus (strings, trombone answers, vibes, flute, horn punches) -> final montuno with the full band; the Gran Via riff in G minor -> five-stroke cierre, timbale fill alone, one held chord |
| VI Puerta del Sol de noche | 217-232 | 84 | G minor | the bolero at night | the bolero's second phrase with vibes and a five-horn chorale -> the opening phrase thinning to tres, bass and two-voice strings -> D7 into bar 1 |

**Motifs**
- **Motif A**, the Madrid call (D G Bb A | G F# G):
  - stated in the bolero, then in son rhythm in the pivot;
  - the five-part horn intro, and inverted as the verse;
  - the trombones' answer in sixths;
  - the C minor cha-cha for flute;
  - the opening of the piano solo;
  - the bolero reprise.
- **Motif B**, the coro (D D Eb D | C Bb A G):
  - the montuno in Part II, and its pivot version with B natural;
  - the final montuno with the full band;
  - its rhythm shapes the G7 hits.
- **Motif C**, the mambo riff (B D D B G): G major in IV, then G minor in V bars 201-208 (a mode transform).
- **Repeat a measure, change its last notes**: about 35 times. Fable lists them in `fable_suite_v2.md`.

**Key journey**: G minor -> C minor (iv) -> D7 pivot -> G major lift -> D7 cierre -> G minor by parallel-mode drop -> D7 -> G minor.

**Hand-offs**
| Bars | Device |
|---|---|
| 16 -> 17-24 | the bolero's last G5 under the son tres; tempo ramp 84 -> 120 across the pivot; clave and guiro replace the maracas at 21; bass walk-up G A Bb C with the leads in step (a minor-mode foreshadow of the quote); cierre and timbale fill |
| 24 -> 25 | 3-side hits and timbale fill, a crash on the downbeat, ramp to 184 |
| 80 -> 81 | the coro with its last note changed to B natural (G7), timbale pickup, ramp to 126 |
| 96 -> 97 | timbale fill straight into the conga solo |
| 112 -> 113 | five-stroke cierre on D7, ramp to 192, crash on the quote |
| 152 -> 153 | five-stroke cierre on D7, ramp to 198; the piano answers alone in G minor |
| 184 -> 185 | 3-side D7 hits and timbale fill into the final montuno |
| 213-216 -> 217 | five-stroke cierre, timbale fill alone, one held G minor chord; ramp down to 84; solo trumpet pickup |
| 232 -> 1 | D7 held, the tres falling to A; the loop |

**Quote (for the record)**: "Guantanamera" (trad., Joseito Fernandez), one measure of the chorus line's stepwise rise, pitched into G major (G A B C), at IV Gran Via: Mambo bar 1 (bar 113), with all five horns, vibes and strings in octaves. It is foreshadowed in minor at bar 23. Re-performed with our samples; no original audio is used.

## Alex's notes and what changed
- **Round 2: "some background instruments are good, the main instruments are too thin"**:
  - Every sound is now a real recording.
  - VCSL percussion: conga (muted, slap, open and tumba strokes picked by pitch, never repitched), bongos, two cowbells, claves, guiro (short and long scrapes), shaker. Timbales are the bongo heads tuned down; the cascara is the second claves.
  - The horns are VSCO trumpets and trombones, each line doubled into a section; the solo trumpet plays the vibrato samples alone.
  - Also the Salamander piano, VCSL vibes and tenor sax, VSCO flute and violins, and the double-bass pizzicato as the baby bass.
  - The tres is two plucked strings an octave apart (its doubled courses).
- **Round 4: "the transitions' writing quality throughout, but let the parts breathe"**: Fable revised the score with the same form, lengths, tempos and grooves.
  - Within each section a layer enters or a texture is handed over every 4-8 bars.
  - The verse builds a mona layer by layer; the montunos add one layer per coro round.
  - Repeats are reharmonised; counter-lines run in the strings; inner voices step in the chorales.
  - No melodic part repeats the same bar more than twice in a row.
- **String vibrato**: the round 4 engine fixes apply.
  - The violins skip the recorded bow swell.
  - The second player adds its own vibrato.
  - Notes above D6 go to three solo violins with vibrato.

## Iteration log
- **A comment ate two parts**: my own edit added a `//` comment in the middle of a line, which commented out the MARACAS and CRASH part mappings. The parser reported both parts as ignored, and that is how I found it.
- **Stem levels in the montuno window (100-130 s)**:
  - clave +15 dB and guiro +26 dB: the scrape samples are recorded very quietly;
  - trombones +10 dB: they sat 12 dB under the trumpets, and salsa trombones are big;
  - piano -4 dB;
  - shaker +15 dB.
- **The conga break**: Rompe bars 1-8 dropped to -28 LUFS short-term, 12 dB under the band. During the break the conga and bongo now play on their own "solo" tracks, 7 dB louder and a little closer. The break now sits at -21 to -22.
- **The track limit**: a later render crashed on the engine's 40-track limit. The limit is now 96, with a guard that fails loudly.

## Measurements
Fullness (`engine/fullness`), whole suite against round 2 (GM):

| | centroid | body 150-1k | low-mid 200-500 | partials | width |
|---|---|---|---|---|---|
| round 2 (GM) | 1107 Hz | 0.26 | 0.11 | 83 | -15.2 dB |
| round 4 (samples) | 838 Hz | 0.61 | 0.26 | 75 | -6.8 dB |

## Weaknesses
- There is no sampled tres; the plucked-string model is the least real sound here.
- The timbales are tuned-down bongos; real timbales have more metal ring.
- No coro voices: the coro is the five horns, which is common in salsa dura arrangements, but a sung coro would be more Madrid.

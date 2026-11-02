# 8 Moscow - the army that cannot be stopped (rounds 3 and 4)

Six parts, 152 bars, 6:11.
The suite is its own seamless loop: the last bar of the Return is Part I bar 1 (the field-drum figure over a C pedal at 72). The wrap was verified at -146.3 dB.
Loudness -16 LUFS integrated, LRA 7.8 LU, true peak -1.3 dBTP; the key estimate is C minor (r 0.76).
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `.txt`, `out/sections.txt`.
- `score.txt` (Fable's round 4 revision), `score.c`.
- `fable_suite_v2.md` (the composer's full notes), `r3a_score.txt` (the round 3 score), `fable_score_orig.txt`.
- Round 2 is kept in `r2/`.

## Plan

| Part | Bars | Tempo | Key | Face | Arc |
|---|---|---|---|---|---|
| I Steppe | 1-16 | 72 | C minor | the empty steppe | a distant field drum over a C pedal; the cellos walk the lament tetrachord C-Bb-Ab-G -> the lone bugle (motif B) with clarinet echoes a bar late, horns and the Phrygian A-flat; the bassoons play B's tail backwards -> drum roll into the march |
| II March Grows | 17-56 | 108 | C minor | the Shostakovich 7 growth | the snare ostinato alone for four bars, then it never stops; each 8-bar section adds a layer and moves the harmony under the unchanged motif A: bass walk, the chromatic-mediant sequence (C, A-flat, C, E-flat minor), the D-flat lift over a C pedal, B in octave canon, the A-flat major peak -> dies to the G pedal with the chant's shape in the cellos |
| III Chant | 57-72 | 84 | G minor | the soldiers' chant | the men's choir sings motif A; the tune never changes, but the floor under it moves every two bars (G, E-flat, F, G, C, D), trumpets in stretto -> the swell drops onto E-flat -> the G-G-flat-F slide |
| IV Lament | 73-88 | 66 | F minor | the lament | motif B on the clarinet over the balalaika, the violas as a singing inner voice -> the violins steal the head a bar early, the cellos answer two octaves below -> the head fragments to a single C over a C pedal as E natural arrives |
| V Army | 89-128 | 116 | C minor | the army arrives | motif A as a three-voice round (cellos, bassoons, violas inverted) -> the Slavianka quote -> B in the middle of the band over A in the bass, then on top with the low brass in canon -> the chromatic-mediant Menace -> the fff unison Climax with B's tail in augmentation in the low brass |
| VI Return | 129-152 | 120, ramping to 72 | C minor | the circle closes | A under B with A sequenced to A-flat -> B at two speeds at once (horns and second violins in augmentation under the trumpets) -> the Steppe's tetrachord, landing on bar 1 |

**Motifs**
- **Motif A**, the march figure (C C C Ab Bb / C Eb D G):
  - bare in the cellos (II.1);
  - sequenced by chromatic mediants (II.2), lifted to D-flat over the C pedal (II.3), on G as the chant's seed (II.5);
  - sung by the choir in G minor and reharmonised four ways (III);
  - as a round with a diatonic inversion in the violas (V.1), under motif B (V.2), the Menace sequence (V.3);
  - under B again in the Return.
  - Every fourth bar of A ends on a flair (A-flat or D-flat): the bar repeated with only its last note changed.
- **Motif B**, the bugle (G C Eb D / C G):
  - the lone trumpet with echoes (I.2);
  - its head through the mediants in the reeds (II.2), the octave canon (II.4), the A-flat peak (II.5);
  - the whole tune in F minor on the clarinet, then fragmented (IV);
  - in the band and then in canon (V.2), in unison with its tail in augmentation (V.4);
  - at two speeds at once (VI.1).

**Key journey**: C minor -> G minor (the dominant as a key; the swell drops to E-flat) -> the G-G-flat-F slide -> F minor -> C major (V of F minor) as the pivot -> C minor.

**Hand-offs**
| Bars | Device |
|---|---|
| 16 -> 17 | drum roll and timpani eighths; the clarinets' G-C pickup lands on the march's first C |
| 49-56 -> 57 | the chant is assembled before it starts: cellos on A in G, the violins' descent, the clarinets' G minor head, the choir humming G |
| 72 -> 73 | the G-G-flat-F slide in six parts under the tam-tam; F is the Lament's pedal |
| 85-88 -> 89 | the head fragmented to a single C; E natural turns C into V of F minor, then the pivot to C minor; the Part I drum figure returns in bar 88 |
| 96 -> 97 | the Slavianka bar as the pickup into the theme |
| 128 -> 129 | unison G hits and rolls in every brass and string part |
| 145-152 -> 1 | the tempo ramps 120 -> 72; the cellos, violins and violas walk the tetrachord; bar 152 is Part I bar 1 (the loop) |

**Quote (for the record)**: Agapkin, "Farewell of Slavianka", the opening bar, transposed to C minor, in the trumpets at Army: Approach bar 8 (bar 96), as the pickup into the theme. Re-performed with our samples; no original audio is used.

## Alex's notes and what changed
- **Round 2: "an impending, unstoppable army (Duel of the Fates / Imperial March, but Russian); a real army band; no happy bells, fair or church"**:
  - The GM band is gone. Every brass part is a doubled VSCO section: trumpets, horns, trombones, tuba, and a euphonium that is a trombone section with its edge low-passed.
  - Field drum, march snare and bass drum are VCSL samples, with clash cymbals and VCSL timpani.
  - VSCO strings switch to spiccato on short notes.
  - The men's choir is the GM choir layered with the formant ensemble (tenors and basses, little vibrato).
  - **No bells**: Fable's BELL strokes are low piano octaves with the pedal down (Rachmaninoff's bell imitation). The tam-tam is the clash cymbal pitched an octave down and left to ring.
  - The balalaika is the VSCO viola pizzicato with its pluck brightened, which is the nearest real plucked sound in the libraries.
- **Round 4: "the transitions' writing quality throughout, but let the parts breathe"**: Fable revised the score, keeping the same form, every section length, the tempos and the ostinato.
  - Inside each section the harmony moves under an unchanged motif, as a bass walk, chromatic mediants, the Phrygian D-flat or a moving floor under the chant.
  - Motifs are passed around as canons, a round, an inversion, augmentation and stretto.
  - The violas and violins have long singing counter-lines instead of doubling A in eighths.
  - A VIOLINS2 part was added.
- **"Don't be afraid to add vibrato"** (all cities):
  - The sampled strings now skip the recorded bow swell, which read as a reversed note.
  - The second players add their own vibrato.
  - Violins above D6 go to three solo violins with vibrato.
  - The contrabass manifests were an octave high and have been fixed, so the basses now sit under the cellos.

## Iteration log
- First render (round 3 score): the trombones were 12 dB under the trumpets, and they carry motif A. Raised trombones and euphonium by 8 dB, field drum +6, bell-piano -6.
- The formant choir layer was louder than the GM layer. It is now 12 dB under the GM track gain, with a darker tilt and no singer's formant (the same layer buzzed like an electric guitar in Vienna).
- The round 3 key estimate came out C major (r 0.75, pedals on C and G with a weak E-flat). With the revised score it reads C minor.
- The round 4 render crashed silently: the score has 45 tracks and the engine's limit was 40. The limit is now 96, with a guard that fails loudly.

## Measurements
Whole suite: centroid 521 Hz, body share 0.63, low-mid 0.29, 93 spectral peaks, width -5.7 dB, 63 Hz band -13.2 dB.
Reference: the Musopen Figaro overture (a real orchestra, CC0) measures 734 Hz, 0.60, 0.30, 81 peaks, -2.8 dB, -15.7 dB. Moscow is darker and heavier than the Mozart orchestra, by design.

## Weaknesses
- The choir has no words or consonants; a sampled men's choir would be the biggest upgrade.
- There is no real balalaika or tam-tam sample; both are stand-ins.
- The opening drums sit at about -30 LUFS short-term for the first 5 s (the distant pp drum), quieter than the rest of the bed.

## Round 4b (blind review and reference items: sub, presence, prepared tempo, vibrato, texture)
- **Prepared tempo change**: the one hard step in the suite (72 -> 108 into the march) is now an accelerando over the bugle's last two bars ("TEMPO 108 RAMP 8 AT -8", a new parser option that starts a ramp before the boundary).
- **Against the USSR anthem recording** (Alex's reference, measured only):
  - a +5 dB shelf from 3 kHz and +3 dB at 55 Hz on the master;
  - mono below 120 Hz, now a true high-pass on the side signal (sub correlation 0.94);
  - the sub share is -17.5 dB against the anthem's -13.3 (round 4 was 7 dB short), inside the reviewer's -12 to -20;
  - mids within 3 dB, presence 3.4 dB under, air 8 dB under.
- **Vibrato**: the strings add 12 cents at 5.6 Hz on top of the recorded vibrato, and every sampled note varies by +-2 cents and +-0.6 dB.
- **The choir's formant layer**: its harmonic ceiling moved from 6 kHz to 9.5 kHz (the reviewer's static low-pass ceiling at 2:05-2:57 and 4:30-5:50), with a darker tilt and breath.
- **Texture**: a light tape stage, room tone, an 18.5 kHz roll-off. 16-20 kHz is -54 dB re total.
- **Not done**: the reviewer's compositional items (a 4-8 bar brass theme over the ostinato, breaking the i-V rocking) need a composition pass, which I have not run.
- Seam -143.0 dB, -16 LUFS, LRA 7.7 LU.
